// Quote book, liquidity conservation and the matching gate.
//
// Matrix coverage: group D in full (D-01 to D-20), plus the matcher half of
// E-07 to E-09. These run against the book and the matcher directly, with no
// store and no session, so a failure here is unambiguous about which layer is
// wrong.
//
// The two rules this suite pins hardest are invariant I-11 (a buyer never pays
// more than its limit, a seller never accepts less) and invariant I-12 (a
// liquidity slice is consumed at most once).

#include <string>

#include "lp/domain.hpp"
#include "lp/matcher.hpp"
#include "lp/quote_book.hpp"
#include "../support/tiny_test.hpp"

using namespace lpsim;

namespace {

const PriceFormat kFx5{5, 1};

QuoteSlice quote(std::uint64_t version, Ticks bid, Ticks ask, Units bid_av, Units ask_av,
                 std::int64_t from_ms = 0, std::int64_t ttl_ms = 2000) {
    QuoteSlice q;
    q.symbol = "EURUSD";
    q.version = version;
    q.bid_ticks = bid;
    q.ask_ticks = ask;
    q.bid_available = bid_av;
    q.ask_available = ask_av;
    q.valid_from_ms = from_ms;
    q.expires_at_ms = from_ms + ttl_ms;
    q.source_fixture_id = "test";
    return q;
}

QuoteBook book_with(const QuoteSlice& q) {
    QuoteBook b;
    b.register_symbol("EURUSD");
    b.register_symbol("GBPUSD");
    CHECK_EQ(b.apply_update(q), QuoteError::Ok);
    return b;
}

OrderChain order(Side side, Units qty, Ticks limit, Units cum = 0,
                 BaseState base = BaseState::New) {
    OrderChain o;
    o.order_id = "SIM-1001";
    o.firm = "TRADER01";
    o.business_day = "20261030";
    o.original_cl_ord_id = "O1";
    o.current_cl_ord_id = "O1";
    o.symbol = "EURUSD";
    o.side = side;
    o.ord_type = OrdType::Limit;
    o.tif = Tif::Day;
    o.original_qty = qty;
    o.effective_qty = qty;
    o.limit_ticks = limit;
    o.cum_qty = cum;
    o.base = base;
    o.leaves_qty = o.expected_leaves();
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// Marketability and fills
// ---------------------------------------------------------------------------

SCENARIO("D-01", "P0", "a buy whose limit covers the ask fills up to the available size") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK(p.traded);
    CHECK_EQ(p.qty, static_cast<Units>(40000));   // min(leaves, available)
    CHECK_EQ(p.px_ticks, static_cast<Ticks>(108490));
    // Invariant I-11: the buy price is at or below the limit.
    CHECK(p.px_ticks <= o.limit_ticks);
    CHECK_EQ(p.quote_version, static_cast<std::uint64_t>(1));
    CHECK_EQ(p.slice_id, std::string("EURUSD:1:ASK"));
}

SCENARIO("D-02", "P0", "a buy whose limit is below the ask does not fill") {
    QuoteBook b = book_with(quote(1, 108480, 108510, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::NotMarketable);
    CHECK_EQ(p.qty, static_cast<Units>(0));
    // The book is untouched by a refused match.
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(40000));
}

SCENARIO("D-03", "P0", "a sell whose limit is at or below the bid fills against the bid") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Sell, 100000, 108470);

    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK(p.traded);
    CHECK_EQ(p.qty, static_cast<Units>(80000));
    CHECK_EQ(p.px_ticks, static_cast<Ticks>(108480));
    // Invariant I-11 on the sell side: the price is at or above the limit.
    CHECK(p.px_ticks >= o.limit_ticks);
    CHECK_EQ(p.slice_id, std::string("EURUSD:1:BID"));
}

SCENARIO("D-04", "P0", "a sell whose limit is above the bid does not fill") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Sell, 100000, 108490);

    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::NotMarketable);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Sell), static_cast<Units>(80000));
}

