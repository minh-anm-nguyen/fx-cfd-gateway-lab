// Lifecycle half of the coordinator: replace, cancel, dispatch handshake,
// verified business events, reconciliation and operator actions.
//
// Split from coordinator.cpp purely for file size; the locking discipline is
// the same -- every mutation takes `domain_mu_` and lands in exactly one store
// transaction.
#include <algorithm>
#include <cstdlib>
#include <limits>

#include "risk/checked.hpp"
#include "risk/coordinator.hpp"
#include "risk/hash.hpp"

namespace risk {
namespace {

RiskResult make_result(RiskDecision decision, Reason reason, std::string detail) {
  RiskResult r;
  r.decision = decision;
  r.reason = reason;
  r.detail = std::move(detail);
  return r;
}

RiskResult refuse(Reason reason, std::string detail) {
  const RiskDecision decision = reason_is_retryable(reason)
                                    ? RiskDecision::HoldRetryable
                                    : RiskDecision::DenyDefinitive;
  return make_result(decision, reason, std::move(detail));
}

RiskResult refuse(const Status& s) { return refuse(s.reason(), s.detail()); }

Reason reach_control_failpoint(FailpointRegistry& failpoints, Failpoint fp) {
  FailpointRegistry::Hit hit;
  if (!failpoints.reach(fp, hit)) return Reason::Ok;
  switch (hit.action) {
    case FailAction::Abort:
      std::_Exit(99);
    case FailAction::Hook:
      if (hit.hook) hit.hook();
      return Reason::Ok;
    case FailAction::CommitFail:
      return Reason::PersistenceCommitFailed;
    case FailAction::CommitAckLost:
      return Reason::PersistenceCommitUnknown;
    case FailAction::FenceReject:
      return Reason::FencingTokenStale;
  }
  return Reason::Ok;
}

std::string decision_label(RiskDecision d) {
  return std::string(risk_decision_name(d));
}

}  // namespace

// ---------------------------------------------------------------------------
// reserve_replace
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::reserve_replace(const ReplaceCommand& command,
                                            const Principal& who) {
  const MonoNanos t_start = clock_.mono_now();
  metrics_.incr_labeled(metric::kAdmissionTotal, "op=replace");

  if (const Reason r = reach_control_failpoint(failpoints_, Failpoint::BeforeAuthz);
      r != Reason::Ok) {
    return refuse(r, "failpoint before authz");
  }
  if (const Status s = check_identity_and_authz(command.tenant_id, command.account_id,
                                                command.command_id,
                                                command.order_chain_id, who);
      !s) {
    return refuse(s);
  }

  const std::string canonical_hash = command.canonical_hash();

  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterAuthzBeforeIdempotency);
      r != Reason::Ok) {
    return refuse(r, "failpoint before idempotency");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);

  if (std::optional<RiskResult> replayed = replay_decision(
          command.tenant_id, command.account_id, command.command_id, canonical_hash);
      replayed.has_value()) {
    return *replayed;
  }

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  // §7.3: NO_NEW blocks `New` *and* `G`. A replace is treated as
  // risk-increasing for gating purposes even when it reduces quantity, because
  // deciding otherwise would require trusting the proposal before it is
  // accepted.
  if (const Status s = check_gates(domain, policy, command.account_id, true); !s) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         {}, canonical_hash, s.reason(), s.detail(), policy, domain,
                         "ReplaceDenied");
  }

  std::optional<ReservationId> chain_ref = store_.find_chain(
      command.tenant_id, command.account_id, command.order_chain_id);
  if (!chain_ref.has_value()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         {}, canonical_hash, Reason::ReplaceNoAcceptedVersion,
                         "no reservation for chain " + command.order_chain_id, policy,
                         domain, "ReplaceDenied");
  }
  std::optional<Reservation> existing = store_.find_reservation(*chain_ref);
  if (!existing.has_value()) {
    return refuse(Reason::ReservationNotFound, *chain_ref);
  }
  const Reservation& current = *existing;

  if (!current.holds_budget()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::ReservationTerminal,
                         "chain is terminal", policy, domain, "ReplaceDenied");
  }
  // RI-26: an unresolved outcome is not a base to modify from. Reconciliation
  // first; the existing worst-case hold stays untouched meanwhile.
  if (current.state == ReservationState::Unknown ||
      current.state == ReservationState::Quarantine) {
    return refuse(Reason::ReconciliationRequired,
                  "chain outcome unresolved: " +
                      std::string(reservation_state_name(current.state)));
  }
  // P-11: at most one in-flight modification per chain in v1.
  if (current.has_pending_modify()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::PendingModifyBusy,
                         "a replace is already pending on this chain", policy, domain,
                         "ReplaceDenied");
  }
  // §14.2: v1 never has a cancel and a replace in flight together.
  if (current.pending_cancel) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::PendingModifyBusy,
                         "a cancel is already pending on this chain", policy, domain,
                         "ReplaceDenied");
  }

  const InstrumentSpec* spec = policy.find_instrument(current.symbol);
  if (spec == nullptr) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::UnknownInstrument,
                         current.symbol, policy, domain, "ReplaceDenied");
  }

  if (command.new_total_units <= 0 ||
      command.new_total_units < spec->min_qty_base_units) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::QtyBelowMin,
                         std::to_string(command.new_total_units), policy, domain,
                         "ReplaceDenied");
  }
  if (command.new_total_units > spec->max_qty_base_units) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::QtyAboveMax,
                         std::to_string(command.new_total_units), policy, domain,
                         "ReplaceDenied");
  }
  if (command.new_total_units % spec->quantity_step_base_units != 0) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::QtyStepViolation,
                         std::to_string(command.new_total_units), policy, domain,
                         "ReplaceDenied");
  }
  if (command.new_limit_price_ticks <= 0) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::PriceNonPositive,
                         "limit price must be > 0", policy, domain, "ReplaceDenied");
  }
  // §14.2 / PB-17: a proposal below the verified cumulative fill is refused.
  // It is not reinterpreted as a cancel, and `Q_new - C` is never allowed to go
  // negative and look like a release.
  if (command.new_total_units < current.cum_units) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::ReplaceBelowCumulative,
                         "proposed total " + std::to_string(command.new_total_units) +
                             " < verified cumulative " +
                             std::to_string(current.cum_units),
                         policy, domain, "ReplaceDenied");
  }

  const MarketView view = feed_.snapshot();
  const DomainExposure committed_exposure = store_.exposure();

  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterSnapshotBeforeMath);
      r != Reason::Ok) {
    return refuse(r, "failpoint before exposure maths");
  }

  const MonoNanos t_math = clock_.mono_now();

  // Collar check against this command's own proposed price (SS9.2).
  {
    Result<Quote> quote = view.quote(current.symbol, t_math, spec->quote_ttl_ms);
    if (!quote.ok()) {
      return record_denial(command.tenant_id, command.account_id, command.command_id,
                           current.symbol, canonical_hash, quote.reason(),
                           quote.detail(), policy, domain, "ReplaceDenied");
    }
    if (const Status collar = check_price_collar(command.new_limit_price_ticks,
                                                 quote.value(),
                                                 spec->max_price_deviation_bps);
        !collar) {
      return record_denial(command.tenant_id, command.account_id, command.command_id,
                           current.symbol, canonical_hash, collar.reason(),
                           collar.detail(), policy, domain, "ReplaceDenied");
    }
  }

  InstrumentSpec revalue_spec = *spec;
  revalue_spec.max_price_deviation_bps = 0;
  Result<Valuation> proposed_valuation = build_valuation(
      view, policy, revalue_spec, command.new_limit_price_ticks, t_math);
  if (!proposed_valuation.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, proposed_valuation.reason(),
                         proposed_valuation.detail(), policy, domain, "ReplaceDenied");
  }

  // RI-06: reserve the maximum of the accepted and proposed branches, in both
  // quantity and valuation, before the G goes out.
  Reservation candidate = current;
  candidate.proposed_total_units = command.new_total_units;
  candidate.proposed_limit_price_ticks = command.new_limit_price_ticks;
  candidate.pending_modify_command_id = command.command_id;
  candidate.proposed_valuation = proposed_valuation.value();

  const DomainExposure candidate_exposure =
      apply_reservation_delta(committed_exposure, &current, &candidate, 0);
  Result<ValuationTable> valuations =
      build_valuations(view, policy, candidate_exposure, current.symbol,
                       candidate.effective_limit_price_ticks(), t_math);
  if (!valuations.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, valuations.reason(),
                         valuations.detail(), policy, domain, "ReplaceDenied");
  }

  Result<DomainMetrics> metrics =
      evaluate_exposure(candidate_exposure, policy, valuations.value());
  if (!metrics.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, metrics.reason(),
                         metrics.detail(), policy, domain, "ReplaceDenied");
  }

  Result<std::int64_t> order_notional =
      notional_minor(candidate.risk_effective_remaining(), *spec,
                     candidate.effective_valuation(),
                     policy.account_currency.minor_scale);
  if (!order_notional.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, order_notional.reason(),
                         order_notional.detail(), policy, domain, "ReplaceDenied");
  }

  Result<MarginBasis> basis = margin_basis(view, policy, command.account_id, t_math);
  if (!basis.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, basis.reason(),
                         basis.detail(), policy, domain, "ReplaceDenied");
  }

  LimitCheckInput check;
  check.account_id = command.account_id;
  check.symbol = current.symbol;
  check.candidate_order_notional_minor = order_notional.value();
  check.candidate_chain_units =
      std::max(current.accepted_total_units, command.new_total_units);
  check.margin_basis = basis.value();

  if (const Status s = check_limits(metrics.value(), policy, check); !s) {
    metrics_.incr_labeled(metric::kLimitBreach,
                          std::string("reason=") +
                              std::string(reason_name(s.reason())));
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, s.reason(), s.detail(),
                         policy, domain, "ReplaceDenied");
  }

  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterMathBeforeDomainLock);
      r != Reason::Ok) {
    return refuse(r, "failpoint before domain lock");
  }

  // Same commit-time freshness re-assertion as a New.
  const MonoNanos t_commit = clock_.mono_now();
  Result<Valuation> recheck = build_valuation(
      view, policy, revalue_spec, command.new_limit_price_ticks, t_commit);
  if (!recheck.ok() ||
      recheck.value().digest() != proposed_valuation.value().digest()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         current.symbol, canonical_hash, Reason::QuoteStale,
                         "valuation changed between evaluation and commit", policy,
                         domain, "ReplaceDenied");
  }

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;

  CommandRecord record;
  record.tenant_id = command.tenant_id;
  record.account_id = command.account_id;
  record.command_id = command.command_id;
  record.canonical_hash = canonical_hash;
  record.decision = decision_label(RiskDecision::AllowCommitted);
  record.reservation_id = candidate.reservation_id;
  record.policy_version = policy.version;
  request.data.commands.push_back(record);
  request.data.reservations.push_back(candidate);
  request.data.exposure = candidate_exposure;

  AuditEvent audit = audit_.begin("ReplaceReserved", {});
  audit.command_id = command.command_id;
  audit.account_id = command.account_id;
  audit.symbol = current.symbol;
  audit.reservation_id = candidate.reservation_id;
  audit.policy_version = policy.version;
  audit.snapshots =
      snapshot_ref(policy, view, &proposed_valuation.value(), domain.revision);
  audit.payload_hash = canonical_hash;
  audit.actor = who.principal_id;
  audit.note = "accepted_remaining=" + std::to_string(current.accepted_remaining()) +
               " proposed_remaining=" + std::to_string(candidate.proposed_remaining()) +
               " effective=" + std::to_string(candidate.risk_effective_remaining());
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = t_commit;
  request.data.audit.push_back(audit);

  OutboxIntent intent;
  intent.intent_id = "OBX-" + canonical_hash.substr(0, 16);
  intent.tenant_id = command.tenant_id;
  intent.account_id = command.account_id;
  intent.order_chain_id = command.order_chain_id;
  intent.command_id = command.command_id;
  intent.reservation_id = candidate.reservation_id;
  intent.kind = "REPLACE";
  intent.symbol = current.symbol;
  intent.side = current.side;
  intent.total_units = command.new_total_units;
  intent.limit_price_ticks = command.new_limit_price_ticks;
  intent.canonical_hash = canonical_hash;
  request.data.outbox_inserts.push_back(intent);

  RiskResult result = commit_or_map(request, record.decision, candidate.reservation_id,
                                    nullptr, canonical_hash, policy.hash(),
                                    &candidate);
  metrics_.admission_commit_seconds().observe_micros(
      (clock_.mono_now() - t_start) / 1000);
  return result;
}

