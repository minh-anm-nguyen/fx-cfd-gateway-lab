// Reconciliation, timers, the operator workflow and the read models
// (design doc 14, 17.3 and 14.4).
//
// The decision tree in 14.2 is implemented literally, including the two places
// where it refuses to decide: without the 35=H capability there is no probe to
// send, so the case goes to MANUAL_HOLD; and when the peer's summary is ahead
// of the local ledger the answer is HISTORY_REQUIRED, never a synthesised fill.
#include <algorithm>

#include "router/router.hpp"

namespace router {
namespace {

// Causes that no automated probe can settle. Design doc 14.2 and R-I24: a
// contradiction is escalated, not retried.
bool needs_human(HoldReason cause) {
    switch (cause) {
        case HoldReason::ExecIdConflict:
        case HoldReason::DuplicateExecIdChain:
        case HoldReason::SecurityQuarantine:
        case HoldReason::IdentityConflict:
        case HoldReason::SemanticConflict:
        case HoldReason::SchemaViolation:
        case HoldReason::UnsupportedEvent:
        case HoldReason::SnapshotMismatch:
        case HoldReason::HistoryRequired:
        case HoldReason::StateRegression:
        case HoldReason::ReplaceAfterTerminal:
        case HoldReason::OrphanReport:
        case HoldReason::OrphanQuotaExceeded:
        case HoldReason::PendingPolicy:
        case HoldReason::StoreFailClosed:
        case HoldReason::CapabilityMissing:
        case HoldReason::ReconBudgetExhausted:
        case HoldReason::OperatorRequested:
            return true;
        case HoldReason::AmbiguousSend:
        case HoldReason::AckTimeout:
        case HoldReason::None:
            return false;
    }
    return true;
}

}  // namespace

void Router::on_timer() {
    const MonoMicros now = clock_.mono_now();

    // Step 1: a request that has been waiting for business evidence longer than
    // the configured budget becomes a reconciliation case. Design doc 12.2 is
    // explicit that the timer does not create a new command.
    {
        std::vector<CommandIntent> to_update;
        Txn txn = store_.begin();
        bool any = false;
        for (const auto& [key, intent] : store_.image().intents) {
            (void)key;
            if (intent.delivery != DeliveryState::AwaitBusiness) continue;
            if (intent.business != BusinessOutcome::Unknown) continue;
            if (!intent.dispatched_mono) continue;
            if (now - *intent.dispatched_mono < cfg_.limits.ack_timeout_micros) continue;
            CommandIntent updated = intent;
            updated.delivery = DeliveryState::Reconciling;
            txn.put_intent(updated);
            open_recon(txn, intent.chain_id, intent.command_id, HoldReason::AckTimeout,
                       intent.payload_hash);
            any = true;
        }
        if (any) txn.commit();
    }

    // Step 1b: a cancel that has been waiting for transport past its deadline.
    // Design doc 8.2 allows a cancel into a durable priority queue while the
    // session is down, with a TTL and a warning that the cancel is not
    // guaranteed. When the deadline passes the state becomes explicitly unknown
    // -- never "cancelled" (OR-H-15, OR-H-16).
    {
        Txn txn = store_.begin();
        bool any = false;
        for (const auto& [key, intent] : store_.image().intents) {
            (void)key;
            if (intent.kind != CommandKind::Cancel) continue;
            if (intent.delivery != DeliveryState::AcceptedDurable &&
                intent.delivery != DeliveryState::Queued)
                continue;
            if (now - intent.created_mono < cfg_.limits.cancel_transport_ttl_micros) continue;
            CommandIntent updated = intent;
            updated.delivery = DeliveryState::UnknownTransport;
            updated.hold = HoldReason::AckTimeout;
            txn.put_intent(updated);
            open_recon(txn, intent.chain_id, intent.command_id, HoldReason::AckTimeout,
                       intent.payload_hash);
            AuditRecord rec;
            rec.kind = AuditKind::HoldOpened;
            rec.actor = "system";
            rec.subject = intent.command_id.str();
            rec.reason = "cancel_transport_deadline_exceeded";
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();
            txn.add_audit(rec);
            alert("cancel " + intent.command_id.str() +
                  " exceeded its transport deadline and is NOT confirmed");
            any = true;
        }
        if (any) txn.commit();
    }

    // Step 2: drive the open cases.
    std::vector<ReconCase> due;
    for (const auto& [key, c] : store_.image().recon_cases) {
        (void)key;
        if (c.state == ReconState::Reconciled || c.state == ReconState::ManualHold ||
            c.state == ReconState::Quarantined)
            continue;
        if (c.next_due_mono > now) continue;
        due.push_back(c);
    }

    for (const ReconCase& original : due) {
        ReconCase c = original;
        Txn txn = store_.begin();
        AuditRecord rec;
        rec.actor = "system";
        rec.subject = c.case_id.str();
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        rec.evidence = c.last_evidence_hash;

        if (needs_human(c.cause)) {
            c.state = ReconState::ManualHold;
            c.notes.push_back("cause requires human review");
            rec.kind = AuditKind::HoldOpened;
            rec.reason = std::string(to_string(c.cause));
            txn.put_recon(c);
            txn.add_audit(rec);
            txn.commit();
            alert("case " + c.case_id.str() + " escalated: " + std::string(to_string(c.cause)));
            continue;
        }

        // OR-B-07, OR-M-08, OR-N-01, PB-13: with the capability off there is no
        // legitimate probe, so zero 35=H leave the router and the case waits for
        // a human. This is the single gate; nothing else emits an H.
        if (!cfg_.capabilities.order_status_request) {
            c.state = ReconState::ManualHold;
            c.notes.push_back("order status query not supported by the peer profile");
            rec.kind = AuditKind::HoldOpened;
            rec.reason = std::string(to_string(HoldReason::CapabilityMissing));
            txn.put_recon(c);
            txn.add_audit(rec);
            txn.commit();
            metrics_.inc(metric::kHoldTotal, std::string(to_string(HoldReason::CapabilityMissing)));
            alert("case " + c.case_id.str() + " on manual hold: no status query capability");
            continue;
        }

        if (c.attempt_count >= cfg_.limits.recon_max_probes) {
            // OR-M-13: the budget is finite, so a peer that never answers
            // produces an escalation rather than a request storm.
            c.state = ReconState::ManualHold;
            c.notes.push_back("probe budget exhausted");
            rec.kind = AuditKind::HoldOpened;
            rec.reason = std::string(to_string(HoldReason::ReconBudgetExhausted));
            txn.put_recon(c);
            txn.add_audit(rec);
            txn.commit();
            metrics_.inc(metric::kHoldTotal,
                         std::string(to_string(HoldReason::ReconBudgetExhausted)));
            continue;
        }

        // OR-N-15 and design doc 14.3: a bounded number of probes may be in
        // flight, so a reconnect storm cannot turn into a query storm.
        if (in_flight_probes_ >= cfg_.limits.status_queries_in_flight) {
            c.next_due_mono = now + cfg_.limits.recon_initial_backoff_micros;
            txn.put_recon(c);
            txn.commit();
            continue;
        }

        const p03::ChainState* chain = store_.find_chain(c.chain_id);
        if (chain == nullptr) {
            c.state = ReconState::ManualHold;
            c.notes.push_back("chain projection missing");
            txn.put_recon(c);
            txn.commit();
            continue;
        }

        const QueryId query_id(c.case_id.str() + "-q" + std::to_string(c.attempt_count + 1));
        fix::StatusFields sf;
        sf.cl_ord_id = chain->accepted_cl_ord_id;
        sf.account = chain->account;
        sf.wire_symbol = chain->wire_symbol;
        sf.side = chain->side;
        if (chain->lp_order_id) sf.order_id = chain->lp_order_id;
        if (cfg_.capabilities.echo_ord_status_req_id) sf.ord_status_req_id = query_id;
        auto encoded = fix::encode_status(sf);
        if (encoded.is_err()) {
            c.state = ReconState::ManualHold;
            c.notes.push_back("status request could not be encoded");
            txn.put_recon(c);
            txn.commit();
            continue;
        }

        // The probe is a command in its own right so that it is durable, audited
        // and idempotent against P01 (CW-16).
        const CommandId probe_id(std::string("recon-") + c.case_id.str() + "-" +
                                 std::to_string(c.attempt_count + 1));
        CommandIntent probe;
        probe.tenant = chain->tenant;
        probe.command_id = probe_id;
        probe.chain_id = c.chain_id;
        probe.kind = CommandKind::StatusQuery;
        probe.cl_ord_id = chain->accepted_cl_ord_id;
        probe.account = chain->account;
        probe.lp = chain->lp;
        probe.session_key = session_.identity().key();
        probe.route_config_version = cfg_.route_config_version;
        probe.profile_version = cfg_.profile_version;
        probe.wire_symbol = chain->wire_symbol;
        probe.side = chain->side;
        probe.fix_body = encoded.value().serialize();
        probe.payload_hash = encoded.value().digest();
        probe.request_hash = probe.payload_hash;
        probe.created_wall = clock_.wall_now();
        probe.created_mono = now;
        probe.delivery = DeliveryState::AcceptedDurable;

        c.attempt_count += 1;
        c.state = ReconState::StatusQueryPending;
        c.query_ids.push_back(query_id.str());
        // Exponential backoff with a deterministic schedule (design doc 14.3).
        std::uint64_t backoff = cfg_.limits.recon_initial_backoff_micros;
        for (int i = 1; i < c.attempt_count; ++i) backoff *= 2;
        c.next_due_mono = now + backoff;

        rec.kind = AuditKind::ReconProbeSent;
        rec.reason = "order_status_request";
        rec.detail = "query=" + query_id.str() + " attempt=" + std::to_string(c.attempt_count);

        txn.put_intent(probe);
        txn.put_recon(c);
        txn.add_audit(rec);
        if (txn.commit().is_err()) continue;

        fp_.check(fp::kDuringStatusDispatch);

        if (!session_.ready_for_app()) continue;
        const p01::StableSendResult sent =
            session_.send_once(probe_id, probe.payload_hash, probe.fix_body);
        if (sent.state == p01::StableSendResult::State::AssignedOriginalSeq ||
            sent.state == p01::StableSendResult::State::AlreadyAssigned) {
            ++status_requests_emitted_;
            ++in_flight_probes_;
            metrics_.inc(metric::kStatusQueriesTotal);
            metrics_.set_gauge(metric::kStatusQueriesInFlight,
                               static_cast<std::int64_t>(in_flight_probes_));
        }
    }

    metrics_.set_gauge(metric::kUnknownOutcomeOpen,
                       static_cast<std::int64_t>(open_unknown_outcomes()));
    metrics_.set_gauge(metric::kReconOpen, static_cast<std::int64_t>(open_recon_cases()));
}

// ---------------------------------------------------------------------------
// Status evidence

void Router::apply_status_evidence(Txn& txn, const NormalizedReport& report,
                                   const p03::ChainState& chain) {
    // CW-17: dying after the reply but before the comparison is committed must
    // not produce a synthetic fill on replay. The comparison is pure, so a
    // replay simply redoes it.
    fp_.check(fp::kAfterStatusReply);

    if (in_flight_probes_ > 0) --in_flight_probes_;

    for (const auto& [key, existing] : store_.image().recon_cases) {
        (void)key;
        if (existing.chain_id != report.chain_id) continue;
        if (existing.state == ReconState::Reconciled || existing.state == ReconState::ManualHold)
            continue;
        if (existing.cause != HoldReason::AmbiguousSend && existing.cause != HoldReason::AckTimeout)
            continue;

        // Correlate the snapshot to this case where the profile lets us. A reply
        // that echoes a different query is not evidence for this one (OR-N-06).
        if (cfg_.capabilities.echo_ord_status_req_id && report.ord_status_req_id) {
            const bool mine = std::find(existing.query_ids.begin(), existing.query_ids.end(),
                                        report.ord_status_req_id->str()) != existing.query_ids.end();
            if (!mine) continue;
        }

        ReconCase c = existing;
        c.state = ReconState::StatusReceived;
        c.last_evidence_hash = report.raw_frame_hash;

        const bool agrees = report.cum_qty == chain.cum_qty;
        const CommandKind kind = [&]() {
            if (!existing.command_id) return CommandKind::New;
            if (const CommandIntent* ci =
                    store_.find_intent(chain.tenant, *existing.command_id))
                return ci->kind;
            return CommandKind::New;
        }();

        if (!agrees) {
            // The reducer has already decided whether this is HistoryRequired or
            // a regression; the case records the same conclusion.
            c.state = report.cum_qty > chain.cum_qty ? ReconState::HistoryRequired
                                                     : ReconState::ManualHold;
            c.notes.push_back("status snapshot disagrees with the fill ledger");
        } else if (kind == CommandKind::New) {
            // OR-M-02: the snapshot proves the order exists and matches, which
            // is what the ambiguous New needed.
            c.state = ReconState::Reconciled;
            c.resolved_by = "status_snapshot";
            c.notes.push_back("order confirmed by a correlated status snapshot");
        } else {
            // OR-M-07: a cancel or replace whose own outcome is unknown is NOT
            // settled by the order still being reported as working. The case
            // stays open until an explicit report or a human closes it.
            c.notes.push_back(
                "status snapshot does not prove the outcome of a cancel or replace request");
        }

        AuditRecord rec;
        rec.kind = c.state == ReconState::Reconciled ? AuditKind::ReconResolved
                                                     : AuditKind::ReconProbeResult;
        rec.actor = "peer";
        rec.subject = c.case_id.str();
        rec.reason = std::string(to_string(c.state));
        rec.detail = "status cum=" + std::to_string(report.cum_qty) + " local cum=" +
                     std::to_string(chain.cum_qty);
        rec.evidence = report.raw_frame_hash;
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        txn.put_recon(c);
        txn.add_audit(rec);
    }
}

// ---------------------------------------------------------------------------
// Operator workflow

Result<bool, RoutingError> Router::operator_resolve_case(
    const Principal& who, const ReconCaseId& id, const std::string& reason, const Hash& evidence,
    const std::vector<std::string>& approvers) {
    using R = Result<bool, RoutingError>;

    const auto deny = [&](const std::string& why) {
        AuditRecord rec;
        rec.kind = AuditKind::OperatorDenied;
        rec.actor = who.actor.str();
        rec.subject = id.str();
        rec.reason = why;
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        store_.audit().append(rec);
    };

    // OR-M-19, OR-Q-08: without the role the case stays exactly as it was.
    if (!may_force_resolve(who)) {
        deny("insufficient_role");
        return R::err(RoutingError::AuthzDenied);
    }
    const ReconCase* existing = store_.find_recon(id);
    if (existing == nullptr) {
        deny("unknown_case");
        return R::err(RoutingError::UnknownChain);
    }
    // OR-M-19 again: no evidence, no resolution. There is no button that marks
    // an order filled without a digest to point at.
    if (evidence.is_zero() || reason.empty()) {
        deny("missing_evidence");
        return R::err(RoutingError::ValidationError);
    }
    // OR-Q-09: an irreversible action needs two distinct approvers.
    std::vector<std::string> distinct = approvers;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (distinct.size() < 2) {
        deny("two_person_rule");
        return R::err(RoutingError::AuthzDenied);
    }

    ReconCase c = *existing;
    c.state = ReconState::Reconciled;
    c.resolved_by = who.actor.str();
    c.last_evidence_hash = evidence;
    c.approvals = distinct;
    c.notes.push_back("operator resolution: " + sanitize_text(reason, 120));

    Txn txn = store_.begin();
    txn.put_recon(c);
    AuditRecord rec;
    rec.kind = AuditKind::OperatorAction;
    rec.actor = who.actor.str();
    rec.subject = id.str();
    rec.reason = "force_resolve";
    rec.detail = "approvers=" + distinct[0] + "," + distinct[1] + " reason=" +
                 sanitize_text(reason, 120);
    rec.evidence = evidence;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    txn.add_audit(rec);
    // Resolving a case does not invent business state. The chain hold is lifted
    // only because a human took responsibility, and that is on the record.
    if (const p03::ChainState* chain = store_.find_chain(c.chain_id)) {
        if (chain->hold) {
            p03::ChainState cleared = *chain;
            cleared.hold.reset();
            ++cleared.version;
            txn.put_chain(cleared);
            // Journalled like any other chain mutation, so a replay reproduces
            // the operator decision rather than diverging from it.
            JournalRecord jr;
            jr.kind = JournalRecord::Kind::ChainMutated;
            jr.chain_id = cleared.chain_id;
            jr.chain_after = cleared;
            jr.resulting_state_hash = cleared.state_hash();
            txn.add_journal(jr);
        }
    }
    if (txn.commit().is_err()) return R::err(RoutingError::StoreUnavailable);
    return R::ok(true);
}

Result<bool, RoutingError> Router::operator_resume(const Principal& who,
                                                   const std::string& reason) {
    using R = Result<bool, RoutingError>;
    if (!may_change_config(who)) {
        AuditRecord rec;
        rec.kind = AuditKind::OperatorDenied;
        rec.actor = who.actor.str();
        rec.subject = "router";
        rec.reason = "insufficient_role";
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        store_.audit().append(rec);
        return R::err(RoutingError::AuthzDenied);
    }
    // Resuming is only allowed once the store is actually healthy again: an
    // operator cannot wave away a corrupt journal (design doc 16.2).
    auto health = store_.check_health();
    if (health.is_err()) return R::err(RoutingError::StoreUnavailable);
    AuditRecord rec;
    rec.kind = AuditKind::OperatorAction;
    rec.actor = who.actor.str();
    rec.subject = "router";
    rec.reason = "resume";
    rec.detail = sanitize_text(reason, 120);
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    store_.audit().append(rec);
    set_mode(store_.open_recon_cases().empty() ? RouterMode::Normal : RouterMode::Degraded,
             "operator_resume");
    return R::ok(true);
}

Result<bool, RoutingError> Router::operator_set_route_active(const Principal& who,
                                                             const AccountId& account,
                                                             bool active,
                                                             const std::string& reason) {
    using R = Result<bool, RoutingError>;
    if (!may_change_config(who) || reason.empty()) {
        AuditRecord rec;
        rec.kind = AuditKind::OperatorDenied;
        rec.actor = who.actor.str();
        rec.subject = account.str();
        rec.reason = may_change_config(who) ? "missing_reason" : "insufficient_role";
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        store_.audit().append(rec);
        return R::err(RoutingError::AuthzDenied);
    }
    cfg_.set_route_active(account, active);
    AuditRecord rec;
    rec.kind = AuditKind::ConfigChanged;
    rec.actor = who.actor.str();
    rec.subject = account.str();
    rec.reason = active ? "route_enabled" : "route_disabled";
    rec.detail = sanitize_text(reason, 120);
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    store_.audit().append(rec);
    return R::ok(true);
}

// ---------------------------------------------------------------------------
// Read models

std::optional<AcceptedIntent> Router::command_status(const TenantId& t, const CommandId& c) const {
    const CommandIntent* intent = store_.find_intent(t, c);
    if (intent == nullptr) return std::nullopt;
    AcceptedIntent out;
    out.command_id = intent->command_id;
    out.chain_id = intent->chain_id;
    out.cl_ord_id = intent->cl_ord_id;
    out.route = intent->lp;
    out.delivery = intent->delivery;
    out.business = intent->business;
    return out;
}

std::size_t Router::open_unknown_outcomes() const {
    std::size_t n = 0;
    for (const auto& [k, intent] : store_.image().intents) {
        (void)k;
        if (intent.delivery == DeliveryState::UnknownTransport ||
            intent.delivery == DeliveryState::Reconciling)
            ++n;
    }
    return n;
}

std::size_t Router::open_recon_cases() const { return store_.open_recon_cases().size(); }

std::string Router::escalation_bundle(const Principal& who, const ReconCaseId& id) const {
    const ReconCase* c = store_.find_recon(id);
    if (c == nullptr) return "{}";
    const p03::ChainState* chain = store_.find_chain(c->chain_id);

    std::string out = "{\n";
    out += "  \"case_id\": \"" + c->case_id.str() + "\",\n";
    out += "  \"chain_id\": \"" + c->chain_id.str() + "\",\n";
    out += "  \"cause\": \"" + std::string(to_string(c->cause)) + "\",\n";
    out += "  \"state\": \"" + std::string(to_string(c->state)) + "\",\n";
    out += "  \"attempts\": " + std::to_string(c->attempt_count) + ",\n";
    out += "  \"command_id\": \"" + (c->command_id ? c->command_id->str() : "") + "\",\n";
    if (chain != nullptr) {
        out += "  \"account\": \"" + chain->account.str() + "\",\n";
        out += "  \"lp\": \"" + chain->lp.str() + "\",\n";
        out += "  \"accepted_cl_ord_id\": \"" + chain->accepted_cl_ord_id.str() + "\",\n";
        out += "  \"order_id\": \"" + (chain->lp_order_id ? chain->lp_order_id->str() : "") +
               "\",\n";
        out += "  \"status\": \"" + std::string(to_string(chain->status)) + "\",\n";
        out += "  \"cum_qty\": " + std::to_string(chain->cum_qty) + ",\n";
        out += "  \"leaves_qty\": " + std::to_string(chain->leaves_qty) + ",\n";
        out += "  \"state_version\": " + std::to_string(chain->version) + ",\n";
        out += "  \"state_hash\": \"" + chain->state_hash().short_hex() + "\",\n";
        out += "  \"fills\": [";
        for (std::size_t i = 0; i < chain->fills.size(); ++i) {
            if (i > 0) out += ", ";
            out += "{\"exec_id\": \"" + chain->fills[i].exec_id.str() + "\", \"qty\": " +
                   std::to_string(chain->fills[i].qty) + "}";
        }
        out += "],\n";
    }
    if (c->command_id && chain != nullptr) {
        if (const CommandIntent* ci = store_.find_intent(chain->tenant, *c->command_id)) {
            out += "  \"original_seq\": " +
                   (ci->original_seq ? std::to_string(*ci->original_seq) : std::string("null")) +
                   ",\n";
            out += "  \"delivery\": \"" + std::string(to_string(ci->delivery)) + "\",\n";
            out += "  \"risk_reservation\": \"" + ci->risk_reservation_id + "\",\n";
        }
    }
    out += "  \"queries\": [";
    for (std::size_t i = 0; i < c->query_ids.size(); ++i) {
        if (i > 0) out += ", ";
        out += "\"" + c->query_ids[i] + "\"";
    }
    out += "],\n";

    // OR-Q-15: raw frames, even redacted, are only shown to a principal that may
    // see them. Everyone else gets the digests.
    out += "  \"raw_frames\": [";
    bool first = true;
    for (const QuarantineEntry& q : store_.image().quarantine) {
        if (!q.chain_hint || *q.chain_hint != c->chain_id) continue;
        if (!first) out += ", ";
        first = false;
        if (may_view_raw_frames(who))
            out += "{\"hash\": \"" + q.raw_hash.short_hex() + "\", \"frame\": \"" +
                   sanitize_text(wire_text(q.redacted_frame), 400) + "\"}";
        else
            out += "{\"hash\": \"" + q.raw_hash.short_hex() + "\", \"frame\": \"<redacted>\"}";
    }
    out += "]\n}";
    return out;
}

}  // namespace router
