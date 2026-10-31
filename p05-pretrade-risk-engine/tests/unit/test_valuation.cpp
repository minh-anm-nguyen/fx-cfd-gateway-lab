// G02 / G05 — instrument rules, the conservative risk price, notional and the
// margin proxy.
//
// The property that matters most here: every rounding step must move the
// charge *up*, never down (RI-10). A test that only checked "the number looks
// about right" would miss a floor division that quietly discounts risk.
#include <limits>

#include "risk/market.hpp"
#include "risk/policy.hpp"
#include "risk/valuation.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

InstrumentSpec eurusd() {
  RiskPolicy p = lab_policy();
  return p.instruments.at("EURUSD");
}

Quote quote_at(std::int64_t bid, std::int64_t ask) {
  Quote q;
  q.source_id = "LAB-FEED";
  q.symbol = "EURUSD";
  q.bid_ticks = bid;
  q.ask_ticks = ask;
  q.sequence = 1;
  q.version = 1;
  q.ttl_ms = 500;
  return q;
}

Valuation valuation_at(std::int64_t risk_price_ticks, Rational fx = Rational{1, 1}) {
  Valuation v;
  v.risk_price_ticks = risk_price_ticks;
  v.fx_upper = fx;
  v.quote_version = 1;
  v.fx_version = 1;
  return v;
}

}  // namespace

TEST(unit_valuation, pow10_bounds) {
  std::int64_t out = 0;
  CHECK(pow10_i64(0, out));
  CHECK_EQ(out, 1);
  CHECK(pow10_i64(2, out));
  CHECK_EQ(out, 100);
  CHECK(pow10_i64(18, out));
  CHECK_EQ(out, 1'000'000'000'000'000'000LL);
  CHECK(!pow10_i64(19, out));
  CHECK(!pow10_i64(-1, out));
}

TEST(unit_valuation, risk_price_takes_the_worse_of_limit_and_ask) {
  const InstrumentSpec spec = eurusd();  // 100 bps buffer

  // Limit above the ask: the limit governs. 110000 * 1.01 = 111100.
  Result<std::int64_t> above =
      risk_price_upper_ticks(110000, quote_at(105000, 105010), spec);
  CHECK_MSG(above.ok(), above.detail());
  CHECK_EQ(above.value(), 111100);

  // Ask above the limit: the ask governs, because a SELL fills at or above its
  // limit and the resulting position is still marked at market.
  Result<std::int64_t> below =
      risk_price_upper_ticks(100000, quote_at(119990, 120000), spec);
  CHECK_MSG(below.ok(), below.detail());
  CHECK_EQ(below.value(), 121200);  // 120000 * 1.01
}

TEST(unit_valuation, risk_price_buffer_rounds_up) {
  InstrumentSpec spec = eurusd();
  spec.risk_price_upper_buffer_bps = 1;  // 0.01%
  // 10000 * 10001/10000 = 10001 exactly.
  Result<std::int64_t> exact = risk_price_upper_ticks(10000, quote_at(1, 2), spec);
  CHECK(exact.ok());
  CHECK_EQ(exact.value(), 10001);

  // 3 * 10001/10000 = 3.0003 -> must become 4, not 3.
  Result<std::int64_t> rounded = risk_price_upper_ticks(3, quote_at(1, 2), spec);
  CHECK(rounded.ok());
  CHECK_EQ(rounded.value(), 4);
}

TEST(unit_valuation, risk_price_with_no_buffer_is_the_base) {
  InstrumentSpec spec = eurusd();
  spec.risk_price_upper_buffer_bps = 0;
  Result<std::int64_t> r = risk_price_upper_ticks(110000, quote_at(1, 2), spec);
  CHECK(r.ok());
  CHECK_EQ(r.value(), 110000);
}

TEST(unit_valuation, risk_price_rejects_bad_inputs) {
  const InstrumentSpec spec = eurusd();
  CHECK(!risk_price_upper_ticks(0, quote_at(1, 2), spec).ok());
  CHECK(!risk_price_upper_ticks(-1, quote_at(1, 2), spec).ok());
  CHECK(!risk_price_upper_ticks(100, quote_at(1, 0), spec).ok());
}

TEST(unit_valuation, risk_price_reports_overflow) {
  InstrumentSpec spec = eurusd();
  spec.risk_price_upper_buffer_bps = 10000;  // doubles the price
  Result<std::int64_t> r = risk_price_upper_ticks(
      std::numeric_limits<std::int64_t>::max(), quote_at(1, 2), spec);
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()),
           static_cast<int>(Reason::ArithmeticOverflow));
}

