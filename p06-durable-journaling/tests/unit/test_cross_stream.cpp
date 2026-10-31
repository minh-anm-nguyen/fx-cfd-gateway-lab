// G12 — Cross-stream ordering, sequence pitfalls and transaction manifests.
//
// Section 7 carries an explicit warning about allocated-before-commit ids,
// and section 12 says causality may not be inferred from a clock, a row id
// or an unordered query. The manifest is what ties the members of one
// business transaction together instead.
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G12-02", "P0", "one business transaction names all its members") {
    Lab l("g12-02");
    seed(l);
    // The admission touched two streams, so the manifest must say two.
    const std::int64_t count = l.scalar(
        "SELECT expected_event_count FROM journal_transactions WHERE tenant_id='t1' AND tx_id="
        "(SELECT tx_id FROM journal_events WHERE stream_id='ord:O1' AND stream_version=1);");
    CHECK_EQ(count, std::int64_t(2));

    const std::int64_t members = l.scalar(
        "SELECT COUNT(*) FROM journal_events WHERE tenant_id='t1' AND tx_id="
        "(SELECT tx_id FROM journal_events WHERE stream_id='ord:O1' AND stream_version=1);");
    CHECK_EQ(members, count);
    expect_consistent(l, "G12-02");
}

SCENARIO("G12-03", "P0", "a manifest with a missing member is detected") {
    Lab l("g12-03");
    seed(l);
    l.store().db().exec("DROP TRIGGER journal_events_append_only_delete;");
    // Remove the risk half of the admission, leaving the order half.
    CHECK_EQ(l.store().db().exec(
                 "DELETE FROM journal_events WHERE stream_id='rsk:D1' AND stream_version=2;"),
             StoreError::None);

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::TxManifestMismatch) || v.has(Incident::StreamVersionGap));
}

SCENARIO("G12-04", "P0", "a manifest whose digest no longer covers its members is detected") {
    Lab l("g12-04");
    seed(l);
    CHECK_EQ(l.store().db().exec("UPDATE journal_transactions SET members_digest='" +
                                 std::string(64, 'c') + "';"),
             StoreError::None);
    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::TxManifestMismatch));
}

SCENARIO("G12-05", "P0", "an event cannot be written without a manifest to belong to") {
    Lab l("g12-05");
    seed(l);
    // The composite foreign key is the structural half of the check.
    const StoreError e = l.store().db().exec(
        "INSERT INTO journal_events(tenant_id,stream_id,stream_version,event_id,tx_id,"
        "event_ordinal,event_type,schema_version,reducer_version,policy_version,payload,"
        "canonical_hash,prev_state_hash,state_hash,created_at) VALUES('t1','ord:O1',99,'ex',"
        "'no-such-tx',0,'FillApplied',1,'p03.v1',1,'{}','" +
        std::string(64, 'a') + "','" + std::string(64, 'a') + "','" + std::string(64, 'a') +
        "',0);");
    CHECK_NE(e, StoreError::None);
}

SCENARIO("G12-06", "P0", "replay follows stream version, not row insertion order") {
    Lab l("g12-06");
    seed(l);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 20000, 10000, "K1"));

    // created_at is rewritten to the reverse of the real order. Section 8
    // note 8: this is audit metadata and must not move replay.
    CHECK_EQ(l.store().db().exec("DROP TRIGGER journal_events_append_only_update;"),
             StoreError::None);
    CHECK_EQ(l.store().db().exec(
                 "UPDATE journal_events SET created_at = 1000000 - stream_version;"),
             StoreError::None);

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
    CHECK_EQ(r.version, std::uint64_t(3));
    const IOrderReducer* red = l.store().reducers().order(reducer_version::kOrderV1);
    CHECK_EQ(canonical_encode(red->to_canon(l.order("O1"))), canonical_encode(r.state));
}

SCENARIO("G12-07", "P0", "a stream read is ordered by version regardless of storage order") {
    Lab l("g12-07");
    seed(l);
    for (int i = 0; i < 5; ++i) {
        l.deliver(l.fill("O1", "E" + std::to_string(i), 10000, 10000, "K1"));
    }
    std::vector<JournalRecord> page;
    CHECK_EQ(l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 0, &page),
             StoreError::None);
    for (std::size_t i = 1; i < page.size(); ++i) {
        CHECK(page[i].version == page[i - 1].version + 1);
    }
}

