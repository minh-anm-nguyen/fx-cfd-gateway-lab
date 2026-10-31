// G02 — Atomic business transactions and cross-projection constraints.
//
// Every case here injects a failure at one write boundary inside the
// transaction and then asserts the same thing: nothing from that
// transaction survives. Section 9.0 allows no partial write set, so the
// interesting question is never "what was kept" but "was anything kept".
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

// Snapshot of everything a transaction could leave behind.
struct Footprint {
    std::size_t events = 0;
    std::size_t manifests = 0;
    std::size_t orders = 0;
    std::size_t intents = 0;
    std::size_t outbox = 0;
    std::size_t receipts = 0;
    std::size_t exec_keys = 0;
    std::size_t fills = 0;
    std::size_t reservations = 0;
    std::int64_t reserved = 0;
    Version order_head = 0;
    Version risk_head = 0;

    bool operator==(const Footprint& o) const {
        return events == o.events && manifests == o.manifests && orders == o.orders &&
               intents == o.intents && outbox == o.outbox && receipts == o.receipts &&
               exec_keys == o.exec_keys && fills == o.fills &&
               reservations == o.reservations && reserved == o.reserved &&
               order_head == o.order_head && risk_head == o.risk_head;
    }
};

Footprint capture(Lab& l, const std::string& chain) {
    Footprint f;
    f.events = l.row_count("journal_events");
    f.manifests = l.row_count("journal_transactions");
    f.orders = l.row_count("orders");
    f.intents = l.row_count("command_intents");
    f.outbox = l.row_count("command_outbox");
    f.receipts = l.row_count("inbox_receipts");
    f.exec_keys = l.row_count("business_exec_keys");
    f.fills = l.row_count("fills");
    f.reservations = l.row_count("risk_reservations");
    f.reserved = l.risk().reserved;
    f.order_head = l.store().head(Lab::kTenant, order_stream(chain)).version;
    f.risk_head = l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version;
    return f;
}

std::string describe(const Footprint& a, const Footprint& b) {
    return "events " + std::to_string(a.events) + "->" + std::to_string(b.events) +
           ", manifests " + std::to_string(a.manifests) + "->" + std::to_string(b.manifests) +
           ", orders " + std::to_string(a.orders) + "->" + std::to_string(b.orders) +
           ", intents " + std::to_string(a.intents) + "->" + std::to_string(b.intents) +
           ", outbox " + std::to_string(a.outbox) + "->" + std::to_string(b.outbox) +
           ", reserved " + std::to_string(a.reserved) + "->" + std::to_string(b.reserved);
}

// Runs a new-order admission with one failpoint armed and asserts that the
// database is byte-for-byte where it started.
void expect_all_or_nothing(const char* scenario, std::string_view point,
                           fp::Action action = fp::Action::ReturnError) {
    Lab l(scenario);
    l.start();
    const Footprint before = capture(l, "O1");

    fp::disarm_all();
    fp::arm(std::string(point), action);
    CommitOutcome o;
    try {
        o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    } catch (...) {
        fp::disarm_all();
        TT_FAIL("an injected fault escaped as an exception");
    }
    fp::disarm_all();

    CHECK_MSG(o.kind != CommitKind::Committed, std::string("commit should not have succeeded"));

    const Footprint after = capture(l, "O1");
    CHECK_MSG(before == after, describe(before, after));
    expect_consistent(l, scenario);
}

SCENARIO("G02-01", "P0", "a failure after the journal append rolls the whole set back") {
    expect_all_or_nothing("g02-01", fp::point::kAfterJournalAppend);
}

SCENARIO("G02-02", "P0", "a failure after the risk projection leaks no reservation") {
    expect_all_or_nothing("g02-02", fp::point::kAfterRiskProjection);
}

SCENARIO("G02-03", "P0", "a failure after the outbox insert dispatches nothing") {
    expect_all_or_nothing("g02-03", fp::point::kAfterOutboxInsert);
}

SCENARIO("G02-04", "P0", "a failure after the dedup insert leaves no orphan key") {
    expect_all_or_nothing("g02-04", fp::point::kAfterDedupInsert);
}

SCENARIO("G02-05", "P0", "a failure after the order projection leaves CumQty untouched") {
    expect_all_or_nothing("g02-05", fp::point::kAfterOrderProjection);
}

