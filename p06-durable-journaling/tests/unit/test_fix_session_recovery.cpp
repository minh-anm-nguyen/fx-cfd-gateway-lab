// G07 — FIX session history, resend and business recovery.
//
// P01 owns the wire protocol; what P06 owns is the contract at the seam.
// These cases are about that contract: one original sequence per command
// key for the life of a session epoch, a mapping that survives a restart,
// and a reconciliation path that reads the mapping instead of guessing.
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, const char* cmd = "K1", const char* chain = "O1") {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order(cmd, chain, 1000), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G07-01", "P0", "one command key binds one original sequence") {
    Lab l("g07-01");
    seed(l);
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;

    const SendBinding a = l.link().send_once(Lab::kSession, "K1", sha256(payload), payload);
    CHECK(a.bound);
    CHECK(!a.ambiguous);
    const SendBinding b = l.link().send_once(Lab::kSession, "K1", sha256(payload), payload);
    CHECK(b.bound);
    CHECK_EQ(b.original_seq, a.original_seq);
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    // And the bytes went out once, not twice (JI-19).
    CHECK_EQ(l.lp().wire_count(payload), std::size_t(1));
}

SCENARIO("G07-02", "P0", "distinct commands get distinct sequences") {
    Lab l("g07-02");
    l.start();
    const SendBinding a = l.link().send_once(Lab::kSession, "K1", sha256("a"), "a");
    const SendBinding b = l.link().send_once(Lab::kSession, "K2", sha256("b"), "b");
    CHECK(a.bound && b.bound);
    CHECK_NE(a.original_seq, b.original_seq);
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(2));
    CHECK_EQ(l.lp().distinct_sequences(Lab::kSession), std::size_t(2));
}

SCENARIO("G07-03", "P0", "the same key with different bytes is a conflict, not a new send") {
    Lab l("g07-03");
    l.start();
    CHECK(l.link().send_once(Lab::kSession, "K1", sha256("a"), "a").bound);
    const SendBinding b = l.link().send_once(Lab::kSession, "K1", sha256("b"), "b");
    CHECK(b.conflict);
    CHECK(!b.bound);
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    CHECK_EQ(l.lp().total_writes(), std::size_t(1));
}

SCENARIO("G07-04", "P0", "the mapping survives a restart") {
    Lab l("g07-04");
    seed(l);
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;
    const SendBinding before = l.link().send_once(Lab::kSession, "K1", sha256(payload), payload);
    CHECK(before.bound);

    l.restart();
    l.start();
    std::uint64_t seq = 0;
    Digest hash{};
    CHECK(l.link().bound_sequence(Lab::kSession, "K1", &seq, &hash));
    CHECK_EQ(seq, before.original_seq);
    CHECK_EQ(to_hex(hash), to_hex(sha256(payload)));
    // Re-presenting the key after the restart does not allocate again.
    CHECK_EQ(l.link().send_once(Lab::kSession, "K1", sha256(payload), payload).original_seq,
             before.original_seq);
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
}

SCENARIO("G07-05", "P0", "sequences are not reused across keys within an epoch") {
    Lab l("g07-05");
    l.start();
    std::vector<std::uint64_t> seqs;
    for (int i = 0; i < 5; ++i) {
        const std::string k = "K" + std::to_string(i);
        const SendBinding b = l.link().send_once(Lab::kSession, k, sha256(k), k);
        CHECK(b.bound);
        seqs.push_back(b.original_seq);
    }
    for (std::size_t i = 1; i < seqs.size(); ++i) CHECK(seqs[i] > seqs[i - 1]);
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(5));
}

SCENARIO("G07-06", "P0", "an unconfirmed send is visible after a restart") {
    Lab l("g07-06");
    seed(l);
    l.lp().refuse_writes(true);
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;
    const SendBinding b = l.link().send_once(Lab::kSession, "K1", sha256(payload), payload);
    CHECK(b.ambiguous);
    CHECK(b.bound);
    CHECK_EQ(l.link().unconfirmed_sends(Lab::kSession), std::size_t(1));

    l.restart();
    l.start();
    // The restarted instance can enumerate what it never confirmed, which
    // is what reconciliation needs to work from.
    CHECK_EQ(l.link().unconfirmed_sends(Lab::kSession), std::size_t(1));
}

SCENARIO("G07-07", "P0", "reconciliation reads the mapping instead of guessing") {
    Lab l("g07-07");
    seed(l);
    // Force the outbox row into UNKNOWN with no sequence ever bound.
    const OutboxRow row = l.store().outbox_rows(Lab::kTenant).front();
    l.store().set_outbox_state(Lab::kTenant, row.outbox_id, "UNKNOWN", 0, std::nullopt,
                               "simulated ambiguity");

    CHECK_EQ(l.dispatcher().reconcile_unknown(Lab::kTenant, l.link()), std::size_t(1));
    // No binding exists, so the same key is safe to dispatch again.
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(1));
    expect_consistent(l, "G07-07");
}

