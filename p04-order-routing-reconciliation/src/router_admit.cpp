// Command admission and outbound dispatch (design doc 8 and 10).
#include <algorithm>

#include "router/numeric.hpp"
#include "router/router.hpp"

namespace router {

std::string_view to_string(RouterMode m) {
    switch (m) {
        case RouterMode::Normal: return "NORMAL";
        case RouterMode::Degraded: return "DEGRADED";
        case RouterMode::Halt: return "HALT";
    }
    return "UNKNOWN";
}

Router::Router(Config& cfg, Store& store, p01::SessionEngine& session, p05::IRiskAdmission& risk,
               Metrics& metrics, FailpointRegistry& fp, const IClock& clock)
    : cfg_(cfg),
      store_(store),
      session_(session),
      risk_(risk),
      metrics_(metrics),
      fp_(fp),
      clock_(clock),
      normalizer_(cfg, store) {
    // Bounded label sets, so a hostile ClOrdID can never become a metric series
    // (design doc 17.2, OR-Q-11).
    metrics_.allow_label(metric::kCommandReceived, {"New", "Cancel", "Replace", "StatusQuery"});
    metrics_.allow_label(metric::kHoldTotal,
                         {"AmbiguousSend", "AckTimeout", "ExecIdConflict", "DuplicateExecIdChain",
                          "OrphanReport", "SecurityQuarantine", "IdentityConflict",
                          "SemanticConflict", "SchemaViolation", "UnsupportedEvent",
                          "SnapshotMismatch", "HistoryRequired", "CapabilityMissing",
                          "ReconBudgetExhausted", "StateRegression", "ReplaceAfterTerminal",
                          "PendingPolicy", "StoreFailClosed", "OrphanQuotaExceeded",
                          "OperatorRequested"});
    metrics_.allow_label(metric::kOrphanReports, {"no_match", "chain_uncommitted", "quota"});
}

void Router::alert(std::string text) {
    // Rate limited per distinct reason: OR-Q-19 floods the same malformed report
    // a hundred thousand times and expects the counters to stay accurate while
    // the log stops repeating itself.
    const std::size_t seen = ++log_suppression_[text];
    if (seen <= cfg_.limits.log_rate_limit_per_reason)
        alerts_.push_back(std::move(text));
    else
        metrics_.inc(metric::kLogSuppressed);
}

void Router::set_mode(RouterMode m, std::string reason) {
    if (m == mode_ && reason == halt_reason_) return;
    mode_ = m;
    halt_reason_ = reason;
    AuditRecord rec;
    rec.kind = m == RouterMode::Normal ? AuditKind::StateResumed : AuditKind::StateHalted;
    rec.actor = "system";
    rec.subject = "router";
    rec.reason = reason;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    store_.audit().append(rec);
    if (m != RouterMode::Normal) alert("mode=" + std::string(to_string(m)) + " reason=" + reason);
}

Result<bool, RoutingError> Router::boot() {
    using R = Result<bool, RoutingError>;

    // Step 1: exclusive ownership. A second router instance against the same
    // store must not be able to issue a second original send (OR-E-18, OR-R-12).
    auto own = store_.acquire_ownership();
    if (own.is_err()) {
        set_mode(RouterMode::Halt, "ownership");
        return R::err(RoutingError::StoreUnavailable);
    }

    // Step 2: schema, integrity and configuration, before anything is sent.
    auto health = store_.check_health();
    if (health.is_err()) {
        AuditRecord rec;
        rec.kind = AuditKind::IntegrityFailure;
        rec.actor = "system";
        rec.subject = "store";
        rec.reason = std::string(to_string(health.error()));
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        store_.audit().append(rec);
        set_mode(RouterMode::Halt, "store:" + std::string(to_string(health.error())));
        return R::err(RoutingError::StoreUnavailable);
    }
    auto cfg_ok = cfg_.validate();
    if (cfg_ok.is_err()) {
        set_mode(RouterMode::Halt, "config");
        return R::err(cfg_ok.error());
    }

    // Step 3 to 5: rebuild pending work and classify anything unresolved. A
    // command that was mid-flight is ambiguous until proven otherwise; it is
    // never silently retried as a new business request (R-I16).
    std::size_t reclassified = 0;
    Txn txn = store_.begin();
    for (auto& [key, intent] : store_.image().intents) {
        (void)key;
        if (intent.delivery == DeliveryState::Dispatching ||
            intent.delivery == DeliveryState::OriginalSeqBound) {
            CommandIntent updated = intent;
            // Ask P01 whether a binding exists. This is the lookup design doc
            // 15.3 step 3 insists on: never invent a new sequence.
            auto binding = session_.lookup_binding(intent.command_id);
            if (binding) {
                updated.original_seq = binding->original_seq;
                updated.delivery = binding->written ? DeliveryState::UnknownTransport
                                                    : DeliveryState::AcceptedDurable;
            } else {
                updated.delivery = DeliveryState::AcceptedDurable;
            }
            txn.put_intent(updated);
            ++reclassified;
        }
    }
    if (reclassified > 0) {
        AuditRecord rec;
        rec.kind = AuditKind::StateResumed;
        rec.actor = "system";
        rec.subject = "recovery";
        rec.reason = "reclassified_pending";
        rec.detail = std::to_string(reclassified) + " commands re-examined";
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        txn.add_audit(rec);
        auto c = txn.commit();
        if (c.is_err()) {
            set_mode(RouterMode::Halt, "recovery_commit");
            return R::err(RoutingError::StoreUnavailable);
        }
    }

    // Step 6 and 7: a clean boot is Normal, but an unresolved case keeps the
    // router in Degraded so a batch of new orders does not go out immediately.
    if (!store_.open_recon_cases().empty())
        set_mode(RouterMode::Degraded, "open_reconciliation");
    else
        set_mode(RouterMode::Normal, "boot");
    return R::ok(true);
}

Hash Router::canonical_digest(const CommandEnvelope& cmd, const CommandId& command_id) const {
    // Strict canonical form: a fixed field set in a fixed order, each length
    // prefixed. Two requests differ if and only if their digests differ, which
    // is what makes IDEMPOTENCY_CONFLICT in OR-A-03 and OR-A-04 exact.
    Sha256 s;
    s.field(command_id.str());
    s.field(cmd.principal.tenant.str());
    s.field(cmd.account.str());
    s.field(to_string(cmd.kind));
    s.field(cmd.symbol);
    s.field(to_string(cmd.side));
    s.field(cmd.qty_text);
    s.field(cmd.price_text);
    s.field(to_string(cmd.ord_type));
    s.field(to_string(cmd.tif));
    s.field(cmd.chain_id ? cmd.chain_id->str() : "");
    return s.finish();
}

bool Router::rate_limit_ok(const AccountId& account) {
    auto& bucket = rate_buckets_[account.str()];
    const MonoMicros now = clock_.mono_now();
    if (now - bucket.first >= 1000000) {
        bucket.first = now;
        bucket.second = 0;
    }
    ++bucket.second;
    return bucket.second <= cfg_.limits.admission_rate_per_account_per_sec;
}

void Router::record_reject(const CommandEnvelope& cmd, const CommandId& id, RoutingError e) {
    metrics_.inc(metric::kCommandRejected);
    AuditRecord rec;
    rec.kind = AuditKind::CommandRejected;
    rec.actor = cmd.principal.actor.str();
    rec.subject = id.str();
    rec.reason = std::string(to_string(e));
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    // Audit of a refusal is best effort against the in-memory log: nothing was
    // committed, so there is no transaction to attach it to.
    store_.audit().append(rec);
}

Result<const Route*, RoutingError> Router::resolve_route(const AccountId& account) {
    using R = Result<const Route*, RoutingError>;
    const Route* route = cfg_.route_for(account);
    if (route == nullptr) return R::err(RoutingError::RouteUnavailable);
    // The session the route names must be the session we are attached to,
    // otherwise we would be sending one peer's order to another (OR-B-06).
    const std::string expected = route->sender_comp_id + ":" + route->target_comp_id + ":" +
                                 route->session_qualifier;
    if (session_.identity().key() != expected) return R::err(RoutingError::ConfigInvalid);
    return R::ok(route);
}

Result<bool, RoutingError> Router::check_common_admission(const CommandEnvelope& cmd) {
    using R = Result<bool, RoutingError>;

    // Authentication and authorisation first: nothing else may run for a caller
    // that does not own the account, so no reservation and no routing decision
    // can leak (OR-A-05, OR-A-12, OR-Q-01, OR-Q-04).
    if (!may_submit_orders(cmd.principal)) return R::err(RoutingError::AuthzDenied);
    if (!cfg_.account_belongs_to(cmd.principal.tenant, cmd.account))
        return R::err(RoutingError::AuthzDenied);

    auto cfg_ok = cfg_.validate();
    if (cfg_ok.is_err()) return R::err(cfg_ok.error());
    if (!cfg_.entitled(cmd.account)) return R::err(RoutingError::EntitlementRevoked);
    if (!rate_limit_ok(cmd.account)) return R::err(RoutingError::Overloaded);

    // Bounded queues. Design doc 16.1: the refusal is explicit, and a durable
    // command that is already accepted is never dropped to make room.
    if (store_.pending_outbox_count() >= cfg_.limits.global_command_queue)
        return R::err(RoutingError::Overloaded);
    if (store_.quarantine_depth() >= cfg_.limits.orphan_quota)
        return R::err(RoutingError::Halted);
    return R::ok(true);
}

Result<AcceptedIntent, RoutingError> Router::submit(const CommandEnvelope& cmd) {
    using R = Result<AcceptedIntent, RoutingError>;
    const CommandId command_id(cmd.client_command_id);
    metrics_.inc(metric::kCommandReceived, std::string(to_string(cmd.kind)));

    const Hash digest = canonical_digest(cmd, command_id);

    // Idempotency before anything else that has an effect. Design doc 8.1 step
    // 3: same digest returns the stored snapshot, a different digest is a
    // conflict with zero side effects.
    if (const CommandIntent* existing = store_.find_intent(cmd.principal.tenant, command_id)) {
        // The record is scoped by tenant, so T2 replaying T1's command id cannot
        // read T1's state (OR-Q-04). Authorisation still applies.
        if (!cfg_.account_belongs_to(cmd.principal.tenant, cmd.account)) {
            record_reject(cmd, command_id, RoutingError::AuthzDenied);
            return R::err(RoutingError::AuthzDenied);
        }
        if (existing->request_hash != digest) {
            record_reject(cmd, command_id, RoutingError::IdempotencyConflict);
            return R::err(RoutingError::IdempotencyConflict);
        }
        metrics_.inc(metric::kCommandDuplicate);
        AuditRecord rec;
        rec.kind = AuditKind::CommandDuplicate;
        rec.actor = cmd.principal.actor.str();
        rec.subject = command_id.str();
        rec.reason = "idempotent_replay";
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        store_.audit().append(rec);

        AcceptedIntent out;
        out.command_id = existing->command_id;
        out.chain_id = existing->chain_id;
        out.cl_ord_id = existing->cl_ord_id;
        out.route = existing->lp;
        out.delivery = existing->delivery;
        out.business = existing->business;
        out.idempotent_replay = true;
        return R::ok(out);
    }

    auto common = check_common_admission(cmd);
    if (common.is_err()) {
        record_reject(cmd, command_id, common.error());
        return R::err(common.error());
    }

    // A halted router accepts no New and no Replace. A cancel reduces exposure
    // and keeps its own path, but only while the store can still make it
    // durable (design doc 16.2, OR-A-19, OR-R-10).
    if (mode_ == RouterMode::Halt && cmd.kind != CommandKind::Cancel) {
        record_reject(cmd, command_id, RoutingError::Halted);
        return R::err(RoutingError::Halted);
    }

    switch (cmd.kind) {
        case CommandKind::New: return admit_new(cmd, command_id, digest);
        case CommandKind::Cancel: return admit_cancel(cmd, command_id, digest);
        case CommandKind::Replace: return admit_replace(cmd, command_id, digest);
        case CommandKind::StatusQuery:
            // A client does not get to ask for a 35=H directly; the
            // reconciliation coordinator owns that decision and the capability
            // gate (design doc 8.2, OR-N-01).
            record_reject(cmd, command_id, RoutingError::CapabilityUnsupported);
            return R::err(RoutingError::CapabilityUnsupported);
    }
    return R::err(RoutingError::ValidationError);
}

namespace {

// Validates the user's quantity and price against the symbol profile. Every
// refusal is a distinct NumError so the A and B series can assert on the exact
// reason rather than on "it failed".
struct SizedOrder {
    std::int64_t qty = 0;
    std::int64_t price_ticks = 0;
};

Result<SizedOrder, RoutingError> size_order(const SymbolProfile& prof, const CommandEnvelope& cmd) {
    using R = Result<SizedOrder, RoutingError>;
    SizedOrder out;

    auto q = parse_decimal_ticks(cmd.qty_text, 0, 18);
    if (q.is_err()) return R::err(RoutingError::ValidationError);
    out.qty = q.value();
    if (out.qty <= 0) return R::err(RoutingError::ValidationError);
    if (out.qty < prof.min_qty || out.qty > prof.max_qty)
        return R::err(RoutingError::ValidationError);
    // OR-B-14: a quantity off the lot step is refused, never rounded.
    if (out.qty % prof.qty_step != 0) return R::err(RoutingError::ValidationError);

    if (cmd.ord_type == OrdType::Limit) {
        auto p = parse_decimal_ticks(cmd.price_text, prof.price_scale, 18);
        if (p.is_err()) return R::err(RoutingError::ValidationError);
        out.price_ticks = p.value();
        if (out.price_ticks <= 0) return R::err(RoutingError::ValidationError);
        if (out.price_ticks > prof.max_price_ticks) return R::err(RoutingError::ValidationError);
        if (out.price_ticks % prof.tick_ticks != 0) return R::err(RoutingError::ValidationError);
    }
    return R::ok(out);
}

}  // namespace

namespace {

// CW-19: the commit may have landed even though the caller was told it failed.
// Design doc 15.1 is explicit that a lost acknowledgement must not be read as a
// rollback, so the authoritative key is re-read before anything is undone.
bool commit_actually_landed(const Store& store, const TenantId& tenant,
                            const CommandId& command_id, const Hash& digest) {
    const CommandIntent* stored = store.find_intent(tenant, command_id);
    return stored != nullptr && stored->request_hash == digest;
}

AcceptedIntent accepted_from(const CommandIntent& in) {
    AcceptedIntent out;
    out.command_id = in.command_id;
    out.chain_id = in.chain_id;
    out.cl_ord_id = in.cl_ord_id;
    out.route = in.lp;
    out.delivery = in.delivery;
    out.business = in.business;
    return out;
}

}  // namespace

Result<AcceptedIntent, RoutingError> Router::admit_new(const CommandEnvelope& cmd,
                                                       const CommandId& command_id,
                                                       const Hash& digest) {
    using R = Result<AcceptedIntent, RoutingError>;

    if (!cfg_.ord_type_allowed(cmd.ord_type)) {
        record_reject(cmd, command_id, RoutingError::UnsupportedOrdType);
        return R::err(RoutingError::UnsupportedOrdType);
    }
    if (!cfg_.tif_allowed(cmd.tif)) {
        record_reject(cmd, command_id, RoutingError::UnsupportedTif);
        return R::err(RoutingError::UnsupportedTif);
    }
    auto wire = cfg_.resolve_symbol_alias(cmd.symbol);
    if (wire.is_err()) {
        record_reject(cmd, command_id, wire.error());
        return R::err(wire.error());
    }
    const SymbolProfile* prof = cfg_.symbol(wire.value());
    if (prof == nullptr) {
        record_reject(cmd, command_id, RoutingError::UnsupportedSymbol);
        return R::err(RoutingError::UnsupportedSymbol);
    }
    auto sized = size_order(*prof, cmd);
    if (sized.is_err()) {
        record_reject(cmd, command_id, sized.error());
        return R::err(sized.error());
    }

    auto route = resolve_route(cmd.account);
    if (route.is_err()) {
        record_reject(cmd, command_id, route.error());
        return R::err(route.error());
    }

    // Risk keys its reservation by the chain, so the chain has to exist before
    // the question is asked. Peeking does not consume the counter: the durable
    // allocation still happens below, after the reservation is in hand.
    const OrderChainId chain_id(store_.peek_chain_id());

    // Risk before the queue. UNAVAILABLE fails closed for a New; it is never
    // read as an approval (OR-A-15, OR-A-16).
    p05::RiskRequest rr;
    rr.command_id = command_id;
    rr.tenant = cmd.principal.tenant;
    rr.account = cmd.account;
    rr.wire_symbol = wire.value();
    rr.side = cmd.side;
    rr.qty = sized.value().qty;
    rr.price_ticks = sized.value().price_ticks;
    rr.increases_exposure = true;
    rr.order_chain_id = chain_id;
    rr.price_scale = prof->price_scale;
    rr.total_units_after = sized.value().qty;
    const p05::RiskDecision decision = risk_.reserve(rr);
    if (decision.verdict == p05::Verdict::Reject) {
        record_reject(cmd, command_id, RoutingError::RiskRejected);
        return R::err(RoutingError::RiskRejected);
    }
    if (decision.verdict == p05::Verdict::Unavailable) {
        record_reject(cmd, command_id, RoutingError::RiskUnavailable);
        return R::err(RoutingError::RiskUnavailable);
    }

    // Identifiers come from durable counters so a restart cannot reissue one.
    const std::uint64_t seq = store_.image().next_cl_ord_seq;
    const ClientOrderId cl_ord_id(ClientOrderId("O-" + std::to_string(seq)));

    CommandIntent intent;
    intent.tenant = cmd.principal.tenant;
    intent.command_id = command_id;
    intent.chain_id = chain_id;
    intent.kind = CommandKind::New;
    intent.request_hash = digest;
    intent.cl_ord_id = cl_ord_id;
    intent.account = cmd.account;
    intent.lp = route.value()->lp;
    intent.session_key = session_.identity().key();
    intent.route_config_version = cfg_.route_config_version;
    intent.profile_version = cfg_.profile_version;
    intent.wire_symbol = wire.value();
    intent.side = cmd.side;
    intent.qty = sized.value().qty;
    intent.price_ticks = sized.value().price_ticks;
    intent.price_scale = prof->price_scale;
    intent.ord_type = cmd.ord_type;
    intent.tif = cmd.tif;
    intent.risk_reservation_id = decision.reservation_id;
    intent.risk_valid_until = decision.valid_until;
    intent.created_wall = clock_.wall_now();
    intent.created_mono = clock_.mono_now();
    intent.delivery = DeliveryState::AcceptedDurable;

    fix::NewOrderFields nf;
    nf.cl_ord_id = cl_ord_id;
    nf.account = cmd.account;
    nf.wire_symbol = wire.value();
    nf.side = cmd.side;
    nf.qty = intent.qty;
    nf.price_ticks = intent.price_ticks;
    nf.price_scale = intent.price_scale;
    nf.ord_type = cmd.ord_type;
    nf.tif = cmd.tif;
    nf.transact_time = format_fix_utc(clock_.wall_now());
    auto encoded = fix::encode_new_order(nf);
    if (encoded.is_err()) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }
    intent.fix_body = encoded.value().serialize();
    intent.payload_hash = encoded.value().digest();

