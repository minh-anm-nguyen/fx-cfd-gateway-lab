// Group J -- adversarial interleavings of fill, cancel and replace
// (design doc 18.J). These are the orderings that cost money when a state
// machine guesses: the commit order decides, and where it cannot, the chain
// stops.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("J-01", "P0", "cancel, fill, pending, final: the fill counts and the cancel is final") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "20000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(trade("X2", "O1")
                       .status(ord_status::kPendingCancel)
                       .lastq("30000")
                       .lastpx("1.08495")
                       .cum("50000"))
              .ok());
    CHECK(L.submit(pending_cancel("C1")).ok());
    CHECK(L.submit(canceled("C1").cum("50000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.cum_qty_units, int64_t{50000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.ledger.size(), size_t{2});
    const JournalOracle o = oracle(L.store(), id);
    CHECK_EQ(o.cum, int64_t{50000});
    CHECK_EQ(o.exec_ids.size(), size_t{2});
    CHECK(check_invariants(a, L.policy()).empty());
}

SCENARIO("J-02", "P0", "pending cancel, partial fill, cancel reject: still working") {
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
    CHECK(L.submit(cancel_reject("C1", "O1", "1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK(!a.pending.has_value());
    CHECK(!a.peer_pending_cancel);
    CHECK_EQ(a.leaves_qty_units(), int64_t{60000});
}

SCENARIO("J-03", "P0", "pending cancel, full fill, cancel reject: Filled, not reopened") {
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
    CHECK(L.submit(cancel_reject("C1", "O1", "1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK(!a.pending.has_value());
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
}

SCENARIO("J-04", "P0", "a final cancel after a full fill is a contradiction, and the fill stands") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingCancel)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    CHECK_EQ(L.lc(id), Lifecycle::Filled);

    expect_quarantine(L.submit(canceled("C1").cum("100000")), Reason::ContradictoryResponse);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});  // nothing rolled back
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.reconciliation_required);
}

SCENARIO("J-05", "P0", "a trade delivered after a confirmed cancel is never applied silently") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);

    expect_quarantine(L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08495")),
                      Reason::PostTerminalEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.reconciliation_required);
}

SCENARIO("J-06", "P0", "replace intent, fill, replace ack: fill on the old version, cum kept") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.accepted_qty_units(), int64_t{150000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger[0].accepted_version_seen, uint32_t{0});
    CHECK_EQ(a.leaves_qty_units(), int64_t{110000});
}

SCENARIO("J-07", "P0", "pending replace, several fills, replace reject: all fills retained") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("20000")
                       .lastpx("1.08490")
                       .cum("20000"))
              .ok());
    CHECK(L.submit(trade("X2", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("30000")
                       .lastpx("1.08492")
                       .cum("50000"))
              .ok());
    CHECK(L.submit(cancel_reject("R1", "O1", "2")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{50000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{50000});
}

SCENARIO("J-08", "P0", "a replace ack after the order filled is refused in v1") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    CHECK_EQ(L.lc(id), Lifecycle::Filled);

    expect_quarantine(L.submit(replaced("R1", "O1").oqty("150000")), Reason::PostTerminalEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
}

SCENARIO("J-09", "P0", "a stale pending-replace report does not downgrade an accepted version") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Working);

    expect(L.submit(pending_replace("R1", "E-PR-LATE")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Working);
    CHECK(!a.peer_pending_replace);
    CHECK_EQ(a.accepted_qty_units(), int64_t{150000});
}

SCENARIO("J-10", "P0", "a late old-version fill after the replacement is not applied twice") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").seq(20)).ok());

    // An earlier source cursor, but the version it names is already superseded.
    expect_quarantine(L.submit(trade("X1", "O1").seq(5).lastq("40000").lastpx("1.08490")),
                      Reason::LineageMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{0});
}

SCENARIO("J-11", "P0", "a cancel and a replace from two producers: exactly one is admitted") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_reject(L.submit(cmd_replace("R1", "O1", "150000")), Reason::PendingMutation);
    CHECK_EQ(L.state(id).pending->kind, CommandKind::Cancel);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{1});
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{0});
}

SCENARIO("J-12", "P0", "two cancels arriving together: one request is sent") {
    Lab L;
    const OrderId id = L.working("O1");
    const EngineResult a = L.submit(cmd_cancel("C1", "O1"));
    const EngineResult b = L.submit(cmd_cancel("C2", "O1"));
    CHECK(a.ok());
    expect_reject(b, Reason::PendingMutation);
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("C1"));
    size_t sent = 0;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.kind == EffectIntent::Kind::RouteCancel) ++sent;
    }
    CHECK_EQ(sent, size_t{1});
}

SCENARIO("J-13", "P0", "two replaces arriving together: one request is sent") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    expect_reject(L.submit(cmd_replace("R2", "O1", "160000")), Reason::PendingMutation);
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("R1"));
    size_t sent = 0;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.kind == EffectIntent::Kind::RouteReplace) ++sent;
    }
    CHECK_EQ(sent, size_t{1});
}

