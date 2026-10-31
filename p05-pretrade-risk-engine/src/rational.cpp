#include "risk/rational.hpp"

#include <cstdlib>
#include <limits>

#include "risk/checked.hpp"

namespace risk {
namespace {

bool i128_mod(I128 a, I128 b, I128& out) noexcept {
  I128 q, prod;
  if (!i128_div_trunc(a, b, q)) return false;
  if (!i128_mul(q, b, prod)) return false;
  return i128_sub(a, prod, out);
}

std::int64_t gcd_i64(std::int64_t a, std::int64_t b) noexcept {
  // Operates on magnitudes; INT64_MIN is handled by the callers, which reject
  // it before reaching here.
  std::uint64_t x = a < 0 ? static_cast<std::uint64_t>(-(a + 1)) + 1
                          : static_cast<std::uint64_t>(a);
  std::uint64_t y = b < 0 ? static_cast<std::uint64_t>(-(b + 1)) + 1
                          : static_cast<std::uint64_t>(b);
  while (y != 0) {
    const std::uint64_t t = x % y;
    x = y;
    y = t;
  }
  if (x == 0) return 1;
  if (x > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return 1;
  return static_cast<std::int64_t>(x);
}

}  // namespace

I128 i128_gcd(I128 a, I128 b) noexcept {
  I128 x = i128_abs_or_max(a);
  I128 y = i128_abs_or_max(b);
  if (x.is_zero() || y.is_zero()) return i128_from_i64(1);
  while (!y.is_zero()) {
    I128 t;
    if (!i128_mod(x, y, t)) return i128_from_i64(1);
    x = y;
    y = t;
  }
  if (x.is_zero()) return i128_from_i64(1);
  return x;
}

bool rational_reduce(Rational& r) noexcept {
  if (r.den == 0) return false;
  if (r.num == std::numeric_limits<std::int64_t>::min() ||
      r.den == std::numeric_limits<std::int64_t>::min())
    return false;
  if (r.den < 0) {
    r.num = -r.num;
    r.den = -r.den;
  }
  if (r.num == 0) {
    r.den = 1;
    return true;
  }
  const std::int64_t g = gcd_i64(r.num, r.den);
  if (g > 1) {
    r.num /= g;
    r.den /= g;
  }
  return true;
}

bool rational_parse(std::string_view text, Rational& out) noexcept {
  if (text.empty()) return false;

  auto parse_u = [](std::string_view s, std::int64_t& v) -> bool {
    if (s.empty() || s.size() > 19) return false;
    std::int64_t acc = 0;
    for (const char c : s) {
      if (c < '0' || c > '9') return false;
      if (!mul_i64(acc, 10, acc)) return false;
      if (!add_i64(acc, static_cast<std::int64_t>(c - '0'), acc)) return false;
    }
    v = acc;
    return true;
  };

  bool negative = false;
  std::string_view body = text;
  if (body.front() == '-') {
    negative = true;
    body.remove_prefix(1);
  }

  const std::size_t slash = body.find('/');
  Rational r;
  if (slash == std::string_view::npos) {
    if (!parse_u(body, r.num)) return false;
    r.den = 1;
  } else {
    if (!parse_u(body.substr(0, slash), r.num)) return false;
    if (!parse_u(body.substr(slash + 1), r.den)) return false;
    if (r.den == 0) return false;
  }
  if (negative) r.num = -r.num;
  if (!rational_reduce(r)) return false;
  out = r;
  return true;
}

std::string rational_to_string(const Rational& r) {
  return std::to_string(r.num) + "/" + std::to_string(r.den);
}

bool rational_compare(const Rational& a, const Rational& b, int& out) noexcept {
  if (a.den <= 0 || b.den <= 0) return false;
  I128 lhs, rhs;
  if (!i128_mul(i128_from_i64(a.num), i128_from_i64(b.den), lhs)) return false;
  if (!i128_mul(i128_from_i64(b.num), i128_from_i64(a.den), rhs)) return false;
  out = (lhs < rhs) ? -1 : (lhs == rhs ? 0 : 1);
  return true;
}

bool rational_mul(const Rational& a, const Rational& b, Rational& out) noexcept {
  if (a.den <= 0 || b.den <= 0) return false;
  // Cross-reduce before multiplying so ordinary metadata never overflows.
  Rational x = a, y = b;
  if (!rational_reduce(x) || !rational_reduce(y)) return false;
  const std::int64_t g1 = gcd_i64(x.num, y.den);
  const std::int64_t g2 = gcd_i64(y.num, x.den);
  if (g1 > 1) {
    x.num /= g1;
    y.den /= g1;
  }
  if (g2 > 1) {
    y.num /= g2;
    x.den /= g2;
  }
  Rational r;
  if (!mul_i64(x.num, y.num, r.num)) return false;
  if (!mul_i64(x.den, y.den, r.den)) return false;
  if (!rational_reduce(r)) return false;
  out = r;
  return true;
}

// ---------------------------------------------------------------------------
// RatioAcc
// ---------------------------------------------------------------------------

void RatioAcc::reduce() noexcept {
  if (num_.is_zero()) {
    den_ = i128_from_i64(1);
    return;
  }
  const I128 g = i128_gcd(num_, den_);
  if (g == i128_from_i64(1)) return;
  I128 n, d;
  if (!i128_div_trunc(num_, g, n)) return;
  if (!i128_div_trunc(den_, g, d)) return;
  num_ = n;
  den_ = d;
}

bool RatioAcc::mul_i64(std::int64_t v) noexcept {
  I128 n;
  if (!i128_mul(num_, i128_from_i64(v), n)) {
    // Reduce and retry once: a pending common factor is the usual cause.
    reduce();
    if (!i128_mul(num_, i128_from_i64(v), n)) return false;
  }
  num_ = n;
  reduce();
  return true;
}

bool RatioAcc::mul_rational(const Rational& r) noexcept {
  if (r.den <= 0) return false;
  if (!mul_i64(r.num)) return false;
  return div_i64(r.den);
}

bool RatioAcc::div_i64(std::int64_t v) noexcept {
  if (v == 0) return false;
  I128 d;
  if (!i128_mul(den_, i128_from_i64(v), d)) {
    reduce();
    if (!i128_mul(den_, i128_from_i64(v), d)) return false;
  }
  den_ = d;
  if (den_.negative()) {
    num_ = i128_neg_unchecked(num_);
    den_ = i128_neg_unchecked(den_);
  }
  reduce();
  return true;
}

bool RatioAcc::is_negative() const noexcept {
  if (num_.is_zero()) return false;
  return num_.negative() != den_.negative();
}

bool RatioAcc::ceil_to_i64_nonneg(std::int64_t& out) const noexcept {
  if (den_.is_zero()) return false;
  I128 n = num_, d = den_;
  if (d.negative()) {
    n = i128_neg_unchecked(n);
    d = i128_neg_unchecked(d);
    if (d.negative()) return false;
  }
  if (n.negative()) return false;  // a risk charge is never negative
  I128 q;
  if (!i128_div_ceil_nonneg(n, d, q)) return false;
  return i128_to_i64(q, out);
}

bool RatioAcc::floor_to_i64(std::int64_t& out) const noexcept {
  if (den_.is_zero()) return false;
  I128 n = num_, d = den_;
  if (d.negative()) {
    n = i128_neg_unchecked(n);
    d = i128_neg_unchecked(d);
    if (d.negative()) return false;
  }
  I128 q;
  if (!i128_div_floor(n, d, q)) return false;
  return i128_to_i64(q, out);
}

std::string RatioAcc::to_string() const {
  return i128_to_string(num_) + "/" + i128_to_string(den_);
}

}  // namespace risk
