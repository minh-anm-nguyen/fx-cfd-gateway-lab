// G04 — Outbox intent, claim and issue-once across the P01 boundary.
//
// The thing these cases keep testing from different angles is section 10's
// bold sentence: a claim timing out does not prove the send never happened.
// Everything the dispatcher is allowed to do follows from whether it can
// show, from durable state, which side of the P01 call it died on.
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, const char* cmd = "K1", const char* chain = "O1") {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order(cmd, chain, 1000), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

std::string state_of(Lab& l, const char* command = "K1") {
    for (const OutboxRow& r : l.store().outbox_rows(Lab::kTenant)) {
        if (r.command_id == command) return r.state;
    }
    return "<absent>";
}

OutboxRow row_of(Lab& l, const char* command = "K1") {
    for (const OutboxRow& r : l.store().outbox_rows(Lab::kTenant)) {
        if (r.command_id == command) return r;
    }
    TT_FAIL(std::string("no outbox row for command ") + command);
}

SCENARIO("G04-01", "P0", "a committed order leaves exactly one READY intent") {
    Lab l("g04-01");
    seed(l);
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    CHECK_EQ(state_of(l), std::string("READY"));
    // Nothing has been sent: the commit and the dispatch are separate steps.
    CHECK_EQ(l.lp().total_writes(), std::size_t(0));
    expect_consistent(l, "G04-01");
}

SCENARIO("G04-02", "P0", "a successful dispatch binds one original sequence") {
    Lab l("g04-02");
    seed(l);
    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(st.claimed, std::size_t(1));
    CHECK_EQ(st.bound, std::size_t(1));
    CHECK_EQ(state_of(l), std::string("BOUND"));

    const OutboxRow r = row_of(l);
    CHECK(r.original_seq.has_value());
    CHECK_EQ(l.lp().total_writes(), std::size_t(1));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-02");
}

SCENARIO("G04-03", "P0", "dispatching twice does not send twice") {
    Lab l("g04-03");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;
    CHECK_EQ(l.lp().wire_count(payload), std::size_t(1));

    // A second pass finds nothing READY, so nothing goes out again.
    const DispatchStats again = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(again.claimed, std::size_t(0));
    CHECK_EQ(l.lp().wire_count(payload), std::size_t(1));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-03");
}

SCENARIO("G04-04", "P0", "a crash before the P01 call leaves the row safely retryable") {
    // W13. The dispatcher died before it reached the session, and the
    // send_attempted flag says so, which is the only reason READY is safe.
    Lab l("g04-04");
    seed(l);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeStableSend), fp::Action::ReturnError);
    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(st.unknown, std::size_t(1));
    CHECK_EQ(l.lp().total_writes(), std::size_t(0));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(0));

    // No sequence was ever bound, so reconciliation can return it to READY.
    CHECK_EQ(l.dispatcher().reconcile_unknown(Lab::kTenant, l.link()), std::size_t(1));
    CHECK_EQ(state_of(l), std::string("READY"));
    expect_consistent(l, "G04-04");
}

SCENARIO("G04-05", "P0", "a crash after the sequence is persisted never mints a second one") {
    // W14. This is the case the whole stable-send contract exists for.
    Lab l("g04-05");
    seed(l);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterSeqPersisted), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(state_of(l), std::string("UNKNOWN"));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    std::uint64_t seq = 0;
    CHECK(l.link().bound_sequence(Lab::kSession, "K1", &seq, nullptr));

    // Reconciliation finds the binding. JI-20: a bound sequence with no
    // counterparty evidence is HELD, not resent and not assumed delivered.
    l.dispatcher().reconcile_unknown(Lab::kTenant, l.link());
    CHECK_EQ(state_of(l), std::string("HELD"));

    // Another dispatch pass must not allocate a second sequence.
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-05");
}