// ---------------------------------------------------------------------------
// request_cancel
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::request_cancel(const CancelCommand& command,
                                           const Principal& who) {
  metrics_.incr_labeled(metric::kAdmissionTotal, "op=cancel");

  if (const Status s = check_identity_and_authz(command.tenant_id, command.account_id,
                                                command.command_id,
                                                command.order_chain_id, who);
      !s) {
    return refuse(s);
  }

  const std::string canonical_hash = command.canonical_hash();
  std::lock_guard<std::mutex> domain_lock(domain_mu_);

  if (std::optional<RiskResult> replayed = replay_decision(
          command.tenant_id, command.account_id, command.command_id, canonical_hash);
      replayed.has_value()) {
    return *replayed;
  }

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  // A cancel reduces risk, so it is admitted in every mode except RECON_ONLY.
  if (const Status s = check_gates(domain, policy, command.account_id, false); !s) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         {}, canonical_hash, s.reason(), s.detail(), policy, domain,
                         "CancelDenied");
  }

  std::optional<ReservationId> chain_ref = store_.find_chain(
      command.tenant_id, command.account_id, command.order_chain_id);
  if (!chain_ref.has_value()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         {}, canonical_hash, Reason::UnknownChain,
                         command.order_chain_id, policy, domain, "CancelDenied");
  }
  std::optional<Reservation> existing = store_.find_reservation(*chain_ref);
  if (!existing.has_value()) {
    return refuse(Reason::ReservationNotFound, *chain_ref);
  }
  if (!existing->holds_budget()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         existing->symbol, canonical_hash, Reason::ReservationTerminal,
                         "chain is terminal", policy, domain, "CancelDenied");
  }

  Reservation candidate = *existing;
  candidate.pending_cancel = true;
  candidate.pending_cancel_command_id = command.command_id;

  // RI-04: the hold is unchanged, and the delta form makes that structural --
  // `pending_cancel` does not enter risk_effective_remaining at all, so the
  // computed delta is exactly zero.
  const DomainExposure unchanged =
      apply_reservation_delta(store_.exposure(), &*existing, &candidate, 0);

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;

  CommandRecord record;
  record.tenant_id = command.tenant_id;
  record.account_id = command.account_id;
  record.command_id = command.command_id;
  record.canonical_hash = canonical_hash;
  record.decision = decision_label(RiskDecision::AllowCommitted);
  record.reservation_id = candidate.reservation_id;
  record.policy_version = policy.version;
  request.data.commands.push_back(record);
  request.data.reservations.push_back(candidate);
  request.data.exposure = unchanged;

  AuditEvent audit = audit_.begin("CancelRequested", {});
  audit.command_id = command.command_id;
  audit.account_id = command.account_id;
  audit.symbol = candidate.symbol;
  audit.reservation_id = candidate.reservation_id;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.payload_hash = canonical_hash;
  audit.actor = who.principal_id;
  audit.note = "hold retained: " +
               std::to_string(candidate.risk_effective_remaining()) + " units";
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  OutboxIntent intent;
  intent.intent_id = "OBX-" + canonical_hash.substr(0, 16);
  intent.tenant_id = command.tenant_id;
  intent.account_id = command.account_id;
  intent.order_chain_id = command.order_chain_id;
  intent.command_id = command.command_id;
  intent.reservation_id = candidate.reservation_id;
  intent.kind = "CANCEL";
  intent.symbol = candidate.symbol;
  intent.side = candidate.side;
  intent.total_units = candidate.accepted_remaining();
  intent.limit_price_ticks = 0;
  intent.canonical_hash = canonical_hash;
  request.data.outbox_inserts.push_back(intent);

  return commit_or_map(request, record.decision, candidate.reservation_id, nullptr,
                       canonical_hash, policy.hash(), &candidate);
}

