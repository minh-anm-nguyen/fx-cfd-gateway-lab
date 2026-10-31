// 128-bit signed arithmetic with explicit overflow detection.
//
// The spec (§8) calls out that `__int128` is a GNU/Clang extension and must not
// be assumed. We therefore expose one type, `I128`, backed by `__int128` where
// the compiler advertises it and by a portable two-limb implementation
// otherwise. Both paths are exercised by the unit tests via RISK_FORCE_PORTABLE_I128.
#pragma once

#include <cstdint>
#include <limits>
#include <string>

#if defined(__SIZEOF_INT128__) && !defined(RISK_FORCE_PORTABLE_I128)
#define RISK_HAS_BUILTIN_I128 1
#else
#define RISK_HAS_BUILTIN_I128 0
#endif

namespace risk {

#if RISK_HAS_BUILTIN_I128

// Thin wrapper so both backends present the same surface.
// `__extension__` is required because -Wpedantic rightly flags __int128 as a
// non-ISO type; the portable backend below exists for compilers that lack it.
__extension__ typedef __int128 RiskInt128;
__extension__ typedef unsigned __int128 RiskUInt128;

struct I128 {
  using Repr = RiskInt128;
  Repr v = 0;

  constexpr I128() = default;
  constexpr I128(std::int64_t x) : v(static_cast<Repr>(x)) {}
  constexpr explicit I128(Repr x) : v(x) {}

  constexpr bool negative() const { return v < 0; }
  constexpr bool is_zero() const { return v == 0; }

  friend constexpr bool operator==(I128 a, I128 b) { return a.v == b.v; }
  friend constexpr bool operator!=(I128 a, I128 b) { return a.v != b.v; }
  friend constexpr bool operator<(I128 a, I128 b) { return a.v < b.v; }
  friend constexpr bool operator<=(I128 a, I128 b) { return a.v <= b.v; }
  friend constexpr bool operator>(I128 a, I128 b) { return a.v > b.v; }
  friend constexpr bool operator>=(I128 a, I128 b) { return a.v >= b.v; }
};

#else

// Portable fallback: sign-magnitude over a 128-bit unsigned pair.
struct I128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;  // two's complement high limb

  constexpr I128() = default;
  constexpr I128(std::int64_t x)
      : lo(static_cast<std::uint64_t>(x)),
        hi(x < 0 ? ~0ULL : 0ULL) {}
  constexpr I128(std::uint64_t h, std::uint64_t l) : lo(l), hi(h) {}

  constexpr bool negative() const { return (hi >> 63) != 0; }
  constexpr bool is_zero() const { return lo == 0 && hi == 0; }

  friend constexpr bool operator==(I128 a, I128 b) {
    return a.lo == b.lo && a.hi == b.hi;
  }
  friend constexpr bool operator!=(I128 a, I128 b) { return !(a == b); }
  friend constexpr bool operator<(I128 a, I128 b) {
    if (a.negative() != b.negative()) return a.negative();
    if (a.hi != b.hi) return a.hi < b.hi;
    return a.lo < b.lo;
  }
  friend constexpr bool operator<=(I128 a, I128 b) { return a < b || a == b; }
  friend constexpr bool operator>(I128 a, I128 b) { return b < a; }
  friend constexpr bool operator>=(I128 a, I128 b) { return b <= a; }
};

#endif

// All of the following return false on overflow / undefined input and leave
// `out` untouched. Callers must treat false as RI-13 (checked arithmetic:
// overflow becomes REJECT/HOLD, never a wrapped small value).
bool i128_add(I128 a, I128 b, I128& out) noexcept;
bool i128_sub(I128 a, I128 b, I128& out) noexcept;
bool i128_mul(I128 a, I128 b, I128& out) noexcept;

// Truncating division (rounds toward zero), like C++ integer division.
bool i128_div_trunc(I128 a, I128 b, I128& out) noexcept;

// Ceiling division for a nonnegative numerator and positive denominator.
// This is the only rounding direction permitted for a risk charge (RI-10).
bool i128_div_ceil_nonneg(I128 a, I128 b, I128& out) noexcept;

// Floor division, used for conservative *lower* bounds (e.g. available
// headroom) where rounding down is the safe direction.
bool i128_div_floor(I128 a, I128 b, I128& out) noexcept;

// Narrowing. Fails rather than truncating.
bool i128_to_i64(I128 a, std::int64_t& out) noexcept;

I128 i128_from_i64(std::int64_t x) noexcept;
I128 i128_neg_unchecked(I128 a) noexcept;
I128 i128_abs_or_max(I128 a) noexcept;

// Decimal rendering, used in audit payloads and test diagnostics.
std::string i128_to_string(I128 a);

}  // namespace risk
