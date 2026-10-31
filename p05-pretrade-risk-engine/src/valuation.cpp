#include "risk/valuation.hpp"

#include <algorithm>

#include "risk/checked.hpp"
#include "risk/hash.hpp"

namespace risk {

std::string Valuation::digest() const {
  Hasher h;
  h.named("risk_price_ticks", risk_price_ticks)
      .named("fx_upper", rational_to_string(fx_upper))
      .named("quote_version", quote_version)
      .named("fx_version", fx_version)
      .named("fx_kind", static_cast<std::uint64_t>(fx_path_kind))
      .named("fx_path", fx_path);
  return h.hex();
}

bool pow10_i64(int scale, std::int64_t& out) noexcept {
  if (scale < 0 || scale > 18) return false;
  std::int64_t v = 1;
  for (int i = 0; i < scale; ++i) {
    if (!mul_i64(v, 10, v)) return false;
  }
  out = v;
  return true;
}

Status check_price_collar(std::int64_t limit_price_ticks, const Quote& quote,
                          std::int64_t max_deviation_bps) {
  if (max_deviation_bps <= 0) return Status::success();
  if (limit_price_ticks <= 0) {
    return Status::fail(Reason::PriceNonPositive, "limit price");
  }

  // Compare |2*limit - (bid+ask)| * 10000 against bps * (bid+ask), i.e. the
  // deviation from the midpoint, without ever dividing.
  std::int64_t sum = 0;
  if (!add_i64(quote.bid_ticks, quote.ask_ticks, sum)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar midpoint");
  }
  std::int64_t twice_limit = 0;
  if (!mul_i64(limit_price_ticks, 2, twice_limit)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar limit");
  }
  std::int64_t diff = 0;
  if (!sub_i64(twice_limit, sum, diff)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar diff");
  }
  std::int64_t abs_diff = 0;
  if (!abs_i64(diff, abs_diff)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar abs");
  }

  I128 lhs, rhs;
  if (!mul_to_i128(abs_diff, 10000, lhs)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar lhs");
  }
  if (!mul_to_i128(max_deviation_bps, sum, rhs)) {
    return Status::fail(Reason::ArithmeticOverflow, "collar rhs");
  }
  if (rhs < lhs) {
    return Status::fail(Reason::LimitPriceCollar,
                        "limit price deviates from the midpoint by more than " +
                            std::to_string(max_deviation_bps) + " bps");
  }
  return Status::success();
}

Result<std::int64_t> risk_price_upper_ticks(std::int64_t limit_price_ticks,
                                            const Quote& quote,
                                            const InstrumentSpec& spec) {
  if (limit_price_ticks <= 0) {
    return Result<std::int64_t>::fail(Reason::PriceNonPositive, "limit price");
  }
  if (quote.ask_ticks <= 0) {
    return Result<std::int64_t>::fail(Reason::PriceNonPositive, "quote ask");
  }

  const std::int64_t base = std::max(limit_price_ticks, quote.ask_ticks);

  // base * (10000 + buffer_bps) / 10000, rounded up so the bound never falls
  // below the exact value (RI-10).
  std::int64_t factor = 0;
  if (!add_i64(10000, spec.risk_price_upper_buffer_bps, factor)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "risk price factor");
  }
  I128 scaled;
  if (!mul_to_i128(base, factor, scaled)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "risk price scale");
  }
  I128 ceiled;
  if (!i128_div_ceil_nonneg(scaled, i128_from_i64(10000), ceiled)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "risk price ceil");
  }
  std::int64_t out = 0;
  if (!i128_to_i64(ceiled, out)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "risk price narrow");
  }
  if (out < base) {
    // Would mean the buffer reduced the bound; that can only be a bug, and
    // silently continuing would understate risk.
    return Result<std::int64_t>::fail(Reason::InternalInvariant,
                                      "risk price bound below its base");
  }
  return Result<std::int64_t>(out);
}

