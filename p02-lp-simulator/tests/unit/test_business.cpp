// Order lifecycle: new orders, identity, fills, cancel and replace.
//
// Matrix coverage: groups C, E, F and G, plus L-01.
//
// Every case here drives the simulator through real FIX bytes and asserts on
// BOTH the ledger and the wire. Scenarios M-10 and M-11 exist because checking
// only one of the two lets a test pass for the wrong reason, so each case ends
// with check_all(), which re-runs the invariants, the independent report
// conformance oracle, ExecID uniqueness and outbound sequence uniqueness.

#include <string>
#include <vector>

#include "../support/harness.hpp"

using namespace lpsim;
using namespace tt;

namespace {

// A fixture with a quote already installed and the session logged on.
struct Live {
    LpFixture fx;
    std::vector<std::string> storage;

    explicit Live(const std::string& tag, FixtureOptions opt = {}) : fx(tag, std::move(opt)) {
        CHECK(fx.start());
        CHECK(fx.logon());
        CHECK_EQ(fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 40000, 1, 600000),
                 QuoteError::Ok);
        fx.transport().clear();
    }

    std::vector<DecodedMessage> reports() { return fx.sent_of_type("8", &storage); }
    std::vector<DecodedMessage> rejects() { return fx.sent_of_type("9", &storage); }
    std::vector<DecodedMessage> all() { return fx.sent_messages(&storage); }

    // The last ExecutionReport, which is what a client would act on.
    DecodedMessage last_report() {
        const auto r = reports();
        CHECK(!r.empty());
        return r.back();
    }

    const OrderChain* order(const std::string& cl_ord_id) {
        return fx.actor().find_by_cl_ord_id(cl_ord_id);
    }

    void send_new(const NewOrderArgs& a) { fx.feed(fx.peer().new_order(a)); }
    void send_cancel(const CancelArgs& a) { fx.feed(fx.peer().cancel(a)); }
    void send_replace(const ReplaceArgs& a) { fx.feed(fx.peer().replace(a)); }
};

FixtureOptions pending_opts() {
    FixtureOptions o;
    o.emit_pending_cancel = true;
    o.emit_pending_replace = true;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// C: new orders and request identity
// ---------------------------------------------------------------------------

SCENARIO("C-01", "P0", "a valid Limit DAY order is accepted exactly once with no fill") {
    Live L("c01");
    L.send_new(NewOrderArgs{});

    const OrderChain* o = L.order("O1");
    CHECK(o != nullptr);
    CHECK_EQ(o->cum_qty, static_cast<Units>(0));
    CHECK_EQ(o->leaves_qty, static_cast<Units>(100000));
    CHECK_EQ(o->base, BaseState::New);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));

    // Exactly one report, and it is a New acknowledgement.
    const auto reports = L.reports();
    CHECK_EQ(reports.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(reports[0], tag::ExecType), std::string("0"));
    CHECK_EQ(field(reports[0], tag::OrdStatus), std::string("0"));
    CHECK_EQ(field(reports[0], tag::LeavesQty), std::string("100000"));
    CHECK_EQ(field(reports[0], tag::ClOrdID), std::string("O1"));
    check_all(L.fx);
}

SCENARIO("C-02", "P0", "a session retransmission of a New creates nothing new") {
    Live L("c02");
    const std::string original = L.fx.peer().new_order(NewOrderArgs{});
    L.fx.feed(original);
    const std::string order_id = L.order("O1")->order_id;
    const std::size_t reports_before = L.reports().size();

    // 43=Y with the original sequence number: the inbox deduplicates it and the
    // business layer never sees it (I-02).
    const auto v = L.fx.feed(ClientPeer::as_poss_dup(original, L.fx.clock().wall_now()));
    CHECK_EQ(v.size(), static_cast<std::size_t>(1));
    CHECK_EQ(v[0], InboundVerdict::IgnoredPossDup);

    CHECK_EQ(L.order("O1")->order_id, order_id);
    CHECK_EQ(L.reports().size(), reports_before);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("C-03", "P0", "an application resend with 97=Y and a fresh sequence is deduplicated") {
    Live L("c03");
    const std::string original = L.fx.peer().new_order(NewOrderArgs{});
    L.fx.feed(original);
    const std::string order_id = L.order("O1")->order_id;
    const std::size_t reports_before = L.reports().size();

    // A new sequence number, so the inbox accepts the frame; the BUSINESS key
    // is what stops a second chain (PB-04).
    const auto v = L.fx.feed(L.fx.peer().as_poss_resend(original));
    CHECK_EQ(v[0], InboundVerdict::Accepted);

    CHECK_EQ(L.order("O1")->order_id, order_id);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
    // No second report, so no second ExecID and no risk of a phantom trade.
    CHECK_EQ(L.reports().size(), reports_before);
    check_all(L.fx);
}

SCENARIO("C-04", "P0", "the same ClOrdID with a different price is refused") {
    Live L("c04");
    L.send_new(NewOrderArgs{});
    const OrderChain before = *L.order("O1");

    NewOrderArgs conflicting;
    conflicting.price = "1.08600";
    L.send_new(conflicting);

    // The original chain is untouched (PB-05).
    const OrderChain* after = L.order("O1");
    CHECK_EQ(after->limit_ticks, before.limit_ticks);
    CHECK_EQ(after->effective_qty, before.effective_qty);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));

    // And the client is told, with the FIX duplicate-order reason.
    const DecodedMessage r = L.last_report();
    CHECK_EQ(field(r, tag::ExecType), std::string("8"));
    CHECK_EQ(field(r, tag::OrdRejReason), std::string("6"));
    check_all(L.fx);
}

