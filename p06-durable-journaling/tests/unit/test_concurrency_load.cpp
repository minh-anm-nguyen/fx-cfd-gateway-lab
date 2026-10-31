// G13 and G20 — Isolation, retries, bounded resources and backpressure.
//
// SQLite serialises writers at BEGIN IMMEDIATE, so the PostgreSQL
// serialization-failure path cannot be reproduced here and the scenarios
// that need it are BLOCKED. What is testable, and is tested, is the engine
// side: a contended write fails cleanly rather than half-applying, retries
// are bounded and carry the same key, and overload refuses admission
// instead of dropping work (JI-36).
#include <atomic>
#include <thread>
#include <vector>

#include "lab.hpp"

using namespace lab;

namespace {

SCENARIO("G13-01", "P0", "writers racing one stream produce one winner and no gap") {
    Lab l("g13-01");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    int committed = 0, conflicted = 0;
    for (int i = 0; i < 6; ++i) {
        TxPlan p;
        p.tenant = Lab::kTenant;
        p.tx_id = "tx-race-" + std::to_string(i);
        p.fence_domain = Lab::kDomain;
        p.fence_token = l.token();
        AppendEvent e;
        e.stream = s;
        e.expected_version = base;  // every one of them believes it is next
        e.event_id = "e-race-" + std::to_string(i);
        e.event_type = std::string(ev::kRiskReserved);
        e.reducer_version = std::string(reducer_version::kRiskV1);
        e.ordinal = 0;
        e.payload = Payload()
                        .s("reservation_id", "r" + std::to_string(i))
                        .s("account", Lab::kAccount)
                        .s("chain", "O" + std::to_string(i))
                        .i("amount", 100)
                        .build();
        p.events.push_back(std::move(e));
        const CommitOutcome o = l.store().apply(p);
        if (o.kind == CommitKind::Committed) ++committed;
        else if (o.kind == CommitKind::Conflict) ++conflicted;
    }
    CHECK_EQ(committed, 1);
    CHECK_EQ(conflicted, 5);
    CHECK_EQ(l.store().head(Lab::kTenant, s).version, base + 1);
    expect_consistent(l, "G13-01");
}

SCENARIO("G13-02", "P0", "concurrent threads never produce two orders for one key") {
    Lab l("g13-02");
    l.start();
    const auto req = l.new_order("K1", "O1", 1000);

    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            const CommitOutcome o = l.service().submit_new(req, l.token());
            if (o.kind == CommitKind::Committed) committed.fetch_add(1);
        });
    }
    for (std::thread& t : threads) t.join();

    // At most one logical command, however the threads interleaved (JI-05).
    CHECK(committed.load() <= 1);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    CHECK_EQ(l.row_count("command_intents"), std::size_t(1));
    expect_consistent(l, "G13-02");
}

SCENARIO("G13-03", "P0", "concurrent distinct orders all land exactly once") {
    Lab l("g13-03", 1000000);
    l.start();
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&, i] {
            const std::string k = "K" + std::to_string(i);
            const std::string c = "O" + std::to_string(i);
            if (l.service().submit_new(l.new_order(k, c, 1000), l.token()).kind ==
                CommitKind::Committed) {
                ok.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();

    CHECK_EQ(ok.load(), 6);
    CHECK_EQ(l.row_count("orders"), std::size_t(6));
    CHECK_EQ(l.risk().reserved, std::int64_t(6000));
    expect_consistent(l, "G13-03");
}

SCENARIO("G13-04", "P0", "a contended domain never exceeds its ceiling") {
    // The ceiling is the thing a race must not be able to breach. Eight
    // threads each ask for a fifth of the limit; at most five can fit.
    Lab l("g13-04", /*limit=*/5000);
    l.start();
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            const std::string k = "K" + std::to_string(i);
            const std::string c = "O" + std::to_string(i);
            if (l.service().submit_new(l.new_order(k, c, 1000), l.token()).kind ==
                CommitKind::Committed) {
                ok.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();

    CHECK(ok.load() <= 5);
    CHECK(l.risk().utilised() <= std::int64_t(5000));
    CHECK_EQ(l.risk().reserved, static_cast<std::int64_t>(ok.load()) * 1000);
    expect_consistent(l, "G13-04");
}

SCENARIO("G13-05", "P0", "a contended inbound path applies each execution once") {
    Lab l("g13-05");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    const auto r = l.fill("O1", "E1", 40000, 10000, "K1");

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] { l.service().apply_report(r, l.token()); });
    }
    for (std::thread& t : threads) t.join();

    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "G13-05");
}

SCENARIO("G13-06", "P0", "a retryable error is classified apart from an ambiguous one") {
    CHECK(is_retryable(StoreError::Serialization));
    CHECK(is_retryable(StoreError::Deadlock));
    CHECK(is_retryable(StoreError::Busy));
    // Section 9.3's central distinction.
    CHECK(!is_retryable(StoreError::CommitUnknown));
    CHECK(is_ambiguous(StoreError::CommitUnknown));
    CHECK(!is_retryable(StoreError::Corrupt));
    CHECK(is_fail_closed(StoreError::Corrupt));
    CHECK(is_fail_closed(StoreError::Full));
    CHECK(is_fail_closed(StoreError::IoError));
    CHECK(is_fail_closed(StoreError::SchemaMismatch));
}

// --- G20: bounded resources -------------------------------------------------

