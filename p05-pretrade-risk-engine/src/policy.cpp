#include "risk/policy.hpp"

#include <algorithm>

#include "risk/hash.hpp"

namespace risk {
namespace {

// Two spellings of the same rate -- 5/100 and 1/20 -- are the same policy, so
// the digest normalises before stringifying. Without this, reloading a
// rendered policy would change its hash and every token pinned to it.
std::string canonical_rational(Rational r) {
  (void)rational_reduce(r);
  return rational_to_string(r);
}

}  // namespace

std::string_view domain_mode_name(DomainMode m) noexcept {
  switch (m) {
    case DomainMode::Normal: return "NORMAL";
    case DomainMode::NoNew: return "NO_NEW";
    case DomainMode::CancelOnly: return "CANCEL_ONLY";
    case DomainMode::ReconOnly: return "RECON_ONLY";
  }
  return "UNKNOWN";
}

bool parse_domain_mode(std::string_view text, DomainMode& out) noexcept {
  if (text == "NORMAL") { out = DomainMode::Normal; return true; }
  if (text == "NO_NEW") { out = DomainMode::NoNew; return true; }
  if (text == "CANCEL_ONLY") { out = DomainMode::CancelOnly; return true; }
  if (text == "RECON_ONLY") { out = DomainMode::ReconOnly; return true; }
  return false;
}

bool mode_blocks_risk_increase(DomainMode m) noexcept {
  return m != DomainMode::Normal;
}

bool parse_product_mode(std::string_view text, ProductMode& out) noexcept {
  if (text == "SPOT_CFD") {
    out = ProductMode::SpotCfd;
    return true;
  }
  return false;
}

Status InstrumentSpec::validate() const {
  const Status sym = validate_symbol(symbol);
  if (!sym) return sym;

  const Currency base{base_currency, 2};
  const Currency quote{quote_currency, 2};
  if (!base.valid()) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": bad base_currency");
  }
  if (!quote.valid()) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": bad quote_currency");
  }
  if (base_currency == quote_currency) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": base and quote currency are identical");
  }
  if (quantity_step_base_units <= 0) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": quantity_step must be > 0");
  }
  if (min_qty_base_units <= 0 || max_qty_base_units <= 0) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": qty bounds must be > 0");
  }
  if (min_qty_base_units > max_qty_base_units) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": min_qty > max_qty");
  }
  if (min_qty_base_units % quantity_step_base_units != 0) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": min_qty is not a multiple of the step");
  }
  if (max_qty_base_units % quantity_step_base_units != 0) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": max_qty is not a multiple of the step");
  }
  if (!tick_size.positive()) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": tick_size must be > 0");
  }
  if (!contract_multiplier.positive()) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": contract_multiplier must be > 0");
  }
  if (quote_ttl_ms <= 0) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": quote_ttl_ms must be > 0");
  }
  if (risk_price_upper_buffer_bps < 0 || risk_price_upper_buffer_bps > 100000) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": risk_price_upper_buffer_bps out of range");
  }
  if (max_price_deviation_bps < 0 || max_price_deviation_bps > 1000000) {
    return Status::fail(Reason::PolicyInvalid,
                        symbol + ": max_price_deviation_bps out of range");
  }
  if (version == 0) {
    return Status::fail(Reason::PolicyInvalid, symbol + ": version must be > 0");
  }
  return Status::success();
}

