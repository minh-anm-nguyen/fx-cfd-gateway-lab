// Fault-injection playbooks PB-01 .. PB-26 (design doc 19).
//
// Each playbook is a whole timeline, not a single assertion: set up, inject at
// a named faultpoint, recover or reconcile, then check an oracle that does not
// share code with the state machine (the journal oracle) plus the full
// invariant set. Where a crash window is involved the case asserts the
// faultpoint fired, because a window that never triggered is not a pass
// (design doc 20.6).
//
// Same scope caveat as group L: the store is in-memory, so these demonstrate
// the atomic-commit contract and deterministic replay, not end-to-end crash
// safety. That belongs to P06.
#include <functional>

#include "../support/lab.hpp"

using namespace lab;

namespace {

// Asserts the full safety set plus agreement with the independent oracle.
void assert_consistent(Lab& L, OrderId id) {
    const OrderAggregate& a = L.state(id);
    const std::vector<InvariantViolation> v = check_invariants(a, L.policy());
    CHECK_MSG(v.empty(), describe(v));
    CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
    CHECK_EQ(to_string(a.notional_ticks_x_qty), to_string(a.ledger_notional()));
    const JournalOracle o = oracle(L.store(), id);
    CHECK_MSG(o.cum == a.cum_qty_units,
              "journal oracle says " + std::to_string(o.cum) + ", aggregate says " +
                  std::to_string(a.cum_qty_units));
    CHECK_EQ(o.exec_ids.size(), a.ledger.size());
}

// A working order with one recorded fill: the starting point for the crash
// playbooks.
OrderId seeded_or(Lab& L) { return L.part_filled("O1", "100000", "40000"); }

bool crash_during(Lab& L, CrashWindow w, const std::function<void()>& action) {
    L.store().failpoint().arm(w);
    bool crashed = false;
    try {
        action();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK_MSG(L.store().failpoint().fired(),
              std::string("faultpoint ") + to_string(w) + " never fired");
    L.store().failpoint().disarm();
    return crashed;
}

}  // namespace

SCENARIO("PB-01", "P0", "a lost New acknowledgement is not resent as a new order") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000", "1.08500"));
    CHECK(n.ok());
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());

    // The only thing that may leave is a status query.
    CHECK_EQ(L.engine().sink().count_key("query-status|1|O1"), size_t{1});
    expect_reject(L.submit(cmd_new("O2", "100000", "1.08500")), Reason::UnsafeDependentCommand);
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O2"), size_t{0});
    CHECK_EQ(L.engine().chain_count(), size_t{1});

    // The answer resolves it, exactly once.
    CHECK(L.submit(new_ack("O1")).ok());
    CHECK_EQ(L.lc(n.id), Lifecycle::Working);
    CHECK_EQ(L.state(n.id).accepted_versions.size(), size_t{1});
    assert_consistent(L, n.id);
}

SCENARIO("PB-02", "P0", "a trade duplicated across a resend and a new session epoch") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto fill = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK(L.submit(fill.r).ok());

    // Same execution, three more times: a plain resend, a PossDup, and a fresh
    // session epoch after a sequence reset.
    expect_decision(L.submit(PeerReport(fill.r)), Decision::DuplicateNoop);
    PeerReport possdup = fill.r;
    possdup.poss_dup = true;
    possdup.source_seq = 99;
    expect_decision(L.submit(possdup), Decision::DuplicateNoop);
    PeerReport new_epoch = fill.r;
    new_epoch.session_epoch = 2;
    new_epoch.session_id = "S2";
    new_epoch.source_seq = 1;
    expect_decision(L.submit(new_epoch), Decision::DuplicateNoop);

    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
    assert_consistent(L, id);
}

SCENARIO("PB-03", "P0", "the same ExecID with an altered quantity is a conflict, not a fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    expect_quarantine(L.submit(trade("X1", "O1").lastq("45000").lastpx("1.08490").cum("45000")),
                      Reason::ConflictingDuplicate);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger[0].qty_units, int64_t{40000});
    CHECK_EQ(a.incidents.back().reason_code, std::string("CONFLICTING_DUPLICATE"));
    assert_consistent(L, id);
}

SCENARIO("PB-04", "P0", "cancel races a fill and the fill wins") {
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

    CHECK_EQ(L.lc(id), Lifecycle::Filled);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{100000});
    // The cancel's own fate is still an open reconciliation item.
    CHECK(L.state(id).pending.has_value());
    CHECK(L.state(id).reconciliation_required);
    assert_consistent(L, id);
}