SCENARIO("G04-06", "P0", "a partial socket write becomes UNKNOWN, never a resend") {
    // W15.
    Lab l("g04-06");
    seed(l);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringSocketWrite), fp::Action::ReturnError);
    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(st.unknown, std::size_t(1));
    CHECK_EQ(state_of(l), std::string("UNKNOWN"));
    CHECK_EQ(l.lp().total_writes(), std::size_t(0));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-06");
}

SCENARIO("G04-07", "P0", "a lost write confirmation is UNKNOWN even though bytes went out") {
    // W16. The bytes reached the wire; JI-17 forbids concluding anything
    // about the counterparty from that, and the lost confirmation means we
    // cannot even claim the write succeeded.
    Lab l("g04-07");
    seed(l);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterSocketWrite), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(state_of(l), std::string("UNKNOWN"));
    CHECK_EQ(l.lp().total_writes(), std::size_t(1));

    // A later pass re-presents the same key. The session does not write the
    // same original twice; protocol resend is P01's business.
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-07");
}

SCENARIO("G04-08", "P0", "an expired claim that never reached P01 returns to READY") {
    Lab l("g04-08");
    seed(l);
    std::vector<OutboxRow> claimed;
    CHECK_EQ(l.store().claim_outbox(Lab::kTenant, "owner-a", l.token(), 10, 1000, &claimed),
             StoreError::None);
    CHECK_EQ(claimed.size(), std::size_t(1));
    CHECK_EQ(state_of(l), std::string("CLAIMED"));

    l.clock().advance(5000);
    int released = 0;
    CHECK_EQ(l.store().release_expired_claims(Lab::kTenant, l.clock().now_ms(), &released),
             StoreError::None);
    CHECK_EQ(released, 1);
    CHECK_EQ(state_of(l), std::string("READY"));
    expect_consistent(l, "G04-08");
}

SCENARIO("G04-09", "P0", "an expired claim that may have sent becomes UNKNOWN") {
    Lab l("g04-09");
    seed(l);
    std::vector<OutboxRow> claimed;
    l.store().claim_outbox(Lab::kTenant, "owner-a", l.token(), 10, 1000, &claimed);
    // The dispatcher recorded that it was about to call P01, then vanished.
    CHECK_EQ(l.store().mark_send_attempted(Lab::kTenant, claimed.front().outbox_id, l.token()),
             StoreError::None);

    l.clock().advance(5000);
    l.store().release_expired_claims(Lab::kTenant, l.clock().now_ms(), nullptr);
    // Section 10 in bold: the timeout proves nothing, so the safe state is
    // UNKNOWN rather than a free retry.
    CHECK_EQ(state_of(l), std::string("UNKNOWN"));
    expect_consistent(l, "G04-09");
}

SCENARIO("G04-10", "P0", "a dispatcher with a stale fence cannot move a row") {
    Lab l("g04-10");
    seed(l);
    const FenceToken old_token = l.token();

    // A new owner takes the domain.
    FenceToken newer = 0;
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer),
             StoreError::None);
    CHECK(newer > old_token);

    std::vector<OutboxRow> claimed;
    l.store().claim_outbox(Lab::kTenant, "owner-new", newer, 10, 5000, &claimed);
    CHECK_EQ(claimed.size(), std::size_t(1));

    // The old owner's late callback. JI-21: refused at the resource.
    CHECK_EQ(l.store().set_outbox_state(Lab::kTenant, claimed.front().outbox_id, "BOUND",
                                        old_token, std::uint64_t(99), "stale"),
             StoreError::NotOwner);
    CHECK_EQ(state_of(l), std::string("CLAIMED"));
    expect_consistent(l, "G04-10");
}

SCENARIO("G04-11", "P0", "a bound sequence is never rewritten with a different one") {
    Lab l("g04-11");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    const OutboxRow bound = row_of(l);
    CHECK(bound.original_seq.has_value());

    // JI-18: the mapping is stable for the session epoch.
    CHECK_EQ(l.store().set_outbox_state(Lab::kTenant, bound.outbox_id, "BOUND", 0,
                                        *bound.original_seq + 5, "rebind"),
             StoreError::Conflict);
    CHECK_EQ(*row_of(l).original_seq, *bound.original_seq);
}

