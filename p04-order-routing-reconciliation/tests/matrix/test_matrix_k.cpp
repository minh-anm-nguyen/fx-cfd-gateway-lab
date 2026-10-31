// Section K: business dedup, ExecID conflicts and idempotent apply.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

namespace {

struct Filled40k {
    Lab lab;
    AcceptedIntent order;
    DurableReceiptId trade_receipt = 0;
    Filled40k() {
        order = lab.must_submit_new("K1");
        lab.drive();
        trade_receipt = lab.lp->execute(order.cl_ord_id.str(), 40000, 108500);
        lab.drive();
    }
};

}  // namespace

SCENARIO("OR-K-01", "P0", "a receipt redelivered before the acknowledgement applies once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const DurableReceiptId receipt = L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);

    // P01 hands the very same receipt out again.
    for (int i = 0; i < 5; ++i) {
        L.session->redeliver(receipt);
        L.rt->pump_inbound();
    }
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-K-02", "P0", "the same execution on a different sequence applies once") {
    Filled40k f;
    p02::ReportSpec again = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 40000, 108500);
    again.cum_qty = 40000;
    again.leaves_qty = 60000;
    f.lab.inject(again);  // a brand new receipt and MsgSeqNum
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-03", "P0", "a duplicate after a restart leaves the ledger unchanged") {
    Filled40k f;
    const Hash before = f.lab.chain(f.order.chain_id)->state_hash();
    f.lab.crash();
    f.lab.boot();

    p02::ReportSpec again = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 40000, 108500);
    again.cum_qty = 40000;
    again.leaves_qty = 60000;
    f.lab.inject(again);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->state_hash().hex(), before.hex());
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-04", "P0", "a replay after a day reset is still deduplicated") {
    Filled40k f;
    f.lab.session->reconnect(true);  // new epoch, sequences restart
    p02::ReportSpec again = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 40000, 108500);
    again.cum_qty = 40000;
    again.leaves_qty = 60000;
    f.lab.inject(again);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-05", "P0", "the same ExecID with a different quantity is a conflict") {
    Filled40k f;
    p02::ReportSpec conflicting = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 50000, 108500);
    conflicting.cum_qty = 50000;
    conflicting.leaves_qty = 50000;
    f.lab.inject(conflicting);

    // The original fill stands, nothing is overwritten, and a case is open.
    const p03::ChainState* c = f.lab.chain(f.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.at(0).qty, 40000);
    CHECK_EQ(c->hold.value(), HoldReason::ExecIdConflict);
    CHECK(f.lab.metrics.counter(metric::kExecIdConflicts) >= 1);
    CHECK(f.lab.case_for(f.order.chain_id) != nullptr);
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-06", "P0", "the same ExecID with a different price does not adjust the fill") {
    Filled40k f;
    p02::ReportSpec conflicting = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 40000, 108900);
    conflicting.cum_qty = 40000;
    conflicting.leaves_qty = 60000;
    f.lab.inject(conflicting);

    const p03::ChainState* c = f.lab.chain(f.order.chain_id);
    CHECK_EQ(c->fills.at(0).px, 108500);
    CHECK_EQ(c->avg_px(), 108500);
    CHECK_EQ(c->hold.value(), HoldReason::ExecIdConflict);
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-07", "P0", "one ExecID at two LPs is two separate executions") {
    Lab L;
    // The dedup key is LP scoped, so the same value from another LP namespace is
    // a different key entirely.
    BusinessExecKey a;
    a.lp_namespace = "LP_A";
    a.semantic_exec_id = "EX1";
    a.payload_fingerprint = sha256("a");
    a.chain_id = OrderChainId("ord-A");
    BusinessExecKey b = a;
    b.lp_namespace = "LP_B";
    b.payload_fingerprint = sha256("b");
    b.chain_id = OrderChainId("ord-B");
    CHECK_NE(a.key(), b.key());

    const auto one = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(one.cl_ord_id.str(), 40000, 108500);
    L.drive();
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|EX-1"), std::size_t{1});
    CHECK_EQ(L.disk.exec_keys.count("LP_OTHER|EX-1"), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-K-08", "P1", "an ambiguous dedup namespace fails closed rather than merging") {
    Filled40k f;
    // The same ExecID arriving against a different chain of the same LP is a
    // collision the ROE does not resolve, so it is held rather than credited.
    const auto other = f.lab.must_submit_new("K2");
    f.lab.drive();
    p02::ReportSpec s = f.lab.lp->trade(other.cl_ord_id.str(), "EX-1", 10000, 108500);
    s.cum_qty = 10000;
    s.leaves_qty = 90000;
    f.lab.inject(s);

    CHECK_EQ(f.lab.chain(other.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK(f.lab.chain(other.chain_id)->hold.has_value());
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-09", "P0", "two status snapshots with ExecID 0 do not collide") {
    Lab L;
    L.cfg.capabilities.order_status_request = true;
    L.cfg.finalize();
    L.lp_disk.capability_status_request = true;
    const auto a = L.must_submit_new("K1");
    L.drive();

    for (int i = 0; i < 5; ++i)
        L.inject(L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt));

    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    for (const auto& [k, ek] : L.disk.exec_keys) {
        (void)k;
        CHECK_NE(ek.semantic_exec_id, std::string("0"));
    }
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-K-10", "P0", "a Trade carrying ExecID 0 is outside the agreed namespace") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "0", 40000, 108500);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    bool conflict = false;
    for (const QuarantineEntry& q : L.disk.quarantine)
        if (q.reason == HoldReason::ExecIdConflict) conflict = true;
    CHECK(conflict);
    // The sentinel did not enter the uniqueness index, so it poisoned nothing.
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|0"), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-K-11", "P0", "a fill that also appears in a snapshot is counted once") {
    Filled40k f;
    p02::ReportSpec s = f.lab.lp->status_snapshot(f.order.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    s.ord_status = OrdStatus::PartiallyFilled;
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), std::size_t{1});
    CHECK(!f.lab.chain(f.order.chain_id)->hold.has_value());
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-12", "P0",
         "a snapshot ahead of the ledger asks for history, never invents a fill") {
    Filled40k f;
    p02::ReportSpec s = f.lab.lp->status_snapshot(f.order.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    f.lab.inject(s);

    const p03::ChainState* c = f.lab.chain(f.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);  // no synthetic 60000
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->hold.value(), HoldReason::HistoryRequired);
    CHECK(f.lab.case_for(f.order.chain_id) != nullptr);
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-13", "P0", "a snapshot behind the ledger does not roll back known fills") {
    Filled40k f;
    p02::ReportSpec s = f.lab.lp->status_snapshot(f.order.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 0;
    s.leaves_qty = 100000;
    s.ord_status = OrdStatus::New;
    f.lab.inject(s);

    const p03::ChainState* c = f.lab.chain(f.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->hold.value(), HoldReason::SnapshotMismatch);
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-14", "P0", "a crash after the commit but before the notification applies once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    L.fp.arm(fp::kAfterApplyCommit, FaultKind::Crash);
    const bool died = L.crash_during([&] { L.rt->pump_inbound(); });
    CHECK(died);
    L.fp.clear();

    // The commit landed; the redelivery after the restart is a no-op.
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-K-15", "P0", "a side effect that failed is retried without releasing twice") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string reservation = L.intent("K1")->risk_reservation_id;
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);

    // The process dies on the way to the risk release.
    L.fp.arm(fp::kBeforeRiskEffect, FaultKind::Crash);
    const bool died = L.crash_during([&] { L.rt->pump_inbound(); });
    CHECK(died);
    L.fp.clear();
    CHECK(L.risk->held(reservation));

    // Replaying it lands the release once.
    L.session->redeliver(2);
    L.rt->pump_inbound();
    L.rt->pump_inbound();
    CHECK(L.risk->effective_releases() <= std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-K-16", "P0", "an ExecID claimed by a second chain is a conflict, not a credit") {
    Filled40k f;
    const auto other = f.lab.must_submit_new("K2");
    f.lab.drive();
    p02::ReportSpec s = f.lab.lp->trade(other.cl_ord_id.str(), "EX-1", 40000, 108500);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    f.lab.inject(s);

    CHECK_EQ(f.lab.chain(other.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.disk.exec_keys.at("LP_SIM_01|EX-1").chain_id.str(), f.order.chain_id.str());
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-17", "P0", "a PossDup replay with changed content is not quietly ignored") {
    Filled40k f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX-1", 40000, 109000);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    s.poss_dup = true;
    f.lab.inject(s);

    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.at(0).px, 108500);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->hold.value(), HoldReason::ExecIdConflict);
    CHECK(f.lab.metrics.counter(metric::kExecIdConflicts) >= 1);
    CHECK_INV(f.lab);
}