SCENARIO("PB-05", "P0", "a cancel rejected after a full fill does not reopen the order") {
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
    CHECK(L.submit(cancel_reject("C1", "O1", "1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK(!a.pending.has_value());
    assert_consistent(L, id);
}

SCENARIO("PB-06", "P0", "a cancel timeout on a dead wire leaves the order known and the cancel not") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "C1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK(!a.terminal());
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK(a.pending->outcome_unknown);
    CHECK(a.reconciliation_required);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    // No second cancel goes out while the first is in the dark.
    expect_reject(L.submit(cmd_cancel("C2", "O1")), Reason::PendingMutation);
    assert_consistent(L, id);
}

SCENARIO("PB-07", "P0", "a replace races a partial fill on the old version") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000"))
              .ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.accepted_qty_units(), int64_t{150000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger[0].accepted_version_seen, uint32_t{0});
    CHECK_EQ(a.leaves_qty_units(), int64_t{110000});
    assert_consistent(L, id);
}

SCENARIO("PB-08", "P0", "a replace rejected while several fills are in flight keeps them all") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    for (int i = 0; i < 3; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .status(ord_status::kPendingReplace)
                           .lastq("10000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 10000)))
                  .ok());
    }
    CHECK(L.submit(cancel_reject("R1", "O1", "2")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{30000});
    CHECK_EQ(a.ledger.size(), size_t{3});
    CHECK(!a.pending.has_value());
    assert_consistent(L, id);
}

SCENARIO("PB-09", "P0", "a replace below the executed quantity never becomes a version") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    // Refused locally.
    expect_reject(L.submit(cmd_replace("R1", "O1", "30000")), Reason::QtyBelowCum);
    // And refused even if a peer claims to have accepted it.
    CHECK(L.submit(cmd_replace("R2", "O1", "70000")).ok());
    expect_quarantine(L.submit(replaced("R2", "O1").oqty("25000")), Reason::QtyBelowCum);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    assert_consistent(L, id);
}

SCENARIO("PB-10", "P0", "Pending Cancel is not Canceled, however long it lasts") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    for (int i = 0; i < 5; ++i) {
        // The peer keeps repeating the pending state.
        const EngineResult r = L.submit(pending_cancel("C1", "E-PC-" + std::to_string(i)));
        CHECK(r.decision == Decision::Applied || r.decision == Decision::DuplicateNoop);
        CHECK(!L.state(id).terminal());
        CHECK_EQ(L.lc(id), Lifecycle::PendingCancel);
        CHECK_EQ(L.state(id).leaves_qty_units(), int64_t{100000});
    }
    CHECK(L.state(id).done_reason != DoneReason::Canceled);
    assert_consistent(L, id);
}

SCENARIO("PB-11", "P0", "a terminal order receiving a late execution does not absorb it") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    expect_quarantine(L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08490")),
                      Reason::PostTerminalEvent);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.reconciliation_required);
    assert_consistent(L, id);
}

SCENARIO("PB-12", "P0", "W06: a crash after the dedup insert before the state write") {
    Lab L;
    const OrderId id = seeded_or(L);
    const size_t keys_before = L.store().applied_key_count();
    const uint64_t hash_before = L.state(id).state_hash();

    const bool crashed = crash_during(L, CrashWindow::W06_AfterDedupInsertBeforeState, [&] {
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    });
    CHECK(crashed);

    CHECK_EQ(L.store().applied_key_count(), keys_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).state_hash(), hash_before);
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    assert_consistent(L, id);
}

SCENARIO("PB-13", "P0", "W07: a crash after the state write before the outbox insert") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const size_t outbox_before = L.store().outbox().size();

    const bool crashed = crash_during(L, CrashWindow::W07_AfterStateBeforeOutbox,
                                      [&] { L.submit(cmd_cancel("C1", "O1")); });
    CHECK(crashed);
    CHECK_EQ(L.store().outbox().size(), outbox_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK(!L.state(id).pending.has_value());  // no state without its intent
    assert_consistent(L, id);
}

SCENARIO("PB-14", "P0", "W09: a crash after commit before the source acknowledgement") {
    Lab L;
    const OrderId id = seeded_or(L);
    const bool crashed = crash_during(L, CrashWindow::W09_AfterCommitBeforeInboxAck, [&] {
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    });
    CHECK(crashed);

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    EngineResult redelivered;
    CHECK(L.engine().pump(&redelivered));
    CHECK_EQ(redelivered.decision, Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
    assert_consistent(L, id);
}

SCENARIO("PB-15", "P0", "W11: a crash after the send before the sent marker") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const std::string key = "route-cancel|FIRM|2026-10-30|C1";
    const bool crashed = crash_during(L, CrashWindow::W11_AfterDispatchBeforeMarkSent,
                                      [&] { L.submit(cmd_cancel("C1", "O1")); });
    CHECK(crashed);
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.engine().sink().count_key(key), size_t{2});  // offered again
    size_t real = 0;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.idempotency_key == key) ++real;
    }
    CHECK_EQ(real, size_t{1});  // sent once
    CHECK(L.state(id).pending.has_value());
    assert_consistent(L, id);
}

