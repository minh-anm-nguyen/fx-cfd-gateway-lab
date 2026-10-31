// Section I: replace lifecycle, version semantics and races.
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
    // Partially fills the working order, which several cases need as setup.
    void partial(std::int64_t qty, std::int64_t px = 108500) {
        lab.lp->execute(order.cl_ord_id.str(), qty, px);
        lab.drive();
    }
};

}  // namespace

SCENARIO("OR-I-01", "P0", "an accepted replace activates the new price") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "100000", "1.08490");
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_price, 108500);  // not yet
    w.lab.lp->accept_replace(r.cl_ord_id.str());
    w.lab.drive();

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(c->accepted_price, 108490);
    CHECK_EQ(c->accepted_qty, 100000);
    CHECK_EQ(w.lab.intent("R1")->business, BusinessOutcome::Replaced);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-02", "P0", "a replace that increases the size keeps the fills already done") {
    Working w;
    w.partial(40000);
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "150000", "1.08500");
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->cum_qty, 40000);

    w.lab.lp->accept_replace(r.cl_ord_id.str());
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->accepted_qty, 150000);
    CHECK_EQ(c->leaves_qty, 110000);
    CHECK_EQ(c->status, OrdStatus::PartiallyFilled);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-03", "P0", "a replace that reduces the total leaves the right remainder") {
    Working w;
    w.partial(40000);
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08500");
    w.lab.drive();
    w.lab.lp->accept_replace(r.cl_ord_id.str());
    w.lab.drive();

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->accepted_qty, 80000);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->leaves_qty, 40000);  // not 80000
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-04", "P0", "a pending replace leaves the original accepted and inactive") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->status, OrdStatus::PendingReplace);
    CHECK_EQ(c->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 100000);
    CHECK_EQ(c->accepted_price, 108500);
    CHECK_EQ(c->pending_replace->str(), r.cl_ord_id.str());
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-05", "P0", "a fill under the original while a replace is pending is applied") {
    Working w;
    w.lab.must_submit_replace("R1", w.order.chain_id, "150000", "1.08500");
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::PendingReplace);

    // The fill names the accepted version, which is still the original.
    w.lab.lp->execute(w.order.cl_ord_id.str(), 20000, 108500);
    w.lab.drive();
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 20000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(c->status, OrdStatus::PendingReplace);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-06", "P0", "a replace accepted after a fill carries the real CumQty forward") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "150000", "1.08500");
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 20000, 108500);
    w.lab.drive();
    w.lab.lp->accept_replace(r.cl_ord_id.str());
    w.lab.drive();

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 20000);  // not reset to a stale snapshot
    CHECK_EQ(c->accepted_qty, 150000);
    CHECK_EQ(c->leaves_qty, 130000);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-07", "P0", "a rejected replace leaves the original accepted") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    w.lab.inject(w.lab.lp->cancel_reject(r.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelReplaceRequest, 2));

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 100000);
    CHECK_EQ(c->accepted_price, 108500);
    CHECK(!c->pending_replace.has_value());
    CHECK_NE(c->status, OrdStatus::PendingReplace);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-08", "P0", "a replace rejected after a partial fill preserves the fill") {
    Working w;
    w.partial(40000);
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "150000", "1.08500");
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 10000, 108505);
    w.lab.drive();
    w.lab.inject(w.lab.lp->cancel_reject(r.cl_ord_id.str(), w.order.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelReplaceRequest, 2));

    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->cum_qty, 50000);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_EQ(c->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 100000);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-09", "P0", "a duplicated replace acceptance activates the version once") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    const p02::ReportSpec accept =
        w.lab.lp->replaced(r.cl_ord_id.str(), w.order.cl_ord_id.str(), 80000, 108490);
    w.lab.inject(accept);
    const Hash after_first = w.lab.chain(w.order.chain_id)->state_hash();
    const StateVersion v = w.lab.chain(w.order.chain_id)->version;

    for (int i = 0; i < 5; ++i) w.lab.inject(accept);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->state_hash().hex(), after_first.hex());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->version, v);
    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-10", "P0", "a replace acceptance after the order filled does not reopen it") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "150000", "1.08500");
    w.lab.drive();
    w.lab.lp->execute(w.order.cl_ord_id.str(), 100000, 108500);
    w.lab.drive();
    CHECK_EQ(w.lab.chain(w.order.chain_id)->status, OrdStatus::Filled);

    w.lab.inject(w.lab.lp->replaced(r.cl_ord_id.str(), w.order.cl_ord_id.str(), 150000, 108500));
    const p03::ChainState* c = w.lab.chain(w.order.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK_EQ(c->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK(c->hold.has_value());
    CHECK_EQ(c->hold.value(), HoldReason::ReplaceAfterTerminal);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-11", "P0", "a cancel while a replace is pending is refused under strict v1") {
    Working w;
    w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    auto c = w.lab.submit(w.lab.cancel_order("C1", w.order.chain_id));
    CHECK(c.is_err());
    CHECK_EQ(c.error(), RoutingError::PendingConflict);
    CHECK_EQ(w.lab.wire_count('F'), std::size_t{0});
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-12", "P0", "a replace chained on a pending replace is refused") {
    Working w;
    w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    auto second = w.lab.submit(w.lab.replace_order("R2", w.order.chain_id, "70000", "1.08480"));
    CHECK(second.is_err());
    CHECK_EQ(second.error(), RoutingError::PendingConflict);
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{1});
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-13", "P0", "a replace after an accepted replace chains correctly across a restart") {
    Working w;
    const auto r1 = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    w.lab.lp->accept_replace(r1.cl_ord_id.str());
    w.lab.drive();

    w.lab.crash();
    w.lab.boot();
    const auto r2 = w.lab.must_submit_replace("R2", w.order.chain_id, "70000", "1.08480");
    CHECK_EQ(w.lab.intent("R2")->orig_cl_ord_id->str(), r1.cl_ord_id.str());
    w.lab.drive();
    CHECK(w.lab.session_disk.wire_log.back().find("41=" + r1.cl_ord_id.str() + "\x01") !=
          std::string::npos);
    (void)r2;
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-14", "P0", "a replace below the quantity already done is refused before the wire") {
    Working w;
    w.partial(40000);
    auto r = w.lab.submit(w.lab.replace_order("R1", w.order.chain_id, "30000", "1.08500"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{0});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->leaves_qty, 60000);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-15", "P0", "a replace cannot flip the side") {
    Working w;
    CommandEnvelope c = w.lab.replace_order("R1", w.order.chain_id, "100000", "1.08490");
    c.side = Side::Sell;
    auto r = w.lab.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{0});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->side, Side::Buy);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-16", "P0", "a replace cannot change the instrument") {
    Working w;
    CommandEnvelope c = w.lab.replace_order("R1", w.order.chain_id, "100000", "1.08490");
    c.symbol = "USD/JPY";
    auto r = w.lab.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{0});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->wire_symbol, std::string("EUR/USD"));
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-17", "P0", "a replace with no quantity cannot be encoded") {
    Working w;
    CommandEnvelope c = w.lab.replace_order("R1", w.order.chain_id, "", "1.08490");
    auto r = w.lab.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{0});

    // The encoder refuses it directly too, so no caller can route around this.
    fix::ReplaceFields f;
    f.cl_ord_id = ClientOrderId("R-1");
    f.orig_cl_ord_id = ClientOrderId("O-1");
    f.account = AccountId("ACC1");
    f.wire_symbol = "EUR/USD";
    f.total_qty = 0;
    f.price_ticks = 108490;
    f.transact_time = "20261030-08:00:00.000";
    CHECK(fix::encode_replace(f).is_err());
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-18", "P1", "a queued replace keeps the price it was committed with") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.session->disconnect();
    L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    const std::string body = L.intent("R1")->fix_body;

    // The market moves and the configuration is touched; the committed bytes do
    // not change.
    L.cfg.finalize();
    L.advance(5000000);
    CHECK_EQ(L.intent("R1")->fix_body, body);
    CHECK(body.find("44=1.08490") != std::string::npos);
    CHECK_INV(L);
}