SCENARIO("OR-K-18", "P0", "a thousand redeliveries of one receipt produce one effect") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const DurableReceiptId receipt = L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();
    const std::size_t receipts_after_first = L.disk.applied_receipts.size();

    for (int i = 0; i < 1000; ++i) {
        L.session->redeliver(receipt);
        L.rt->pump_inbound();
    }
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    // The applied-receipt table did not grow: it is the same key every time.
    CHECK_EQ(L.disk.applied_receipts.size(), receipts_after_first);
    CHECK_INV(L);
}

SCENARIO("OR-K-19", "P0", "a duplicated cancel reject does not reopen the order") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    const p02::ReportSpec rej = L.lp->cancel_reject(
        c.cl_ord_id.str(), a.cl_ord_id.str(), CxlRejResponseTo::OrderCancelRequest, 0);
    L.inject(rej);

    for (int i = 0; i < 3; ++i) L.inject(rej);
    // The request is closed once. A 35=9 carries no ExecID, so the repeats are
    // refused as lineage contradictions rather than applied again.
    CHECK(!L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_INV(L);
}

SCENARIO("OR-K-20", "P0", "a replace acceptance seen three times activates the version once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    const p02::ReportSpec accept =
        L.lp->replaced(r.cl_ord_id.str(), a.cl_ord_id.str(), 80000, 108490);

    L.inject(accept);
    const StateVersion v = L.chain(a.chain_id)->version;
    L.inject(accept);
    L.inject(accept);

    CHECK_EQ(L.chain(a.chain_id)->version, v);
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(L.chain(a.chain_id)->accepted_qty, 80000);
    CHECK(L.disk.audit.count(AuditKind::InboundDeduplicated) >= std::size_t{2});
    CHECK_INV(L);
}