SCENARIO("PB-16", "P0", "a corrupt snapshot with a valid journal recovers from the journal") {
    Lab L;
    const OrderId id = seeded_or(L);
    const uint64_t good = L.state(id).state_hash();

    Snapshot corrupt;
    corrupt.complete = true;
    corrupt.schema_version = L.policy().event_schema_version;
    corrupt.journal_watermark = L.store().journal().size() + 1;  // impossible
    OrderAggregate lie = L.state(id);
    lie.cum_qty_units = 999999;
    corrupt.aggregates[id.value] = lie;
    L.store().install_snapshot(corrupt);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SnapshotRejected);
    CHECK(rep.trading_allowed);
    CHECK_EQ(L.state(id).state_hash(), good);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    assert_consistent(L, id);
}

SCENARIO("PB-17", "P0", "a conflicting OrderID alias is not reassigned") {
    Lab L;
    const OrderId first = L.working("O1", "100000");
    const EngineResult second = L.submit(cmd_new("O2", "50000"));
    CHECK(second.ok());
    expect_quarantine(L.submit(new_ack("O2").oid("LPO-O1")), Reason::AliasCollision);

    CHECK(L.state(first).has_alias("LP1", "LPO-O1"));
    CHECK(!L.state(second.id).has_order_id("LPO-O1"));
    CHECK(!L.state(second.id).has_accepted());
    // The healthy chain still works.
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    assert_consistent(L, first);
}

SCENARIO("PB-18", "P0", "a spoofed cross-account execution changes nothing") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000", "1.08500", "EURUSD", "1", "ACC-A"));
    const EngineResult b = L.submit(cmd_new("B1", "100000", "1.08500", "EURUSD", "1", "ACC-B"));
    CHECK(L.submit(new_ack("A1").acct("ACC-A")).ok());
    CHECK(L.submit(new_ack("B1").acct("ACC-B")).ok());

    expect_quarantine(
        L.submit(trade("SPOOF", "A1").acct("ACC-B").lastq("100000").lastpx("1.08490")),
        Reason::AccountMismatch);
    expect_quarantine(
        L.submit(trade("SPOOF2", "A1").peer("EVIL").lastq("100000").lastpx("1.08490")),
        Reason::PeerMismatch);

    CHECK_EQ(L.state(a.id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(b.id).cum_qty_units, int64_t{0});
    assert_consistent(L, b.id);
}

SCENARIO("PB-19", "P0", "a status snapshot missing its fill evidence is not adopted") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "O1")).ok());
    expect_quarantine(L.submit(status_snapshot("O1")
                                   .status(ord_status::kPartiallyFilled)
                                   .cum("90000")
                                   .gen(1)),
                      Reason::MissingFillEvidence);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.reconciliation_required);
    assert_consistent(L, id);
}

SCENARIO("PB-20", "P0", "the FIX 4.4 ExecType compatibility trap is not fallen into") {
    // Each trap gets its own chain: the first quarantine would otherwise stop
    // the chain and mask whether the next one is detected at all.
    {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        expect_quarantine(
            L.submit(trade("X1", "O1").etype("1").lastq("40000").lastpx("1.08490")),
            Reason::WrongFixVersionSemantics);
        CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
        assert_consistent(L, id);
    }
    {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        expect_quarantine(
            L.submit(trade("X2", "O1").etype("2").lastq("40000").lastpx("1.08490")),
            Reason::WrongFixVersionSemantics);
        CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
        assert_consistent(L, id);
    }
    {
        // OrdStatus=5 Replaced is not a FIX 4.4 current state.
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
        expect_quarantine(L.submit(replaced("R1", "O1").status(ord_status::kReplacedObsolete)),
                          Reason::ObsoleteOrdStatus);
        CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
        CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});
        assert_consistent(L, id);
    }
}