SCENARIO("D-05", "P0", "no ask liquidity means no trade and no negative inventory") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 0));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::NoLiquidity);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
    // An explicit consume of more than exists is refused too.
    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 1000), QuoteError::InsufficientLiquidity);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
}

SCENARIO("D-06", "P0", "a quote past its TTL cannot back a fill") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000, 0, 2000));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    CHECK(plan_auto_match(o, b, 1999).traded);
    const MatchPlan p = plan_auto_match(o, b, 2500);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::StaleQuote);
}

SCENARIO("D-07", "P0", "expiry is exclusive: at exactly expires_at the quote is stale") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000, 0, 2000));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    // The documented rule is usable while now < expires_at.
    CHECK(plan_auto_match(o, b, 1999).traded);
    CHECK_EQ(plan_auto_match(o, b, 2000).reason, NoTrade::StaleQuote);
    // And before valid_from it is not usable either.
    QuoteBook later = book_with(quote(1, 108480, 108490, 80000, 40000, 5000, 2000));
    CHECK_EQ(plan_auto_match(o, later, 4999).reason, NoTrade::StaleQuote);
    CHECK(plan_auto_match(o, later, 5000).traded);
}

SCENARIO("D-08", "P0", "a crossed book is refused outright") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    // bid above ask
    CHECK_EQ(b.apply_update(quote(1, 108510, 108500, 80000, 40000)), QuoteError::CrossedBook);
    // bid equal to ask is locked, and refused for the same reason
    CHECK_EQ(b.apply_update(quote(1, 108500, 108500, 80000, 40000)), QuoteError::CrossedBook);
    // Nothing was installed, so no accidental fill is possible.
    CHECK(b.current("EURUSD") == nullptr);
}

SCENARIO("D-09", "P0", "negative or non-positive quote fields are refused") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    CHECK_EQ(b.apply_update(quote(1, 108480, 108490, 80000, -1)), QuoteError::NegativeQuantity);
    CHECK_EQ(b.apply_update(quote(1, 108480, 108490, -5, 40000)), QuoteError::NegativeQuantity);
    CHECK_EQ(b.apply_update(quote(1, 0, 108490, 80000, 40000)), QuoteError::NotPositivePrice);
    CHECK_EQ(b.apply_update(quote(1, -108480, 108490, 80000, 40000)),
             QuoteError::NotPositivePrice);
    CHECK(b.current("EURUSD") == nullptr);
}

SCENARIO("L-10", "P0", "an absurd available quantity is refused before it can be consumed") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    CHECK_EQ(b.apply_update(quote(1, 108480, 108490, 80000, 9000000000000000LL)),
             QuoteError::QuantityOverflow);
    CHECK(b.current("EURUSD") == nullptr);
}

SCENARIO("D-10", "P0", "the last available unit can be taken once and only once") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 1000));
    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 1000), QuoteError::Ok);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
    // A second attempt at the same slice is refused: invariant I-12.
    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 1000), QuoteError::InsufficientLiquidity);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
}

SCENARIO("D-11", "P0", "two orders cannot together take more than the slice holds") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o1 = order(Side::Buy, 40000, 108500);

    const MatchPlan p1 = plan_auto_match(o1, b, 0);
    CHECK(p1.traded);
    CHECK_EQ(p1.qty, static_cast<Units>(40000));
    CHECK_EQ(b.consume("EURUSD", p1.quote_version, Side::Buy, p1.qty), QuoteError::Ok);

    // The second order sees the depleted slice.
    OrderChain o2 = order(Side::Buy, 40000, 108500);
    o2.order_id = "SIM-1002";
    const MatchPlan p2 = plan_auto_match(o2, b, 0);
    CHECK(!p2.traded);
    CHECK_EQ(p2.reason, NoTrade::NoLiquidity);
    CHECK_EQ(p1.qty + p2.qty, static_cast<Units>(40000));
}

