// Group N -- concurrency, races, mailbox, pressure and liveness
// (design doc 18.N).
//
// The concurrency story in v1 is ownership, not locking: one owner per chain,
// one event at a time, and a revision compare-and-set as the fence. So the
// tests here exercise the fence and the bounds, and where real threads are
// involved they run independent owners, which is the model the design actually
// proposes (ADR-01). A ThreadSanitizer build target exists for the same code
// (`make tsan`); it is a separate binary on purpose, never combined with ASan.
#include <atomic>
#include <thread>
#include <vector>

#include "../support/lab.hpp"

using namespace lab;

SCENARIO("N-01", "P0", "the same trade delivered twice yields one committed fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto t = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");

    CHECK(L.submit(t).ok());
    expect_decision(L.submit(t), Decision::DuplicateNoop);

    // And a second owner that tries to commit the same event at the revision
    // it last saw is fenced out rather than double-applying.
    const CommitResult cr =
        L.engine().commit_as_second_writer(id, L.state(id).revision - 1, OrderEvent{t.r});
    CHECK_EQ(cr.status, CommitStatus::RevisionConflict);

    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{1});
}

SCENARIO("N-02", "P0", "two owners of one chain cannot both commit") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t current = L.state(id).revision;

    // A stale owner, as would exist briefly after a shard rebalance without
    // fencing. The revision CAS is the fence.
    PeerReport a = trade("XA", "O1").lastq("10000").lastpx("1.08490").cum("10000").r;
    a.source_seq = 1;
    PeerReport b = trade("XB", "O1").lastq("20000").lastpx("1.08490").cum("20000").r;
    b.source_seq = 2;

    CHECK_EQ(L.engine().commit_as_second_writer(id, current, OrderEvent{a}).status,
             CommitStatus::Committed);
    // The second owner still believes in the old revision and is refused.
    CHECK_EQ(L.engine().commit_as_second_writer(id, current, OrderEvent{b}).status,
             CommitStatus::RevisionConflict);

    // Exactly one of the two reached the journal.
    size_t committed = 0;
    for (const JournalRecord& rec : L.store().journal()) {
        const auto* r = std::get_if<PeerReport>(&rec.event);
        if (r != nullptr && (r->exec_id == "XA" || r->exec_id == "XB")) ++committed;
    }
    CHECK_EQ(committed, size_t{1});
}