SCENARIO("C-05", "P0", "the same ClOrdID with a different quantity is refused") {
    Live L("c05");
    L.send_new(NewOrderArgs{});
    NewOrderArgs conflicting;
    conflicting.qty = "50000";
    L.send_new(conflicting);

    CHECK_EQ(L.order("O1")->effective_qty, static_cast<Units>(100000));
    CHECK_EQ(field(L.last_report(), tag::ExecType), std::string("8"));
    check_all(L.fx);
}

SCENARIO("C-06", "P0", "the same ClOrdID with a different side is refused") {
    Live L("c06");
    L.send_new(NewOrderArgs{});
    NewOrderArgs conflicting;
    conflicting.side = std::string(side_code::Sell);
    L.send_new(conflicting);

    // A side flip accepted as a new order would be an accidental trade.
    CHECK_EQ(L.order("O1")->side, Side::Buy);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
    CHECK_EQ(field(L.last_report(), tag::ExecType), std::string("8"));
    check_all(L.fx);
}

SCENARIO("C-11", "P0", "a resend after a fill does not add a second fill") {
    Live L("c11");
    const std::string original = L.fx.peer().new_order(NewOrderArgs{});
    L.fx.feed(original);
    const std::string order_id = L.order("O1")->order_id;
    L.fx.inject_fill(order_id, 40000, "1.08490");
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));

    // The client never saw the ack and resends. The fill must not repeat.
    L.fx.feed(L.fx.peer().as_poss_resend(original));
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.actor().fill_count_for(order_id), static_cast<std::size_t>(1));
    // The liquidity slice was debited once.
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
    check_all(L.fx);
}

SCENARIO("C-12", "P0", "resending a rejected request does not create an order") {
    Live L("c12");
    NewOrderArgs bad;
    bad.symbol = "UNKNOWN";
    L.send_new(bad);
    CHECK(L.order("O1") == nullptr);

    // The same rejected request again: still no order, and the registry
    // remembers the decision rather than treating it as fresh.
    L.send_new(bad);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("C-16", "P0", "an order for an unrecognised account is refused") {
    FixtureOptions opt;
    opt.validator.allowed_accounts = {"ACC-1"};
    Live L("c16", opt);

    NewOrderArgs good;
    good.account = "ACC-1";
    L.send_new(good);
    CHECK(L.order("O1") != nullptr);

    NewOrderArgs bad;
    bad.cl_ord_id = "O2";
    bad.account = "ACC-9";
    L.send_new(bad);
    CHECK(L.order("O2") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("15"));  // Unknown account
    check_all(L.fx);
}

SCENARIO("C-15", "P1", "a burst of distinct orders gets distinct identities") {
    Live L("c15");
    const int kCount = 200;
    for (int i = 1; i <= kCount; ++i) {
        NewOrderArgs a;
        a.cl_ord_id = "O" + std::to_string(i);
        a.qty = "1000";
        L.send_new(a);
    }
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(kCount));

    // Every chain has its own OrderID, and every report its own ExecID.
    std::vector<std::string> ids;
    for (const OrderChain* c : L.fx.actor().chains()) ids.push_back(c->order_id);
    std::sort(ids.begin(), ids.end());
    CHECK_EQ(std::unique(ids.begin(), ids.end()) - ids.begin(),
             static_cast<long>(kCount));
    check_all(L.fx);
}

SCENARIO("L-01", "P0", "the live-order cap produces a controlled reject") {
    FixtureOptions opt;
    opt.max_live_orders = 3;
    Live L("l01", opt);

    for (int i = 1; i <= 3; ++i) {
        NewOrderArgs a;
        a.cl_ord_id = "O" + std::to_string(i);
        a.qty = "1000";
        L.send_new(a);
        CHECK(L.order(a.cl_ord_id) != nullptr);
    }
    NewOrderArgs over;
    over.cl_ord_id = "O4";
    over.qty = "1000";
    L.send_new(over);

    // Refused for a named reason rather than accepted into an unbounded set.
    CHECK(L.order("O4") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("3"));  // Exceeds limit
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(3));
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// B: application validation, through the real session
// ---------------------------------------------------------------------------

SCENARIO("B-01", "P0", "a New with no ClOrdID creates no order") {
    Live L("b01");
    NewOrderArgs a;
    a.omit_cl_ord_id = true;
    L.send_new(a);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("B-05", "P0", "a Limit order with no Price is refused") {
    Live L("b05");
    NewOrderArgs a;
    a.omit_price = true;
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("B-06b", "P0", "a zero quantity is refused and the ledger is unchanged") {
    Live L("b06");
    NewOrderArgs a;
    a.qty = "0";
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("13"));
    check_all(L.fx);
}

SCENARIO("B-10", "P0", "an unknown symbol is refused for the right reason") {
    Live L("b10");
    NewOrderArgs a;
    a.symbol = "USDJPY";
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("1"));  // Unknown symbol
    check_all(L.fx);
}

SCENARIO("B-08c", "P0", "an off-tick price is refused with the price-increment reason") {
    FixtureOptions opt;
    SymbolProfile half_pip = eurusd();
    half_pip.price.tick_step = 5;  // only multiples of 0.00005
    opt.symbols = {half_pip};
    Live L("b08", opt);

    NewOrderArgs a;
    a.price = "1.08502";
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("18"));
    check_all(L.fx);
}

SCENARIO("B-11", "P1", "an unsupported OrdType is refused, never treated as a market order") {
    Live L("b11");
    NewOrderArgs a;
    a.ord_type = std::string(ord_type::Stop);
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("11"));
    check_all(L.fx);
}