Status Limits::validate() const {
  const std::pair<const char*, std::int64_t> fields[] = {
      {"max_order_notional_minor", max_order_notional_minor},
      {"max_chain_qty_base_units", max_chain_qty_base_units},
      {"max_account_symbol_net_minor", max_account_symbol_net_minor},
      {"max_account_symbol_gross_minor", max_account_symbol_gross_minor},
      {"max_account_total_gross_minor", max_account_total_gross_minor},
      {"max_firm_gross_minor", max_firm_gross_minor},
      {"min_margin_buffer_minor", min_margin_buffer_minor},
      {"max_working_orders_per_account", max_working_orders_per_account},
      {"max_working_orders_per_account_symbol",
       max_working_orders_per_account_symbol},
      {"max_admissions_per_second", max_admissions_per_second},
  };
  for (const auto& [name, value] : fields) {
    if (value < 0) {
      return Status::fail(Reason::PolicyInvalid,
                          std::string(name) + " must not be negative");
    }
  }
  // A gross cap below the net cap would make the net check unreachable, which
  // usually means the operator transposed two numbers.
  if (max_account_symbol_gross_minor < max_account_symbol_net_minor) {
    return Status::fail(Reason::PolicyInvalid,
                        "account-symbol gross cap is below the net cap");
  }
  if (max_account_total_gross_minor < max_account_symbol_gross_minor) {
    return Status::fail(Reason::PolicyInvalid,
                        "account total gross cap is below the per-symbol cap");
  }
  if (max_firm_gross_minor < max_account_total_gross_minor) {
    return Status::fail(Reason::PolicyInvalid,
                        "firm gross cap is below the per-account cap");
  }
  return Status::success();
}

Status QueueCaps::validate() const {
  if (admission_cap <= 0 || event_cap <= 0 || recon_cap <= 0) {
    return Status::fail(Reason::PolicyInvalid, "queue caps must be > 0");
  }
  return Status::success();
}

Status SafetyFlags::validate() const {
  if (allow_market_orders) {
    // RI-11: there is no upper price bound for a market order in this model.
    return Status::fail(Reason::PolicyInvalid,
                        "allow_market_orders is not supported in v1");
  }
  if (allow_reduce_only_new) {
    // §7.3: reduce-only needs an authoritative position plus LP enforcement.
    return Status::fail(Reason::PolicyInvalid,
                        "allow_reduce_only_new is not supported in v1");
  }
  if (auto_release_on_timeout) {
    // RI-26: a timeout is not evidence.
    return Status::fail(Reason::PolicyInvalid,
                        "auto_release_on_timeout must remain false");
  }
  if (max_pending_modify_per_chain != 1) {
    // RI-11 / P-11: v1 allows exactly one in-flight modification per chain.
    return Status::fail(Reason::PolicyInvalid,
                        "max_pending_modify_per_chain must be 1 in v1");
  }
  return Status::success();
}

Status RiskPolicy::validate() const {
  if (version == 0) {
    return Status::fail(Reason::PolicyInvalid, "policy version must be > 0");
  }
  if (account_mode != "NETTING") {
    return Status::fail(Reason::PolicyInvalid,
                        "only NETTING accounts are supported in v1");
  }
  if (!account_currency.valid()) {
    return Status::fail(Reason::PolicyInvalid, "invalid account_currency");
  }
  if (order_types.empty() || order_types.size() != 1 ||
      order_types.front() != OrderType::Limit) {
    return Status::fail(Reason::PolicyInvalid, "only LIMIT orders are supported");
  }
  if (time_in_force.empty() || time_in_force.size() != 1 ||
      time_in_force.front() != TimeInForce::Day) {
    return Status::fail(Reason::PolicyInvalid, "only DAY time-in-force is supported");
  }
  if (instruments.empty()) {
    return Status::fail(Reason::PolicyInvalid, "no instruments configured");
  }
  if (fx_ttl_ms <= 0 || equity_ttl_ms <= 0) {
    return Status::fail(Reason::PolicyInvalid, "TTLs must be > 0");
  }
  if (fx_upper_buffer_bps < 0 || fx_upper_buffer_bps > 100000) {
    return Status::fail(Reason::PolicyInvalid, "fx_upper_buffer_bps out of range");
  }
  for (const std::string& pivot : fx_pivot_currencies) {
    if (!Currency{pivot, 2}.valid()) {
      return Status::fail(Reason::PolicyInvalid,
                          "invalid fx pivot currency: " + pivot);
    }
  }
  if (!margin_proxy_rate.valid() || margin_proxy_rate.num < 0) {
    return Status::fail(Reason::PolicyInvalid, "invalid margin_proxy_rate");
  }
  int cmp = 0;
  if (!rational_compare(margin_proxy_rate, Rational{1, 1}, cmp) || cmp > 0) {
    return Status::fail(Reason::PolicyInvalid, "margin_proxy_rate must be <= 1");
  }

  for (const auto& [symbol, spec] : instruments) {
    if (symbol != spec.symbol) {
      return Status::fail(Reason::PolicyInvalid,
                          "instrument key does not match its symbol field");
    }
    const Status s = spec.validate();
    if (!s) return s;
  }
  for (const auto& [id, profile] : accounts) {
    if (id != profile.account_id) {
      return Status::fail(Reason::PolicyInvalid,
                          "account key does not match its account_id field");
    }
    const Status s = validate_identifier(id, "account_id");
    if (!s) return s;
    if (profile.margin_budget_minor < 0) {
      return Status::fail(Reason::PolicyInvalid,
                          id + ": margin_budget_minor must not be negative");
    }
  }

  if (const Status s = limits.validate(); !s) return s;
  if (const Status s = queues.validate(); !s) return s;
  if (const Status s = safety.validate(); !s) return s;
  return Status::success();
}

