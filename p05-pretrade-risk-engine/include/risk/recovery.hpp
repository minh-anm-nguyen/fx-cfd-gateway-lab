// Startup recovery (§16.3) and the admission gate it controls.
//
// RI-32: admission stays closed until the journal has been verified, the
// projection has been re-derived and cross-checked against the reservation
// ledger, every unresolved outcome has been enumerated, and the reference
// feeds are fresh. Anything missing leaves the domain in RECON_ONLY with all
// holds intact -- a failed recovery never widens risk.
#pragma once

#include <string>
#include <vector>

#include "risk/coordinator.hpp"
#include "risk/store.hpp"

namespace risk {

struct RecoveryOptions {
  // When true, the domain is allowed to open admission once every gate passes.
  bool allow_open_admission = true;
  // Symbols whose feeds must be fresh even if they carry no exposure. Empty
  // means "only the symbols that matter right now".
  std::vector<SymbolId> required_symbols;
};

struct RecoveryReport {
  std::uint64_t journal_records_applied = 0;
  std::uint64_t truncated_tail_bytes = 0;

  std::size_t reservations_total = 0;
  std::size_t reservations_holding_budget = 0;
  std::size_t unknown_outcomes = 0;
  std::size_t quarantined_chains = 0;
  std::size_t undispatched_intents = 0;
  std::size_t quarantined_accounts = 0;

  bool projection_consistent = false;
  bool audit_chain_valid = false;
  bool feeds_fresh = false;

  DomainMode resulting_mode = DomainMode::ReconOnly;
  bool admission_open = false;
  std::string state_hash;

  // Human-readable reasons admission was not opened. Empty means clean.
  std::vector<std::string> blockers;

  bool clean() const { return blockers.empty(); }
  std::string render() const;
};

// Runs the gates and commits the resulting mode / admission flag through the
// coordinator, so the decision itself is audited and durable.
Result<RecoveryReport> run_recovery(RiskCoordinator& coordinator, RiskStore& store,
                                    MarketFeed& feed, Clock& clock,
                                    const RecoveryOptions& options);

// Verifies that replaying a journal yields exactly the live state. Used by the
// crash tests as the restart-equivalence oracle (P-13).
Status assert_replay_equivalence(const RiskStore& live, const RiskStore& replayed);

}  // namespace risk
