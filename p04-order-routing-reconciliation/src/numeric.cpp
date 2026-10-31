#include "router/numeric.hpp"

#include <limits>

namespace router {

std::string_view to_string(NumError e) {
    switch (e) {
        case NumError::None: return "None";
        case NumError::Empty: return "Empty";
        case NumError::NotDigit: return "NotDigit";
        case NumError::Separator: return "Separator";
        case NumError::TooManyDigits: return "TooManyDigits";
        case NumError::Overflow: return "Overflow";
        case NumError::TooPrecise: return "TooPrecise";
        case NumError::Negative: return "Negative";
        case NumError::Zero: return "Zero";
        case NumError::NotOnTick: return "NotOnTick";
        case NumError::NotOnStep: return "NotOnStep";
        case NumError::OutOfRange: return "OutOfRange";
    }
    return "Unknown";
}

namespace {

using NumResult = Result<std::int64_t, NumError>;

// Everything that is not a plain ASCII digit is classified, so the caller can
// tell an exponent apart from a thousands separator in its error message.
NumError classify_bad_char(char c) {
    if (c == ',' || c == '\'' || c == ' ' || c == '_') return NumError::Separator;
    if (c == '-' || c == '+') return NumError::Negative;
    return NumError::NotDigit;
}

}  // namespace

Result<std::int64_t, NumError> parse_u63(std::string_view text, int max_digits) {
    if (text.empty()) return NumResult::err(NumError::Empty);
    if (static_cast<int>(text.size()) > max_digits) return NumResult::err(NumError::TooManyDigits);
    std::int64_t acc = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return NumResult::err(classify_bad_char(c));
        std::int64_t scaled = 0;
        if (!checked_mul(acc, 10, scaled)) return NumResult::err(NumError::Overflow);
        if (!checked_add(scaled, c - '0', acc)) return NumResult::err(NumError::Overflow);
    }
    return NumResult::ok(acc);
}

Result<std::int64_t, NumError> parse_decimal_ticks(std::string_view text, int scale,
                                                   int max_digits) {
    if (text.empty()) return NumResult::err(NumError::Empty);
    if (static_cast<int>(text.size()) > max_digits + 2)
        return NumResult::err(NumError::TooManyDigits);

    // A leading sign is never acceptable here. OR-D-17 feeds "-100" and expects a
    // refusal rather than a negative quantity reaching the reducer.
    if (text.front() == '-' || text.front() == '+') return NumResult::err(NumError::Negative);

    const std::size_t dot = text.find('.');
    std::string_view int_part = dot == std::string_view::npos ? text : text.substr(0, dot);
    std::string_view frac_part =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);

    // A second dot, or an exponent marker, lands in the character scan below.
    if (frac_part.find('.') != std::string_view::npos) return NumResult::err(NumError::NotDigit);
    if (int_part.empty() && frac_part.empty()) return NumResult::err(NumError::Empty);
    if (int_part.empty()) return NumResult::err(NumError::NotDigit);  // ".5" is not FIX decimal
    if (dot != std::string_view::npos && frac_part.empty())
        return NumResult::err(NumError::NotDigit);  // "1." is not FIX decimal

    // The digit budget is checked on the integer part itself, so an absurd
    // input is refused as too long instead of overflowing first (OR-A-08).
    if (static_cast<int>(int_part.size()) > max_digits)
        return NumResult::err(NumError::TooManyDigits);

    if (static_cast<int>(frac_part.size()) > scale) {
        // "1.085001" against a 0.00001 tick. Rounding here would quietly change a
        // customer price, which R-I10 forbids outright.
        for (char c : frac_part)
            if (c < '0' || c > '9') return NumResult::err(classify_bad_char(c));
        return NumResult::err(NumError::TooPrecise);
    }

    std::int64_t acc = 0;
    for (char c : int_part) {
        if (c < '0' || c > '9') return NumResult::err(classify_bad_char(c));
        std::int64_t scaled = 0;
        if (!checked_mul(acc, 10, scaled)) return NumResult::err(NumError::Overflow);
        if (!checked_add(scaled, c - '0', acc)) return NumResult::err(NumError::Overflow);
    }
    for (int i = 0; i < scale; ++i) {
        std::int64_t scaled = 0;
        if (!checked_mul(acc, 10, scaled)) return NumResult::err(NumError::Overflow);
        acc = scaled;
        const int digit = i < static_cast<int>(frac_part.size()) ? frac_part[static_cast<std::size_t>(i)] - '0' : 0;
        if (i < static_cast<int>(frac_part.size())) {
            const char c = frac_part[static_cast<std::size_t>(i)];
            if (c < '0' || c > '9') return NumResult::err(classify_bad_char(c));
        }
        if (!checked_add(acc, digit, acc)) return NumResult::err(NumError::Overflow);
    }
    return NumResult::ok(acc);
}

Result<std::int64_t, NumError> parse_signed_decimal_ticks(std::string_view text, int scale,
                                                          int max_digits) {
    if (text.empty()) return NumResult::err(NumError::Empty);
    bool neg = false;
    if (text.front() == '-') {
        neg = true;
        text.remove_prefix(1);
    }
    auto r = parse_decimal_ticks(text, scale, max_digits);
    if (r.is_err()) return r;
    return NumResult::ok(neg ? -r.value() : r.value());
}

std::string format_decimal_ticks(std::int64_t ticks, int scale) {
    const bool neg = ticks < 0;
    // Negated through unsigned so INT64_MIN does not trap.
    std::uint64_t mag = neg ? (~static_cast<std::uint64_t>(ticks) + 1u)
                            : static_cast<std::uint64_t>(ticks);
    std::string digits;
    if (mag == 0) {
        digits = "0";
    } else {
        while (mag > 0) {
            digits.push_back(static_cast<char>('0' + (mag % 10)));
            mag /= 10;
        }
        std::string rev(digits.rbegin(), digits.rend());
        digits = rev;
    }
    if (scale <= 0) return (neg ? "-" : "") + digits;
    while (static_cast<int>(digits.size()) <= scale) digits.insert(digits.begin(), '0');
    std::string out = digits.substr(0, digits.size() - static_cast<std::size_t>(scale));
    out.push_back('.');
    out += digits.substr(digits.size() - static_cast<std::size_t>(scale));
    return (neg ? "-" : "") + out;
}

bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) return false;
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) return false;
    out = a + b;
    return true;
}

bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    // Checked before multiplying: signed overflow is undefined behaviour, so the
    // usual "multiply then divide back" trick is not safe here.
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (a > 0) {
        if (b > 0) {
            if (a > kMax / b) return false;
        } else {
            if (b < kMin / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < kMin / b) return false;
        } else {
            if (a < kMax / b) return false;
        }
    }
    out = a * b;
    return true;
}

}  // namespace router
