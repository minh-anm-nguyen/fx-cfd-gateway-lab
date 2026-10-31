// Durable store contract: one atomic unit of work covering the risk ledger,
// command dedup, event dedup, projection, audit and the router outbox.
//
// §6 / RI-23 / §23.2: there is exactly one commit boundary. The risk hold and
// the router intent are written by the same `commit()` call or neither is
// written. The interface deliberately offers no way to write them separately,
// so the dual-write counterexample in §23.2 is not expressible against this
// API.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "risk/clock.hpp"
#include "risk/exposure.hpp"
#include "risk/failpoint.hpp"
#include "risk/policy.hpp"
#include "risk/reservation.hpp"
#include "risk/types.hpp"

namespace risk {

// §16.1 risk_domains
struct RiskDomainState {
  TenantId tenant_id;
  DomainId domain_id;
  Revision revision = 0;
  FencingToken fencing_token = 0;
  PolicyVersion policy_version = 0;
  DomainMode mode = DomainMode::Normal;

  // Set when evidence is irreconcilable; blocks risk increase until an audited
  // reconciliation clears it (RI-27).
  bool quarantined = false;
  // Startup gate: admission stays closed until recovery has verified the
  // journal and enumerated unknown outcomes (RI-32).
  bool admission_open = false;

  std::string digest() const;
};

// §16.1 risk_commands. The durable record of one decision, keyed by
// (tenant, account, command_id) so a retry returns the original answer (RI-02).
struct CommandRecord {
  TenantId tenant_id;
  AccountId account_id;
  CommandId command_id;
  std::string canonical_hash;
  std::string decision;  // ALLOW_COMMITTED / DENY_DEFINITIVE / ...
  Reason reason = Reason::Ok;
  ReservationId reservation_id;
  Revision commit_revision = 0;
  PolicyVersion policy_version = 0;
};

// §16.1 risk_applied_events
struct AppliedEventRecord {
  TenantId tenant_id;
  std::string source_namespace;
  EventId event_id;
  std::string canonical_hash;
  Revision applied_revision = 0;
};

// §16.1 risk_audit_events
struct AuditEvent {
  TenantId tenant_id;
  DomainId domain_id;
  Revision commit_revision = 0;
  int event_ordinal = 0;
  std::string event_type;

  CommandId command_id;
  ReservationId reservation_id;
  AccountId account_id;
  SymbolId symbol;
  EventId source_event_id;

  PolicyVersion policy_version = 0;
  SnapshotRef snapshots;
  std::string payload_hash;
  std::string old_state_hash;
  std::string new_state_hash;
  Reason reason = Reason::Ok;
  std::string actor;
  std::string note;

  UtcNanos commit_utc = 0;
  MonoNanos commit_mono = 0;
  std::string component_version;

  std::string digest() const;
};

// P04's durable order intent. It is created inside the same transaction as the
// hold, and the dispatcher may only consume it afterwards (RI-01).
struct OutboxIntent {
  std::string intent_id;  // stable: derived from (tenant, account, command_id)
  TenantId tenant_id;
  AccountId account_id;
  OrderChainId order_chain_id;
  CommandId command_id;
  ReservationId reservation_id;

  std::string kind;  // NEW / REPLACE
  SymbolId symbol;
  Side side = Side::Buy;
  std::int64_t total_units = 0;
  std::int64_t limit_price_ticks = 0;

  Revision commit_revision = 0;
  std::string canonical_hash;

  // Dispatch bookkeeping. `claimed` marks that the dispatcher took it;
  // `dispatched` that the session durably assigned it a sequence. Neither is
  // evidence that the LP received it (§23.2).
  bool claimed = false;
  bool dispatched = false;
};

enum class CommitOutcome : std::uint8_t {
  // Durably committed and acknowledged.
  Committed,
  // Nothing was written; safe to retry the whole transaction.
  RolledBack,
  // Possibly or definitely durable, but the acknowledgement was lost. The
  // caller must NOT treat this as a rollback: recover by reading back the
  // command id (RI-24, CW-12).
  UnknownButDurable,
  // The writer no longer owns the domain; the write was refused at the storage
  // boundary (RI-25).
  Fenced,
};

std::string_view commit_outcome_name(CommitOutcome o) noexcept;

// Everything one transaction may change. Absent optionals mean "leave alone".
struct TransactionData {
  std::vector<CommandRecord> commands;
  std::vector<Reservation> reservations;
  std::vector<AppliedEventRecord> applied_events;
  std::vector<AuditEvent> audit;
  std::vector<OutboxIntent> outbox_inserts;
  // Intent id -> (claimed, dispatched) updates.
  std::vector<std::pair<std::string, std::pair<bool, bool>>> outbox_updates;