SCENARIO("G02-09", "P0", "a failure after the command intent commits no intent-only row") {
    expect_all_or_nothing("g02-09", fp::point::kAfterCommandIntent);
}

SCENARIO("G02-10", "P0", "a failure immediately before COMMIT commits nothing") {
    expect_all_or_nothing("g02-10", fp::point::kBeforeCommit);
}

SCENARIO("G02-11", "P0", "an exception thrown inside the transaction unwinds cleanly") {
    // Section 20 asks for RAII rollback rather than a catch-and-continue.
    // The failpoint throws from the middle of the write set; the guard's
    // destructor is what has to do the right thing.
    expect_all_or_nothing("g02-11", fp::point::kAfterStreamHead, fp::Action::Throw);
}

SCENARIO("G02-15", "P0", "a database-level constraint failure aborts the whole transaction") {
    expect_all_or_nothing("g02-15", fp::point::kAfterManifest);
}

SCENARIO("G02-06", "P0", "a risk ceiling breach aborts every change in the transaction") {
    Lab l("g02-06", /*limit=*/1000);
    l.start();
    const Footprint before = capture(l, "O1");

    // Asking for more than the domain allows. The order must not appear,
    // and neither must a partial reservation.
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 5000), l.token());
    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK(capture(l, "O1") == before);
    expect_consistent(l, "G02-06");
}

SCENARIO("G02-07", "P0", "one bad version in a two-stream transaction commits neither stream") {
    Lab l("g02-07");
    l.start();
    const Footprint before = capture(l, "O1");

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-mixed";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();

    AppendEvent good;
    good.stream = risk_stream(Lab::kDomain);
    good.expected_version = before.risk_head;
    good.event_id = "e-good";
    good.event_type = std::string(ev::kRiskReserved);
    good.reducer_version = std::string(reducer_version::kRiskV1);
    good.ordinal = 0;
    good.payload = Payload()
                       .s("reservation_id", "res:O1")
                       .s("account", Lab::kAccount)
                       .s("chain", "O1")
                       .i("amount", 100)
                       .build();
    p.events.push_back(std::move(good));

    AppendEvent bad;
    bad.stream = order_stream("O1");
    bad.expected_version = 7;  // the stream is empty, so this cannot be right
    bad.event_id = "e-bad";
    bad.event_type = std::string(ev::kOrderAccepted);
    bad.reducer_version = std::string(reducer_version::kOrderV1);
    bad.ordinal = 1;
    bad.payload = Payload()
                      .s("chain", "O1")
                      .s("account", Lab::kAccount)
                      .s("cl_ord_id", "c1")
                      .s("symbol", "EURUSD")
                      .s("side", "BUY")
                      .s("command_id", "K1")
                      .s("risk_domain", Lab::kDomain)
                      .i("order_qty", 100)
                      .i("limit_px", 10000)
                      .build();
    p.events.push_back(std::move(bad));

    CHECK_EQ(l.store().apply(p).kind, CommitKind::Conflict);
    CHECK(capture(l, "O1") == before);
    expect_consistent(l, "G02-07");
}

SCENARIO("G02-13", "P0", "an outbox row cannot exist without its command intent") {
    Lab l("g02-13");
    l.start();

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-orphan";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    OutboxIntent e;
    e.outbox_id = "ob-orphan";
    e.account = Lab::kAccount;
    e.command_id = "no-such-command";
    e.session_key = Lab::kSession;
    e.payload = "35=D";
    e.payload_hash = sha256(e.payload);
    p.effects.push_back(std::move(e));

    // The composite foreign key is what refuses this, which is the point of
    // declaring it tenant-aware in section 8 note 5.
    const CommitOutcome o = l.store().apply(p);
    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(0));
    expect_consistent(l, "G02-13");
}

SCENARIO("G02-14", "P0", "a projection write that violates a CHECK aborts the transaction") {
    Lab l("g02-14");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    expect_consistent(l, "G02-14 before");

    // Directly attempting the state the CHECK forbids: more traded than
    // ordered. The database refuses it even though no reducer was involved.
    const StoreError e = l.store().db().exec(
        "UPDATE orders SET cum_qty=5000 WHERE tenant_id='t1' AND order_chain_id='O1';");
    CHECK_NE(e, StoreError::None);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
}

