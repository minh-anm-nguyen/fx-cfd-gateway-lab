// Group L -- durability, crash windows and replay recovery (design doc 18.L).
//
// Scope warning, stated once and meant: this suite runs against MemoryStore,
// whose durability_mode() is IN_MEMORY. It proves that state, dedup keys, the
// journal and the outbox pass through one atomic boundary and that replay is
// deterministic. It does NOT prove the system is crash safe -- that claim
// needs P06 with a real PostgreSQL transaction, WAL and fsync behind it
// (design doc 13, ADR-08). Release gate G3 must not be read as passed from
// these results.
//
// Every case asserts the faultpoint actually fired. A crash window that did
// not trigger is not a pass.
#include "../support/lab.hpp"

using namespace lab;

namespace {

struct CrashOutcome {
    bool crashed = false;
    bool ambiguous = false;
    CrashWindow window = CrashWindow::None;
};

// Submits an event with a window armed and reports what happened, so each case
// can assert the fault really fired.
template <typename Event>
CrashOutcome crash_at(Lab& L, CrashWindow w, const Event& e) {
    CrashOutcome out;
    L.store().failpoint().arm(w);
    try {
        L.submit(e);
    } catch (const SimulatedCrash& c) {
        out.crashed = true;
        out.window = c.window;
    } catch (const AmbiguousCommit&) {
        out.ambiguous = true;
    }
    CHECK_MSG(L.store().failpoint().fired(),
              std::string("faultpoint ") + to_string(w) + " never fired");
    L.store().failpoint().disarm();
    return out;
}

// A working order plus one fill, as the starting point for most windows.
OrderId seeded(Lab& L) { return L.part_filled("O1", "100000", "40000"); }

}  // namespace

SCENARIO("L-01", "P0", "W00 before parse: the inbox redelivers and nothing is lost") {
    Lab L;
    const OrderId id = seeded(L);
    const uint64_t before = L.state(id).state_hash();

    const CrashOutcome c =
        crash_at(L, CrashWindow::W00_BeforeParse,
                 trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    CHECK_EQ(c.window, CrashWindow::W00_BeforeParse);

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).state_hash(), before);  // nothing was applied
    CHECK(L.store().unconsumed_inbox() > 0);     // and nothing was lost
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("L-02", "P0", "W01 after parse: no phantom chain, replay rebuilds the same state") {
    Lab L;
    const CrashOutcome c = crash_at(L, CrashWindow::W01_AfterParseBeforeCorrelation,
                                    cmd_new("O1", "100000"));
    CHECK(c.crashed);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.engine().chain_count(), size_t{0});  // no phantom
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("L-03", "P0", "W02 after correlation: no alias leak, replay is deterministic") {
    Lab L;
    const OrderId id = seeded(L);
    const CrashOutcome c = crash_at(L, CrashWindow::W02_AfterCorrelationBeforeDedup,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).order_id_aliases.size(), size_t{1});
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("L-04", "P0", "W03 after dedup lookup: the event is processed exactly once") {
    Lab L;
    const OrderId id = seeded(L);
    const CrashOutcome c = crash_at(L, CrashWindow::W03_AfterDedupBeforeReduce,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{2});
}