TEST(unit_valuation, price_collar_measures_deviation_from_the_midpoint) {
  // Midpoint of 109990/110010 is 110000. A 500 bps collar allows +/- 5%.
  const Quote q = quote_at(109990, 110010);

  CHECK(check_price_collar(110000, q, 500).ok());
  CHECK(check_price_collar(115500, q, 500).ok());   // exactly +5%
  CHECK(check_price_collar(104500, q, 500).ok());   // exactly -5%

  Status s = check_price_collar(115501, q, 500);
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::LimitPriceCollar));

  s = check_price_collar(104499, q, 500);
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::LimitPriceCollar));
}

TEST(unit_valuation, a_zero_collar_disables_the_check) {
  const Quote q = quote_at(109990, 110010);
  CHECK(check_price_collar(1, q, 0).ok());
  CHECK(check_price_collar(999'999'999, q, 0).ok());
}

TEST(unit_valuation, collar_rejects_non_positive_prices) {
  const Quote q = quote_at(109990, 110010);
  const Status s = check_price_collar(0, q, 500);
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::PriceNonPositive));
}

TEST(unit_valuation, notional_matches_the_hand_computed_value) {
  const InstrumentSpec spec = eurusd();  // tick 1/100000, multiplier 1/1
  // 100,000 units at 1.11100 USD/EUR with a 2-digit currency:
  // 100000 * 1.111 = 111,100 USD = 11,110,000 cents.
  Result<std::int64_t> n = notional_minor(100000, spec, valuation_at(111100), 2);
  CHECK_MSG(n.ok(), n.detail());
  CHECK_EQ(n.value(), 11'110'000);
}