    OutboxRow row;
    row.id = store_.next_outbox_id();
    row.tenant = intent.tenant;
    row.command_id = command_id;
    row.payload_hash = intent.payload_hash;
    row.immutable_fix_fields = intent.fix_body;
    row.state = OutboxState::Pending;
    row.enqueued_mono = clock_.mono_now();
    row.priority = cfg_.limits.credit_new;
    intent.outbox_id = row.id;

    p03::ChainState chain = store_.reducer().create(
        chain_id, intent.tenant, cmd.account, intent.lp, intent.wire_symbol, cmd.side,
        intent.price_scale, cl_ord_id, intent.qty, intent.price_ticks);

    // CW-01: dying before the commit leaves no trace, and the client is free to
    // retry with the same command id.
    fp_.check(fp::kBeforeIntentCommit);

    Txn txn = store_.begin();
    txn.put_intent(intent);
    txn.put_outbox(row);
    txn.put_chain(chain);
    txn.reserve_cl_ord_id(intent.lp, cl_ord_id, intent.key());
    AuditRecord rec;
    rec.kind = AuditKind::CommandAdmitted;
    rec.actor = cmd.principal.actor.str();
    rec.subject = command_id.str();
    rec.reason = "ACCEPTED_DURABLE";
    rec.detail = "chain=" + chain_id.str() + " clordid=" + cl_ord_id.str() +
                 " lp=" + intent.lp.str();
    rec.evidence = intent.payload_hash;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    txn.add_audit(rec);
    JournalRecord jr;
    jr.kind = JournalRecord::Kind::IntentCommitted;
    jr.intent = intent;
    jr.chain_id = chain_id;
    jr.resulting_state_hash = chain.state_hash();
    txn.add_journal(jr);

