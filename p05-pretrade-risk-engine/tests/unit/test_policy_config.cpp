// G16 (policy validation) and the configuration loader.
//
// The loader is strict on purpose: the dangerous failure mode for a risk
// config is not a parse error, it is a typo that silently becomes a zero cap
// or an unlimited one. Every case below is a shape that a lenient parser would
// have accepted.
#include "risk/config.hpp"
#include "risk/policy.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

// The §19.3 example, with the two consistency fixes the lab policy also makes
// (a notional cap that can actually accommodate a max-size order, and the
// explicit account block the engine needs).
const char* kLabYaml = R"YAML(
risk_policy_version: 1
mode: NORMAL
account_mode: NETTING
order_types: [LIMIT]
time_in_force: [DAY]
account_currency: USD
account_currency_minor_scale: 2
fx_ttl_ms: 500
equity_ttl_ms: 1000
fx_upper_buffer_bps: 50
fx_pivot_currencies: [USD]
margin_proxy_rate: "5/100"
instruments:
  EURUSD:
    base_currency: EUR
    quote_currency: USD
    quantity_step_base_units: 1000
    min_qty_base_units: 1000
    max_qty_base_units: 200000
    tick_size: "1/100000"
    contract_multiplier: "1/1"
    quote_ttl_ms: 500
    risk_price_upper_buffer_bps: 100
    max_price_deviation_bps: 500
    product_mode: SPOT_CFD
    enabled: true
    version: 1
accounts:
  ACC-A:
    margin_budget_minor: 10000000
    use_equity_feed: false
    enabled: true
limits:
  max_order_notional_minor: 15000000
  max_chain_qty_base_units: 200000
  max_account_symbol_net_minor: 20000000
  max_account_symbol_gross_minor: 30000000
  max_account_total_gross_minor: 50000000
  max_firm_gross_minor: 100000000
  min_margin_buffer_minor: 500000
  max_working_orders_per_account: 64
  max_working_orders_per_account_symbol: 32
  max_admissions_per_second: 0
queues:
  admission_cap: 32768
  event_cap: 65536
  recon_cap: 10000
safety:
  require_atomic_outbox: true
  abort_on_stale_data: true
  allow_market_orders: false
  allow_reduce_only_new: false
  auto_release_on_timeout: false
  max_pending_modify_per_chain: 1
)YAML";

Result<RiskPolicy> load(const std::string& text) {
  Result<YamlNode> root = parse_yaml_subset(text);
  if (!root.ok()) return Result<RiskPolicy>::fail(root.reason(), root.detail());
  return policy_from_yaml(root.value());
}

std::string with_replacement(const std::string& from, const std::string& to) {
  std::string out = kLabYaml;
  const std::size_t at = out.find(from);
  CHECK_MSG(at != std::string::npos, "pattern not in fixture: " + from);
  out.replace(at, from.size(), to);
  return out;
}

}  // namespace

TEST(unit_policy, the_lab_policy_validates) {
  const RiskPolicy p = lab_policy();
  const Status s = p.validate();
  CHECK_MSG(s.ok(), s.detail());
}

TEST(unit_policy, hash_changes_with_every_decision_relevant_field) {
  const RiskPolicy base = lab_policy();
  const std::string h = base.hash();

  {
    RiskPolicy p = base;
    p.version += 1;
    CHECK_NE(p.hash(), h);
  }
  {
    RiskPolicy p = base;
    p.limits.max_firm_gross_minor -= 1;
    CHECK_NE(p.hash(), h);
  }
  {
    RiskPolicy p = base;
    p.margin_proxy_rate = Rational{6, 100};
    CHECK_NE(p.hash(), h);
  }
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").tick_size = Rational{1, 10000};
    CHECK_NE(p.hash(), h);
  }
  {
    RiskPolicy p = base;
    p.accounts.at("ACC-A").margin_budget_minor += 1;
    CHECK_NE(p.hash(), h);
  }
  {
    RiskPolicy p = base;
    p.fx_upper_buffer_bps += 1;
    CHECK_NE(p.hash(), h);
  }
  {
    // Rebuilding the same policy must give the same hash: std::map ordering
    // makes the digest independent of insertion order.
    RiskPolicy p = lab_policy();
    CHECK_EQ(p.hash(), h);
  }
}