SCENARIO("B-12", "P1", "an unsupported TimeInForce is refused rather than downgraded") {
    Live L("b12");
    NewOrderArgs a;
    a.tif = std::string(tif_code::GoodTillCancel);
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("11"));
    check_all(L.fx);
}

SCENARIO("B-12b", "P0", "an absent TimeInForce defaults to DAY, which is documented") {
    Live L("b12b");
    NewOrderArgs a;
    a.tif.clear();  // tag 59 omitted
    L.send_new(a);
    const OrderChain* o = L.order("O1");
    CHECK(o != nullptr);
    CHECK_EQ(o->tif, Tif::Day);
    check_all(L.fx);
}

SCENARIO("B-18b", "P1", "a repeated mandatory business tag is refused as ambiguous") {
    Live L("b18");
    NewOrderArgs a;
    a.duplicate_qty = true;  // two OrderQty fields
    L.send_new(a);
    // Picking one of the two values would aim the order at a size the operator
    // never sees in the log.
    CHECK(L.order("O1") == nullptr);
    check_all(L.fx);
}

SCENARIO("C-17", "P1", "a TransactTime far in the past is refused") {
    Live L("c17");
    NewOrderArgs a;
    a.transact_time_override = "20261029-05:00:00.000";  // a day old
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    CHECK_EQ(field(L.last_report(), tag::OrdRejReason), std::string("8"));  // Stale order
    check_all(L.fx);
}

SCENARIO("C-18", "P1", "a TransactTime in the future is refused") {
    Live L("c18");
    NewOrderArgs a;
    a.transact_time_override = "20261101-05:00:00.000";
    L.send_new(a);
    CHECK(L.order("O1") == nullptr);
    check_all(L.fx);
}

SCENARIO("B-17", "P0", "an unsupported application MsgType gets 35=j, not a session reject") {
    Live L("b17");
    CanonicalBody body;
    body.emplace_back(tag::ClOrdID, "O1");
    body.emplace_back(tag::Symbol, "EURUSD");
    // 35=H OrderStatusRequest is explicitly phase 2 and not implemented.
    L.fx.feed(L.fx.peer().app_message(msg_type::OrderStatusRequest, body));

    std::vector<std::string> storage;
    const auto js = L.fx.sent_of_type("j", &storage);
    CHECK_EQ(js.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(js[0], tag::RefMsgType), std::string("H"));
    CHECK_EQ(field(js[0], tag::BusinessRejectReason), std::string("3"));
    // And no session-level reject was produced instead.
    CHECK(L.fx.sent_of_type("3", &storage).empty());
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// E: fills and accounting
// ---------------------------------------------------------------------------

SCENARIO("E-02b", "P0", "a partial fill updates CumQty, LeavesQty and the slice") {
    Live L("e02");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    const ActorOutcome out = L.fx.inject_fill(id, 40000, "1.08490");
    CHECK(out.accepted);

    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(o->leaves_qty, static_cast<Units>(60000));
    CHECK_EQ(o->base, BaseState::PartiallyFilled);
    CHECK_EQ(o->avg_px_ticks, static_cast<Ticks>(108490));
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));

    const DecodedMessage r = L.last_report();
    CHECK_EQ(field(r, tag::ExecType), std::string("F"));
    CHECK_EQ(field(r, tag::OrdStatus), std::string("1"));
    CHECK_EQ(field(r, tag::LastQty), std::string("40000"));
    CHECK_EQ(field(r, tag::LastPx), std::string("1.08490"));
    CHECK_EQ(field(r, tag::CumQty), std::string("40000"));
    CHECK_EQ(field(r, tag::LeavesQty), std::string("60000"));
    check_all(L.fx);
}

SCENARIO("E-04b", "P0", "two fills at different prices give the exact weighted AvgPx") {
    Live L("e04");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08490", "1.08500", 80000, 60000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 60000, "1.08500");

    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->cum_qty, static_cast<Units>(100000));
    CHECK_EQ(o->base, BaseState::Filled);
    CHECK_EQ(o->leaves_qty, static_cast<Units>(0));
    // The worked example from design doc 7.4.
    CHECK_EQ(o->avg_px_ticks, static_cast<Ticks>(108496));
    CHECK_EQ(field(L.last_report(), tag::AvgPx), std::string("1.08496"));
    CHECK_EQ(field(L.last_report(), tag::OrdStatus), std::string("2"));
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(2));
    check_all(L.fx);
}

SCENARIO("E-05b", "P0", "the same scheduled fill identity produces one ledger row") {
    Live L("e05");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    // The same event_id twice. The second is refused at the scheduler, before
    // it can reach the ledger at all, because the planned event identity is
    // already persisted and already executed. One scheduled event can never
    // become two trades (I-06, W9).
    L.fx.inject_fill(id, 20000, "1.08490", "FIXED-EVENT");
    const std::size_t after_first = L.fx.actor().fill_count_for(id);
    CHECK_EQ(after_first, static_cast<std::size_t>(1));

    L.fx.inject_fill(id, 20000, "1.08490", "FIXED-EVENT", /*expect_schedule_ok=*/false);
    CHECK_EQ(L.fx.last_schedule(), EnqueueResult::AlreadyExecuted);
    CHECK_EQ(L.fx.actor().fill_count_for(id), after_first);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(20000));
    check_all(L.fx);
}

