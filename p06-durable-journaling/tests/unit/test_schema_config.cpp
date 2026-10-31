// G18 and G23 — Schema evolution, serialisation compatibility, clock drift
// and configuration.
//
// The two groups share a theme: something outside the business logic
// changes underneath it, and the engine has to notice rather than carry on
// with a plausible-looking result.
#include <fstream>

#include "jrnl/config.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

// --- G18: schema and serialisation ----------------------------------------

SCENARIO("G18-01", "P0", "a database from a newer schema version is refused") {
    tt::TempDir dir("g18-01");
    const std::string path = dir.file("db.sqlite");
    {
        SqliteAtomicStore::Options opt;
        opt.path = path;
        std::unique_ptr<SqliteAtomicStore> store;
        std::string why;
        CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::None);
        store->db().exec("UPDATE schema_meta SET value='7' WHERE key='schema_version';");
    }
    SqliteAtomicStore::Options opt;
    opt.path = path;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::SchemaMismatch);
}

SCENARIO("G18-02", "P0", "an event records the reducer that produced it") {
    Lab l("g18-02");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 0, &page);
    CHECK_EQ(page.size(), std::size_t(1));
    CHECK_EQ(page[0].reducer_version, std::string(reducer_version::kOrderV1));
    CHECK_EQ(page[0].schema_version, SchemaVersion(1));
    CHECK(page[0].policy_version > 0);
}

SCENARIO("G18-03", "P0", "a retired reducer must stay available for replay") {
    // JI-37. Both versions are registered; replay of a v1 stream uses v1
    // even though v2 exists and is newer.
    Lab l("g18-03");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"));

    CHECK(l.store().reducers().order(reducer_version::kOrderV1) != nullptr);
    CHECK(l.store().reducers().order(reducer_version::kOrderV2) != nullptr);
    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);

    // Replaying the same events under v2 would produce a different state,
    // which is exactly why the version is pinned rather than inferred.
    const IOrderReducer* v2 = l.store().reducers().order(reducer_version::kOrderV2);
    OrderState under_v2;
    ReduceError e;
    CHECK(v2->apply(&under_v2, ev::kOrderAccepted,
                    Payload()
                        .s("chain", "O1")
                        .s("account", Lab::kAccount)
                        .s("cl_ord_id", "K1-cl")
                        .s("symbol", "EURUSD")
                        .s("side", "BUY")
                        .s("command_id", "K1")
                        .s("risk_domain", Lab::kDomain)
                        .i("order_qty", 100000)
                        .i("limit_px", 10000)
                        .build(),
                    &e));
    CHECK_NE(canonical_encode(v2->to_canon(under_v2)), canonical_encode(r.state));
}

SCENARIO("G18-04", "P0", "an unknown reducer version is not silently upgraded") {
    Lab l("g18-04");
    l.start();
    CHECK(l.store().reducers().order("p03.v999") == nullptr);

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-unknown-reducer";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent ev;
    ev.stream = order_stream("O-x");
    ev.expected_version = 0;
    ev.event_id = "e1";
    ev.event_type = std::string(jrnl::ev::kOrderAccepted);
    ev.reducer_version = "p03.v999";
    ev.ordinal = 0;
    ev.payload = Payload()
                     .s("chain", "O-x")
                     .s("account", Lab::kAccount)
                     .s("cl_ord_id", "c")
                     .s("symbol", "EURUSD")
                     .s("side", "BUY")
                     .s("command_id", "KX")
                     .s("risk_domain", Lab::kDomain)
                     .i("order_qty", 10)
                     .i("limit_px", 1)
                     .build();
    p.events.push_back(std::move(ev));

    CHECK_EQ(l.store().apply(p).kind, CommitKind::Rejected);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
}

SCENARIO("G18-05", "P0", "a payload missing a required field is refused before commit") {
    Lab l("g18-05");
    l.start();
    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-bad-payload";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O-y");
    e.expected_version = 0;
    e.event_id = "e-bad";
    e.event_type = std::string(jrnl::ev::kOrderAccepted);
    e.reducer_version = std::string(reducer_version::kOrderV1);
    e.ordinal = 0;
    e.payload = Payload().s("chain", "O-y").build();  // everything else absent
    p.events.push_back(std::move(e));

    const CommitOutcome o = l.store().apply(p);
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
    expect_consistent(l, "G18-05");
}

SCENARIO("G18-06", "P0", "a stored payload round-trips through the canonical codec") {
    Lab l("g18-06");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 0, &page);

    CanonValue v;
    std::string why;
    CHECK_MSG(canonical_decode(page[0].payload_text, &v, &why), why);
    CHECK_EQ(canonical_encode(v), page[0].payload_text);
    std::int64_t qty = 0;
    CHECK(v.get_int("order_qty", &qty));
    CHECK_EQ(qty, std::int64_t(100000));
}

// --- G23: clock, configuration and lifecycle -------------------------------