TEST(unit_policy, rejects_unsupported_safety_flags) {
  {
    RiskPolicy p = lab_policy();
    p.safety.allow_market_orders = true;
    const Status s = p.validate();
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::PolicyInvalid));
  }
  {
    // RI-26: a configuration that lets a timeout release a hold must not be
    // installable at all.
    RiskPolicy p = lab_policy();
    p.safety.auto_release_on_timeout = true;
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.safety.allow_reduce_only_new = true;
    CHECK(!p.validate().ok());
  }
  {
    // P-11: more than one pending modification per chain is not supported.
    RiskPolicy p = lab_policy();
    p.safety.max_pending_modify_per_chain = 2;
    CHECK(!p.validate().ok());
  }
}

TEST(unit_policy, rejects_an_incoherent_limit_hierarchy) {
  {
    RiskPolicy p = lab_policy();
    p.limits.max_account_symbol_gross_minor =
        p.limits.max_account_symbol_net_minor - 1;
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.limits.max_firm_gross_minor = p.limits.max_account_total_gross_minor - 1;
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.limits.max_order_notional_minor = -1;
    CHECK(!p.validate().ok());
  }
}

TEST(unit_policy, rejects_unsupported_scope) {
  {
    RiskPolicy p = lab_policy();
    p.account_mode = "HEDGING";
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.instruments.clear();
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.margin_proxy_rate = Rational{101, 100};
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.account_currency = Currency{"usd", 2};
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = lab_policy();
    p.version = 0;
    CHECK(!p.validate().ok());
  }
}

TEST(unit_policy, rejects_bad_instrument_metadata) {
  const RiskPolicy base = lab_policy();
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").quantity_step_base_units = 0;
    CHECK(!p.validate().ok());
  }
  {
    // A min quantity that is not a multiple of the step can never be ordered.
    RiskPolicy p = base;
    p.instruments.at("EURUSD").min_qty_base_units = 1500;
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").min_qty_base_units = 300000;
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").tick_size = Rational{0, 1};
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").quote_currency = "EUR";  // same as base
    CHECK(!p.validate().ok());
  }
  {
    RiskPolicy p = base;
    p.instruments.at("EURUSD").quote_ttl_ms = 0;
    CHECK(!p.validate().ok());
  }
  {
    // A key that disagrees with the record's own symbol would let a lookup and
    // an aggregation disagree about which instrument they are talking about.
    RiskPolicy p = base;
    p.instruments["GBPUSD"].symbol = "EURUSD";
    CHECK(!p.validate().ok());
  }
}

TEST(unit_config, loads_the_spec_example) {
  Result<RiskPolicy> p = load(kLabYaml);
  CHECK_MSG(p.ok(), std::string(reason_name(p.reason())) + ": " + p.detail());
  CHECK_EQ(p.value().version, static_cast<PolicyVersion>(1));
  CHECK_EQ(p.value().instruments.size(), static_cast<std::size_t>(1));
  CHECK_EQ(p.value().accounts.size(), static_cast<std::size_t>(1));
  CHECK_EQ(p.value().limits.max_firm_gross_minor, 100'000'000);
  CHECK_EQ(p.value().margin_proxy_rate.num, 1);
  CHECK_EQ(p.value().margin_proxy_rate.den, 20);  // 5/100 reduced
  CHECK_EQ(p.value().instruments.at("EURUSD").tick_size.den, 100000);
  CHECK_EQ(p.value().fx_pivot_currencies.size(), static_cast<std::size_t>(1));
}

TEST(unit_config, round_trips_through_the_renderer) {
  Result<RiskPolicy> first = load(kLabYaml);
  CHECK_MSG(first.ok(), first.detail());
  Result<RiskPolicy> second = load(policy_to_yaml(first.value()));
  CHECK_MSG(second.ok(), second.detail());
  // The hash covers every decision-relevant field, so equality here means the
  // renderer and the loader agree about all of them.
  CHECK_EQ(second.value().hash(), first.value().hash());
}

TEST(unit_config, the_lab_policy_round_trips) {
  const RiskPolicy p = lab_policy();
  Result<RiskPolicy> reloaded = load(policy_to_yaml(p));
  CHECK_MSG(reloaded.ok(), reloaded.detail());
  CHECK_EQ(reloaded.value().hash(), p.hash());
}

