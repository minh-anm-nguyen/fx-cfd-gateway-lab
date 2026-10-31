// Strict decimal to fixed-point conversion.
//
// Design doc 9.2 is explicit: no scientific notation, no locale separators, no
// NaN, no silent rounding past the tick, and no integer wraparound. Those rules
// are tested directly by OR-A-06..A-10, OR-D-15..D-18 and OR-R-15, so the parser
// here is deliberately pedantic and reports *why* it refused.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "router/ids.hpp"
#include "router/result.hpp"

namespace router {

enum class NumError {
    None = 0,
    Empty,
    NotDigit,        // letter, sign, space, or an exponent marker    OR-D-15
    Separator,       // locale comma or apostrophe grouping           OR-D-16
    TooManyDigits,   // would not fit in int64                        OR-A-08 OR-D-18
    Overflow,
    TooPrecise,      // more fraction digits than the tick scale      OR-A-10
    Negative,        // a signed value where none is allowed          OR-A-07 OR-D-17
    Zero,            // zero where a positive value is required       OR-A-06
    NotOnTick,       // not an integral multiple of the tick          OR-A-10
    NotOnStep,       // quantity not a multiple of the lot step       OR-B-14
    OutOfRange,      // outside the configured min/max
};

std::string_view to_string(NumError e);

// Parses an unsigned integer with no separators and no sign. `max_digits` keeps
// the bounded-parser promise of OR-D-13 and OR-D-18.
Result<std::int64_t, NumError> parse_u63(std::string_view text, int max_digits = 18);

// Parses a decimal such as "1.08500" into a count of ticks given `scale`
// fraction digits. Trailing zeros are allowed, extra precision is not.
// Accepts "1.085" for scale 5 (it is exactly representable) but refuses
// "1.085001", which would need scale 6.
Result<std::int64_t, NumError> parse_decimal_ticks(std::string_view text, int scale,
                                                   int max_digits = 18);

// Renders a tick count back to FIX decimal form with no trailing-zero trimming
// beyond `scale`, so an encode/decode round trip is byte stable (OR-D-01).
std::string format_decimal_ticks(std::int64_t ticks, int scale);

// Signed variants, used only where FIX genuinely permits a signed value. They
// still refuse exponents and separators.
Result<std::int64_t, NumError> parse_signed_decimal_ticks(std::string_view text, int scale,
                                                          int max_digits = 18);

// Checked arithmetic used by the ledger math. Design doc R-I10 forbids clamping,
// so these report failure instead of saturating.
bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out);
bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out);

}  // namespace router