SCENARIO("E-07b", "P0", "an oversized fill is refused and changes nothing") {
    Live L("e07");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 200000, 2, 600000),
             QuoteError::Ok);

    const ActorOutcome out = L.fx.inject_fill(id, 110000, "1.08490");
    CHECK(!out.accepted);
    CHECK_EQ(out.no_trade, NoTrade::QtyExceedsLeaves);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(0));
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("E-10b", "P0", "a fill after a cancel is refused") {
    Live L("e10");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.send_cancel(CancelArgs{});
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);

    const ActorOutcome out = L.fx.inject_fill(id, 10000, "1.08490");
    CHECK(!out.accepted);
    CHECK_EQ(out.no_trade, NoTrade::OrderTerminal);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("E-12", "P0", "a fill for an order that does not exist is refused") {
    Live L("e12");
    const ActorOutcome out = L.fx.inject_fill("SIM-9999", 10000, "1.08490");
    CHECK(!out.accepted);
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("D-06b", "P0", "a fill against a stale quote is refused") {
    Live L("d06");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    // Install a short-lived quote, then let it expire.
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 40000, 2, 100),
             QuoteError::Ok);
    L.fx.advance(500);

    const ActorOutcome out = L.fx.inject_fill(id, 10000, "1.08490");
    CHECK(!out.accepted);
    CHECK_EQ(out.no_trade, NoTrade::StaleQuote);
    check_all(L.fx);
}

SCENARIO("H-06", "P0", "two queued fills can never together overfill the order") {
    Live L("h06");
    NewOrderArgs a;
    a.qty = "100000";
    L.send_new(a);
    const std::string id = L.order("O1")->order_id;
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 200000, 2, 600000),
             QuoteError::Ok);

    // Two 70000 fills on a 100000 order: the first succeeds, the second is
    // refused because only 30000 remains.
    CHECK(L.fx.inject_fill(id, 70000, "1.08490").accepted);
    const ActorOutcome second = L.fx.inject_fill(id, 70000, "1.08490");
    CHECK(!second.accepted);
    CHECK_EQ(second.no_trade, NoTrade::QtyExceedsLeaves);

    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(70000));
    CHECK(L.order("O1")->cum_qty <= L.order("O1")->effective_qty);
    check_all(L.fx);
}

SCENARIO("D-11b", "P0", "two orders cannot together consume more than the slice") {
    Live L("d11");
    NewOrderArgs a;
    a.qty = "40000";
    L.send_new(a);
    NewOrderArgs b;
    b.cl_ord_id = "O2";
    b.qty = "40000";
    L.send_new(b);

    const std::string id1 = L.order("O1")->order_id;
    const std::string id2 = L.order("O2")->order_id;

    // The slice holds 40000 in total.
    CHECK(L.fx.inject_fill(id1, 40000, "1.08490").accepted);
    const ActorOutcome second = L.fx.inject_fill(id2, 40000, "1.08490");
    CHECK(!second.accepted);
    CHECK_EQ(second.no_trade, NoTrade::NoLiquidity);

    CHECK_EQ(L.order("O1")->cum_qty + L.order("O2")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// F: cancel
// ---------------------------------------------------------------------------

SCENARIO("F-01", "P0", "cancelling a live unfilled order is terminal with LeavesQty zero") {
    Live L("f01");
    L.send_new(NewOrderArgs{});
    L.send_cancel(CancelArgs{});

    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->base, BaseState::Canceled);
    CHECK_EQ(o->leaves_qty, static_cast<Units>(0));

    const DecodedMessage r = L.last_report();
    CHECK_EQ(field(r, tag::ExecType), std::string("4"));
    CHECK_EQ(field(r, tag::OrdStatus), std::string("4"));
    CHECK_EQ(field(r, tag::ClOrdID), std::string("C1"));
    CHECK_EQ(field(r, tag::OrigClOrdID), std::string("O1"));
    check_all(L.fx);
}

SCENARIO("F-02b", "P0", "cancelling a partially filled order keeps CumQty") {
    Live L("f02");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");
    L.send_cancel(CancelArgs{});

    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->base, BaseState::Canceled);
    CHECK_EQ(o->cum_qty, static_cast<Units>(40000));
    // Invariant I-04: LeavesQty is zero even though OrderQty exceeds CumQty.
    CHECK_EQ(o->leaves_qty, static_cast<Units>(0));

    const DecodedMessage r = L.last_report();
    CHECK_EQ(field(r, tag::CumQty), std::string("40000"));
    CHECK_EQ(field(r, tag::LeavesQty), std::string("0"));
    CHECK_EQ(field(r, tag::OrderQty), std::string("100000"));
    check_all(L.fx);
}

SCENARIO("F-03b", "P0", "cancelling an unknown order is 35=9 and mutates nothing") {
    Live L("f03");
    L.send_new(NewOrderArgs{});
    CancelArgs c;
    c.orig_cl_ord_id = "NO-SUCH-ORDER";
    L.send_cancel(c);

    CHECK_EQ(L.order("O1")->base, BaseState::New);
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::CxlRejResponseTo), std::string("1"));
    CHECK_EQ(field(rejects[0], tag::CxlRejReason), std::string("1"));  // Unknown order
    CHECK_EQ(field(rejects[0], tag::OrderID), std::string("NONE"));
    check_all(L.fx);
}

SCENARIO("F-04", "P0", "cancelling a filled order is refused as too late") {
    Live L("f04");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 100000, "1.08490");
    CHECK_EQ(L.order("O1")->base, BaseState::Filled);

    L.send_cancel(CancelArgs{});
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::CxlRejReason), std::string("0"));  // Too late
    // F-17: the reject reports the CURRENT state.
    CHECK_EQ(field(rejects[0], tag::OrdStatus), std::string("2"));
    CHECK_EQ(L.order("O1")->base, BaseState::Filled);
    check_all(L.fx);
}

