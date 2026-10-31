#include "risk/coordinator.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "risk/checked.hpp"
#include "risk/hash.hpp"

namespace risk {
namespace {

constexpr const char* kNamespaceCommand = "p05.command";

// A failpoint outside the store still has to be reachable, but the only
// meaningful outcomes there are "crash" and "turn this request into a hold".
// Returns the reason to report, or Reason::Ok to continue.
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

RiskResult make_result(RiskDecision decision, Reason reason, std::string detail) {
  RiskResult r;
  r.decision = decision;
  r.reason = reason;
  r.detail = std::move(detail);
  return r;
}

// Maps a refusal reason onto the §12.2 decision contract. "We could not prove
// it" must become HOLD, never DENY: a denial tells the caller the command is
// definitely not at the LP, and only a proven refusal may say that.
RiskResult refuse(Reason reason, std::string detail) {
  const RiskDecision decision = reason_is_retryable(reason)
                                    ? RiskDecision::HoldRetryable
                                    : RiskDecision::DenyDefinitive;
  return make_result(decision, reason, std::move(detail));
}

RiskResult refuse(const Status& s) { return refuse(s.reason(), s.detail()); }

std::string decision_label(RiskDecision d) {
  return std::string(risk_decision_name(d));
}

// Saturating accumulation for the derived projection. A value that overflows is
// pinned at INT64_MAX so every downstream cap check fails; it can never wrap to
// a small number that looks safe (RI-13).
void add_saturating(std::int64_t& acc, std::int64_t delta) {
  std::int64_t out = 0;
  if (!add_i64(acc, delta, out)) {
    acc = std::numeric_limits<std::int64_t>::max();
    return;
  }
  acc = out;
}

}  // namespace

std::string_view risk_decision_name(RiskDecision d) noexcept {
  switch (d) {
    case RiskDecision::AllowCommitted: return "ALLOW_COMMITTED";
    case RiskDecision::DenyDefinitive: return "DENY_DEFINITIVE";
    case RiskDecision::HoldRetryable: return "HOLD_RETRYABLE";
    case RiskDecision::Conflict: return "CONFLICT";
    case RiskDecision::Quarantined: return "QUARANTINED";
  }
  return "UNKNOWN_DECISION";
}

std::string_view order_event_kind_name(OrderEventKind k) noexcept {
  switch (k) {
    case OrderEventKind::OrderAccepted: return "OrderAccepted";
    case OrderEventKind::AppliedFill: return "AppliedFill";
    case OrderEventKind::NewRejected: return "NewRejected";
    case OrderEventKind::PendingCancel: return "PendingCancel";
    case OrderEventKind::CancelAccepted: return "CancelAccepted";
    case OrderEventKind::CancelRejected: return "CancelRejected";
    case OrderEventKind::OrderFilled: return "OrderFilled";
    case OrderEventKind::Expired: return "Expired";
    case OrderEventKind::PendingReplace: return "PendingReplace";
    case OrderEventKind::ReplaceAccepted: return "ReplaceAccepted";
    case OrderEventKind::ReplaceRejected: return "ReplaceRejected";
    case OrderEventKind::UnknownOutcome: return "UnknownOutcome";
    case OrderEventKind::StatusSnapshot: return "StatusSnapshot";
    case OrderEventKind::TradeCorrect: return "TradeCorrect";
    case OrderEventKind::TradeBust: return "TradeBust";
  }
  return "UnknownKind";
}

bool parse_order_event_kind(std::string_view text, OrderEventKind& out) noexcept {
  const OrderEventKind all[] = {
      OrderEventKind::OrderAccepted,  OrderEventKind::AppliedFill,
      OrderEventKind::NewRejected,    OrderEventKind::PendingCancel,
      OrderEventKind::CancelAccepted, OrderEventKind::CancelRejected,
      OrderEventKind::OrderFilled,    OrderEventKind::Expired,
      OrderEventKind::PendingReplace,
      OrderEventKind::ReplaceAccepted, OrderEventKind::ReplaceRejected,
      OrderEventKind::UnknownOutcome, OrderEventKind::StatusSnapshot,
      OrderEventKind::TradeCorrect,   OrderEventKind::TradeBust};
  for (const OrderEventKind k : all) {
    if (order_event_kind_name(k) == text) {
      out = k;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Canonical command serialisation
// ---------------------------------------------------------------------------

std::string NewOrderCommand::canonical_bytes() const {
  Hasher h;
  h.named("kind", std::string("NEW"))
      .named("tenant", tenant_id)
      .named("account", account_id)
      .named("command", command_id)
      .named("chain", order_chain_id)
      .named("symbol", symbol)
      .named("side", side_name(side))
      .named("total_units", total_units)
      .named("limit_price_ticks", limit_price_ticks)
      .named("order_type", std::string("LIMIT"))
      .named("tif", std::string("DAY"));
  return h.canonical_bytes();
}

std::string NewOrderCommand::canonical_hash() const {
  return digest_hex(sha256(canonical_bytes()));
}

std::string ReplaceCommand::canonical_bytes() const {
  Hasher h;
  h.named("kind", std::string("REPLACE"))
      .named("tenant", tenant_id)
      .named("account", account_id)
      .named("command", command_id)
      .named("chain", order_chain_id)
      .named("new_total_units", new_total_units)
      .named("new_limit_price_ticks", new_limit_price_ticks);
  return h.canonical_bytes();
}

std::string ReplaceCommand::canonical_hash() const {
  return digest_hex(sha256(canonical_bytes()));
}

std::string CancelCommand::canonical_bytes() const {
  Hasher h;
  h.named("kind", std::string("CANCEL"))
      .named("tenant", tenant_id)
      .named("account", account_id)
      .named("command", command_id)
      .named("chain", order_chain_id);
  return h.canonical_bytes();
}

std::string CancelCommand::canonical_hash() const {
  return digest_hex(sha256(canonical_bytes()));
}

std::string VerifiedOrderEvent::canonical_bytes() const {
  Hasher h;
  h.named("tenant", tenant_id)
      .named("account", account_id)
      .named("chain", order_chain_id)
      .named("namespace", source_namespace)
      .named("event_id", event_id)
      .named("kind", order_event_kind_name(kind))
      .named("symbol", symbol)
      .named("side", side_name(side))
      .named("fill_units", fill_units)
      .named("verified_cum_units", verified_cum_units)
      .named("accepted_total_units", accepted_total_units)
      .named("limit_price_ticks", limit_price_ticks)
      .named("related_command", related_command_id);
  return h.canonical_bytes();
}

std::string VerifiedOrderEvent::canonical_hash() const {
  return digest_hex(sha256(canonical_bytes()));
}

std::string ReconcileEvidence::canonical_hash() const {
  Hasher h;
  h.named("tenant", tenant_id)
      .named("account", account_id)
      .named("chain", order_chain_id)
      .named("evidence", evidence_id)
      .named("namespace", source_namespace)
      .named("resolved_state", reservation_state_name(resolved_state))
      .named("verified_cum_units", verified_cum_units)
      .named("verified_accepted_total_units", verified_accepted_total_units)
      .named("clear_proposed", clear_proposed_branch ? std::string("1")
                                                     : std::string("0"));
  return h.hex();
}

std::string ReservationToken::signing_payload() const {
  Hasher h;
  h.named("reservation", reservation_id)
      .named("tenant", tenant_id)
      .named("account", account_id)
      .named("command", command_id)
      .named("canonical_hash", canonical_hash)
      .named("policy_epoch", policy_epoch)
      .named("policy_hash", policy_hash)
      .named("commit_revision", commit_revision)
      .named("fencing_token", fencing_token)
      .named("dispatch_deadline_mono", dispatch_deadline_mono);
  return h.canonical_bytes();
}

// ---------------------------------------------------------------------------
// RiskCoordinator
// ---------------------------------------------------------------------------

RiskCoordinator::RiskCoordinator(RiskStore& store, MarketFeed& feed, Clock& clock,
                                 FailpointRegistry& failpoints, Options options)
    : store_(store),
      feed_(feed),
      clock_(clock),
      failpoints_(failpoints),
      options_(std::move(options)),
      audit_(options_.tenant_id, options_.domain_id, options_.component_version) {
  rate_last_refill_ = clock_.mono_now();
}

DomainExposure RiskCoordinator::derive_exposure(
    const DomainExposure& positions_source,
    const std::vector<Reservation>& reservations) {
  DomainExposure out;

  // Positions are authoritative state from the fill ledger; they are carried
  // over verbatim. Only the pending (B/S) side is recomputed.
  for (const auto& [account, ae] : positions_source.accounts) {
    for (const auto& [symbol, se] : ae.symbols) {
      if (se.position_units == 0) continue;
      out.mutable_account(account).mutable_symbol(symbol).position_units =
          se.position_units;
    }
  }

  for (const Reservation& r : reservations) {
    if (!r.holds_budget()) continue;
    SymbolExposure& se = out.mutable_account(r.account_id).mutable_symbol(r.symbol);
    const std::int64_t remaining = r.risk_effective_remaining();
    if (r.side == Side::Buy) {
      add_saturating(se.pending_buy_units, remaining);
    } else {
      add_saturating(se.pending_sell_units, remaining);
    }
    add_saturating(se.working_orders, 1);
  }

  out.prune();
  return out;
}

DomainExposure RiskCoordinator::apply_reservation_delta(
    const DomainExposure& committed, const Reservation* before,
    const Reservation* after, std::int64_t position_delta) {
  const Reservation* any = after != nullptr ? after : before;
  if (any == nullptr) return committed;

  DomainExposure next = committed;
  SymbolExposure& row =
      next.mutable_account(any->account_id).mutable_symbol(any->symbol);

  // risk_effective_remaining() already returns 0 for a terminal reservation,
  // so a release is just another delta.
  const std::int64_t old_remaining =
      before != nullptr ? before->risk_effective_remaining() : 0;
  const std::int64_t new_remaining =
      after != nullptr ? after->risk_effective_remaining() : 0;
  const std::int64_t old_live =
      (before != nullptr && before->holds_budget()) ? 1 : 0;
  const std::int64_t new_live = (after != nullptr && after->holds_budget()) ? 1 : 0;

  std::int64_t* pending = any->side == Side::Buy ? &row.pending_buy_units
                                                 : &row.pending_sell_units;
  add_saturating(*pending, new_remaining - old_remaining);
  add_saturating(row.working_orders, new_live - old_live);
  add_saturating(row.position_units, position_delta);

  // A negative pending quantity would make the envelope formula meaningless,
  // so it is clamped here and caught as drift by cross_check_projection().
  if (*pending < 0) *pending = 0;
  if (row.working_orders < 0) row.working_orders = 0;

  next.prune();
  return next;
}

Status RiskCoordinator::check_identity_and_authz(const TenantId& tenant,
                                                 const AccountId& account,
                                                 const CommandId& command,
                                                 const OrderChainId& chain,
                                                 const Principal& who) {
  if (const Status s = validate_identifier(tenant, "tenant_id"); !s) return s;
  if (const Status s = validate_identifier(account, "account_id"); !s) return s;
  if (const Status s = validate_identifier(command, "command_id"); !s) return s;
  if (const Status s = validate_identifier(chain, "order_chain_id"); !s) return s;

  if (who.principal_id.empty()) {
    return Status::fail(Reason::Unauthenticated, "no principal");
  }
  // The domain only serves its own tenant; a request for another one is a
  // spoofing attempt, not a routing mistake.
  if (tenant != options_.tenant_id) {
    return Status::fail(Reason::TenantMismatch,
                        "domain serves tenant " + options_.tenant_id);
  }
  if (who.tenant_id != tenant) {
    return Status::fail(Reason::TenantMismatch, "principal tenant mismatch");
  }
  if (!who.has_role(Role::Trader)) {
    return Status::fail(Reason::Unauthorized, "trader role required");
  }
  // Account ownership is checked *before* the idempotency lookup, so a caller
  // cannot probe another tenant's command ids (§18.1).
  if (!who.owns_account(account)) {
    return Status::fail(Reason::Unauthorized, "principal does not own " + account);
  }
  return Status::success();
}

std::int64_t RiskCoordinator::unknown_backlog() const {
  // Maintained by the store, so the gate does not re-scan every reservation on
  // every admission.
  return store_.unresolved_reservation_count();
}

Status RiskCoordinator::check_gates(const RiskDomainState& domain,
                                    const RiskPolicy& policy,
                                    const AccountId& account,
                                    bool risk_increasing) const {
  // RI-32: nothing is admitted before recovery has verified the journal.
  if (!domain.admission_open) {
    return Status::fail(Reason::RecoveryIncomplete, "admission gate closed");
  }
  if (domain.quarantined) {
    return Status::fail(Reason::Quarantined, "risk domain quarantined");
  }
  if (store_.account_quarantined(account)) {
    return Status::fail(Reason::Quarantined, "account quarantined: " + account);
  }

  if (risk_increasing) {
    switch (domain.mode) {
      case DomainMode::Normal:
        break;
      case DomainMode::NoNew:
        return Status::fail(Reason::KillSwitchNoNew, "mode NO_NEW");
      case DomainMode::CancelOnly:
        return Status::fail(Reason::KillSwitchCancelOnly, "mode CANCEL_ONLY");
      case DomainMode::ReconOnly:
        return Status::fail(Reason::KillSwitchReconOnly, "mode RECON_ONLY");
    }
    // §17: an unbounded reconciliation backlog stops new risk rather than
    // silently expiring the holds that caused it.
    if (unknown_backlog() >= policy.queues.recon_cap) {
      return Status::fail(Reason::BacklogFull, "unknown/recon backlog at cap");
    }
  } else if (domain.mode == DomainMode::ReconOnly) {
    return Status::fail(Reason::KillSwitchReconOnly, "mode RECON_ONLY");
  }

  const AccountProfile* profile = policy.find_account(account);
  if (profile == nullptr) {
    return Status::fail(Reason::AccountNotFound, account);
  }
  if (!profile->enabled) {
    return Status::fail(Reason::AccountDisabled, account);
  }
  return Status::success();
}

Result<ValuationTable> RiskCoordinator::build_valuations(
    const MarketView& view, const RiskPolicy& policy,
    const DomainExposure& exposure, const SymbolId& extra,
    std::int64_t extra_limit_price_ticks, MonoNanos now_mono) const {
  // Every symbol carrying exposure, plus the candidate. A live reservation
  // always leaves a projection row behind (its working-order count), so
  // iterating the projection covers the same set the ledger would.
  std::set<SymbolId> symbols;
  for (const auto& [_, ae] : exposure.accounts) {
    for (const auto& [symbol, se] : ae.symbols) {
      (void)se;
      symbols.insert(symbol);
    }
  }
  if (!extra.empty()) symbols.insert(extra);

  ValuationTable table;
  for (const SymbolId& symbol : symbols) {
    const InstrumentSpec* spec = policy.find_instrument(symbol);
    if (spec == nullptr) {
      return Result<ValuationTable>::fail(Reason::UnknownInstrument, symbol);
    }
    if (!spec->enabled) {
      return Result<ValuationTable>::fail(Reason::InstrumentDisabled, symbol);
    }

    // The revaluation input is the highest *raw* limit price across the
    // candidate and every live reservation on this symbol. Using raw prices
    // keeps revaluation idempotent: feeding a previously buffered price back in
    // would push the bound up a little further on every pass.
    std::int64_t reference_price = 0;
    if (symbol == extra) reference_price = extra_limit_price_ticks;
    reference_price =
        std::max(reference_price, store_.max_live_limit_price(symbol));
    if (reference_price <= 0) {
      // No pinned price: fall back to the live ask, which build_valuation
      // bounds from above anyway.
      Result<Quote> q = view.quote(symbol, now_mono, spec->quote_ttl_ms);
      if (!q.ok()) return Result<ValuationTable>::fail(q.reason(), q.detail());
      reference_price = q.value().ask_ticks;
    }

    // The price collar belongs to the candidate command and is checked by the
    // caller against that command's own price. A symbol revalued only because
    // it still holds old exposure must not be rejected because the market has
    // since moved away from the old limit price.
    InstrumentSpec revalue_spec = *spec;
    revalue_spec.max_price_deviation_bps = 0;

    Result<Valuation> v =
        build_valuation(view, policy, revalue_spec, reference_price, now_mono);
    if (!v.ok()) return Result<ValuationTable>::fail(v.reason(), v.detail());
    table[symbol] = v.value();
  }
  return Result<ValuationTable>(table);
}

Result<MarginBasis> RiskCoordinator::margin_basis(const MarketView& view,
                                                  const RiskPolicy& policy,
                                                  const AccountId& account,
                                                  MonoNanos now_mono) const {
  const AccountProfile* profile = policy.find_account(account);
  if (profile == nullptr) {
    return Result<MarginBasis>::fail(Reason::AccountNotFound, account);
  }

  MarginBasis basis;
  if (!profile->use_equity_feed) {
    // §9.3: an independent conservative budget, so the decision does not
    // depend on an external equity feed at all.
    basis.budget_minor = profile->margin_budget_minor;
    basis.from_equity_feed = false;
    return Result<MarginBasis>(basis);
  }

  Result<EquitySnapshot> equity =
      view.equity(account, now_mono, policy.equity_ttl_ms);
  if (!equity.ok()) {
    return Result<MarginBasis>::fail(equity.reason(), equity.detail());
  }
  if (equity.value().currency != policy.account_currency.code) {
    return Result<MarginBasis>::fail(Reason::EquityCurrencyMismatch,
                                     equity.value().currency + " != " +
                                         policy.account_currency.code);
  }
  std::int64_t available = 0;
  if (!sub_i64(equity.value().equity_minor, equity.value().existing_margin_minor,
               available)) {
    return Result<MarginBasis>::fail(Reason::ArithmeticOverflow, "equity minus margin");
  }
  basis.budget_minor = available > 0 ? available : 0;
  basis.from_equity_feed = true;
  basis.equity_version = equity.value().version;
  return Result<MarginBasis>(basis);
}

std::optional<RiskResult> RiskCoordinator::replay_decision(
    const TenantId& tenant, const AccountId& account, const CommandId& command,
    const std::string& canonical_hash) const {
  std::optional<CommandRecord> existing = store_.find_command(tenant, account, command);
  if (!existing.has_value()) return std::nullopt;

  // RI-02: the same id with a different payload is a conflict, and the
  // original decision is left untouched.
  if (existing->canonical_hash != canonical_hash) {
    RiskResult r = make_result(RiskDecision::Conflict, Reason::CommandIdConflict,
                               "command " + command + " already decided with a "
                               "different canonical payload");
    r.reservation_id = existing->reservation_id;
    r.commit_revision = existing->commit_revision;
    return r;
  }

  RiskResult r;
  r.reason = existing->reason;
  r.detail = "idempotent replay of " + existing->decision;
  r.reservation_id = existing->reservation_id;
  r.commit_revision = existing->commit_revision;

  if (existing->decision == decision_label(RiskDecision::AllowCommitted)) {
    r.decision = RiskDecision::AllowCommitted;
    std::optional<Reservation> reservation =
        store_.find_reservation(existing->reservation_id);
    if (reservation.has_value()) {
      r.token = mint_token(*reservation, canonical_hash, store_.policy().hash(),
                           existing->commit_revision);
    }
  } else if (existing->decision == decision_label(RiskDecision::Quarantined)) {
    r.decision = RiskDecision::Quarantined;
  } else {
    r.decision = RiskDecision::DenyDefinitive;
  }
  return r;
}

ReservationToken RiskCoordinator::mint_token(const Reservation& reservation,
                                             const std::string& canonical_hash,
                                             const std::string& policy_hash,
                                             Revision commit_revision) const {
  ReservationToken token;
  token.reservation_id = reservation.reservation_id;
  token.tenant_id = reservation.tenant_id;
  token.account_id = reservation.account_id;
  token.command_id = reservation.command_id;
  token.canonical_hash = canonical_hash;
  token.policy_epoch = reservation.policy_version;
  token.policy_hash = policy_hash;
  token.commit_revision = commit_revision;
  token.fencing_token = options_.fencing_token;
  token.dispatch_deadline_mono =
      clock_.mono_now() + millis_to_nanos(options_.dispatch_ttl_ms);
  token.signature = digest_hex(
      hmac_sha256(options_.token_secret, token.signing_payload()));
  return token;
}

Status RiskCoordinator::verify_token(const ReservationToken& token) const {
  const std::string expected = digest_hex(
      hmac_sha256(options_.token_secret, token.signing_payload()));
  if (!constant_time_equals(expected, token.signature)) {
    return Status::fail(Reason::Unauthorized, "token signature mismatch");
  }
  if (token.tenant_id != options_.tenant_id) {
    return Status::fail(Reason::TenantMismatch, token.tenant_id);
  }

  std::optional<Reservation> reservation =
      store_.find_reservation(token.reservation_id);
  if (!reservation.has_value()) {
    return Status::fail(Reason::ReservationNotFound, token.reservation_id);
  }
  // A correctly signed token still has to match durable state: the signature
  // proves authorship, not that the hold survived (RI-22).
  if (reservation->account_id != token.account_id ||
      reservation->command_id != token.command_id) {
    return Status::fail(Reason::Unauthorized, "token does not match reservation");
  }
  const RiskDomainState domain = store_.domain_state();
  if (token.fencing_token < domain.fencing_token) {
    return Status::fail(Reason::FencingTokenStale,
                        "token fencing " + std::to_string(token.fencing_token) +
                            " < current " + std::to_string(domain.fencing_token));
  }
  if (token.policy_epoch != reservation->policy_version) {
    return Status::fail(Reason::PolicyVersionRegression, "policy epoch mismatch");
  }
  if (token.commit_revision > domain.revision) {
    return Status::fail(Reason::RevisionCasFailed, "token ahead of the ledger");
  }
  return Status::success();
}

SnapshotRef RiskCoordinator::snapshot_ref(const RiskPolicy& policy,
                                          const MarketView& view,
                                          const Valuation* valuation,
                                          Revision ledger) const {
  SnapshotRef ref;
  ref.policy = policy.version;
  ref.ledger = ledger;
  ref.quote = valuation != nullptr ? valuation->quote_version : view.max_quote_version();
  ref.fx = valuation != nullptr ? valuation->fx_version : view.max_fx_version();
  ref.instrument = policy.version;
  ref.equity = 0;
  return ref;
}

// ---------------------------------------------------------------------------
// reserve_new
// ---------------------------------------------------------------------------

RiskResult RiskCoordinator::reserve_new(const NewOrderCommand& command,
                                        const Principal& who) {
  const MonoNanos t_start = clock_.mono_now();
  metrics_.incr_labeled(metric::kAdmissionTotal, "op=new");

  // CW-01: before authentication. Nothing may be durable at this point.
  if (const Reason r = reach_control_failpoint(failpoints_, Failpoint::BeforeAuthz);
      r != Reason::Ok) {
    return refuse(r, "failpoint before authz");
  }

  if (const Status s = check_identity_and_authz(command.tenant_id, command.account_id,
                                                command.command_id,
                                                command.order_chain_id, who);
      !s) {
    metrics_.incr_labeled(metric::kAdmissionTotal,
                          std::string("decision=DENY,reason=") +
                              std::string(reason_name(s.reason())));
    return refuse(s);
  }
  if (const Status s = validate_symbol(command.symbol); !s) return refuse(s);

  // Bounded concurrency (§17): over the cap the request is told BUSY rather
  // than queued without limit or half-committed.
  const std::int64_t cap = options_.admission_concurrency_cap > 0
                               ? options_.admission_concurrency_cap
                               : store_.policy().queues.admission_cap;
  const std::int64_t in_flight = admissions_in_flight_.fetch_add(1) + 1;
  struct Release {
    std::atomic<std::int64_t>* counter;
    ~Release() { counter->fetch_sub(1); }
  } release{&admissions_in_flight_};
  metrics_.set_gauge(std::string(metric::kQueueDepth) + "{lane=admission}", in_flight);
  if (in_flight > cap) {
    metrics_.incr_labeled(metric::kAdmissionTotal, "decision=DENY,reason=QUEUE_FULL");
    return refuse(Reason::QueueFull,
                  "admission concurrency " + std::to_string(in_flight) + " > " +
                      std::to_string(cap));
  }

  const std::string canonical_hash = command.canonical_hash();

  // CW-02: after authz, before the idempotency lookup.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterAuthzBeforeIdempotency);
      r != Reason::Ok) {
    return refuse(r, "failpoint before idempotency");
  }

  std::lock_guard<std::mutex> domain_lock(domain_mu_);

  // §12.1 step 2: the lookup happens on the owner's committed state.
  if (std::optional<RiskResult> replayed = replay_decision(
          command.tenant_id, command.account_id, command.command_id, canonical_hash);
      replayed.has_value()) {
    metrics_.incr_labeled(metric::kAdmissionTotal, "decision=REPLAY");
    return *replayed;
  }

  const RiskDomainState domain = store_.domain_state();
  const RiskPolicy policy = store_.policy();

  if (const Status s = check_gates(domain, policy, command.account_id, true); !s) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, s.reason(), s.detail(),
                         policy, domain, "NewDenied");
  }

  // Rate scope (§7.1).
  if (policy.limits.max_admissions_per_second > 0) {
    std::lock_guard<std::mutex> rate_lock(rate_mu_);
    const MonoNanos now = clock_.mono_now();
    const double elapsed_s =
        static_cast<double>(now - rate_last_refill_) / 1'000'000'000.0;
    rate_last_refill_ = now;
    const double capacity =
        static_cast<double>(policy.limits.max_admissions_per_second);
    rate_tokens_ = std::min(capacity, rate_tokens_ + elapsed_s * capacity);
    if (rate_tokens_ < 1.0) {
      return refuse(Reason::LimitRate, "admission rate limit");
    }
    rate_tokens_ -= 1.0;
  }

  if (!policy.allows_order_type(command.order_type)) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash,
                         Reason::UnsupportedOrderType, "order type", policy, domain,
                         "NewDenied");
  }
  if (!policy.allows_time_in_force(command.time_in_force)) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash,
                         Reason::UnsupportedTimeInForce, "time in force", policy,
                         domain, "NewDenied");
  }

  const InstrumentSpec* spec = policy.find_instrument(command.symbol);
  if (spec == nullptr) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::UnknownInstrument,
                         command.symbol, policy, domain, "NewDenied");
  }
  if (!spec->enabled) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::InstrumentDisabled,
                         command.symbol, policy, domain, "NewDenied");
  }

  // §12.1 step 3: instrument rules.
  if (command.total_units <= 0) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::QtyBelowMin,
                         "quantity must be > 0", policy, domain, "NewDenied");
  }
  if (command.total_units < spec->min_qty_base_units) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::QtyBelowMin,
                         std::to_string(command.total_units) + " < " +
                             std::to_string(spec->min_qty_base_units),
                         policy, domain, "NewDenied");
  }
  if (command.total_units > spec->max_qty_base_units) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::QtyAboveMax,
                         std::to_string(command.total_units) + " > " +
                             std::to_string(spec->max_qty_base_units),
                         policy, domain, "NewDenied");
  }
  if (command.total_units % spec->quantity_step_base_units != 0) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::QtyStepViolation,
                         "quantity is not a multiple of " +
                             std::to_string(spec->quantity_step_base_units),
                         policy, domain, "NewDenied");
  }
  if (command.limit_price_ticks <= 0) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::PriceNonPositive,
                         "limit price must be > 0", policy, domain, "NewDenied");
  }

  // A New must start a fresh chain; reusing one would silently merge two
  // orders into a single reservation.
  if (store_.find_chain(command.tenant_id, command.account_id,
                        command.order_chain_id)
          .has_value()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, Reason::ChainAlreadyExists,
                         command.order_chain_id, policy, domain, "NewDenied");
  }

  const MarketView view = feed_.snapshot();
  const DomainExposure committed_exposure = store_.exposure();

  // CW-03: after reading policy/quote/equity, before the exposure maths.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterSnapshotBeforeMath);
      r != Reason::Ok) {
    return refuse(r, "failpoint before exposure maths");
  }

  const MonoNanos t_math = clock_.mono_now();

  // The price collar applies to this command's own limit price (SS9.2), so it
  // is checked here rather than inside the shared revaluation pass.
  {
    Result<Quote> quote = view.quote(command.symbol, t_math, spec->quote_ttl_ms);
    if (!quote.ok()) {
      if (quote.reason() == Reason::QuoteStale) {
        metrics_.incr_labeled(metric::kQuoteStale, "symbol=" + command.symbol);
      }
      return record_denial(command.tenant_id, command.account_id, command.command_id,
                           command.symbol, canonical_hash, quote.reason(),
                           quote.detail(), policy, domain, "NewDenied");
    }
    if (const Status collar = check_price_collar(command.limit_price_ticks,
                                                 quote.value(),
                                                 spec->max_price_deviation_bps);
        !collar) {
      return record_denial(command.tenant_id, command.account_id, command.command_id,
                           command.symbol, canonical_hash, collar.reason(),
                           collar.detail(), policy, domain, "NewDenied");
    }
  }

  Result<ValuationTable> valuations =
      build_valuations(view, policy, committed_exposure, command.symbol,
                       command.limit_price_ticks, t_math);
  if (!valuations.ok()) {
    if (valuations.reason() == Reason::QuoteStale) {
      metrics_.incr_labeled(metric::kQuoteStale, "symbol=" + command.symbol);
    }
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, valuations.reason(),
                         valuations.detail(), policy, domain, "NewDenied");
  }
  const Valuation& candidate_valuation = valuations.value().at(command.symbol);

  // Build the candidate reservation and derive the whole projection from the
  // reservation set. Deriving rather than incrementing is what makes a double
  // count structurally impossible (RI-07).
  Reservation candidate;
  candidate.reservation_id =
      "RSV-" + command.account_id + "-" + command.order_chain_id;
  candidate.tenant_id = command.tenant_id;
  candidate.domain_id = options_.domain_id;
  candidate.account_id = command.account_id;
  candidate.order_chain_id = command.order_chain_id;
  candidate.command_id = command.command_id;
  candidate.symbol = command.symbol;
  candidate.side = command.side;
  candidate.accepted_total_units = command.total_units;
  candidate.cum_units = 0;
  candidate.accepted_limit_price_ticks = command.limit_price_ticks;
  candidate.state = ReservationState::Committed;
  candidate.policy_version = policy.version;
  candidate.policy_hash = policy.hash();
  candidate.accepted_valuation = candidate_valuation;

  const DomainExposure candidate_exposure =
      apply_reservation_delta(committed_exposure, nullptr, &candidate, 0);

  Result<DomainMetrics> metrics =
      evaluate_exposure(candidate_exposure, policy, valuations.value());
  if (!metrics.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, metrics.reason(),
                         metrics.detail(), policy, domain, "NewDenied");
  }

  Result<std::int64_t> order_notional = notional_minor(
      command.total_units, *spec, candidate_valuation,
      policy.account_currency.minor_scale);
  if (!order_notional.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, order_notional.reason(),
                         order_notional.detail(), policy, domain, "NewDenied");
  }

  Result<MarginBasis> basis =
      margin_basis(view, policy, command.account_id, t_math);
  if (!basis.ok()) {
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, basis.reason(),
                         basis.detail(), policy, domain, "NewDenied");
  }

  LimitCheckInput check;
  check.account_id = command.account_id;
  check.symbol = command.symbol;
  check.candidate_order_notional_minor = order_notional.value();
  check.candidate_chain_units = command.total_units;
  check.margin_basis = basis.value();

  if (const Status s = check_limits(metrics.value(), policy, check); !s) {
    metrics_.incr_labeled(metric::kLimitBreach,
                          std::string("reason=") + std::string(reason_name(s.reason())));
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, s.reason(), s.detail(),
                         policy, domain, "NewDenied");
  }

  // CW-04: after the maths, before the domain lock/commit. PB-04 advances the
  // virtual clock here so the TTL expires between evaluation and commit.
  if (const Reason r = reach_control_failpoint(
          failpoints_, Failpoint::AfterMathBeforeDomainLock);
      r != Reason::Ok) {
    return refuse(r, "failpoint before domain lock");
  }

  // §12.1 step 6: the snapshot must still be valid at commit time. Re-deriving
  // it and comparing digests catches both expiry and an adverse revaluation.
  const MonoNanos t_commit = clock_.mono_now();
  Result<ValuationTable> recheck =
      build_valuations(view, policy, committed_exposure, command.symbol,
                       command.limit_price_ticks, t_commit);
  if (!recheck.ok()) {
    if (recheck.reason() == Reason::QuoteStale) {
      metrics_.incr_labeled(metric::kQuoteStale, "symbol=" + command.symbol);
    }
    return record_denial(command.tenant_id, command.account_id, command.command_id,
                         command.symbol, canonical_hash, recheck.reason(),
                         "stale at commit: " + recheck.detail(), policy, domain,
                         "NewDenied");
  }
  for (const auto& [symbol, valuation] : valuations.value()) {
    const auto it = recheck.value().find(symbol);
    if (it == recheck.value().end() || it->second.digest() != valuation.digest()) {
      return record_denial(command.tenant_id, command.account_id, command.command_id,
                           command.symbol, canonical_hash, Reason::QuoteStale,
                           "valuation changed between evaluation and commit for " +
                               symbol,
                           policy, domain, "NewDenied");
    }
  }

  // --- stage the single transaction --------------------------------------
  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;

  CommandRecord record;
  record.tenant_id = command.tenant_id;
  record.account_id = command.account_id;
  record.command_id = command.command_id;
  record.canonical_hash = canonical_hash;
  record.decision = decision_label(RiskDecision::AllowCommitted);
  record.reason = Reason::Ok;
  record.reservation_id = candidate.reservation_id;
  record.policy_version = policy.version;
  request.data.commands.push_back(record);

  request.data.reservations.push_back(candidate);
  request.data.chain_index.emplace_back(
      chain_key(command.tenant_id, command.account_id, command.order_chain_id),
      candidate.reservation_id);
  request.data.exposure = candidate_exposure;

  AuditEvent audit = audit_.begin("NewCommitted", {});
  audit.command_id = command.command_id;
  audit.account_id = command.account_id;
  audit.symbol = command.symbol;
  audit.reservation_id = candidate.reservation_id;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, view, &candidate_valuation, domain.revision);
  audit.payload_hash = canonical_hash;
  audit.reason = Reason::Ok;
  audit.actor = who.principal_id;
  audit.note = "order_notional_minor=" + std::to_string(order_notional.value());
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
  intent.kind = "NEW";
  intent.symbol = command.symbol;
  intent.side = command.side;
  intent.total_units = command.total_units;
  intent.limit_price_ticks = command.limit_price_ticks;
  intent.canonical_hash = canonical_hash;
  request.data.outbox_inserts.push_back(intent);

  RiskResult result = commit_or_map(request, record.decision, candidate.reservation_id,
                                    &command, canonical_hash, policy.hash(),
                                    &candidate);

  const MonoNanos t_end = clock_.mono_now();
  metrics_.admission_commit_seconds().observe_micros((t_end - t_start) / 1000);
  metrics_.incr_labeled(metric::kAdmissionTotal,
                        std::string("decision=") +
                            std::string(risk_decision_name(result.decision)));
  return result;
}

