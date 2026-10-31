// G10 — Cancel, replace, fill races and journal state fidelity.
//
// These are the interleavings where an order's quantity accounting is
// easiest to get wrong: a fill that lands while a cancel is in flight, a
// replace that would shrink an order below what has already traded, and a
// terminal state arriving twice.
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, std::int64_t qty = 100000) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", qty), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G10-01", "P0", "a fill arriving during a pending cancel is applied") {
    Lab l("g10-01");
    seed(l);
    CHECK_EQ(l.service().submit_cancel(l.cancel("K2", "O1"), l.token()).kind,
             CommitKind::Committed);
    CHECK_EQ(l.order("O1").effective_status(), OrdStatus::PendingCancel);

    CHECK_EQ(l.deliver(l.fill("O1", "E1", 30000, 10000, "K1")).kind, CommitKind::Committed);
    const OrderState o = l.order("O1");
    CHECK_EQ(o.cum_qty, std::int64_t(30000));
    CHECK_EQ(o.leaves_qty, std::int64_t(70000));
    // Still pending: the fill does not answer the cancel.
    CHECK(o.pending_cancel);
    expect_consistent(l, "G10-01");
}

SCENARIO("G10-02", "P0", "a cancel after a partial fill keeps the filled quantity") {
    Lab l("g10-02");
    seed(l);
    l.service().submit_cancel(l.cancel("K2", "O1"), l.token());
    l.deliver(l.fill("O1", "E1", 30000, 10000, "K1"));
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K2")).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK_EQ(o.status, OrdStatus::Canceled);
    CHECK_EQ(o.cum_qty, std::int64_t(30000));
    CHECK_EQ(o.leaves_qty, std::int64_t(0));
    CHECK(!o.pending_cancel);
    CHECK_EQ(l.risk().exposure, std::int64_t(30000));
    CHECK_EQ(l.risk().reserved, std::int64_t(0));
    expect_consistent(l, "G10-02");
}

SCENARIO("G10-03", "P0", "a cancel that loses the race to a full fill leaves Filled") {
    Lab l("g10-03");
    seed(l, 50000);
    l.service().submit_cancel(l.cancel("K2", "O1"), l.token());
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 50000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);

    // The counterparty's cancel confirmation arrives afterwards. The order
    // traded in full; the engine must not rewrite that as Canceled.
    const CommitOutcome o = l.deliver(l.terminal("O1", "CANCELED", "K2"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(50000));
    expect_consistent(l, "G10-03");
}

SCENARIO("G10-04", "P0", "a replace below the traded quantity is refused up front") {
    Lab l("g10-04");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 60000, 10000, "K1"));

    const CommitOutcome o = l.service().submit_replace(l.replace("K2", "O1", 50000), l.token());
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK(!l.order("O1").pending_replace);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(100000));
    expect_consistent(l, "G10-04");
}

SCENARIO("G10-05", "P0", "an accepted replace re-strikes the hold to the new remainder") {
    Lab l("g10-05");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    CHECK_EQ(l.risk().reserved, std::int64_t(80000));

    CHECK_EQ(l.service().submit_replace(l.replace("K2", "O1", 60000), l.token()).kind,
             CommitKind::Committed);
    CHECK_EQ(l.deliver(l.terminal("O1", "REPLACED", "K2")).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK_EQ(o.order_qty, std::int64_t(60000));
    CHECK_EQ(o.cum_qty, std::int64_t(20000));
    CHECK_EQ(o.leaves_qty, std::int64_t(40000));
    CHECK_EQ(o.cl_ord_id, std::string("K2-cl"));
    CHECK_EQ(o.orig_cl_ord_id, std::string("K1-cl"));

    const RiskDomainState r = l.risk();
    CHECK_EQ(r.reserved, std::int64_t(40000));
    CHECK_EQ(r.exposure, std::int64_t(20000));
    // The domain is never momentarily over-committed: release and reserve
    // happen in the same transaction.
    CHECK_EQ(r.utilised(), std::int64_t(60000));
    expect_consistent(l, "G10-05");
}

SCENARIO("G10-06", "P0", "a fill between the replace request and its acceptance is kept") {
    Lab l("g10-06");
    seed(l, 100000);
    l.service().submit_replace(l.replace("K2", "O1", 80000), l.token());
    CHECK(l.order("O1").pending_replace);

    // The old order traded while the replace was in flight.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 30000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.terminal("O1", "REPLACED", "K2")).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK_EQ(o.cum_qty, std::int64_t(30000));
    CHECK_EQ(o.order_qty, std::int64_t(80000));
    CHECK_EQ(o.leaves_qty, std::int64_t(50000));
    CHECK_EQ(l.risk().utilised(), std::int64_t(80000));
    expect_consistent(l, "G10-06");
}

SCENARIO("G10-07", "P0", "a replace made impossible by an intervening fill is quarantined") {
    Lab l("g10-07");
    seed(l, 100000);
    l.service().submit_replace(l.replace("K2", "O1", 40000), l.token());
    // The order trades past the replacement quantity before the
    // counterparty confirms. Shrinking now would make leaves negative.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 70000, 10000, "K1")).kind, CommitKind::Committed);

    const CommitOutcome o = l.deliver(l.terminal("O1", "REPLACED", "K2"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(100000));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(70000));
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));
    expect_consistent(l, "G10-07");
}

