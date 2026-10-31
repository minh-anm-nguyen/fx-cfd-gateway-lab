// RiskCoordinator: the single writer for one risk domain.
//
// Everything that can change risk state goes through this object, in the strict
// order of §12.1, and lands in exactly one store transaction. There is no API
// to adjust a reserved number directly, no `force_release`, and no way to get a
// dispatchable token without a confirmed commit (§18.1, RI-01, RI-22).
//
// Concurrency: one lock per domain serialises admissions, so the firm cap is
// evaluated against a single committed snapshot (§15.2). The store additionally
// enforces a revision CAS and a fencing token, so even a second coordinator
// process cannot double-spend a shared cap (RI-15, RI-25).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "risk/audit.hpp"
#include "risk/clock.hpp"
#include "risk/failpoint.hpp"
#include "risk/market.hpp"
#include "risk/metrics.hpp"
#include "risk/store.hpp"

namespace risk {

// ---------------------------------------------------------------------------
// Identity and authorisation (§18.1)
// ---------------------------------------------------------------------------

enum class Role : std::uint8_t { Trader, RiskAdmin, Operator, Auditor };

struct Principal {
  std::string principal_id;
  TenantId tenant_id;
  std::set<AccountId> accounts;
  std::set<Role> roles;

  bool has_role(Role r) const { return roles.count(r) != 0; }
  bool owns_account(const AccountId& a) const { return accounts.count(a) != 0; }
};

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

struct NewOrderCommand {
  TenantId tenant_id;
  AccountId account_id;
  CommandId command_id;
  OrderChainId order_chain_id;
  SymbolId symbol;
  Side side = Side::Buy;
  std::int64_t total_units = 0;
  std::int64_t limit_price_ticks = 0;
  OrderType order_type = OrderType::Limit;
  TimeInForce time_in_force = TimeInForce::Day;

  // Canonical serialisation, then its hash. Two requests with the same
  // command_id must produce the same hash or the second is a CONFLICT (RI-02).
  std::string canonical_bytes() const;
  std::string canonical_hash() const;
};

struct ReplaceCommand {
  TenantId tenant_id;
  AccountId account_id;
  CommandId command_id;
  OrderChainId order_chain_id;
  std::int64_t new_total_units = 0;
  std::int64_t new_limit_price_ticks = 0;

  std::string canonical_bytes() const;
  std::string canonical_hash() const;
};

struct CancelCommand {
  TenantId tenant_id;
  AccountId account_id;
  CommandId command_id;
  OrderChainId order_chain_id;

  std::string canonical_bytes() const;
  std::string canonical_hash() const;
};

// ---------------------------------------------------------------------------
// Decisions and tokens
// ---------------------------------------------------------------------------

// §12.2
enum class RiskDecision : std::uint8_t {
  AllowCommitted,
  DenyDefinitive,
  HoldRetryable,
  Conflict,
  Quarantined,
};

std::string_view risk_decision_name(RiskDecision d) noexcept;

struct ReservationToken {
  ReservationId reservation_id;
  TenantId tenant_id;
  AccountId account_id;
  CommandId command_id;
  std::string canonical_hash;
  PolicyVersion policy_epoch = 0;
  std::string policy_hash;
  Revision commit_revision = 0;
  FencingToken fencing_token = 0;
  // Dispatch eligibility only. Expiry removes the right to dispatch; it never
  // releases the reservation (§12.2).
  MonoNanos dispatch_deadline_mono = 0;
  std::string signature;

  std::string signing_payload() const;
};

struct RiskResult {
  RiskDecision decision = RiskDecision::DenyDefinitive;
  Reason reason = Reason::Ok;
  std::string detail;
  std::optional<ReservationToken> token;
  ReservationId reservation_id;
  Revision commit_revision = 0;

  bool allowed() const { return decision == RiskDecision::AllowCommitted; }
};

// ---------------------------------------------------------------------------
// Verified business events from P03/P04
// ---------------------------------------------------------------------------

enum class OrderEventKind : std::uint8_t {
  OrderAccepted,
  AppliedFill,
  NewRejected,
  PendingCancel,
  CancelAccepted,
  CancelRejected,
  OrderFilled,
  Expired,
  PendingReplace,
  ReplaceAccepted,
  ReplaceRejected,
  UnknownOutcome,
  StatusSnapshot,
  TradeCorrect,
  TradeBust,
};

std::string_view order_event_kind_name(OrderEventKind k) noexcept;
bool parse_order_event_kind(std::string_view text, OrderEventKind& out) noexcept;

struct VerifiedOrderEvent {
  TenantId tenant_id;
  AccountId account_id;
  OrderChainId order_chain_id;

