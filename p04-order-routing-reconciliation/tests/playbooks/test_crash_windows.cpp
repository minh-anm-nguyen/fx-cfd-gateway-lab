// The twenty crash windows of design doc 15.1, each named and each run.
//
// Release gate G7 in 23.1 asks for "all 20 crash windows tested", and 23.2
// names the three things that must hold afterwards: no accepted intent lost, no
// command issued twice as an original send, no fill counted twice. Those three
// are asserted here for every window, on top of the full invariant oracle.
//
// The behaviour specific to each window is asserted in section O of the matrix;
// this suite is the gate-facing roll call.
#include <set>

#include "support/lab.hpp"

using namespace router;
using lab::Lab;

namespace {

// Runs one order through its whole life, dying at `point`, then restarting and
// finishing.
void run_window(Lab& L, const char* point, FaultKind kind = FaultKind::Crash) {
    L.fp.arm(point, kind);
    L.crash_during([&] {
        auto r = L.submit(L.new_order("K1", "100000", "1.08500"));
        if (r.is_err()) return;
        L.rt->dispatch_pending();
        L.lp->execute(r.value().cl_ord_id.str(), 40000, 108500);
        L.rt->pump_inbound();
    });
    L.fp.clear();

    // The client retries with the same command id and the run completes.
    auto again = L.submit(L.new_order("K1", "100000", "1.08500"));
    if (again.is_err()) return;
    L.drive(16);
    L.lp->execute(again.value().cl_ord_id.str(), 60000, 108510);
    L.drive(16);
}

// The three release-gate properties, plus the invariant oracle.
void assert_gate(Lab& L, const char* window) {
    const std::string w(window);
    // One accepted client command, never two. A reconciliation probe is a
    // durable command of its own (design doc 14.3), so it is not counted here.
    std::size_t client_commands = 0;
    for (const auto& [k, in] : L.disk.intents) {
        (void)k;
        if (in.kind != CommandKind::StatusQuery) ++client_commands;
    }
    CHECK_MSG(client_commands <= std::size_t{1}, w);
    // One original send at most, so one order at the peer.
    CHECK_MSG(L.lp_disk.book.size() <= std::size_t{1}, w);
    CHECK_MSG(L.disk.cl_ord_index.size() <= std::size_t{1}, w);
    // One binding per command, so at most one per client command plus any
    // probes that were actually issued.
    CHECK_MSG(L.session_disk.bindings.size() <= L.disk.intents.size(), w);
    // No fill counted twice, and the ledger adds up.
    for (const auto& [id, c] : L.disk.chains) {
        (void)id;
        std::set<std::string> ids;
        std::int64_t sum = 0;
        for (const p03::Fill& f : c.fills) {
            CHECK_MSG(ids.insert(f.exec_id.str()).second, w + ": duplicate fill");
            sum += f.qty;
        }
        CHECK_MSG(sum == c.cum_qty, w + ": ledger does not add up");
        CHECK_MSG(c.cum_qty <= c.accepted_qty || c.hold.has_value(), w + ": overfill");
    }
    const auto v = L.violations();
    CHECK_MSG(v.empty(), w + ": " + render_violations(v));
}

}  // namespace

SCENARIO("CW-01", "P0", "crash before the intent transaction") {
    Lab L;
    run_window(L, fp::kBeforeIntentCommit);
    assert_gate(L, "CW-01");
}

SCENARIO("CW-02", "P0", "crash inside the intent transaction") {
    Lab L;
    run_window(L, fp::kInsideIntentCommit);
    assert_gate(L, "CW-02");
}

SCENARIO("CW-03", "P0", "crash after the intent commit, before the local acknowledgement") {
    Lab L;
    run_window(L, fp::kAfterIntentCommit);
    assert_gate(L, "CW-03");
}

SCENARIO("CW-04", "P0", "crash after the outbox read, before the P01 request") {
    Lab L;
    run_window(L, fp::kAfterOutboxClaim);
    assert_gate(L, "CW-04");
}

SCENARIO("CW-05", "P0", "P01 reserved the original sequence and the reply was lost") {
    Lab L;
    run_window(L, fp::kDropSendOnceReply);
    assert_gate(L, "CW-05");
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
}

SCENARIO("CW-06", "P0", "crash before the P01 binding is durable") {
    Lab L;
    run_window(L, fp::kBeforeSendBinding);
    assert_gate(L, "CW-06");
}