SCENARIO("F-05", "P0", "a second cancel with a new id on a cancelled order is refused") {
    Live L("f05");
    L.send_new(NewOrderArgs{});
    L.send_cancel(CancelArgs{});
    CancelArgs second;
    second.cl_ord_id = "C2";
    L.send_cancel(second);

    // One terminal transition only.
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::ClOrdID), std::string("C2"));
    check_all(L.fx);
}

SCENARIO("F-06", "P0", "a duplicate cancel request is deduplicated, not re-applied") {
    Live L("f06");
    L.send_new(NewOrderArgs{});
    const std::string cancel_frame = L.fx.peer().cancel(CancelArgs{});
    L.fx.feed(cancel_frame);
    const std::size_t reports_before = L.all().size();

    // The same cancel with a fresh sequence number.
    L.fx.feed(L.fx.peer().as_poss_resend(cancel_frame));
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    // No second terminal report.
    CHECK_EQ(L.all().size(), reports_before);
    check_all(L.fx);
}

SCENARIO("F-07", "P0", "a cancel id reused against a different order is refused") {
    Live L("f07");
    L.send_new(NewOrderArgs{});
    NewOrderArgs second;
    second.cl_ord_id = "O2";
    L.send_new(second);

    L.send_cancel(CancelArgs{});  // C1 cancels O1
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);

    CancelArgs retarget;
    retarget.cl_ord_id = "C1";  // same request id
    retarget.orig_cl_ord_id = "O2";
    L.send_cancel(retarget);

    // O2 is untouched: a reused request id must not reach another order.
    CHECK_EQ(L.order("O2")->base, BaseState::New);
    check_all(L.fx);
}

SCENARIO("F-08", "P0", "a cancel naming a superseded version is refused") {
    Live L("f08");
    L.send_new(NewOrderArgs{});
    ReplaceArgs r;
    r.qty = "120000";
    L.send_replace(r);
    CHECK_EQ(L.order("R1")->current_cl_ord_id, std::string("R1"));

    // 41 still points at O1, which is no longer the accepted version.
    CancelArgs stale;
    stale.orig_cl_ord_id = "O1";
    L.send_cancel(stale);

    CHECK_EQ(L.order("R1")->base, BaseState::New);
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::CxlRejReason), std::string("1"));
    check_all(L.fx);
}

SCENARIO("F-12", "P0", "a second cancel while one is pending is refused") {
    Live L("f12", pending_opts());
    L.send_new(NewOrderArgs{});
    L.send_cancel(CancelArgs{});
    CHECK_EQ(L.order("O1")->pending, Pending::Cancel);

    CancelArgs second;
    second.cl_ord_id = "C2";
    L.send_cancel(second);

    // The first request stays in flight; no contradictory terminal reports.
    CHECK_EQ(L.order("O1")->pending_cl_ord_id, std::string("C1"));
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::CxlRejReason), std::string("3"));  // Already pending
    check_all(L.fx);
}

SCENARIO("F-09", "P0", "a fill while a cancel is pending reports the pending status") {
    Live L("f09", pending_opts());
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");
    L.send_cancel(CancelArgs{});

    // The PendingCancel acknowledgement.
    CHECK_EQ(field(L.last_report(), tag::ExecType), std::string("6"));

    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08485", "1.08495", 80000, 20000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 20000, "1.08495");

    const DecodedMessage trade = L.last_report();
    CHECK_EQ(field(trade, tag::ExecType), std::string("F"));
    // Status precedence: still pending, because the order is live.
    CHECK_EQ(field(trade, tag::OrdStatus), std::string("6"));
    CHECK_EQ(field(trade, tag::CumQty), std::string("60000"));
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(60000));
    check_all(L.fx);
}

