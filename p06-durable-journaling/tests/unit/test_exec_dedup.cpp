// G08 — ExecID, fill ledger, duplicate/replay and correction.
//
// Transport dedup stops the same message being applied twice. These cases
// are about the layer underneath it: the same economic execution arriving
// under different transport identities, which is the only duplication that
// a FIX counterparty can actually inflict on a ledger.
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, std::int64_t qty = 100000) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", qty), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G08-01", "P0", "one execution produces one fill ledger row") {
    Lab l("g08-01");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 40000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    CHECK_EQ(l.row_count("business_exec_keys"), std::size_t(1));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.order("O1").leaves_qty, std::int64_t(60000));
    expect_consistent(l, "G08-01");
}

SCENARIO("G08-02", "P0", "the same execution under a new receipt applies once") {
    Lab l("g08-02");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 40000, 10000, "K1")).kind, CommitKind::Committed);
    // Different transport identity, same trade. Only the business key can
    // catch this (JI-06).
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 40000, 10000, "K1")).kind, CommitKind::Replayed);

    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.lp().economic_trade_count("E100"), std::size_t(1));
    expect_consistent(l, "G08-02");
}

SCENARIO("G08-03", "P0", "the same execution id with different economics is quarantined") {
    Lab l("g08-03");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 40000, 10000, "K1")).kind, CommitKind::Committed);

    // JI-07: a collision between a key and its content is not resolved by
    // preferring one of the two.
    const CommitOutcome o = l.deliver(l.fill("O1", "E100", 50000, 10000, "K1"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));
    expect_consistent(l, "G08-03");
}

SCENARIO("G08-04", "P0", "an execution id of zero is never used as a trade key") {
    Lab l("g08-04");
    seed(l);
    // Section 9.2 names this case explicitly: a status snapshot carrying
    // ExecID=0 is not a trade and must not become one.
    auto r = l.fill("O1", "0", 40000, 10000, "K1");
    const CommitOutcome o = l.deliver(r);
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    CHECK_EQ(l.row_count("fills"), std::size_t(0));

    auto empty = l.fill("O1", "", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(empty).kind, CommitKind::Quarantined);
    CHECK_EQ(l.row_count("fills"), std::size_t(0));
    expect_consistent(l, "G08-04");
}

SCENARIO("G08-05", "P0", "an overfill is refused rather than clamped") {
    Lab l("g08-05");
    seed(l, 50000);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 40000, 10000, "K1")).kind, CommitKind::Committed);

    // JI-08. There is no policy under which the ledger may exceed the order.
    const CommitOutcome o = l.deliver(l.fill("O1", "E2", 20000, 10000, "K1"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.order("O1").leaves_qty, std::int64_t(10000));
    expect_consistent(l, "G08-05");
}

SCENARIO("G08-06", "P0", "partial fills accumulate exactly") {
    Lab l("g08-06");
    seed(l, 100000);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 30000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.fill("O1", "E2", 25000, 10001, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.fill("O1", "E3", 45000, 10002, "K1")).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK_EQ(o.cum_qty, std::int64_t(100000));
    CHECK_EQ(o.leaves_qty, std::int64_t(0));
    CHECK_EQ(o.status, OrdStatus::Filled);
    CHECK_EQ(o.fill_count, std::int64_t(3));
    // Average price is carried as an exact integer sum, not a rounded mean.
    CHECK_EQ(o.exec_value, std::int64_t(30000) * 10000 + std::int64_t(25000) * 10001 +
                               std::int64_t(45000) * 10002);
    CHECK_EQ(l.lp().traded_quantity(), std::int64_t(100000));
    expect_consistent(l, "G08-06");
}

SCENARIO("G08-07", "P0", "a fill after a terminal state is quarantined") {
    Lab l("g08-07");
    seed(l);
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").status, OrdStatus::Canceled);

    // JI-40: the engine does not invent a state that would make this fit.
    const CommitOutcome o = l.deliver(l.fill("O1", "E-late", 10000, 10000, "K1"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    expect_consistent(l, "G08-07");
}

SCENARIO("G08-08", "P0", "the same execution id in two counterparty namespaces is distinct") {
    Lab l("g08-08");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E100", 20000, 10000, "K1")).kind, CommitKind::Committed);

    auto other = l.fill("O1", "E100", 20000, 10000, "K1");
    other.lp_namespace = "LP2";
    // The key is scoped to the namespace that makes it meaningful, so two
    // providers may legitimately both use "E100".
    CHECK_EQ(l.deliver(other).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("business_exec_keys"), std::size_t(2));
    expect_consistent(l, "G08-08");
}

SCENARIO("G08-09", "P0", "the fingerprint covers the economics, not the wrapper") {
    const Digest base = exec_fingerprint("O1", 40000, 10000, "TRADE");
    CHECK_NE(to_hex(exec_fingerprint("O1", 40001, 10000, "TRADE")), to_hex(base));
    CHECK_NE(to_hex(exec_fingerprint("O1", 40000, 10001, "TRADE")), to_hex(base));
    CHECK_NE(to_hex(exec_fingerprint("O2", 40000, 10000, "TRADE")), to_hex(base));
    CHECK_NE(to_hex(exec_fingerprint("O1", 40000, 10000, "CANCELED")), to_hex(base));
    CHECK_EQ(to_hex(exec_fingerprint("O1", 40000, 10000, "TRADE")), to_hex(base));
}

SCENARIO("G08-10", "P0", "every fill row has a dedup key behind it") {
    Lab l("g08-10");
    seed(l);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 10000, 10000, "K1"));

    // Verification SQL 5 from section 26.1, as an executable check: the
    // verifier reports an orphan fill rather than trusting the foreign key.
    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(v.ok());
    CHECK_EQ(l.row_count("fills"), l.row_count("business_exec_keys"));

    // Remove the key and watch the verifier notice.
    CHECK_EQ(l.store().db().exec("DELETE FROM business_exec_keys WHERE semantic_exec_id='E1';"),
             StoreError::None);
    const VerifyReport after = verify_all(l.store(), Lab::kTenant);
    CHECK(!after.ok());
    CHECK(after.has(Incident::OrphanDedupKey));
}