Result<Valuation> build_valuation(const MarketView& view, const RiskPolicy& policy,
                                  const InstrumentSpec& spec,
                                  std::int64_t limit_price_ticks,
                                  MonoNanos now_mono) {
  Result<Quote> q = view.quote(spec.symbol, now_mono, spec.quote_ttl_ms);
  if (!q.ok()) return Result<Valuation>::fail(q.reason(), q.detail());

  if (const Status collar =
          check_price_collar(limit_price_ticks, q.value(), spec.max_price_deviation_bps);
      !collar) {
    return Result<Valuation>::fail(collar.reason(), collar.detail());
  }

  Result<std::int64_t> price = risk_price_upper_ticks(limit_price_ticks, q.value(), spec);
  if (!price.ok()) return Result<Valuation>::fail(price.reason(), price.detail());

  Result<FxResolution> fx =
      view.fx_upper(spec.quote_currency, policy.account_currency.code, now_mono,
                    policy.fx_ttl_ms, policy.fx_pivot_currencies,
                    policy.fx_upper_buffer_bps);
  if (!fx.ok()) return Result<Valuation>::fail(fx.reason(), fx.detail());

  Valuation v;
  v.risk_price_ticks = price.value();
  v.fx_upper = fx.value().upper_bound;
  v.quote_version = q.value().version;
  v.fx_version = fx.value().version;
  v.fx_path_kind = fx.value().kind;
  v.fx_path = fx.value().path;
  return Result<Valuation>(v);
}

Result<std::int64_t> notional_minor(std::int64_t units, const InstrumentSpec& spec,
                                    const Valuation& valuation, int minor_scale) {
  if (units < 0) {
    return Result<std::int64_t>::fail(Reason::NegativeRiskCharge,
                                      "notional over negative units");
  }
  if (units == 0) return Result<std::int64_t>(static_cast<std::int64_t>(0));
  if (valuation.risk_price_ticks <= 0) {
    return Result<std::int64_t>::fail(Reason::PriceNonPositive, "risk price");
  }
  if (!spec.tick_size.positive() || !spec.contract_multiplier.positive() ||
      !valuation.fx_upper.positive()) {
    return Result<std::int64_t>::fail(Reason::PolicyInvalid,
                                      "non-positive valuation factor");
  }

  std::int64_t scale_factor = 0;
  if (!pow10_i64(minor_scale, scale_factor)) {
    return Result<std::int64_t>::fail(Reason::ScaleConversionUnsupported,
                                      "minor scale " + std::to_string(minor_scale));
  }

  RatioAcc acc(units);
  if (!acc.mul_i64(valuation.risk_price_ticks) ||
      !acc.mul_rational(spec.tick_size) ||
      !acc.mul_rational(spec.contract_multiplier) ||
      !acc.mul_rational(valuation.fx_upper) || !acc.mul_i64(scale_factor)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow,
                                      "notional accumulation");
  }

  std::int64_t out = 0;
  if (!acc.ceil_to_i64_nonneg(out)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow,
                                      "notional narrowing: " + acc.to_string());
  }
  return Result<std::int64_t>(out);
}

Result<std::int64_t> margin_proxy_minor(std::int64_t gross_notional_minor,
                                        const Rational& rate) {
  if (gross_notional_minor < 0) {
    return Result<std::int64_t>::fail(Reason::NegativeRiskCharge, "margin proxy");
  }
  if (!rate.valid() || rate.num < 0) {
    return Result<std::int64_t>::fail(Reason::PolicyInvalid, "margin rate");
  }
  RatioAcc acc(gross_notional_minor);
  if (!acc.mul_rational(rate)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "margin proxy");
  }
  std::int64_t out = 0;
  if (!acc.ceil_to_i64_nonneg(out)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "margin narrowing");
  }
  return Result<std::int64_t>(out);
}

}  // namespace risk