    auto committed = txn.commit();
    if (committed.is_err()) {
        if (committed.error() == StoreError::CommitAckLost &&
            commit_actually_landed(store_, intent.tenant, command_id, digest)) {
            // The row is there. Treat it as the success it was, rather than
            // releasing a reservation that now belongs to a live order.
            store_.image().next_cl_ord_seq = seq + 1;
            ++store_.image().next_chain_seq;
            return R::ok(accepted_from(*store_.find_intent(intent.tenant, command_id)));
        }
        // R-I20: nothing may look accepted-durable when the store refused, and
        // the risk reservation is handed back so a retry is clean.
        risk_.release(decision.reservation_id);
        if (committed.error() == StoreError::Conflict) {
            // (lp, ClOrdID) uniqueness refused the allocation: the counter must
            // have been seeded backwards. Allocation halts rather than reusing
            // an identifier (OR-C-17).
            set_mode(RouterMode::Halt, "clordid_uniqueness");
            record_reject(cmd, command_id, RoutingError::StoreUnavailable);
            return R::err(RoutingError::StoreUnavailable);
        }
        if (committed.error() == StoreError::Full || committed.error() == StoreError::IoError ||
            committed.error() == StoreError::Unavailable)
            set_mode(RouterMode::Halt, "store:" + std::string(to_string(committed.error())));
        record_reject(cmd, command_id, RoutingError::StoreUnavailable);
        return R::err(RoutingError::StoreUnavailable);
    }
    store_.image().next_cl_ord_seq = seq + 1;
    ++store_.image().next_chain_seq;
    metrics_.observe(metric::kCommandToIntentLatency, clock_.mono_now() - intent.created_mono);