SCENARIO("L-05", "P0", "W04 after the in-memory reduce: the volatile state is discarded") {
    Lab L;
    const OrderId id = seeded(L);
    const size_t journal_before = L.store().journal().size();
    const CrashOutcome c = crash_at(L, CrashWindow::W04_AfterReduceBeforeTxn,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    // Nothing reached the journal.
    CHECK_EQ(L.store().journal().size(), journal_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("L-06", "P0", "W05 inside the transaction: the whole transaction rolls back") {
    Lab L;
    const OrderId id = seeded(L);
    const size_t journal_before = L.store().journal().size();
    const size_t keys_before = L.store().applied_key_count();

    const CrashOutcome c = crash_at(L, CrashWindow::W05_InTxnBeforeJournalInsert,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    CHECK_EQ(L.store().journal().size(), journal_before);
    CHECK_EQ(L.store().applied_key_count(), keys_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    // The retry is safe.
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("L-07", "P0", "W06 after the dedup insert: key and state commit or roll back together") {
    Lab L;
    const OrderId id = seeded(L);
    const size_t keys_before = L.store().applied_key_count();
    const uint64_t revision_before = L.state(id).revision;

    const CrashOutcome c = crash_at(L, CrashWindow::W06_AfterDedupInsertBeforeState,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);
    // Neither half survived on its own.
    CHECK_EQ(L.store().applied_key_count(), keys_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).revision, revision_before);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    // And the event was not consumed, so it still applies once.
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("L-08", "P0", "W07 after the state update: there is no state without its intent") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const size_t outbox_before = L.store().outbox().size();
    const uint64_t revision_before = L.state(id).revision;

    const CrashOutcome c = crash_at(L, CrashWindow::W07_AfterStateBeforeOutbox,
                                    cmd_cancel("C1", "O1"));
    CHECK(c.crashed);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).revision, revision_before);
    CHECK(!L.state(id).pending.has_value());  // no half-committed pending cancel
    CHECK_EQ(L.store().outbox().size(), outbox_before);
}

SCENARIO("L-09", "P0", "W08 after the outbox insert: no phantom outbound effect") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const size_t outbox_before = L.store().outbox().size();

    const CrashOutcome c = crash_at(L, CrashWindow::W08_AfterOutboxBeforeCommit,
                                    cmd_cancel("C1", "O1"));
    CHECK(c.crashed);
    // The dispatcher can see nothing: the rows never left the staging area.
    CHECK_EQ(L.store().outbox().size(), outbox_before);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{0});
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK(!L.state(id).pending.has_value());
}

SCENARIO("L-10", "P0", "W09 after commit before the inbox ack: redelivery is a no-op") {
    Lab L;
    const OrderId id = seeded(L);
    const CrashOutcome c = crash_at(L, CrashWindow::W09_AfterCommitBeforeInboxAck,
                                    trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(c.crashed);

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    // The commit survived.
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});

    // The unacked row is redelivered and recognised as already applied.
    EngineResult again;
    CHECK(L.engine().pump(&again));
    CHECK_EQ(again.decision, Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{2});
}

SCENARIO("L-11", "P0", "W10 after commit before dispatch: the effect is not lost") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const CrashOutcome c =
        crash_at(L, CrashWindow::W10_AfterCommitBeforeDispatch, cmd_cancel("C1", "O1"));
    CHECK(c.crashed);
    const std::string key = "route-cancel|FIRM|2026-10-30|C1";
    CHECK_EQ(L.engine().sink().count_key(key), size_t{0});  // never went out

    // The commit stands and the pending row is still waiting.
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK(L.state(id).pending.has_value());
    CHECK_EQ(L.state(id).pending->kind, CommandKind::Cancel);
    // Recovery dispatched it with its original identity.
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});
}

SCENARIO("L-12", "P0", "W11 after the send before the marker: the port issues once") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const CrashOutcome c = crash_at(L, CrashWindow::W11_AfterDispatchBeforeMarkSent,
                                    cmd_cancel("C1", "O1"));
    CHECK(c.crashed);
    const std::string key = "route-cancel|FIRM|2026-10-30|C1";
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});
    const size_t real_sends = L.engine().sink().dispatched().size();

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    // Offered again under the same identity, suppressed by the port: no second
    // cancel request and certainly no new-ID order.
    CHECK_EQ(L.engine().sink().count_key(key), size_t{2});
    CHECK_EQ(L.engine().sink().dispatched().size(), real_sends);
    CHECK(L.state(id).pending.has_value());
}