// ---------------------------------------------------------------------------
// mark_dispatched
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::mark_dispatched(const ReservationToken& token) {
  if (const Status s = verify_token(token); !s) return refuse(s);

  // CW-13: after P05 said ALLOW, before P04 claimed the outbox row.
  if (const Reason r =
          reach_control_failpoint(failpoints_, Failpoint::AfterAllowBeforeClaim);
      r != Reason::Ok) {
    return refuse(r, "failpoint before outbox claim");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);
  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  std::optional<Reservation> existing = store_.find_reservation(token.reservation_id);
  if (!existing.has_value()) {
    return refuse(Reason::ReservationNotFound, token.reservation_id);
  }

  // Expiry removes the right to dispatch; it never releases the hold (§12.2).
  if (token.dispatch_deadline_mono > 0 &&
      clock_.mono_now() > token.dispatch_deadline_mono) {
    return refuse(Reason::ReconciliationRequired,
                  "dispatch window expired; hold retained, verify command status");
  }

  const std::vector<OutboxIntent> outbox = store_.outbox();
  const OutboxIntent* target = nullptr;
  for (const OutboxIntent& o : outbox) {
    if (o.command_id == token.command_id && o.account_id == token.account_id) {
      target = &o;
      break;
    }
  }
  if (target == nullptr) {
    return refuse(Reason::InternalInvariant,
                  "no outbox intent for command " + token.command_id);
  }
  if (target->dispatched) {
    // Idempotent: a second claim of the same intent must not produce a second
    // original message (CW-14).
    RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::DuplicateCommand,
                               "intent already dispatched");
    r.reservation_id = token.reservation_id;
    r.commit_revision = domain.revision;
    return r;
  }

  Reservation candidate = *existing;
  if (candidate.state == ReservationState::Committed) {
    if (const Status s = validate_transition(ReservationState::Committed,
                                             ReservationState::InFlight);
        !s) {
      return refuse(s);
    }
    candidate.state = ReservationState::InFlight;
  }

  // CW-14/CW-15: between the claim and the durable sequence assignment.
  if (const Reason r =
          reach_control_failpoint(failpoints_, Failpoint::AfterClaimBeforeSequence);
      r != Reason::Ok) {
    return refuse(r, "failpoint before sequence assignment");
  }

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.reservations.push_back(candidate);
  request.data.outbox_updates.emplace_back(target->intent_id,
                                           std::make_pair(true, true));
  request.data.exposure =
      apply_reservation_delta(store_.exposure(), &*existing, &candidate, 0);

  AuditEvent audit = audit_.begin("IntentDispatched", {});
  audit.command_id = token.command_id;
  audit.account_id = token.account_id;
  audit.symbol = candidate.symbol;
  audit.reservation_id = candidate.reservation_id;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.payload_hash = token.canonical_hash;
  audit.note = "intent " + target->intent_id;
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(outcome == CommitOutcome::Fenced ? Reason::FencingTokenStale
                                                   : Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }

  // CW-16: the socket write happened but no LP acknowledgement arrived. The
  // hold stays and the outcome is unknown; nothing is auto-released (RI-26).
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterSocketWriteBeforeAck);
      r != Reason::Ok) {
    return refuse(r, "failpoint after socket write");
  }

  RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok, "dispatched");
  r.reservation_id = candidate.reservation_id;
  r.commit_revision = store_.domain_state().revision;
  return r;
}

