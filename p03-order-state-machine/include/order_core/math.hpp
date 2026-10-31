// Checked integer arithmetic for order quantities, prices and notional.
//
// Design doc invariant I12: no double/float ever touches a price, a quantity
// or a notional. Every operation that can overflow returns a bool instead of
// wrapping, because a wrapped CumQty is an overfill that no later check can
// detect (what-if C-15, C-16, D-20).
#pragma once

#include <cstdint>
#include <string>

namespace order_core {

// Wide intermediate for qty x price. __int128 is a GCC/Clang extension and not
// ISO C++20; the design doc calls this out (section 15). The fallback keeps the
// library buildable elsewhere by refusing wide products rather than wrapping.
#if defined(__SIZEOF_INT128__)
// Pedantic mode is right that this is not ISO C++; the design doc says so too
// (section 15). The warning is acknowledged here rather than left to repeat in
// every translation unit that includes this header.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
using Int128 = __int128;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
inline constexpr bool kHasWideInt = true;
#else
using Int128 = int64_t;
inline constexpr bool kHasWideInt = false;
#endif

inline constexpr int64_t kInt64Max = INT64_MAX;
inline constexpr int64_t kInt64Min = INT64_MIN;

[[nodiscard]] inline bool checked_add(int64_t a, int64_t b, int64_t& out) noexcept {
    return !__builtin_add_overflow(a, b, &out);
}

[[nodiscard]] inline bool checked_sub(int64_t a, int64_t b, int64_t& out) noexcept {
    return !__builtin_sub_overflow(a, b, &out);
}

[[nodiscard]] inline bool checked_mul(int64_t a, int64_t b, int64_t& out) noexcept {
    return !__builtin_mul_overflow(a, b, &out);
}

// qty_units x price_ticks. The product of two int64 values near their limits
// does not fit in int64, so the result is the wide type. Still checked: with
// kHasWideInt the only failure is a 128-bit overflow, which needs truly absurd
// inputs, but refusing it beats wrapping silently.
[[nodiscard]] inline bool checked_mul_wide(int64_t a, int64_t b, Int128& out) noexcept {
    return !__builtin_mul_overflow(static_cast<Int128>(a), static_cast<Int128>(b), &out);
}

[[nodiscard]] inline bool checked_add_wide(Int128 a, Int128 b, Int128& out) noexcept {
    return !__builtin_add_overflow(a, b, &out);
}

// Floor division that stays correct for negative numerators, unlike the
// truncation C++ gives for `/`.
[[nodiscard]] inline Int128 floor_div(Int128 n, Int128 d) noexcept {
    Int128 q = n / d;
    if ((n % d != 0) && ((n < 0) != (d < 0))) --q;
    return q;
}

// Round-half-to-even, used only at a serialization boundary (design doc 9.1:
// never round between fills). Deterministic for the same inputs regardless of
// thread count or accumulation order, which what-if D-18 requires.
[[nodiscard]] inline bool divide_round_half_even(Int128 numerator, Int128 denominator,
                                                 Int128& out) noexcept {
    if (denominator == 0) return false;
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const Int128 q = floor_div(numerator, denominator);
    const Int128 rem = numerator - q * denominator;  // 0 <= rem < denominator
    const Int128 twice = rem * 2;
    if (twice > denominator) {
        out = q + 1;
    } else if (twice < denominator) {
        out = q;
    } else {
        out = (q % 2 == 0) ? q : q + 1;  // exact half: pick the even neighbour
    }
    return true;
}

[[nodiscard]] inline bool int128_fits_int64(Int128 v) noexcept {
    return v >= static_cast<Int128>(kInt64Min) && v <= static_cast<Int128>(kInt64Max);
}

// Decimal rendering of the wide type for audit records and failure messages.
std::string to_string(Int128 v);

}  // namespace order_core
