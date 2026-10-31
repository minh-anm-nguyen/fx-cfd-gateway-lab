// Group F -- business dedup, replay and idempotency (design doc 18.F).
//
// Two kinds of duplicate meet here. A transport duplicate is the same logical
// event redelivered with different session fields; a business duplicate is the
// same ExecID carrying different business fields. The first is a no-op, the
// second is a conflict, and nothing in between.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("F-01", "P0", "the same trade delivered twice applies once") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto first = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK(L.submit(first).ok());
    expect(L.submit(first), Decision::DuplicateNoop, Reason::DuplicateSamePayload);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{1});
    CHECK_EQ(L.engine().metrics().duplicates, uint64_t{1});
}

SCENARIO("F-02", "P0", "the same ExecID with a different LastQty is a conflict") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    expect_quarantine(L.submit(trade("X1", "O1").lastq("50000").lastpx("1.08490").cum("50000")),
                      Reason::ConflictingDuplicate);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});  // the first committed fill stands
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(a.ledger[0].qty_units, int64_t{40000});
}

SCENARIO("F-03", "P0", "the same ExecID with a different LastPx is a conflict") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    const Int128 notional = L.state(id).notional_ticks_x_qty;
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08495").cum("40000")),
                      Reason::ConflictingDuplicate);
    CHECK_EQ(to_string(L.state(id).notional_ticks_x_qty), to_string(notional));
}

SCENARIO("F-04", "P0", "the same ExecID naming another order is never remapped") {
    Lab L;
    const OrderId first = L.working("O1", "100000");
    const OrderId second = L.working("O2", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // X1 now arrives claiming to belong to O2. The key is already owned by
    // chain one, so the engine refuses before the reducer sees it.
    expect_quarantine(L.submit(trade("X1", "O2").lastq("40000").lastpx("1.08490").cum("40000")),
                      Reason::ConflictingDuplicate);
    CHECK_EQ(L.state(first).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(second).cum_qty_units, int64_t{0});
}

SCENARIO("F-05", "P0", "transport fields changing does not defeat semantic dedup") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000")
                       .seq(10)
                       .sending("20261030-09:00:00.000"))
              .ok());

    // Same execution, resent: new sequence, new sending time, PossResend set.
    expect(L.submit(trade("X1", "O1")
                        .lastq("40000")
                        .lastpx("1.08490")
                        .cum("40000")
                        .seq(99)
                        .epoch(2)
                        .resend()
                        .dup()
                        .sending("20261030-09:05:00.000")),
           Decision::DuplicateNoop, Reason::DuplicateSamePayload);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("F-06", "P0", "a session sequence reset does not reset business dedup") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").seq(5)).ok());

    // The FIX session reset its numbering; epoch 2 starts from sequence 1.
    expect(L.submit(trade("X1", "O1")
                        .lastq("40000")
                        .lastpx("1.08490")
                        .cum("40000")
                        .epoch(2)
                        .seq(1)
                        .session("S2")),
           Decision::DuplicateNoop, Reason::DuplicateSamePayload);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    // The durable dedup row survived the reset.
    CHECK(L.store().applied_keys().count("LP1|FIRM-DEMO:2026-10-30|X1") == 1);
}

SCENARIO("F-07", "P0", "resubmitting the same New business identity creates nothing new") {
    Lab L;
    const EngineResult first = L.submit(cmd_new("O1"));
    CHECK(first.ok());
    LocalCommand again = cmd_new("O1");
    again.command_seq = 77;  // a different producer attempt, same business intent
    expect(L.submit(again), Decision::DuplicateNoop, Reason::DuplicateSamePayload);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O1"), size_t{1});
}

SCENARIO("F-08", "P0", "a blind retry under a fresh ClOrdID is refused until reconciled") {
    Lab L;
    CHECK(L.submit(cmd_new("O1", "100000", "1.08500")).ok());
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());

    // Same order, new identifier, while the first outcome is unknown. This is
    // the duplicate-order path the design doc calls a release blocker.
    expect_reject(L.submit(cmd_new("O2", "100000", "1.08500")),
                  Reason::UnsafeDependentCommand);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O2"), size_t{0});

    // Once reconciliation resolves the first order, a genuinely new order is
    // admitted again.
    SystemEvent resolved = sys(SystemKind::ReconcileResolved, "O1");
    resolved.evidence_provided = true;
    resolved.resolved_status = "REJECTED";
    CHECK(L.submit(resolved).ok());
    CHECK(L.submit(cmd_new("O3", "100000", "1.08500")).ok());
}

SCENARIO("F-09", "P0", "a duplicate cancel intent produces one cancel request") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect(L.submit(cmd_cancel("C1", "O1")), Decision::DuplicateNoop,
           Reason::DuplicateSamePayload);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{1});
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("C1"));
}

SCENARIO("F-10", "P0", "a duplicate replace intent produces one replace request") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    expect(L.submit(cmd_replace("R1", "O1", "120000")), Decision::DuplicateNoop,
           Reason::DuplicateSamePayload);
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{1});
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
}

SCENARIO("F-11", "P0", "a duplicate final cancel is a no-op") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    const uint64_t hash = L.state(id).state_hash();
    expect_decision(L.submit(canceled("C1").cum("0")), Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).state_hash(), hash);
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("F-12", "P0", "a duplicate replace acknowledgement adds no second version") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});

    expect_decision(L.submit(replaced("R1", "O1").oqty("120000")), Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});
}

