// G19 — Retention, compaction, partitioning and long-term audit.
//
// Section 13 says v1 deletes no journal, and JI-14 lists what else may not
// be pruned while it is still needed. Most of what the pruner does is
// refuse, so most of these cases assert a refusal and the reason given.
#include "lab.hpp"

using namespace lab;

namespace {

RetentionPolicy lenient() {
    RetentionPolicy p;
    p.horizon_ms = 0;  // everything is old enough
    p.command_key_horizon_ms = 0;
    p.resend_window_ms = 0;
    p.allow_journal_deletion = false;
    return p;
}

// A store where every order has reached a terminal state and every effect
// is resolved, which is the only situation in which pruning is allowed.
void settle(Lab& l) {
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 50000), l.token()).kind,
             CommitKind::Committed);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 50000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);
    l.clock().advance(30LL * 24 * 3600 * 1000);
}

SCENARIO("G19-01", "P0", "journal deletion is refused outright") {
    Lab l("g19-01");
    settle(l);
    RetentionPolicy p = lenient();
    p.allow_journal_deletion = true;
    const RetentionResult r = prune(l.store(), Lab::kTenant, p);
    CHECK(r.refused);
    CHECK(r.refusal_reason.find("journal") != std::string::npos);
    expect_consistent(l, "G19-01");
}

SCENARIO("G19-02", "P0", "pruning is refused while an order is still open") {
    Lab l("g19-02");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 50000), l.token());
    l.clock().advance(30LL * 24 * 3600 * 1000);

    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK(r.refused);
    CHECK(r.refusal_reason.find("open") != std::string::npos);
    CHECK_EQ(r.receipts_pruned, std::size_t(0));
    expect_consistent(l, "G19-02");
}

SCENARIO("G19-03", "P0", "pruning is refused while an outbound effect is unresolved") {
    Lab l("g19-03");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 50000), l.token());
    // The report carries no originating command, so nothing confirms the
    // outbound effect even though the order itself is now terminal.
    l.deliver(l.fill("O1", "E1", 50000, 10000, ""));
    // The outbox row is still READY: nothing proved it reached anyone.
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(1));
    l.clock().advance(30LL * 24 * 3600 * 1000);

    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK(r.refused);
    CHECK(r.refusal_reason.find("unresolved") != std::string::npos);
    expect_consistent(l, "G19-03");
}

SCENARIO("G19-04", "P0", "pruning is refused while a reconciliation case is open") {
    Lab l("g19-04");
    settle(l);
    RecoveryCasePlan c;
    c.case_id = "case-open";
    c.chain = "O1";
    c.reason_code = "UNKNOWN_OUTCOME";
    c.status = "OPEN";
    CHECK_EQ(l.store().open_case(Lab::kTenant, c), StoreError::None);

    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK(r.refused);
    CHECK(r.refusal_reason.find("case") != std::string::npos);
    expect_consistent(l, "G19-04");
}

SCENARIO("G19-05", "P0", "a settled store prunes receipts and keeps the journal") {
    Lab l("g19-05");
    settle(l);
    const std::size_t events = l.row_count("journal_events");
    const std::size_t intents = l.row_count("command_intents");
    CHECK(l.row_count("inbox_receipts") > 0);

    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK_MSG(!r.refused, r.refusal_reason);
    CHECK(r.receipts_pruned > 0);
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(0));
    // The journal and the idempotency keys are untouched.
    CHECK_EQ(l.row_count("journal_events"), events);
    CHECK_EQ(l.row_count("command_intents"), intents);
    expect_consistent(l, "G19-05");
}

SCENARIO("G19-06", "P0", "command intents are never pruned") {
    // Section 18: a client replaying an old command id after the key is
    // gone would place the order twice.
    Lab l("g19-06");
    settle(l);
    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK(!r.refused);
    CHECK(l.row_count("command_intents") > 0);

    bool said_so = false;
    for (const std::string& s : r.kept_because) {
        if (s.find("command_intents") != std::string::npos) said_so = true;
    }
    CHECK(said_so);

    // And the key still protects against a replay.
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 50000), l.token()).kind,
             CommitKind::Replayed);
    expect_consistent(l, "G19-06");
}

