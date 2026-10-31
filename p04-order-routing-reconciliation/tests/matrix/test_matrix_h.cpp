// Section H: cancel flow, pending behaviour and races.
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

SCENARIO("OR-H-01", "P0", "an order is only Canceled once the LP confirms it") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    // Pending cancel is not a cancellation.
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::PendingCancel);
    CHECK_INV(w.lab);

    w.lab.lp->accept_cancel(c.cl_ord_id.str());
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->leaves_qty, 0);
    CHECK_EQ(w.lab.intent("C1")->business, BusinessOutcome::Canceled);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-02", "P0", "a pending cancel acknowledgement is reported as pending, not done") {
    Working w;
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->status, OrdStatus::PendingCancel);
    CHECK_NE(c->status, OrdStatus::Canceled);
    CHECK_EQ(c->leaves_qty, 100000);  // still working
    CHECK(c->pending_cancel.has_value());
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-03", "P0", "a cancel rejected because the order filled leaves it Filled") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 100000, 108500);
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Filled);

    w.lab.inject(w.lab.lp->cancel_reject(c.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelRequest, 0));
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Filled);
    CHECK(!w.lab.chain(w.order.chain_id)->pending_cancel.has_value());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 100000);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-04", "P0", "a cancel rejected as unknown order does not change the order state") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    p02::ReportSpec s = w.lab.lp->cancel_reject(c.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                                CxlRejResponseTo::OrderCancelRequest, 1);
    s.order_id = "NONE";
    w.lab.inject(s);

    const p03::ChainState* chain = w.lab.chain(w.order.chain_id);
    CHECK_NE(chain->status, OrdStatus::Canceled);
    CHECK_NE(chain->status, OrdStatus::Rejected);
    CHECK(!chain->pending_cancel.has_value());
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-05", "P0", "a cancel reject with no 434 leaves the request unresolved") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    p02::ReportSpec s = w.lab.lp->cancel_reject(c.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                                CxlRejResponseTo::OrderCancelRequest, 0);
    s.omit.insert(434);
    w.lab.inject(s);
    CHECK(w.lab.chain(w.order.chain_id)->pending_cancel.has_value());
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-06", "P0", "a fill while the cancel is pending is applied once") {
    Working w;
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::PendingCancel);

    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    // The cancel is still pending; the fill resolved it neither way.
    CHECK_EQ(c->status, OrdStatus::PendingCancel);
    CHECK(c->pending_cancel.has_value());
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-07", "P0", "an order that fills while cancelling ends Filled, never Canceled") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 100000, 108500);
    w.lab.drive();
    w.lab.inject(w.lab.lp->cancel_reject(c.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelRequest, 0));
    const p03::ChainState* chain = w.lab.chain(w.order.chain_id);
    CHECK_EQ(chain->status, OrdStatus::Filled);
    CHECK_EQ(chain->cum_qty, 100000);
    CHECK_EQ(chain->fills.size(), std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-08", "P0", "a cancel confirmation before the pending ack does not regress") {
    Working w;
    w.lab.lp_disk.auto_pending_cancel = false;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();

    w.lab.inject(w.lab.lp->canceled(c.cl_ord_id.str(), w.order.cl_ord_id.str()));
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);

    // The late pending report must not pull the order back out of terminal.
    w.lab.inject(w.lab.lp->pending_cancel(c.cl_ord_id.str(), w.order.cl_ord_id.str()));
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-09", "P0", "a hundred retries of one cancel never change exposure twice") {
    Working w;
    const auto c = w.lab.must_submit_cancel("C1", w.order.chain_id);
    for (int i = 0; i < 100; ++i) {
        auto again = w.lab.submit(w.lab.cancel_order("C1", w.order.chain_id));
        CHECK(again.is_ok());
        CHECK_EQ(again.value().cl_ord_id.str(), c.cl_ord_id.str());
    }
    w.lab.drive();
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{1});
    CHECK_EQ(w.lab.disk.cl_ord_index.size(), std::size_t{2});  // the order and the cancel
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-10", "P0", "a second cancel while one is pending is refused") {
    Working w;
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    auto second = w.lab.submit(w.lab.cancel_order("C2", w.order.chain_id));
    CHECK(second.is_err());
    CHECK_EQ(second.error(), RoutingError::PendingConflict);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-11", "P0", "a cancel after an accepted replace names the new version") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    w.lab.lp->accept_replace(r.cl_ord_id.str());
    w.lab.drive();

    w.lab.must_submit_cancel("C2", w.order.chain_id);
    w.lab.drive();
    const std::string frame = w.lab.session_disk.wire_log.back();
    CHECK(frame.find("41=" + r.cl_ord_id.str() + "\x01") != std::string::npos);
    CHECK(frame.find("41=" + w.order.cl_ord_id.str() + "\x01") == std::string::npos);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-12", "P0", "a pending replace identifier is never used as a cancel target") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    CHECK(w.lab.chain(w.order.chain_id)->pending_replace.has_value());

    auto c = w.lab.submit(w.lab.cancel_order("C1", w.order.chain_id));
    CHECK(c.is_err());
    CHECK_EQ(c.error(), RoutingError::PendingConflict);
    // Nothing on the wire names the pending replace as an order to cancel.
    for (const std::string& f : w.lab.session_disk.wire_log)
        CHECK(!(f.rfind("35=F\x01", 0) == 0 &&
                f.find("41=" + r.cl_ord_id.str() + "\x01") != std::string::npos));
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-13", "P0", "a cancel for a filled order is refused locally") {
    Working w;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 100000, 108500);
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Filled);

    auto c = w.lab.submit(w.lab.cancel_order("C1", w.order.chain_id));
    CHECK(c.is_err());
    CHECK_EQ(c.error(), RoutingError::TerminalState);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{0});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Filled);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-14", "P0", "a cancel for an already cancelled order returns the terminal state") {
    Working w;
    const auto c1 = w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    w.lab.lp->accept_cancel(c1.cl_ord_id.str());
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);

    auto c2 = w.lab.submit(w.lab.cancel_order("C2", w.order.chain_id));
    CHECK(c2.is_err());
    CHECK_EQ(c2.error(), RoutingError::TerminalState);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-15", "P0", "a cancel queued while the session is down is clearly not confirmed") {
    Working w;
    w.lab.session->disconnect();
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.rt->dispatch_pending();

    // Durable, queued, and explicitly not a cancellation.
    CHECK_EQ(w.lab.intent("C1")->business, BusinessOutcome::Unknown);
    CHECK_NE(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{0});
    // It sits at the head of the queue by priority, ahead of any New.
    w.lab.must_submit_new("K2");
    const auto pending = w.lab.store->pending_outbox();
    CHECK(pending.size() >= std::size_t{2});
    CHECK_EQ(pending.front()->command_id.str(), std::string("C1"));
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-16", "P0", "a cancel that misses its deadline becomes unknown, not cancelled") {
    Working w;
    w.lab.session->disconnect();
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.rt->dispatch_pending();

    w.lab.advance(w.lab.cfg.limits.cancel_transport_ttl_micros + 1000);
    w.lab.rt->on_timer();

    CHECK_EQ(w.lab.intent("C1")->delivery, DeliveryState::UnknownTransport);
    CHECK_EQ(w.lab.intent("C1")->hold.value(), HoldReason::AckTimeout);
    CHECK_NE(w.lab.chain(w.order.chain_id)->status, OrdStatus::Canceled);
    CHECK(w.lab.case_for(w.order.chain_id) != nullptr);
    bool warned = false;
    for (const std::string& a : w.lab.rt->alerts())
        if (a.find("NOT confirmed") != std::string::npos) warned = true;
    CHECK(warned);
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-17", "P0", "another account cannot cancel this order") {
    Working w;
    CommandEnvelope c = w.lab.cancel_order("C1", w.order.chain_id);
    c.principal = Lab::trader("T2", "bob");
    c.account = AccountId("ACC2");
    auto r = w.lab.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::AuthzDenied);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{0});
    CHECK(!w.lab.chain(w.order.chain_id)->pending_cancel.has_value());
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-18", "P0", "a cancel reject echoing an unknown request is quarantined") {
    Working w;
    w.lab.must_submit_cancel("C1", w.order.chain_id);
    w.lab.drive();
    p02::ReportSpec s = w.lab.lp->cancel_reject("C-9999", w.order.cl_ord_id.str(),
                                                CxlRejResponseTo::OrderCancelRequest, 0);
    w.lab.inject(s);
    CHECK(w.lab.chain(w.order.chain_id)->pending_cancel.has_value());
    CHECK(w.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-19", "P0", "a fill landing between the read and the cancel forces a decision") {
    Working w;
    const StateVersion seen = w.lab.chain(w.order.chain_id)->version;
    w.lab.lp->execute(w.order.cl_ord_id.str(), 40000, 108500);
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 40000);

    CommandEnvelope c = w.lab.cancel_order("C1", w.order.chain_id);
    c.expected_version = seen;
    auto r = w.lab.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::StaleVersion);
    // The fill is intact, and a cancel against the current version is accepted.
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 40000);
    CommandEnvelope c2 = w.lab.cancel_order("C2", w.order.chain_id);
    c2.expected_version = w.lab.chain(w.order.chain_id)->version;
    CHECK(w.lab.submit(c2).is_ok());
    CHECK_INV(w.lab);
}

SCENARIO("OR-H-20", "P1", "cancels are scheduled ahead of a backlog of new orders") {
    Lab L;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 300; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);

    // Now stop the session and queue a mix of cancels and new orders.
    L.session->disconnect();
    for (int i = 0; i < 20; ++i)
        L.must_submit_cancel("C" + std::to_string(i),
                             orders.at(static_cast<std::size_t>(i)).chain_id);
    for (int i = 300; i < 320; ++i) L.must_submit_new("K" + std::to_string(i));

    const auto pending = L.store->pending_outbox();
    CHECK(pending.size() >= std::size_t{40});
    std::size_t first_new = pending.size();
    std::size_t last_cancel = 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const CommandIntent* in = L.store->find_intent(pending[i]->tenant, pending[i]->command_id);
        if (in == nullptr) continue;
        if (in->kind == CommandKind::Cancel) last_cancel = i;
        if (in->kind == CommandKind::New && i < first_new) first_new = i;
    }
    CHECK(last_cancel < first_new);
    CHECK_INV(L);
}