SCENARIO("G20-01", "P0", "a transaction larger than the bound is refused") {
    Lab l("g20-01");
    l.start();
    SqliteAtomicStore::Options opt;
    opt.path = l.dir().file("bounded.db");
    opt.clock = &l.clock();
    opt.max_events_per_tx = 2;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::None);

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-huge";
    for (int i = 0; i < 5; ++i) {
        AppendEvent e;
        e.stream = order_stream("O" + std::to_string(i));
        e.event_id = "e" + std::to_string(i);
        e.event_type = std::string(ev::kOrderAccepted);
        e.reducer_version = std::string(reducer_version::kOrderV1);
        e.ordinal = i;
        e.payload = Payload().s("chain", "O").build();
        p.events.push_back(std::move(e));
    }
    const CommitOutcome o = store->apply(p);
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK(o.detail.find("max_events_per_tx") != std::string::npos);
}

SCENARIO("G20-02", "P0", "the dispatcher takes a bounded slice of the backlog") {
    Lab l("g20-02", 1000000);
    l.start();
    for (int i = 0; i < 25; ++i) {
        l.service().submit_new(
            l.new_order("K" + std::to_string(i), "O" + std::to_string(i), 1000), l.token());
    }
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(25));

    OutboxDispatcher::Options opt;
    opt.owner = "bounded";
    opt.claim_limit = 5;
    OutboxDispatcher d(l.store(), l.link(), opt);
    CHECK_EQ(d.dispatch_once(Lab::kTenant, l.token()).claimed, std::size_t(5));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(20));
    expect_consistent(l, "G20-02");
}

SCENARIO("G20-03", "P0", "a backlog is never silently dropped") {
    // JI-36: overload produces backpressure, not loss.
    Lab l("g20-03", 1000000);
    l.start();
    for (int i = 0; i < 12; ++i) {
        l.service().submit_new(
            l.new_order("K" + std::to_string(i), "O" + std::to_string(i), 1000), l.token());
    }
    OutboxDispatcher::Options opt;
    opt.owner = "bounded";
    opt.claim_limit = 4;
    OutboxDispatcher d(l.store(), l.link(), opt);

    std::size_t sent = 0;
    for (int pass = 0; pass < 3; ++pass) sent += d.dispatch_once(Lab::kTenant, l.token()).bound;
    CHECK_EQ(sent, std::size_t(12));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(0));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "BOUND").size(), std::size_t(12));
    CHECK_EQ(l.lp().distinct_sequences(Lab::kSession), std::size_t(12));
    expect_consistent(l, "G20-03");
}

SCENARIO("G20-04", "P0", "admission stops at the ceiling rather than queueing past it") {
    Lab l("g20-04", /*limit=*/3000);
    l.start();
    int admitted = 0;
    for (int i = 0; i < 10; ++i) {
        if (l.service()
                .submit_new(l.new_order("K" + std::to_string(i), "O" + std::to_string(i), 1000),
                            l.token())
                .kind == CommitKind::Committed) {
            ++admitted;
        }
    }
    CHECK_EQ(admitted, 3);
    CHECK_EQ(l.risk().headroom(), std::int64_t(0));
    CHECK(l.metrics().get(metric::kAdmissionBlocked) >= 7);
    expect_consistent(l, "G20-04");
}

SCENARIO("G20-05", "P1", "a larger history still replays in bounded memory") {
    Lab l("g20-05", 1000000);
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 400000), l.token());
    for (int i = 0; i < 200; ++i) {
        const CommitOutcome o =
            l.deliver(l.fill("O1", "E" + std::to_string(i), 2000, 10000, "K1"));
        CHECK_EQ(o.kind, CommitKind::Committed);
    }
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(400000));
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
    CHECK_EQ(r.events_applied, std::size_t(201));
    expect_consistent(l, "G20-05");
}

SCENARIO("G20-06", "P1", "reading a stream can be paged") {
    Lab l("g20-06");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    for (int i = 0; i < 10; ++i) {
        l.deliver(l.fill("O1", "E" + std::to_string(i), 1000, 10000, "K1"));
    }
    std::vector<JournalRecord> page;
    CHECK_EQ(l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 4, &page),
             StoreError::None);
    CHECK_EQ(page.size(), std::size_t(4));
    CHECK_EQ(page.back().version, Version(4));
    CHECK_EQ(l.store().read_stream(Lab::kTenant, order_stream("O1"), 5, 4, &page),
             StoreError::None);
    CHECK_EQ(page.front().version, Version(5));
}

SCENARIO_BLOCKED("G13-07", "P0", "a SERIALIZABLE abort with SQLSTATE 40001",
                 "SQLite takes the write lock at BEGIN IMMEDIATE, so a serialization failure "
                 "in the PostgreSQL sense never occurs. The classification and the "
                 "same-key retry rule are covered by G13-06 and G05-12.")

SCENARIO_BLOCKED("G13-08", "P0", "a deadlock cycle between the risk and order lock orders",
                 "one writer at a time makes a cycle impossible here. The fixed lock order "
                 "section 5.2 prescribes is a property of the PostgreSQL deployment.")

SCENARIO_BLOCKED("G20-07", "P1", "WAL growth and checkpoint storms under sustained load",
                 "this measures PostgreSQL's checkpointer. A number produced against SQLite "
                 "would describe a different system, and section 19 warns against publishing "
                 "an SLO that was not measured on the target.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_concurrency_load"); }