SCENARIO("G08-11", "P0", "the fill ledger survives a restart intact") {
    Lab l("g08-11");
    seed(l);
    l.deliver(l.fill("O1", "E1", 30000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 20000, 10000, "K1"));
    const OrderState before = l.order("O1");

    l.restart();
    l.start();
    CHECK(l.order("O1") == before);
    CHECK_EQ(l.row_count("fills"), std::size_t(2));
    // And the restarted instance rejects a redelivery of either one.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 30000, 10000, "K1")).kind, CommitKind::Replayed);
    CHECK_EQ(l.order("O1").cum_qty, before.cum_qty);
    expect_consistent(l, "G08-11");
}

SCENARIO("G08-12", "P0", "a correction is a new event with named authority") {
    Lab l("g08-12");
    seed(l);
    l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"));
    const Version before = l.store().head(Lab::kTenant, order_stream("O1")).version;

    // JI-02: a bust does not edit history. It appends, and it carries who
    // authorised it.
    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-bust";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O1");
    e.expected_version = before;
    e.event_id = "e-bust";
    e.event_type = std::string(ev::kOrderCorrected);
    e.reducer_version = std::string(reducer_version::kOrderV1);
    e.ordinal = 0;
    e.payload = Payload()
                    .s("authorized_by", "risk-officer+ops-lead")
                    .i("cum_qty", 0)
                    .i("exec_value", 0)
                    .build();
    p.events.push_back(std::move(e));
    CHECK_EQ(l.store().apply(p).kind, CommitKind::Committed);

    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    CHECK_EQ(l.store().head(Lab::kTenant, order_stream("O1")).version, before + 1);
    // The original fill event is still there.
    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 0, &page);
    bool saw_fill = false;
    for (const JournalRecord& r : page) {
        if (r.event_type == ev::kFillApplied) saw_fill = true;
    }
    CHECK(saw_fill);
    expect_consistent(l, "G08-12");
}

SCENARIO("G08-13", "P0", "a correction without an authorising party is refused") {
    Lab l("g08-13");
    seed(l);
    l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"));
    const Version before = l.store().head(Lab::kTenant, order_stream("O1")).version;

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-bust-anon";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O1");
    e.expected_version = before;
    e.event_id = "e-bust-anon";
    e.event_type = std::string(ev::kOrderCorrected);
    e.reducer_version = std::string(reducer_version::kOrderV1);
    e.ordinal = 0;
    e.payload = Payload().s("authorized_by", "").i("cum_qty", 0).i("exec_value", 0).build();
    p.events.push_back(std::move(e));

    CHECK_NE(l.store().apply(p).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "G08-13");
}

SCENARIO("G08-14", "P0", "a correction beyond the order quantity is refused") {
    Lab l("g08-14");
    seed(l, 50000);
    l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"));

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-bust-big";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O1");
    e.expected_version = l.store().head(Lab::kTenant, order_stream("O1")).version;
    e.event_id = "e-bust-big";
    e.event_type = std::string(ev::kOrderCorrected);
    e.reducer_version = std::string(reducer_version::kOrderV1);
    e.ordinal = 0;
    e.payload =
        Payload().s("authorized_by", "ops").i("cum_qty", 999999).i("exec_value", 0).build();
    p.events.push_back(std::move(e));

    CHECK_EQ(l.store().apply(p).kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "G08-14");
}

SCENARIO("G08-15", "P1", "a zero or negative quantity is not a trade") {
    Lab l("g08-15");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E-zero", 0, 10000, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.deliver(l.fill("O1", "E-neg", -5, 10000, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.row_count("fills"), std::size_t(0));
    expect_consistent(l, "G08-15");
}

SCENARIO_BLOCKED("G08-16", "P0", "the counterparty's ExecID scope differs from the assumption",
                 "section 30 lists ExecID scope as something only a real Rules of Engagement "
                 "can settle. The engine's behaviour under an assumed scope is tested by "
                 "G08-08; the assumption itself cannot be validated in a lab.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_exec_dedup"); }