SCENARIO("D-12", "P0", "consuming against a superseded quote version is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    // A match planned against version 1 ...
    const OrderChain o = order(Side::Buy, 100000, 108500);
    const MatchPlan p = plan_auto_match(o, b, 0);
    CHECK_EQ(p.quote_version, static_cast<std::uint64_t>(1));

    // ... and then version 2 arrives before it is committed.
    CHECK_EQ(b.apply_update(quote(2, 108485, 108495, 70000, 30000)), QuoteError::Ok);
    // The pinned version is no longer current, so the consumption is refused
    // rather than silently applied at the new price.
    CHECK_EQ(b.consume("EURUSD", p.quote_version, Side::Buy, p.qty),
             QuoteError::VersionMismatch);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(30000));
}

SCENARIO("D-13", "P0", "the same version with different content is a conflict") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    // An identical replay is an idempotent no-op.
    CHECK_EQ(b.apply_update(quote(1, 108480, 108490, 80000, 40000)), QuoteError::Ok);
    CHECK_EQ(b.current("EURUSD")->ask_ticks, static_cast<Ticks>(108490));
    // A different body under the same version is refused, and nothing rewinds.
    CHECK_EQ(b.apply_update(quote(1, 108480, 108495, 80000, 40000)),
             QuoteError::DuplicateVersion);
    CHECK_EQ(b.current("EURUSD")->ask_ticks, static_cast<Ticks>(108490));
}

SCENARIO("D-14", "P1", "an older version after a newer one is refused") {
    QuoteBook b = book_with(quote(4, 108480, 108490, 80000, 40000));
    CHECK_EQ(b.apply_update(quote(3, 108470, 108480, 70000, 30000)), QuoteError::NonMonotonic);
    CHECK_EQ(b.current("EURUSD")->version, static_cast<std::uint64_t>(4));
    // Version zero is not a version.
    CHECK_EQ(b.apply_update(quote(0, 108480, 108490, 80000, 40000)), QuoteError::NonMonotonic);
}

SCENARIO("D-16", "P1", "liquidity is per symbol and never consumed across symbols") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    b.register_symbol("GBPUSD");
    QuoteSlice eur = quote(1, 108480, 108490, 80000, 40000);
    QuoteSlice gbp = quote(1, 125000, 125010, 50000, 25000);
    gbp.symbol = "GBPUSD";
    CHECK_EQ(b.apply_update(eur), QuoteError::Ok);
    CHECK_EQ(b.apply_update(gbp), QuoteError::Ok);

    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 40000), QuoteError::Ok);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));
    // GBPUSD is untouched.
    CHECK_EQ(b.inventory_units("GBPUSD", Side::Buy), static_cast<Units>(25000));
    // And the slice identities differ, so the ledger can tell them apart.
    CHECK_NE(eur.slice_id(Side::Buy), gbp.slice_id(Side::Buy));
}

SCENARIO("D-17", "P0", "only an explicit new quote restores capacity") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 40000), QuoteError::Ok);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));

    // Re-applying the SAME version does not restock: it is an idempotent no-op,
    // so a replayed fixture cannot manufacture liquidity.
    CHECK_EQ(b.apply_update(quote(1, 108480, 108490, 80000, 40000)), QuoteError::Ok);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(0));

    // A new version does.
    CHECK_EQ(b.apply_update(quote(2, 108480, 108490, 80000, 25000)), QuoteError::Ok);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(25000));
}

SCENARIO("D-18", "P1", "a restored quote keeps its REMAINING size, not its original") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    CHECK_EQ(b.consume("EURUSD", 1, Side::Buy, 30000), QuoteError::Ok);
    const std::vector<QuoteSlice> snap = b.snapshot();
    CHECK_EQ(snap.size(), static_cast<std::size_t>(1));
    CHECK_EQ(snap[0].ask_available, static_cast<Units>(10000));

    // A restart restores what was left, so consumed liquidity cannot come back.
    QuoteBook after;
    after.restore(snap);
    CHECK_EQ(after.inventory_units("EURUSD", Side::Buy), static_cast<Units>(10000));

    // And a stale restored quote still cannot back a fill.
    const OrderChain o = order(Side::Buy, 100000, 108500);
    CHECK_EQ(plan_auto_match(o, after, 5000).reason, NoTrade::StaleQuote);
}