// ---------------------------------------------------------------------------
// quarantine_account
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::quarantine_account(
    const TenantId& tenant, const AccountId& account, const OrderChainId& chain,
    Reason reason, std::string detail, const VerifiedOrderEvent* event,
    const Reservation* reservation) {
  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.account_quarantine.emplace_back(account, true);

  // The reservation, if any, goes to QUARANTINE but keeps its hold: the point
  // of quarantine is that we stop trusting the evidence, not the exposure.
  if (reservation != nullptr && reservation->holds_budget()) {
    Reservation candidate = *reservation;
    if (validate_transition_path(candidate.state, ReservationState::Quarantine).ok()) {
      candidate.state = ReservationState::Quarantine;
      candidate.note = detail;
      request.data.reservations.push_back(candidate);
      // QUARANTINE still holds budget, so the pending delta is zero; it is
      // taken anyway so the row's working-order count stays exact.
      request.data.exposure =
          apply_reservation_delta(store_.exposure(), reservation, &candidate, 0);
    }
  }

  // The offending event is recorded as applied so a redelivery of the same
  // bytes is a no-op rather than a second quarantine event.
  if (event != nullptr) {
    AppliedEventRecord applied;
    applied.tenant_id = tenant;
    applied.source_namespace = event->source_namespace;
    applied.event_id = event->event_id;
    applied.canonical_hash = event->canonical_hash();
    request.data.applied_events.push_back(applied);
  }

  AuditEvent audit = audit_.begin("Quarantine", {});
  audit.account_id = account;
  audit.symbol = reservation != nullptr ? reservation->symbol : std::string{};
  audit.reservation_id =
      reservation != nullptr ? reservation->reservation_id : std::string{};
  audit.source_event_id = event != nullptr ? event->event_id : std::string{};
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.payload_hash = event != nullptr ? event->canonical_hash() : std::string{};
  audit.reason = reason;
  audit.note = detail + " (chain " + chain + ")";
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  metrics_.incr_labeled(metric::kQuarantine,
                        std::string("reason=") + std::string(reason_name(reason)));
  if (outcome != CommitOutcome::Committed) {
    return refuse(Reason::PersistenceCommitFailed,
                  "could not record quarantine: " +
                      std::string(commit_outcome_name(outcome)));
  }

  RiskResult r = make_result(RiskDecision::Quarantined, reason, std::move(detail));
  r.reservation_id = reservation != nullptr ? reservation->reservation_id : std::string{};
  r.commit_revision = store_.domain_state().revision;
  return r;
}

