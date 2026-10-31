// Section N: OrderStatusRequest (35=H) and the data completeness policy.
//
// Every case here turns on one idea from design doc 14.2: a status snapshot is
// the peer's summary, and a summary is not a fill history. The router may use
// it to confirm existence and to detect disagreement; it may never use it to
// write a trade.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

namespace {

// A chain whose outcome is in doubt, which is the only situation that may
// produce a status query at all.
struct Unknown {
    Lab lab;
    AcceptedIntent order;
    explicit Unknown(bool enable_h, bool echo = true) {
        if (enable_h) lab.enable_status_queries(echo);
        lab.lp_disk.suppress_reports = true;
        order = lab.must_submit_new("K1");
        lab.drive();
    }
    void let_the_budget_expire(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) lab.tick(lab.cfg.limits.ack_timeout_micros * 3);
    }
};

}  // namespace

SCENARIO("OR-N-01", "P0", "with the capability off the router emits no 35=H at all") {
    Unknown u(false);
    u.let_the_budget_expire(4);
    CHECK_EQ(u.lab.wire_count('H'), std::size_t{0});
    CHECK_EQ(u.lab.lp_disk.status_requests_seen, std::size_t{0});
    CHECK_EQ(u.lab.rt->status_requests_emitted(), std::size_t{0});
    CHECK_EQ(u.lab.case_for(u.order.chain_id)->state, ReconState::ManualHold);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-02", "P0", "an enabled status request carries the required fields") {
    Unknown u(true);
    u.lab.lp_disk.suppress_reports = false;
    u.let_the_budget_expire();
    CHECK(u.lab.wire_count('H') >= std::size_t{1});

    std::string probe;
    for (const std::string& f : u.lab.session_disk.wire_log)
        if (f.rfind("35=H\x01", 0) == 0) probe = f;
    CHECK(!probe.empty());
    CHECK(probe.find("11=" + u.order.cl_ord_id.str() + "\x01") != std::string::npos);
    CHECK(probe.find("55=EUR/USD\x01") != std::string::npos);
    CHECK(probe.find("54=1\x01") != std::string::npos);
    CHECK(probe.find("790=") != std::string::npos);  // durable query id, echoed profile
    CHECK(!u.lab.case_for(u.order.chain_id)->query_ids.empty());
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-03", "P0", "a status answer with ExecType I inserts nothing into the ledger") {
    Unknown u(true);
    u.lab.lp_disk.suppress_reports = false;
    u.let_the_budget_expire();

    const p03::ChainState* c = u.lab.chain(u.order.chain_id);
    CHECK_EQ(c->fills.size(), std::size_t{0});
    CHECK_EQ(c->cum_qty, 0);
    CHECK(u.lab.disk.exec_keys.empty());
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-04", "P0", "the answer is correlated to the exact case by its query id") {
    Unknown u(true);
    u.lab.lp_disk.suppress_reports = false;
    u.let_the_budget_expire();

    const ReconCase* c = u.lab.case_for(u.order.chain_id);
    CHECK(c != nullptr);
    CHECK(!c->query_ids.empty());
    CHECK_EQ(c->state, ReconState::Reconciled);
    CHECK_EQ(c->resolved_by, std::string("status_snapshot"));
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-05", "P0", "without the echo profile an unmatched answer is not forced to fit") {
    // The peer does not echo 790, so the router correlates by chain instead and
    // never guesses which outstanding query an answer belongs to.
    Unknown u(true, /*echo=*/false);
    u.lab.lp_disk.suppress_reports = false;
    u.let_the_budget_expire();

    std::string probe;
    for (const std::string& f : u.lab.session_disk.wire_log)
        if (f.rfind("35=H\x01", 0) == 0) probe = f;
    CHECK(!probe.empty());
    CHECK(probe.find("790=") == std::string::npos);  // not sent when not agreed
    CHECK_EQ(u.lab.case_for(u.order.chain_id)->state, ReconState::Reconciled);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-06", "P0", "an answer echoing an unknown query id is not evidence") {
    Unknown u(true);
    u.lab.lp_disk.suppress_reports = false;
    p02::ReportSpec s =
        u.lab.lp->status_snapshot(u.order.cl_ord_id.str(), std::string("Q-forged"));
    s.ord_status_req_id = std::string("Q-forged");
    u.lab.inject(s);

    CHECK(u.lab.quarantine_depth() >= std::size_t{1});
    CHECK_EQ(u.lab.chain(u.order.chain_id)->cum_qty, 0);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-07", "P0", "a peer that calls the order unknown does not prove it never existed") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Rejected;
    s.order_id = "NONE";
    s.cum_qty = 0;
    s.leaves_qty = 0;
    L.inject(s);

    // Local evidence wins over the peer's denial, and the disagreement is held.
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK_INV(L);
}