SCENARIO("D-15", "P0", "an unknown symbol is refused rather than created on demand") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    QuoteSlice other = quote(1, 108480, 108490, 80000, 40000);
    other.symbol = "USDJPY";
    CHECK_EQ(b.apply_update(other), QuoteError::UnknownSymbol);
    CHECK(b.current("USDJPY") == nullptr);
    CHECK_EQ(b.consume("USDJPY", 1, Side::Buy, 1000), QuoteError::UnknownSymbol);
}

// ---------------------------------------------------------------------------
// The injected-fill gate
// ---------------------------------------------------------------------------

SCENARIO("E-07", "P0", "an injected fill larger than LeavesQty is refused, not clamped") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 200000));
    const OrderChain o = order(Side::Buy, 100000, 108500);

    const MatchPlan p = plan_injected_fill(o, b, 110000, 108490, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::QtyExceedsLeaves);
    // Clamping would have produced a 100000 fill and a silent pass.
    CHECK_EQ(p.qty, static_cast<Units>(0));
}

SCENARIO("E-08", "P0", "a zero or negative injected fill is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500);
    CHECK_EQ(plan_injected_fill(o, b, 0, 108490, 0).reason, NoTrade::QtyNotPositive);
    CHECK_EQ(plan_injected_fill(o, b, -1000, 108490, 0).reason, NoTrade::QtyNotPositive);
}

SCENARIO("E-09", "P0", "an injected fill worse than the limit is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500);
    // 1.08510 is above the buy limit of 1.08500.
    const MatchPlan p = plan_injected_fill(o, b, 10000, 108510, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::PriceWorseThanLimit);
    CHECK_EQ(b.inventory_units("EURUSD", Side::Buy), static_cast<Units>(40000));
}

SCENARIO("E-09b", "P0", "an injected fill at a price no slice quoted is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500);
    // 1.08485 is inside the limit and better than the ask, but nothing is
    // offered there. Accepting it would decouple the fill from the slice it
    // consumes and make the conservation invariant unverifiable.
    const MatchPlan p = plan_injected_fill(o, b, 10000, 108485, 0);
    CHECK(!p.traded);
    CHECK_EQ(p.reason, NoTrade::PriceNotAtQuote);
}

SCENARIO("E-10", "P0", "a fill against a cancelled order is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500, 40000, BaseState::Canceled);
    CHECK_EQ(plan_injected_fill(o, b, 10000, 108490, 0).reason, NoTrade::OrderTerminal);
    CHECK_EQ(plan_auto_match(o, b, 0).reason, NoTrade::OrderTerminal);
}

SCENARIO("E-11", "P0", "a fill against a fully filled order is refused") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    const OrderChain o = order(Side::Buy, 100000, 108500, 100000, BaseState::Filled);
    CHECK_EQ(plan_injected_fill(o, b, 1000, 108490, 0).reason, NoTrade::OrderTerminal);
}

SCENARIO("F-09m", "P0", "a pending cancel does not block a fill") {
    QuoteBook b = book_with(quote(1, 108480, 108490, 80000, 40000));
    OrderChain o = order(Side::Buy, 100000, 108500, 40000, BaseState::PartiallyFilled);
    o.pending = Pending::Cancel;
    o.pending_cl_ord_id = "C1";
    o.leaves_qty = o.expected_leaves();

    // The race in F-09 and G-13 only exists because a fill is still possible
    // while a request is in flight.
    const MatchPlan p = plan_injected_fill(o, b, 20000, 108490, 0);
    CHECK(p.traded);
    CHECK_EQ(p.qty, static_cast<Units>(20000));
}

SCENARIO("D-20", "P1", "an injected fill on an unquoted symbol is refused") {
    QuoteBook b;
    b.register_symbol("EURUSD");
    const OrderChain o = order(Side::Buy, 100000, 108500);
    CHECK_EQ(plan_injected_fill(o, b, 10000, 108490, 0).reason, NoTrade::QuoteMissing);
    CHECK_EQ(plan_auto_match(o, b, 0).reason, NoTrade::QuoteMissing);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "quote"); }