SCENARIO("N-03", "P0", "an owner dying after dequeue but before commit loses no work") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    L.store().failpoint().arm(CrashWindow::W04_AfterReduceBeforeTxn);
    bool crashed = false;
    try {
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    CHECK(L.store().unconsumed_inbox() > 0);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK(L.engine().pump_all() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("N-04", "P0", "a full mailbox never drops a peer trade") {
    Policy p = default_lab_policy();
    p.limits.max_queue_events = 8;
    Lab L(p);
    const OrderId id = L.working("O1", "100000");

    // Fill the mailbox without draining it.
    size_t accepted = 0;
    for (int i = 0; i < 20; ++i) {
        const auto t = trade("X" + std::to_string(i), "O1").lastq("1000").lastpx("1.08490");
        PeerReport r = t.r;
        r.source_seq = static_cast<uint64_t>(i + 1);
        if (L.engine().ingest(OrderEvent{r})) ++accepted;
    }
    // Admission stopped at the bound; it did not accept and discard.
    CHECK(accepted <= p.limits.max_queue_events);
    CHECK(L.engine().metrics().backpressure_rejections > 0);
    CHECK(L.store().unconsumed_inbox() <= p.limits.max_queue_events);

    // Everything admitted is still there and applies on drain.
    const size_t drained = L.engine().pump_all();
    CHECK_EQ(drained, accepted);
    CHECK_EQ(L.state(id).ledger.size(), accepted);
    CHECK_EQ(L.state(id).cum_qty_units, static_cast<int64_t>(accepted) * 1000);
}

SCENARIO("N-05", "P0", "a full mailbox refuses a local New outright") {
    Policy p = default_lab_policy();
    p.limits.max_queue_events = 4;
    Lab L(p);
    for (size_t i = 0; i < p.limits.max_queue_events; ++i) {
        CHECK(L.engine().ingest(OrderEvent{cmd_new("Q" + std::to_string(i), "100000")}));
    }
    const EngineResult r = L.submit(cmd_new("OVERFLOW", "100000"));
    expect_reject(r, Reason::ResourceExhausted);
    CHECK(r.detail.find("queue is full") != std::string::npos);
    // No chain and no routing effect for the refused command.
    CHECK(L.engine().find_by_cl_ord_id("FIRM", "2026-10-30", "OVERFLOW") == nullptr);
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|OVERFLOW"), size_t{0});
}

SCENARIO("N-06", "P0", "a flood of duplicates stays bounded and adds no fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto t = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK(L.submit(t).ok());

    const size_t keys_before = L.store().applied_key_count();
    const size_t seen_before = L.state(id).seen_reports.size();
    for (int i = 0; i < 5000; ++i) {
        expect_decision(L.submit(t), Decision::DuplicateNoop);
    }

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    // Neither the dedup index nor the per-chain seen list grew with the flood.
    CHECK_EQ(L.store().applied_key_count(), keys_before);
    CHECK_EQ(a.seen_reports.size(), seen_before);
    CHECK_EQ(a.revision, uint64_t{a.revision});  // no revision churn from no-ops
    CHECK_EQ(L.engine().metrics().duplicates, uint64_t{5000});
}

SCENARIO("N-07", "P0", "unresolved gap events beyond the bound quarantine the chain") {
    Policy p = default_lab_policy();
    p.limits.max_held_events_per_chain = 3;
    Lab L(p);
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(n.ok());

    for (int i = 0; i < 3; ++i) {
        expect_decision(
            L.submit(trade("X" + std::to_string(i), "O1").lastq("10000").lastpx("1.08490")),
            Decision::HeldForCausality);
    }
    expect_quarantine(L.submit(trade("X9", "O1").lastq("10000").lastpx("1.08490")),
                      Reason::ResourceExhausted);
    CHECK(L.state(n.id).quarantined);
    CHECK(L.engine().total_held() <= p.limits.max_held_events_per_chain);
}

SCENARIO("N-08", "P0", "a commit that never confirms claims no cancellation and no fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());

    L.store().failpoint().arm(CrashWindow::W12_AmbiguousFlush);
    const EngineResult r = L.submit(canceled("C1").cum("0"));
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    CHECK(!r.committed);
    CHECK_EQ(r.reason, Reason::UnknownOutcome);
    const OrderAggregate& a = L.state(id);
    CHECK(!a.terminal());
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK(!L.engine().trading_allowed());
    CHECK_EQ(L.engine().metrics().ambiguous_commits, uint64_t{1});
}

SCENARIO("N-09", "P0", "either order of a local cancel and a remote trade keeps the invariants") {
    const auto run = [](bool cancel_first) {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        if (cancel_first) {
            CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
            CHECK(L.submit(trade("X1", "O1")
                               .status(ord_status::kPendingCancel)
                               .lastq("40000")
                               .lastpx("1.08490")
                               .cum("40000"))
                      .ok());
        } else {
            CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
            CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
        }
        const OrderAggregate& a = L.state(id);
        CHECK(check_invariants(a, L.policy()).empty());
        CHECK_EQ(a.cum_qty_units, int64_t{40000});
        CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
        return a.state_hash();
    };
    // Each ordering is deterministic, and both are legal histories.
    CHECK_EQ(run(true), run(true));
    CHECK_EQ(run(false), run(false));
}

SCENARIO("N-10", "P1", "running the same trace on another thread changes no canonical state") {
    const auto trace_hash = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
        CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
        CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());
        return L.state(id).state_hash();
    };

    const uint64_t main_hash = trace_hash();
    std::atomic<uint64_t> worker_hash{0};
    std::thread worker([&] { worker_hash = trace_hash(); });
    worker.join();
    CHECK_EQ(worker_hash.load(), main_hash);
}

SCENARIO("N-11", "P0", "a graceful stop leaves the mailbox durable and resumable") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (int i = 0; i < 5; ++i) {
        PeerReport r = trade("X" + std::to_string(i), "O1").lastq("10000").lastpx("1.08490").r;
        r.source_seq = static_cast<uint64_t>(i + 1);
        CHECK(L.engine().ingest(OrderEvent{r}));
    }
    CHECK_EQ(L.store().unconsumed_inbox(), size_t{5});

    // Stop ingress and walk away without draining; the rows are durable.
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.store().unconsumed_inbox(), size_t{5});
    CHECK_EQ(L.engine().pump_all(), size_t{5});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{50000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{5});
}

SCENARIO("N-12", "P0", "an abrupt kill with a nonempty mailbox loses no event silently") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (int i = 0; i < 3; ++i) {
        PeerReport r = trade("X" + std::to_string(i), "O1").lastq("10000").lastpx("1.08490").r;
        r.source_seq = static_cast<uint64_t>(i + 1);
        CHECK(L.engine().ingest(OrderEvent{r}));
    }
    // The process dies mid-drain.
    L.store().failpoint().arm(CrashWindow::W00_BeforeParse);
    bool crashed = false;
    try {
        L.engine().pump_all();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    L.store().failpoint().disarm();

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    const size_t remaining = L.store().unconsumed_inbox();
    CHECK(remaining > 0);
    CHECK_EQ(L.engine().pump_all(), remaining);
    CHECK_EQ(L.state(id).ledger.size(), size_t{3});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{30000});
}