// ---------------------------------------------------------------------------
// apply_verified_order_event
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::apply_verified_order_event(
    const VerifiedOrderEvent& event) {
  // CW-17: the inbound report is durable but not yet applied.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterInboundDurableBeforeApply);
      r != Reason::Ok) {
    return refuse(r, "failpoint before event apply");
  }

  if (const Status s = validate_identifier(event.tenant_id, "tenant_id"); !s) {
    return refuse(s);
  }
  if (const Status s = validate_identifier(event.account_id, "account_id"); !s) {
    return refuse(s);
  }
  if (const Status s = validate_identifier(event.order_chain_id, "order_chain_id");
      !s) {
    return refuse(s);
  }
  if (const Status s = validate_identifier(event.event_id, "event_id"); !s) {
    return refuse(s);
  }
  if (const Status s = validate_identifier(event.source_namespace, "source_namespace");
      !s) {
    return refuse(s);
  }
  if (event.tenant_id != options_.tenant_id) {
    return refuse(Reason::TenantMismatch, event.tenant_id);
  }

  const std::string event_hash = event.canonical_hash();
  std::lock_guard<std::mutex> domain_lock(domain_mu_);

  // RI-19: same key and same bytes is a no-op; same key with different bytes
  // is contradictory evidence and goes to quarantine.
  if (std::optional<AppliedEventRecord> prior = store_.find_applied_event(
          event.tenant_id, event.source_namespace, event.event_id);
      prior.has_value()) {
    if (prior->canonical_hash == event_hash) {
      metrics_.incr_labeled(metric::kEventDedup, "result=duplicate_noop");
      RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::DuplicateEvent,
                                 "event already applied at revision " +
                                     std::to_string(prior->applied_revision));
      r.commit_revision = prior->applied_revision;
      return r;
    }
    metrics_.incr_labeled(metric::kEventDedup, "result=hash_mismatch");
    std::optional<ReservationId> chain_ref = store_.find_chain(
        event.tenant_id, event.account_id, event.order_chain_id);
    std::optional<Reservation> reservation =
        chain_ref.has_value() ? store_.find_reservation(*chain_ref) : std::nullopt;
    return quarantine_account(
        event.tenant_id, event.account_id, event.order_chain_id,
        Reason::EventHashMismatch,
        "event " + event.event_id + " replayed with different content", nullptr,
        reservation.has_value() ? &*reservation : nullptr);
  }

  std::optional<ReservationId> chain_ref =
      store_.find_chain(event.tenant_id, event.account_id, event.order_chain_id);
  if (!chain_ref.has_value()) {
    // §13: an execution on a chain we never reserved is not clamped away.
    return quarantine_account(event.tenant_id, event.account_id,
                              event.order_chain_id, Reason::UnknownChain,
                              "event for an unknown order chain", &event, nullptr);
  }
  std::optional<Reservation> existing = store_.find_reservation(*chain_ref);
  if (!existing.has_value()) {
    return quarantine_account(event.tenant_id, event.account_id,
                              event.order_chain_id, Reason::ReservationNotFound,
                              "chain index points at a missing reservation", &event,
                              nullptr);
  }
  const Reservation current = *existing;

  if (!event.symbol.empty() && event.symbol != current.symbol) {
    return quarantine_account(event.tenant_id, event.account_id,
                              event.order_chain_id, Reason::FillSymbolMismatch,
                              event.symbol + " != " + current.symbol, &event,
                              &current);
  }

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  Reservation candidate = current;
  std::int64_t position_delta = 0;
  bool mutate_reservation = true;
  Reason result_reason = Reason::Ok;
  std::string note;

  switch (event.kind) {
    case OrderEventKind::OrderAccepted: {
      if (!current.holds_budget()) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::ReservationTerminal,
                                  "acceptance for a terminal chain", &event, &current);
      }
      if (event.accepted_total_units != 0 &&
          event.accepted_total_units != current.accepted_total_units) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "LP accepted " + std::to_string(event.accepted_total_units) +
                " but we reserved " + std::to_string(current.accepted_total_units),
            &event, &current);
      }
      candidate.state = ReservationState::Working;
      note = "LP acknowledged the order";
      break;
    }

    case OrderEventKind::AppliedFill: {
      if (event.fill_units <= 0) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::InternalInvariant,
                                  "fill quantity must be > 0", &event, &current);
      }
      if (event.side != current.side) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::FillSideMismatch,
                                  std::string(side_name(event.side)) + " != " +
                                      std::string(side_name(current.side)),
                                  &event, &current);
      }
      if (!current.holds_budget()) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::ReservationTerminal,
                                  "fill arrived after the chain went terminal",
                                  &event, &current);
      }
      std::int64_t new_cum = 0;
      if (!add_i64(current.cum_units, event.fill_units, new_cum)) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::ArithmeticOverflow,
                                  "cumulative quantity overflow", &event, &current);
      }
      // The ceiling is the larger branch: during a pending replace-up the LP
      // may legitimately fill beyond the accepted total (§14.1).
      const std::int64_t ceiling =
          std::max(current.accepted_total_units,
                   current.proposed_total_units.value_or(0));
      if (new_cum > ceiling) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::FillExceedsAccepted,
            "cumulative " + std::to_string(new_cum) + " > reserved ceiling " +
                std::to_string(ceiling),
            &event, &current);
      }
      if (event.verified_cum_units != 0 && event.verified_cum_units != new_cum) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "reported cumulative " + std::to_string(event.verified_cum_units) +
                " != ledger " + std::to_string(new_cum),
            &event, &current);
      }

      candidate.cum_units = new_cum;
      // RI-07: the position moves and the outstanding shrinks in this one
      // transaction. Because the pending side is derived from
      // risk_effective_remaining, there is no window where the quantity exists
      // in neither place.
      position_delta = side_sign(current.side) * event.fill_units;
      if (current.state == ReservationState::Committed ||
          current.state == ReservationState::InFlight ||
          current.state == ReservationState::Unknown) {
        candidate.state = ReservationState::Working;
      }
      note = "fill " + std::to_string(event.fill_units) + ", cum " +
             std::to_string(new_cum);
      break;
    }

    case OrderEventKind::NewRejected: {
      // RI-17: a reject that arrives after fills contradicts the ledger.
      if (current.cum_units > 0) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "New reject after " + std::to_string(current.cum_units) + " filled units",
            &event, &current);
      }
      if (!current.holds_budget()) {
        return quarantine_account(event.tenant_id, event.account_id,
                                  event.order_chain_id, Reason::ReservationTerminal,
                                  "reject for a terminal chain", &event, &current);
      }
      candidate.state = ReservationState::Final;
      candidate.terminal_reason = Reason::Ok;
      note = "LP business reject; working hold released";
      break;
    }

    case OrderEventKind::PendingCancel: {
      if (!current.holds_budget()) {
        result_reason = Reason::ReconciliationRequired;
        note = "pending-cancel marker for a terminal chain";
        mutate_reservation = false;
        break;
      }
      // RI-04: recorded, but the held quantity is untouched.
      candidate.pending_cancel = true;
      if (!event.related_command_id.empty()) {
        candidate.pending_cancel_command_id = event.related_command_id;
      }
      note = "pending cancel; hold retained";
      break;
    }

    case OrderEventKind::CancelRejected: {
      if (!current.holds_budget()) {
        result_reason = Reason::ReconciliationRequired;
        note = "cancel-reject for a terminal chain";
        mutate_reservation = false;
        break;
      }
      candidate.pending_cancel = false;
      candidate.pending_cancel_command_id.reset();
      note = "cancel rejected; order still working, hold retained";
      break;
    }

    case OrderEventKind::CancelAccepted:
    case OrderEventKind::Expired: {
      if (!current.holds_budget()) {
        result_reason = Reason::ReconciliationRequired;
        note = "terminal report for an already terminal chain";
        mutate_reservation = false;
        break;
      }
      // PB-19: the final report's cumulative must agree with the ledger, or we
      // do not know how much was actually executed.
      if (event.verified_cum_units != current.cum_units) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "terminal cumulative " + std::to_string(event.verified_cum_units) +
                " != ledger " + std::to_string(current.cum_units),
            &event, &current);
      }
      candidate.state = ReservationState::Final;
      candidate.pending_cancel = false;
      candidate.pending_cancel_command_id.reset();
      // A terminal report also ends any pending modification.
      candidate.proposed_total_units.reset();
      candidate.proposed_limit_price_ticks.reset();
      candidate.proposed_valuation.reset();
      candidate.pending_modify_command_id.reset();
      note = "terminal; unfilled commitment released, position retained";
      break;
    }

    case OrderEventKind::OrderFilled: {
      if (!current.holds_budget()) {
        result_reason = Reason::ReconciliationRequired;
        note = "filled report for an already terminal chain";
        mutate_reservation = false;
        break;
      }
      if (event.verified_cum_units != current.cum_units) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "filled cumulative " + std::to_string(event.verified_cum_units) +
                " != ledger " + std::to_string(current.cum_units),
            &event, &current);
      }
      if (current.cum_units != current.accepted_total_units) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "Filled claimed with cumulative " + std::to_string(current.cum_units) +
                " of accepted " + std::to_string(current.accepted_total_units),
            &event, &current);
      }
      candidate.state = ReservationState::Final;
      candidate.proposed_total_units.reset();
      candidate.proposed_limit_price_ticks.reset();
      candidate.proposed_valuation.reset();
      candidate.pending_modify_command_id.reset();
      note = "fully filled";
      break;
    }

    case OrderEventKind::PendingReplace: {
      // RI-01 detector: a G is in flight for a branch we never reserved.
      if (!current.has_pending_modify()) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::ReplaceNoAcceptedVersion,
            "pending replace reported with no reserved proposed branch", &event,
            &current);
      }
      mutate_reservation = false;
      note = "pending replace acknowledged; worst-case branch already reserved";
      break;
    }

    case OrderEventKind::ReplaceAccepted: {
      if (!current.has_pending_modify()) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::ReplaceNoAcceptedVersion,
            "replace accepted with no reserved proposed branch", &event, &current);
      }
      const std::int64_t proposed = *current.proposed_total_units;
      if (event.accepted_total_units != 0 && event.accepted_total_units != proposed) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::CumulativeMismatch,
            "LP accepted total " + std::to_string(event.accepted_total_units) +
                " != proposed " + std::to_string(proposed),
            &event, &current);
      }
      if (proposed < current.cum_units) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::ReplaceBelowCumulative,
            "promoted total " + std::to_string(proposed) + " < cumulative " +
                std::to_string(current.cum_units),
            &event, &current);
      }

      candidate.accepted_total_units = proposed;
      if (current.proposed_valuation.has_value()) {
        candidate.accepted_valuation = *current.proposed_valuation;
      }
      if (current.proposed_limit_price_ticks.has_value()) {
        candidate.accepted_limit_price_ticks = *current.proposed_limit_price_ticks;
      }
      candidate.proposed_total_units.reset();
      candidate.proposed_limit_price_ticks.reset();
      candidate.proposed_valuation.reset();
      candidate.pending_modify_command_id.reset();
      candidate.state = ReservationState::Working;

      // §14.2: promotion may only ever lower or keep the reserved worst case.
      // A promotion that would need more budget than was reserved means the
      // reservation and the LP disagree, which is a quarantine, not a credit.
      if (candidate.risk_effective_remaining() > current.risk_effective_remaining()) {
        return quarantine_account(
            event.tenant_id, event.account_id, event.order_chain_id,
            Reason::ReplaceGrantExceedsReservation,
            "promoted remaining " +
                std::to_string(candidate.risk_effective_remaining()) +
                " > reserved " + std::to_string(current.risk_effective_remaining()),
            &event, &current);
      }
      note = "proposed branch promoted; conservative difference released";
      break;
    }

    case OrderEventKind::ReplaceRejected: {
      if (!current.has_pending_modify()) {
        result_reason = Reason::ReconciliationRequired;
        note = "replace reject with no pending branch";
        mutate_reservation = false;
        break;
      }
      candidate.proposed_total_units.reset();
      candidate.proposed_limit_price_ticks.reset();
      candidate.proposed_valuation.reset();
      candidate.pending_modify_command_id.reset();
      if (current.holds_budget()) candidate.state = ReservationState::Working;
      note = "proposed branch dropped; accepted branch and fills unchanged";
      break;
    }

    case OrderEventKind::UnknownOutcome: {
      if (!current.holds_budget()) {
        result_reason = Reason::ReconciliationRequired;
        note = "unknown outcome for a terminal chain";
        mutate_reservation = false;
        break;
      }
      candidate.state = ReservationState::Unknown;
      result_reason = Reason::ReconciliationRequired;
      note = "outcome unknown; worst-case hold retained";
      metrics_.incr_labeled(metric::kUnknownOutcome, "scope=chain");
      break;
    }

    case OrderEventKind::StatusSnapshot: {
      // RI-20: a status snapshot is not a fill. A disagreement moves the chain
      // to UNKNOWN for reconciliation; it never synthesises the difference.
      if (event.verified_cum_units != current.cum_units) {
        if (current.holds_budget()) {
          candidate.state = ReservationState::Unknown;
        } else {
          mutate_reservation = false;
        }
        result_reason = Reason::ReconciliationRequired;
        note = "status cumulative " + std::to_string(event.verified_cum_units) +
               " != ledger " + std::to_string(current.cum_units) +
               "; no synthetic fill created";
        metrics_.incr(metric::kReconciliationPending);
      } else {
        mutate_reservation = false;
        note = "status snapshot agrees with the ledger";
      }
      break;
    }

    case OrderEventKind::TradeCorrect:
    case OrderEventKind::TradeBust: {
      // RI-21: there is no automatic path that rewrites the fill ledger.
      return quarantine_account(
          event.tenant_id, event.account_id, event.order_chain_id,
          Reason::TradeCorrectUnsupported,
          std::string(order_event_kind_name(event.kind)) +
              " requires manual verification",
          &event, &current);
    }
  }

  if (mutate_reservation && candidate.state != current.state) {
    if (const Status s = validate_transition_path(current.state, candidate.state);
        !s) {
      return quarantine_account(event.tenant_id, event.account_id,
                                event.order_chain_id, s.reason(), s.detail(), &event,
                                &current);
    }
  }

  // --- build the new projection -----------------------------------------
  //
  // RI-07: the position move and the outstanding decrease are one delta on one
  // row, so there is no intermediate state in which the quantity exists in
  // neither place.
  const DomainExposure committed_exposure = store_.exposure();
  if (position_delta != 0) {
    const AccountExposure* ae = committed_exposure.find(current.account_id);
    const SymbolExposure* se = ae != nullptr ? ae->find(current.symbol) : nullptr;
    std::int64_t moved = 0;
    if (se != nullptr && !add_i64(se->position_units, position_delta, moved)) {
      return quarantine_account(event.tenant_id, event.account_id,
                                event.order_chain_id, Reason::ArithmeticOverflow,
                                "position overflow", &event, &current);
    }
  }
  const DomainExposure new_exposure = apply_reservation_delta(
      committed_exposure, &current, mutate_reservation ? &candidate : &current,
      position_delta);

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  if (mutate_reservation) request.data.reservations.push_back(candidate);
  request.data.exposure = new_exposure;

  AppliedEventRecord applied;
  applied.tenant_id = event.tenant_id;
  applied.source_namespace = event.source_namespace;
  applied.event_id = event.event_id;
  applied.canonical_hash = event_hash;
  request.data.applied_events.push_back(applied);

  AuditEvent audit =
      audit_.begin(std::string(order_event_kind_name(event.kind)), {});
  audit.account_id = event.account_id;
  audit.symbol = current.symbol;
  audit.reservation_id = current.reservation_id;
  audit.command_id = event.related_command_id;
  audit.source_event_id = event.event_id;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{},
                                 &current.accepted_valuation, domain.revision);
  audit.payload_hash = event_hash;
  audit.reason = result_reason;
  audit.note = note;
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  // RI-27: if the new state breaches a cap, stop risk-increasing commands in
  // the same transaction. The exposure itself is never pretended away.
  Result<DomainMetrics> post_metrics = [&]() -> Result<DomainMetrics> {
    const MarketView view = feed_.snapshot();
    Result<ValuationTable> valuations =
        build_valuations(view, policy, new_exposure, {}, 0, clock_.mono_now());
    if (!valuations.ok()) {
      return Result<DomainMetrics>::fail(valuations.reason(), valuations.detail());
    }
    return evaluate_exposure(new_exposure, policy, valuations.value());
  }();
  if (post_metrics.ok() && domain.mode == DomainMode::Normal) {
    LimitCheckInput breach_check;
    breach_check.account_id = current.account_id;
    breach_check.symbol = current.symbol;
    Result<MarginBasis> basis =
        margin_basis(feed_.snapshot(), policy, current.account_id, clock_.mono_now());
    breach_check.margin_basis = basis.ok() ? basis.value() : MarginBasis{};
    if (const Status s = check_limits(post_metrics.value(), policy, breach_check);
        !s) {
      request.data.mode = DomainMode::NoNew;
      AuditEvent breach = audit_.begin("LimitBreach", {});
      breach.account_id = current.account_id;
      breach.symbol = current.symbol;
      breach.policy_version = policy.version;
      breach.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
      breach.reason = s.reason();
      breach.note = "mode forced to NO_NEW: " + s.detail();
      breach.commit_utc = clock_.utc_now();
      breach.commit_mono = clock_.mono_now();
      request.data.audit.push_back(breach);
      metrics_.incr_labeled(metric::kLimitBreach, "scope=post_event");
    }
  }

  // CW-18: the ledger mutation is staged but not committed.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterLedgerStageBeforeCommit);
      r != Reason::Ok) {
    return refuse(r, "failpoint before event commit");
  }

  const CommitOutcome outcome = store_.commit(request);
  metrics_.incr_labeled(metric::kCommitOutcome,
                        std::string("outcome=") +
                            std::string(commit_outcome_name(outcome)));
  if (outcome == CommitOutcome::Fenced) {
    metrics_.incr(metric::kFencingReject);
    return refuse(Reason::FencingTokenStale, "writer fenced");
  }
  if (outcome == CommitOutcome::RolledBack) {
    return refuse(Reason::PersistenceCommitFailed, "event commit rolled back");
  }
  if (outcome == CommitOutcome::UnknownButDurable) {
    return refuse(Reason::PersistenceCommitUnknown,
                  "event commit acknowledgement lost");
  }

  // CW-19: committed, but the durable inbox cursor was not advanced. A replay
  // of the same event must be a no-op, which the dedup key guarantees.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterApplyCommitBeforeCursorAck);
      r != Reason::Ok) {
    return refuse(r, "failpoint after event commit");
  }

  RiskResult r = make_result(
      result_reason == Reason::Ok ? RiskDecision::AllowCommitted
                                  : RiskDecision::HoldRetryable,
      result_reason, note);
  r.reservation_id = current.reservation_id;
  r.commit_revision = store_.domain_state().revision;
  return r;
}