SCENARIO("L-13", "P0", "W12 an ambiguous flush publishes nothing and blocks trading") {
    Lab L;
    const OrderId id = seeded(L);
    const uint64_t before = L.state(id).state_hash();
    const uint64_t revision_before = L.state(id).revision;

    L.store().failpoint().arm(CrashWindow::W12_AmbiguousFlush);
    const EngineResult r =
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    // Neither success nor failure was claimed.
    CHECK_EQ(r.decision, Decision::RetryCommit);
    CHECK_EQ(r.reason, Reason::UnknownOutcome);
    CHECK(!r.committed);
    CHECK_EQ(r.commit_status, CommitStatus::Ambiguous);
    CHECK_EQ(L.state(id).state_hash(), before);
    CHECK_EQ(L.state(id).revision, revision_before);
    CHECK(!L.engine().trading_allowed());
    CHECK_EQ(L.engine().metrics().ambiguous_commits, uint64_t{1});
}

SCENARIO("L-14", "P0", "a storage failure fails closed with no routing side effect") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t before = L.state(id).state_hash();
    const size_t effects_before = L.engine().sink().dispatched().size();

    L.store().set_fail_next_commit(true, "WAL disk full");
    const EngineResult r = L.submit(cmd_cancel("C1", "O1"));
    CHECK_EQ(r.decision, Decision::RetryCommit);
    CHECK(!r.committed);
    CHECK_EQ(r.commit_status, CommitStatus::Failed);
    CHECK(r.detail.find("disk full") != std::string::npos);

    CHECK_EQ(L.state(id).state_hash(), before);
    CHECK_EQ(L.engine().sink().dispatched().size(), effects_before);
    CHECK_EQ(L.engine().sink().count_key("route-cancel|FIRM|2026-10-30|C1"), size_t{0});
    CHECK_EQ(L.engine().metrics().commit_failures, uint64_t{1});
}

SCENARIO("L-15", "P0", "a corrupt journal record halts startup instead of skipping a fill") {
    Lab L;
    const OrderId id = seeded(L);
    CHECK(L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000")).ok());
    CHECK(L.store().journal().size() > 3);

    // Tamper with a record's recorded state hash.
    CHECK(L.store().corrupt_journal_hash(2));
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::HashDivergence);
    CHECK(!rep.trading_allowed);
    CHECK(!L.engine().trading_allowed());
    CHECK(rep.first_divergent_event != 0);
    CHECK(rep.detail.find("state hash") != std::string::npos);
    (void)id;
}

SCENARIO("L-16", "P0", "a snapshot behind the log replays the remainder to the same hash") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (int i = 0; i < 5; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .lastq("10000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 10000)))
                  .ok());
    }
    const uint64_t full_hash = L.state(id).state_hash();
    const int64_t full_cum = L.state(id).cum_qty_units;

    // Checkpoint after the first few records, leaving the rest in the log.
    std::map<uint64_t, AggregateRow> live;
    const size_t watermark = 3;
    {
        // Rebuild the state as of the watermark by replaying that prefix.
        Lab prefix;
        (void)prefix;
    }
    // Simpler and equivalent: snapshot the current state at the current
    // watermark, then add more events and recover.
    live[id.value] = AggregateRow{L.state(id).revision, L.state(id)};
    L.store().write_snapshot(live, L.store().journal().size(), L.policy().policy_version);
    CHECK(L.store().snapshot().has_value());
    CHECK(L.store().snapshot()->complete);

    CHECK(L.submit(trade("X9", "O1").lastq("50000").lastpx("1.08495").cum("100000")).ok());
    const uint64_t after_hash = L.state(id).state_hash();

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::Clean);
    CHECK_EQ(rep.records_replayed, size_t{1});  // only the record past the watermark
    CHECK_EQ(L.state(id).state_hash(), after_hash);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{100000});
    CHECK(full_hash != after_hash);
    CHECK_EQ(full_cum, int64_t{50000});
    (void)watermark;
}

