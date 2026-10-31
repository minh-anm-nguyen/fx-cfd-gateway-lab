#include "risk/exposure.hpp"

#include "risk/checked.hpp"
#include "risk/hash.hpp"

namespace risk {

Result<std::int64_t> net_envelope_units(const SymbolExposure& e) {
  if (!e.structurally_valid()) {
    return Result<std::int64_t>::fail(Reason::InternalInvariant,
                                      "negative pending units");
  }
  std::int64_t upper = 0;  // P + B
  std::int64_t lower = 0;  // P - S
  if (!add_i64(e.position_units, e.pending_buy_units, upper)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "P+B");
  }
  if (!sub_i64(e.position_units, e.pending_sell_units, lower)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "P-S");
  }
  std::int64_t abs_upper = 0, abs_lower = 0;
  if (!abs_i64(upper, abs_upper) || !abs_i64(lower, abs_lower)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "envelope abs");
  }
  return Result<std::int64_t>(abs_upper > abs_lower ? abs_upper : abs_lower);
}

Result<std::int64_t> gross_units(const SymbolExposure& e) {
  if (!e.structurally_valid()) {
    return Result<std::int64_t>::fail(Reason::InternalInvariant,
                                      "negative pending units");
  }
  std::int64_t abs_position = 0;
  if (!abs_i64(e.position_units, abs_position)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "abs(P)");
  }
  std::int64_t total = 0;
  if (!add_i64(abs_position, e.pending_buy_units, total)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "abs(P)+B");
  }
  if (!add_i64(total, e.pending_sell_units, total)) {
    return Result<std::int64_t>::fail(Reason::ArithmeticOverflow, "abs(P)+B+S");
  }
  return Result<std::int64_t>(total);
}

// ---------------------------------------------------------------------------
// AccountExposure / DomainExposure
// ---------------------------------------------------------------------------

std::int64_t AccountExposure::working_orders() const {
  std::int64_t total = 0;
  for (const auto& [_, e] : symbols) {
    if (!add_i64(total, e.working_orders, total)) return total;
  }
  return total;
}

const SymbolExposure* AccountExposure::find(const SymbolId& symbol) const {
  const auto it = symbols.find(symbol);
  return it == symbols.end() ? nullptr : &it->second;
}

SymbolExposure& AccountExposure::mutable_symbol(const SymbolId& symbol) {
  return symbols[symbol];
}

void AccountExposure::prune() {
  for (auto it = symbols.begin(); it != symbols.end();) {
    const SymbolExposure& e = it->second;
    const bool empty = e.position_units == 0 && e.pending_buy_units == 0 &&
                       e.pending_sell_units == 0 && e.working_orders == 0;
    it = empty ? symbols.erase(it) : std::next(it);
  }
}

const AccountExposure* DomainExposure::find(const AccountId& account) const {
  const auto it = accounts.find(account);
  return it == accounts.end() ? nullptr : &it->second;
}

AccountExposure& DomainExposure::mutable_account(const AccountId& account) {
  return accounts[account];
}

void DomainExposure::prune() {
  for (auto it = accounts.begin(); it != accounts.end();) {
    it->second.prune();
    it = it->second.symbols.empty() ? accounts.erase(it) : std::next(it);
  }
}