    // CW-03: dying here means the client never heard the answer. A retry with
    // the same command id must return this same record (OR-E-02, OR-O-03).
    fp_.check(fp::kAfterIntentCommit);

    AcceptedIntent out;
    out.command_id = command_id;
    out.chain_id = chain_id;
    out.cl_ord_id = cl_ord_id;
    out.route = intent.lp;
    out.delivery = DeliveryState::AcceptedDurable;
    out.business = BusinessOutcome::Unknown;
    return R::ok(out);
}

namespace {

// Shared checks for a cancel or a replace: the chain must exist, be ours, be
// workable, and have nothing conflicting outstanding.
Result<const p03::ChainState*, RoutingError> locate_chain(const Store& store, const Config& cfg,
                                                          const CommandEnvelope& cmd) {
    using R = Result<const p03::ChainState*, RoutingError>;
    if (!cmd.chain_id) return R::err(RoutingError::ValidationError);
    const p03::ChainState* chain = store.find_chain(*cmd.chain_id);
    if (chain == nullptr) return R::err(RoutingError::UnknownChain);
    // Ownership: a caller may only act on a chain belonging to an account it
    // owns, and the error must not reveal anything about the other tenant
    // (OR-A-12, OR-H-17, OR-Q-01).
    if (!cfg.account_belongs_to(cmd.principal.tenant, chain->account))
        return R::err(RoutingError::AuthzDenied);
    if (chain->account != cmd.account) return R::err(RoutingError::AuthzDenied);
    if (chain->hold) return R::err(RoutingError::UnknownOutcomeBlocked);
    return R::ok(chain);
}

}  // namespace

