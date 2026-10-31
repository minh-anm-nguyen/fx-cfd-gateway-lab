// The router: command admission, deterministic routing, outbound dispatch,
// inbound apply, reconciliation and the operator workflow.
//
// What this class is careful never to do is as important as what it does. It
// owns the idempotent command record, the routing decision, the outbox, the
// audit trail and the reconciliation cases (design doc 5.1). It does not own
// order status, the fill ledger, FIX sequence numbers or the LP book, and it
// has no code path that writes any of those.
//
// The three loops are explicit rather than threaded, because design doc 22.1
// wants step-based determinism:
//   submit()           -- a client command, admitted or refused, nothing sent
//   dispatch_pending() -- outbox to P01, under the session and config gates
//   pump_inbound()     -- P01 receipts to P03, commit then acknowledge
//   on_timer()         -- ack timeouts and reconciliation probes
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "deps/p01_session.hpp"
#include "deps/p05_risk.hpp"
#include "router/audit.hpp"
#include "router/config.hpp"
#include "router/metrics.hpp"
#include "router/normalizer.hpp"
#include "router/store.hpp"

namespace router {

// A client request. Prices and quantities arrive as text so that the strict
// canonicalisation in design doc 8.1 step 2 is actually exercised: "1.085001"
// has to be refused, not rounded, and that decision belongs here rather than in
// whatever client library produced the number.
struct CommandEnvelope {
    Principal principal;
    AccountId account;
    CommandKind kind = CommandKind::New;
    std::string client_command_id;

    std::string symbol;      // user alias, resolved against the profile
    Side side = Side::Buy;
    std::string qty_text;
    std::string price_text;
    OrdType ord_type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;

    std::optional<OrderChainId> chain_id;        // cancel, replace, status
    std::optional<StateVersion> expected_version;  // optimistic check for replace
};

struct AcceptedIntent {
    CommandId command_id;
    OrderChainId chain_id;
    ClientOrderId cl_ord_id;
    LpId route;
    DeliveryState delivery = DeliveryState::AcceptedDurable;
    BusinessOutcome business = BusinessOutcome::Unknown;
    // True when this call matched an existing command record rather than
    // creating one. The caller gets the same answer either way (FR-01).
    bool idempotent_replay = false;
};

// Overall posture. Design doc 16.2: a store or audit failure halts New and
// Replace while leaving the cancel path available where it is safe.
enum class RouterMode { Normal, Degraded, Halt };
std::string_view to_string(RouterMode m);

struct DispatchStats {
    std::size_t sent = 0;
    std::size_t queued = 0;
    std::size_t unknown = 0;
    std::size_t held = 0;
    std::size_t blocked_not_ready = 0;
};

struct InboundStats {
    std::size_t applied = 0;
    std::size_t deduplicated = 0;
    std::size_t quarantined = 0;
    std::size_t held = 0;
    std::size_t orphaned = 0;
    std::size_t invalid = 0;
};

class Router {
public:
    Router(Config& cfg, Store& store, p01::SessionEngine& session, p05::IRiskAdmission& risk,
           Metrics& metrics, FailpointRegistry& fp, const IClock& clock);

    // The recovery sequence from design doc 15.3. Must be called before any
    // command is accepted; it takes the owner lease, validates integrity and
    // configuration, rebuilds pending work and classifies everything unresolved.
    Result<bool, RoutingError> boot();

    // --- the four loops ----------------------------------------------------
    Result<AcceptedIntent, RoutingError> submit(const CommandEnvelope& cmd);
    DispatchStats dispatch_pending(std::size_t max = 1024);
    InboundStats pump_inbound(std::size_t max = 1024);
    void on_timer();

    // --- operator workflow (design doc 14.4, 17.1) -------------------------
    // Force-resolving a case needs the role, a reason, an evidence digest and,
    // for an irreversible action, two distinct approvers (OR-Q-08, OR-Q-09,
    // OR-M-19, OR-M-20).
    Result<bool, RoutingError> operator_resolve_case(const Principal& who, const ReconCaseId& id,
                                                     const std::string& reason,
                                                     const Hash& evidence,
                                                     const std::vector<std::string>& approvers);
    Result<bool, RoutingError> operator_resume(const Principal& who, const std::string& reason);
    // Route changes are an administrative action: they need the role and they
    // land in the audit trail with the actor and the reason (OR-Q-07).
    Result<bool, RoutingError> operator_set_route_active(const Principal& who,
                                                        const AccountId& account, bool active,
                                                        const std::string& reason);
    // Re-attempts correlation for quarantined orphans whose chain has since been
    // committed (OR-L-06). Nothing is applied without passing the full pipeline
    // again.
    std::size_t resolve_orphans();

    // --- read models -------------------------------------------------------
    RouterMode mode() const { return mode_; }
    const std::string& halt_reason() const { return halt_reason_; }
    std::optional<AcceptedIntent> command_status(const TenantId& t, const CommandId& c) const;
    std::size_t open_unknown_outcomes() const;
    std::size_t open_recon_cases() const;
    std::size_t original_sends() const { return original_sends_; }
    std::size_t status_requests_emitted() const { return status_requests_emitted_; }
    const std::vector<std::string>& alerts() const { return alerts_; }

    // The escalation bundle in design doc 14.4. Raw frames are only included
    // for a principal allowed to see them (OR-Q-15).
    std::string escalation_bundle(const Principal& who, const ReconCaseId& id) const;

    Store& store() { return store_; }
    Config& config() { return cfg_; }

private:
    // admission helpers
    Result<AcceptedIntent, RoutingError> admit_new(const CommandEnvelope& cmd,
                                                   const CommandId& command_id, const Hash& digest);
    Result<AcceptedIntent, RoutingError> admit_cancel(const CommandEnvelope& cmd,
                                                      const CommandId& command_id,
                                                      const Hash& digest);
    Result<AcceptedIntent, RoutingError> admit_replace(const CommandEnvelope& cmd,
                                                       const CommandId& command_id,
                                                       const Hash& digest);
    Result<bool, RoutingError> check_common_admission(const CommandEnvelope& cmd);
    Result<const Route*, RoutingError> resolve_route(const AccountId& account);
    Hash canonical_digest(const CommandEnvelope& cmd, const CommandId& command_id) const;
    bool rate_limit_ok(const AccountId& account);

    // inbound and bookkeeping helpers
    void record_reject(const CommandEnvelope& cmd, const CommandId& id, RoutingError e);
    void open_recon(Txn& txn, const OrderChainId& chain, const std::optional<CommandId>& cmd,
                    HoldReason cause, const Hash& evidence);
    // Compares an ExecType=I snapshot with the ledger and moves the matching
    // reconciliation cases. Never writes a fill: that is the whole point
    // (R-I09, R-I17).
    void apply_status_evidence(Txn& txn, const NormalizedReport& report,
                               const p03::ChainState& chain);
    void alert(std::string text);
    void set_mode(RouterMode m, std::string reason);

    Config& cfg_;
    Store& store_;
    p01::SessionEngine& session_;
    p05::IRiskAdmission& risk_;
    Metrics& metrics_;
    FailpointRegistry& fp_;
    const IClock& clock_;
    Normalizer normalizer_;

    RouterMode mode_ = RouterMode::Normal;
    std::string halt_reason_;
    std::vector<std::string> alerts_;
    std::size_t original_sends_ = 0;
    std::size_t status_requests_emitted_ = 0;
    std::size_t in_flight_probes_ = 0;
    std::map<std::string, std::pair<MonoMicros, std::size_t>> rate_buckets_;
    std::map<std::string, std::size_t> log_suppression_;
};

}  // namespace router
