// Exposure projection (P, B, S), the worst-case envelope, and the limit
// hierarchy check.
//
// §10. For one symbol of a NETTING account, inventory after any subset of the
// outstanding orders fills lies in [P - S, P + B]. The worst-case absolute net
// inventory is therefore
//     E = max(|P + B|, |P - S|)
// and the conservative activity proxy is
//     G = |P| + B + S.
// `B` and `S` are *risk-effective* outstanding units: they include pending New
// orders and the conservative branch of a pending Replace, and they are not
// reduced by a cancel request that has not been confirmed (RI-04, RI-05).
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "risk/policy.hpp"
#include "risk/result.hpp"
#include "risk/types.hpp"
#include "risk/valuation.hpp"

namespace risk {

struct SymbolExposure {
  std::int64_t position_units = 0;       // signed; from the confirmed fill ledger
  std::int64_t pending_buy_units = 0;    // >= 0
  std::int64_t pending_sell_units = 0;   // >= 0
  std::int64_t working_orders = 0;       // >= 0, count of live reservations

  bool structurally_valid() const noexcept {
    return pending_buy_units >= 0 && pending_sell_units >= 0 && working_orders >= 0;
  }
};

// E and G in base units. Both return an error rather than a wrapped value when
// the sums overflow (RI-13).
Result<std::int64_t> net_envelope_units(const SymbolExposure& e);
Result<std::int64_t> gross_units(const SymbolExposure& e);

struct AccountExposure {
  std::map<SymbolId, SymbolExposure> symbols;

  std::int64_t working_orders() const;
  const SymbolExposure* find(const SymbolId& symbol) const;
  SymbolExposure& mutable_symbol(const SymbolId& symbol);
  // Drops rows that carry no exposure at all, so the state hash of two
  // equivalent states cannot differ by an empty row.
  void prune();
};

struct DomainExposure {
  std::map<AccountId, AccountExposure> accounts;

  const AccountExposure* find(const AccountId& account) const;
  AccountExposure& mutable_account(const AccountId& account);
  void prune();

  // Canonical hash over the whole projection. Equality of this digest is the
  // oracle for "restart reproduced the committed state" (P-13).
  std::string state_hash() const;
};

// Valuations pinned for this evaluation, one per symbol that carries exposure.
using ValuationTable = std::map<SymbolId, Valuation>;

struct SymbolMetrics {
  std::int64_t net_units = 0;
  std::int64_t gross_units = 0;
  std::int64_t net_notional_minor = 0;
  std::int64_t gross_notional_minor = 0;
  std::int64_t working_orders = 0;
};

struct AccountMetrics {
  std::map<SymbolId, SymbolMetrics> symbols;
  std::int64_t total_gross_notional_minor = 0;
  std::int64_t margin_proxy_minor = 0;
  std::int64_t working_orders = 0;
};

struct DomainMetrics {
  std::map<AccountId, AccountMetrics> accounts;
  std::int64_t firm_gross_notional_minor = 0;

  const AccountMetrics* find(const AccountId& account) const;
};

// Converts a projection into money metrics.
//
// Every symbol carrying exposure must have a valuation in `valuations`; a
// missing one yields Reason::QuoteMissing rather than a zero contribution,
// because a symbol that cannot be valued must not look risk-free (RI-12).
Result<DomainMetrics> evaluate_exposure(const DomainExposure& exposure,
                                        const RiskPolicy& policy,
                                        const ValuationTable& valuations);

// The per-account money available to back a margin proxy. Either derived from
// a fresh equity snapshot or from the account's independent lab budget (§9.3).
struct MarginBasis {
  std::int64_t budget_minor = 0;
  bool from_equity_feed = false;
  DataVersion equity_version = 0;
};

// §7.1. Checks every scope that applies to one candidate command. All scopes
// must pass; the first failure is reported with its specific reason code so the
// audit records which cap bound the decision.
struct LimitCheckInput {
  AccountId account_id;
  SymbolId symbol;
  std::int64_t candidate_order_notional_minor = 0;
  std::int64_t candidate_chain_units = 0;
  MarginBasis margin_basis;
};

Status check_limits(const DomainMetrics& metrics, const RiskPolicy& policy,
                    const LimitCheckInput& input);

}  // namespace risk