SCENARIO("PB-21", "P0", "numeric and tick boundaries are refused at the edge") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40500").lastpx("1.08490")),
                      Reason::QtyStepMismatch);
    expect_quarantine(L.submit(trade("X2", "O1").lastq("40000").lastpx("1.084901")),
                      Reason::ScaleMismatch);
    expect_quarantine(
        L.submit(trade("X3", "O1").lastq("99999999999999999999").lastpx("1.08490")),
        Reason::MalformedNumber);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});

    // The valid neighbour still works.
    CHECK(L.submit(trade("X4", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    assert_consistent(L, id);
}

SCENARIO("PB-22", "P0", "a duplicate commit from a split-brain writer is fenced") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t revision = L.state(id).revision;
    PeerReport fill = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").r;
    fill.source_seq = 1;

    CHECK_EQ(L.engine().commit_as_second_writer(id, revision, OrderEvent{fill}).status,
             CommitStatus::Committed);
    // The same writer, or its twin, tries again at the revision it remembers.
    CHECK_EQ(L.engine().commit_as_second_writer(id, revision, OrderEvent{fill}).status,
             CommitStatus::RevisionConflict);

    size_t applied = 0;
    for (const JournalRecord& rec : L.store().journal()) {
        const auto* r = std::get_if<PeerReport>(&rec.event);
        if (r != nullptr && r->exec_id == "X1") ++applied;
    }
    CHECK_EQ(applied, size_t{1});
}

SCENARIO("PB-23", "P0", "a counterparty burst is absorbed by backpressure, not by dropping") {
    Policy p = default_lab_policy();
    p.limits.max_queue_events = 32;
    Lab L(p);
    const OrderId id = L.working("O1", "100000000");

    size_t admitted = 0;
    size_t refused = 0;
    for (int i = 0; i < 500; ++i) {
        PeerReport r = trade("X" + std::to_string(i), "O1").lastq("1000").lastpx("1.08490").r;
        r.source_seq = static_cast<uint64_t>(i + 1);
        if (L.engine().ingest(OrderEvent{r})) {
            ++admitted;
        } else {
            ++refused;
            // Drain and retry: this is the consumer being told to slow down,
            // which is what the durable inbox is for.
            L.engine().pump_all();
            if (L.engine().ingest(OrderEvent{r})) ++admitted;
        }
    }
    L.engine().pump_all();

    CHECK(refused > 0);  // the bound was really reached
    CHECK_EQ(admitted, size_t{500});
    CHECK_EQ(L.state(id).ledger.size(), size_t{500});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{500000});
    CHECK(L.engine().metrics().queue_high_water <= p.limits.max_queue_events);
    assert_consistent(L, id);
}

SCENARIO("PB-24", "P0", "two sources disagreeing about one chain is a reconciliation item") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "O1")).ok());

    // Source one: a status snapshot saying canceled at 40000.
    // Source two: a snapshot from the same generation saying 60000 executed.
    expect_quarantine(L.submit(status_snapshot("O1", "ST-B")
                                   .status(ord_status::kPartiallyFilled)
                                   .cum("60000")
                                   .gen(1)),
                      Reason::MissingFillEvidence);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK(a.reconciliation_required);
    CHECK(a.quarantined);
    // No automatic retry of anything.
    expect_reject(L.submit(cmd_cancel("C1", "O1")), Reason::TerminalState);
    assert_consistent(L, id);
}

SCENARIO("PB-25", "P0", "a full disk and an ambiguous commit both fail closed") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const uint64_t hash = L.state(id).state_hash();

    L.store().set_fail_next_commit(true, "no space left on device");
    const EngineResult full =
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(!full.committed);
    CHECK_EQ(L.state(id).state_hash(), hash);

    L.store().failpoint().arm(CrashWindow::W12_AmbiguousFlush);
    const EngineResult ambiguous =
        L.submit(trade("X3", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();
    CHECK(!ambiguous.committed);
    CHECK_EQ(ambiguous.commit_status, CommitStatus::Ambiguous);
    CHECK(!L.engine().trading_allowed());
    CHECK_EQ(L.state(id).state_hash(), hash);
    assert_consistent(L, id);
}

SCENARIO("PB-26", "P0", "a manual incident override needs evidence and dual control") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(L.submit(trade("X2", "O1").lastq("90000").lastpx("1.08490")),
                      Reason::Overfill);
    const uint64_t hash = L.state(id).state_hash();
    const size_t audit_before = L.engine().audit().size();

    SystemEvent bare = sys(SystemKind::ManualOverride, "O1");
    bare.resolved_status = "WORKING";
    expect_reject(L.submit(bare), Reason::NotAuthorized);
    CHECK_EQ(L.state(id).state_hash(), hash);
    CHECK(L.state(id).quarantined);
    CHECK(L.engine().audit().size() > audit_before);  // the attempt is audited

    SystemEvent authorized = bare;
    authorized.evidence_provided = true;
    authorized.dual_control_signed = true;
    CHECK(L.submit(authorized).ok());
    CHECK(!L.state(id).quarantined);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
    assert_consistent(L, id);
}