std::string RiskPolicy::hash() const {
  Hasher h;
  h.named("version", version)
      .named("mode", domain_mode_name(mode))
      .named("account_mode", account_mode)
      .named("account_currency", account_currency.code)
      .named("account_minor_scale",
             static_cast<std::int64_t>(account_currency.minor_scale))
      .named("fx_ttl_ms", fx_ttl_ms)
      .named("equity_ttl_ms", equity_ttl_ms)
      .named("fx_upper_buffer_bps", fx_upper_buffer_bps)
      .named("margin_proxy_rate", canonical_rational(margin_proxy_rate));

  h.field("fx_pivots");
  for (const std::string& pivot : fx_pivot_currencies) h.field(pivot);

  h.field("limits");
  h.field(limits.max_order_notional_minor);
  h.field(limits.max_chain_qty_base_units);
  h.field(limits.max_account_symbol_net_minor);
  h.field(limits.max_account_symbol_gross_minor);
  h.field(limits.max_account_total_gross_minor);
  h.field(limits.max_firm_gross_minor);
  h.field(limits.min_margin_buffer_minor);
  h.field(limits.max_working_orders_per_account);
  h.field(limits.max_working_orders_per_account_symbol);
  h.field(limits.max_admissions_per_second);

  h.field("queues");
  h.field(queues.admission_cap);
  h.field(queues.event_cap);
  h.field(queues.recon_cap);

  h.field("safety");
  h.flag(safety.require_atomic_outbox);
  h.flag(safety.abort_on_stale_data);
  h.flag(safety.allow_market_orders);
  h.flag(safety.allow_reduce_only_new);
  h.flag(safety.auto_release_on_timeout);
  h.field(safety.max_pending_modify_per_chain);

  // std::map iteration is ordered, so the digest is stable without an
  // explicit sort step.
  h.field("instruments");
  for (const auto& [symbol, spec] : instruments) {
    h.field(symbol);
    h.field(spec.base_currency);
    h.field(spec.quote_currency);
    h.field(spec.quantity_step_base_units);
    h.field(spec.min_qty_base_units);
    h.field(spec.max_qty_base_units);
    h.field(canonical_rational(spec.tick_size));
    h.field(canonical_rational(spec.contract_multiplier));
    h.field(spec.quote_ttl_ms);
    h.field(spec.risk_price_upper_buffer_bps);
    h.field(spec.max_price_deviation_bps);
    h.field(static_cast<std::uint64_t>(spec.product_mode));
    h.flag(spec.enabled);
    h.field(spec.version);
  }

  h.field("accounts");
  for (const auto& [id, profile] : accounts) {
    h.field(id);
    h.field(profile.margin_budget_minor);
    h.flag(profile.use_equity_feed);
    h.flag(profile.enabled);
  }

  return h.hex();
}