SCENARIO("G19-07", "P0", "a recent receipt inside the horizon is kept") {
    Lab l("g19-07");
    settle(l);
    // A second, fresh receipt that is younger than the horizon.
    l.service().submit_new(l.new_order("K2", "O2", 10000), l.token());
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    l.deliver(l.fill("O2", "E2", 10000, 10000, "K2"));

    RetentionPolicy p = lenient();
    p.horizon_ms = 7LL * 24 * 3600 * 1000;
    p.resend_window_ms = 7LL * 24 * 3600 * 1000;
    p.command_key_horizon_ms = 365LL * 24 * 3600 * 1000;

    const RetentionResult r = prune(l.store(), Lab::kTenant, p);
    CHECK_MSG(!r.refused, r.refusal_reason);
    // The old one went, the recent one stayed.
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(1));
    expect_consistent(l, "G19-07");
}

SCENARIO("G19-08", "P0", "execution keys outlive receipts") {
    // A counterparty can re-report an old trade long after the transport
    // receipt is gone; only the business key stops it becoming a new fill.
    Lab l("g19-08");
    settle(l);
    RetentionPolicy p = lenient();
    p.horizon_ms = 0;
    p.command_key_horizon_ms = 365LL * 24 * 3600 * 1000;  // keys kept far longer

    const RetentionResult r = prune(l.store(), Lab::kTenant, p);
    CHECK(!r.refused);
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(0));
    CHECK(l.row_count("business_exec_keys") > 0);

    // A re-report with a new transport identity is still deduped.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 50000, 10000, "K1")).kind, CommitKind::Replayed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(50000));
    expect_consistent(l, "G19-08");
}

SCENARIO("G19-09", "P0", "a superseded snapshot may go, the newest verified one stays") {
    Lab l("g19-09");
    settle(l);
    Version first = 0, second = 0;
    std::string why;
    CHECK_EQ(l.snapshots().checkpoint(Lab::kTenant, risk_stream(Lab::kDomain), &first, &why),
             StoreError::None);
    l.service().set_risk_limit(Lab::kTenant, Lab::kDomain, 2000000, l.token());
    CHECK_EQ(l.snapshots().checkpoint(Lab::kTenant, risk_stream(Lab::kDomain), &second, &why),
             StoreError::None);
    CHECK(second > first);

    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK_MSG(!r.refused, r.refusal_reason);
    SnapshotRecord keep;
    CHECK(l.snapshots().newest_usable(Lab::kTenant, risk_stream(Lab::kDomain), &keep));
    CHECK_EQ(keep.version, second);
    expect_consistent(l, "G19-09");
}

SCENARIO("G19-10", "P0", "pruning leaves the store verifying") {
    Lab l("g19-10");
    settle(l);
    const RetentionResult r = prune(l.store(), Lab::kTenant, lenient());
    CHECK(!r.refused);
    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK_MSG(v.ok(), v.details.empty() ? std::string() : v.details.front());
    // And replay still reproduces every projection.
    for (const StreamId& s : l.store().list_streams(Lab::kTenant)) {
        CHECK(replay_from_empty(l.store(), Lab::kTenant, s).ok);
    }
}

SCENARIO("G19-11", "P0", "the audit trail is not pruned") {
    Lab l("g19-11");
    settle(l);
    const std::size_t audit = l.store().audit_count(Lab::kTenant);
    CHECK(audit > 0);
    prune(l.store(), Lab::kTenant, lenient());
    CHECK_EQ(l.store().audit_count(Lab::kTenant), audit);
    std::string why;
    CHECK_MSG(l.store().audit_chain_intact(Lab::kTenant, &why), why);
}

SCENARIO("G19-12", "P1", "pruning an already-pruned store is a no-op") {
    Lab l("g19-12");
    settle(l);
    prune(l.store(), Lab::kTenant, lenient());
    const RetentionResult again = prune(l.store(), Lab::kTenant, lenient());
    CHECK(!again.refused);
    CHECK_EQ(again.receipts_pruned, std::size_t(0));
    expect_consistent(l, "G19-12");
}

SCENARIO_BLOCKED("G19-13", "P1", "partitioning the journal by tenant and month",
                 "partitioning is a PostgreSQL physical layout decision that section 8 defers "
                 "to migrations. It changes no behaviour this suite can observe, and claiming "
                 "a pass for it here would be claiming something untested.")

SCENARIO_BLOCKED("G19-14", "P0", "archiving to cold storage outside the writer's trust domain",
                 "section 18 requires the archive to live where the database writer cannot "
                 "reach it. That is a deployment property, not a code path; the archive "
                 "format and its verification are covered by G16.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_retention"); }
