// G05 — integer precision, rounding, overflow.
//
// These cases are the oracle for RI-10 (monotone bounding) and RI-13 (checked
// arithmetic). Every one of them exists because the alternative behaviour --
// wrapping, truncating, or rounding a charge down -- would understate risk.
#include <limits>

#include "risk/checked.hpp"
#include "risk/rational.hpp"
#include "risk/wide.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

}  // namespace

TEST(unit_arithmetic, add_detects_positive_overflow) {
  std::int64_t out = 0;
  CHECK(add_i64(kI64Max - 1, 1, out));
  CHECK_EQ(out, kI64Max);
  CHECK(!add_i64(kI64Max, 1, out));
}

TEST(unit_arithmetic, add_detects_negative_overflow) {
  std::int64_t out = 0;
  CHECK(!add_i64(kI64Min, -1, out));
  CHECK(add_i64(kI64Min + 1, -1, out));
  CHECK_EQ(out, kI64Min);
}

TEST(unit_arithmetic, sub_detects_overflow) {
  std::int64_t out = 0;
  CHECK(!sub_i64(kI64Min, 1, out));
  CHECK(!sub_i64(kI64Max, -1, out));
  CHECK(sub_i64(0, kI64Max, out));
  CHECK_EQ(out, -kI64Max);
}

TEST(unit_arithmetic, mul_detects_overflow) {
  std::int64_t out = 0;
  CHECK(mul_i64(3'037'000'499LL, 3'037'000'499LL, out));
  CHECK(!mul_i64(3'037'000'500LL, 3'037'000'500LL, out));
  // The classic wrap: INT64_MIN * -1 has no representable result.
  CHECK(!mul_i64(kI64Min, -1, out));
}

TEST(unit_arithmetic, abs_refuses_int64_min) {
  std::int64_t out = 0;
  // Returning INT64_MIN unchanged would make a huge short position look like a
  // huge long one and pass every magnitude check.
  CHECK(!abs_i64(kI64Min, out));
  CHECK(abs_i64(kI64Min + 1, out));
  CHECK_EQ(out, kI64Max);
}

TEST(unit_arithmetic, ceil_div_rounds_up_and_rejects_bad_input) {
  std::int64_t out = 0;
  CHECK(ceil_div_nonneg_i64(10, 3, out));
  CHECK_EQ(out, 4);
  CHECK(ceil_div_nonneg_i64(9, 3, out));
  CHECK_EQ(out, 3);
  CHECK(ceil_div_nonneg_i64(0, 3, out));
  CHECK_EQ(out, 0);
  CHECK(ceil_div_nonneg_i64(1, 1'000'000, out));
  CHECK_EQ(out, 1);  // a sub-unit charge still costs one minor unit

  CHECK(!ceil_div_nonneg_i64(-1, 3, out));
  CHECK(!ceil_div_nonneg_i64(10, 0, out));
  CHECK(!ceil_div_nonneg_i64(10, -3, out));
}

TEST(unit_arithmetic, ceil_div_near_the_int64_ceiling) {
  // num + den - 1 overflows int64 here; the 128-bit path must still be exact.
  std::int64_t out = 0;
  CHECK(ceil_div_nonneg_i64(kI64Max, 2, out));
  // ceil(9223372036854775807 / 2); written as a literal because computing
  // (kI64Max + 1) / 2 in the test would itself overflow.
  CHECK_EQ(out, 4'611'686'018'427'387'904LL);
  CHECK(ceil_div_nonneg_i64(kI64Max, kI64Max, out));
  CHECK_EQ(out, 1);
}

TEST(unit_wide, mul_and_narrow_round_trip) {
  I128 wide;
  CHECK(mul_to_i128(1'000'000'000LL, 1'000'000'000LL, wide));
  std::int64_t narrow = 0;
  CHECK(i128_to_i64(wide, narrow));
  CHECK_EQ(narrow, 1'000'000'000'000'000'000LL);
}

TEST(unit_wide, narrowing_refuses_out_of_range) {
  I128 a = i128_from_i64(kI64Max);
  I128 sum;
  CHECK(i128_add(a, i128_from_i64(1), sum));
  std::int64_t narrow = 0;
  CHECK(!i128_to_i64(sum, narrow));
}

TEST(unit_wide, mul_detects_128_bit_overflow) {
  // 2^64 * 2^64 does not fit in a signed 128-bit value.
  I128 big;
  CHECK(mul_to_i128(4'294'967'296LL, 4'294'967'296LL, big));  // 2^64
  I128 product;
  CHECK(!i128_mul(big, big, product));
}

TEST(unit_wide, ceil_division_is_exact) {
  I128 q;
  CHECK(i128_div_ceil_nonneg(i128_from_i64(7), i128_from_i64(2), q));
  CHECK_EQ(i128_to_string(q), std::string("4"));
  CHECK(i128_div_ceil_nonneg(i128_from_i64(8), i128_from_i64(2), q));
  CHECK_EQ(i128_to_string(q), std::string("4"));
  // A negative numerator is refused rather than rounded toward zero.
  CHECK(!i128_div_ceil_nonneg(i128_from_i64(-7), i128_from_i64(2), q));
  CHECK(!i128_div_ceil_nonneg(i128_from_i64(7), i128_from_i64(0), q));
}

TEST(unit_wide, floor_division_rounds_toward_negative_infinity) {
  I128 q;
  CHECK(i128_div_floor(i128_from_i64(-7), i128_from_i64(2), q));
  CHECK_EQ(i128_to_string(q), std::string("-4"));
  CHECK(i128_div_floor(i128_from_i64(7), i128_from_i64(2), q));
  CHECK_EQ(i128_to_string(q), std::string("3"));
}