std::string DomainExposure::state_hash() const {
  Hasher h;
  h.field("domain_exposure_v1");
  for (const auto& [account, ae] : accounts) {
    h.field(account);
    for (const auto& [symbol, e] : ae.symbols) {
      h.field(symbol);
      h.field(e.position_units);
      h.field(e.pending_buy_units);
      h.field(e.pending_sell_units);
      h.field(e.working_orders);
    }
    h.field("/account");
  }
  return h.hex();
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Result<DomainMetrics> evaluate_exposure(const DomainExposure& exposure,
                                        const RiskPolicy& policy,
                                        const ValuationTable& valuations) {
  DomainMetrics metrics;

  for (const auto& [account_id, account] : exposure.accounts) {
    AccountMetrics am;
    for (const auto& [symbol, e] : account.symbols) {
      const InstrumentSpec* spec = policy.find_instrument(symbol);
      if (spec == nullptr) {
        return Result<DomainMetrics>::fail(Reason::UnknownInstrument, symbol);
      }
      const auto vit = valuations.find(symbol);
      if (vit == valuations.end()) {
        // A symbol with live exposure and no fresh valuation cannot be scored.
        // Treating it as zero would make the account look safer than it is.
        return Result<DomainMetrics>::fail(Reason::QuoteMissing,
                                           "no valuation for " + symbol);
      }

      SymbolMetrics sm;
      sm.working_orders = e.working_orders;

      Result<std::int64_t> net = net_envelope_units(e);
      if (!net.ok()) return Result<DomainMetrics>::fail(net.reason(), net.detail());
      Result<std::int64_t> gross = gross_units(e);
      if (!gross.ok()) return Result<DomainMetrics>::fail(gross.reason(), gross.detail());
      sm.net_units = net.value();
      sm.gross_units = gross.value();

      Result<std::int64_t> net_minor = notional_minor(
          sm.net_units, *spec, vit->second, policy.account_currency.minor_scale);
      if (!net_minor.ok()) {
        return Result<DomainMetrics>::fail(net_minor.reason(), net_minor.detail());
      }
      Result<std::int64_t> gross_minor = notional_minor(
          sm.gross_units, *spec, vit->second, policy.account_currency.minor_scale);
      if (!gross_minor.ok()) {
        return Result<DomainMetrics>::fail(gross_minor.reason(), gross_minor.detail());
      }
      sm.net_notional_minor = net_minor.value();
      sm.gross_notional_minor = gross_minor.value();

      if (!add_i64(am.total_gross_notional_minor, sm.gross_notional_minor,
                   am.total_gross_notional_minor)) {
        return Result<DomainMetrics>::fail(Reason::ArithmeticOverflow,
                                           "account gross sum");
      }
      if (!add_i64(am.working_orders, sm.working_orders, am.working_orders)) {
        return Result<DomainMetrics>::fail(Reason::ArithmeticOverflow,
                                           "working order count");
      }
      am.symbols[symbol] = sm;
    }

    Result<std::int64_t> margin =
        margin_proxy_minor(am.total_gross_notional_minor, policy.margin_proxy_rate);
    if (!margin.ok()) {
      return Result<DomainMetrics>::fail(margin.reason(), margin.detail());
    }
    am.margin_proxy_minor = margin.value();

    // RI-09: accounts are summed, never netted against each other.
    if (!add_i64(metrics.firm_gross_notional_minor, am.total_gross_notional_minor,
                 metrics.firm_gross_notional_minor)) {
      return Result<DomainMetrics>::fail(Reason::ArithmeticOverflow, "firm gross sum");
    }
    metrics.accounts[account_id] = std::move(am);
  }

  return Result<DomainMetrics>(metrics);
}

const AccountMetrics* DomainMetrics::find(const AccountId& account) const {
  const auto it = accounts.find(account);
  return it == accounts.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Limit hierarchy
// ---------------------------------------------------------------------------

Status check_limits(const DomainMetrics& metrics, const RiskPolicy& policy,
                    const LimitCheckInput& input) {
  const Limits& L = policy.limits;

  // --- per request ---------------------------------------------------------
  if (input.candidate_order_notional_minor > L.max_order_notional_minor) {
    return Status::fail(Reason::LimitOrderNotional,
                        std::to_string(input.candidate_order_notional_minor) +
                            " > " + std::to_string(L.max_order_notional_minor));
  }

  // --- per chain -----------------------------------------------------------
  if (input.candidate_chain_units > L.max_chain_qty_base_units) {
    return Status::fail(Reason::LimitChainQty,
                        std::to_string(input.candidate_chain_units) + " > " +
                            std::to_string(L.max_chain_qty_base_units));
  }

  const AccountMetrics* am = metrics.find(input.account_id);

  // --- account-symbol ------------------------------------------------------
  if (am != nullptr) {
    const auto sit = am->symbols.find(input.symbol);
    if (sit != am->symbols.end()) {
      const SymbolMetrics& sm = sit->second;
      if (sm.net_notional_minor > L.max_account_symbol_net_minor) {
        return Status::fail(Reason::LimitAccountSymbolNet,
                            std::to_string(sm.net_notional_minor) + " > " +
                                std::to_string(L.max_account_symbol_net_minor));
      }
      if (sm.gross_notional_minor > L.max_account_symbol_gross_minor) {
        return Status::fail(Reason::LimitAccountSymbolGross,
                            std::to_string(sm.gross_notional_minor) + " > " +
                                std::to_string(L.max_account_symbol_gross_minor));
      }
      if (L.max_working_orders_per_account_symbol > 0 &&
          sm.working_orders > L.max_working_orders_per_account_symbol) {
        return Status::fail(Reason::LimitAccountWorkingCount,
                            input.symbol + " working orders " +
                                std::to_string(sm.working_orders));
      }
    }

    // --- account ----------------------------------------------------------
    if (am->total_gross_notional_minor > L.max_account_total_gross_minor) {
      return Status::fail(Reason::LimitAccountTotalGross,
                          std::to_string(am->total_gross_notional_minor) + " > " +
                              std::to_string(L.max_account_total_gross_minor));
    }
    if (L.max_working_orders_per_account > 0 &&
        am->working_orders > L.max_working_orders_per_account) {
      return Status::fail(Reason::LimitAccountWorkingCount,
                          "account working orders " +
                              std::to_string(am->working_orders));
    }

    // --- margin buffer ----------------------------------------------------
    // Required: margin_proxy + min_buffer <= budget. Computed as a subtraction
    // on the budget side so an overflow cannot produce a false pass.
    std::int64_t required = 0;
    if (!add_i64(am->margin_proxy_minor, L.min_margin_buffer_minor, required)) {
      return Status::fail(Reason::ArithmeticOverflow, "margin requirement");
    }
    if (required > input.margin_basis.budget_minor) {
      return Status::fail(Reason::LimitMarginBuffer,
                          "need " + std::to_string(required) + " have " +
                              std::to_string(input.margin_basis.budget_minor));
    }
  }

  // --- firm / global -------------------------------------------------------
  if (metrics.firm_gross_notional_minor > L.max_firm_gross_minor) {
    return Status::fail(Reason::LimitFirmGross,
                        std::to_string(metrics.firm_gross_notional_minor) + " > " +
                            std::to_string(L.max_firm_gross_minor));
  }

  return Status::success();
}

}  // namespace risk