SCENARIO("J-14", "P0", "a trade and a final cancel at one instant follow the commit order") {
    // Wall-clock simultaneity is not a tie the state machine has to break:
    // the durable commit order is total, and each order is deterministic.
    const auto run_trade_first = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
        CHECK(L.submit(trade("X1", "O1")
                           .status(ord_status::kPendingCancel)
                           .lastq("40000")
                           .lastpx("1.08490")
                           .cum("40000")
                           .sending("20261030-12:00:00.000"))
                  .ok());
        CHECK(L.submit(canceled("C1").cum("40000").sending("20261030-12:00:00.000")).ok());
        return L.state(id).state_hash();
    };
    const auto run_cancel_first = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
        CHECK(L.submit(canceled("C1").cum("0").sending("20261030-12:00:00.000")).ok());
        const EngineResult late = L.submit(trade("X1", "O1")
                                               .lastq("40000")
                                               .lastpx("1.08490")
                                               .sending("20261030-12:00:00.000"));
        expect_quarantine(late, Reason::PostTerminalEvent);
        return L.state(id).state_hash();
    };

    // Each ordering is reproducible.
    CHECK_EQ(run_trade_first(), run_trade_first());
    CHECK_EQ(run_cancel_first(), run_cancel_first());
    // And the two orderings are genuinely different histories, not a coin flip
    // resolved by timestamps.
    CHECK_NE(run_trade_first(), run_cancel_first());
}

SCENARIO("J-15", "P0", "a trade and a replace ack at one instant replay to the same hash") {
    const auto run = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
        CHECK(L.submit(trade("X1", "O1")
                           .status(ord_status::kPendingReplace)
                           .lastq("40000")
                           .lastpx("1.08490")
                           .cum("40000")
                           .sending("20261030-12:00:00.000"))
                  .ok());
        CHECK(L.submit(replaced("R1", "O1")
                           .oqty("150000")
                           .cum("40000")
                           .sending("20261030-12:00:00.000"))
                  .ok());
        return L.state(id).state_hash();
    };
    CHECK_EQ(run(), run());

    // The same trace replayed from the journal lands on the same hash.
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000"))
              .ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());
    const uint64_t before = L.state(id).state_hash();
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).state_hash(), before);
}

SCENARIO("J-16", "P0", "responses to two different requests do not both mutate the chain") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    // A replace acknowledgement turns up although the outstanding request is a
    // cancel. One of the two statements is wrong; neither version moves.
    expect_quarantine(L.submit(replaced("R1", "O1").oqty("150000")), Reason::UnknownRequest);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK(a.reconciliation_required);
}

SCENARIO("J-17", "P0", "a disconnect during a pending cancel conserves CumQty on reconnect") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "20000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "C1")).ok());
    CHECK(L.state(id).pending->outcome_unknown);

    // On reconnect the session resends a fill we already have and then the
    // final cancel.
    CHECK(L.submit(trade("X2", "O1")
                       .epoch(2)
                       .seq(1)
                       .lastq("30000")
                       .lastpx("1.08495")
                       .cum("50000"))
              .ok());
    expect_decision(L.submit(trade("X2", "O1")
                                 .epoch(2)
                                 .seq(2)
                                 .resend()
                                 .lastq("30000")
                                 .lastpx("1.08495")
                                 .cum("50000")),
                    Decision::DuplicateNoop);
    CHECK(L.submit(canceled("C1").cum("50000").epoch(2).seq(3)).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.cum_qty_units, int64_t{50000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{50000});
}

SCENARIO("J-18", "P0", "a disconnect during a pending replace yields one version and one fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "R1")).ok());

    const auto fill = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").epoch(2);
    CHECK(L.submit(fill).ok());
    expect_decision(L.submit(fill), Decision::DuplicateNoop);
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000").epoch(2)).ok());
    expect_decision(L.submit(replaced("R1", "O1").oqty("150000").cum("40000").epoch(2)),
                    Decision::DuplicateNoop);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK(!a.pending.has_value());
    // No extra routing command came out of the reconnect.
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{1});
}

SCENARIO("J-19", "P0", "a Canceled snapshot hiding an unseen fill is held, not adjusted") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "C1")).ok());

    // The counterparty says canceled at 70000 executed; the ledger knows
    // 40000. The missing 30000 has no execution identity, so nothing is
    // synthesised.
    expect_quarantine(
        L.submit(status_snapshot("O1").status(ord_status::kCanceled).cum("70000").gen(1)),
        Reason::MissingFillEvidence);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK(a.reconciliation_required);
}

SCENARIO("J-20", "P0", "a crash between a trade and a cancel result recovers to the serial answer") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());

    // The process dies after the trade was reduced but before its transaction.
    L.store().failpoint().arm(CrashWindow::W04_AfterReduceBeforeTxn);
    bool crashed = false;
    try {
        L.submit(trade("X2", "O1")
                     .status(ord_status::kPendingCancel)
                     .lastq("30000")
                     .lastpx("1.08495")
                     .cum("70000"));
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    // Recovery, then the durable inbox redelivers the trade and the cancel
    // completes.
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});  // the uncommitted fill is gone
    CHECK_EQ(L.engine().pump_all() > 0, true);            // the inbox still holds it
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{70000});
    CHECK(L.submit(canceled("C1").cum("70000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.cum_qty_units, int64_t{70000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{70000});
    CHECK(check_invariants(a, L.policy()).empty());
}