SCENARIO("OR-I-19", "P0", "a retried replace keeps one identifier and one sequence") {
    Working w;
    const auto r = w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.drive();
    const MsgSeqNum seq = w.lab.session->lookup_binding(CommandId("R1"))->original_seq;

    for (int i = 0; i < 20; ++i) {
        auto again = w.lab.submit(w.lab.replace_order("R1", w.order.chain_id, "80000", "1.08490"));
        CHECK(again.is_ok());
        CHECK_EQ(again.value().cl_ord_id.str(), r.cl_ord_id.str());
        w.lab.drive();
    }
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{1});
    CHECK_EQ(w.lab.session->lookup_binding(CommandId("R1"))->original_seq, seq);
    CHECK_INV(w.lab);
}

SCENARIO("OR-I-20", "P1", "a replace queued while the session recovers does not take effect") {
    Working w;
    w.lab.session->set_state(p01::SessionState::Synchronizing);
    w.lab.must_submit_replace("R1", w.order.chain_id, "80000", "1.08490");
    w.lab.rt->dispatch_pending();

    CHECK_EQ(w.lab.wire_count('G'), std::size_t{0});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_cl_ord_id.str(), w.order.cl_ord_id.str());
    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_qty, 100000);
    CHECK_INV(w.lab);

    w.lab.session->set_state(p01::SessionState::Active);
    w.lab.drive();
    CHECK_EQ(w.lab.wire_count('G'), std::size_t{1});
    CHECK_EQ(w.lab.chain(w.order.chain_id)->accepted_qty, 100000);  // still pending
    CHECK_INV(w.lab);
}
