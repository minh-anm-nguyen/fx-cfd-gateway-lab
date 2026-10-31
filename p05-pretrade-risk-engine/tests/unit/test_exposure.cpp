// G06 — exposure envelope and the limit hierarchy.
//
// The envelope identities are the heart of §10, so they are tested both on the
// worked example from §10.2 and on the structural properties that make the
// formula safe (transfer invariance, no cross-account netting).
#include <limits>

#include "risk/exposure.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

SymbolExposure make(std::int64_t position, std::int64_t buys, std::int64_t sells,
                    std::int64_t working = 1) {
  SymbolExposure e;
  e.position_units = position;
  e.pending_buy_units = buys;
  e.pending_sell_units = sells;
  e.working_orders = working;
  return e;
}

std::int64_t envelope(const SymbolExposure& e) {
  Result<std::int64_t> r = net_envelope_units(e);
  CHECK_MSG(r.ok(), std::string(reason_name(r.reason())));
  return r.value();
}

std::int64_t gross(const SymbolExposure& e) {
  Result<std::int64_t> r = gross_units(e);
  CHECK_MSG(r.ok(), std::string(reason_name(r.reason())));
  return r.value();
}

// The worked example from §10.2, in base units.
Valuation eurusd_valuation() {
  Valuation v;
  v.risk_price_ticks = 110000;  // 1.10000 with a 1/100000 tick
  v.fx_upper = Rational{1, 1};
  v.quote_version = 1;
  v.fx_version = 1;
  return v;
}

RiskPolicy policy_with_generous_caps() {
  RiskPolicy p = lab_policy();
  p.limits.max_order_notional_minor = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.max_chain_qty_base_units = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.max_account_symbol_net_minor = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.max_account_symbol_gross_minor = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.max_account_total_gross_minor = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.max_firm_gross_minor = std::numeric_limits<std::int64_t>::max() / 4;
  p.limits.min_margin_buffer_minor = 0;
  return p;
}

}  // namespace

TEST(unit_exposure, spec_worked_example_10_2) {
  // P=+10,000, BUY pending 40,000, SELL pending 20,000.
  const SymbolExposure e = make(10000, 40000, 20000);
  CHECK_EQ(envelope(e), 50000);  // max(|10k+40k|, |10k-20k|)
  CHECK_EQ(gross(e), 70000);     // |10k| + 40k + 20k
}

TEST(unit_exposure, spec_worked_example_after_a_buy_fill) {
  // §10.2: a 15,000 BUY fill moves P to 25,000 and B to 25,000. Neither the
  // envelope nor the gross proxy may drop because of the transfer alone.
  const SymbolExposure before = make(10000, 40000, 20000);
  const SymbolExposure after = make(25000, 25000, 20000);
  CHECK_EQ(envelope(before), envelope(after));
  CHECK_EQ(gross(before), gross(after));
  CHECK_EQ(envelope(after), 50000);
  CHECK_EQ(gross(after), 70000);
}

TEST(unit_exposure, transfer_invariance_holds_for_every_split) {
  // RI-07 as a property: for any fill f <= B, moving f from B into P leaves
  // P+B unchanged, so the upper branch of the envelope cannot shrink.
  for (std::int64_t f = 0; f <= 40000; f += 1000) {
    const SymbolExposure moved = make(10000 + f, 40000 - f, 20000);
    CHECK_EQ(moved.position_units + moved.pending_buy_units, 50000);
    CHECK(envelope(moved) >= 30000);
  }
}

TEST(unit_exposure, sell_fill_is_symmetric) {
  // After a SELL fill f: P decreases by f and S decreases by f, so P-S is
  // unchanged.
  const SymbolExposure before = make(10000, 0, 20000);
  const SymbolExposure after = make(10000 - 5000, 0, 20000 - 5000);
  CHECK_EQ(before.position_units - before.pending_sell_units,
           after.position_units - after.pending_sell_units);
  CHECK_EQ(envelope(before), envelope(after));
}

TEST(unit_exposure, short_position_uses_the_lower_branch) {
  const SymbolExposure e = make(-30000, 5000, 10000);
  // |P+B| = 25,000 but |P-S| = 40,000, so the short side governs.
  CHECK_EQ(envelope(e), 40000);
  CHECK_EQ(gross(e), 45000);
}

TEST(unit_exposure, flat_book_with_two_sided_orders) {
  const SymbolExposure e = make(0, 30000, 30000);
  CHECK_EQ(envelope(e), 30000);
  // Gross is strictly larger: two-sided exposure is activity, not a hedge.
  CHECK_EQ(gross(e), 60000);
}