RiskResult RiskCoordinator::commit_or_map(CommitRequest& request,
                                          const std::string& label,
                                          const ReservationId& reservation_id,
                                          const NewOrderCommand* /*new_cmd*/,
                                          const std::string& canonical_hash,
                                          const std::string& policy_hash,
                                          const Reservation* reservation) {
  const CommitOutcome outcome = store_.commit(request);
  metrics_.incr_labeled(metric::kCommitOutcome,
                        std::string("outcome=") +
                            std::string(commit_outcome_name(outcome)));

  switch (outcome) {
    case CommitOutcome::Committed: {
      RiskResult r = make_result(RiskDecision::AllowCommitted, Reason::Ok, label);
      r.reservation_id = reservation_id;
      r.commit_revision = store_.domain_state().revision;
      // §12.1 step 7: the token is minted only after a confirmed commit.
      if (reservation != nullptr) {
        Reservation committed = *reservation;
        committed.revision = r.commit_revision;
        r.token = mint_token(committed, canonical_hash, policy_hash, r.commit_revision);
      }
      return r;
    }
    case CommitOutcome::RolledBack:
      // Nothing durable: safe to retry the whole transaction with the same
      // command id. Still a HOLD, not a DENY, because the caller must not
      // report "definitely not sent" without proof it was never dispatched.
      return refuse(Reason::PersistenceCommitFailed, "commit rolled back");
    case CommitOutcome::UnknownButDurable:
      metrics_.incr_labeled(metric::kUnknownOutcome, "scope=commit");
      return refuse(Reason::PersistenceCommitUnknown,
                    "commit acknowledgement lost; query by command id");
    case CommitOutcome::Fenced:
      metrics_.incr(metric::kFencingReject);
      return refuse(Reason::FencingTokenStale, "writer fenced");
  }
  return refuse(Reason::InternalInvariant, "unmapped commit outcome");
}

