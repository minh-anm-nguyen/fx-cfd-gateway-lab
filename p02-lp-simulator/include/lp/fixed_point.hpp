// Fixed-point quantity and price arithmetic.
//
// Design doc 6.4 is categorical: quantities are integral base-currency units
// and prices are integral ticks. Nothing in this project converts a FIX decimal
// through a binary floating-point type, because the resulting rounding would
// make the conservation invariants (I-03, I-11, I-12) untestable.
//
// Scenarios driven directly from this file: B-06 (zero/negative qty),
// B-07 (non-numeric price), B-08 (off-tick), B-09 (qty beyond int64),
// D-19 (overflow in the weighted notional sum), E-04 and E-19 (AvgPx).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lpsim {

// Quantity in base-currency units, for example 100000 for EUR 100,000.
using Units = std::int64_t;

// Price as an integer count of 10^-scale increments. With scale 5,
// 1.08500 is 108500. The configured tick_step says which of those integers
// the counterparty actually accepts (design doc 3.3).
using Ticks = std::int64_t;

struct PriceFormat {
    int scale = 5;             // decimal digits carried on the wire
    std::int64_t tick_step = 1;  // minimum increment, in 10^-scale units
};

// Why a numeric field was refused. Every one of these is a named reject rather
// than a coerced value (B-07 must not parse to zero).
enum class NumParse {
    Ok = 0,
    Empty,
    NotNumeric,        // letters, exponent notation, stray signs
    TooManyDecimals,   // more fractional digits than the symbol scale
    Overflow,          // does not fit in the fixed-point domain
    NotPositive,       // zero or negative where only positive is legal
    FractionalQty,     // a non-zero fraction on an integral quantity
};

std::string_view to_string(NumParse e);

// Parse a FIX decimal price into ticks. Rejects exponent notation, embedded
// spaces, a bare sign, multiple decimal points and anything that does not fit
// the scale. Does NOT check tick_step: that is a separate business decision so
// the reject reason can distinguish a malformed price from an off-tick one.
NumParse parse_price_ticks(std::string_view s, const PriceFormat& fmt, Ticks* out);

// Parse a FIX quantity into base units. A trailing all-zero fraction is
// accepted ("100000.00"); a non-zero fraction is refused because v1 quantities
// are integral (design doc 6.4).
NumParse parse_qty_units(std::string_view s, Units* out);

// Render a price with exactly fmt.scale decimals, suitable for tags 44 and 31.
std::string format_price(Ticks ticks, int scale);

// Render a quantity for tags 38, 14, 151 and 32.
std::string format_qty(Units units);

inline bool on_tick(Ticks t, const PriceFormat& fmt) {
    return fmt.tick_step > 0 && (t % fmt.tick_step) == 0;
}

inline bool on_lot_step(Units q, Units lot_step) {
    return lot_step > 0 && (q % lot_step) == 0;
}

// ---------------------------------------------------------------------------
// Checked integer arithmetic. Every quantity mutation in the order actor goes
// through these; an unchecked += would turn an overfill into a wraparound that
// no invariant check could see afterwards.
// ---------------------------------------------------------------------------
bool checked_add(std::int64_t a, std::int64_t b, std::int64_t* out);
bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t* out);
bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t* out);

inline Units checked_min(Units a, Units b) { return a < b ? a : b; }

// ---------------------------------------------------------------------------
// Weighted average price.
//
// AvgPx is never stored as an incrementally updated rounded value: it is
// recomputed from the fill ledger, which is what lets startup recovery compare
// a snapshot against the ledger and HALT on a mismatch (E-13, J-17).
//
// Rounding is round-half-away-from-zero at the 10^-scale increment, stated here
// rather than inherited from a library default, so that E-19 has a fixed
// expected value.
//
// On the overflow guards: add() checks both the 128-bit notional sum and the
// int64 quantity sum. With int64 quantities and prices the quantity sum is
// always the binding one, because the notional cannot exceed about 8.5e37
// against a domain of 1.7e38. The notional check is therefore defence in depth
// today and the real constraint if the quantity type ever widens; the D-19 test
// asserts which of the two actually fires rather than assuming.
// ---------------------------------------------------------------------------
class NotionalAccumulator {
public:
    // Returns false on overflow, in which case the accumulator is unchanged and
    // the caller must refuse the fill rather than publish a corrupt AvgPx.
    bool add(Units qty, Ticks price_ticks);

    Units qty() const { return qty_; }
    bool empty() const { return qty_ == 0; }

    // Zero when no quantity has accumulated: a New report carries 6=0
    // (design doc 7.4), not an undefined value.
    Ticks avg_px_ticks() const;

private:
    __int128 notional_ = 0;  // sum of qty * price_ticks
    Units qty_ = 0;
};

}  // namespace lpsim