SCENARIO("OR-N-08", "P0", "a snapshot saying New while we hold a fill is a contradiction") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::New;
    s.cum_qty = 0;
    s.leaves_qty = 100000;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::SnapshotMismatch);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-N-09", "P0", "a snapshot that agrees is consistent and changes nothing") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::PartiallyFilled;
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());
    CHECK(!L.chain(a.chain_id)->hold.has_value());
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-N-10", "P0", "a snapshot saying Filled against a partial ledger needs history") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Filled;
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    // No ExecID was fabricated for the missing 60000: only the acknowledgement
    // and the one real fill have keys.
    CHECK_EQ(L.disk.exec_keys.size(), std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-N-11", "P0", "a Canceled snapshot with a bigger CumQty than ours needs history") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Canceled;
    s.cum_qty = 60000;
    s.leaves_qty = 0;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    // The cancel evidence is kept alongside the gap rather than replacing it.
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    CHECK_INV(L);
}

SCENARIO("OR-N-12", "P0", "a status answer on the wrong side is quarantined") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.side = Side::Sell;
    L.inject(s);
    CHECK(L.chain(a.chain_id)->hold.has_value() || L.quarantine_depth() >= std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->side, Side::Buy);
    CHECK_INV(L);
}

SCENARIO("OR-N-13", "P0", "a status answer about another instrument updates nothing") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.symbol = "GBP/USD";
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->wire_symbol, std::string("EUR/USD"));
    CHECK(L.chain(a.chain_id)->hold.has_value() || L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-N-14", "P0", "a hundred identical snapshots are bounded and collide with nothing") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    const p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    for (int i = 0; i < 100; ++i) L.lp->send(s);
    L.rt->pump_inbound(200);

    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|0"), std::size_t{0});
    CHECK_EQ(L.session->pending_inbound(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-N-15", "P0", "a peer that rate limits the probe produces backoff then a hold") {
    Unknown u(true);
    u.lab.lp_disk.reject_status_requests = true;
    for (int i = 0; i < 20; ++i) u.lab.tick(u.lab.cfg.limits.ack_timeout_micros * 8);

    const ReconCase* c = u.lab.case_for(u.order.chain_id);
    CHECK_EQ(c->state, ReconState::ManualHold);
    CHECK(u.lab.wire_count('H') <= std::size_t{3});
    CHECK(c->attempt_count >= 1);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-16", "P0", "a probe that times out concludes nothing about the order") {
    Unknown u(true);
    u.lab.lp_disk.reject_status_requests = true;
    u.let_the_budget_expire(2);

    CHECK_EQ(u.lab.chain(u.order.chain_id)->cum_qty, 0);
    CHECK_EQ(u.lab.chain(u.order.chain_id)->status, OrdStatus::PendingNew);
    CHECK_EQ(u.lab.intent("K1")->business, BusinessOutcome::Unknown);
    CHECK_NE(u.lab.chain(u.order.chain_id)->status, OrdStatus::Rejected);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-17", "P1", "a status query about a finished order is read only") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Filled;
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);
    CHECK_INV(L);
}

SCENARIO("OR-N-18", "P1", "a status query after a session reset still correlates") {
    Unknown u(true);
    u.lab.session->reconnect(true);
    u.lab.lp_disk.suppress_reports = false;
    u.let_the_budget_expire(2);

    const ReconCase* c = u.lab.case_for(u.order.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->chain_id.str(), u.order.chain_id.str());
    CHECK(c->state == ReconState::Reconciled || c->state == ReconState::StatusQueryPending ||
          c->state == ReconState::StatusReceived);
    CHECK_INV(u.lab);
}

SCENARIO("OR-N-19", "P1", "a status answer with a different OrderID is not bound heuristically") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string bound = L.chain(a.chain_id)->lp_order_id->str();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.order_id = "LP-RESTATED";
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->lp_order_id->str(), bound);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-N-20", "P1",
         "agreeing snapshots with no history interface is not full reconciliation") {
    Lab L;
    L.enable_status_queries();
    CHECK(!L.cfg.capabilities.trade_history_query);  // no fill history exists
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // The peer's summary is ahead of us and there is no way to fetch the detail.
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 70000;
    s.leaves_qty = 30000;
    s.ord_status = OrdStatus::PartiallyFilled;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_NE(c->state, ReconState::Reconciled);
    // And it stays unresolved however long we wait, because nothing can settle it.
    for (int i = 0; i < 10; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 4);
    CHECK_NE(L.case_for(a.chain_id)->state, ReconState::Reconciled);
    CHECK_INV(L);
}