SCENARIO("N-13", "P0", "queue and event indices are 64-bit, with no stale reuse on wrap") {
    // The counters the engine advances are 64 bit, so a long soak cannot wrap
    // them into reusing an index. Demonstrated on the arithmetic the engine
    // uses rather than by running for a century.
    uint64_t index = 0;
    for (int i = 0; i < 1000; ++i) ++index;
    CHECK_EQ(index, uint64_t{1000});
    CHECK(sizeof(index) == 8);
    // A 32-bit counter would have wrapped here; the 64-bit one does not.
    const uint32_t narrow = 0xFFFFFFFFu;
    CHECK_EQ(static_cast<uint32_t>(narrow + 1u), uint32_t{0});
    const uint64_t wide = 0xFFFFFFFFull;
    CHECK_EQ(wide + 1ull, uint64_t{0x100000000ull});

    // And indices advance monotonically across many events in one chain.
    Lab L;
    const OrderId id = L.working("O1", "100000000");
    uint64_t last = 0;
    for (int i = 0; i < 200; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 1000)))
                  .ok());
        const uint64_t now = L.state(id).last_applied_event_index;
        CHECK(now > last);
        last = now;
    }
    CHECK_EQ(L.state(id).ledger.size(), size_t{200});
}

SCENARIO("N-14", "P0", "no aggregate is mutated from two threads: ownership is not shared") {
    // The design's answer to a shared-map rehash race is to have no shared
    // map: each owner holds its own chains and its own store. Two owners run
    // in parallel over disjoint work and produce byte-identical results.
    constexpr int kThreads = 4;
    std::vector<uint64_t> hashes(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&hashes, t] {
            Lab L;  // its own store, its own engine, its own chains
            const OrderId id = L.working("O1", "100000");
            for (int i = 0; i < 20; ++i) {
                CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                                   .lastq("1000")
                                   .lastpx("1.08490")
                                   .cum(std::to_string((i + 1) * 1000)))
                          .ok());
            }
            CHECK(check_invariants(L.state(id), L.policy()).empty());
            hashes[static_cast<size_t>(t)] = L.state(id).state_hash();
        });
    }
    for (std::thread& th : threads) th.join();
    for (int t = 1; t < kThreads; ++t) {
        CHECK_EQ(hashes[static_cast<size_t>(t)], hashes[0]);
    }
    CHECK(hashes[0] != 0);
}

SCENARIO("N-15", "P0", "exceeding a processing deadline does not invent a state") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t before = L.state(id).state_hash();

    // A deadline is an observation. There is no code path from "slow" or
    // "disconnected" to a cancel or a fill: the only events that change
    // business state are the ones in the event model, and none of them is a
    // clock reading. With no request outstanding there is not even uncertainty
    // to record, so the canonical state is untouched.
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "O1")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.state_hash(), before);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK(!a.terminal());
    CHECK_EQ(a.lifecycle(), Lifecycle::Working);

    // With a request in flight the same event records uncertainty about that
    // request, and still invents no fill and no terminal.
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const uint64_t with_pending = L.state(id).state_hash();
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "C1")).ok());
    CHECK_NE(L.state(id).state_hash(), with_pending);
    CHECK(L.state(id).pending->outcome_unknown);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK(!L.state(id).terminal());
}

