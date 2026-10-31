#include "lp/matcher.hpp"

namespace lpsim {

std::string_view to_string(NoTrade r) {
    switch (r) {
        case NoTrade::None: return "None";
        case NoTrade::QuoteMissing: return "QuoteMissing";
        case NoTrade::StaleQuote: return "StaleQuote";
        case NoTrade::NotMarketable: return "NotMarketable";
        case NoTrade::NoLiquidity: return "NoLiquidity";
        case NoTrade::OrderTerminal: return "OrderTerminal";
        case NoTrade::QtyNotPositive: return "QtyNotPositive";
        case NoTrade::QtyExceedsLeaves: return "QtyExceedsLeaves";
        case NoTrade::PriceWorseThanLimit: return "PriceWorseThanLimit";
        case NoTrade::PriceNotAtQuote: return "PriceNotAtQuote";
        case NoTrade::VersionMismatch: return "VersionMismatch";
        case NoTrade::Overflow: return "Overflow";
    }
    return "Unknown";
}

bool marketable(Side taker_side, Ticks quote_px, Ticks limit_ticks) {
    if (quote_px <= 0 || limit_ticks <= 0) return false;
    return taker_side == Side::Buy ? (quote_px <= limit_ticks) : (quote_px >= limit_ticks);
}

namespace {

// Checks common to both policies, performed before any quantity arithmetic.
// Returns NoTrade::None when the order and the book are jointly eligible, and
// publishes the usable quote through *q.
NoTrade gate(const OrderChain& o, const QuoteBook& book, std::int64_t now_ms,
             const QuoteSlice** q) {
    if (is_terminal(o.base)) return NoTrade::OrderTerminal;

    const QuoteSlice* quote = book.current(o.symbol);
    if (quote == nullptr) return NoTrade::QuoteMissing;
    if (!quote->usable_at(now_ms)) return NoTrade::StaleQuote;

    const Ticks px = quote->price_for(o.side);
    if (px <= 0) return NoTrade::QuoteMissing;
    if (!marketable(o.side, px, o.limit_ticks)) return NoTrade::NotMarketable;

    *q = quote;
    return NoTrade::None;
}

}  // namespace

MatchPlan plan_auto_match(const OrderChain& o, const QuoteBook& book, std::int64_t now_ms) {
    MatchPlan plan;
    const QuoteSlice* quote = nullptr;
    plan.reason = gate(o, book, now_ms, &quote);
    if (plan.reason != NoTrade::None) return plan;

    const Units leaves = o.expected_leaves();
    if (leaves <= 0) {
        plan.reason = NoTrade::NoLiquidity;
        return plan;
    }
    const Units available = quote->available_for(o.side);
    if (available <= 0) {
        plan.reason = NoTrade::NoLiquidity;
        return plan;
    }

    plan.qty = checked_min(leaves, available);
    plan.px_ticks = quote->price_for(o.side);
    plan.quote_version = quote->version;
    plan.slice_id = quote->slice_id(o.side);

    // Guard the weighted notional the fill will contribute before promising it.
    NotionalAccumulator probe;
    if (!probe.add(plan.qty, plan.px_ticks)) {
        plan.qty = 0;
        plan.reason = NoTrade::Overflow;
        return plan;
    }

    plan.traded = true;
    return plan;
}

MatchPlan plan_injected_fill(const OrderChain& o, const QuoteBook& book, Units qty,
                             Ticks px_ticks, std::int64_t now_ms) {
    MatchPlan plan;
    if (qty <= 0) {
        plan.reason = NoTrade::QtyNotPositive;
        return plan;
    }

    const QuoteSlice* quote = nullptr;
    plan.reason = gate(o, book, now_ms, &quote);
    if (plan.reason != NoTrade::None) return plan;

    // The injected price must be the price the counterparty is actually
    // quoting on the side being taken. Checking the limit alone would let a
    // scenario fill at a price no slice ever offered.
    if (px_ticks != quote->price_for(o.side)) {
        // Distinguish "worse than my limit" from "not the quoted price": the
        // first is a business-rule violation the matrix names separately (E-09).
        plan.reason = marketable(o.side, px_ticks, o.limit_ticks) ? NoTrade::PriceNotAtQuote
                                                                  : NoTrade::PriceWorseThanLimit;
        return plan;
    }

    const Units leaves = o.expected_leaves();
    if (qty > leaves) {
        plan.reason = NoTrade::QtyExceedsLeaves;
        return plan;
    }
    if (qty > quote->available_for(o.side)) {
        plan.reason = NoTrade::NoLiquidity;
        return plan;
    }

    NotionalAccumulator probe;
    if (!probe.add(qty, px_ticks)) {
        plan.reason = NoTrade::Overflow;
        return plan;
    }

    plan.traded = true;
    plan.qty = qty;
    plan.px_ticks = px_ticks;
    plan.quote_version = quote->version;
    plan.slice_id = quote->slice_id(o.side);
    return plan;
}

}  // namespace lpsim
