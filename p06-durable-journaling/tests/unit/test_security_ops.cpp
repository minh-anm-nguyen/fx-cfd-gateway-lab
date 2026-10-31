// G21 and G22 — Tenant isolation, tamper evidence, alerting and the
// operator path.
//
// Section 18 is careful about what a hash stored next to the data it
// protects can and cannot do, and these cases keep to that line: they
// assert detection, never prevention, wherever the attacker is assumed to
// hold database credentials.
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l) {
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 100000), l.token()).kind,
             CommitKind::Committed);
    l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"));
}

// --- Tenant isolation ------------------------------------------------------

SCENARIO("SEC-01", "P0", "a command for another tenant cannot touch this one's state") {
    Lab l("sec-01");
    seed(l);
    FenceToken t2 = 0;
    CHECK_EQ(l.store().acquire_fence("t2", Lab::kDomain, "other", 60000, &t2),
             StoreError::None);

    GatewayService::Options gopt;
    GatewayService svc2(l.store(), gopt);
    CHECK_EQ(svc2.set_risk_limit("t2", Lab::kDomain, 500000, t2).kind, CommitKind::Committed);

    auto r = l.new_order("K1", "O1", 999);
    r.tenant = "t2";
    CHECK_EQ(svc2.submit_new(r, t2).kind, CommitKind::Committed);

    // Each tenant's projection is its own (JI-33).
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(100000));
    OrderState other;
    CHECK(l.store().load_order("t2", "O1", &other));
    CHECK_EQ(other.order_qty, std::int64_t(999));
    expect_consistent(l, "SEC-01");
}

SCENARIO("SEC-02", "P0", "a receipt for another tenant does not dedup against this one") {
    Lab l("sec-02");
    seed(l);
    const std::size_t before = l.row_count("inbox_receipts");
    // Same session, same receipt id, different tenant: a different key.
    auto r = l.fill("O1", "E-x", 1000, 10000, "K1");
    r.tenant = "t2";
    l.service().apply_report(r, l.token());
    CHECK_EQ(l.row_count("inbox_receipts"), before);
}

