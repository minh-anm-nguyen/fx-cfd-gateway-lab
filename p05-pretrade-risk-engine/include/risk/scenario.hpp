// Lab environment, the §20.2 fixtures, and the deterministic scenario runner.
//
// `LabEnv` wires a virtual clock, a mock feed, a store and a coordinator into
// one object so a test, the CLI and the scenario runner all drive the engine
// the same way. Everything is deterministic: the clock only moves when a step
// says so, and the feed only changes when a step publishes.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "risk/coordinator.hpp"
#include "risk/journal_store.hpp"
#include "risk/memory_store.hpp"
#include "risk/recovery.hpp"

namespace risk {

// §20.2
enum class Fixture : std::uint8_t {
  F0Empty,
  F1AtLimit,
  F2Partial,
  F3PendingCancel,
  F4PendingReplaceUp,
  F5PendingReplaceDown,
  F6UnknownNew,
  F7MultiAccount,
  F8StaleFeed,
  F9CrashPoint,
  F10BadExec,
  F11Breached,
};

std::string_view fixture_name(Fixture f) noexcept;
bool parse_fixture(std::string_view text, Fixture& out) noexcept;

struct LabOptions {
  // Empty means an in-memory store; a path means a durable journal, which is
  // what the crash and restart tests need.
  std::string journal_path;
  RiskPolicy policy = lab_policy();
  TenantId tenant_id = "T1";
  DomainId domain_id = "D1";
  FencingToken fencing_token = 1;
  std::string token_secret = "lab-secret";
  bool publish_default_market = true;
  bool open_admission = true;
  // Drives the engine from a SystemClock instead of the virtual one. Only the
  // benchmark wants this: a measured latency is meaningless on a clock that
  // only moves when a test says so. Tests leave it false so every TTL boundary
  // stays reproducible.
  bool real_clock = false;
};

class LabEnv {
 public:
  static Result<std::unique_ptr<LabEnv>> create(const LabOptions& options);
  ~LabEnv();

  LabEnv(const LabEnv&) = delete;
  LabEnv& operator=(const LabEnv&) = delete;

  VirtualClock& clock() { return *clock_; }
  MarketFeed& feed() { return *feed_; }
  FailpointRegistry& failpoints() { return *failpoints_; }
  RiskStore& store() { return *store_; }
  RiskCoordinator& coordinator() { return *coordinator_; }

  const VirtualClock& clock() const { return *clock_; }
  const MarketFeed& feed() const { return *feed_; }
  const RiskStore& store() const { return *store_; }
  const RiskCoordinator& coordinator() const { return *coordinator_; }
  const RiskPolicy& policy() const { return options_.policy; }
  const LabOptions& options() const { return options_; }

  const Principal& trader() const { return trader_; }
  const Principal& risk_admin() const { return risk_admin_; }
  const Principal& risk_admin2() const { return risk_admin2_; }
  const Principal& operator_principal() const { return operator_; }

  // Publishes a consistent, fresh quote/FX/equity set for every configured
  // instrument. Sequence numbers advance, so a second call is accepted.
  Status publish_default_market();
  Status publish_quote(const SymbolId& symbol, std::int64_t bid_ticks,
                       std::int64_t ask_ticks, std::int64_t ttl_ms = 0);
  Status publish_fx(const std::string& from, const std::string& to,
                    const Rational& bid, const Rational& ask,
                    std::int64_t ttl_ms = 0);
  Status publish_equity(const AccountId& account, std::int64_t equity_minor,
                        std::int64_t existing_margin_minor);

  // Convenience wrappers that fill in the usual identifiers.
  RiskResult new_order(const AccountId& account, const OrderChainId& chain,
                       const SymbolId& symbol, Side side, std::int64_t units,
                       std::int64_t limit_price_ticks,
                       const CommandId& command_id = {});
  RiskResult replace_order(const AccountId& account, const OrderChainId& chain,
                           std::int64_t new_units, std::int64_t new_price_ticks,
                           const CommandId& command_id = {});
  RiskResult cancel_order(const AccountId& account, const OrderChainId& chain,
                          const CommandId& command_id = {});
  RiskResult dispatch(const CommandId& command_id);