TEST(unit_wide, to_string_handles_sign_and_zero) {
  CHECK_EQ(i128_to_string(i128_from_i64(0)), std::string("0"));
  CHECK_EQ(i128_to_string(i128_from_i64(-1)), std::string("-1"));
  CHECK_EQ(i128_to_string(i128_from_i64(kI64Min)),
           std::string("-9223372036854775808"));
}

TEST(unit_rational, parse_accepts_fractions_and_integers) {
  Rational r;
  CHECK(rational_parse("1/100000", r));
  CHECK_EQ(r.num, 1);
  CHECK_EQ(r.den, 100000);

  CHECK(rational_parse("3", r));
  CHECK_EQ(r.num, 3);
  CHECK_EQ(r.den, 1);

  // Reduction happens at parse time so two spellings hash alike.
  CHECK(rational_parse("50/100", r));
  CHECK_EQ(r.num, 1);
  CHECK_EQ(r.den, 2);

  CHECK(rational_parse("-1/4", r));
  CHECK_EQ(r.num, -1);
  CHECK_EQ(r.den, 4);
}

TEST(unit_rational, parse_rejects_malformed_input) {
  Rational r;
  CHECK(!rational_parse("", r));
  CHECK(!rational_parse("1/0", r));
  CHECK(!rational_parse("1/", r));
  CHECK(!rational_parse("/2", r));
  CHECK(!rational_parse("1.5", r));
  CHECK(!rational_parse("1 / 2", r));
  CHECK(!rational_parse("+1/2", r));
  CHECK(!rational_parse("1/2/3", r));
  CHECK(!rational_parse("abc", r));
  CHECK(!rational_parse("99999999999999999999", r));
}

TEST(unit_rational, multiplication_cross_reduces_before_multiplying) {
  Rational a{1'000'000'000, 7};
  Rational b{7, 1'000'000'000};
  Rational out;
  CHECK(rational_mul(a, b, out));
  CHECK_EQ(out.num, 1);
  CHECK_EQ(out.den, 1);
}

TEST(unit_rational, comparison_is_exact_without_overflow) {
  int cmp = 0;
  CHECK(rational_compare(Rational{1, 3}, Rational{2, 6}, cmp));
  CHECK_EQ(cmp, 0);
  CHECK(rational_compare(Rational{kI64Max, 2}, Rational{kI64Max, 3}, cmp));
  CHECK_EQ(cmp, 1);
  CHECK(rational_compare(Rational{-1, 3}, Rational{1, 1000}, cmp));
  CHECK_EQ(cmp, -1);
}

TEST(unit_rational, accumulator_multiplies_exactly_then_ceils) {
  // 50,000 units x 110,000 ticks x 1/100000 tick size x 1/1 multiplier
  // x 1/1 fx x 100 minor units = 5,500,000 minor units, exactly.
  RatioAcc acc(50000);
  CHECK(acc.mul_i64(110000));
  CHECK(acc.mul_rational(Rational{1, 100000}));
  CHECK(acc.mul_rational(Rational{1, 1}));
  CHECK(acc.mul_i64(100));
  std::int64_t out = 0;
  CHECK(acc.ceil_to_i64_nonneg(out));
  CHECK_EQ(out, 5'500'000);
}

TEST(unit_rational, accumulator_never_rounds_a_charge_down) {
  // 1 unit at a price whose exact minor value is 0.0001: the charge must be 1
  // minor unit, not 0.
  RatioAcc acc(1);
  CHECK(acc.mul_rational(Rational{1, 10000}));
  std::int64_t out = 0;
  CHECK(acc.ceil_to_i64_nonneg(out));
  CHECK_EQ(out, 1);
}

TEST(unit_rational, accumulator_refuses_negative_charges) {
  RatioAcc acc(-5);
  CHECK(acc.is_negative());
  std::int64_t out = 0;
  CHECK(!acc.ceil_to_i64_nonneg(out));
  // Floor narrowing is allowed for signed quantities.
  CHECK(acc.floor_to_i64(out));
  CHECK_EQ(out, -5);
}

TEST(unit_rational, accumulator_survives_a_long_multiplication_chain) {
  // Deliberately large: 10^12 units at 10^7 ticks with a 10^-5 tick size and a
  // 2-digit currency is 10^16 minor units, which fits only because the
  // accumulator stays in 128-bit space and reduces as it goes.
  RatioAcc acc(1'000'000'000'000LL);
  CHECK(acc.mul_i64(10'000'000LL));
  CHECK(acc.mul_rational(Rational{1, 100000}));
  CHECK(acc.mul_i64(100));
  std::int64_t out = 0;
  CHECK(acc.ceil_to_i64_nonneg(out));
  CHECK_EQ(out, 10'000'000'000'000'000LL);
}

TEST(unit_rational, accumulator_reports_overflow_instead_of_wrapping) {
  RatioAcc acc(kI64Max);
  CHECK(acc.mul_i64(kI64Max));
  std::int64_t out = 0;
  // The product is exact in 128-bit space but cannot narrow to int64.
  CHECK(!acc.ceil_to_i64_nonneg(out));
}

TEST(unit_rational, division_by_zero_is_refused) {
  RatioAcc acc(10);
  CHECK(!acc.div_i64(0));
  CHECK(!acc.mul_rational(Rational{1, 0}));
}

TEST(unit_rational, gcd_of_zero_is_one) {
  CHECK(i128_gcd(i128_from_i64(0), i128_from_i64(5)) == i128_from_i64(1));
  CHECK(i128_gcd(i128_from_i64(12), i128_from_i64(18)) == i128_from_i64(6));
}
