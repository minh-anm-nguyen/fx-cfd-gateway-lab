#include "order_core/decimal.hpp"

#include <algorithm>

namespace order_core {

const char* to_string(DecimalError e) {
    switch (e) {
        case DecimalError::None: return "None";
        case DecimalError::Empty: return "Empty";
        case DecimalError::NotCanonical: return "NotCanonical";
        case DecimalError::TooManyDigits: return "TooManyDigits";
        case DecimalError::Overflow: return "Overflow";
        case DecimalError::OffTick: return "OffTick";
        case DecimalError::NegativeTick: return "NegativeTick";
    }
    return "Unknown";
}

namespace {

constexpr uint32_t kMaxSignificantDigits = 18;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool pow10(uint32_t exp, int64_t& out) {
    out = 1;
    for (uint32_t i = 0; i < exp; ++i) {
        if (!checked_mul(out, 10, out)) return false;
    }
    return true;
}

}  // namespace

DecimalParse parse_decimal(std::string_view text) {
    DecimalParse r;
    if (text.empty()) {
        r.error = DecimalError::Empty;
        return r;
    }

    size_t i = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = (text[0] == '-');
        i = 1;
    }

    // Integer part: at least one digit, no separators of any kind.
    const size_t int_begin = i;
    while (i < text.size() && is_digit(text[i])) ++i;
    const size_t int_digits = i - int_begin;
    if (int_digits == 0) {
        r.error = DecimalError::NotCanonical;
        return r;
    }

    size_t frac_begin = i;
    size_t frac_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        frac_begin = i;
        while (i < text.size() && is_digit(text[i])) ++i;
        frac_digits = i - frac_begin;
        if (frac_digits == 0) {  // trailing dot, "1."
            r.error = DecimalError::NotCanonical;
            return r;
        }
    }

    // Anything left over (a second dot, a comma, 'e', whitespace, a NUL byte,
    // a UTF-8 lead byte) makes the literal non-canonical.
    if (i != text.size()) {
        r.error = DecimalError::NotCanonical;
        return r;
    }

    if (int_digits + frac_digits > kMaxSignificantDigits) {
        r.error = DecimalError::TooManyDigits;
        return r;
    }

    int64_t mantissa = 0;
    for (size_t k = int_begin; k < int_begin + int_digits; ++k) {
        mantissa = mantissa * 10 + (text[k] - '0');
    }
    for (size_t k = frac_begin; k < frac_begin + frac_digits; ++k) {
        mantissa = mantissa * 10 + (text[k] - '0');
    }

    r.value.mantissa = negative ? -mantissa : mantissa;
    r.value.scale = static_cast<uint32_t>(frac_digits);
    r.value.negative = negative;
    return r;
}

DecimalParse decimal_to_ticks(const Decimal& d, int64_t tick_numerator, int64_t tick_denominator,
                              int64_t& out_ticks) {
    DecimalParse r;
    r.value = d;
    if (tick_numerator <= 0 || tick_denominator <= 0) {
        r.error = DecimalError::NegativeTick;
        return r;
    }

    // value = mantissa / 10^scale, tick = num / den
    // ticks = value / tick = mantissa * den / (10^scale * num), exact only.
    int64_t scale_factor = 0;
    if (!pow10(d.scale, scale_factor)) {
        r.error = DecimalError::Overflow;
        return r;
    }

    Int128 numerator = 0;
    if (!checked_mul_wide(d.mantissa, tick_denominator, numerator)) {
        r.error = DecimalError::Overflow;
        return r;
    }
    Int128 denominator = 0;
    if (!checked_mul_wide(scale_factor, tick_numerator, denominator)) {
        r.error = DecimalError::Overflow;
        return r;
    }
    if (denominator == 0) {
        r.error = DecimalError::NegativeTick;
        return r;
    }
    if (numerator % denominator != 0) {
        r.error = DecimalError::OffTick;
        return r;
    }

    const Int128 ticks = numerator / denominator;
    if (!int128_fits_int64(ticks)) {
        r.error = DecimalError::Overflow;
        return r;
    }
    out_ticks = static_cast<int64_t>(ticks);
    return r;
}

DecimalError parse_ticks(std::string_view text, int64_t tick_numerator, int64_t tick_denominator,
                         int64_t& out_ticks) {
    const DecimalParse p = parse_decimal(text);
    if (!p.ok()) return p.error;
    return decimal_to_ticks(p.value, tick_numerator, tick_denominator, out_ticks).error;
}

DecimalError parse_units(std::string_view text, int64_t& out_units) {
    const DecimalParse p = parse_decimal(text);
    if (!p.ok()) return p.error;
    if (p.value.scale != 0) {
        // A fractional quantity in a base-unit profile is a contract mismatch,
        // not something to truncate.
        int64_t scale_factor = 0;
        if (!pow10(p.value.scale, scale_factor)) return DecimalError::Overflow;
        if (p.value.mantissa % scale_factor != 0) return DecimalError::OffTick;
        out_units = p.value.mantissa / scale_factor;
        return DecimalError::None;
    }
    out_units = p.value.mantissa;
    return DecimalError::None;
}

std::string ticks_to_text(int64_t ticks, int64_t tick_numerator, int64_t tick_denominator) {
    if (tick_numerator <= 0 || tick_denominator <= 0) return "<bad-tick>";

    // value = ticks * num / den. Render with just enough decimal places to be
    // exact when den is a power of ten, which is the FX case this lab pins.
    Int128 scaled = static_cast<Int128>(ticks) * tick_numerator;
    const bool negative = scaled < 0;
    if (negative) scaled = -scaled;

    uint32_t places = 0;
    int64_t den = tick_denominator;
    while (den % 10 == 0 && places < 18) {
        den /= 10;
        ++places;
    }
    if (den != 1) {
        // Not a decimal tick; emit an exact rational so nothing is silently lost.
        return to_string(static_cast<Int128>(ticks) * tick_numerator) + "/" +
               std::to_string(tick_denominator);
    }

    Int128 whole_den = 1;
    for (uint32_t i = 0; i < places; ++i) whole_den *= 10;
    const Int128 whole = scaled / whole_den;
    const Int128 frac = scaled % whole_den;

    std::string out;
    if (negative) out.push_back('-');
    out += to_string(whole);
    if (places > 0) {
        const std::string f = to_string(frac);
        out.push_back('.');
        out.append(places - std::min<size_t>(places, f.size()), '0');
        out += f;
    }
    return out;
}

}  // namespace order_core