SCENARIO("SEC-03", "P0", "the supervisor serves exactly one tenant") {
    Lab l("sec-03");
    l.start();
    CHECK(l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
    CHECK(!l.supervisor().may_admit("t2", Lab::kDomain));
    CHECK(!l.supervisor().may_dispatch("t2", Lab::kDomain));
    CHECK(!l.supervisor().why_blocked("t2", Lab::kDomain).empty());
}

SCENARIO("SEC-04", "P0", "the verifier reports per tenant and does not mix evidence") {
    Lab l("sec-04");
    seed(l);
    FenceToken t2 = 0;
    l.store().acquire_fence("t2", Lab::kDomain, "other", 60000, &t2);
    GatewayService::Options gopt;
    GatewayService svc2(l.store(), gopt);
    svc2.set_risk_limit("t2", Lab::kDomain, 500000, t2);
    auto r = l.new_order("K9", "O9", 1000);
    r.tenant = "t2";
    svc2.submit_new(r, t2);

    // Break tenant 1 only.
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    l.store().db().exec("UPDATE journal_events SET state_hash='" + std::string(64, 'e') +
                        "' WHERE tenant_id='t1' AND stream_id='ord:O1' AND stream_version=1;");

    CHECK(!verify_all(l.store(), Lab::kTenant).ok());
    CHECK(verify_all(l.store(), "t2").ok());
}

// --- Tamper evidence --------------------------------------------------------

SCENARIO("SEC-AUDIT-01", "P0", "an altered audit row breaks the chain") {
    Lab l("sec-audit-01");
    seed(l);
    std::string why;
    CHECK_MSG(l.store().audit_chain_intact(Lab::kTenant, &why), why);
    CHECK(l.store().audit_count(Lab::kTenant) >= std::size_t(3));

    CHECK_EQ(l.store().db().exec(
                 "UPDATE audit_log SET detail='nothing happened' WHERE audit_id="
                 "(SELECT MIN(audit_id) FROM audit_log WHERE tenant_id='t1');"),
             StoreError::None);
    CHECK(!l.store().audit_chain_intact(Lab::kTenant, &why));
    CHECK(!why.empty());
}

SCENARIO("SEC-AUDIT-02", "P0", "a removed audit row breaks the chain") {
    Lab l("sec-audit-02");
    seed(l);
    CHECK_EQ(l.store().db().exec(
                 "DELETE FROM audit_log WHERE audit_id=(SELECT MIN(audit_id) FROM audit_log "
                 "WHERE tenant_id='t1');"),
             StoreError::None);
    std::string why;
    CHECK(!l.store().audit_chain_intact(Lab::kTenant, &why));

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(!v.audit_valid);
}

SCENARIO("SEC-AUDIT-03", "P0", "the audit chain claims detection, not resistance") {
    // Section 18 states the limit plainly: an attacker who can write rows
    // can also rewrite the chain, which is why an external anchor is asked
    // for on top. What the chain does guarantee is that a partial edit,
    // which is the realistic accident, is visible.
    Lab l("sec-audit-03");
    seed(l);
    CHECK_EQ(l.store().db().exec("UPDATE audit_log SET data='{}' WHERE audit_id="
                                 "(SELECT MAX(audit_id) FROM audit_log WHERE tenant_id='t1');"),
             StoreError::None);
    std::string why;
    CHECK(!l.store().audit_chain_intact(Lab::kTenant, &why));
}

SCENARIO("SEC-05", "P0", "an audit record exists for every business decision") {
    Lab l("sec-05");
    l.start();
    const std::size_t base = l.store().audit_count(Lab::kTenant);
    l.service().submit_new(l.new_order("K1", "O1", 50000), l.token());
    CHECK_EQ(l.store().audit_count(Lab::kTenant), base + 1);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    CHECK_EQ(l.store().audit_count(Lab::kTenant), base + 2);
    l.deliver(l.fill("O-unknown", "E2", 10000, 10000, ""));  // quarantine
    CHECK_EQ(l.store().audit_count(Lab::kTenant), base + 3);
    std::string why;
    CHECK_MSG(l.store().audit_chain_intact(Lab::kTenant, &why), why);
}

SCENARIO("SEC-06", "P0", "audit records carry hashes, not payloads") {
    // Section 22 forbids unmasked payloads in shared logs. The audit data
    // for an inbound report carries a digest of the raw message, not the
    // message.
    Lab l("sec-06");
    seed(l);
    Stmt s;
    CHECK_EQ(l.store().db().prepare(
                 "SELECT data FROM audit_log WHERE tenant_id='t1' AND category='inbound.applied'"
                 " ORDER BY audit_id DESC LIMIT 1;",
                 &s),
             StoreError::None);
    bool row = false;
    CHECK_EQ(s.step(&row), StoreError::None);
    CHECK(row);
    const std::string data = s.col_text(0);
    CHECK(data.find("raw_hash") != std::string::npos);
    // The raw FIX bytes themselves are not in there.
    CHECK(data.find("35=8|17=") == std::string::npos);
}

// --- Operations and alerting --------------------------------------------------

SCENARIO("OPS-01", "P0", "an integrity failure increments the metric an operator is paged on") {
    Lab l("ops-01");
    seed(l);
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    l.store().db().exec("UPDATE journal_events SET state_hash='" + std::string(64, 'f') +
                        "' WHERE stream_id='ord:O1' AND stream_version=1;");

    CHECK_EQ(l.metrics().get(metric::kRecoveryIntegrityFail), std::int64_t(0));
    l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK(l.metrics().get(metric::kRecoveryIntegrityFail) > 0);
}

SCENARIO("OPS-02", "P0", "unknown outbound effects are counted for the alert threshold") {
    Lab l("ops-02");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    l.store().set_outbox_state(Lab::kTenant,
                               l.store().outbox_rows(Lab::kTenant).front().outbox_id, "UNKNOWN",
                               0, std::nullopt, "forced for the test");

    l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK_EQ(l.metrics().get(metric::kOutboxUnknownCount), std::int64_t(1));
}

SCENARIO("OPS-03", "P0", "a quarantine is counted and leaves a case with evidence") {
    Lab l("ops-03");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O-unknown", "E9", 1000, 10000, "")).kind,
             CommitKind::Quarantined);
    CHECK_EQ(l.metrics().get(metric::kQuarantineTotal), std::int64_t(1));

    const std::vector<RecoveryCasePlan> cases = l.store().open_cases(Lab::kTenant);
    CHECK_EQ(cases.size(), std::size_t(1));
    CHECK_EQ(cases.front().reason_code, std::string("CONTRADICTORY_REPORT"));
    // The evidence hash lets an investigator find the exact message.
    CHECK_NE(to_hex(cases.front().evidence_hash), to_hex(zero_digest()));
}