SCENARIO("G10-08", "P0", "a rejected replace restores the previous working order") {
    Lab l("g10-08");
    seed(l, 100000);
    l.service().submit_replace(l.replace("K2", "O1", 60000), l.token());

    const Version base = l.store().head(Lab::kTenant, order_stream("O1")).version;
    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-replace-reject";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O1");
    e.expected_version = base;
    e.event_id = "e-rr";
    e.event_type = std::string(ev::kReplaceRejected);
    e.reducer_version = std::string(reducer_version::kOrderV1);
    e.ordinal = 0;
    e.payload = Payload().s("reason", "TOO_LATE").build();
    p.events.push_back(std::move(e));
    CHECK_EQ(l.store().apply(p).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK(!o.pending_replace);
    CHECK_EQ(o.order_qty, std::int64_t(100000));
    CHECK_EQ(o.status, OrdStatus::New);
    CHECK_EQ(l.risk().reserved, std::int64_t(100000));
    expect_consistent(l, "G10-08");
}

SCENARIO("G10-09", "P0", "two cancels for one order cannot both be pending") {
    Lab l("g10-09");
    seed(l);
    CHECK_EQ(l.service().submit_cancel(l.cancel("K2", "O1"), l.token()).kind,
             CommitKind::Committed);
    // A second, different cancel command for the same order.
    const CommitOutcome o = l.service().submit_cancel(l.cancel("K3", "O1"), l.token());
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").pending_cl_ord_id, std::string("K2-cl"));
    expect_consistent(l, "G10-09");
}

SCENARIO("G10-10", "P0", "a cancel against a terminal order is refused before admission") {
    Lab l("g10-10");
    seed(l, 50000);
    l.deliver(l.fill("O1", "E1", 50000, 10000, "K1"));
    CHECK_EQ(l.order("O1").status, OrdStatus::Filled);

    // JI-38, on the admission side rather than the report side.
    const CommitOutcome o = l.service().submit_cancel(l.cancel("K2", "O1"), l.token());
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    expect_consistent(l, "G10-10");
}

SCENARIO("G10-11", "P0", "every interleaving replays to the same state") {
    Lab l("g10-11");
    seed(l, 100000);
    l.service().submit_cancel(l.cancel("K2", "O1"), l.token());
    l.deliver(l.fill("O1", "E1", 15000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 15000, 10001, "K1"));
    l.deliver(l.terminal("O1", "CANCELED", "K2"));

    const OrderState projected = l.order("O1");
    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
    const IOrderReducer* red = l.store().reducers().order(reducer_version::kOrderV1);
    CHECK_EQ(canonical_encode(red->to_canon(projected)), canonical_encode(r.state));
    expect_consistent(l, "G10-11");
}

SCENARIO("G10-12", "P0", "the lineage survives a restart in the middle of a replace") {
    Lab l("g10-12");
    seed(l, 100000);
    l.service().submit_replace(l.replace("K2", "O1", 70000), l.token());

    l.restart();
    l.start();
    CHECK(l.order("O1").pending_replace);
    CHECK_EQ(l.order("O1").pending_qty, std::int64_t(70000));

    CHECK_EQ(l.deliver(l.terminal("O1", "REPLACED", "K2")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(70000));
    CHECK_EQ(l.order("O1").orig_cl_ord_id, std::string("K1-cl"));
    expect_consistent(l, "G10-12");
}

SCENARIO("G10-13", "P0", "an expiry is terminal and releases the hold") {
    Lab l("g10-13");
    seed(l, 100000);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    CHECK_EQ(l.deliver(l.terminal("O1", "EXPIRED", "K1")).kind, CommitKind::Committed);

    CHECK_EQ(l.order("O1").status, OrdStatus::Expired);
    CHECK_EQ(l.order("O1").leaves_qty, std::int64_t(0));
    CHECK_EQ(l.risk().reserved, std::int64_t(0));
    CHECK_EQ(l.risk().exposure, std::int64_t(10000));
    expect_consistent(l, "G10-13");
}

SCENARIO("G10-14", "P0", "a second terminal report is quarantined, not applied") {
    Lab l("g10-14");
    seed(l, 50000);
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K1")).kind, CommitKind::Committed);
    const RiskDomainState after_first = l.risk();

    const CommitOutcome o = l.deliver(l.terminal("O1", "CANCELED", "K1"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK(l.risk() == after_first);
    expect_consistent(l, "G10-14");
}

SCENARIO_BLOCKED("G10-15", "P0", "a trade bust arrives after the chain was replaced",
                 "trade correction and bust semantics are listed in section 30 as needing a "
                 "real Rules of Engagement. The correction mechanism itself is exercised by "
                 "G08-12; what it should mean after a replace is not ours to assume.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_cancel_replace"); }