RiskResult RiskCoordinator::record_denial(
    const TenantId& tenant, const AccountId& account, const CommandId& command,
    const SymbolId& symbol, const std::string& canonical_hash, Reason reason,
    std::string detail, const RiskPolicy& policy, const RiskDomainState& domain,
    const std::string& event_type) {
  // A retryable reason is not a decision: it must stay a HOLD and must not be
  // written down as a definitive answer, or a later retry would be told "no"
  // for a command that might already be at the LP (§2.3).
  if (reason_is_retryable(reason)) {
    return refuse(reason, std::move(detail));
  }

  CommitRequest request;
  request.fencing_token = options_.fencing_token;
  request.expected_revision = domain.revision;

  CommandRecord record;
  record.tenant_id = tenant;
  record.account_id = account;
  record.command_id = command;
  record.canonical_hash = canonical_hash;
  record.decision = decision_label(RiskDecision::DenyDefinitive);
  record.reason = reason;
  record.policy_version = policy.version;
  request.data.commands.push_back(record);

  AuditEvent audit = audit_.begin(event_type, {});
  audit.command_id = command;
  audit.account_id = account;
  audit.symbol = symbol;
  audit.policy_version = policy.version;
  audit.snapshots = snapshot_ref(policy, MarketView{}, nullptr, domain.revision);
  audit.payload_hash = canonical_hash;
  audit.reason = reason;
  audit.note = detail;
  audit.commit_utc = clock_.utc_now();
  audit.commit_mono = clock_.mono_now();
  request.data.audit.push_back(audit);

  const CommitOutcome outcome = store_.commit(request);
  if (outcome != CommitOutcome::Committed) {
    // The denial itself could not be recorded. Report a hold: an unrecorded
    // decision must not be presented as durable (RI-24).
    return refuse(Reason::PersistenceCommitFailed,
                  "could not record denial (" + std::string(reason_name(reason)) +
                      "): " + std::string(commit_outcome_name(outcome)));
  }

  RiskResult r = make_result(RiskDecision::DenyDefinitive, reason, std::move(detail));
  r.commit_revision = store_.domain_state().revision;
  return r;
}

Result<DomainMetrics> RiskCoordinator::current_metrics() const {
  const RiskPolicy policy = store_.policy();
  const MarketView view = feed_.snapshot();
  const DomainExposure exposure = store_.exposure();
  Result<ValuationTable> valuations =
      build_valuations(view, policy, exposure, {}, 0, clock_.mono_now());
  if (!valuations.ok()) {
    return Result<DomainMetrics>::fail(valuations.reason(), valuations.detail());
  }
  return evaluate_exposure(exposure, policy, valuations.value());
}

Status RiskCoordinator::cross_check_projection() const {
  const DomainExposure stored = store_.exposure();
  const DomainExposure derived = derive_exposure(stored, store_.reservations());
  if (stored.state_hash() != derived.state_hash()) {
    return Status::fail(Reason::InternalInvariant,
                        "stored projection drifted from the reservation ledger: "
                        "stored=" +
                            stored.state_hash() + " derived=" +
                            derived.state_hash());
  }
  return Status::success();
}

}  // namespace risk