SCENARIO("OPS-04", "P0", "a blocked admission is counted, not silently dropped") {
    Lab l("ops-04");
    l.start();
    l.supervisor().demote("operator freeze");
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Rejected);
    CHECK(l.metrics().get(metric::kAdmissionBlocked) > 0);
}

SCENARIO("OPS-05", "P0", "the reason a domain is blocked is reportable in words") {
    Lab l("ops-05");
    seed(l);
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    l.store().db().exec("UPDATE stream_heads SET state_hash='" + std::string(64, 'a') +
                        "' WHERE stream_id='ord:O1';");
    l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);

    const std::string why = l.supervisor().why_blocked(Lab::kTenant, Lab::kDomain);
    CHECK(!why.empty());
    CHECK(why.find("QUARANTINE") != std::string::npos ||
          why.find("frozen") != std::string::npos);
}

SCENARIO("OPS-06", "P0", "the recovery report names every incident it found") {
    Lab l("ops-06");
    seed(l);
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    l.store().db().exec("UPDATE journal_events SET state_hash='" + std::string(64, 'b') +
                        "' WHERE stream_id='ord:O1' AND stream_version=1;");

    const RecoveryReport rep = l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK(!rep.incidents.empty());
    CHECK_EQ(rep.incidents.size(), rep.details.size());
    for (const std::string& d : rep.details) CHECK(!d.empty());
}

SCENARIO("OPS-07", "P1", "metrics can be enumerated for a scrape") {
    Lab l("ops-07");
    seed(l);
    l.metrics().inc(metric::kDeadlockTotal, 2);
    l.metrics().set(metric::kOutboxOldestAge, 42);
    const auto snap = l.metrics().snapshot();
    CHECK(!snap.empty());
    CHECK_EQ(l.metrics().get(metric::kDeadlockTotal), std::int64_t(2));
    CHECK_EQ(l.metrics().get(metric::kOutboxOldestAge), std::int64_t(42));
    // A name that was never touched reads as zero rather than erroring.
    CHECK_EQ(l.metrics().get(metric::kSnapshotChecksumFailure), std::int64_t(0));
}

SCENARIO("OPS-08", "P0", "an operator cannot close a case without approvals") {
    Lab l("ops-08");
    seed(l);
    l.deliver(l.fill("O-unknown", "E9", 1000, 10000, ""));
    const CaseId id = l.store().open_cases(Lab::kTenant).front().case_id;

    CHECK_EQ(l.store().resolve_case(Lab::kTenant, id, "RESOLVED", ""), StoreError::Rejected);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));
    CHECK_EQ(l.store().resolve_case(Lab::kTenant, id, "RESOLVED", "ops-lead+risk-officer"),
             StoreError::None);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(0));
}

SCENARIO_BLOCKED("SEC-07", "P0", "database credentials are held in a secret manager",
                 "section 18 asks for runtime secrets and least privilege, which are "
                 "deployment properties. Nothing in this code path reads a credential, so "
                 "there is nothing here to assert.")

SCENARIO_BLOCKED("SEC-08", "P0", "row-level security confines a compromised role",
                 "RLS is PostgreSQL. The tenant scoping that the application enforces is "
                 "covered by SEC-01 to SEC-04; a database-enforced version needs the real "
                 "engine and a migration that grants it.")

SCENARIO_BLOCKED("OPS-09", "P1", "alert routing reaches the on-call engineer",
                 "the metric names and their values are asserted by OPS-01 to OPS-04. Where "
                 "those values are routed is a monitoring deployment, outside this binary.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_security_ops"); }
