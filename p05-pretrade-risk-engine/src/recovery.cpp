#include "risk/recovery.hpp"

#include <algorithm>
#include <set>

#include "risk/audit.hpp"
#include "risk/journal_store.hpp"

namespace risk {

std::string RecoveryReport::render() const {
  std::string out;
  out += "journal_records_applied=" + std::to_string(journal_records_applied) + "\n";
  out += "truncated_tail_bytes=" + std::to_string(truncated_tail_bytes) + "\n";
  out += "reservations_total=" + std::to_string(reservations_total) + "\n";
  out += "reservations_holding_budget=" +
         std::to_string(reservations_holding_budget) + "\n";
  out += "unknown_outcomes=" + std::to_string(unknown_outcomes) + "\n";
  out += "quarantined_chains=" + std::to_string(quarantined_chains) + "\n";
  out += "quarantined_accounts=" + std::to_string(quarantined_accounts) + "\n";
  out += "undispatched_intents=" + std::to_string(undispatched_intents) + "\n";
  out += "projection_consistent=" + std::string(projection_consistent ? "1" : "0") + "\n";
  out += "audit_chain_valid=" + std::string(audit_chain_valid ? "1" : "0") + "\n";
  out += "feeds_fresh=" + std::string(feeds_fresh ? "1" : "0") + "\n";
  out += "resulting_mode=" + std::string(domain_mode_name(resulting_mode)) + "\n";
  out += "admission_open=" + std::string(admission_open ? "1" : "0") + "\n";
  out += "state_hash=" + state_hash + "\n";
  for (const std::string& b : blockers) out += "blocker=" + b + "\n";
  return out;
}

Result<RecoveryReport> run_recovery(RiskCoordinator& coordinator, RiskStore& store,
                                    MarketFeed& feed, Clock& clock,
                                    const RecoveryOptions& options) {
  RecoveryReport report;
  report.state_hash = store.state_hash();

  if (const auto* journal = dynamic_cast<const JournalStore*>(&store)) {
    report.journal_records_applied = journal->stats().records_applied;
    report.truncated_tail_bytes = journal->stats().truncated_tail_bytes;
    // A torn tail is the expected shape of a crash mid-append: the record was
    // never acknowledged, so discarding it is correct, not a corruption.
    if (report.truncated_tail_bytes > 0) {
      report.blockers.push_back(
          "discarded " + std::to_string(report.truncated_tail_bytes) +
          " bytes of an unacknowledged journal tail");
    }
  }

  // --- rebuild and cross-check the projection -----------------------------
  const std::vector<Reservation> reservations = store.reservations();
  report.reservations_total = reservations.size();

  std::set<SymbolId> symbols_in_use;
  for (const Reservation& r : reservations) {
    if (r.holds_budget()) {
      ++report.reservations_holding_budget;
      symbols_in_use.insert(r.symbol);
    }
    if (r.state == ReservationState::Unknown) ++report.unknown_outcomes;
    if (r.state == ReservationState::Quarantine) ++report.quarantined_chains;
  }
  const DomainExposure stored = store.exposure();
  for (const auto& [account, ae] : stored.accounts) {
    (void)account;
    for (const auto& [symbol, se] : ae.symbols) {
      (void)se;
      symbols_in_use.insert(symbol);
    }
  }
  for (const SymbolId& s : options.required_symbols) symbols_in_use.insert(s);

  if (const Status s = coordinator.cross_check_projection(); s) {
    report.projection_consistent = true;
  } else {
    report.blockers.push_back("projection inconsistent: " + s.detail());
  }

  // --- audit chain --------------------------------------------------------
  const AuditChainReport chain = verify_audit_chain(store.audit());
  report.audit_chain_valid = chain.ok;
  if (!chain.ok) {
    report.blockers.push_back("audit chain broken: " + chain.detail);
  }
  if (const Status s = verify_every_reservation_audited(reservations, store.audit());
      !s) {
    report.blockers.push_back("unaudited reservation: " + s.detail());
  }

  // --- unresolved outcomes ------------------------------------------------
  for (const OutboxIntent& o : store.outbox()) {
    if (!o.dispatched) ++report.undispatched_intents;
  }
  report.quarantined_accounts = store.quarantined_accounts().size();

  if (report.unknown_outcomes > 0) {
    report.blockers.push_back(std::to_string(report.unknown_outcomes) +
                              " chain(s) with an unresolved outcome");
  }
  if (report.quarantined_chains > 0) {
    report.blockers.push_back(std::to_string(report.quarantined_chains) +
                              " quarantined chain(s)");
  }
  if (report.undispatched_intents > 0) {
    // Not necessarily a fault: the dispatcher may simply resume. It still has
    // to be reported, because a durable intent that was never claimed may or
    // may not have reached the LP (CW-13/CW-14).
    report.blockers.push_back(std::to_string(report.undispatched_intents) +
                              " order intent(s) not yet dispatched");
  }

  // --- feed freshness -----------------------------------------------------
  const RiskPolicy policy = store.policy();
  const MarketView view = feed.snapshot();
  const MonoNanos now = clock.mono_now();
  bool fresh = true;
  for (const SymbolId& symbol : symbols_in_use) {
    const InstrumentSpec* spec = policy.find_instrument(symbol);
    if (spec == nullptr) {
      fresh = false;
      report.blockers.push_back("no instrument spec for " + symbol);
      continue;
    }
    Result<Quote> q = view.quote(symbol, now, spec->quote_ttl_ms);
    if (!q.ok()) {
      fresh = false;
      report.blockers.push_back("stale or missing quote for " + symbol + ": " +
                                std::string(reason_name(q.reason())));
      continue;
    }
    Result<FxResolution> fx =
        view.fx_upper(spec->quote_currency, policy.account_currency.code, now,
                      policy.fx_ttl_ms, policy.fx_pivot_currencies,
                      policy.fx_upper_buffer_bps);
    if (!fx.ok()) {
      fresh = false;
      report.blockers.push_back("no usable FX path for " + symbol + ": " +
                                std::string(reason_name(fx.reason())));
    }
  }
  report.feeds_fresh = fresh;

  // --- decide the resulting gate -----------------------------------------
  const bool hard_block = !report.projection_consistent || !report.audit_chain_valid;
  if (hard_block) {
    report.resulting_mode = DomainMode::ReconOnly;
    report.admission_open = false;
  } else if (!report.blockers.empty() || !fresh) {
    // Holds are kept; only evidence and recovery traffic is accepted.
    report.resulting_mode = DomainMode::ReconOnly;
    report.admission_open = options.allow_open_admission;
  } else {
    report.resulting_mode = policy.mode;
    report.admission_open = options.allow_open_admission;
  }

  Principal recovery_principal;
  recovery_principal.principal_id = "recovery";
  recovery_principal.tenant_id = store.domain_state().tenant_id;
  recovery_principal.roles = {Role::Operator};

  const RiskResult mode_result = coordinator.set_mode(
      report.resulting_mode, recovery_principal,
      "recovery: " + std::to_string(report.blockers.size()) + " blocker(s)");
  if (!mode_result.allowed()) {
    return Result<RecoveryReport>::fail(mode_result.reason, mode_result.detail);
  }

  const RiskResult gate_result = coordinator.open_admission(
      report.admission_open,
      report.admission_open ? "recovery gates passed" : "recovery gates failed");
  if (!gate_result.allowed()) {
    return Result<RecoveryReport>::fail(gate_result.reason, gate_result.detail);
  }

  report.state_hash = store.state_hash();
  return Result<RecoveryReport>(report);
}

Status assert_replay_equivalence(const RiskStore& live, const RiskStore& replayed) {
  const std::string a = live.state_hash();
  const std::string b = replayed.state_hash();
  if (a == b) return Status::success();

  // Narrow the report down to the first component that differs, so a failing
  // crash test says *what* diverged rather than only that something did.
  std::string detail = "state hash mismatch";
  if (live.domain_state().digest() != replayed.domain_state().digest()) {
    detail += "; domain state differs";
  }
  if (live.exposure().state_hash() != replayed.exposure().state_hash()) {
    detail += "; projection differs";
  }
  if (live.policy().hash() != replayed.policy().hash()) {
    detail += "; policy differs";
  }
  const std::vector<Reservation> lr = live.reservations();
  const std::vector<Reservation> rr = replayed.reservations();
  if (lr.size() != rr.size()) {
    detail += "; reservation count " + std::to_string(lr.size()) + " vs " +
              std::to_string(rr.size());
  } else {
    for (std::size_t i = 0; i < lr.size(); ++i) {
      if (lr[i].digest() != rr[i].digest()) {
        detail += "; reservation " + lr[i].reservation_id + " differs";
        break;
      }
    }
  }
  if (live.outbox().size() != replayed.outbox().size()) {
    detail += "; outbox count " + std::to_string(live.outbox().size()) + " vs " +
              std::to_string(replayed.outbox().size());
  }
  return Status::fail(Reason::SnapshotMismatch, detail);
}

}  // namespace risk