  // Event identity comes from the verified LP ExecID inside an agreed
  // namespace, not from a FIX sequence number and not from a ClOrdID alone
  // (§13, RI-03).
  std::string source_namespace;
  EventId event_id;

  OrderEventKind kind = OrderEventKind::OrderAccepted;
  SymbolId symbol;
  Side side = Side::Buy;

  std::int64_t fill_units = 0;             // AppliedFill
  std::int64_t verified_cum_units = 0;     // cross-check against the ledger
  std::int64_t accepted_total_units = 0;   // ReplaceAccepted / OrderAccepted
  std::int64_t limit_price_ticks = 0;
  CommandId related_command_id;

  std::string canonical_bytes() const;
  std::string canonical_hash() const;
};

// §13.1 step 5: an audited resolution carrying the verified facts.
struct ReconcileEvidence {
  TenantId tenant_id;
  AccountId account_id;
  OrderChainId order_chain_id;
  EventId evidence_id;
  std::string source_namespace = "recon";

  ReservationState resolved_state = ReservationState::Working;
  std::int64_t verified_cum_units = 0;
  std::int64_t verified_accepted_total_units = 0;
  bool clear_proposed_branch = true;

  std::string note;

  std::string canonical_hash() const;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class RiskCoordinator {
 public:
  struct Options {
    TenantId tenant_id = "T1";
    DomainId domain_id = "D1";
    FencingToken fencing_token = 1;
    std::string component_version = "p05-lab";
    // HMAC key for reservation tokens. Must not be empty in a configuration
    // that requires an atomic outbox.
    std::string token_secret = "lab-secret";
    std::int64_t dispatch_ttl_ms = 2000;
    // Bounded in-flight admissions (§17). Exceeding it yields BUSY, never a
    // partial commit.
    std::int64_t admission_concurrency_cap = 0;  // 0 == use the policy value
  };

  RiskCoordinator(RiskStore& store, MarketFeed& feed, Clock& clock,
                  FailpointRegistry& failpoints, Options options);

  // --- admission ---------------------------------------------------------
  RiskResult reserve_new(const NewOrderCommand& command, const Principal& who);
  RiskResult reserve_replace(const ReplaceCommand& command, const Principal& who);
  // A cancel never reserves budget and never releases one; it records the
  // pending-cancel marker and lets the command through on the priority lane
  // (§13, RI-04).
  RiskResult request_cancel(const CancelCommand& command, const Principal& who);

  // --- dispatch handshake ------------------------------------------------
  // P04 calls this with the token it was handed. Verifying the signature,
  // policy epoch, revision and fencing token is what stops a caller from
  // asserting `approved = true` (RI-22).
  Status verify_token(const ReservationToken& token) const;
  RiskResult mark_dispatched(const ReservationToken& token);

  // --- verified business events ------------------------------------------
  RiskResult apply_verified_order_event(const VerifiedOrderEvent& event);
  RiskResult reconcile_with_evidence(const ReconcileEvidence& evidence,
                                     const Principal& actor,
                                     const Principal& approver);

  // --- operations --------------------------------------------------------
  RiskResult set_mode(DomainMode mode, const Principal& who, std::string note);
  RiskResult install_policy(const RiskPolicy& policy, const Principal& actor,
                            const Principal& approver, std::string note);
  RiskResult open_admission(bool open, std::string note);
  RiskResult clear_account_quarantine(const AccountId& account,
                                      const Principal& actor,
                                      const Principal& approver,
                                      std::string note);

  // --- read-only introspection -------------------------------------------
  // Recomputes the metrics for the current committed state. Used by the CLI,
  // by the breach detector and by tests as a cross-check of the stored
  // projection.
  Result<DomainMetrics> current_metrics() const;
  // Rebuilds the projection from the reservation set and compares it with the
  // stored one. A mismatch is a P0 failure: the materialised view drifted from
  // the ledger (RI-23 cross-check).
  Status cross_check_projection() const;

  Metrics& metrics() { return metrics_; }
  const Metrics& metrics() const { return metrics_; }

  FencingToken fencing_token() const { return options_.fencing_token; }
  void set_fencing_token(FencingToken token) { options_.fencing_token = token; }

  const Options& options() const { return options_; }

  // Derives the pending (B/S) side of the projection purely from the
  // reservation set, keeping the stored positions. This is the from-scratch
  // definition, used by `cross_check_projection` and by recovery; the commit
  // path uses the delta form below, which is O(1) rather than O(reservations).
  static DomainExposure derive_exposure(const DomainExposure& positions_source,
                                        const std::vector<Reservation>& reservations);

  // Applies one reservation change to a copy of the committed projection.
  //
  // `before` is null for a chain that did not exist yet. Both sides refer to
  // the same account, symbol and side -- a reservation never moves between
  // them (RI-14) -- so the update touches exactly one row. The result must
  // equal `derive_exposure` over the whole ledger, which
  // `cross_check_projection` verifies and the property suite asserts after
  // every step.
  static DomainExposure apply_reservation_delta(const DomainExposure& committed,
                                                const Reservation* before,
                                                const Reservation* after,
                                                std::int64_t position_delta);

 private:
  struct AdmissionContext {
    MarketView view;
    RiskPolicy policy;
    RiskDomainState domain;
    DomainExposure exposure;
    std::vector<Reservation> reservations;
    MonoNanos started_mono = 0;
    ValuationTable valuations;
  };

  // Steps 1-3 of §12.1, shared by New and Replace.
  Status check_identity_and_authz(const TenantId& tenant, const AccountId& account,
                                  const CommandId& command,
                                  const OrderChainId& chain, const Principal& who);
  Status check_gates(const RiskDomainState& domain, const RiskPolicy& policy,
                     const AccountId& account, bool risk_increasing) const;

  // Builds a valuation for every symbol that carries exposure plus `extra`.
  // The reference price per symbol comes from the store's live-price index
  // rather than from a scan of every reservation, so the cost is proportional
  // to the number of symbols in play, not to the number of open orders.
  Result<ValuationTable> build_valuations(const MarketView& view,
                                          const RiskPolicy& policy,
                                          const DomainExposure& exposure,
                                          const SymbolId& extra,
                                          std::int64_t extra_limit_price_ticks,
                                          MonoNanos now_mono) const;

  Result<MarginBasis> margin_basis(const MarketView& view, const RiskPolicy& policy,
                                   const AccountId& account,
                                   MonoNanos now_mono) const;

  // Returns the already-recorded decision for an idempotent retry, or nullopt.
  std::optional<RiskResult> replay_decision(const TenantId& tenant,
                                            const AccountId& account,
                                            const CommandId& command,
                                            const std::string& canonical_hash) const;

  ReservationToken mint_token(const Reservation& reservation,
                              const std::string& canonical_hash,
                              const std::string& policy_hash,
                              Revision commit_revision) const;

  RiskResult commit_or_map(CommitRequest& request, const std::string& decision_label,
                           const ReservationId& reservation_id,
                           const NewOrderCommand* new_cmd,
                           const std::string& canonical_hash,
                           const std::string& policy_hash,
                           const Reservation* reservation);

  // Records a definitive denial durably so a retry of the same command gets
  // the same answer instead of being re-evaluated against newer data (RI-02).
  RiskResult record_denial(const TenantId& tenant, const AccountId& account,
                           const CommandId& command, const SymbolId& symbol,
                           const std::string& canonical_hash, Reason reason,
                           std::string detail, const RiskPolicy& policy,
                           const RiskDomainState& domain,
                           const std::string& event_type);

  RiskResult quarantine_account(const TenantId& tenant, const AccountId& account,
                                const OrderChainId& chain, Reason reason,
                                std::string detail, const VerifiedOrderEvent* event,
                                const Reservation* reservation);

  SnapshotRef snapshot_ref(const RiskPolicy& policy, const MarketView& view,
                           const Valuation* valuation, Revision ledger) const;

  std::int64_t unknown_backlog() const;

  RiskStore& store_;
  MarketFeed& feed_;
  Clock& clock_;
  FailpointRegistry& failpoints_;
  Options options_;
  Metrics metrics_;
  AuditBuilder audit_;

  // Serialises the whole read-evaluate-commit sequence for this domain.
  mutable std::mutex domain_mu_;
  std::atomic<std::int64_t> admissions_in_flight_{0};
  std::atomic<std::uint64_t> reservation_counter_{0};

  // Token-bucket rate limiter for admissions (§7.1 "Rate" scope).
  mutable std::mutex rate_mu_;
  double rate_tokens_ = 0.0;
  MonoNanos rate_last_refill_ = 0;
};

}  // namespace risk