// ---------------------------------------------------------------------------
// reconcile_with_evidence
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::reconcile_with_evidence(const ReconcileEvidence& evidence,
                                                    const Principal& actor,
                                                    const Principal& approver) {
  if (const Status s = validate_identifier(evidence.evidence_id, "evidence_id"); !s) {
    return refuse(s);
  }
  if (evidence.tenant_id != options_.tenant_id) {
    return refuse(Reason::TenantMismatch, evidence.tenant_id);
  }
  // §18.1: a quarantine release needs two people, and `force_release` does not
  // exist. Only this path -- with evidence and a durable audit -- can move a
  // reservation out of UNKNOWN or QUARANTINE.
  if (!actor.has_role(Role::Operator) && !actor.has_role(Role::RiskAdmin)) {
    return refuse(Reason::Unauthorized, "operator or risk_admin role required");
  }
  if (!approver.has_role(Role::RiskAdmin)) {
    return refuse(Reason::ApprovalRequired, "risk_admin approver required");
  }
  if (approver.principal_id.empty() ||
      approver.principal_id == actor.principal_id) {
    return refuse(Reason::ApprovalRequired, "two distinct principals required");
  }
  if (evidence.resolved_state != ReservationState::Working &&
      evidence.resolved_state != ReservationState::Final) {
    return refuse(Reason::EvidenceInsufficient,
                  "resolution must be WORKING or FINAL");
  }
  if (evidence.verified_cum_units < 0 || evidence.verified_accepted_total_units <= 0) {
    return refuse(Reason::EvidenceInsufficient, "non-positive verified quantities");
  }
  if (evidence.verified_cum_units > evidence.verified_accepted_total_units) {
    return refuse(Reason::EvidenceInsufficient,
                  "verified cumulative exceeds the verified accepted total");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);

  if (std::optional<AppliedEventRecord> prior = store_.find_applied_event(
          evidence.tenant_id, evidence.source_namespace, evidence.evidence_id);
      prior.has_value()) {
    if (prior->canonical_hash == evidence.canonical_hash()) {
      RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::DuplicateEvent,
                                 "evidence already applied");
      r.commit_revision = prior->applied_revision;
      return r;
    }
    return refuse(Reason::EventHashMismatch,
                  "evidence id reused with different content");
  }

  std::optional<ReservationId> chain_ref = store_.find_chain(
      evidence.tenant_id, evidence.account_id, evidence.order_chain_id);
  if (!chain_ref.has_value()) {
    return refuse(Reason::UnknownChain, evidence.order_chain_id);
  }
  std::optional<Reservation> existing = store_.find_reservation(*chain_ref);
  if (!existing.has_value()) {
    return refuse(Reason::ReservationNotFound, *chain_ref);
  }
  const Reservation current = *existing;

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  Reservation candidate = current;
  candidate.accepted_total_units = evidence.verified_accepted_total_units;
  candidate.cum_units = evidence.verified_cum_units;
  if (evidence.clear_proposed_branch) {
    candidate.proposed_total_units.reset();
    candidate.proposed_limit_price_ticks.reset();
    candidate.proposed_valuation.reset();
    candidate.pending_modify_command_id.reset();
  }
  candidate.state = evidence.resolved_state;
  candidate.pending_cancel = false;
  candidate.pending_cancel_command_id.reset();
  candidate.note = evidence.note;

  if (const Status s = validate_transition_path(current.state, candidate.state); !s) {
    return refuse(s);
  }

  // Positions must follow the verified cumulative, in the same transaction.
  std::int64_t cum_delta = 0;
  if (!sub_i64(evidence.verified_cum_units, current.cum_units, cum_delta)) {
    return refuse(Reason::ArithmeticOverflow, "cumulative delta");
  }
  std::int64_t position_delta = 0;
  if (!mul_i64(side_sign(current.side), cum_delta, position_delta)) {
    return refuse(Reason::ArithmeticOverflow, "position delta");
  }
  const DomainExposure new_exposure = apply_reservation_delta(
      store_.exposure(), &current, &candidate, position_delta);

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.reservations.push_back(candidate);
  request.data.exposure = new_exposure;

  AppliedEventRecord applied;
  applied.tenant_id = evidence.tenant_id;
  applied.source_namespace = evidence.source_namespace;
  applied.event_id = evidence.evidence_id;
  applied.canonical_hash = evidence.canonical_hash();
  request.data.applied_events.push_back(applied);

  // Clearing the account quarantine is part of the same audited transaction;
  // it is never a separate "unstick it" call.
  bool still_quarantined =
      candidate.state == ReservationState::Quarantine;
  for (const Reservation& r : store_.reservations()) {
    if (r.account_id != current.account_id) continue;
    if (r.reservation_id == candidate.reservation_id) continue;
    if (r.state == ReservationState::Quarantine) still_quarantined = true;
  }
  if (!still_quarantined && store_.account_quarantined(current.account_id)) {
    request.data.account_quarantine.emplace_back(current.account_id, false);
  }

  AuditEvent audit = audit_.begin("ReconcileResolved", {});
  audit.account_id = current.account_id;
  audit.symbol = current.symbol;
  audit.reservation_id = current.reservation_id;
  audit.source_event_id = evidence.evidence_id;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.payload_hash = evidence.canonical_hash();
  audit.actor = actor.principal_id + "+" + approver.principal_id;
  audit.note = "resolved to " +
               std::string(reservation_state_name(evidence.resolved_state)) +
               " cum=" + std::to_string(evidence.verified_cum_units) +
               " accepted=" + std::to_string(evidence.verified_accepted_total_units) +
               (evidence.note.empty() ? "" : " note=" + evidence.note);
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(outcome == CommitOutcome::Fenced ? Reason::FencingTokenStale
                                                   : Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }

  // CW-20: the release committed but the operator acknowledgement was lost. A
  // retry is a dedup no-op, so the release happens exactly once.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterReleaseCommitBeforeAck);
      r != Reason::Ok) {
    return refuse(r, "failpoint after reconciliation commit");
  }

  RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok, "reconciled");
  r.reservation_id = current.reservation_id;
  r.commit_revision = store_.domain_state().revision;
  return r;
}