Result<AcceptedIntent, RoutingError> Router::admit_cancel(const CommandEnvelope& cmd,
                                                          const CommandId& command_id,
                                                          const Hash& digest) {
    using R = Result<AcceptedIntent, RoutingError>;
    auto located = locate_chain(store_, cfg_, cmd);
    if (located.is_err()) {
        record_reject(cmd, command_id, located.error());
        return R::err(located.error());
    }
    const p03::ChainState* chain = located.value();

    // A terminal order cannot be cancelled, and saying "canceled" would be a
    // lie (OR-H-13, OR-H-14).
    if (chain->terminal()) {
        record_reject(cmd, command_id, RoutingError::TerminalState);
        return R::err(RoutingError::TerminalState);
    }
    // Strict pessimistic chaining: one outstanding request per chain, and a
    // cancel while a replace is pending is refused rather than chained
    // optimistically (OR-C-09, OR-H-10, OR-I-11).
    if (chain->pending_cancel || chain->pending_replace) {
        record_reject(cmd, command_id, RoutingError::PendingConflict);
        return R::err(RoutingError::PendingConflict);
    }
    // Optimistic concurrency, same as for a replace: if a fill landed between
    // the caller reading the chain and this commit, the caller is told rather
    // than having its stale view acted on (OR-H-19).
    if (cmd.expected_version && *cmd.expected_version != chain->version) {
        record_reject(cmd, command_id, RoutingError::StaleVersion);
        return R::err(RoutingError::StaleVersion);
    }

    // A chain whose own New is still ambiguous has no identity to cancel
    // against: design doc 8.2 forbids sending a cancel without enough identity.
    const CommandIntent* origin = store_.find_intent_by_cl_ord_id(chain->lp,
                                                                  chain->accepted_cl_ord_id);
    if (origin != nullptr && origin->delivery == DeliveryState::UnknownTransport &&
        !chain->lp_acked) {
        record_reject(cmd, command_id, RoutingError::UnknownOutcomeBlocked);
        return R::err(RoutingError::UnknownOutcomeBlocked);
    }

    auto route = resolve_route(cmd.account);
    if (route.is_err()) {
        record_reject(cmd, command_id, route.error());
        return R::err(route.error());
    }

    const std::uint64_t seq = store_.image().next_cl_ord_seq;
    const ClientOrderId cancel_id("C-" + std::to_string(seq));

    CommandIntent intent;
    intent.tenant = cmd.principal.tenant;
    intent.command_id = command_id;
    intent.chain_id = chain->chain_id;
    intent.kind = CommandKind::Cancel;
    intent.request_hash = digest;
    intent.cl_ord_id = cancel_id;
    intent.orig_cl_ord_id = chain->accepted_cl_ord_id;  // R-I05, last accepted
    intent.account = cmd.account;
    intent.lp = route.value()->lp;
    intent.session_key = session_.identity().key();
    intent.route_config_version = cfg_.route_config_version;
    intent.profile_version = cfg_.profile_version;
    intent.wire_symbol = chain->wire_symbol;
    intent.side = chain->side;
    intent.qty = chain->accepted_qty;
    intent.price_ticks = chain->accepted_price;
    intent.price_scale = chain->price_scale;
    intent.created_wall = clock_.wall_now();
    intent.created_mono = clock_.mono_now();
    intent.delivery = DeliveryState::AcceptedDurable;

    fix::CancelFields cf;
    cf.cl_ord_id = cancel_id;
    cf.orig_cl_ord_id = chain->accepted_cl_ord_id;
    cf.account = cmd.account;
    cf.wire_symbol = chain->wire_symbol;
    cf.side = chain->side;
    cf.transact_time = format_fix_utc(clock_.wall_now());
    auto encoded = fix::encode_cancel(cf);
    if (encoded.is_err()) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }
    intent.fix_body = encoded.value().serialize();
    intent.payload_hash = encoded.value().digest();

    OutboxRow row;
    row.id = store_.next_outbox_id();
    row.tenant = intent.tenant;
    row.command_id = command_id;
    row.payload_hash = intent.payload_hash;
    row.immutable_fix_fields = intent.fix_body;
    row.state = OutboxState::Pending;
    row.enqueued_mono = clock_.mono_now();
    row.priority = cfg_.limits.credit_cancel;  // cancels go first across chains
    intent.outbox_id = row.id;

    p03::ChainState updated = *chain;
    store_.reducer().note_cancel_requested(updated, cancel_id);

    fp_.check(fp::kBeforeIntentCommit);
    Txn txn = store_.begin();
    txn.put_intent(intent);
    txn.put_outbox(row);
    txn.put_chain(updated);
    txn.reserve_cl_ord_id(intent.lp, cancel_id, intent.key());
    AuditRecord rec;
    rec.kind = AuditKind::CommandAdmitted;
    rec.actor = cmd.principal.actor.str();
    rec.subject = command_id.str();
    rec.reason = "ACCEPTED_DURABLE";
    rec.detail = "cancel chain=" + chain->chain_id.str() + " orig=" +
                 chain->accepted_cl_ord_id.str() + " clordid=" + cancel_id.str();
    rec.evidence = intent.payload_hash;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    txn.add_audit(rec);
    JournalRecord jr;
    jr.kind = JournalRecord::Kind::IntentCommitted;
    jr.intent = intent;
    jr.chain_id = chain->chain_id;
    jr.resulting_state_hash = updated.state_hash();
    txn.add_journal(jr);

    auto committed = txn.commit();
    if (committed.is_err()) {
        if (committed.error() == StoreError::CommitAckLost &&
            commit_actually_landed(store_, intent.tenant, command_id, digest)) {
            store_.image().next_cl_ord_seq = seq + 1;
            // EI-24: the cancel is durable, so risk keeps headroom for the
            // fills that can still arrive before the LP acts on it.
            risk_.note_cancel_requested(intent.tenant, intent.account, chain->chain_id,
                                        command_id);
            return R::ok(accepted_from(*store_.find_intent(intent.tenant, command_id)));
        }
        record_reject(cmd, command_id, RoutingError::StoreUnavailable);
        return R::err(RoutingError::StoreUnavailable);
    }
    store_.image().next_cl_ord_seq = seq + 1;
    risk_.note_cancel_requested(intent.tenant, intent.account, chain->chain_id, command_id);

    AcceptedIntent out;
    out.command_id = command_id;
    out.chain_id = chain->chain_id;
    out.cl_ord_id = cancel_id;
    out.route = intent.lp;
    out.delivery = DeliveryState::AcceptedDurable;
    out.business = BusinessOutcome::Unknown;
    return R::ok(out);
}