TEST(unit_config, rejects_an_unknown_key) {
  // The whole point of the strict loader: a misspelled cap must not be
  // silently ignored and left at its default.
  Result<RiskPolicy> p = load(with_replacement("  max_firm_gross_minor: 100000000",
                                               "  max_firm_gross_minr: 100000000"));
  CHECK(!p.ok());
  CHECK_EQ(static_cast<int>(p.reason()), static_cast<int>(Reason::PolicyInvalid));
  CHECK(p.detail().find("max_firm_gross_minr") != std::string::npos);
}

TEST(unit_config, rejects_a_non_integer_cap) {
  Result<RiskPolicy> p = load(with_replacement("  max_firm_gross_minor: 100000000",
                                               "  max_firm_gross_minor: 1_000_000"));
  CHECK(!p.ok());
}

TEST(unit_config, rejects_a_float_tick_size) {
  Result<RiskPolicy> p =
      load(with_replacement("    tick_size: \"1/100000\"",
                            "    tick_size: \"0.00001\""));
  CHECK(!p.ok());
}

TEST(unit_config, rejects_tabs_and_odd_indentation) {
  {
    Result<YamlNode> r = parse_yaml_subset("a:\n\tb: 1\n");
    CHECK(!r.ok());
    CHECK(r.detail().find("tabs") != std::string::npos);
  }
  {
    Result<YamlNode> r = parse_yaml_subset("a:\n   b: 1\n");
    CHECK(!r.ok());
  }
}

TEST(unit_config, rejects_anchors_aliases_and_block_sequences) {
  CHECK(!parse_yaml_subset("a: &anchor 1\nb: *anchor\n").ok());
  CHECK(!parse_yaml_subset("a:\n  - 1\n  - 2\n").ok());
  CHECK(!parse_yaml_subset("---\na: 1\n").ok());
}

TEST(unit_config, rejects_a_duplicate_key) {
  Result<YamlNode> r = parse_yaml_subset("a: 1\na: 2\n");
  CHECK(!r.ok());
  CHECK(r.detail().find("duplicate") != std::string::npos);
}

TEST(unit_config, rejects_an_empty_block) {
  CHECK(!parse_yaml_subset("a:\n").ok());
  CHECK(!parse_yaml_subset("").ok());
  CHECK(!parse_yaml_subset("just-a-scalar\n").ok());
}

TEST(unit_config, strips_comments_outside_quotes) {
  Result<YamlNode> r = parse_yaml_subset("a: 1 # trailing\nb: \"x#y\"\n");
  CHECK_MSG(r.ok(), r.detail());
  CHECK_EQ(r.value().child("a")->scalar, std::string("1"));
  CHECK_EQ(r.value().child("b")->scalar, std::string("x#y"));
}

TEST(unit_config, rejects_an_unsupported_order_type_or_tif) {
  CHECK(!load(with_replacement("order_types: [LIMIT]", "order_types: [MARKET]")).ok());
  CHECK(!load(with_replacement("time_in_force: [DAY]", "time_in_force: [GTC]")).ok());
}

TEST(unit_config, rejects_an_unsupported_product_mode) {
  Result<RiskPolicy> p = load(with_replacement("    product_mode: SPOT_CFD",
                                               "    product_mode: INVERSE_PERP"));
  CHECK(!p.ok());
  CHECK_EQ(static_cast<int>(p.reason()),
           static_cast<int>(Reason::UnsupportedProductMode));
}

TEST(unit_config, rejects_an_unsafe_safety_block) {
  Result<RiskPolicy> p = load(with_replacement("  auto_release_on_timeout: false",
                                               "  auto_release_on_timeout: true"));
  CHECK(!p.ok());
  CHECK_EQ(static_cast<int>(p.reason()), static_cast<int>(Reason::PolicyInvalid));
}

TEST(unit_config, rejects_a_non_boolean_flag) {
  CHECK(!load(with_replacement("  allow_market_orders: false",
                               "  allow_market_orders: no")).ok());
}

TEST(unit_config, requires_the_mandatory_blocks) {
  // Dropping the limits block entirely must be an error, not a default of zero
  // (which would deny everything) or infinity (which would allow everything).
  std::string text = kLabYaml;
  const std::size_t at = text.find("limits:");
  CHECK(at != std::string::npos);
  const std::size_t end = text.find("queues:");
  text.erase(at, end - at);
  Result<RiskPolicy> p = load(text);
  CHECK(!p.ok());
}