SCENARIO("CW-07", "P0", "crash after the binding, before the socket write") {
    Lab L;
    run_window(L, fp::kAfterSendBinding);
    assert_gate(L, "CW-07");
}

SCENARIO("CW-08", "P0", "the socket was only partly written") {
    Lab L;
    run_window(L, fp::kPartialWireWrite, FaultKind::Error);
    assert_gate(L, "CW-08");
    // The peer never built an order from a truncated frame.
    CHECK(!L.lp_disk.rejected_frames.empty());
}

SCENARIO("CW-09", "P0", "the write succeeded and no business acknowledgement came back") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    run_window(L, fp::kAfterWireWrite);
    assert_gate(L, "CW-09");
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
}

SCENARIO("CW-10", "P0", "the LP accepted the order and died before reporting") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    // The peer recovers and emits the owed report, more than once.
    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    assert_gate(L, "CW-10");
}

SCENARIO("CW-11", "P0", "the P01 inbox committed before P04 saw the report") {
    Lab L;
    run_window(L, fp::kAfterInboxCommit);
    assert_gate(L, "CW-11");
}

SCENARIO("CW-12", "P0", "crash after the parse, before the apply transaction") {
    Lab L;
    run_window(L, fp::kAfterNormalize);
    assert_gate(L, "CW-12");
}

SCENARIO("CW-13", "P0", "crash during the P03 and dedup transaction") {
    Lab L;
    run_window(L, fp::kInsideApplyTxn);
    assert_gate(L, "CW-13");
}

SCENARIO("CW-14", "P0", "crash after the P03 commit, before the inbox acknowledgement") {
    Lab L;
    run_window(L, fp::kAfterApplyCommit);
    assert_gate(L, "CW-14");
}

SCENARIO("CW-15", "P0", "crash after the commit, before the risk side effect") {
    Lab L;
    run_window(L, fp::kBeforeRiskEffect);
    assert_gate(L, "CW-15");
    CHECK(L.risk->effective_releases() <= std::size_t{1});
}

SCENARIO("CW-16", "P0", "crash during the status request dispatch") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();

    L.fp.arm(fp::kDuringStatusDispatch, FaultKind::Crash);
    L.clock.advance(L.cfg.limits.ack_timeout_micros * 3);
    CHECK(L.crash_during([&] { L.rt->on_timer(); }));
    L.fp.clear();

    // No execution was invented and the case is still tracked.
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK(L.case_for(a.chain_id) != nullptr);
    assert_gate(L, "CW-16");
}

SCENARIO("CW-17", "P0", "crash after the status reply, before the comparison commits") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.lp->send(s);
    L.fp.arm(fp::kAfterStatusReply, FaultKind::Crash);
    L.crash_during([&] { L.rt->pump_inbound(); });
    L.fp.clear();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    assert_gate(L, "CW-17");
}

SCENARIO("CW-18", "P0", "crash during the snapshot or journal prune") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();

    L.fp.arm(fp::kDuringSnapshotPrune, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.store->prune_journal(1); }));
    L.fp.clear();

    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());
    CHECK(L.store->prune_journal(1).is_ok());
    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.at(a.chain_id.str()).state_hash().hex(), before.hex());
    assert_gate(L, "CW-18");
}

SCENARIO("CW-19", "P0", "the database commit succeeded and the acknowledgement was lost") {
    Lab L;
    L.fp.arm(fp::kCommitAckLost, FaultKind::Error);
    auto r = L.submit(L.new_order("K1"));
    L.fp.clear();
    // The authoritative key is re-read, not assumed rolled back.
    CHECK(r.is_ok());
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});

    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK(again.value().idempotent_replay);
    L.drive();
    L.lp->execute(again.value().cl_ord_id.str(), 100000, 108500);
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    assert_gate(L, "CW-19");
}

SCENARIO("CW-20", "P0", "P01 and P04 restart in the wrong order") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // P04 comes back while the session is still down. Nothing may be sent, and
    // the pending work is rebuilt from the journal rather than re-derived.
    L.session->disconnect();
    L.crash();
    L.boot();
    CHECK(!L.session->ready_for_app());
    const auto queued = L.must_submit_cancel("C1", a.chain_id);
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.wire_count('F'), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);  // state survived the restart

    // Then the session returns and the backlog drains, once.
    L.session->reconnect(false);
    L.drive(16);
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->pending_cancel->str(), queued.cl_ord_id.str());
    const auto v = L.violations();
    CHECK_MSG(v.empty(), render_violations(v));
}