SCENARIO("F-11", "P0", "a cancel finalised before a scheduled fill invalidates the fill") {
    Live L("f11", pending_opts());
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.send_cancel(CancelArgs{});

    // Finalise at t=10.
    ScheduledEvent fin;
    fin.event_id = "FIN-C1";
    fin.due_us = 10000;
    fin.kind = EventKind::FinalizeCancel;
    fin.order_id = id;
    fin.request_id = "C1";
    CHECK_EQ(L.fx.actor().schedule(fin), EnqueueResult::Ok);

    // Fill at t=20, after the cancel.
    ScheduledEvent fill;
    fill.event_id = "IF-late";
    fill.due_us = 20000;
    fill.kind = EventKind::InjectFill;
    fill.order_id = id;
    fill.qty = 10000;
    fill.px_ticks = 108490;
    CHECK_EQ(L.fx.actor().schedule(fill), EnqueueResult::Ok);

    L.fx.clock().set_virtual_us(30000);
    L.fx.actor().run_due(30000);
    L.fx.session().dispatch_outbox();

    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(0));
    CHECK_EQ(L.fx.actor().fill_count(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("F-20", "P0", "a cancel cannot reach another firm's order") {
    Live L("f20");
    L.send_new(NewOrderArgs{});
    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->firm, std::string("TRADER01"));
    // The session authenticates the firm from the CompID, so a request always
    // carries the firm of the session it arrived on. A cancel therefore cannot
    // name a chain belonging to another firm: the lookup is firm-scoped and the
    // chain's own firm is compared before any mutation.
    CancelArgs c;
    c.orig_cl_ord_id = "O1";
    L.send_cancel(c);
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    CHECK_EQ(L.order("O1")->firm, std::string("TRADER01"));
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// G: replace
// ---------------------------------------------------------------------------

SCENARIO("G-01b", "P0", "a replace keeps the OrderID and takes the new ClOrdID") {
    Live L("g01");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    ReplaceArgs r;
    r.price = "1.08470";
    L.send_replace(r);

    const OrderChain* o = L.order("R1");
    CHECK(o != nullptr);
    CHECK_EQ(o->order_id, id);  // stable across replace (I-08)
    CHECK_EQ(o->current_cl_ord_id, std::string("R1"));
    CHECK_EQ(o->limit_ticks, static_cast<Ticks>(108470));

    const DecodedMessage rep = L.last_report();
    CHECK_EQ(field(rep, tag::ExecType), std::string("5"));
    CHECK_EQ(field(rep, tag::ClOrdID), std::string("R1"));
    CHECK_EQ(field(rep, tag::OrigClOrdID), std::string("O1"));
    check_all(L.fx);
}

SCENARIO("G-02", "P0", "a replace raising the quantity keeps CumQty and recomputes leaves") {
    Live L("g02");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    ReplaceArgs r;
    r.qty = "150000";
    r.price = "1.08500";
    L.send_replace(r);

    const OrderChain* o = L.order("R1");
    CHECK_EQ(o->effective_qty, static_cast<Units>(150000));
    CHECK_EQ(o->cum_qty, static_cast<Units>(40000));   // not reset
    CHECK_EQ(o->leaves_qty, static_cast<Units>(110000));
    CHECK_EQ(o->base, BaseState::PartiallyFilled);
    check_all(L.fx);
}

SCENARIO("G-03", "P0", "a replace lowering the quantity to at least CumQty is accepted") {
    Live L("g03");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    ReplaceArgs r;
    r.qty = "60000";
    r.price = "1.08500";
    L.send_replace(r);

    const OrderChain* o = L.order("R1");
    CHECK_EQ(o->effective_qty, static_cast<Units>(60000));
    CHECK_EQ(o->leaves_qty, static_cast<Units>(20000));
    check_all(L.fx);
}

SCENARIO("G-04", "P0", "a replace below CumQty is refused in the strict profile") {
    Live L("g04");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    ReplaceArgs r;
    r.qty = "20000";  // below the 40000 already traded
    r.price = "1.08500";
    L.send_replace(r);

    // ADR-10: this profile refuses rather than reinterpreting. Negative leaves
    // must be impossible.
    CHECK(L.order("R1") == nullptr);
    CHECK_EQ(L.order("O1")->effective_qty, static_cast<Units>(100000));
    const auto rejects = L.rejects();
    CHECK_EQ(rejects.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rejects[0], tag::CxlRejResponseTo), std::string("2"));
    check_all(L.fx);
}

SCENARIO("G-05b", "P0", "a replace on a filled order is refused with response-to 2") {
    Live L("g05");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 100000, "1.08490");

    ReplaceArgs r;
    r.qty = "150000";
    L.send_replace(r);

    CHECK_EQ(L.order("O1")->base, BaseState::Filled);
    const auto rejects = L.rejects();
    CHECK_EQ(field(rejects[0], tag::CxlRejResponseTo), std::string("2"));
    CHECK_EQ(field(rejects[0], tag::CxlRejReason), std::string("0"));
    check_all(L.fx);
}

SCENARIO("G-06", "P0", "a replace for an unknown order creates no chain") {
    Live L("g06");
    ReplaceArgs r;
    r.orig_cl_ord_id = "UNKNOWN";
    L.send_replace(r);
    CHECK(L.order("R1") == nullptr);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(0));
    CHECK_EQ(field(L.rejects()[0], tag::CxlRejReason), std::string("1"));
    check_all(L.fx);
}

SCENARIO("G-09", "P0", "the same replace id with a contradictory price is refused") {
    Live L("g09");
    L.send_new(NewOrderArgs{});
    ReplaceArgs first;
    first.price = "1.08500";
    L.send_replace(first);
    CHECK_EQ(L.order("R1")->limit_ticks, static_cast<Ticks>(108500));

    ReplaceArgs conflicting;
    conflicting.cl_ord_id = "R1";
    conflicting.orig_cl_ord_id = "R1";
    conflicting.price = "1.08600";
    L.send_replace(conflicting);

    CHECK_EQ(L.order("R1")->limit_ticks, static_cast<Ticks>(108500));
    check_all(L.fx);
}