TEST(unit_exposure, gross_is_never_below_the_net_envelope) {
  const std::int64_t positions[] = {-50000, -1000, 0, 1000, 50000};
  const std::int64_t pendings[] = {0, 1, 1000, 100000};
  for (const std::int64_t p : positions) {
    for (const std::int64_t b : pendings) {
      for (const std::int64_t s : pendings) {
        const SymbolExposure e = make(p, b, s);
        CHECK_MSG(gross(e) >= envelope(e),
                  "P=" + std::to_string(p) + " B=" + std::to_string(b) +
                      " S=" + std::to_string(s));
      }
    }
  }
}

TEST(unit_exposure, envelope_rejects_negative_pending) {
  SymbolExposure bad = make(0, -1, 0);
  Result<std::int64_t> r = net_envelope_units(bad);
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()), static_cast<int>(Reason::InternalInvariant));
}

TEST(unit_exposure, envelope_reports_overflow_instead_of_wrapping) {
  SymbolExposure e;
  e.position_units = std::numeric_limits<std::int64_t>::max();
  e.pending_buy_units = 1;
  Result<std::int64_t> r = net_envelope_units(e);
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()),
           static_cast<int>(Reason::ArithmeticOverflow));
}

TEST(unit_exposure, state_hash_is_insensitive_to_empty_rows) {
  DomainExposure a;
  a.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(100, 0, 0, 0);
  DomainExposure b = a;
  b.mutable_account("ACC-B").mutable_symbol("GBPUSD") = make(0, 0, 0, 0);
  b.prune();
  CHECK_EQ(a.state_hash(), b.state_hash());
}

TEST(unit_exposure, state_hash_changes_with_any_field) {
  DomainExposure base;
  base.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(100, 200, 300, 1);
  const std::string h = base.state_hash();

  DomainExposure moved = base;
  moved.mutable_account("ACC-A").mutable_symbol("EURUSD").position_units = 101;
  CHECK_NE(moved.state_hash(), h);

  DomainExposure renamed;
  renamed.mutable_account("ACC-B").mutable_symbol("EURUSD") = make(100, 200, 300, 1);
  CHECK_NE(renamed.state_hash(), h);

  // Swapping B and S must change the digest: the two are not interchangeable.
  DomainExposure swapped;
  swapped.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(100, 300, 200, 1);
  CHECK_NE(swapped.state_hash(), h);
}

