#include "jrnl/service.hpp"

namespace jrnl {
namespace {

CommitOutcome reject(const std::string& why) {
    CommitOutcome o;
    o.kind = CommitKind::Rejected;
    o.error = StoreError::Rejected;
    o.detail = why;
    return o;
}

Digest hash_of(const CanonMap& m) {
    return sha256(canonical_encode(CanonValue::map(m)));
}

// An execution identity that is not safe to dedup on. Section 9.2: a status
// snapshot carrying ExecID=0 is not a trade key, and treating it as one
// would either drop a real fill or invent one.
bool usable_exec_id(const ExecId& id) { return !id.empty() && id != "0"; }

// A plan is built from stream heads read outside the transaction, so a
// concurrent writer can move one between the read and the commit. Section
// 9.1 says to re-enter with the exact same idempotency key, which means
// rebuilding the plan rather than resubmitting the stale one. The key, the
// canonical hash and the derived transaction id are unchanged across
// attempts, so a retry can never become a second command.
constexpr int kStaleVersionAttempts = 8;

bool should_rebuild(const CommitOutcome& o, int attempt) {
    return o.error == StoreError::StaleVersion && attempt + 1 < kStaleVersionAttempts;
}

}  // namespace

Digest GatewayService::canonical_hash(const NewOrderRequest& r) {
    CanonMap m;
    m["kind"] = CanonValue::str("NEW");
    m["tenant"] = CanonValue::str(r.tenant);
    m["account"] = CanonValue::str(r.account);
    m["command_id"] = CanonValue::str(r.command_id);
    m["chain"] = CanonValue::str(r.chain);
    m["cl_ord_id"] = CanonValue::str(r.cl_ord_id);
    m["symbol"] = CanonValue::str(r.symbol);
    m["side"] = CanonValue::str(r.side);
    m["order_qty"] = CanonValue::integer(r.order_qty);
    m["limit_px"] = CanonValue::integer(r.limit_px);
    m["risk_domain"] = CanonValue::str(r.risk_domain);
    return hash_of(m);
}

Digest GatewayService::canonical_hash(const CancelRequest& r) {
    CanonMap m;
    m["kind"] = CanonValue::str("CANCEL");
    m["tenant"] = CanonValue::str(r.tenant);
    m["account"] = CanonValue::str(r.account);
    m["command_id"] = CanonValue::str(r.command_id);
    m["chain"] = CanonValue::str(r.chain);
    m["cl_ord_id"] = CanonValue::str(r.cl_ord_id);
    return hash_of(m);
}

Digest GatewayService::canonical_hash(const ReplaceRequest& r) {
    CanonMap m;
    m["kind"] = CanonValue::str("REPLACE");
    m["tenant"] = CanonValue::str(r.tenant);
    m["account"] = CanonValue::str(r.account);
    m["command_id"] = CanonValue::str(r.command_id);
    m["chain"] = CanonValue::str(r.chain);
    m["cl_ord_id"] = CanonValue::str(r.cl_ord_id);
    m["order_qty"] = CanonValue::integer(r.order_qty);
    m["limit_px"] = CanonValue::integer(r.limit_px);
    return hash_of(m);
}

bool GatewayService::gate_blocks(const TenantId& tenant, const RiskDomainId& domain,
                                 std::string* why) const {
    if (opt_.gate == nullptr) return false;
    if (opt_.gate->may_admit(tenant, domain)) return false;
    *why = opt_.gate->why_blocked(tenant, domain);
    if (opt_.metrics != nullptr) opt_.metrics->inc(metric::kAdmissionBlocked);
    return true;
}

CommitOutcome GatewayService::set_risk_limit(const TenantId& tenant, const RiskDomainId& domain,
                                             std::int64_t limit, FenceToken token) {
    const StreamId stream = risk_stream(domain);
    const StreamHead head = store_.head(tenant, stream);

    CanonMap key;
    key["kind"] = CanonValue::str("LIMIT");
    key["domain"] = CanonValue::str(domain);
    key["limit"] = CanonValue::integer(limit);
    key["version"] = CanonValue::integer(static_cast<std::int64_t>(head.version));

    TxPlan plan;
    plan.tenant = tenant;
    plan.tx_id = derive_tx_id(tenant, "limit", hash_of(key));
    plan.fence_domain = domain;
    plan.fence_token = token;

    AppendEvent e;
    e.stream = stream;
    e.expected_version = head.version;
    e.event_id = plan.tx_id + ":0";
    e.event_type = std::string(ev::kRiskLimitSet);
    e.reducer_version = std::string(reducer_version::kRiskV1);
    e.policy_version = opt_.policy_version;
    e.ordinal = 0;
    e.payload = Payload().s("domain", domain).i("limit", limit).build();
    plan.events.push_back(std::move(e));

    AuditPlan a;
    a.category = "risk.limit";
    a.detail = "limit for " + domain + " set to " + std::to_string(limit);
    a.data = Payload().s("domain", domain).i("limit", limit).build();
    plan.audit.push_back(std::move(a));

    return store_.apply(plan);
}

CommitOutcome GatewayService::submit_new_once(const NewOrderRequest& r, FenceToken token) {
    std::string why;
    if (gate_blocks(r.tenant, r.risk_domain, &why)) {
        return reject("admission blocked: " + why);
    }
    if (r.order_qty <= 0 || !amount_in_range(r.order_qty)) {
        return reject("order quantity out of admissible range");
    }

    const Digest key = canonical_hash(r);

    // Idempotent short circuit outside the transaction. The store repeats
    // the check under the lock; this one only saves work and keeps the
    // common retry path cheap.
    const CommandRecord prior = store_.lookup(r.tenant, r.account, r.command_id);
    if (prior.found) {
        CommitOutcome o;
        if (prior.canonical_hash == key) {
            o.kind = CommitKind::Replayed;
            o.tx = prior.tx;
            o.detail = "command already durable";
        } else {
            o.kind = CommitKind::Conflict;
            o.error = StoreError::Conflict;
            o.detail = "command id reused with different content";
        }
        return o;
    }

    const StreamId ostream = order_stream(r.chain);
    const StreamId rstream = risk_stream(r.risk_domain);
    const StreamHead ohead = store_.head(r.tenant, ostream);
    const StreamHead rhead = store_.head(r.tenant, rstream);
    if (!rhead.exists) return reject("risk domain " + r.risk_domain + " has no limit set");

    // Dry run. The reducers are pure, so asking them here costs nothing and
    // turns an admission failure into a clean rejection instead of a
    // transaction that begins and then aborts.
    {
        RiskDomainState rs;
        if (!store_.load_risk(r.tenant, r.risk_domain, &rs)) {
            return reject("risk domain projection missing");
        }
        if (r.order_qty > rs.headroom()) {
            if (opt_.metrics != nullptr) opt_.metrics->inc(metric::kAdmissionBlocked);
            return reject("insufficient risk headroom: need " + std::to_string(r.order_qty) +
                          ", have " + std::to_string(rs.headroom()));
        }
    }

    TxPlan plan;
    plan.tenant = r.tenant;
    plan.tx_id = derive_tx_id(r.tenant, "new", key);
    plan.fence_domain = r.risk_domain;
    plan.fence_token = token;

    // Risk first, then the order. Section 9.1 reserves before it reduces the
    // order intent, and the ordinals record that order in the manifest.
    AppendEvent reserve;
    reserve.stream = rstream;
    reserve.expected_version = rhead.version;
    reserve.event_id = plan.tx_id + ":0";
    reserve.event_type = std::string(ev::kRiskReserved);
    reserve.reducer_version = std::string(reducer_version::kRiskV1);
    reserve.policy_version = opt_.policy_version;
    reserve.ordinal = 0;
    reserve.payload = Payload()
                          .s("reservation_id", reservation_for(r.chain))
                          .s("account", r.account)
                          .s("chain", r.chain)
                          .i("amount", r.order_qty)
                          .build();
    plan.events.push_back(std::move(reserve));

    AppendEvent accept;
    accept.stream = ostream;
    accept.expected_version = ohead.version;
    accept.event_id = plan.tx_id + ":1";
    accept.event_type = std::string(ev::kOrderAccepted);
    accept.reducer_version = std::string(store_.reducers().current_order_version());
    accept.policy_version = opt_.policy_version;
    accept.ordinal = 1;
    accept.payload = Payload()
                         .s("chain", r.chain)
                         .s("account", r.account)
                         .s("cl_ord_id", r.cl_ord_id)
                         .s("symbol", r.symbol)
                         .s("side", r.side)
                         .s("command_id", r.command_id)
                         .s("risk_domain", r.risk_domain)
                         .i("order_qty", r.order_qty)
                         .i("limit_px", r.limit_px)
                         .build();
    plan.events.push_back(std::move(accept));

    CommandIntentPlan intent;
    intent.account = r.account;
    intent.command_id = r.command_id;
    intent.chain = r.chain;
    intent.canonical_hash = key;
    intent.reservation = reservation_for(r.chain);
    intent.decision = "ACCEPTED";
    plan.intent = intent;

    OutboxIntent effect;
    effect.outbox_id = plan.tx_id + ":out";
    effect.account = r.account;
    effect.command_id = r.command_id;
    effect.session_key = r.session_key;
    effect.payload = r.fix_payload;
    effect.payload_hash = sha256(r.fix_payload);
    plan.effects.push_back(std::move(effect));

    AuditPlan a;
    a.category = "order.new";
    a.detail = "accepted " + r.command_id + " on chain " + r.chain;
    a.data = Payload().s("chain", r.chain).i("qty", r.order_qty).s("hash", to_hex(key)).build();
    plan.audit.push_back(std::move(a));

    const CommitOutcome out = store_.apply(plan);
    if (opt_.metrics != nullptr && out.kind == CommitKind::Unknown) {
        opt_.metrics->inc(metric::kDbCommitUnknown);
    }
    return out;
}

CommitOutcome GatewayService::submit_cancel_once(const CancelRequest& r, FenceToken token) {
    std::string why;
    if (gate_blocks(r.tenant, r.risk_domain, &why)) {
        return reject("admission blocked: " + why);
    }

    const Digest key = canonical_hash(r);
    const CommandRecord prior = store_.lookup(r.tenant, r.account, r.command_id);
    if (prior.found) {
        CommitOutcome o;
        o.kind = prior.canonical_hash == key ? CommitKind::Replayed : CommitKind::Conflict;
        if (o.kind == CommitKind::Conflict) o.error = StoreError::Conflict;
        o.tx = prior.tx;
        return o;
    }

    const StreamId ostream = order_stream(r.chain);
    const StreamHead ohead = store_.head(r.tenant, ostream);
    OrderState os;
    if (!store_.load_order(r.tenant, r.chain, &os)) return reject("unknown order chain");
    if (is_terminal(os.status)) {
        // JI-38: no cancel from a post-terminal state, including one that
        // recovery replayed us into.
        return reject("order is already terminal in status " + std::string(to_string(os.status)));
    }

    TxPlan plan;
    plan.tenant = r.tenant;
    plan.tx_id = derive_tx_id(r.tenant, "cancel", key);
    plan.fence_domain = r.risk_domain;
    plan.fence_token = token;

    AppendEvent e;
    e.stream = ostream;
    e.expected_version = ohead.version;
    e.event_id = plan.tx_id + ":0";
    e.event_type = std::string(ev::kCancelRequested);
    e.reducer_version = ohead.reducer_version;
    e.policy_version = opt_.policy_version;
    e.ordinal = 0;
    e.payload = Payload().s("cl_ord_id", r.cl_ord_id).s("chain", r.chain).build();
    plan.events.push_back(std::move(e));

    // No risk event. JI-10: a pending cancel releases nothing, because the
    // order can still trade while the cancel is in flight.

    CommandIntentPlan intent;
    intent.account = r.account;
    intent.command_id = r.command_id;
    intent.chain = r.chain;
    intent.canonical_hash = key;
    intent.decision = "CANCEL_REQUESTED";
    plan.intent = intent;

    OutboxIntent effect;
    effect.outbox_id = plan.tx_id + ":out";
    effect.account = r.account;
    effect.command_id = r.command_id;
    effect.session_key = r.session_key;
    effect.payload = r.fix_payload;
    effect.payload_hash = sha256(r.fix_payload);
    plan.effects.push_back(std::move(effect));

    AuditPlan a;
    a.category = "order.cancel";
    a.detail = "cancel requested for chain " + r.chain;
    a.data = Payload().s("chain", r.chain).s("cl_ord_id", r.cl_ord_id).build();
    plan.audit.push_back(std::move(a));

    return store_.apply(plan);
}

CommitOutcome GatewayService::submit_replace_once(const ReplaceRequest& r, FenceToken token) {
    std::string why;
    if (gate_blocks(r.tenant, r.risk_domain, &why)) {
        return reject("admission blocked: " + why);
    }
    if (r.order_qty <= 0 || !amount_in_range(r.order_qty)) {
        return reject("replacement quantity out of admissible range");
    }

    const Digest key = canonical_hash(r);
    const CommandRecord prior = store_.lookup(r.tenant, r.account, r.command_id);
    if (prior.found) {
        CommitOutcome o;
        o.kind = prior.canonical_hash == key ? CommitKind::Replayed : CommitKind::Conflict;
        if (o.kind == CommitKind::Conflict) o.error = StoreError::Conflict;
        o.tx = prior.tx;
        return o;
    }

    const StreamId ostream = order_stream(r.chain);
    const StreamHead ohead = store_.head(r.tenant, ostream);
    OrderState os;
    if (!store_.load_order(r.tenant, r.chain, &os)) return reject("unknown order chain");
    if (is_terminal(os.status)) {
        return reject("order is already terminal in status " + std::string(to_string(os.status)));
    }
    if (r.order_qty < os.cum_qty) {
        return reject("replacement quantity is below the quantity already traded");
    }

    TxPlan plan;
    plan.tenant = r.tenant;
    plan.tx_id = derive_tx_id(r.tenant, "replace", key);
    plan.fence_domain = r.risk_domain;
    plan.fence_token = token;

    AppendEvent e;
    e.stream = ostream;
    e.expected_version = ohead.version;
    e.event_id = plan.tx_id + ":0";
    e.event_type = std::string(ev::kReplaceRequested);
    e.reducer_version = ohead.reducer_version;
    e.policy_version = opt_.policy_version;
    e.ordinal = 0;
    e.payload = Payload()
                    .s("cl_ord_id", r.cl_ord_id)
                    .i("order_qty", r.order_qty)
                    .i("limit_px", r.limit_px)
                    .build();
    plan.events.push_back(std::move(e));

    CommandIntentPlan intent;
    intent.account = r.account;
    intent.command_id = r.command_id;
    intent.chain = r.chain;
    intent.canonical_hash = key;
    intent.decision = "REPLACE_REQUESTED";
    plan.intent = intent;

    OutboxIntent effect;
    effect.outbox_id = plan.tx_id + ":out";
    effect.account = r.account;
    effect.command_id = r.command_id;
    effect.session_key = r.session_key;
    effect.payload = r.fix_payload;
    effect.payload_hash = sha256(r.fix_payload);
    plan.effects.push_back(std::move(effect));

    AuditPlan a;
    a.category = "order.replace";
    a.detail = "replace requested for chain " + r.chain;
    a.data = Payload().s("chain", r.chain).i("order_qty", r.order_qty).build();
    plan.audit.push_back(std::move(a));

    return store_.apply(plan);
}

CommitOutcome GatewayService::quarantine(const ExecutionReport& r, const std::string& reason,
                                         FenceToken token) {
    TxPlan plan;
    plan.tenant = r.tenant;
    plan.quarantine = true;
    plan.quarantine_reason = reason;
    plan.fence_domain = r.risk_domain;
    plan.fence_token = token;

    InboxReceipt rec;
    rec.session_key = r.session_key;
    rec.session_epoch = r.session_epoch;
    rec.receipt_id = r.receipt_id;
    rec.payload_hash = sha256(r.raw);
    plan.receipt = rec;

    CanonMap k;
    k["session"] = CanonValue::str(r.session_key);
    k["receipt"] = CanonValue::integer(static_cast<std::int64_t>(r.receipt_id));
    k["epoch"] = CanonValue::integer(static_cast<std::int64_t>(r.session_epoch));
    plan.tx_id = derive_tx_id(r.tenant, "quarantine", hash_of(k));

    RecoveryCasePlan c;
    c.case_id = plan.tx_id;
    c.chain = r.chain;
    c.reason_code = "CONTRADICTORY_REPORT";
    c.evidence_hash = sha256(r.raw);
    c.status = "OPEN";
    plan.cases.push_back(std::move(c));

    AuditPlan a;
    a.category = "inbound.quarantine";
    a.detail = reason;
    a.data = Payload()
                 .s("chain", r.chain)
                 .s("exec_id", r.exec_id)
                 .s("raw_hash", to_hex(sha256(r.raw)))
                 .build();
    plan.audit.push_back(std::move(a));

    if (opt_.metrics != nullptr) opt_.metrics->inc(metric::kQuarantineTotal);
    return store_.apply(plan);
}

CommitOutcome GatewayService::apply_report_once(const ExecutionReport& r, FenceToken token) {
    const Digest raw_hash = sha256(r.raw);

    // Transport dedup first, because a redelivery must be cheap and must not
    // depend on the business state being loadable.
    InboxReceipt rec;
    rec.session_key = r.session_key;
    rec.session_epoch = r.session_epoch;
    rec.receipt_id = r.receipt_id;
    rec.payload_hash = raw_hash;
    const ReceiptRecord prior = store_.lookup_receipt(r.tenant, rec);
    if (prior.found) {
        CommitOutcome o;
        if (prior.payload_hash == raw_hash) {
            o.kind = CommitKind::Replayed;
            o.tx = prior.tx;
            o.detail = "receipt already applied in state " + prior.apply_state;
            if (opt_.metrics != nullptr) opt_.metrics->inc(metric::kReceiptRedelivery);
        } else {
            o.kind = CommitKind::Quarantined;
            o.error = StoreError::Quarantined;
            o.detail = "receipt redelivered with different content";
        }
        return o;
    }

    OrderState os;
    if (!store_.load_order(r.tenant, r.chain, &os)) {
        return quarantine(r, "report references an unknown order chain " + r.chain, token);
    }
    RiskDomainState rs;
    if (!store_.load_risk(r.tenant, r.risk_domain, &rs)) {
        return quarantine(r, "report references an unknown risk domain " + r.risk_domain, token);
    }

    const StreamId ostream = order_stream(r.chain);
    const StreamId rstream = risk_stream(r.risk_domain);
    const StreamHead ohead = store_.head(r.tenant, ostream);
    const StreamHead rhead = store_.head(r.tenant, rstream);
    const ReservationId res = reservation_for(r.chain);

    const IOrderReducer* ored = store_.reducers().order(ohead.reducer_version);
    const IRiskReducer* rred = store_.reducers().risk(reducer_version::kRiskV1);
    if (ored == nullptr || rred == nullptr) {
        return quarantine(r, "no reducer available for this stream", token);
    }

    TxPlan plan;
    plan.tenant = r.tenant;
    plan.fence_domain = r.risk_domain;
    plan.fence_token = token;
    plan.receipt = rec;

    CanonMap txkey;
    txkey["session"] = CanonValue::str(r.session_key);
    txkey["epoch"] = CanonValue::integer(static_cast<std::int64_t>(r.session_epoch));
    txkey["receipt"] = CanonValue::integer(static_cast<std::int64_t>(r.receipt_id));
    plan.tx_id = derive_tx_id(r.tenant, "report", hash_of(txkey));

    // Held amount for this chain, which is what a terminal outcome releases
    // and what a fill consumes from.
    std::int64_t held = 0;
    {
        const auto it = rs.reservations.find(res);
        if (it != rs.reservations.end() && it->second.state == ReservationState::Held) {
            held = it->second.amount;
        }
    }

    int ordinal = 0;
    OrderState dry_order = os;
    RiskDomainState dry_risk = rs;
    ReduceError rerr;

    auto add_order_event = [&](std::string_view type, CanonValue payload) {
        AppendEvent e;
        e.stream = ostream;
        e.expected_version = ohead.version;
        e.event_id = plan.tx_id + ":" + std::to_string(ordinal);
        e.event_type = std::string(type);
        e.reducer_version = ohead.reducer_version;
        e.policy_version = opt_.policy_version;
        e.ordinal = ordinal++;
        e.payload = std::move(payload);
        plan.events.push_back(std::move(e));
    };
    auto add_risk_event = [&](std::string_view type, CanonValue payload) {
        AppendEvent e;
        e.stream = rstream;
        // Each risk event in this transaction advances the risk stream, so
        // the expected version counts the ones already added.
        std::size_t prior_risk = 0;
        for (const AppendEvent& x : plan.events) {
            if (x.stream == rstream) ++prior_risk;
        }
        e.expected_version = rhead.version + prior_risk;
        e.event_id = plan.tx_id + ":" + std::to_string(ordinal);
        e.event_type = std::string(type);
        e.reducer_version = std::string(reducer_version::kRiskV1);
        e.policy_version = opt_.policy_version;
        e.ordinal = ordinal++;
        e.payload = std::move(payload);
        plan.events.push_back(std::move(e));
    };

    if (r.exec_type == "TRADE") {
        if (!usable_exec_id(r.exec_id)) {
            return quarantine(r, "trade report carries no usable execution identity", token);
        }
        if (r.last_qty <= 0 || !amount_in_range(r.last_qty)) {
            return quarantine(r, "trade report carries an inadmissible quantity", token);
        }

        // Business dedup, before anything is built.
        const ExecKeyRecord seen = store_.lookup_exec_key(r.tenant, r.lp_namespace, r.exec_id);
        const Digest fp = exec_fingerprint(r.chain, r.last_qty, r.last_px, r.exec_type);
        if (seen.found) {
            CommitOutcome o;
            if (seen.fingerprint == fp) {
                o.kind = CommitKind::Replayed;
                o.tx = seen.tx;
                o.detail = "execution already applied";
            } else {
                // JI-07. Same identity, different economics: one of the two
                // is wrong and the engine does not get to choose.
                return quarantine(r, "execution id " + r.exec_id +
                                         " reused with a different business fingerprint",
                                  token);
            }
            return o;
        }

        const CanonValue fill_payload = Payload()
                                            .s("exec_id", r.exec_id)
                                            .i("last_qty", r.last_qty)
                                            .i("last_px", r.last_px)
                                            .build();
        if (!ored->apply(&dry_order, ev::kFillApplied, fill_payload, &rerr)) {
            // The overfill and post-terminal cases land here, before any
            // transaction opens (JI-08, JI-40).
            return quarantine(r, "fill contradicts the order state: " + rerr.detail, token);
        }

        const std::int64_t consume = r.last_qty < held ? r.last_qty : held;
        if (consume > 0) {
            const CanonValue cpay =
                Payload().s("reservation_id", res).i("amount", consume).build();
            if (!rred->apply(&dry_risk, ev::kRiskConsumed, cpay, &rerr)) {
                return quarantine(r, "fill cannot be reconciled with the risk hold: " +
                                         rerr.detail,
                                  token);
            }
            add_order_event(ev::kFillApplied, fill_payload);
            add_risk_event(ev::kRiskConsumed, cpay);
        } else {
            add_order_event(ev::kFillApplied, fill_payload);
        }

        ExecKeyPlan k;
        k.lp_namespace = r.lp_namespace;
        k.semantic_exec_id = r.exec_id;
        k.fingerprint = fp;
        k.chain = r.chain;
        k.last_qty = r.last_qty;
        k.last_px = r.last_px;
        plan.exec_key = k;

        // A fill that fully completes the order releases whatever is still
        // held, so a partially filled order that completes does not leave a
        // dangling reservation.
        if (dry_order.leaves_qty == 0) {
            std::int64_t remaining = 0;
            const auto it = dry_risk.reservations.find(res);
            if (it != dry_risk.reservations.end() && it->second.state == ReservationState::Held) {
                remaining = it->second.amount;
            }
            if (remaining > 0) {
                const CanonValue rp =
                    Payload().s("reservation_id", res).i("amount", remaining).build();
                if (rred->apply(&dry_risk, ev::kRiskReleased, rp, &rerr)) {
                    add_risk_event(ev::kRiskReleased, rp);
                }
            }
        }
    } else if (r.exec_type == "CANCELED" || r.exec_type == "REJECTED" ||
               r.exec_type == "EXPIRED") {
        const std::string_view type = r.exec_type == "CANCELED"  ? ev::kCancelAccepted
                                      : r.exec_type == "REJECTED" ? ev::kOrderRejected
                                                                  : ev::kOrderExpired;
        const CanonValue payload = Payload().s("reason", r.exec_type).build();
        if (!ored->apply(&dry_order, type, payload, &rerr)) {
            return quarantine(r, "terminal report contradicts the order state: " + rerr.detail,
                              token);
        }
        add_order_event(type, payload);
        if (held > 0) {
            const CanonValue rp = Payload().s("reservation_id", res).i("amount", held).build();
            if (!rred->apply(&dry_risk, ev::kRiskReleased, rp, &rerr)) {
                return quarantine(r, "cannot release the risk hold: " + rerr.detail, token);
            }
            add_risk_event(ev::kRiskReleased, rp);
        }
    } else if (r.exec_type == "REPLACED") {
        const CanonValue payload = Payload().s("reason", "REPLACED").build();
        if (!ored->apply(&dry_order, ev::kReplaceAccepted, payload, &rerr)) {
            return quarantine(r, "replace report contradicts the order state: " + rerr.detail,
                              token);
        }
        add_order_event(ev::kReplaceAccepted, payload);

        // The hold is re-struck to match the new remaining quantity: the old
        // one is released in full and a new one taken, both inside this one
        // transaction, so the domain is never momentarily over-committed.
        const std::int64_t want = dry_order.leaves_qty;
        if (held > 0) {
            const CanonValue rp = Payload().s("reservation_id", res).i("amount", held).build();
            if (!rred->apply(&dry_risk, ev::kRiskReleased, rp, &rerr)) {
                return quarantine(r, "cannot release the previous hold: " + rerr.detail, token);
            }
            add_risk_event(ev::kRiskReleased, rp);
        }
        if (want > 0) {
            const ReservationId nres = res + ":v" + std::to_string(dry_order.fill_count + 1) +
                                       ":" + std::to_string(want);
            const CanonValue np = Payload()
                                      .s("reservation_id", nres)
                                      .s("account", r.account)
                                      .s("chain", r.chain)
                                      .i("amount", want)
                                      .build();
            if (!rred->apply(&dry_risk, ev::kRiskReserved, np, &rerr)) {
                return quarantine(r, "cannot re-reserve for the replaced order: " + rerr.detail,
                                  token);
            }
            add_risk_event(ev::kRiskReserved, np);
        }
    } else {
        return quarantine(r, "unsupported execution type " + r.exec_type, token);
    }

    // Any valid report about a command is evidence that the command reached
    // the counterparty, so its outbound effect moves to CONFIRMED in the
    // same transaction that records the evidence.
    if (!r.originating_command.empty()) {
        OutboxRow row;
        if (store_.outbox_for_command(r.tenant, r.account, r.originating_command, &row) &&
            row.state != "CONFIRMED") {
            OutboxTransition t;
            t.outbox_id = row.outbox_id;
            t.new_state = "CONFIRMED";
            t.reason = "counterparty report " + r.exec_id;
            plan.outbox_updates.push_back(std::move(t));
        }
    }

    AuditPlan a;
    a.category = "inbound.applied";
    a.detail = r.exec_type + " for chain " + r.chain;
    a.data = Payload()
                 .s("chain", r.chain)
                 .s("exec_id", r.exec_id)
                 .i("last_qty", r.last_qty)
                 .s("raw_hash", to_hex(raw_hash))
                 .build();
    plan.audit.push_back(std::move(a));

    return store_.apply(plan);
}

// --- Retrying entry points --------------------------------------------------
//
// Each one rebuilds its plan from fresh stream heads and re-applies under
// the same idempotency key when a concurrent writer moved a stream. Any
// other outcome is returned as it stands.

CommitOutcome GatewayService::submit_new(const NewOrderRequest& r, FenceToken token) {
    CommitOutcome o;
    for (int attempt = 0; attempt < kStaleVersionAttempts; ++attempt) {
        o = submit_new_once(r, token);
        if (!should_rebuild(o, attempt)) break;
    }
    return o;
}

CommitOutcome GatewayService::submit_cancel(const CancelRequest& r, FenceToken token) {
    CommitOutcome o;
    for (int attempt = 0; attempt < kStaleVersionAttempts; ++attempt) {
        o = submit_cancel_once(r, token);
        if (!should_rebuild(o, attempt)) break;
    }
    return o;
}

CommitOutcome GatewayService::submit_replace(const ReplaceRequest& r, FenceToken token) {
    CommitOutcome o;
    for (int attempt = 0; attempt < kStaleVersionAttempts; ++attempt) {
        o = submit_replace_once(r, token);
        if (!should_rebuild(o, attempt)) break;
    }
    return o;
}

CommitOutcome GatewayService::apply_report(const ExecutionReport& r, FenceToken token) {
    CommitOutcome o;
    for (int attempt = 0; attempt < kStaleVersionAttempts; ++attempt) {
        o = apply_report_once(r, token);
        if (!should_rebuild(o, attempt)) break;
    }
    return o;
}

StoreError GatewayService::ack_delivery(const ExecutionReport& r) {
    if (opt_.link == nullptr) return StoreError::None;
    return opt_.link->ack_delivery(r.session_key, r.session_epoch, r.receipt_id);
}

}  // namespace jrnl