Result<AcceptedIntent, RoutingError> Router::admit_replace(const CommandEnvelope& cmd,
                                                           const CommandId& command_id,
                                                           const Hash& digest) {
    using R = Result<AcceptedIntent, RoutingError>;
    auto located = locate_chain(store_, cfg_, cmd);
    if (located.is_err()) {
        record_reject(cmd, command_id, located.error());
        return R::err(located.error());
    }
    const p03::ChainState* chain = located.value();

    if (chain->terminal()) {
        record_reject(cmd, command_id, RoutingError::TerminalState);
        return R::err(RoutingError::TerminalState);
    }
    if (chain->pending_cancel || chain->pending_replace) {
        record_reject(cmd, command_id, RoutingError::PendingConflict);
        return R::err(RoutingError::PendingConflict);
    }
    // Optimistic concurrency against the P03 aggregate version (OR-A-14).
    if (cmd.expected_version && *cmd.expected_version != chain->version) {
        record_reject(cmd, command_id, RoutingError::StaleVersion);
        return R::err(RoutingError::StaleVersion);
    }

    auto wire = cfg_.resolve_symbol_alias(cmd.symbol.empty() ? chain->wire_symbol : cmd.symbol);
    if (wire.is_err()) {
        record_reject(cmd, command_id, wire.error());
        return R::err(wire.error());
    }
    // A replace may not change the instrument or the direction in v1
    // (OR-I-15, OR-I-16).
    if (wire.value() != chain->wire_symbol && !cfg_.order_profile.allow_symbol_change_on_replace) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }
    if (cmd.side != chain->side && !cfg_.order_profile.allow_side_change_on_replace) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }
    if (!cfg_.ord_type_allowed(cmd.ord_type)) {
        record_reject(cmd, command_id, RoutingError::UnsupportedOrdType);
        return R::err(RoutingError::UnsupportedOrdType);
    }

    const SymbolProfile* prof = cfg_.symbol(chain->wire_symbol);
    if (prof == nullptr) {
        record_reject(cmd, command_id, RoutingError::UnsupportedSymbol);
        return R::err(RoutingError::UnsupportedSymbol);
    }
    auto sized = size_order(*prof, cmd);
    if (sized.is_err()) {
        record_reject(cmd, command_id, sized.error());
        return R::err(sized.error());
    }
    // OR-I-14: a total below what is already done would need a negative
    // remainder. Strict v1 refuses before the wire.
    if (sized.value().qty < chain->cum_qty) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }

    auto route = resolve_route(cmd.account);
    if (route.is_err()) {
        record_reject(cmd, command_id, route.error());
        return R::err(route.error());
    }

    // A replace that increases exposure needs risk approval exactly like a New.
    const bool increases = sized.value().qty > chain->accepted_qty;
    p05::RiskDecision decision;
    decision.verdict = p05::Verdict::Approve;
    if (increases) {
        p05::RiskRequest rr;
        rr.command_id = command_id;
        rr.tenant = cmd.principal.tenant;
        rr.account = cmd.account;
        rr.wire_symbol = chain->wire_symbol;
        rr.side = chain->side;
        rr.qty = sized.value().qty - chain->accepted_qty;
        rr.price_ticks = sized.value().price_ticks;
        rr.increases_exposure = true;
        rr.order_chain_id = chain->chain_id;
        rr.price_scale = chain->price_scale;
        rr.total_units_after = sized.value().qty;
        decision = risk_.reserve(rr);
        if (decision.verdict == p05::Verdict::Reject) {
            record_reject(cmd, command_id, RoutingError::RiskRejected);
            return R::err(RoutingError::RiskRejected);
        }
        if (decision.verdict == p05::Verdict::Unavailable) {
            record_reject(cmd, command_id, RoutingError::RiskUnavailable);
            return R::err(RoutingError::RiskUnavailable);
        }
    }

    const std::uint64_t seq = store_.image().next_cl_ord_seq;
    const ClientOrderId replace_id("R-" + std::to_string(seq));

    CommandIntent intent;
    intent.tenant = cmd.principal.tenant;
    intent.command_id = command_id;
    intent.chain_id = chain->chain_id;
    intent.kind = CommandKind::Replace;
    intent.request_hash = digest;
    intent.cl_ord_id = replace_id;
    intent.orig_cl_ord_id = chain->accepted_cl_ord_id;
    intent.account = cmd.account;
    intent.lp = route.value()->lp;
    intent.session_key = session_.identity().key();
    intent.route_config_version = cfg_.route_config_version;
    intent.profile_version = cfg_.profile_version;
    intent.wire_symbol = chain->wire_symbol;
    intent.side = chain->side;
    intent.qty = sized.value().qty;
    intent.price_ticks = sized.value().price_ticks;
    intent.price_scale = prof->price_scale;
    intent.ord_type = cmd.ord_type;
    intent.tif = cmd.tif;
    intent.risk_reservation_id = decision.reservation_id;
    intent.risk_valid_until = decision.valid_until;
    intent.created_wall = clock_.wall_now();
    intent.created_mono = clock_.mono_now();
    intent.delivery = DeliveryState::AcceptedDurable;

    fix::ReplaceFields rf;
    rf.cl_ord_id = replace_id;
    rf.orig_cl_ord_id = chain->accepted_cl_ord_id;
    rf.account = cmd.account;
    rf.wire_symbol = chain->wire_symbol;
    rf.side = chain->side;
    rf.total_qty = sized.value().qty;  // the whole intended size, never LeavesQty
    rf.price_ticks = sized.value().price_ticks;
    rf.price_scale = prof->price_scale;
    rf.ord_type = cmd.ord_type;
    rf.tif = cmd.tif;
    rf.transact_time = format_fix_utc(clock_.wall_now());
    auto encoded = fix::encode_replace(rf);
    if (encoded.is_err()) {
        record_reject(cmd, command_id, RoutingError::ValidationError);
        return R::err(RoutingError::ValidationError);
    }
    intent.fix_body = encoded.value().serialize();
    intent.payload_hash = encoded.value().digest();

    OutboxRow row;
    row.id = store_.next_outbox_id();
    row.tenant = intent.tenant;
    row.command_id = command_id;
    row.payload_hash = intent.payload_hash;
    row.immutable_fix_fields = intent.fix_body;
    row.state = OutboxState::Pending;
    row.enqueued_mono = clock_.mono_now();
    row.priority = cfg_.limits.credit_replace;
    intent.outbox_id = row.id;

    p03::ChainState updated = *chain;
    store_.reducer().note_replace_requested(updated, replace_id, sized.value().qty,
                                                   sized.value().price_ticks);

    fp_.check(fp::kBeforeIntentCommit);
    Txn txn = store_.begin();
    txn.put_intent(intent);
    txn.put_outbox(row);
    txn.put_chain(updated);
    txn.reserve_cl_ord_id(intent.lp, replace_id, intent.key());
    AuditRecord rec;
    rec.kind = AuditKind::CommandAdmitted;
    rec.actor = cmd.principal.actor.str();
    rec.subject = command_id.str();
    rec.reason = "ACCEPTED_DURABLE";
    rec.detail = "replace chain=" + chain->chain_id.str() + " orig=" +
                 chain->accepted_cl_ord_id.str() + " clordid=" + replace_id.str();
    rec.evidence = intent.payload_hash;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    txn.add_audit(rec);
    JournalRecord jr;
    jr.kind = JournalRecord::Kind::IntentCommitted;
    jr.intent = intent;
    jr.chain_id = chain->chain_id;
    jr.resulting_state_hash = updated.state_hash();
    txn.add_journal(jr);

    auto committed = txn.commit();
    if (committed.is_err()) {
        if (committed.error() == StoreError::CommitAckLost &&
            commit_actually_landed(store_, intent.tenant, command_id, digest)) {
            store_.image().next_cl_ord_seq = seq + 1;
            return R::ok(accepted_from(*store_.find_intent(intent.tenant, command_id)));
        }
        if (increases) risk_.release(decision.reservation_id);
        record_reject(cmd, command_id, RoutingError::StoreUnavailable);
        return R::err(RoutingError::StoreUnavailable);
    }
    store_.image().next_cl_ord_seq = seq + 1;

    AcceptedIntent out;
    out.command_id = command_id;
    out.chain_id = chain->chain_id;
    out.cl_ord_id = replace_id;
    out.route = intent.lp;
    out.delivery = DeliveryState::AcceptedDurable;
    out.business = BusinessOutcome::Unknown;
    return R::ok(out);
}