TEST(unit_valuation, notional_applies_the_fx_upper_bound) {
  InstrumentSpec spec = eurusd();
  spec.quote_currency = "JPY";
  spec.tick_size = Rational{1, 1000};
  // 1,000 units at 160.000 JPY, converted at 0.0067 USD/JPY:
  // 1000 * 160 = 160,000 JPY * 0.0067 = 1,072 USD = 107,200 cents.
  Result<std::int64_t> n = notional_minor(
      1000, spec, valuation_at(160000, Rational{67, 10000}), 2);
  CHECK_MSG(n.ok(), n.detail());
  CHECK_EQ(n.value(), 107'200);
}

TEST(unit_valuation, notional_of_zero_units_is_zero) {
  Result<std::int64_t> n = notional_minor(0, eurusd(), valuation_at(111100), 2);
  CHECK(n.ok());
  CHECK_EQ(n.value(), 0);
}

TEST(unit_valuation, notional_refuses_negative_units) {
  // A negative exposure magnitude would turn a charge into a credit.
  Result<std::int64_t> n = notional_minor(-1000, eurusd(), valuation_at(111100), 2);
  CHECK(!n.ok());
  CHECK_EQ(static_cast<int>(n.reason()),
           static_cast<int>(Reason::NegativeRiskCharge));
}

TEST(unit_valuation, notional_never_rounds_down) {
  // One unit at a price of 1/100000 with a 2-digit currency has an exact value
  // of 0.001 cents. The charge must be 1 cent, not 0.
  InstrumentSpec spec = eurusd();
  Result<std::int64_t> n = notional_minor(1, spec, valuation_at(1), 2);
  CHECK_MSG(n.ok(), n.detail());
  CHECK_EQ(n.value(), 1);
}

TEST(unit_valuation, notional_is_monotone_in_units) {
  const InstrumentSpec spec = eurusd();
  const Valuation v = valuation_at(111100);
  std::int64_t previous = -1;
  for (std::int64_t units = 0; units <= 200000; units += 1000) {
    Result<std::int64_t> n = notional_minor(units, spec, v, 2);
    CHECK_MSG(n.ok(), n.detail());
    CHECK_MSG(n.value() >= previous, "units=" + std::to_string(units));
    previous = n.value();
  }
}

TEST(unit_valuation, notional_is_monotone_in_price_and_fx) {
  const InstrumentSpec spec = eurusd();
  Result<std::int64_t> low = notional_minor(100000, spec, valuation_at(110000), 2);
  Result<std::int64_t> high = notional_minor(100000, spec, valuation_at(111100), 2);
  CHECK(low.ok() && high.ok());
  CHECK(high.value() > low.value());

  Result<std::int64_t> fx_low = notional_minor(
      100000, spec, valuation_at(110000, Rational{1, 1}), 2);
  Result<std::int64_t> fx_high = notional_minor(
      100000, spec, valuation_at(110000, Rational{11, 10}), 2);
  CHECK(fx_low.ok() && fx_high.ok());
  CHECK(fx_high.value() > fx_low.value());
}

TEST(unit_valuation, notional_rejects_non_positive_factors) {
  InstrumentSpec spec = eurusd();
  CHECK(!notional_minor(1000, spec, valuation_at(0), 2).ok());

  spec.tick_size = Rational{0, 1};
  CHECK(!notional_minor(1000, spec, valuation_at(111100), 2).ok());

  spec = eurusd();
  CHECK(!notional_minor(1000, spec, valuation_at(111100, Rational{0, 1}), 2).ok());
}

TEST(unit_valuation, notional_rejects_an_unsupported_minor_scale) {
  Result<std::int64_t> n = notional_minor(1000, eurusd(), valuation_at(111100), 19);
  CHECK(!n.ok());
  CHECK_EQ(static_cast<int>(n.reason()),
           static_cast<int>(Reason::ScaleConversionUnsupported));
}

TEST(unit_valuation, notional_reports_overflow_instead_of_wrapping) {
  // G05 / PB-07: an absurd quantity must produce an error, not a small number.
  InstrumentSpec spec = eurusd();
  spec.tick_size = Rational{1, 1};
  spec.contract_multiplier = Rational{1'000'000'000, 1};
  Result<std::int64_t> n = notional_minor(
      std::numeric_limits<std::int64_t>::max() / 2, spec,
      valuation_at(1'000'000'000), 2);
  CHECK(!n.ok());
  CHECK_EQ(static_cast<int>(n.reason()),
           static_cast<int>(Reason::ArithmeticOverflow));
}

TEST(unit_valuation, margin_proxy_rounds_up) {
  // 5% of 11,110,000 = 555,500 exactly.
  Result<std::int64_t> exact = margin_proxy_minor(11'110'000, Rational{5, 100});
  CHECK(exact.ok());
  CHECK_EQ(exact.value(), 555'500);

  // 5% of 1 = 0.05, which must cost 1 minor unit.
  Result<std::int64_t> rounded = margin_proxy_minor(1, Rational{5, 100});
  CHECK(rounded.ok());
  CHECK_EQ(rounded.value(), 1);

  // A zero rate really is zero.
  Result<std::int64_t> zero = margin_proxy_minor(11'110'000, Rational{0, 1});
  CHECK(zero.ok());
  CHECK_EQ(zero.value(), 0);
}

TEST(unit_valuation, margin_proxy_rejects_bad_input) {
  CHECK(!margin_proxy_minor(-1, Rational{5, 100}).ok());
  CHECK(!margin_proxy_minor(100, Rational{-5, 100}).ok());
  CHECK(!margin_proxy_minor(100, Rational{1, 0}).ok());
}

TEST(unit_valuation, build_valuation_pins_versions_and_the_fx_path) {
  RiskPolicy policy = lab_policy();
  policy.fx_upper_buffer_bps = 0;

  MarketFeed feed;
  feed.trust_source("LAB-FEED");
  Quote q = quote_at(109990, 110010);
  q.ingest_mono = 1000;
  CHECK(feed.publish_quote(q).ok());

  const InstrumentSpec spec = policy.instruments.at("EURUSD");
  Result<Valuation> v = build_valuation(feed.snapshot(), policy, spec, 110000, 1000);
  CHECK_MSG(v.ok(), v.detail());
  CHECK_EQ(v.value().risk_price_ticks, 111111);  // ceil(110010 * 1.01)
  CHECK_EQ(v.value().quote_version, static_cast<DataVersion>(1));
  CHECK(v.value().fx_path_kind == FxPathKind::Identity);

  // The digest must change when any pinned input changes, because it is what
  // the commit-time freshness re-check compares.
  Valuation other = v.value();
  other.risk_price_ticks += 1;
  CHECK_NE(other.digest(), v.value().digest());
}

TEST(unit_valuation, build_valuation_fails_closed_on_a_stale_quote) {
  RiskPolicy policy = lab_policy();
  MarketFeed feed;
  feed.trust_source("LAB-FEED");
  Quote q = quote_at(109990, 110010);
  q.ingest_mono = 1000;
  CHECK(feed.publish_quote(q).ok());

  const InstrumentSpec spec = policy.instruments.at("EURUSD");
  Result<Valuation> v = build_valuation(feed.snapshot(), policy, spec, 110000,
                                        1000 + millis_to_nanos(spec.quote_ttl_ms));
  CHECK(!v.ok());
  CHECK_EQ(static_cast<int>(v.reason()), static_cast<int>(Reason::QuoteStale));
}

TEST(unit_valuation, build_valuation_fails_closed_without_an_fx_path) {
  RiskPolicy policy = lab_policy();
  policy.fx_pivot_currencies.clear();

  MarketFeed feed;
  feed.trust_source("LAB-FEED");
  Quote q = quote_at(159990, 160010);
  q.symbol = "EURJPY";
  q.ingest_mono = 1000;
  CHECK(feed.publish_quote(q).ok());

  const InstrumentSpec spec = policy.instruments.at("EURJPY");
  Result<Valuation> v = build_valuation(feed.snapshot(), policy, spec, 160000, 1000);
  CHECK(!v.ok());
  CHECK_EQ(static_cast<int>(v.reason()), static_cast<int>(Reason::FxPathMissing));
}