  RiskResult lp_accept(const AccountId& account, const OrderChainId& chain,
                       const EventId& event_id);
  RiskResult lp_fill(const AccountId& account, const OrderChainId& chain,
                     const EventId& event_id, std::int64_t units);
  RiskResult lp_event(const VerifiedOrderEvent& event);

  // Reopens the store from its journal and reruns recovery, modelling a
  // process restart. Only valid for a journal-backed environment.
  Status restart();

  std::optional<ReservationToken> token_for(const CommandId& command_id) const;
  std::optional<Reservation> reservation_for(const AccountId& account,
                                             const OrderChainId& chain) const;

  // Current risk-effective outstanding units for one account/symbol/side.
  std::int64_t effective_remaining(const AccountId& account, const SymbolId& symbol,
                                   Side side) const;
  std::int64_t position(const AccountId& account, const SymbolId& symbol) const;

  const RecoveryReport* last_recovery() const {
    return last_recovery_.has_value() ? &*last_recovery_ : nullptr;
  }

 private:
  LabEnv() = default;
  Status build_store();
  Status build_coordinator();

  LabOptions options_;
  std::unique_ptr<VirtualClock> clock_;
  std::unique_ptr<SystemClock> system_clock_;
  // Points at one of the two above. Everything the engine sees goes through
  // this, so the feed's ingest timestamps and the engine's TTL checks always
  // share one axis.
  Clock* engine_clock_ = nullptr;
  std::unique_ptr<MarketFeed> feed_;
  std::unique_ptr<FailpointRegistry> failpoints_;
  std::unique_ptr<RiskStore> store_;
  std::unique_ptr<RiskCoordinator> coordinator_;

  Principal trader_;
  Principal risk_admin_;
  Principal risk_admin2_;
  Principal operator_;

  std::map<CommandId, ReservationToken> tokens_;
  std::uint64_t command_counter_ = 0;
  std::uint64_t quote_sequence_ = 0;
  std::optional<RecoveryReport> last_recovery_;
};

// Applies one of the standard fixtures to a freshly created environment.
Status apply_fixture(LabEnv& env, Fixture fixture);

// ---------------------------------------------------------------------------
// Scenario DSL
// ---------------------------------------------------------------------------
//
// A line-oriented rendering of the DSL in §24.5. One directive per line:
//
//   scenario_id RISK-G11-REPLACE-FILL-CRASH
//   seed 92305
//   fixture F0_EMPTY
//   step reserve_new account=ACC-A chain=c1 symbol=EURUSD side=BUY qty=100000
//     price=110000 command_id=new-1 expect=ALLOW_COMMITTED   (same line in a file)
//   step lp_fill account=ACC-A chain=c1 event_id=e-1 qty=40000
//   step assert effective_buy_remaining=60000 account=ACC-A symbol=EURUSD
//
// A block sequence of inline maps was avoided deliberately: the config parser
// refuses YAML sequences, and one syntax for both files keeps the surface
// small.

struct ScenarioStepResult {
  int line = 0;
  std::string directive;
  bool ok = true;
  std::string detail;
};

struct ScenarioRun {
  std::string scenario_id;
  std::int64_t seed = 0;
  Fixture fixture = Fixture::F0Empty;
  // `store journal` in the file. A scenario with a `restart` step needs this,
  // and the runner provides a temporary journal when the caller did not name
  // one on the command line.
  bool durable = false;
  bool ok = true;
  std::string failure;
  std::vector<ScenarioStepResult> steps;

  std::string render() const;
};

// Runs a scenario against a freshly created environment. `journal_path`
// overrides the scenario's own store choice, which lets the crash harness
// point a scenario at a durable file.
Result<ScenarioRun> run_scenario_text(const std::string& text,
                                      const std::string& journal_path = {});
Result<ScenarioRun> run_scenario_file(const std::string& path,
                                      const std::string& journal_path = {});

}  // namespace risk
