// Matching policy: decides whether a trade may happen, and for how much.
//
// Reference: design doc 8.1 to 8.3. Two fill-trigger policies exist and
// exactly one is active at a time (the doc is explicit that enabling both would
// let a fixture auto-fill at New and then inject the same fill again):
//
//   scripted            - a trade happens only when a scenario injects one.
//   auto_on_new_or_quote - the matcher fires on New and on every quote update.
//
// Both go through the same safety gate. A scripted injection is not a back door:
// design doc 14.2 requires it to validate the book, the price and the quantity
// exactly as production-like matching would, so the only thing the scenario
// controls is WHEN a trade is attempted, never WHETHER the invariants hold.
//
// Decisions pinned here because the matrix asserts them:
//
//   * A buyer may only take ask liquidity at or below its limit, a seller only
//     bid liquidity at or above its limit (invariant I-11, D-01 to D-04).
//   * An injected fill price must equal the quoted price for the side being
//     taken. Letting a scenario name an arbitrary better price would decouple
//     the fill from the liquidity slice it consumes and make the conservation
//     invariant I-12 unverifiable.
//   * An oversized fill is REFUSED, not clamped to LeavesQty (E-07). Clamping
//     would turn a fixture bug into a silent pass.
//   * A pending cancel or replace does NOT block a fill: that race is the point
//     of F-09 and G-13.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lp/domain.hpp"
#include "lp/quote_book.hpp"

namespace lpsim {

enum class NoTrade {
    None = 0,
    QuoteMissing,          // symbol never quoted
    StaleQuote,            // outside [valid_from, expires_at)        D-06, D-07
    NotMarketable,         // quote on the wrong side of the limit    D-02, D-04
    NoLiquidity,           // nothing available on the taken side     D-05
    OrderTerminal,         // Filled / Canceled / Expired / Rejected  E-10, E-11
    QtyNotPositive,        // injected zero or negative quantity      E-08
    QtyExceedsLeaves,      // injected more than LeavesQty            E-07
    PriceWorseThanLimit,   // injected price violates the limit       E-09
    PriceNotAtQuote,       // injected price is not the quoted price
    VersionMismatch,       // the pinned quote version is no longer current D-12
    Overflow,              // checked arithmetic refused the result   D-19
};

std::string_view to_string(NoTrade r);

struct MatchPlan {
    bool traded = false;
    Units qty = 0;
    Ticks px_ticks = 0;
    std::uint64_t quote_version = 0;
    std::string slice_id;
    NoTrade reason = NoTrade::None;
};

// A buyer is marketable when the ask is at or below its limit; a seller when
// the bid is at or above its limit.
bool marketable(Side taker_side, Ticks quote_px, Ticks limit_ticks);

// auto_on_new_or_quote policy: take min(LeavesQty, available) at the quoted
// price. Mutates nothing; the caller commits the plan inside TX-B.
MatchPlan plan_auto_match(const OrderChain& o, const QuoteBook& book, std::int64_t now_ms);

// scripted policy: the scenario names the quantity and price, and every safety
// check above still applies.
MatchPlan plan_injected_fill(const OrderChain& o, const QuoteBook& book, Units qty,
                             Ticks px_ticks, std::int64_t now_ms);

}  // namespace lpsim