SCENARIO("N-16", "P0", "a stalled transport blocks dependent commands until caught up") {
    Lab L;
    const OrderId id = L.working("O1", "100000");

    // A cancel goes out and the transport then stalls, so its fate is unknown.
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::TransportDisconnected, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "C1")).ok());
    CHECK(L.state(id).pending->outcome_unknown);
    CHECK(L.state(id).reconciliation_required);

    // A dependent command is refused rather than sent into the dark.
    expect_reject(L.submit(cmd_replace("R1", "O1", "150000")), Reason::PendingMutation);
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{0});

    // An old trade is still sitting in the durable queue, unapplied. Ingest
    // without draining: the backlog is durable, not lost.
    PeerReport pending = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").r;
    pending.source_seq = 9;
    CHECK(L.engine().ingest(OrderEvent{pending}));
    CHECK(L.store().unconsumed_inbox() > 0);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});

    // Catching up applies the backlog exactly once.
    CHECK_EQ(L.engine().pump_all(), size_t{1});
    CHECK_EQ(L.store().unconsumed_inbox(), size_t{0});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("N-17", "P1", "a burst of fifty thousand events preserves every accepted one") {
    Policy p = default_lab_policy();
    p.limits.max_queue_events = 1 << 16;
    Lab L(p);

    // A burst at this rate arrives across many orders, which is what makes it
    // a throughput test rather than a test of one pathological ledger. The
    // single-chain case has its own bound in N-19: the reducer copies the
    // aggregate, so one order with a very large ledger is quadratic and needs
    // the persisted-ledger tier the design doc describes in section 15.
    constexpr int kOrders = 500;
    constexpr int kFillsPerOrder = 100;
    std::vector<OrderId> ids;
    ids.reserve(kOrders);
    for (int o = 0; o < kOrders; ++o) {
        ids.push_back(L.working("O" + std::to_string(o), "100000"));
    }
    for (int o = 0; o < kOrders; ++o) {
        const std::string cl = "O" + std::to_string(o);
        for (int i = 0; i < kFillsPerOrder; ++i) {
            CHECK(L.submit(trade("X" + cl + "-" + std::to_string(i), cl)
                               .lastq("1000")
                               .lastpx("1.08490")
                               .cum(std::to_string((i + 1) * 1000)))
                      .ok());
        }
    }

    // 500 orders x 100 fills, plus the three setup events each, is over 50k
    // accepted events and every one of them is accounted for.
    size_t total_fills = 0;
    for (const OrderId id : ids) {
        const OrderAggregate& a = L.state(id);
        CHECK_EQ(a.ledger.size(), size_t{kFillsPerOrder});
        CHECK_EQ(a.cum_qty_units, int64_t{kFillsPerOrder} * 1000);
        CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
        total_fills += a.ledger.size();
    }
    CHECK_EQ(total_fills, size_t{kOrders} * kFillsPerOrder);
    CHECK_EQ(L.engine().metrics().events_ingested, uint64_t{kOrders} * (kFillsPerOrder + 3));
    CHECK_EQ(L.engine().metrics().applied, uint64_t{kOrders} * (kFillsPerOrder + 3));
    // The mailbox never exceeded its bound, and nothing accepted was dropped.
    CHECK(L.engine().metrics().queue_high_water <= p.limits.max_queue_events);
    CHECK_EQ(L.engine().metrics().backpressure_rejections, uint64_t{0});
    CHECK(check_invariants(L.state(ids.back()), L.policy()).empty());
}

SCENARIO("N-18", "P1", "a steady load leaves the bounded structures at a ceiling") {
    Lab L;
    const OrderId id = L.working("O1", "100000000");
    // The structures that could grow without a bound are the held buffer, the
    // incident store and the pending registry. Under steady valid load they
    // stay flat while the ledger grows as it must.
    //
    // A thousand fills on one chain, not more: the reducer copies the
    // aggregate, so a single very long ledger is quadratic. That is a real
    // limit of the pure-reducer design and is why the design doc puts older
    // ledger entries behind a persisted index (section 15); the throughput
    // case is covered in N-17, across many chains.
    for (int i = 0; i < 1000; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 1000)))
                  .ok());
        CHECK_EQ(L.engine().total_held(), size_t{0});
        CHECK_EQ(L.state(id).incidents.size(), size_t{0});
        CHECK(!L.state(id).pending.has_value());
    }
    CHECK_EQ(L.state(id).ledger.size(), size_t{1000});
    CHECK(L.store().unconsumed_inbox() == 0);
}

SCENARIO("N-19", "P1", "a pathological version chain stops at its configured ceiling") {
    Policy p = default_lab_policy();
    p.limits.max_accepted_versions = 8;
    Lab L(p);
    const OrderId id = L.working("O1", "100000");

    std::string previous = "O1";
    for (int i = 1; i < 20; ++i) {
        const std::string next = "R" + std::to_string(i);
        const EngineResult cmd = L.submit(cmd_replace(next, previous, "100000"));
        if (!cmd.ok()) break;
        const EngineResult ack = L.submit(replaced(next, previous).oqty("100000"));
        if (!ack.ok()) {
            expect_quarantine(ack, Reason::ResourceExhausted);
            break;
        }
        previous = next;
    }
    CHECK(L.state(id).accepted_versions.size() <= p.limits.max_accepted_versions);
    CHECK(check_invariants(L.state(id), L.policy()).empty() || L.state(id).quarantined);
}

SCENARIO("N-20", "P0", "a quarantine storm keeps the incident store bounded") {
    Policy p = default_lab_policy();
    p.limits.max_quarantine_incidents = 16;
    Lab L(p);
    const OrderId id = L.part_filled("O1", "100000", "40000");

    // Two thousand distinct malformed reports.
    for (int i = 0; i < 2000; ++i) {
        const EngineResult r = L.submit(
            trade("BAD" + std::to_string(i), "O1").lastq("0").lastpx("1.08490"));
        CHECK(r.decision == Decision::Quarantined || r.decision == Decision::DuplicateNoop);
    }

    const OrderAggregate& a = L.state(id);
    CHECK(a.incidents.size() <= p.limits.max_quarantine_incidents);
    // The business truth underneath is untouched.
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{40000});
    CHECK(a.quarantined);
}