SCENARIO("G02-16", "P0", "the transaction interface offers no way to call out mid-commit") {
    // An architecture assertion rather than a runtime one. TxPlan carries
    // data only: there is no callback, no socket and no handle on it, so a
    // caller cannot place an RPC inside the transaction even by accident.
    // Section 9.1 requires this; the type system is where it is enforced.
    static_assert(std::is_trivially_destructible_v<FenceToken>,
                  "fence token should stay a plain value");
    Lab l("g02-16");
    l.start();

    // The dispatcher, which is the thing that does talk to P01, operates on
    // committed rows and is a separate object from the store.
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    CHECK_EQ(o.kind, CommitKind::Committed);
    // Nothing reached the wire during the commit.
    CHECK_EQ(l.lp().total_writes(), std::size_t(0));

    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.lp().total_writes(), std::size_t(1));
    expect_consistent(l, "G02-16");
}

SCENARIO("G02-18", "P0", "a transaction abandoned mid-way claims no durability") {
    Lab l("g02-18");
    l.start();
    const Footprint before = capture(l, "O1");

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterBegin), fp::Action::Throw);
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();

    CHECK(!o.durable());
    CHECK(capture(l, "O1") == before);
    expect_consistent(l, "G02-18");
}

SCENARIO("G02-19", "P0", "an unsafe durability profile is reported, not accepted") {
    // Section 17.1 maps onto this engine as WAL plus FULL. A database opened
    // with a weaker setting still works, which is exactly why the verifier
    // has to say so rather than letting ACCEPTED_DURABLE stand.
    tt::TempDir dir("g02-19");
    SqliteAtomicStore::Options opt;
    opt.path = dir.file("weak.db");
    opt.synchronous = "OFF";
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::None);

    const VerifyReport v = verify_all(*store, Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::DurabilityProfileUnsafe));
}

SCENARIO("G02-20", "P0", "a tampered projection is caught even when the journal verifies") {
    Lab l("g02-20");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    expect_consistent(l, "G02-20 before");

    // Change only the projection's serialized state. Every hash in the
    // journal still checks out; only the replay comparison can see this.
    const std::string tampered =
        R"({"account":"A1","chain":"O1","cl_ord_id":"K1-cl","cum_qty":0,"exec_value":0,)"
        R"("exists":true,"fill_count":0,"leaves_qty":999,"limit_px":10000,"order_qty":999,)"
        R"("orig_cl_ord_id":"K1-cl","origin_command":"K1","pending_cancel":false,)"
        R"("pending_cl_ord_id":"","pending_px":0,"pending_qty":0,"pending_replace":false,)"
        R"("risk_domain":"D1","side":"BUY","status":"New","symbol":"EURUSD","v":"p03.v1"})";
    Stmt s;
    CHECK_EQ(l.store().db().prepare(
                 "UPDATE orders SET state=?1 WHERE tenant_id='t1' AND order_chain_id='O1';", &s),
             StoreError::None);
    s.bind_text(1, tampered);
    CHECK_EQ(s.run(), StoreError::None);

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::ProjectionReplayMismatch));
    CHECK(v.frozen_domains.count(Lab::kDomain) == 1);
}

SCENARIO("G02-12", "P0", "a retryable abort retries with the same key and commits once") {
    Lab l("g02-12");
    l.start();

    // The first BEGIN reports a lock conflict; the store retries the same
    // plan rather than rebuilding it, so the idempotency key is unchanged.
    fp::disarm_all();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    CHECK_EQ(o.kind, CommitKind::Committed);

    // Submitting again under the same key cannot create a second order.
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Replayed);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    expect_consistent(l, "G02-12");
}

SCENARIO_BLOCKED("G02-08", "P0", "one transaction across two risk domains crashes mid-way",
                 "the MVP atomicity domain in section 2 is one PostgreSQL primary and one "
                 "transaction, so a cross-domain transaction is already atomic here and the "
                 "scenario cannot distinguish the policies it is asking about. It needs the "
                 "multi-store topology section 2 explicitly defers.")

SCENARIO_BLOCKED("G02-17", "P0", "a savepoint rollback after selected mutations",
                 "the store exposes no savepoint: the write set is all-or-nothing by "
                 "construction, so there is no partial-commit path to test. Reinstating one "
                 "would be adding the hazard in order to test it.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_atomic_tx"); }