// ---------------------------------------------------------------------------
// operator actions
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::set_mode(DomainMode mode, const Principal& who,
                                     std::string note) {
  if (!who.has_role(Role::RiskAdmin) && !who.has_role(Role::Operator)) {
    return refuse(Reason::Unauthorized, "risk_admin or operator role required");
  }
  // §18.1: a trader may not touch the kill switch even if they can trade.
  if (who.has_role(Role::Trader) && !who.has_role(Role::RiskAdmin) &&
      !who.has_role(Role::Operator)) {
    return refuse(Reason::Unauthorized, "traders cannot change the domain mode");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);
  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.mode = mode;

  AuditEvent audit = audit_.begin("ModeChanged", {});
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.actor = who.principal_id;
  audit.note = std::string(domain_mode_name(domain.mode)) + " -> " +
               std::string(domain_mode_name(mode)) +
               (note.empty() ? "" : " (" + note + ")");
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }
  RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok,
                             std::string(domain_mode_name(mode)));
  r.commit_revision = store_.domain_state().revision;
  return r;
}

RiskResult RiskCoordinator::install_policy(const RiskPolicy& policy,
                                           const Principal& actor,
                                           const Principal& approver,
                                           std::string note) {
  // §7.2 step 1: the whole bundle validates before anything is installed.
  if (const Status s = policy.validate(); !s) return refuse(s);

  if (!actor.has_role(Role::RiskAdmin)) {
    return refuse(Reason::Unauthorized, "risk_admin role required");
  }
  // Dual approval for anything that moves a hard cap.
  if (!approver.has_role(Role::RiskAdmin) || approver.principal_id.empty() ||
      approver.principal_id == actor.principal_id) {
    return refuse(Reason::ApprovalRequired,
                  "a second distinct risk_admin must approve a policy change");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);
  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy current = store_.policy();

  // A policy version must move forward, so an audit can order two decisions.
  if (policy.version <= current.version) {
    return refuse(Reason::PolicyVersionRegression,
                  "new version " + std::to_string(policy.version) +
                      " <= current " + std::to_string(current.version));
  }
  if (policy.account_currency.code != current.account_currency.code ||
      policy.account_currency.minor_scale != current.account_currency.minor_scale) {
    // Re-denominating an account mid-flight would silently rescale every
    // committed limit and hold.
    return refuse(Reason::ScaleConversionUnsupported,
                  "account currency or scale change is not supported in v1");
  }

  const DomainExposure exposure = store_.exposure();

  // §7.2 step 3-4: revalue under the new ruleset, and if the live envelope no
  // longer fits, mark BREACHED and stop risk increase. Existing reservations
  // are never deleted to make the numbers fit (PB-25).
  DomainMode next_mode = policy.mode;
  std::string breach_note;
  const MarketView view = feed_.snapshot();
  Result<ValuationTable> valuations =
      build_valuations(view, policy, exposure, {}, 0, clock_.mono_now());
  if (valuations.ok()) {
    Result<DomainMetrics> metrics =
        evaluate_exposure(exposure, policy, valuations.value());
    if (metrics.ok()) {
      for (const auto& [account, am] : metrics.value().accounts) {
        LimitCheckInput check;
        check.account_id = account;
        Result<MarginBasis> basis =
            margin_basis(view, policy, account, clock_.mono_now());
        check.margin_basis = basis.ok() ? basis.value() : MarginBasis{};
        for (const auto& [symbol, sm] : am.symbols) {
          (void)sm;
          check.symbol = symbol;
          if (const Status s = check_limits(metrics.value(), policy, check); !s) {
            next_mode = DomainMode::NoNew;
            breach_note = "breached on install: " + std::string(reason_name(s.reason())) +
                          " (" + account + "/" + symbol + ")";
            break;
          }
        }
        if (!breach_note.empty()) break;
      }
    } else {
      next_mode = DomainMode::NoNew;
      breach_note = "could not revalue on install: " + metrics.detail();
    }
  } else {
    next_mode = DomainMode::NoNew;
    breach_note = "could not value reference data on install: " + valuations.detail();
  }

  RiskPolicy installed = policy;
  installed.mode = next_mode;

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.policy = installed;
  request.data.mode = next_mode;

  AuditEvent audit = audit_.begin("PolicyInstalled", {});
  audit.policy_version = installed.version;
  audit.snapshots = snapshot_ref(installed, view, nullptr, domain.revision);
  audit.payload_hash = installed.hash();
  audit.actor = actor.principal_id + "+" + approver.principal_id;
  audit.reason = breach_note.empty() ? Reason::Ok : Reason::LimitBreached;
  audit.note = "v" + std::to_string(current.version) + " -> v" +
               std::to_string(installed.version) +
               (note.empty() ? "" : " (" + note + ")") +
               (breach_note.empty() ? "" : "; " + breach_note);
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }
  if (!breach_note.empty()) metrics_.incr_labeled(metric::kLimitBreach, "scope=policy");

  RiskResult r = make_result(RiskDecision::AllowCommitted,
                             breach_note.empty() ? Reason::Ok : Reason::LimitBreached,
                             breach_note.empty() ? "policy installed" : breach_note);
  r.commit_revision = store_.domain_state().revision;
  return r;
}