SCENARIO("G12-08", "P0", "two independent domains advance without interfering") {
    Lab l("g12-08");
    l.start();
    FenceToken t2 = 0;
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, "D2", "gateway-lab", 60000, &t2),
             StoreError::None);
    CHECK_EQ(l.service().set_risk_limit(Lab::kTenant, "D2", 500000, t2).kind,
             CommitKind::Committed);

    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    auto other = l.new_order("K2", "O2", 2000);
    other.risk_domain = "D2";
    CHECK_EQ(l.service().submit_new(other, t2).kind, CommitKind::Committed);

    CHECK_EQ(l.risk().reserved, std::int64_t(1000));
    RiskDomainState d2;
    CHECK(l.store().load_risk(Lab::kTenant, "D2", &d2));
    CHECK_EQ(d2.reserved, std::int64_t(2000));
    expect_consistent(l, "G12-08");
}

SCENARIO("G12-09", "P0", "a transaction touching two streams advances both or neither") {
    Lab l("g12-09");
    seed(l);
    const Version obefore = l.store().head(Lab::kTenant, order_stream("O1")).version;
    const Version rbefore = l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version;

    CHECK_EQ(l.deliver(l.fill("O1", "E1", 10000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.store().head(Lab::kTenant, order_stream("O1")).version, obefore + 1);
    CHECK_EQ(l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version, rbefore + 1);

    // Both halves carry the same transaction id.
    const std::int64_t shared = l.scalar(
        "SELECT COUNT(DISTINCT tx_id) FROM journal_events WHERE tenant_id='t1' AND "
        "((stream_id='ord:O1' AND stream_version=" +
        std::to_string(obefore + 1) + ") OR (stream_id='rsk:D1' AND stream_version=" +
        std::to_string(rbefore + 1) + "));");
    CHECK_EQ(shared, std::int64_t(1));
    expect_consistent(l, "G12-09");
}

SCENARIO("G12-10", "P0", "several risk events in one transaction take consecutive versions") {
    // A replace releases and re-reserves in the same transaction, so the
    // risk stream advances twice under one manifest.
    Lab l("g12-10");
    seed(l);
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    l.service().submit_replace(l.replace("K2", "O1", 60000), l.token());
    const Version rbefore = l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version;

    CHECK_EQ(l.deliver(l.terminal("O1", "REPLACED", "K2")).kind, CommitKind::Committed);
    CHECK_EQ(l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version, rbefore + 2);
    expect_consistent(l, "G12-10");
}

SCENARIO("G12-11", "P0", "ordinals within a transaction are unique and dense") {
    Lab l("g12-11");
    seed(l);
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    l.service().submit_replace(l.replace("K2", "O1", 60000), l.token());
    l.deliver(l.terminal("O1", "REPLACED", "K2"));

    const std::int64_t bad = l.scalar(
        "SELECT COUNT(*) FROM (SELECT tx_id, COUNT(*) c, COUNT(DISTINCT event_ordinal) d, "
        "MAX(event_ordinal) m FROM journal_events WHERE tenant_id='t1' GROUP BY tx_id) "
        "WHERE c<>d OR m<>c-1;");
    CHECK_EQ(bad, std::int64_t(0));
    expect_consistent(l, "G12-11");
}

SCENARIO("G12-12", "P0", "the verifier walks every stream, not just the ones it was given") {
    Lab l("g12-12");
    seed(l);
    l.service().submit_new(l.new_order("K2", "O2", 1000), l.token());
    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(v.ok());
    // Three streams: the risk domain and two order chains.
    CHECK_EQ(v.streams_checked, std::size_t(3));
    CHECK(v.events_checked >= std::size_t(5));
}

SCENARIO("G12-13", "P1", "an empty tenant verifies rather than erroring") {
    Lab l("g12-13");
    l.start();
    const VerifyReport v = verify_all(l.store(), "tenant-with-nothing");
    CHECK(v.ok());
    CHECK_EQ(v.streams_checked, std::size_t(0));
}

SCENARIO_BLOCKED("G12-14", "P0", "BIGSERIAL ids allocated out of commit order",
                 "the hazard section 7 warns about is a PostgreSQL sequence handing out ids "
                 "before COMMIT. This store never derives ordering from a row id, which "
                 "G12-06 shows, but reproducing the hazard itself needs PostgreSQL and two "
                 "concurrent writers.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_cross_stream"); }