// ---------------------------------------------------------------------------
// Dispatch

DispatchStats Router::dispatch_pending(std::size_t max) {
    DispatchStats stats;

    // Gate 1: the session. Design doc 16.2 and OR-F-01 to OR-F-03: nothing
    // application level goes out before P01 reports ACTIVE.
    if (!session_.ready_for_app()) {
        stats.blocked_not_ready = store_.pending_outbox_count();
        metrics_.set_gauge(metric::kOutboxDepth,
                           static_cast<std::int64_t>(stats.blocked_not_ready));
        return stats;
    }

    std::vector<OutboxId> ids;
    for (const OutboxRow* row : store_.pending_outbox()) ids.push_back(row->id);

    std::size_t handled = 0;
    for (OutboxId id : ids) {
        if (handled >= max) break;
        auto row_it = store_.image().outbox.find(id);
        if (row_it == store_.image().outbox.end()) continue;
        OutboxRow row = row_it->second;
        const CommandIntent* intent_ptr = store_.find_intent(row.tenant, row.command_id);
        if (intent_ptr == nullptr) continue;
        CommandIntent intent = *intent_ptr;
        if (intent.delivery == DeliveryState::ManualHold ||
            intent.delivery == DeliveryState::LpAcked ||
            intent.delivery == DeliveryState::AwaitBusiness) {
            continue;
        }
        ++handled;

        // Gate 2: the configuration must not have moved under a pending
        // command. OR-B-04, OR-B-13 and OR-B-15 all land here: the committed
        // payload is never mutated, so an incompatible change becomes a hold.
        const Route* live = cfg_.route_for(intent.account);
        // The session the intent was admitted against must still be the session
        // we are attached to. A CompID that changed underneath a durable outbox
        // row would otherwise send one peer's order to another (OR-F-14).
        const bool session_moved = intent.session_key != session_.identity().key();
        if (live == nullptr || live->lp != intent.lp ||
            intent.profile_version != cfg_.profile_version || session_moved ||
            !cfg_.entitled(intent.account)) {
            intent.delivery = DeliveryState::ManualHold;
            intent.hold = HoldReason::PendingPolicy;
            row.state = OutboxState::Held;
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            AuditRecord rec;
            rec.kind = AuditKind::HoldOpened;
            rec.actor = "system";
            rec.subject = intent.command_id.str();
            rec.reason = "config_changed_while_pending";
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();
            txn.add_audit(rec);
            txn.commit();
            metrics_.inc(metric::kHoldTotal, "PendingPolicy");
            ++stats.held;
            alert("dispatch held: configuration changed under command " +
                  intent.command_id.str());
            continue;
        }

        // Gate 3: the payload must still be the bytes we committed. OR-E-11
        // corrupts the record in memory and expects the mismatch to be caught
        // rather than sent.
        if (row.immutable_fix_fields.empty() || row.payload_hash.is_zero()) {
            // OR-E-19: a migrated row with no payload or no hash cannot be
            // issued safely, so it waits for a repair instead.
            intent.delivery = DeliveryState::ManualHold;
            intent.hold = HoldReason::StoreFailClosed;
            row.state = OutboxState::Held;
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            txn.commit();
            ++stats.held;
            continue;
        }
        if (intent.payload_hash != row.payload_hash || intent.fix_body != row.immutable_fix_fields) {
            intent.delivery = DeliveryState::ManualHold;
            intent.hold = HoldReason::StoreFailClosed;
            row.state = OutboxState::Held;
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            AuditRecord rec;
            rec.kind = AuditKind::HoldOpened;
            rec.actor = "system";
            rec.subject = intent.command_id.str();
            rec.reason = "payload_hash_mismatch";
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();
            txn.add_audit(rec);
            txn.commit();
            metrics_.inc(metric::kHoldTotal, "StoreFailClosed");
            ++stats.held;
            continue;
        }

        // Gate 4: risk must still hold the reservation for anything that
        // increases exposure (OR-B-15, OR-G-10).
        if (!intent.risk_reservation_id.empty() && !risk_.held(intent.risk_reservation_id)) {
            intent.delivery = DeliveryState::ManualHold;
            intent.hold = HoldReason::OperatorRequested;
            row.state = OutboxState::Held;
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            AuditRecord rec;
            rec.kind = AuditKind::HoldOpened;
            rec.actor = "system";
            rec.subject = intent.command_id.str();
            rec.reason = "risk_reservation_revoked";
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();
            txn.add_audit(rec);
            txn.commit();
            ++stats.held;
            alert("dispatch held: risk reservation revoked for " + intent.command_id.str());
            continue;
        }
        // A verdict that has expired is rechecked, never reused (OR-A-18).
        if (intent.risk_valid_until != 0 && clock_.wall_now() > intent.risk_valid_until) {
            intent.delivery = DeliveryState::ManualHold;
            intent.hold = HoldReason::OperatorRequested;
            row.state = OutboxState::Held;
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            AuditRecord rec;
            rec.kind = AuditKind::HoldOpened;
            rec.actor = "system";
            rec.subject = intent.command_id.str();
            rec.reason = "risk_verdict_expired";
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();
            txn.add_audit(rec);
            txn.commit();
            ++stats.held;
            continue;
        }

        // Claim the row with our fencing token, so two instances cannot both
        // believe they own the send (OR-E-04).
        row.state = OutboxState::Claimed;
        row.claim_fence = store_.fence();
        ++row.attempts;
        intent.delivery = DeliveryState::Dispatching;
        intent.send_attempts = row.attempts;
        {
            Txn txn = store_.begin();
            txn.put_intent(intent);
            txn.put_outbox(row);
            auto c = txn.commit();
            if (c.is_err()) {
                ++stats.held;
                continue;
            }
        }

        // CW-04: dying after the claim. The row is still pending work and the
        // same command id is replayed, never a new business intent.
        fp_.check(fp::kAfterOutboxClaim);

        const p01::StableSendResult sent =
            session_.send_once(intent.command_id, intent.payload_hash, intent.fix_body);

        CommandIntent after = intent;
        OutboxRow after_row = row;
        AuditRecord rec;
        rec.actor = "system";
        rec.subject = intent.command_id.str();
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        rec.evidence = intent.payload_hash;

        switch (sent.state) {
            case p01::StableSendResult::State::AssignedOriginalSeq:
            case p01::StableSendResult::State::AlreadyAssigned: {
                const bool fresh = sent.state == p01::StableSendResult::State::AssignedOriginalSeq;
                after.original_seq = sent.original_seq;
                after.delivery = DeliveryState::AwaitBusiness;
                after.dispatched_mono = clock_.mono_now();
                after_row.state = OutboxState::Sent;
                rec.kind = AuditKind::OriginalSendAssigned;
                rec.reason = fresh ? "assigned" : "already_assigned";
                rec.detail = "seq=" + std::to_string(sent.original_seq.value_or(0));
                if (fresh) {
                    ++original_sends_;
                    metrics_.inc(metric::kOriginalSendTotal);
                }
                ++stats.sent;
                break;
            }
            case p01::StableSendResult::State::NotReady: {
                after.delivery = DeliveryState::Queued;
                after_row.state = OutboxState::Pending;
                rec.kind = AuditKind::RouteDecided;
                rec.reason = "session_not_ready";
                ++stats.queued;
                break;
            }
            case p01::StableSendResult::State::Unknown: {
                // R-I16 in one place: the outcome is unknown, so it is recorded
                // as unknown. No new ClOrdID, no second D, and a reconciliation
                // case instead of a guess.
                after.original_seq = sent.original_seq;
                after.delivery = DeliveryState::UnknownTransport;
                after.dispatched_mono = clock_.mono_now();
                after_row.state = OutboxState::Sent;
                rec.kind = AuditKind::SendAmbiguous;
                rec.reason = "unknown_transport";
                rec.detail = "seq=" + std::to_string(sent.original_seq.value_or(0));
                ++stats.unknown;
                break;
            }
            case p01::StableSendResult::State::Fatal: {
                after.delivery = DeliveryState::ManualHold;
                after.hold = HoldReason::IdentityConflict;
                after_row.state = OutboxState::Held;
                rec.kind = AuditKind::HoldOpened;
                rec.reason = "stable_send_payload_conflict";
                ++stats.held;
                alert("send_once reported a payload conflict for " + intent.command_id.str());
                break;
            }
            case p01::StableSendResult::State::NotSupported: {
                // PB-03: without StableSendV1 the issue-once property cannot be
                // claimed. The feature is blocked, loudly, rather than
                // approximated.
                after.delivery = DeliveryState::ManualHold;
                after.hold = HoldReason::CapabilityMissing;
                after_row.state = OutboxState::Held;
                rec.kind = AuditKind::HoldOpened;
                rec.reason = "stable_send_not_implemented";
                ++stats.held;
                set_mode(RouterMode::Degraded, "stable_send_unavailable");
                alert("StableSendV1 is not implemented: issue-once is BLOCKED, not proven");
                break;
            }
        }

        Txn txn = store_.begin();
        txn.put_intent(after);
        txn.put_outbox(after_row);
        txn.add_audit(rec);
        if (after.delivery == DeliveryState::UnknownTransport)
            open_recon(txn, after.chain_id, after.command_id, HoldReason::AmbiguousSend,
                       after.payload_hash);
        if (after.hold.has_value()) metrics_.inc(metric::kHoldTotal, std::string(to_string(*after.hold)));
        auto c = txn.commit();
        if (c.is_err()) {
            // The bytes may already be out. Recording that we could not persist
            // the fact is itself an unknown state, so it escalates.
            alert("could not persist dispatch result for " + intent.command_id.str());
            set_mode(RouterMode::Halt, "dispatch_commit");
        }
    }

    metrics_.set_gauge(metric::kOutboxDepth,
                       static_cast<std::int64_t>(store_.pending_outbox_count()));
    return stats;
}

}  // namespace router
