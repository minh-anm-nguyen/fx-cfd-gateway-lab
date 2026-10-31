// Risk policy: the immutable snapshot of limits and reference data that a
// single admission decision is evaluated against.
//
// A policy is a *value*. The coordinator never mutates one in place; a policy
// change installs a new version with a strictly greater number inside the same
// commit boundary as its audit record (§7.2), so every decision can name the
// exact ruleset that produced it (RI-30).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "risk/rational.hpp"
#include "risk/result.hpp"
#include "risk/types.hpp"

namespace risk {

// §7.3. The mode gates which commands may be admitted at all; it is checked
// before any limit arithmetic so a kill switch cannot be raced by a slow
// valuation (RI-28).
enum class DomainMode : std::uint8_t {
  Normal = 0,      // risk-increasing commands allowed when all checks pass
  NoNew = 1,       // New/Replace-up blocked; cancels and fills still flow
  CancelOnly = 2,  // no new risk tokens at all; cancels do not release holds
  ReconOnly = 3,   // evidence and recovery only
};

std::string_view domain_mode_name(DomainMode m) noexcept;
bool parse_domain_mode(std::string_view text, DomainMode& out) noexcept;

// True when the mode forbids issuing a token that increases exposure.
bool mode_blocks_risk_increase(DomainMode m) noexcept;

// Product models the engine can value correctly. Anything else is denied
// rather than approximated (§10.3).
enum class ProductMode : std::uint8_t { SpotCfd = 0 };
bool parse_product_mode(std::string_view text, ProductMode& out) noexcept;

struct InstrumentSpec {
  SymbolId symbol;
  std::string base_currency;
  std::string quote_currency;

  std::int64_t quantity_step_base_units = 0;
  std::int64_t min_qty_base_units = 0;
  std::int64_t max_qty_base_units = 0;

  Rational tick_size{1, 100000};
  Rational contract_multiplier{1, 1};

  std::int64_t quote_ttl_ms = 500;
  // Added to the conservative risk price so a LIMIT order's notional bound is
  // not merely the limit price (§9.2).
  std::int64_t risk_price_upper_buffer_bps = 0;
  // Rejects a limit price too far from the current market (price collar).
  std::int64_t max_price_deviation_bps = 0;

  ProductMode product_mode = ProductMode::SpotCfd;
  bool enabled = true;
  DataVersion version = 1;

  Status validate() const;
};

// §7.1 limit hierarchy. Every cap is in minor units of the account reporting
// currency unless the name says base units. A cap of 0 means "deny everything"
// and is a legal, if drastic, configuration; it is never read as "unlimited".
struct Limits {
  std::int64_t max_order_notional_minor = 0;
  std::int64_t max_chain_qty_base_units = 0;
  std::int64_t max_account_symbol_net_minor = 0;
  std::int64_t max_account_symbol_gross_minor = 0;
  std::int64_t max_account_total_gross_minor = 0;
  std::int64_t max_firm_gross_minor = 0;
  std::int64_t min_margin_buffer_minor = 0;
  std::int64_t max_working_orders_per_account = 0;
  std::int64_t max_working_orders_per_account_symbol = 0;
  std::int64_t max_admissions_per_second = 0;

  Status validate() const;
};

struct QueueCaps {
  std::int64_t admission_cap = 32768;
  std::int64_t event_cap = 65536;
  std::int64_t recon_cap = 10000;

  Status validate() const;
};

// Flags that exist specifically so an unsafe configuration has to be written
// down explicitly rather than reached by accident.
struct SafetyFlags {
  bool require_atomic_outbox = true;
  bool abort_on_stale_data = true;
  bool allow_market_orders = false;
  bool allow_reduce_only_new = false;
  bool auto_release_on_timeout = false;  // must stay false: RI-26
  std::int64_t max_pending_modify_per_chain = 1;

  Status validate() const;
};

struct AccountProfile {
  AccountId account_id;
  // Independent conservative margin budget, used instead of an external equity
  // feed when `use_equity_feed` is false (§9.3).
  std::int64_t margin_budget_minor = 0;
  bool use_equity_feed = false;
  bool enabled = true;
};

struct RiskPolicy {
  PolicyVersion version = 1;
  DomainMode mode = DomainMode::Normal;
  std::string account_mode = "NETTING";

  std::vector<OrderType> order_types{OrderType::Limit};
  std::vector<TimeInForce> time_in_force{TimeInForce::Day};

  std::map<SymbolId, InstrumentSpec> instruments;
  std::map<AccountId, AccountProfile> accounts;

  Currency account_currency{"USD", 2};
  std::int64_t fx_ttl_ms = 500;
  std::int64_t equity_ttl_ms = 1000;

  // Spread/stress buffer added on top of the unfavourable FX side (§9.1). Kept
  // in policy, and therefore in the policy hash, so a decision can never be
  // re-derived with a different buffer than the one that approved it.
  std::int64_t fx_upper_buffer_bps = 0;
  // Ordered whitelist of intermediate currencies for a two-hop conversion. A
  // conversion reachable through two different pivots is denied as ambiguous
  // rather than resolved by picking the first one.
  std::vector<std::string> fx_pivot_currencies;

  Limits limits;
  Rational margin_proxy_rate{5, 100};
  QueueCaps queues;
  SafetyFlags safety;

  // Full-bundle validation (§7.2 step 1). A policy that does not validate is
  // never installed, so the engine cannot run on a half-sane ruleset.
  Status validate() const;

  // Canonical hash over every field that can change a decision. Pinned into
  // each reservation so an audit can prove which ruleset applied (RI-01).
  std::string hash() const;

  const InstrumentSpec* find_instrument(const SymbolId& symbol) const;
  const AccountProfile* find_account(const AccountId& account) const;

  bool allows_order_type(OrderType t) const;
  bool allows_time_in_force(TimeInForce t) const;
};

// The lab policy from §19.3, built in code so tests do not depend on file I/O.
RiskPolicy lab_policy();

}  // namespace risk
