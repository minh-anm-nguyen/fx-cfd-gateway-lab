// Group H -- cancel flow, reject and terminal semantics (design doc 18.H).
//
// The single rule behind the whole group: Pending Cancel is not Canceled, and
// only a valid final acknowledgement makes it so.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("H-01", "P0", "a cancel on a working order is pending, not terminal") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
    CHECK(!a.terminal());
    CHECK_EQ(a.leaves_qty_units(), int64_t{100000});
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{1});
}

SCENARIO("H-02", "P0", "a cancel on a partially filled order does not reverse the fill") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{60000});  // only the remainder is at stake
    CHECK_EQ(a.ledger.size(), size_t{1});
}

SCENARIO("H-03", "P0", "a cancel after Filled is refused with no routing") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    expect_reject(L.submit(cmd_cancel("C1", "O1")), Reason::TerminalState);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{0});
    CHECK_EQ(L.lc(id), Lifecycle::Filled);
}

SCENARIO("H-04", "P0", "a second cancel after Canceled is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    expect_reject(L.submit(cmd_cancel("C2", "O1")), Reason::TerminalState);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C2"), size_t{0});
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("H-05", "P0", "a cancel while the New is unacknowledged is refused") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1"));
    CHECK(n.ok());
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    expect_reject(L.submit(cmd_cancel("C1", "O1")), Reason::UnsafeDependentCommand);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{0});
    CHECK(!L.state(n.id).pending.has_value() || L.state(n.id).pending->kind == CommandKind::New);
}

SCENARIO("H-06", "P0", "a peer pending-cancel report assumes nothing about leaves") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK(a.peer_pending_cancel);
    CHECK_EQ(a.leaves_qty_units(), int64_t{100000});
    CHECK(!a.terminal());
    CHECK_EQ(a.pending->phase, PendingRequest::Phase::PeerPending);
}

SCENARIO("H-07", "P0", "a final cancel with CumQty zero is terminal with leaves zero") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, int64_t{0});
}

SCENARIO("H-08", "P0", "a final cancel with a partial fill keeps the fill and zeroes leaves") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{40000});
}

SCENARIO("H-09", "P0", "a valid cancel reject leaves the order working") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(cancel_reject("C1", "O1", "1").rejreason("0")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK(!a.pending.has_value());
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{60000});
}

SCENARIO("H-10", "P0", "a reject tagged as answering a replace does not resolve a cancel") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(cancel_reject("C1", "O1", "2")), Reason::ContradictoryResponse);
    // The cancel request is still outstanding.
    CHECK(L.state(id).pending.has_value());
    CHECK_EQ(L.state(id).pending->kind, CommandKind::Cancel);
}

SCENARIO("H-11", "P0", "UnknownOrder on a cancel reject means reconcile, never resend") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const EngineResult r = L.submit(cancel_reject("C1", "O1", "1").rejreason("1"));
    CHECK(r.ok());
    CHECK_EQ(r.reason, Reason::UnknownOutcome);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.confidence, Confidence::Unknown);
    CHECK(a.reconciliation_required);
    CHECK_EQ(a.lifecycle(), Lifecycle::ReconciliationRequired);
    CHECK_EQ(L.engine().sink().count_key("query-status|1|unknown-order"), size_t{1});
    // Dependent commands are blocked until this is resolved.
    expect_reject(L.submit(cmd_cancel("C2", "O1")), Reason::UnsafeDependentCommand);
}

SCENARIO("H-12", "P0", "a second cancel while the first is pending is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_reject(L.submit(cmd_cancel("C2", "O1")), Reason::PendingMutation);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C2"), size_t{0});
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("C1"));
}

SCENARIO("H-13", "P0", "a fill while the cancel is peer-pending applies once and keeps pending") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingCancel)
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000"))
              .ok());
    expect_decision(L.submit(trade("X1", "O1")
                                 .status(ord_status::kPendingCancel)
                                 .lastq("40000")
                                 .lastpx("1.08490")
                                 .cum("40000")),
                    Decision::DuplicateNoop);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
}

SCENARIO("H-14", "P0", "a fill that exhausts the quantity leaves the cancel to reconcile") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingCancel)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    // The cancel's own disposition is still open and flagged.
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->kind, CommandKind::Cancel);
    CHECK(a.reconciliation_required);
}

SCENARIO("H-15", "P0", "a cancel timeout never marks the order canceled") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const EngineResult t = L.submit(sys(SystemKind::AckTimeout, "C1"));
    CHECK(t.ok());
    CHECK_EQ(t.reason, Reason::UnknownOutcome);

    const OrderAggregate& a = L.state(id);
    CHECK(!a.terminal());
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK(a.pending->outcome_unknown);
    CHECK_EQ(a.lifecycle(), Lifecycle::ReconciliationRequired);
    // The order itself is still known to exist.
    CHECK_EQ(a.confidence, Confidence::Confirmed);
    CHECK_EQ(L.engine().sink().count_key("query-status|1|C1"), size_t{1});
}

SCENARIO("H-16", "P0", "a final cancel naming the wrong request terminates nothing") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    // Carries the order's OrderID, so it correlates to this chain and is
    // judged on its request lineage rather than discarded as an orphan.
    expect_quarantine(L.submit(canceled("CX").oid("LPO-O1").cum("0")),
                      Reason::UnknownRequest);
    CHECK(!L.state(id).terminal());
    CHECK(L.state(id).pending.has_value());
}

SCENARIO("H-17", "P0", "a final cancel reporting less CumQty than the ledger is quarantined") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(canceled("C1").cum("10000")), Reason::CumRegression);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK(a.done_reason != DoneReason::Canceled);
}

SCENARIO("H-18", "P0", "a final cancel carrying nonzero LeavesQty is inconsistent") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(canceled("C1").cum("0").leaves("50000")),
                      Reason::LeavesInconsistent);
    CHECK(!L.state(id).terminal());
}

SCENARIO("H-19", "P0", "a cancel reject with no request outstanding is quarantined") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(L.submit(cancel_reject("C1", "O1", "1")), Reason::UnknownRequest);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK(!a.pending.has_value());
}

SCENARIO("H-20", "P1", "a 35=8 reject where 35=9 is required does not cancel the order") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    // The peer answers the cancel with an ExecutionReport reject instead of an
    // OrderCancelReject. It names the cancel request, not the order.
    expect_quarantine(L.submit(rejected("C1")), Reason::ProtocolMismatch);
    const OrderAggregate& a = L.state(id);
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK(a.done_reason != DoneReason::Rejected);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
}