SCENARIO("G23-01", "P0", "the shipped configuration validates") {
    RecoveryConfig c;
    std::string why;
    CHECK_MSG(load_config(std::string(JRNL_CONFIG_DIR) + "/recovery.yaml", &c, &why), why);
    const std::vector<ConfigProblem> problems = validate_config(c);
    std::string joined;
    for (const ConfigProblem& p : problems) joined += p.key + " ";
    CHECK_MSG(problems.empty(), joined);
    CHECK(!c.recovery.dispatch_default_enabled);
    CHECK_EQ(c.storage.require_synchronous_commit, std::string("on"));
}

SCENARIO("G23-02", "P0", "every unsafe setting is reported, not warned about") {
    RecoveryConfig c;
    c.storage.require_fsync = false;
    c.storage.require_full_page_writes = false;
    c.storage.require_synchronous_commit = "off";
    c.recovery.dispatch_default_enabled = true;
    c.recovery.verify_journal_hashes = false;
    c.recovery.verify_projections = false;
    c.recovery.on_unknown_lp_outcome = "assume_filled";
    c.recovery.on_data_mismatch = "repair";
    c.outbox.safe_retry_requires_p01_stable_send = false;
    c.outbox.default_ambiguous_action = "resend";
    c.backup.require_restore_drill = false;
    c.backup.block_unsafe_wal_pruning = false;

    const std::vector<ConfigProblem> problems = validate_config(c);
    CHECK(problems.size() >= std::size_t(12));
    for (const ConfigProblem& p : problems) {
        CHECK(!p.key.empty());
        CHECK(!p.why.empty());
    }
}

SCENARIO("G23-03", "P0", "a typo in a configuration key is an error, not a default") {
    RecoveryConfig c;
    std::string why;
    CHECK(!parse_config("storage:\n  requrie_fsync: true\n", &c, &why));
    CHECK(why.find("unknown storage key") != std::string::npos);
    CHECK(!parse_config("storag:\n  require_fsync: true\n", &c, &why));
    CHECK(!parse_config("storage:\n  require_fsync: maybe\n", &c, &why));
    CHECK(!parse_config("  require_fsync: true\n", &c, &why));
}

SCENARIO("G23-04", "P0", "comments and quoting parse the way the doc writes them") {
    RecoveryConfig c;
    std::string why;
    const char* text =
        "storage:\n"
        "  require_synchronous_commit: \"on\"   # pinned, see section 17.1\n"
        "  statement_timeout_ms: 2000  # lab only\n"
        "recovery:\n"
        "  dispatch_default: disabled\n";
    CHECK_MSG(parse_config(text, &c, &why), why);
    CHECK_EQ(c.storage.require_synchronous_commit, std::string("on"));
    CHECK_EQ(c.storage.statement_timeout_ms, 2000);
    CHECK(!c.recovery.dispatch_default_enabled);
}

SCENARIO("G23-05", "P0", "a clock that steps backwards does not extend a lease") {
    Lab l("g23-05");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    std::vector<OutboxRow> claimed;
    l.store().claim_outbox(Lab::kTenant, "owner", l.token(), 10, 1000, &claimed);
    CHECK_EQ(claimed.size(), std::size_t(1));

    // The wall clock jumps back an hour. The claim was taken against a
    // wall-clock expiry, so the test asserts what actually matters: no
    // extra claim appears and nothing is dispatched twice.
    l.clock().set_wall(l.clock().now_ms() - 3600000);
    std::vector<OutboxRow> again;
    l.store().claim_outbox(Lab::kTenant, "owner-2", l.token(), 10, 1000, &again);
    CHECK_EQ(again.size(), std::size_t(0));
    expect_consistent(l, "G23-05");
}

SCENARIO("G23-06", "P0", "ordering is unaffected by a clock jump in either direction") {
    Lab l("g23-06");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    l.clock().set_wall(l.clock().now_ms() - 86400000);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    l.clock().advance(172800000);
    l.deliver(l.fill("O1", "E2", 10000, 10000, "K1"));

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(20000));
    expect_consistent(l, "G23-06");
}

SCENARIO("G23-07", "P0", "the monotonic clock is what timing decisions use") {
    ManualClock c(1000000);
    const std::int64_t steady = c.steady_ms();
    c.set_wall(0);  // wall clock yanked backwards
    CHECK_EQ(c.steady_ms(), steady);
    c.advance_steady(500);
    CHECK_EQ(c.steady_ms(), steady + 500);
    CHECK_EQ(c.now_ms(), std::int64_t(0));
}

SCENARIO("G23-08", "P1", "timestamps render as ISO-8601 UTC for audit") {
    ManualClock c(1767225600000);  // 2026-01-01T00:00:00Z
    const std::string s = c.now_iso8601();
    CHECK_EQ(s, std::string("2026-01-01T00:00:00.000Z"));
}

SCENARIO("G23-09", "P0", "a fresh instance starts with dispatch disabled") {
    // JI-24 as a lifecycle property: before verify_and_replay has run, the
    // gate says no to everything.
    Lab l("g23-09");
    CHECK_EQ(l.supervisor().phase(), Phase::Stopped);
    CHECK(!l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));
}

SCENARIO_BLOCKED("G23-10", "P1", "two hosts whose clocks differ by more than the lease",
                 "a second host is needed to produce the skew. The engine-side consequence, "
                 "that safety never depends on the wall clock, is covered by G23-05 to "
                 "G23-07.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_schema_config"); }
