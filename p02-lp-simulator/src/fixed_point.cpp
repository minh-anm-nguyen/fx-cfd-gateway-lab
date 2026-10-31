#include "lp/fixed_point.hpp"

#include <cstdio>
#include <limits>

namespace lpsim {

std::string_view to_string(NumParse e) {
    switch (e) {
        case NumParse::Ok: return "Ok";
        case NumParse::Empty: return "Empty";
        case NumParse::NotNumeric: return "NotNumeric";
        case NumParse::TooManyDecimals: return "TooManyDecimals";
        case NumParse::Overflow: return "Overflow";
        case NumParse::NotPositive: return "NotPositive";
        case NumParse::FractionalQty: return "FractionalQty";
    }
    return "Unknown";
}

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();

constexpr __int128 int128_max() {
    return static_cast<__int128>((static_cast<unsigned __int128>(-1)) >> 1);
}

std::int64_t pow10_i64(int n) {
    std::int64_t v = 1;
    for (int i = 0; i < n; ++i) v *= 10;
    return v;
}

// Split a decimal literal into sign, integer digits and fraction digits.
// Rejects anything that is not strictly [-]digits[.digits]: no exponent, no
// whitespace, no thousands separators, no bare sign, no second decimal point.
struct Decimal {
    bool negative = false;
    std::string_view int_digits;
    std::string_view frac_digits;
};

bool split_decimal(std::string_view s, Decimal* out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        i = 1;
    }
    const std::size_t int_begin = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    const std::size_t int_end = i;

    std::size_t frac_begin = i;
    std::size_t frac_end = i;
    if (i < s.size() && s[i] == '.') {
        ++i;
        frac_begin = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        frac_end = i;
    }
    if (i != s.size()) return false;                      // trailing junk, e.g. "1e9", "1.0x"
    if (int_end == int_begin && frac_end == frac_begin) return false;  // "", "-", "."

    out->negative = neg;
    out->int_digits = s.substr(int_begin, int_end - int_begin);
    out->frac_digits = s.substr(frac_begin, frac_end - frac_begin);
    return true;
}

// Accumulate digits with overflow checking.
bool digits_to_i64(std::string_view digits, std::int64_t* out) {
    std::int64_t v = 0;
    for (char c : digits) {
        if (v > (kI64Max - (c - '0')) / 10) return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

}  // namespace

NumParse parse_price_ticks(std::string_view s, const PriceFormat& fmt, Ticks* out) {
    if (s.empty()) return NumParse::Empty;
    if (fmt.scale < 0 || fmt.scale > 18) return NumParse::Overflow;

    Decimal d;
    if (!split_decimal(s, &d)) return NumParse::NotNumeric;

    // A price with more precision than the symbol carries is ambiguous. The
    // design doc (B-08) forbids silently rounding it away.
    if (static_cast<int>(d.frac_digits.size()) > fmt.scale) return NumParse::TooManyDecimals;

    std::int64_t int_part = 0;
    if (!digits_to_i64(d.int_digits, &int_part)) return NumParse::Overflow;

    // Scale the integer part, then add the zero-padded fraction.
    const std::int64_t scale_mul = pow10_i64(fmt.scale);
    std::int64_t scaled = 0;
    if (!checked_mul(int_part, scale_mul, &scaled)) return NumParse::Overflow;

    std::int64_t frac_part = 0;
    if (!d.frac_digits.empty()) {
        if (!digits_to_i64(d.frac_digits, &frac_part)) return NumParse::Overflow;
        const int missing = fmt.scale - static_cast<int>(d.frac_digits.size());
        if (!checked_mul(frac_part, pow10_i64(missing), &frac_part)) return NumParse::Overflow;
    }

    std::int64_t total = 0;
    if (!checked_add(scaled, frac_part, &total)) return NumParse::Overflow;

    // A limit price of zero or below cannot be marketable under any quote and
    // is refused here rather than at match time (B-07).
    if (d.negative || total <= 0) return NumParse::NotPositive;

    *out = total;
    return NumParse::Ok;
}

NumParse parse_qty_units(std::string_view s, Units* out) {
    if (s.empty()) return NumParse::Empty;

    Decimal d;
    if (!split_decimal(s, &d)) return NumParse::NotNumeric;

    for (char c : d.frac_digits) {
        if (c != '0') return NumParse::FractionalQty;
    }

    std::int64_t v = 0;
    if (!digits_to_i64(d.int_digits, &v)) return NumParse::Overflow;
    if (d.negative || v <= 0) return NumParse::NotPositive;

    *out = v;
    return NumParse::Ok;
}

std::string format_price(Ticks ticks, int scale) {
    if (scale <= 0) return format_qty(ticks);
    const bool neg = ticks < 0;
    // Negate in the unsigned domain so that INT64_MIN does not overflow.
    unsigned long long mag =
        neg ? (~static_cast<unsigned long long>(ticks) + 1ull)
            : static_cast<unsigned long long>(ticks);

    const unsigned long long div = static_cast<unsigned long long>(pow10_i64(scale));
    const unsigned long long whole = mag / div;
    const unsigned long long frac = mag % div;

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%llu.%0*llu", neg ? "-" : "", whole, scale, frac);
    return std::string(buf);
}

std::string format_qty(Units units) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(units));
    return std::string(buf);
}

bool checked_add(std::int64_t a, std::int64_t b, std::int64_t* out) {
    return !__builtin_add_overflow(a, b, out);
}

bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t* out) {
    return !__builtin_sub_overflow(a, b, out);
}

bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t* out) {
    return !__builtin_mul_overflow(a, b, out);
}

bool NotionalAccumulator::add(Units qty, Ticks price_ticks) {
    if (qty <= 0 || price_ticks <= 0) return false;

    // qty * price in the 128-bit domain, with the magnitude checked rather
    // than assumed to fit (D-19).
    const __int128 q = qty;
    const __int128 p = price_ticks;
    if (q > int128_max() / p) return false;
    const __int128 product = q * p;
    if (notional_ > int128_max() - product) return false;

    Units new_qty = 0;
    if (!checked_add(qty_, qty, &new_qty)) return false;

    notional_ += product;
    qty_ = new_qty;
    return true;
}

Ticks NotionalAccumulator::avg_px_ticks() const {
    if (qty_ <= 0) return 0;
    // Round half away from zero: (2n + d) / 2d for non-negative n.
    const __int128 d = qty_;
    const __int128 doubled = notional_ * 2 + d;
    return static_cast<Ticks>(doubled / (d * 2));
}

}  // namespace lpsim
