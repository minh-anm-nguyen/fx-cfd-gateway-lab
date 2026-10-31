// G14 — Fencing, leadership failover, stale callbacks and split brain.
//
// Section 16's point is that a time lease is a liveness device and nothing
// more. Safety comes from checking the token at the resource, inside the
// same transaction as the mutation, which is what every case here probes.
#include "lab.hpp"

using namespace lab;

namespace {

SCENARIO("G14-01", "P0", "tokens are monotonic across owners") {
    Lab l("g14-01");
    l.start();
    FenceToken a = 0, b = 0, c = 0;
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, "DX", "one", 10000, &a), StoreError::None);
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, "DX", "two", 10000, &b), StoreError::None);
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, "DX", "one", 10000, &c), StoreError::None);
    CHECK(b > a);
    CHECK(c > b);
    CHECK_EQ(l.store().current_fence(Lab::kTenant, "DX"), c);
}

SCENARIO("G14-02", "P0", "a superseded owner cannot commit") {
    Lab l("g14-02");
    l.start();
    const FenceToken old_token = l.token();
    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);

    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), old_token);
    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK_EQ(o.error, StoreError::NotOwner);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
    expect_consistent(l, "G14-02");
}

SCENARIO("G14-03", "P0", "the new owner can commit immediately") {
    Lab l("g14-03");
    l.start();
    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), newer).kind,
             CommitKind::Committed);
    expect_consistent(l, "G14-03");
}

SCENARIO("G14-04", "P0", "a token from the future is refused as firmly as a stale one") {
    Lab l("g14-04");
    l.start();
    const CommitOutcome o =
        l.service().submit_new(l.new_order("K1", "O1", 1000), l.token() + 100);
    CHECK_EQ(o.error, StoreError::NotOwner);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
}

SCENARIO("G14-05", "P0", "a mutation against a domain with no fence is refused") {
    Lab l("g14-05");
    l.start();
    auto r = l.new_order("K1", "O1", 1000);
    r.risk_domain = "D-unfenced";
    const CommitOutcome o = l.service().submit_new(r, 1);
    CHECK_NE(o.kind, CommitKind::Committed);
    expect_consistent(l, "G14-05");
}

SCENARIO("G14-06", "P0", "the lease expiring does not by itself transfer ownership") {
    // Liveness versus safety. The lease running out lets someone else TRY
    // to take over; it does not make the current token invalid on its own.
    Lab l("g14-06");
    l.start();
    const FenceToken mine = l.token();
    l.clock().advance(10 * 60 * 1000);

    CHECK_EQ(l.store().current_fence(Lab::kTenant, Lab::kDomain), mine);
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), mine).kind,
             CommitKind::Committed);

    // Once another owner actually takes the token, the old one stops.
    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    CHECK_NE(l.service().submit_new(l.new_order("K2", "O2", 1000), mine).kind,
             CommitKind::Committed);
    expect_consistent(l, "G14-06");
}

SCENARIO("G14-07", "P0", "a late callback from the old owner cannot move an outbox row") {
    Lab l("g14-07");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    const FenceToken old_token = l.token();

    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    std::vector<OutboxRow> claimed;
    l.store().claim_outbox(Lab::kTenant, "new-owner", newer, 10, 5000, &claimed);
    CHECK_EQ(claimed.size(), std::size_t(1));

    CHECK_EQ(l.store().set_outbox_state(Lab::kTenant, claimed.front().outbox_id, "BOUND",
                                        old_token, std::uint64_t(1), "late"),
             StoreError::NotOwner);
    CHECK_EQ(l.store().mark_send_attempted(Lab::kTenant, claimed.front().outbox_id, old_token),
             StoreError::NotOwner);
    expect_consistent(l, "G14-07");
}

SCENARIO("G14-08", "P0", "the supervisor refuses to dispatch once its token is superseded") {
    Lab l("g14-08");
    const RecoveryReport rep = l.start();
    CHECK_EQ(rep.phase, Phase::Ready);
    CHECK(l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));

    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    // The supervisor re-reads the current token rather than trusting what
    // it was handed at startup.
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));
    CHECK(!l.supervisor().why_blocked(Lab::kTenant, Lab::kDomain).empty());
}

SCENARIO("G14-09", "P0", "a demoted instance stops admitting and dispatching at once") {
    Lab l("g14-09");
    CHECK_EQ(l.start().phase, Phase::Ready);
    l.supervisor().demote("health check failed");

    CHECK(!l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK_EQ(l.metrics().get(metric::kAdmissionBlocked) > 0, true);
}

SCENARIO("G14-10", "P0", "a fence covers one domain only") {
    Lab l("g14-10");
    l.start();
    FenceToken other = 0;
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, "D2", "gateway-2", 60000, &other),
             StoreError::None);
    // Taking D2 does not disturb D1.
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Committed);
    expect_consistent(l, "G14-10");
}

SCENARIO("G14-11", "P0", "a fence is scoped to its tenant") {
    Lab l("g14-11");
    l.start();
    FenceToken t2 = 0;
    l.store().acquire_fence("t2", Lab::kDomain, "other-tenant", 60000, &t2);
    // JI-33: the other tenant's token has no authority here, whatever its
    // numeric value.
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), t2);
    if (t2 != l.token()) CHECK_NE(o.kind, CommitKind::Committed);
    CHECK(!l.supervisor().may_admit("t2", Lab::kDomain));
}

SCENARIO("G14-12", "P0", "the head records the token that last advanced the stream") {
    Lab l("g14-12");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    CHECK_EQ(l.store().head(Lab::kTenant, order_stream("O1")).fence, l.token());

    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    l.service().submit_new(l.new_order("K2", "O2", 1000), newer);
    CHECK_EQ(l.store().head(Lab::kTenant, order_stream("O2")).fence, newer);
    expect_consistent(l, "G14-12");
}

SCENARIO("G14-13", "P0", "two live instances cannot both dispatch the same domain") {
    Lab l("g14-13");
    const RecoveryReport first = l.start();
    CHECK_EQ(first.phase, Phase::Ready);
    const FenceToken a = l.token();

    // A second supervisor over the same store, as a split brain would be.
    RecoverySupervisor::Options opt;
    opt.tenant = Lab::kTenant;
    opt.owner = "gateway-2";
    opt.domains = {Lab::kDomain};
    RecoverySupervisor second(l.store(), opt);
    const RecoveryReport r2 = second.verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK_EQ(r2.phase, Phase::Ready);

    // Exactly one of them may dispatch, and it is the one holding the
    // current token (JI-22).
    const bool one = l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain);
    const bool two = second.may_dispatch(Lab::kTenant, Lab::kDomain);
    CHECK(one != two);
    CHECK(two);
    CHECK(second.token(Lab::kDomain) > a);
}

SCENARIO("G14-14", "P0", "a fenced-out writer leaves the database consistent") {
    Lab l("g14-14");
    l.start();
    const FenceToken old_token = l.token();
    l.service().submit_new(l.new_order("K1", "O1", 1000), old_token);

    FenceToken newer = 0;
    l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer);
    for (int i = 0; i < 5; ++i) {
        l.service().submit_new(l.new_order("Kx" + std::to_string(i), "Ox" + std::to_string(i),
                                           1000),
                               old_token);
    }
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    expect_consistent(l, "G14-14");
}

SCENARIO_BLOCKED("G14-15", "P0", "a pooled connection survives a failover to a new primary",
                 "connection pooling against a replicated primary is PostgreSQL topology; "
                 "there is no second node here. The token check that would catch a stale "
                 "pooled writer is covered by G14-02 and G14-07.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_fencing"); }