TEST(unit_exposure, notional_conversion_matches_the_spec_example) {
  // §10.2: E = 50,000 EUR at 1.10000 USD/EUR is 55,000 USD = 5,500,000 cents;
  // G = 70,000 EUR is 77,000 USD = 7,700,000 cents.
  RiskPolicy policy = policy_with_generous_caps();
  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD") =
      make(10000, 40000, 20000);

  ValuationTable valuations;
  valuations["EURUSD"] = eurusd_valuation();

  Result<DomainMetrics> metrics = evaluate_exposure(exposure, policy, valuations);
  CHECK_MSG(metrics.ok(), metrics.detail());

  const AccountMetrics* am = metrics.value().find("ACC-A");
  CHECK(am != nullptr);
  const SymbolMetrics& sm = am->symbols.at("EURUSD");
  CHECK_EQ(sm.net_units, 50000);
  CHECK_EQ(sm.gross_units, 70000);
  CHECK_EQ(sm.net_notional_minor, 5'500'000);
  CHECK_EQ(sm.gross_notional_minor, 7'700'000);

  // Margin proxy at 5% of gross: 385,000 cents = 3,850 USD.
  CHECK_EQ(am->margin_proxy_minor, 385'000);
  CHECK_EQ(metrics.value().firm_gross_notional_minor, 7'700'000);
}

TEST(unit_exposure, a_symbol_without_a_valuation_is_an_error_not_a_zero) {
  // RI-12: a symbol that cannot be valued must not contribute nothing, which
  // would make the account look safer than it is.
  RiskPolicy policy = policy_with_generous_caps();
  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(0, 10000, 0);
  exposure.mutable_account("ACC-A").mutable_symbol("GBPUSD") = make(0, 10000, 0);

  ValuationTable partial;
  partial["EURUSD"] = eurusd_valuation();

  Result<DomainMetrics> metrics = evaluate_exposure(exposure, policy, partial);
  CHECK(!metrics.ok());
  CHECK_EQ(static_cast<int>(metrics.reason()),
           static_cast<int>(Reason::QuoteMissing));
}

TEST(unit_exposure, accounts_are_summed_never_netted) {
  // RI-09 / P-15: account A long and account B short does not free firm budget.
  RiskPolicy policy = policy_with_generous_caps();
  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(100000, 0, 0);
  exposure.mutable_account("ACC-B").mutable_symbol("EURUSD") = make(-100000, 0, 0);

  ValuationTable valuations;
  valuations["EURUSD"] = eurusd_valuation();

  Result<DomainMetrics> metrics = evaluate_exposure(exposure, policy, valuations);
  CHECK_MSG(metrics.ok(), metrics.detail());
  // Each account contributes 100,000 EUR -> 110,000 USD -> 11,000,000 cents.
  CHECK_EQ(metrics.value().firm_gross_notional_minor, 22'000'000);
}

TEST(unit_limits, each_scope_reports_its_own_reason) {
  RiskPolicy policy = policy_with_generous_caps();
  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(0, 100000, 0);
  ValuationTable valuations;
  valuations["EURUSD"] = eurusd_valuation();
  Result<DomainMetrics> metrics = evaluate_exposure(exposure, policy, valuations);
  CHECK_MSG(metrics.ok(), metrics.detail());

  LimitCheckInput input;
  input.account_id = "ACC-A";
  input.symbol = "EURUSD";
  input.candidate_order_notional_minor = 11'000'000;
  input.candidate_chain_units = 100000;
  input.margin_basis.budget_minor = 100'000'000;

  CHECK(check_limits(metrics.value(), policy, input).ok());

  {
    RiskPolicy tight = policy;
    tight.limits.max_order_notional_minor = 10'999'999;
    const Status s = check_limits(metrics.value(), tight, input);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()),
             static_cast<int>(Reason::LimitOrderNotional));
  }
  {
    RiskPolicy tight = policy;
    tight.limits.max_chain_qty_base_units = 99999;
    const Status s = check_limits(metrics.value(), tight, input);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::LimitChainQty));
  }
  {
    RiskPolicy tight = policy;
    tight.limits.max_account_symbol_net_minor = 10'999'999;
    const Status s = check_limits(metrics.value(), tight, input);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()),
             static_cast<int>(Reason::LimitAccountSymbolNet));
  }
  {
    RiskPolicy tight = policy;
    tight.limits.max_account_symbol_gross_minor = 10'999'999;
    // The net cap must stay at or below the gross cap for the bundle to be
    // coherent, so lower it too; the gross breach is still the one reported
    // because the net check uses the same number here.
    tight.limits.max_account_symbol_net_minor = 10'999'999;
    const Status s = check_limits(metrics.value(), tight, input);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()),
             static_cast<int>(Reason::LimitAccountSymbolNet));
  }
  {
    RiskPolicy tight = policy;
    tight.limits.max_firm_gross_minor = 10'999'999;
    const Status s = check_limits(metrics.value(), tight, input);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::LimitFirmGross));
  }
  {
    // Margin: 5% of 11,000,000 is 550,000. With a 100,000 buffer the account
    // needs 650,000 of budget.
    LimitCheckInput poor = input;
    poor.margin_basis.budget_minor = 649'999;
    RiskPolicy with_buffer = policy;
    with_buffer.limits.min_margin_buffer_minor = 100'000;
    const Status s = check_limits(metrics.value(), with_buffer, poor);
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()),
             static_cast<int>(Reason::LimitMarginBuffer));

    poor.margin_basis.budget_minor = 650'000;
    CHECK(check_limits(metrics.value(), with_buffer, poor).ok());
  }
}

TEST(unit_limits, working_order_counts_are_capped_per_scope) {
  RiskPolicy policy = policy_with_generous_caps();
  policy.limits.max_working_orders_per_account_symbol = 2;
  policy.limits.max_working_orders_per_account = 3;

  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD") = make(0, 1000, 0, 3);
  ValuationTable valuations;
  valuations["EURUSD"] = eurusd_valuation();
  Result<DomainMetrics> metrics = evaluate_exposure(exposure, policy, valuations);
  CHECK_MSG(metrics.ok(), metrics.detail());

  LimitCheckInput input;
  input.account_id = "ACC-A";
  input.symbol = "EURUSD";
  input.margin_basis.budget_minor = 100'000'000;
  const Status s = check_limits(metrics.value(), policy, input);
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()),
           static_cast<int>(Reason::LimitAccountWorkingCount));
}

TEST(unit_limits, a_zero_cap_denies_rather_than_meaning_unlimited) {
  RiskPolicy policy = policy_with_generous_caps();
  policy.limits.max_order_notional_minor = 0;

  DomainMetrics empty;
  LimitCheckInput input;
  input.account_id = "ACC-A";
  input.symbol = "EURUSD";
  input.candidate_order_notional_minor = 1;
  const Status s = check_limits(empty, policy, input);
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()),
           static_cast<int>(Reason::LimitOrderNotional));
}
