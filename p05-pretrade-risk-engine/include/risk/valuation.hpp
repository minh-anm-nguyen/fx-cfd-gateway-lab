// Conservative valuation: base units -> minor units of the account currency.
//
// Two rules drive everything here:
//   * RI-10, monotone bounding: every narrowing rounds up, so the engine never
//     understates a charge by a sub-minor-unit amount;
//   * RI-11, no unbounded market: if a defensible upper bound for the price or
//     the FX rate cannot be computed, the command is refused rather than valued
//     with a midpoint.
#pragma once

#include <cstdint>
#include <string>

#include "risk/market.hpp"
#include "risk/policy.hpp"
#include "risk/rational.hpp"
#include "risk/result.hpp"

namespace risk {

// The pinned, conservative price/FX basis for one decision. Stored on the
// reservation so a later revaluation can be compared against what approved it.
struct Valuation {
  std::int64_t risk_price_ticks = 0;  // conservative upper price, in ticks
  Rational fx_upper{1, 1};            // account currency per quote currency
  DataVersion quote_version = 0;
  DataVersion fx_version = 0;
  FxPathKind fx_path_kind = FxPathKind::Identity;
  std::string fx_path;

  std::string digest() const;
};

// 10^scale with overflow checking; `scale` must be within [0, 18].
bool pow10_i64(int scale, std::int64_t& out) noexcept;

// Price collar (§9.2): rejects a limit price too far from the current midpoint.
// Comparison is done by cross-multiplication so no division or float is
// involved. A deviation cap of 0 disables the check.
Status check_price_collar(std::int64_t limit_price_ticks, const Quote& quote,
                          std::int64_t max_deviation_bps);

// Conservative upper price for valuing an order of either side.
//
// A LIMIT BUY cannot fill above its limit, but the resulting *position* is
// still marked at market, and a LIMIT SELL fills at or above its limit. Taking
// max(limit, ask) and then adding the instrument's buffer bounds both cases
// with one formula. The model deliberately over-reserves rather than branching
// on side with a weaker bound (§9.2).
Result<std::int64_t> risk_price_upper_ticks(std::int64_t limit_price_ticks,
                                            const Quote& quote,
                                            const InstrumentSpec& spec);

// Resolves quote + FX into a pinned Valuation. `now_mono` is the monotonic
// reading the TTLs are measured against.
Result<Valuation> build_valuation(const MarketView& view,
                                  const RiskPolicy& policy,
                                  const InstrumentSpec& spec,
                                  std::int64_t limit_price_ticks,
                                  MonoNanos now_mono);

// notional_minor = ceil(units x price_ticks x tick_size x multiplier x fx x 10^scale)
//
// `units` must be nonnegative: callers pass an exposure magnitude, never a
// signed position, so a negative value can never become a credit (RI-10).
Result<std::int64_t> notional_minor(std::int64_t units, const InstrumentSpec& spec,
                                    const Valuation& valuation, int minor_scale);

// margin_proxy_minor = ceil(gross_notional_minor x margin_proxy_rate).
// A lab proxy only; it is not a broker margin figure (§9.3).
Result<std::int64_t> margin_proxy_minor(std::int64_t gross_notional_minor,
                                        const Rational& rate);

}  // namespace risk
