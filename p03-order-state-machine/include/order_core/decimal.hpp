// Canonical decimal text <-> fixed-point conversion.
//
// This is the only place a decimal string becomes a number. It is deliberately
// strict: the what-if matrix group D treats locale commas, scientific
// notation, NaN/Inf, embedded whitespace, control bytes and off-tick precision
// as rejections, not as inputs to be repaired. There is no strtod anywhere in
// the project.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "order_core/math.hpp"

namespace order_core {

enum class DecimalError {
    None,
    Empty,
    NotCanonical,   // sign/digit/dot grammar violated, or stray bytes
    TooManyDigits,  // would overflow the mantissa before scaling
    Overflow,       // scaling overflowed the target representation
    OffTick,        // exact value is not an integer multiple of the tick
    NegativeTick,   // tick numerator/denominator not strictly positive
};

const char* to_string(DecimalError e);

// A decimal literal held exactly: value == mantissa / 10^scale.
struct Decimal {
    int64_t mantissa = 0;
    uint32_t scale = 0;
    bool negative = false;  // kept separately so "-0.0" is still recognisably signed
};

struct DecimalParse {
    DecimalError error = DecimalError::None;
    Decimal value{};
    [[nodiscard]] bool ok() const { return error == DecimalError::None; }
};

// Accepts exactly: [ '-' | '+' ] digit+ [ '.' digit+ ]
// Rejects: empty, lone sign, lone dot, two dots, ',' as a separator, 'e'/'E'
// exponents, "nan"/"inf", leading/trailing/embedded spaces or tabs, NUL or any
// other byte outside the grammar. At most 18 significant digits so the
// mantissa cannot overflow before scaling.
DecimalParse parse_decimal(std::string_view text);

// Convert a parsed decimal to an integer tick count where one tick equals
// tick_numerator / tick_denominator. Fails with OffTick when the value is not
// an exact multiple, so 1.085001 against a 0.00001 tick is refused rather than
// rounded (what-if D-03).
DecimalParse decimal_to_ticks(const Decimal& d, int64_t tick_numerator, int64_t tick_denominator,
                              int64_t& out_ticks);

// Text -> ticks in one step.
DecimalError parse_ticks(std::string_view text, int64_t tick_numerator, int64_t tick_denominator,
                         int64_t& out_ticks);

// Text -> integer base units, rejecting any fractional part. Quantities in
// this profile are whole base units (design doc 9.1).
DecimalError parse_units(std::string_view text, int64_t& out_units);

// ticks -> canonical decimal text. Round-trips with parse_ticks for every
// value this library accepts (what-if D-01).
std::string ticks_to_text(int64_t ticks, int64_t tick_numerator, int64_t tick_denominator);

}  // namespace order_core