  // Written once, by the bootstrap record, so a journal self-identifies and a
  // later open can refuse to adopt a file belonging to another domain.
  std::optional<TenantId> tenant_id;
  std::optional<DomainId> domain_id;

  std::optional<DomainExposure> exposure;
  std::optional<RiskPolicy> policy;
  std::optional<DomainMode> mode;
  std::optional<bool> quarantined;
  std::optional<bool> admission_open;
  // Chain index entries: (tenant, account, chain) -> reservation id.
  std::vector<std::pair<std::string, ReservationId>> chain_index;
  // Per-account quarantine flips. Account granularity matters: contradictory
  // evidence on one account must not stop the whole firm from reconciling the
  // others (§4, "QUARANTINE account/order").
  std::vector<std::pair<AccountId, bool>> account_quarantine;

  bool empty() const;
};

struct CommitRequest {
  FencingToken fencing_token = 0;
  // CAS guard: the commit is refused unless the domain is still at this
  // revision (RI-15: no read-then-write race on shared caps).
  Revision expected_revision = 0;
  TransactionData data;
};

std::string chain_key(const TenantId& tenant, const AccountId& account,
                      const OrderChainId& chain);
std::string command_key(const TenantId& tenant, const AccountId& account,
                        const CommandId& command);
std::string event_key(const TenantId& tenant, const std::string& ns,
                      const EventId& event);

class RiskStore {
 public:
  virtual ~RiskStore() = default;

  // --- committed reads ---------------------------------------------------
  virtual RiskDomainState domain_state() const = 0;
  virtual DomainExposure exposure() const = 0;
  virtual RiskPolicy policy() const = 0;

  virtual std::optional<CommandRecord> find_command(const TenantId& tenant,
                                                    const AccountId& account,
                                                    const CommandId& command) const = 0;
  virtual std::optional<Reservation> find_reservation(const ReservationId& id) const = 0;
  virtual std::optional<ReservationId> find_chain(const TenantId& tenant,
                                                  const AccountId& account,
                                                  const OrderChainId& chain) const = 0;
  virtual std::optional<AppliedEventRecord> find_applied_event(
      const TenantId& tenant, const std::string& ns, const EventId& event) const = 0;

  virtual std::vector<Reservation> reservations() const = 0;
  virtual std::vector<AuditEvent> audit() const = 0;
  virtual std::vector<OutboxIntent> outbox() const = 0;
  virtual bool account_quarantined(const AccountId& account) const = 0;
  virtual std::vector<AccountId> quarantined_accounts() const = 0;

  // Highest raw limit price across the live reservations on `symbol`, or 0
  // when there are none. Revaluation uses raw prices so that re-running it is
  // idempotent (see Reservation::effective_limit_price_ticks).
  virtual std::int64_t max_live_limit_price(const SymbolId& symbol) const = 0;
  // Chains whose outcome is unresolved (UNKNOWN or QUARANTINE), for the
  // reconciliation backlog cap.
  virtual std::int64_t unresolved_reservation_count() const = 0;

  // --- the single commit boundary ----------------------------------------
  virtual CommitOutcome commit(const CommitRequest& request) = 0;

  // Installs a new fencing token, simulating a leadership change. A commit
  // carrying an older token is refused afterwards.
  virtual void install_fencing_token(FencingToken token) = 0;

  // Canonical hash of all committed state. Two stores holding equivalent state
  // must produce the same value, which is what makes the restart-equivalence
  // assertion meaningful (P-13).
  virtual std::string state_hash() const;

  virtual bool durable() const = 0;
  virtual std::string describe() const = 0;
};

}  // namespace risk