SCENARIO("F-13", "P0", "a duplicate cancel reject derives no extra command") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(cancel_reject("C1", "O1", "1")).ok());
    const uint64_t hash = L.state(id).state_hash();
    const size_t effects = L.engine().sink().dispatched().size();

    expect_decision(L.submit(cancel_reject("C1", "O1", "1")), Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).state_hash(), hash);
    CHECK_EQ(L.engine().sink().dispatched().size(), effects);
}

SCENARIO("F-14", "P0", "a repeated status snapshot under a new ExecID creates no fill") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(status_snapshot("O1", "ST-1").status(ord_status::kPartiallyFilled).cum("40000"))
              .ok());
    CHECK(L.submit(status_snapshot("O1", "ST-2").status(ord_status::kPartiallyFilled).cum("40000"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{1});
}

SCENARIO("F-15", "P0", "two genuinely different partials each apply exactly once") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("30000").lastpx("1.08491").cum("70000")).ok());
    // Both redelivered.
    expect_decision(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
                    Decision::DuplicateNoop);
    expect_decision(L.submit(trade("X2", "O1").lastq("30000").lastpx("1.08491").cum("70000")),
                    Decision::DuplicateNoop);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{70000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    const JournalOracle o = oracle(L.store(), id);
    CHECK_EQ(o.exec_ids.size(), size_t{2});
    CHECK_EQ(o.cum, int64_t{70000});
}

SCENARIO("F-16", "P0", "a new trade ExecID repeating the old CumQty is a contradiction") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // X2 is a new execution with a positive LastQty, yet CumQty did not move.
    // Treating this as a harmless duplicate would lose the quantity. The
    // reported total is now below what the ledger proves, which is the
    // contradiction the chain stops on.
    expect_quarantine(L.submit(trade("X2", "O1").lastq("30000").lastpx("1.08491").cum("40000")),
                      Reason::CumRegression);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("F-17", "P0", "dedup rows cannot be evicted inside the resend horizon") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    const size_t before = L.store().applied_key_count();
    CHECK(before > 0);

    // Eviction without a proven safe watermark is refused outright.
    CHECK(!L.store().evict_applied_keys_before(1000000, false));
    CHECK_EQ(L.store().applied_key_count(), before);

    // And a configuration whose retention is shorter than the resend horizon
    // never starts.
    Policy bad = L.policy();
    bad.dedup_retention_units = 10;
    bad.fix_resend_horizon_units = 50;
    CHECK_EQ(bad.validate().code, ConfigErrorCode::RetentionBelowResendHorizon);

    // The fill is still protected: a resend is a no-op.
    expect_decision(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
                    Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("F-18", "P0", "two writers racing one chain leave exactly one committed fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto t = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK(L.submit(t).ok());
    const uint64_t revision = L.state(id).revision;

    // A second owner that still believes in the previous revision is refused
    // by the compare-and-set, so it cannot double-apply.
    PeerReport second = t.r;
    second.exec_id = "X2";
    second.last_qty = "10000";
    second.cum_qty = "50000";
    const CommitResult cr = L.engine().commit_as_second_writer(id, revision - 1, OrderEvent{second});
    CHECK_EQ(cr.status, CommitStatus::RevisionConflict);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(a.revision, revision);
}

SCENARIO("F-19", "P0", "a duplicated status report after a terminal does not regress it") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);

    CHECK(L.submit(status_snapshot("O1", "ST-1").status(ord_status::kCanceled).cum("0")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
    expect_decision(L.submit(status_snapshot("O1", "ST-1").status(ord_status::kCanceled).cum("0")),
                    Decision::DuplicateNoop);
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("F-20", "P0", "an outbox effect redelivered after a crash is not a second order") {
    Lab L;
    const std::string key = "route-new|FIRM|2026-10-30|O1";

    // The process dies after the routing command went out but before the row
    // was marked sent, which is crash window W11.
    L.store().failpoint().arm(CrashWindow::W11_AfterDispatchBeforeMarkSent);
    bool crashed = false;
    try {
        L.submit(cmd_new("O1"));
    } catch (const SimulatedCrash& c) {
        crashed = true;
        CHECK_EQ(c.window, CrashWindow::W11_AfterDispatchBeforeMarkSent);
    }
    CHECK(crashed);
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    // The command was handed out once and the row is still pending.
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});
    size_t pending = 0;
    for (const OutboxRow& row : L.store().outbox()) {
        if (!row.marked_sent) ++pending;
    }
    CHECK(pending >= size_t{1});

    // Recovery offers it again under the same stable identity. The port
    // recognises it, so no second order reaches the counterparty. Counted per
    // key, because the same recovery legitimately dispatches the risk
    // reservation intent that the crash had not reached yet.
    const auto route_new_sends = [&L, &key] {
        size_t n = 0;
        for (const OutboxRow& row : L.engine().sink().dispatched()) {
            if (row.idempotency_key == key) ++n;
        }
        return n;
    };
    CHECK_EQ(route_new_sends(), size_t{1});
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::Clean);
    CHECK_EQ(L.engine().sink().count_key(key), size_t{2});  // offered twice
    CHECK_EQ(route_new_sends(), size_t{1});                 // sent once
    CHECK(L.engine().sink().suppressed() >= size_t{1});
}
