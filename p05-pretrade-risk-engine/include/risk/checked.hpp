// Checked 64-bit arithmetic and conservative rounding helpers.
//
// RI-10 (monotone bounding) and RI-13 (checked arithmetic) require that every
// risk quantity be computed with explicit overflow detection and rounded in the
// direction that cannot understate risk. These helpers are the only sanctioned
// way to do arithmetic on units/money inside the engine; raw `+`/`*` on
// std::int64_t is reserved for loop counters and the like.
#pragma once

#include <cstdint>
#include <limits>

#include "risk/wide.hpp"

namespace risk {

#if defined(__GNUC__) || defined(__clang__)
#define RISK_HAS_OVERFLOW_BUILTINS 1
#else
#define RISK_HAS_OVERFLOW_BUILTINS 0
#endif

inline bool add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
#if RISK_HAS_OVERFLOW_BUILTINS
  return !__builtin_add_overflow(a, b, &out);
#else
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  if (b > 0 && a > kMax - b) return false;
  if (b < 0 && a < kMin - b) return false;
  out = a + b;
  return true;
#endif
}

inline bool sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
#if RISK_HAS_OVERFLOW_BUILTINS
  return !__builtin_sub_overflow(a, b, &out);
#else
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  if (b < 0 && a > kMax + b) return false;
  if (b > 0 && a < kMin + b) return false;
  out = a - b;
  return true;
#endif
}

inline bool mul_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
#if RISK_HAS_OVERFLOW_BUILTINS
  return !__builtin_mul_overflow(a, b, &out);
#else
  I128 wide;
  if (!i128_mul(i128_from_i64(a), i128_from_i64(b), wide)) return false;
  return i128_to_i64(wide, out);
#endif
}

// abs() that refuses INT64_MIN instead of returning it unchanged.
inline bool abs_i64(std::int64_t a, std::int64_t& out) noexcept {
  if (a == std::numeric_limits<std::int64_t>::min()) return false;
  out = a < 0 ? -a : a;
  return true;
}

// Ceiling division restricted to a nonnegative numerator and a positive
// denominator. This is the rounding used for every risk charge: notional,
// margin, and conservative bounds (RI-10).
inline bool ceil_div_nonneg_i64(std::int64_t num, std::int64_t den,
                                std::int64_t& out) noexcept {
  if (num < 0 || den <= 0) return false;
  // num + den - 1 can overflow, so do it in 128-bit space.
  I128 wide_num, wide_out;
  if (!i128_add(i128_from_i64(num), i128_from_i64(den - 1), wide_num)) return false;
  if (!i128_div_trunc(wide_num, i128_from_i64(den), wide_out)) return false;
  return i128_to_i64(wide_out, out);
}

// Floor division for a nonnegative numerator / positive denominator. Used for
// headroom (a *lower* bound), never for a charge.
inline bool floor_div_nonneg_i64(std::int64_t num, std::int64_t den,
                                 std::int64_t& out) noexcept {
  if (num < 0 || den <= 0) return false;
  out = num / den;
  return true;
}

// `a * b` accumulated in 128-bit space, kept wide so a chain of multiplications
// (units x price x multiplier x fx) never has to narrow in the middle.
inline bool mul_to_i128(std::int64_t a, std::int64_t b, I128& out) noexcept {
  return i128_mul(i128_from_i64(a), i128_from_i64(b), out);
}

}  // namespace risk
