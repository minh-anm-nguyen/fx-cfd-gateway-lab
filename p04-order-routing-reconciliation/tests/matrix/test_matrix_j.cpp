// Section J: inbound ExecutionReport and OrderCancelReject validation.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

namespace {

struct Working {
    Lab lab;
    AcceptedIntent order;
    Working() {
        order = lab.must_submit_new("K1");
        lab.drive();
    }
};

}  // namespace

SCENARIO("OR-J-01", "P0", "a valid New acknowledgement normalises with no trade event") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.inject(L.lp->new_ack(a.cl_ord_id.str()));

    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->status, OrdStatus::New);
    CHECK_EQ(c->fills.size(), std::size_t{0});
    CHECK(c->lp_acked);
    CHECK(c->lp_order_id.has_value());
    CHECK_INV(L);
}

SCENARIO("OR-J-02", "P0", "a valid partial Trade is applied exactly once") {
    Working w;
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    s.ord_status = OrdStatus::PartiallyFilled;
    w.lab.inject(s);
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->fills.at(0).px, 108500);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-03", "P0", "a valid filling Trade reaches a terminal state") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 60000, 108520);
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK(c->terminal());
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-04", "P0", "a Pending Cancel does not erase the quantity already filled") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->status, OrdStatus::PendingCancel);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_NE(c->status, OrdStatus::Canceled);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-05", "P0", "the event is taken from ExecType, not assumed from OrdStatus") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    // ExecType=5 Replaced, while OrdStatus still reports the pending state. The
    // reducer must act on the ExecType and not refuse because 39 disagrees.
    p02::ReportSpec s =
        w.lab.lp->replaced(r.cl_ord_id.str(), w.order.cl_ord_id.str(), 80000, 108490);
    s.ord_status = OrdStatus::PendingReplace;
    w.lab.inject(s);

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 80000);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-06", "P0", "a status report is a summary and nothing more") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    const Hash before = w.lab.chain(w.order.chain_id)->state_hash();

    p02::ReportSpec s = w.lab.lp->status_snapshot(w.order.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::PartiallyFilled;
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    w.lab.inject(s);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->state_hash().hex(), before.hex());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-07", "P0", "a valid cancel reject closes the request and keeps the order") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    w.lab.inject(w.lab.lp->cancel_reject(c.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelRequest, 0));
    const p03::ChainState* chain = w.lab.chain(w.order.chain_id);
    CHECK(!chain->pending_cancel.has_value());
    CHECK_EQ(chain->status, OrdStatus::New);
    CHECK_EQ(chain->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-08", "P0", "a valid replace reject closes the request and keeps the version") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    w.lab.inject(w.lab.lp->cancel_reject(r.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelReplaceRequest, 2));
    const p03::ChainState* chain = w.lab.chain(w.order.chain_id);
    CHECK(!chain->pending_replace.has_value());
    CHECK_EQ(chain->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(chain->accepted_qty, 100000);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-09", "P0", "a session reject reaching the application layer is an incident") {
    Working w;
    p02::ReportSpec s;
    s.raw[35] = "3";  // session level Reject
    s.cl_ord_id = w.order.cl_ord_id.str();
    w.lab.inject(s);

    // It is not normalised into a business rejection.
    CHECK_NE(w.lab.chain(w.order.chain_id)->status, OrdStatus::Rejected);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK(!w.lab.rt->alerts().empty());
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-10", "P0", "a Trade with no ExecID can never be applied") {
    Working w;
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.omit.insert(17);
    w.lab.inject(s);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 0);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-11", "P0", "a Trade for an unknown order is quarantined with its provenance") {
    Working w;
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX9", 40000, 108500);
    s.cl_ord_id = "O-9999";
    s.orig_cl_ord_id.clear();
    const std::string frame = s.encode();
    w.lab.inject(s);

    CHECK_EQ(w.lab.quarantine_depth(), std::size_t{1});
    const QuarantineEntry& q = w.lab.disk.quarantine.at(0);
    CHECK_EQ(q.reason, HoldReason::OrphanReport);
    CHECK(q.raw_hash == sha256(frame));
    CHECK(!q.redacted_frame.empty());
    CHECK(q.receipt_id != 0);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 0);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-12", "P0", "a trade cancellation is quarantined, never subtracted") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EXH1", 40000, 108500);
    s.exec_type = ExecType::TradeCancel;
    s.exec_ref_id = "EX-1";
    w.lab.inject(s);

    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 40000);  // untouched
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    bool found = false;
    for (const QuarantineEntry& q : w.lab.disk.quarantine)
        if (q.reason == HoldReason::UnsupportedEvent) found = true;
    CHECK(found);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-13", "P0", "a trade correction is quarantined without any correction maths") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EXG1", 50000, 108600);
    s.exec_type = ExecType::TradeCorrect;
    s.exec_ref_id = "EX-1";
    w.lab.inject(s);

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.at(0).qty, 40000);
    CHECK_EQ(c->fills.at(0).px, 108500);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-14", "P0", "a restatement is quarantined until the profile supports it") {
    Working w;
    p02::ReportSpec s = w.lab.lp->new_ack(w.order.cl_ord_id.str());
    s.exec_type = ExecType::Restated;
    s.order_qty = 50000;  // the broker changed the order behind our back
    w.lab.inject(s);

    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_qty, 100000);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-15", "P0", "a report naming another account is a security event") {
    Working w;
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.account = "ACC2";
    w.lab.inject(s);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 0);
    bool security = false;
    for (const QuarantineEntry& q : w.lab.disk.quarantine)
        if (q.reason == HoldReason::SecurityQuarantine) security = true;
    CHECK(security);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-16", "P0", "a report on the wrong side is a semantic conflict") {
    Working w;
    p02::ReportSpec s = w.lab.lp->new_ack(w.order.cl_ord_id.str());
    s.exec_id = "EX-WRONG-SIDE";  // a fresh execution identity, so this is not a replay
    s.side = Side::Sell;
    w.lab.inject(s);
    CHECK(w.lab.chain(w.order.chain_id)->hold.has_value());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->hold.value(), HoldReason::SemanticConflict);
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-17", "P0", "a report on the wrong instrument is a semantic conflict") {
    Working w;
    p02::ReportSpec s = w.lab.lp->new_ack(w.order.cl_ord_id.str());
    s.exec_id = "EX-WRONG-SYMBOL";
    s.symbol = "USD/JPY";
    s.price_scale = 3;  // quote the prices the way that symbol's profile does
    w.lab.inject(s);
    CHECK(w.lab.chain(w.order.chain_id)->hold.has_value());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->hold.value(), HoldReason::SemanticConflict);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->wire_symbol, std::string("EUR/USD"));
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-18", "P0", "an impossible quantity relation on a working order is refused") {
    Working w;
    p02::ReportSpec s = w.lab.lp->new_ack(w.order.cl_ord_id.str());
    s.raw[14] = "50000";   // CumQty
    s.raw[151] = "90000";  // LeavesQty, with OrderQty 100000
    s.order_qty = 100000;
    w.lab.inject(s);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 0);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-J-19", "P1", "a New acknowledgement without OrigClOrdID is perfectly valid") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->new_ack(a.cl_ord_id.str());
    CHECK(s.orig_cl_ord_id.empty());
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-J-20", "P1", "unknown fields follow the profile policy and never crash") {
    Working w;
    // Outside the tolerated list: refused as a must-understand violation.
    p02::ReportSpec s = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.extra.push_back({9001, "x"});
    s.extra.push_back({9002, "y"});
    w.lab.inject(s);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 0);
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);

    // Inside the list: accepted, and the state moves normally.
    w.lab.cfg.order_profile.tolerated_custom_tags = {9001, 9002};
    w.lab.cfg.finalize();
    p02::ReportSpec ok = w.lab.lp->trade(w.order.cl_ord_id.str(), "EX2", 40000, 108500);
    ok.extra.push_back({9001, "x"});
    ok.extra.push_back({9002, "y"});
    w.lab.inject(ok);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 40000);
    CHECK_INV(w.lab);
}