const InstrumentSpec* RiskPolicy::find_instrument(const SymbolId& symbol) const {
  const auto it = instruments.find(symbol);
  return it == instruments.end() ? nullptr : &it->second;
}

const AccountProfile* RiskPolicy::find_account(const AccountId& account) const {
  const auto it = accounts.find(account);
  return it == accounts.end() ? nullptr : &it->second;
}

bool RiskPolicy::allows_order_type(OrderType t) const {
  return std::find(order_types.begin(), order_types.end(), t) != order_types.end();
}

bool RiskPolicy::allows_time_in_force(TimeInForce t) const {
  return std::find(time_in_force.begin(), time_in_force.end(), t) !=
         time_in_force.end();
}

RiskPolicy lab_policy() {
  RiskPolicy p;
  p.version = 1;
  p.mode = DomainMode::Normal;
  p.account_mode = "NETTING";
  p.account_currency = Currency{"USD", 2};
  p.fx_ttl_ms = 500;
  p.equity_ttl_ms = 1000;
  p.fx_upper_buffer_bps = 50;
  p.fx_pivot_currencies = {"USD"};

  InstrumentSpec eurusd;
  eurusd.symbol = "EURUSD";
  eurusd.base_currency = "EUR";
  eurusd.quote_currency = "USD";
  eurusd.quantity_step_base_units = 1000;
  eurusd.min_qty_base_units = 1000;
  eurusd.max_qty_base_units = 200000;
  eurusd.tick_size = Rational{1, 100000};
  eurusd.contract_multiplier = Rational{1, 1};
  eurusd.quote_ttl_ms = 500;
  eurusd.risk_price_upper_buffer_bps = 100;
  eurusd.max_price_deviation_bps = 500;
  eurusd.version = 1;
  p.instruments[eurusd.symbol] = eurusd;

  InstrumentSpec gbpusd = eurusd;
  gbpusd.symbol = "GBPUSD";
  gbpusd.base_currency = "GBP";
  p.instruments[gbpusd.symbol] = gbpusd;

  // EURJPY exists so the FX conversion path (quote currency != account
  // currency) is exercised by the default fixtures rather than only by a
  // special-case test.
  InstrumentSpec eurjpy = eurusd;
  eurjpy.symbol = "EURJPY";
  eurjpy.base_currency = "EUR";
  eurjpy.quote_currency = "JPY";
  eurjpy.tick_size = Rational{1, 1000};
  p.instruments[eurjpy.symbol] = eurjpy;

  // The §19.3 example pairs a 100,000-unit max quantity with a 10,000,000
  // minor-unit order cap, but 100,000 EUR valued at the lab price is about
  // 11,110,000 cents, so the two numbers contradict each other. The caps here
  // are the spec's shape with consistent magnitudes: a single max-size order
  // fits, and the 150,000-unit replace used by the §24.5 scenario fits too.
  p.limits.max_order_notional_minor = 15'000'000;
  p.limits.max_chain_qty_base_units = 200'000;
  p.limits.max_account_symbol_net_minor = 20'000'000;
  p.limits.max_account_symbol_gross_minor = 30'000'000;
  p.limits.max_account_total_gross_minor = 50'000'000;
  p.limits.max_firm_gross_minor = 100'000'000;
  p.limits.min_margin_buffer_minor = 500'000;
  p.limits.max_working_orders_per_account = 64;
  p.limits.max_working_orders_per_account_symbol = 32;
  p.limits.max_admissions_per_second = 0;  // 0 == rate limiting disabled

  p.margin_proxy_rate = Rational{5, 100};

  AccountProfile a1;
  a1.account_id = "ACC-A";
  a1.margin_budget_minor = 10'000'000;
  p.accounts[a1.account_id] = a1;

  AccountProfile a2 = a1;
  a2.account_id = "ACC-B";
  p.accounts[a2.account_id] = a2;

  AccountProfile a3 = a1;
  a3.account_id = "ACC-C";
  p.accounts[a3.account_id] = a3;

  return p;
}

}  // namespace risk