RiskResult RiskCoordinator::open_admission(bool open, std::string note) {
  std::lock_guard<std::mutex> domain_lock(domain_mu_);
  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.admission_open = open;

  AuditEvent audit = audit_.begin("AdmissionGate", {});
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.note = (open ? "opened" : "closed") + (note.empty() ? "" : ": " + note);
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }
  RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok,
                             open ? "admission open" : "admission closed");
  r.commit_revision = store_.domain_state().revision;
  return r;
}

RiskResult RiskCoordinator::clear_account_quarantine(const AccountId& account,
                                                     const Principal& actor,
                                                     const Principal& approver,
                                                     std::string note) {
  if (!actor.has_role(Role::Operator) && !actor.has_role(Role::RiskAdmin)) {
    return refuse(Reason::Unauthorized, "operator or risk_admin role required");
  }
  if (!approver.has_role(Role::RiskAdmin) || approver.principal_id.empty() ||
      approver.principal_id == actor.principal_id) {
    return refuse(Reason::ApprovalRequired, "two distinct principals required");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);
  if (!store_.account_quarantined(account)) {
    return refuse(Reason::Unsupported, account + " is not quarantined");
  }
  // A quarantine may only be lifted once every quarantined chain on the
  // account has been reconciled with evidence; otherwise this would be the
  // `force_release` the spec forbids.
  for (const Reservation& r : store_.reservations()) {
    if (r.account_id != account) continue;
    if (r.state == ReservationState::Quarantine) {
      return refuse(Reason::EvidenceInsufficient,
                    "chain " + r.order_chain_id +
                        " is still quarantined; reconcile it with evidence first");
    }
  }

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;
  request.data.account_quarantine.emplace_back(account, false);

  AuditEvent audit = audit_.begin("QuarantineCleared", {});
  audit.account_id = account;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.actor = actor.principal_id + "+" + approver.principal_id;
  audit.note = note;
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    return refuse(Reason::PersistenceCommitFailed,
                  std::string(commit_outcome_name(outcome)));
  }
  RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok, "cleared");
  r.commit_revision = store_.domain_state().revision;
  return r;
}

}  // namespace risk