SCENARIO("L-17", "P0", "a snapshot ahead of the journal watermark is rejected") {
    Lab L;
    const OrderId id = seeded(L);
    const uint64_t good_hash = L.state(id).state_hash();

    Snapshot bad;
    bad.complete = true;
    bad.schema_version = L.policy().event_schema_version;
    bad.journal_watermark = L.store().journal().size() + 50;  // impossible
    bad.aggregates[id.value] = L.state(id);
    L.store().install_snapshot(bad);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SnapshotRejected);
    CHECK(rep.trading_allowed);  // recovered from a valid base instead
    CHECK_EQ(L.state(id).state_hash(), good_hash);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("L-18", "P0", "a snapshot interrupted mid-write is never adopted") {
    Lab L;
    const OrderId id = seeded(L);
    const uint64_t good_hash = L.state(id).state_hash();

    std::map<uint64_t, AggregateRow> live;
    live[id.value] = AggregateRow{L.state(id).revision, L.state(id)};
    L.store().failpoint().arm(CrashWindow::W13_DuringSnapshotCompaction);
    bool crashed = false;
    try {
        L.store().write_snapshot(live, L.store().journal().size(), 1);
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    // A half-written snapshot exists and is recognisable as unusable.
    CHECK(L.store().snapshot().has_value());
    CHECK(!L.store().snapshot()->complete);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SnapshotRejected);
    CHECK_EQ(L.state(id).state_hash(), good_hash);
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});

    // Rebuilding it again from the same base produces an identical snapshot.
    L.store().clear_snapshot();
    L.store().write_snapshot(live, L.store().journal().size(), 1);
    const uint64_t digest_one = L.store().snapshot()->digest;
    L.store().clear_snapshot();
    L.store().write_snapshot(live, L.store().journal().size(), 1);
    CHECK_EQ(L.store().snapshot()->digest, digest_one);
}

SCENARIO("L-19", "P0", "a journal on an unknown schema version halts startup") {
    Lab L;
    const OrderId id = seeded(L);
    L.store().set_schema_version(99);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SchemaMismatch);
    CHECK(!rep.trading_allowed);
    CHECK(!L.engine().trading_allowed());
    CHECK_EQ(rep.chains_recovered, size_t{0});  // nothing was reinterpreted
    (void)id;

    // Back on a known version it recovers cleanly.
    L.store().set_schema_version(1);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("L-20", "P0", "one hundred restarts over the same journal give one canonical hash") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "R1").lastq("60000").lastpx("1.08495").cum("100000")).ok());
    CHECK(L.submit(cmd_cancel("C1", "R1")).ok());
    CHECK(L.submit(canceled("C1").cum("100000")).ok());

    const uint64_t expected = L.state(id).state_hash();
    const int64_t expected_cum = L.state(id).cum_qty_units;
    const size_t expected_fills = L.state(id).ledger.size();
    const size_t expected_versions = L.state(id).accepted_versions.size();

    for (int restart = 0; restart < 100; ++restart) {
        const RecoveryReport rep = L.engine().recover();
        CHECK_MSG(rep.status == RecoveryStatus::Clean,
                  "restart " + std::to_string(restart) + ": " + rep.detail);
        const OrderAggregate& a = L.state(id);
        CHECK_MSG(a.state_hash() == expected, "hash drift at restart " + std::to_string(restart));
        CHECK_EQ(a.cum_qty_units, expected_cum);
        CHECK_EQ(a.ledger.size(), expected_fills);
        CHECK_EQ(a.accepted_versions.size(), expected_versions);
        CHECK(check_invariants(a, L.policy()).empty());
    }

    // The independent journal oracle agrees with the recovered ledger.
    CHECK_EQ(oracle(L.store(), id).cum, expected_cum);
    // And no duplicate effect was issued across a hundred recoveries.
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O1"), size_t{1});
}
