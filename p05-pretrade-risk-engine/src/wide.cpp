#include "risk/wide.hpp"

#include <algorithm>
#include <cstddef>

namespace risk {
namespace {

#if !RISK_HAS_BUILTIN_I128

// ---------------------------------------------------------------------------
// Portable backend helpers: unsigned 128-bit primitives over two 64-bit limbs.
// ---------------------------------------------------------------------------
struct U128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
};

constexpr U128 u128_of(const I128& v) { return U128{v.lo, v.hi}; }
constexpr I128 i128_of(const U128& v) { return I128{v.hi, v.lo}; }

// Returns the carry out of the whole 128-bit add.
U128 u128_add(U128 a, U128 b, bool& carry_out) {
  U128 r;
  r.lo = a.lo + b.lo;
  const std::uint64_t carry = (r.lo < a.lo) ? 1ULL : 0ULL;
  const std::uint64_t hi_sum = a.hi + b.hi;
  const bool c1 = hi_sum < a.hi;
  r.hi = hi_sum + carry;
  const bool c2 = r.hi < hi_sum;
  carry_out = c1 || c2;
  return r;
}

bool u128_lt(U128 a, U128 b) {
  if (a.hi != b.hi) return a.hi < b.hi;
  return a.lo < b.lo;
}

bool u128_is_zero(U128 a) { return a.lo == 0 && a.hi == 0; }

U128 u128_shl1(U128 a) { return U128{a.lo << 1, (a.hi << 1) | (a.lo >> 63)}; }

U128 u128_sub(U128 a, U128 b) {
  U128 r;
  r.lo = a.lo - b.lo;
  const std::uint64_t borrow = (a.lo < b.lo) ? 1ULL : 0ULL;
  r.hi = a.hi - b.hi - borrow;
  return r;
}

bool u128_bit(U128 a, int i) {
  return i < 64 ? ((a.lo >> i) & 1ULL) != 0 : ((a.hi >> (i - 64)) & 1ULL) != 0;
}

void u128_set_bit(U128& a, int i) {
  if (i < 64) {
    a.lo |= (1ULL << i);
  } else {
    a.hi |= (1ULL << (i - 64));
  }
}

// 64x64 -> 128 via 32-bit limbs; no compiler extension required.
U128 mul64(std::uint64_t a, std::uint64_t b) {
  const std::uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
  const std::uint64_t b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
  const std::uint64_t p00 = a0 * b0;
  const std::uint64_t p01 = a0 * b1;
  const std::uint64_t p10 = a1 * b0;
  const std::uint64_t p11 = a1 * b1;

  const std::uint64_t mid =
      (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
  U128 r;
  r.lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
  r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return r;
}

// Unsigned 128x128 multiply with overflow detection.
bool u128_mul(U128 a, U128 b, U128& out) {
  if (a.hi != 0 && b.hi != 0) return false;  // product is >= 2^128
  U128 acc = mul64(a.lo, b.lo);
  if (a.hi != 0) {
    const U128 t = mul64(a.hi, b.lo);
    if (t.hi != 0) return false;
    bool carry = false;
    acc = u128_add(acc, U128{0, t.lo}, carry);
    if (carry) return false;
  }
  if (b.hi != 0) {
    const U128 t = mul64(b.hi, a.lo);
    if (t.hi != 0) return false;
    bool carry = false;
    acc = u128_add(acc, U128{0, t.lo}, carry);
    if (carry) return false;
  }
  out = acc;
  return true;
}

// Restoring binary long division; quotient and remainder of a / b.
void u128_divmod(U128 a, U128 b, U128& q, U128& r) {
  q = U128{};
  r = U128{};
  for (int i = 127; i >= 0; --i) {
    r = u128_shl1(r);
    if (u128_bit(a, i)) r.lo |= 1ULL;
    if (!u128_lt(r, b)) {
      r = u128_sub(r, b);
      u128_set_bit(q, i);
    }
  }
}

const U128 kAbsMin{0ULL, 0x8000000000000000ULL};   // 2^127
const U128 kMaxPos{~0ULL, 0x7FFFFFFFFFFFFFFFULL};  // 2^127 - 1

// Magnitude of a two's complement value. 2^127 is representable as U128.
U128 magnitude(I128 v) {
  const U128 u = u128_of(v);
  if (!v.negative()) return u;
  U128 r{~u.lo, ~u.hi};
  bool carry = false;
  r = u128_add(r, U128{1, 0}, carry);
  return r;
}

bool from_magnitude(U128 m, bool negative, I128& out) {
  if (!negative) {
    if (u128_lt(kMaxPos, m)) return false;
    out = i128_of(m);
    return true;
  }
  if (u128_lt(kAbsMin, m)) return false;
  U128 r{~m.lo, ~m.hi};
  bool carry = false;
  r = u128_add(r, U128{1, 0}, carry);
  out = i128_of(r);
  return true;
}

#endif  // !RISK_HAS_BUILTIN_I128

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

I128 i128_from_i64(std::int64_t x) noexcept { return I128{x}; }

#if RISK_HAS_BUILTIN_I128

bool i128_add(I128 a, I128 b, I128& out) noexcept {
  I128::Repr r = 0;
  if (__builtin_add_overflow(a.v, b.v, &r)) return false;
  out = I128{r};
  return true;
}

bool i128_sub(I128 a, I128 b, I128& out) noexcept {
  I128::Repr r = 0;
  if (__builtin_sub_overflow(a.v, b.v, &r)) return false;
  out = I128{r};
  return true;
}

bool i128_mul(I128 a, I128 b, I128& out) noexcept {
  I128::Repr r = 0;
  if (__builtin_mul_overflow(a.v, b.v, &r)) return false;
  out = I128{r};
  return true;
}

bool i128_div_trunc(I128 a, I128 b, I128& out) noexcept {
  if (b.v == 0) return false;
  // The only signed-division overflow is MIN / -1.
  if (b.v == -1) {
    I128::Repr neg = 0;
    if (__builtin_sub_overflow(static_cast<I128::Repr>(0), a.v, &neg)) return false;
    out = I128{neg};
    return true;
  }
  out = I128{static_cast<I128::Repr>(a.v / b.v)};
  return true;
}

bool i128_div_floor(I128 a, I128 b, I128& out) noexcept {
  I128 q;
  if (!i128_div_trunc(a, b, q)) return false;
  I128::Repr prod = 0;
  if (__builtin_mul_overflow(q.v, b.v, &prod)) return false;
  const I128::Repr rem = a.v - prod;
  if (rem != 0 && ((rem < 0) != (b.v < 0))) {
    I128::Repr adj = 0;
    if (__builtin_sub_overflow(q.v, static_cast<I128::Repr>(1), &adj)) return false;
    out = I128{adj};
    return true;
  }
  out = q;
  return true;
}

bool i128_div_ceil_nonneg(I128 a, I128 b, I128& out) noexcept {
  if (a.v < 0 || b.v <= 0) return false;
  I128::Repr num = 0;
  if (__builtin_add_overflow(a.v, static_cast<I128::Repr>(b.v - 1), &num)) return false;
  out = I128{static_cast<I128::Repr>(num / b.v)};
  return true;
}

bool i128_to_i64(I128 a, std::int64_t& out) noexcept {
  if (a.v > static_cast<I128::Repr>(std::numeric_limits<std::int64_t>::max()))
    return false;
  if (a.v < static_cast<I128::Repr>(std::numeric_limits<std::int64_t>::min()))
    return false;
  out = static_cast<std::int64_t>(a.v);
  return true;
}

I128 i128_neg_unchecked(I128 a) noexcept {
  return I128{static_cast<I128::Repr>(-a.v)};
}

I128 i128_abs_or_max(I128 a) noexcept {
  if (a.v >= 0) return a;
  I128::Repr r = 0;
  if (__builtin_sub_overflow(static_cast<I128::Repr>(0), a.v, &r)) {
    // abs(MIN) is unrepresentable; saturate so callers still see a huge value
    // that fails every downstream range check instead of wrapping negative.
    return I128{static_cast<I128::Repr>(
        (static_cast<RiskUInt128>(1) << 127) - 1)};
  }
  return I128{r};
}

std::string i128_to_string(I128 a) {
  if (a.v == 0) return "0";
  const bool neg = a.v < 0;
  RiskUInt128 m = neg ? (~static_cast<RiskUInt128>(a.v) + 1)
                      : static_cast<RiskUInt128>(a.v);
  std::string digits;
  while (m != 0) {
    digits.push_back(static_cast<char>('0' + static_cast<int>(m % 10)));
    m /= 10;
  }
  if (neg) digits.push_back('-');
  std::reverse(digits.begin(), digits.end());
  return digits;
}

#else  // portable backend

bool i128_add(I128 a, I128 b, I128& out) noexcept {
  bool carry = false;
  const U128 r = u128_add(u128_of(a), u128_of(b), carry);
  const I128 res = i128_of(r);
  // Signed overflow iff the operands share a sign the result does not.
  if (a.negative() == b.negative() && res.negative() != a.negative()) return false;
  out = res;
  return true;
}

bool i128_sub(I128 a, I128 b, I128& out) noexcept {
  if (b.is_zero()) {
    out = a;
    return true;
  }
  // Negating the minimum is unrepresentable, so route through magnitudes.
  I128 nb;
  if (!from_magnitude(magnitude(b), !b.negative(), nb)) return false;
  return i128_add(a, nb, out);
}

bool i128_mul(I128 a, I128 b, I128& out) noexcept {
  if (a.is_zero() || b.is_zero()) {
    out = I128{0};
    return true;
  }
  U128 m;
  if (!u128_mul(magnitude(a), magnitude(b), m)) return false;
  return from_magnitude(m, a.negative() != b.negative(), out);
}

bool i128_div_trunc(I128 a, I128 b, I128& out) noexcept {
  if (b.is_zero()) return false;
  U128 q, r;
  u128_divmod(magnitude(a), magnitude(b), q, r);
  return from_magnitude(q, a.negative() != b.negative(), out);
}

bool i128_div_floor(I128 a, I128 b, I128& out) noexcept {
  if (b.is_zero()) return false;
  U128 q, r;
  u128_divmod(magnitude(a), magnitude(b), q, r);
  const bool neg = a.negative() != b.negative();
  if (neg && !u128_is_zero(r)) {
    bool carry = false;
    q = u128_add(q, U128{1, 0}, carry);
    if (carry) return false;
  }
  return from_magnitude(q, neg, out);
}

bool i128_div_ceil_nonneg(I128 a, I128 b, I128& out) noexcept {
  if (a.negative() || b.negative() || b.is_zero()) return false;
  U128 q, r;
  u128_divmod(u128_of(a), u128_of(b), q, r);
  if (!u128_is_zero(r)) {
    bool carry = false;
    q = u128_add(q, U128{1, 0}, carry);
    if (carry) return false;
  }
  return from_magnitude(q, false, out);
}

bool i128_to_i64(I128 a, std::int64_t& out) noexcept {
  const U128 m = magnitude(a);
  if (m.hi != 0) return false;
  if (!a.negative()) {
    if (m.lo > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return false;
    out = static_cast<std::int64_t>(m.lo);
    return true;
  }
  if (m.lo > 0x8000000000000000ULL) return false;
  if (m.lo == 0x8000000000000000ULL) {
    out = std::numeric_limits<std::int64_t>::min();
    return true;
  }
  out = -static_cast<std::int64_t>(m.lo);
  return true;
}

I128 i128_neg_unchecked(I128 a) noexcept {
  I128 r;
  if (!from_magnitude(magnitude(a), !a.negative(), r)) return a;
  return r;
}

I128 i128_abs_or_max(I128 a) noexcept {
  if (!a.negative()) return a;
  I128 r;
  if (!from_magnitude(magnitude(a), false, r)) return i128_of(kMaxPos);
  return r;
}

std::string i128_to_string(I128 a) {
  if (a.is_zero()) return "0";
  U128 m = magnitude(a);
  const U128 ten{10, 0};
  std::string digits;
  while (!u128_is_zero(m)) {
    U128 q, r;
    u128_divmod(m, ten, q, r);
    digits.push_back(static_cast<char>('0' + static_cast<int>(r.lo)));
    m = q;
  }
  if (a.negative()) digits.push_back('-');
  std::reverse(digits.begin(), digits.end());
  return digits;
}

#endif

}  // namespace risk