SCENARIO("G07-08", "P0", "a bound sequence with no counterparty evidence is held") {
    Lab l("g07-08");
    seed(l);
    const std::string payload = l.new_order("K1", "O1", 1000).fix_payload;
    CHECK(l.link().send_once(Lab::kSession, "K1", sha256(payload), payload).bound);

    const OutboxRow row = l.store().outbox_rows(Lab::kTenant).front();
    l.store().set_outbox_state(Lab::kTenant, row.outbox_id, "UNKNOWN", 0, std::nullopt,
                               "simulated ambiguity");

    l.dispatcher().reconcile_unknown(Lab::kTenant, l.link());
    // JI-20 and JI-40: a bound sequence proves we issued something, not
    // what the counterparty did with it, so a human decides.
    const std::vector<OutboxRow> held = l.store().outbox_rows(Lab::kTenant, "HELD");
    CHECK_EQ(held.size(), std::size_t(1));
    CHECK(held.front().last_error.find("evidence") != std::string::npos);
    expect_consistent(l, "G07-08");
}

SCENARIO("G07-09", "P0", "a binding whose payload no longer matches is held, never resent") {
    Lab l("g07-09");
    seed(l);
    // P01 holds this key against different bytes than our outbox row has.
    CHECK(l.link().send_once(Lab::kSession, "K1", sha256("other"), "other").bound);
    const OutboxRow row = l.store().outbox_rows(Lab::kTenant).front();
    l.store().set_outbox_state(Lab::kTenant, row.outbox_id, "UNKNOWN", 0, std::nullopt, "amb");

    l.dispatcher().reconcile_unknown(Lab::kTenant, l.link());
    const std::vector<OutboxRow> held = l.store().outbox_rows(Lab::kTenant, "HELD");
    CHECK_EQ(held.size(), std::size_t(1));
    CHECK(held.front().last_error.find("different payload") != std::string::npos);
}

SCENARIO("G07-10", "P0", "a session epoch bump does not weaken business dedup") {
    Lab l("g07-10");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 20000, 10000, "K1")).kind, CommitKind::Committed);

    std::uint64_t epoch = 0;
    CHECK_EQ(l.link().bump_epoch(Lab::kSession, &epoch), StoreError::None);

    // After a reconnect the same trade is re-reported under a new epoch and
    // a new receipt id. Transport dedup cannot help; the business key does.
    auto again = l.fill("O1", "E1", 20000, 10000, "K1");
    again.session_epoch = epoch;
    CHECK_EQ(l.deliver(again).kind, CommitKind::Replayed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(20000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    expect_consistent(l, "G07-10");
}

SCENARIO("G07-11", "P0", "a restart holds dispatch while a binding has no evidence") {
    Lab l("g07-11");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "BOUND").size(), std::size_t(1));

    l.restart();
    const RecoveryReport rep = l.start();
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));

    // Evidence from the counterparty is what releases it.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 1000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "CONFIRMED").size(), std::size_t(1));
    const RecoveryReport again = l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK_EQ(again.phase, Phase::Ready);
    expect_consistent(l, "G07-11");
}

SCENARIO("G07-12", "P0", "the session store and the journal are separate commits") {
    // Section 6's open blocker, made visible: the two databases are
    // genuinely separate files, so nothing here may claim one transaction
    // spans them.
    Lab l("g07-12");
    seed(l);
    CHECK_NE(l.db_path(), l.link_path());
    l.link().send_once(Lab::kSession, "K1", sha256("p"), "p");
    // The session binding exists while our outbox row still says READY,
    // which is precisely the window the contract has to survive.
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(1));
}

SCENARIO("G07-13", "P1", "the inbound queue is drained in receipt order") {
    Lab l("g07-13");
    l.start();
    for (int i = 0; i < 5; ++i) {
        RawReceipt r;
        r.session_key = Lab::kSession;
        r.session_epoch = 1;
        r.receipt_id = 10 + static_cast<std::uint64_t>(i);
        r.raw = "35=8|n=" + std::to_string(i);
        r.raw_hash = sha256(r.raw);
        CHECK_EQ(l.link().persist_raw(r), StoreError::None);
    }
    const auto page = l.link().undelivered(Lab::kSession, 0);
    CHECK_EQ(page.size(), std::size_t(5));
    for (std::size_t i = 1; i < page.size(); ++i) {
        CHECK(page[i].receipt_id > page[i - 1].receipt_id);
    }
}

SCENARIO_BLOCKED("G07-14", "P0", "a FIX ResendRequest replays the session history",
                 "MsgSeqNum, gap fill and the resend window are project 01's engine. What "
                 "P06 contributes is that resend must reuse the bound original sequence "
                 "rather than mint a new one, which G07-01 and G07-04 assert.")

SCENARIO_BLOCKED("G07-15", "P0", "a counterparty-initiated sequence reset",
                 "same boundary: the reset is negotiated by the session engine. P06's "
                 "concern is that a new epoch does not weaken business dedup, which G07-10 "
                 "covers.")

SCENARIO_BLOCKED("G07-16", "P1", "a logon with mismatched CompIDs is refused",
                 "session identity is validated in project 01. Nothing in this project reads "
                 "a CompID.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_fix_session_recovery"); }
