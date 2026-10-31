// Exact rational arithmetic for tick sizes, contract multipliers, FX rates and
// margin rates.
//
// The spec forbids floating point anywhere on the risk path (§8), so every
// scaling factor is carried as an exact fraction and the final narrowing to
// minor currency units rounds *up* (RI-10). `RatioAcc` exists so a chain of
// multiplications (units x ticks x tick_size x multiplier x fx x 10^scale) can
// be accumulated without an intermediate narrowing, reducing by gcd at each
// step to keep the 128-bit headroom usable.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "risk/wide.hpp"

namespace risk {

// A reduced fraction with a strictly positive denominator.
struct Rational {
  std::int64_t num = 0;
  std::int64_t den = 1;

  constexpr Rational() = default;
  constexpr Rational(std::int64_t n, std::int64_t d) : num(n), den(d) {}

  bool valid() const noexcept { return den > 0; }
  bool positive() const noexcept { return den > 0 && num > 0; }
};

// Reduce in place; fails on den == 0 or on the INT64_MIN edge case.
bool rational_reduce(Rational& r) noexcept;

// Parses "num/den" or a bare integer ("3" == 3/1). Rejects zero/negative
// denominators, leading '+', whitespace, and anything that does not consume the
// whole string -- malformed instrument metadata must not silently become 1/1.
bool rational_parse(std::string_view text, Rational& out) noexcept;

std::string rational_to_string(const Rational& r);

// Exact comparison a <=> b without overflow (cross-multiplies in 128-bit).
// Returns false if the comparison itself cannot be computed.
bool rational_compare(const Rational& a, const Rational& b, int& out) noexcept;

bool rational_mul(const Rational& a, const Rational& b, Rational& out) noexcept;

// ---------------------------------------------------------------------------
// RatioAcc: a 128-bit exact fraction accumulator.
// ---------------------------------------------------------------------------
class RatioAcc {
 public:
  RatioAcc() = default;
  explicit RatioAcc(std::int64_t value)
      : num_(i128_from_i64(value)), den_(i128_from_i64(1)) {}

  // All mutators return false on overflow and leave the accumulator usable but
  // poisoned; callers must abort the whole valuation (RI-13).
  bool mul_i64(std::int64_t v) noexcept;
  bool mul_rational(const Rational& r) noexcept;
  bool div_i64(std::int64_t v) noexcept;

  // Narrow to int64, rounding away from zero on the positive side only.
  // Requires a nonnegative value: a risk charge is never negative (RI-10).
  bool ceil_to_i64_nonneg(std::int64_t& out) const noexcept;

  // Narrow rounding toward negative infinity. Accepts negative values; used for
  // headroom and for signed mark-to-market style quantities.
  bool floor_to_i64(std::int64_t& out) const noexcept;

  bool is_negative() const noexcept;

  I128 numerator() const noexcept { return num_; }
  I128 denominator() const noexcept { return den_; }
  std::string to_string() const;

 private:
  void reduce() noexcept;

  I128 num_ = i128_from_i64(0);
  I128 den_ = i128_from_i64(1);
};

// 128-bit gcd of magnitudes; returns 1 when either side is zero.
I128 i128_gcd(I128 a, I128 b) noexcept;

}  // namespace risk