SCENARIO("G04-12", "P0", "the same key with different bytes is held, not sent") {
    Lab l("g04-12");
    seed(l);
    // P01 already bound this command to other content, as a corrupted
    // retry or an attack would produce.
    const SendBinding first =
        l.link().send_once(Lab::kSession, "K1", sha256("different payload"), "different payload");
    CHECK(first.bound);

    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(st.held, std::size_t(1));
    CHECK_EQ(state_of(l), std::string("HELD"));
    expect_consistent(l, "G04-12");
}

SCENARIO("G04-13", "P0", "evidence from the counterparty confirms the effect") {
    Lab l("g04-13");
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 100000), l.token()).kind,
             CommitKind::Committed);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(state_of(l), std::string("BOUND"));

    // A report naming the command is the first thing that actually proves
    // the counterparty saw it.
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 40000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(state_of(l), std::string("CONFIRMED"));
    expect_consistent(l, "G04-13");
}

SCENARIO("G04-14", "P0", "unresolved effects are enumerable for an operator") {
    Lab l("g04-14");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    l.service().submit_new(l.new_order("K2", "O2", 1000), l.token());

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterSocketWrite), fp::Action::ReturnError, 0, 1);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    const std::vector<OutboxRow> open = l.dispatcher().unresolved(Lab::kTenant);
    CHECK_EQ(open.size(), std::size_t(1));
    CHECK_EQ(open.front().state, std::string("UNKNOWN"));
    CHECK(!open.front().last_error.empty());
    expect_consistent(l, "G04-14");
}

SCENARIO("G04-15", "P0", "the claim limit bounds how much one pass takes on") {
    Lab l("g04-15", 1000000);
    l.start();
    for (int i = 0; i < 7; ++i) {
        const std::string k = "K" + std::to_string(i);
        const std::string c = "O" + std::to_string(i);
        CHECK_EQ(l.service().submit_new(l.new_order(k, c, 1000), l.token()).kind,
                 CommitKind::Committed);
    }
    OutboxDispatcher::Options opt;
    opt.owner = "bounded";
    opt.claim_limit = 3;
    OutboxDispatcher d(l.store(), l.link(), opt);

    CHECK_EQ(d.dispatch_once(Lab::kTenant, l.token()).claimed, std::size_t(3));
    CHECK_EQ(d.dispatch_once(Lab::kTenant, l.token()).claimed, std::size_t(3));
    CHECK_EQ(d.dispatch_once(Lab::kTenant, l.token()).claimed, std::size_t(1));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(0));
    expect_consistent(l, "G04-15");
}

SCENARIO("G04-16", "P0", "a restart re-reads the outbox and does not duplicate the send") {
    Lab l("g04-16");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;
    CHECK_EQ(l.lp().wire_count(payload), std::size_t(1));

    l.restart();
    const RecoveryReport rep = l.start();
    // A BOUND effect with no counterparty evidence holds the instance in
    // RECON_ONLY rather than letting it resume (JI-16).
    CHECK_EQ(rep.phase, Phase::ReconOnly);

    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "G04-16");
}

SCENARIO("G04-17", "P1", "a refusing session leaves the row unresolved rather than lost") {
    Lab l("g04-17");
    seed(l);
    l.lp().refuse_writes(true);
    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(st.unknown, std::size_t(1));
    CHECK_EQ(state_of(l), std::string("UNKNOWN"));
    expect_consistent(l, "G04-17");
}

SCENARIO_BLOCKED("G04-18", "P1", "SKIP LOCKED reader starvation under many dispatchers",
                 "SQLite serialises writers, so the PostgreSQL SKIP LOCKED queue semantics "
                 "section 9.3 relies on cannot be reproduced. The claim logic that is testable "
                 "here is covered by G04-10 and G04-15.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_outbox"); }