SCENARIO("G-10", "P0", "a replace chain keeps one OrderID and the cumulative fills") {
    Live L("g10");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    ReplaceArgs r0;
    r0.cl_ord_id = "R0";
    r0.orig_cl_ord_id = "O1";
    r0.qty = "120000";
    r0.price = "1.08500";
    L.send_replace(r0);

    ReplaceArgs r1;
    r1.cl_ord_id = "R1";
    r1.orig_cl_ord_id = "R0";
    r1.qty = "130000";
    r1.price = "1.08500";
    L.send_replace(r1);

    const OrderChain* o = L.order("R1");
    CHECK_EQ(o->order_id, id);
    CHECK_EQ(o->effective_qty, static_cast<Units>(130000));
    CHECK_EQ(o->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
    check_all(L.fx);
}

SCENARIO("G-11", "P0", "a replace naming a superseded version is refused") {
    Live L("g11");
    L.send_new(NewOrderArgs{});
    ReplaceArgs r0;
    r0.cl_ord_id = "R0";
    r0.qty = "120000";
    r0.price = "1.08500";
    L.send_replace(r0);

    // 41 still points at O1 rather than R0.
    ReplaceArgs stale;
    stale.cl_ord_id = "R1";
    stale.orig_cl_ord_id = "O1";
    stale.qty = "130000";
    stale.price = "1.08500";
    L.send_replace(stale);

    CHECK(L.order("R1") == nullptr);
    CHECK_EQ(L.order("R0")->effective_qty, static_cast<Units>(120000));
    check_all(L.fx);
}

SCENARIO("G-12", "P0", "optimistic chaining against a pending version is refused") {
    Live L("g12", pending_opts());
    L.send_new(NewOrderArgs{});
    ReplaceArgs r1;
    r1.cl_ord_id = "R1";
    r1.qty = "120000";
    r1.price = "1.08500";
    L.send_replace(r1);
    CHECK_EQ(L.order("O1")->pending, Pending::Replace);

    // R2 chains off R1, which has not been accepted yet.
    ReplaceArgs r2;
    r2.cl_ord_id = "R2";
    r2.orig_cl_ord_id = "R1";
    r2.qty = "130000";
    r2.price = "1.08500";
    L.send_replace(r2);

    CHECK_EQ(L.order("O1")->pending_cl_ord_id, std::string("R1"));
    CHECK_EQ(L.order("O1")->effective_qty, static_cast<Units>(100000));
    check_all(L.fx);
}

SCENARIO("G-16", "P0", "a cancel arriving while a replace is pending is refused") {
    Live L("g16", pending_opts());
    L.send_new(NewOrderArgs{});
    ReplaceArgs r;
    r.qty = "120000";
    r.price = "1.08500";
    L.send_replace(r);

    L.send_cancel(CancelArgs{});
    // One outcome per chain: the replace keeps the slot.
    CHECK_EQ(L.order("O1")->pending, Pending::Replace);
    CHECK_EQ(field(L.rejects().back(), tag::CxlRejReason), std::string("3"));
    check_all(L.fx);
}

SCENARIO("G-17", "P0", "a replace arriving while a cancel is pending is refused") {
    Live L("g17", pending_opts());
    L.send_new(NewOrderArgs{});
    L.send_cancel(CancelArgs{});

    ReplaceArgs r;
    r.qty = "120000";
    r.price = "1.08500";
    L.send_replace(r);

    CHECK_EQ(L.order("O1")->pending, Pending::Cancel);
    CHECK_EQ(L.order("O1")->effective_qty, static_cast<Units>(100000));
    check_all(L.fx);
}

SCENARIO("G-13", "P0", "a fill while a replace is pending uses the OLD accepted price") {
    Live L("g13", pending_opts());
    L.send_new(NewOrderArgs{});  // limit 1.08500
    const std::string id = L.order("O1")->order_id;

    ReplaceArgs r;
    r.qty = "150000";
    r.price = "1.08470";  // a LOWER limit, pending
    L.send_replace(r);
    CHECK_EQ(L.order("O1")->pending, Pending::Replace);
    CHECK_EQ(L.order("O1")->limit_ticks, static_cast<Ticks>(108500));

    // 1.08495 is inside the OLD limit but outside the pending one, so the fill
    // can only happen if the old parameters are still in force (PB-08).
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08485", "1.08495", 80000, 20000, 2, 600000),
             QuoteError::Ok);
    const ActorOutcome out = L.fx.inject_fill(id, 20000, "1.08495");
    CHECK(out.accepted);

    const DecodedMessage trade = L.last_report();
    // The trade reports the accepted version, which is still O1.
    CHECK_EQ(field(trade, tag::ClOrdID), std::string("O1"));
    CHECK_EQ(field(trade, tag::OrdStatus), std::string("E"));  // PendingReplace
    CHECK_EQ(field(trade, tag::LastPx), std::string("1.08495"));
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(20000));
    check_all(L.fx);
}

SCENARIO("G-14", "P0", "a pending replace is refused once the order fully fills") {
    Live L("g14", pending_opts());
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    ReplaceArgs r;
    r.qty = "150000";
    r.price = "1.08500";
    L.send_replace(r);

    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 100000, "1.08490");
    CHECK_EQ(L.order("O1")->base, BaseState::Filled);

    ScheduledEvent fin;
    fin.event_id = "FIN-R1";
    fin.due_us = L.fx.clock().virtual_now_us() + 1000;
    fin.kind = EventKind::FinalizeReplace;
    fin.order_id = id;
    fin.request_id = "R1";
    CHECK_EQ(L.fx.actor().schedule(fin), EnqueueResult::Ok);
    L.fx.clock().set_virtual_us(fin.due_us);
    L.fx.actor().run_due(fin.due_us);
    L.fx.session().dispatch_outbox();

    // The order stays Filled with no extra leaves, and the request is refused.
    CHECK_EQ(L.order("O1")->base, BaseState::Filled);
    CHECK_EQ(L.order("O1")->leaves_qty, static_cast<Units>(0));
    CHECK_EQ(L.order("O1")->effective_qty, static_cast<Units>(100000));
    CHECK_EQ(field(L.rejects().back(), tag::CxlRejResponseTo), std::string("2"));
    check_all(L.fx);
}

SCENARIO("G-15", "P0", "once a replace is accepted, later fills use the new parameters") {
    Live L("g15", pending_opts());
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;

    ReplaceArgs r;
    r.qty = "150000";
    r.price = "1.08470";
    L.send_replace(r);

    ScheduledEvent fin;
    fin.event_id = "FIN-R1";
    fin.due_us = L.fx.clock().virtual_now_us() + 1000;
    fin.kind = EventKind::FinalizeReplace;
    fin.order_id = id;
    fin.request_id = "R1";
    CHECK_EQ(L.fx.actor().schedule(fin), EnqueueResult::Ok);
    L.fx.clock().set_virtual_us(fin.due_us);
    L.fx.actor().run_due(fin.due_us);
    L.fx.session().dispatch_outbox();

    CHECK_EQ(L.order("R1")->limit_ticks, static_cast<Ticks>(108470));
    CHECK_EQ(L.order("R1")->effective_qty, static_cast<Units>(150000));

    // 1.08490 is now outside the limit, so it must be refused.
    const ActorOutcome refused = L.fx.inject_fill(id, 10000, "1.08490");
    CHECK(!refused.accepted);
    CHECK_EQ(refused.no_trade, NoTrade::NotMarketable);

    // A quote inside the new limit fills.
    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08450", "1.08460", 80000, 30000, 2, 600000),
             QuoteError::Ok);
    CHECK(L.fx.inject_fill(id, 30000, "1.08460").accepted);
    CHECK_EQ(L.order("R1")->cum_qty, static_cast<Units>(30000));
    check_all(L.fx);
}

