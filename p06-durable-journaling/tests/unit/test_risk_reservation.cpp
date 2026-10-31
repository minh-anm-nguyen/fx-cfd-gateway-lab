// G09 — Risk reservation, limits and atomic exposure recovery.
//
// Risk is measured in quantity units here, so every assertion below is an
// exact integer equality rather than a tolerance. That is deliberate: JI-09
// says the reservation moves in the same transaction as the order effect,
// and an equality is the only way to show it did.
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, std::int64_t qty = 100000) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", qty), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G09-01", "P0", "accepting an order holds exactly its quantity") {
    Lab l("g09-01");
    seed(l, 40000);
    const RiskDomainState r = l.risk();
    CHECK_EQ(r.reserved, std::int64_t(40000));
    CHECK_EQ(r.exposure, std::int64_t(0));
    CHECK_EQ(r.utilised(), std::int64_t(40000));
    expect_consistent(l, "G09-01");
}

SCENARIO("G09-02", "P0", "a fill moves quantity from held to exposed, never duplicating it") {
    Lab l("g09-02");
    seed(l, 100000);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 40000, 10000, "K1")).kind, CommitKind::Committed);

    const RiskDomainState r = l.risk();
    CHECK_EQ(r.reserved, std::int64_t(60000));
    CHECK_EQ(r.exposure, std::int64_t(40000));
    // The total committed against the limit has not moved: a fill converts
    // a hold, it does not add to it.
    CHECK_EQ(r.utilised(), std::int64_t(100000));
    expect_consistent(l, "G09-02");
}

SCENARIO("G09-03", "P0", "a completed order leaves nothing held") {
    Lab l("g09-03");
    seed(l, 50000);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 50000, 10000, "K1")).kind, CommitKind::Committed);

    const RiskDomainState r = l.risk();
    CHECK_EQ(r.reserved, std::int64_t(0));
    CHECK_EQ(r.exposure, std::int64_t(50000));
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);
    expect_consistent(l, "G09-03");
}

SCENARIO("G09-04", "P0", "a cancel releases the remaining hold and only that") {
    Lab l("g09-04");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 30000, 10000, "K1"));
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K1")).kind, CommitKind::Committed);

    const RiskDomainState r = l.risk();
    CHECK_EQ(r.reserved, std::int64_t(0));
    // What traded stays exposed. A cancel does not undo a fill.
    CHECK_EQ(r.exposure, std::int64_t(30000));
    expect_consistent(l, "G09-04");
}

SCENARIO("G09-05", "P0", "a pending cancel releases nothing") {
    // JI-10, stated as plainly as the invariant does: the order can still
    // trade while the cancel is in flight.
    Lab l("g09-05");
    seed(l, 100000);
    CHECK_EQ(l.service().submit_cancel(l.cancel("K2", "O1"), l.token()).kind,
             CommitKind::Committed);

    CHECK(l.order("O1").pending_cancel);
    CHECK_EQ(l.order("O1").effective_status(), OrdStatus::PendingCancel);
    CHECK_EQ(l.risk().reserved, std::int64_t(100000));
    CHECK_EQ(l.risk().exposure, std::int64_t(0));

    // And a fill against the pending order still works.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 20000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.risk().exposure, std::int64_t(20000));
    CHECK_EQ(l.risk().reserved, std::int64_t(80000));
    expect_consistent(l, "G09-05");
}

SCENARIO("G09-06", "P0", "the domain ceiling cannot be exceeded by two accounts") {
    // The firm-limit half of the group: two accounts drawing on one shared
    // domain cannot both be admitted past the ceiling.
    Lab l("g09-06", /*limit=*/100000);
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 70000), l.token()).kind,
             CommitKind::Committed);

    auto second = l.new_order("K2", "O2", 70000);
    second.account = "A2";
    const CommitOutcome o = l.service().submit_new(second, l.token());
    CHECK_EQ(o.kind, CommitKind::Rejected);

    CHECK_EQ(l.risk().utilised(), std::int64_t(70000));
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    expect_consistent(l, "G09-06");
}

SCENARIO("G09-07", "P0", "an order exactly at the ceiling is admitted") {
    Lab l("g09-07", /*limit=*/50000);
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 50000), l.token()).kind,
             CommitKind::Committed);
    CHECK_EQ(l.risk().headroom(), std::int64_t(0));
    // And one unit more is not.
    CHECK_EQ(l.service().submit_new(l.new_order("K2", "O2", 1), l.token()).kind,
             CommitKind::Rejected);
    expect_consistent(l, "G09-07");
}

SCENARIO("G09-08", "P0", "releasing more than is held is refused") {
    Lab l("g09-08");
    seed(l, 50000);
    const Version base = l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version;

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-overrelease";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = risk_stream(Lab::kDomain);
    e.expected_version = base;
    e.event_id = "e-overrelease";
    e.event_type = std::string(ev::kRiskReleased);
    e.reducer_version = std::string(reducer_version::kRiskV1);
    e.ordinal = 0;
    e.payload = Payload()
                    .s("reservation_id", GatewayService::reservation_for("O1"))
                    .i("amount", 99999)
                    .build();
    p.events.push_back(std::move(e));

    const CommitOutcome o = l.store().apply(p);
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.risk().reserved, std::int64_t(50000));
    expect_consistent(l, "G09-08");
}