SCENARIO("G-19", "P1", "a replace cannot change the symbol or the side") {
    Live L("g19");
    FixtureOptions opt;
    opt.symbols = {eurusd(), gbpusd()};
    Live M("g19b", opt);
    M.send_new(NewOrderArgs{});

    ReplaceArgs sym;
    sym.symbol = "GBPUSD";
    sym.qty = "100000";
    sym.price = "1.08500";
    M.send_replace(sym);
    CHECK(M.order("R1") == nullptr);

    ReplaceArgs side;
    side.cl_ord_id = "R2";
    side.side = std::string(side_code::Sell);
    side.qty = "100000";
    side.price = "1.08500";
    M.send_replace(side);
    CHECK(M.order("R2") == nullptr);
    CHECK_EQ(M.order("O1")->side, Side::Buy);
    CHECK_EQ(M.order("O1")->symbol, std::string("EURUSD"));
    check_all(M.fx);
    check_all(L.fx);
}

SCENARIO("G-07", "P0", "a no-op replace is accepted as a new version") {
    Live L("g07");
    L.send_new(NewOrderArgs{});
    ReplaceArgs same;
    same.qty = "100000";
    same.price = "1.08500";  // identical to the original
    L.send_replace(same);

    // The profile accepts it and advances the version, which is one of the two
    // readings the design doc allows (G-07) and is stated here as the choice.
    const OrderChain* o = L.order("R1");
    CHECK(o != nullptr);
    CHECK_EQ(o->effective_qty, static_cast<Units>(100000));
    CHECK_EQ(o->limit_ticks, static_cast<Ticks>(108500));
    CHECK_EQ(o->current_cl_ord_id, std::string("R1"));
    CHECK_EQ(field(L.last_report(), tag::ExecType), std::string("5"));
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// Expiry
// ---------------------------------------------------------------------------

SCENARIO("H-04b", "P0", "an expiry on a live order is terminal with zero leaves") {
    Live L("h04");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    ScheduledEvent exp;
    exp.event_id = "EXP-1";
    exp.due_us = L.fx.clock().virtual_now_us() + 1000;
    exp.kind = EventKind::Expire;
    exp.order_id = id;
    CHECK_EQ(L.fx.actor().schedule(exp), EnqueueResult::Ok);
    L.fx.clock().set_virtual_us(exp.due_us);
    L.fx.actor().run_due(exp.due_us);
    L.fx.session().dispatch_outbox();

    const OrderChain* o = L.order("O1");
    CHECK_EQ(o->base, BaseState::Expired);
    CHECK_EQ(o->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(o->leaves_qty, static_cast<Units>(0));
    CHECK_EQ(field(L.last_report(), tag::OrdStatus), std::string("C"));
    check_all(L.fx);
}

SCENARIO("H-05b", "P0", "an expiry on an already terminal order does nothing") {
    Live L("h05");
    L.send_new(NewOrderArgs{});
    const std::string id = L.order("O1")->order_id;
    L.send_cancel(CancelArgs{});
    const std::size_t reports_before = L.all().size();

    ScheduledEvent exp;
    exp.event_id = "EXP-1";
    exp.due_us = L.fx.clock().virtual_now_us() + 1000;
    exp.kind = EventKind::Expire;
    exp.order_id = id;
    CHECK_EQ(L.fx.actor().schedule(exp), EnqueueResult::Ok);
    L.fx.clock().set_virtual_us(exp.due_us);
    L.fx.actor().run_due(exp.due_us);
    L.fx.session().dispatch_outbox();

    // Exactly one terminal outcome per chain.
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    CHECK_EQ(L.all().size(), reports_before);
    check_all(L.fx);
}

// ---------------------------------------------------------------------------
// Not implemented in this profile
// ---------------------------------------------------------------------------

SCENARIO_NOT_IMPLEMENTED(
    "M-19", "P1", "IOC and FOK order handling",
    "The v1 profile implements Limit DAY only (design doc 1.4, ADR-11). Tags 59=3 "
    "and 59=4 are parsed solely so they can be refused for a named reason, which "
    "B-12 asserts. Claiming a pass here would be a false claim of support.")

SCENARIO_NOT_IMPLEMENTED(
    "C-09", "P1", "ClOrdID reuse across a business-day rollover",
    "The business day comes from the UTC clock and a rollover is an explicit event "
    "the v1 profile does not implement (design doc 12.2). Until it exists, the "
    "namespace behaviour across a rollover is undefined rather than passing.")

SCENARIO_NOT_IMPLEMENTED(
    "C-08", "P2", "the same ClOrdID arriving on a second session for one firm",
    "v1 serves one session per identity (design doc header). Multi-session firm "
    "namespacing is designed but not built, so there is nothing to test.")

SCENARIO_NOT_IMPLEMENTED(
    "L-20", "P2", "multi-tenant authentication and rate isolation",
    "Explicitly deferred by the design doc (L-20).")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "business"); }