SCENARIO("G09-09", "P0", "releasing the same hold twice is refused") {
    Lab l("g09-09");
    seed(l, 50000);
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.risk().reserved, std::int64_t(0));

    const Version base = l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version;
    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-rerelease";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = risk_stream(Lab::kDomain);
    e.expected_version = base;
    e.event_id = "e-rerelease";
    e.event_type = std::string(ev::kRiskReleased);
    e.reducer_version = std::string(reducer_version::kRiskV1);
    e.ordinal = 0;
    e.payload = Payload()
                    .s("reservation_id", GatewayService::reservation_for("O1"))
                    .i("amount", 50000)
                    .build();
    p.events.push_back(std::move(e));

    CHECK_EQ(l.store().apply(p).kind, CommitKind::Quarantined);
    CHECK_EQ(l.risk().reserved, std::int64_t(0));
    expect_consistent(l, "G09-09");
}

SCENARIO("G09-10", "P0", "a failed admission leaks no hold") {
    Lab l("g09-10", 100000);
    l.start();
    const std::int64_t before = l.risk().reserved;

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterCommandIntent), fp::Action::ReturnError);
    l.service().submit_new(l.new_order("K1", "O1", 40000), l.token());
    fp::disarm_all();

    CHECK_EQ(l.risk().reserved, before);
    CHECK_EQ(l.row_count("risk_reservations"), std::size_t(0));
    expect_consistent(l, "G09-10");
}

SCENARIO("G09-11", "P0", "the exposure ledger equals the sum of unique verified fills") {
    Lab l("g09-11");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 25000, 10000, "K1"));
    // Two redeliveries that must change nothing.
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 25000, 10000, "K1"));

    // Release gate D from section 27: the risk ledger matches the unique
    // executions the independent counterparty ledger knows about.
    CHECK_EQ(l.risk().exposure, std::int64_t(45000));
    CHECK_EQ(l.lp().traded_quantity(), std::int64_t(45000));
    CHECK_EQ(l.lp().distinct_trades(), std::size_t(2));
    CHECK_EQ(l.scalar("SELECT COALESCE(SUM(last_qty),0) FROM fills WHERE tenant_id='t1';"),
             std::int64_t(45000));
    expect_consistent(l, "G09-11");
}

SCENARIO("G09-12", "P0", "a reservation sum that disagrees with the domain is caught") {
    Lab l("g09-12");
    seed(l, 50000);
    // Corrupt only the aggregate figure. Every hash still verifies; only
    // the conservation check sees it (section 16).
    CHECK_EQ(l.store().db().exec(
                 "UPDATE risk_domains SET reserved=12345 WHERE tenant_id='t1' AND "
                 "domain_id='D1';"),
             StoreError::None);

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::RiskLedgerDiff));
    CHECK(v.frozen_domains.count(Lab::kDomain) == 1);
}

SCENARIO("G09-13", "P0", "lowering a limit below what is committed is refused") {
    Lab l("g09-13", 100000);
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 80000), l.token());

    const CommitOutcome o = l.service().set_risk_limit(Lab::kTenant, Lab::kDomain, 10000,
                                                       l.token());
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.risk().limit, std::int64_t(100000));
    expect_consistent(l, "G09-13");
}

SCENARIO("G09-14", "P0", "the risk state is rebuilt exactly by replay") {
    Lab l("g09-14");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 30000, 10000, "K1"));
    l.service().submit_new(l.new_order("K2", "O2", 20000), l.token());
    l.deliver(l.terminal("O2", "REJECTED", "K2"));

    const RiskDomainState projected = l.risk();
    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, risk_stream(Lab::kDomain));
    CHECK_MSG(r.ok, r.detail);

    const IRiskReducer* red = l.store().reducers().risk(reducer_version::kRiskV1);
    CHECK_EQ(canonical_encode(red->to_canon(projected)), canonical_encode(r.state));
    expect_consistent(l, "G09-14");
}

SCENARIO("G09-15", "P0", "risk survives a restart and admission continues from there") {
    Lab l("g09-15", 100000);
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 60000), l.token());
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    const RiskDomainState before = l.risk();

    l.restart();
    l.start();
    CHECK(l.risk() == before);
    // Headroom is what it was, so an order that would have fitted still
    // fits and one that would not still does not.
    CHECK_EQ(l.service().submit_new(l.new_order("K2", "O2", 40000), l.token()).kind,
             CommitKind::Committed);
    CHECK_EQ(l.service().submit_new(l.new_order("K3", "O3", 1), l.token()).kind,
             CommitKind::Rejected);
    expect_consistent(l, "G09-15");
}

SCENARIO_BLOCKED("G09-16", "P0", "two writers race the same domain under SERIALIZABLE",
                 "SQLite takes the write lock at BEGIN IMMEDIATE, so the PostgreSQL "
                 "serialization-failure path section 9.3 describes cannot be produced. The "
                 "version CAS that protects the same invariant is covered by G01-04.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_risk_reservation"); }
