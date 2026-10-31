#include "jrnl/store.hpp"

#include <algorithm>

#include "jrnl/failpoint.hpp"
#include "jrnl/schema.hpp"

namespace jrnl {
namespace {

// The chain value stored on a row and in stream_heads. It binds the previous
// chain value, the event bytes AND the state the reducer produced, so that a
// divergence in any of the three is visible to replay. Section 12 asks replay
// to check prev_state_hash and the resulting state separately; binding both
// into one value checks the same two things and additionally detects a
// reordering of otherwise valid rows.
Digest event_chain_hash(const Digest& prev, const Digest& canonical_hash,
                        const Digest& state_dig, const StreamId& stream, Version version,
                        const std::string& reducer_version) {
    const std::string bind = to_hex(canonical_hash) + to_hex(state_dig);
    return chain_step(prev, bind, stream, version, reducer_version);
}

// Canonical envelope of an event: payload plus the metadata whose alteration
// would change what the event means. Hashing the payload alone would let a
// tampered event_type through.
CanonValue event_envelope(const TenantId& tenant, const AppendEvent& e, Version version) {
    CanonMap m;
    m["tenant"] = CanonValue::str(tenant);
    m["stream"] = CanonValue::str(e.stream);
    m["version"] = CanonValue::integer(static_cast<std::int64_t>(version));
    m["event_id"] = CanonValue::str(e.event_id);
    m["event_type"] = CanonValue::str(e.event_type);
    m["schema_version"] = CanonValue::integer(e.schema_version);
    m["reducer_version"] = CanonValue::str(e.reducer_version);
    m["policy_version"] = CanonValue::integer(static_cast<std::int64_t>(e.policy_version));
    m["payload"] = e.payload;
    return CanonValue::map(std::move(m));
}

bool read_outbox_row(Stmt& s, OutboxRow* r) {
    r->outbox_id = s.col_text(0);
    r->tenant = s.col_text(1);
    r->account = s.col_text(2);
    r->command_id = s.col_text(3);
    r->session_key = s.col_text(4);
    if (!s.col_hex(5, &r->payload_hash)) return false;
    r->payload = s.col_text(6);
    r->state = s.col_text(7);
    if (s.col_is_null(8)) r->original_seq.reset();
    else r->original_seq = s.col_u64(8);
    r->claim_owner = s.col_text(9);
    r->claim_token = s.col_u64(10);
    r->claim_expires_at = s.col_i64(11);
    r->attempts = static_cast<int>(s.col_i64(12));
    r->last_error = s.col_text(13);
    return true;
}

constexpr const char* kOutboxCols =
    "outbox_id,tenant_id,account_id,command_id,session_key,original_payload_hash,"
    "original_payload,state,p01_original_seq,claim_owner,claim_token,claim_expires_at,"
    "attempts,last_error";

}  // namespace

TxId derive_tx_id(const TenantId& tenant, const std::string& scope, const Digest& key) {
    Sha256 h;
    h.update(tenant);
    h.update(std::string_view("\x1f", 1));
    h.update(scope);
    h.update(std::string_view("\x1f", 1));
    h.update(key);
    return to_hex(h.finish()).substr(0, 32);
}

Digest exec_fingerprint(const OrderChainId& chain, std::int64_t last_qty, std::int64_t last_px,
                        const std::string& exec_type) {
    CanonMap m;
    m["chain"] = CanonValue::str(chain);
    m["last_qty"] = CanonValue::integer(last_qty);
    m["last_px"] = CanonValue::integer(last_px);
    m["exec_type"] = CanonValue::str(exec_type);
    return sha256(canonical_encode(CanonValue::map(std::move(m))));
}

// ---------------------------------------------------------------------------

SqliteAtomicStore::~SqliteAtomicStore() = default;

StoreError SqliteAtomicStore::open(const Options& opt, std::unique_ptr<SqliteAtomicStore>* out,
                                   std::string* why) {
    auto store = std::unique_ptr<SqliteAtomicStore>(new SqliteAtomicStore());
    store->opt_ = opt;

    Db::Options dopt;
    dopt.path = opt.path;
    dopt.create_if_missing = opt.create_if_missing;
    dopt.busy_timeout_ms = opt.busy_timeout_ms;
    dopt.journal_mode = opt.journal_mode;
    dopt.synchronous = opt.synchronous;

    StoreError e = Db::open(dopt, &store->db_, why);
    if (e != StoreError::None) return e;

    e = apply_schema(*store->db_, why);
    if (e != StoreError::None) return e;

    if (opt.clock != nullptr) {
        store->clock_ = opt.clock;
    } else {
        store->owned_clock_ = std::make_unique<SystemClock>();
        store->clock_ = store->owned_clock_.get();
    }
    store->reducers_ = opt.reducers != nullptr ? opt.reducers : &ReducerRegistry::instance();

    *out = std::move(store);
    return StoreError::None;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

CommitOutcome SqliteAtomicStore::apply(const TxPlan& plan) {
    std::lock_guard<std::mutex> serialise(tx_mu_);
    CommitOutcome out;
    last_detail_.clear();

    // Validation before BEGIN. Section 9.1: canonicalise and validate with no
    // side effects, so that W01 and W02 leave nothing behind by definition.
    if (plan.tenant.empty()) {
        out.kind = CommitKind::Rejected;
        out.detail = "empty tenant";
        last_detail_ = out.detail;
        return out;
    }
    if (plan.tx_id.empty()) {
        out.kind = CommitKind::Rejected;
        out.detail = "empty transaction id";
        last_detail_ = out.detail;
        return out;
    }
    if (opt_.max_events_per_tx > 0 && plan.events.size() > opt_.max_events_per_tx) {
        // Bounded work per transaction (section 19). An unbounded plan would
        // hold row locks for an unbounded time, which is a liveness failure
        // dressed up as a feature.
        out.kind = CommitKind::Rejected;
        out.error = StoreError::Rejected;
        out.detail = "transaction exceeds max_events_per_tx";
        last_detail_ = out.detail;
        return out;
    }
    for (const AppendEvent& e : plan.events) {
        if (e.stream.empty() || e.event_id.empty() || e.event_type.empty() ||
            e.reducer_version.empty()) {
            out.kind = CommitKind::Rejected;
            out.detail = "incomplete event descriptor";
            last_detail_ = out.detail;
            return out;
        }
        if (e.schema_version <= 0) {
            out.kind = CommitKind::Rejected;
            out.detail = "schema_version must be positive";
            last_detail_ = out.detail;
            return out;
        }
    }

    if (JRNL_FP(fp::point::kBeforeBegin) == fp::Action::ReturnError) {
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        out.detail = "injected failure before BEGIN";
        last_detail_ = out.detail;
        return out;
    }

    // Bounded retry on a retryable abort, always with the same plan and
    // therefore the same idempotency key (JI-27).
    constexpr int kMaxAttempts = 5;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        TxGuard tx(*db_);
        StoreError e = tx.begin();
        if (is_retryable(e) && attempt < kMaxAttempts) continue;
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            out.detail = "cannot begin transaction: " + db_->last_error();
            last_detail_ = out.detail;
            return out;
        }

        std::string detail;
        CommitOutcome inner;
        try {
            JRNL_FP(fp::point::kAfterBegin);
            inner = apply_locked(plan, &detail);
        } catch (const fp::InjectedFault& f) {
            // The RAII guard rolls back on the way out. G02-11 asks for
            // exactly this: a reducer that throws leaves no side effects.
            out.kind = CommitKind::Rejected;
            out.error = StoreError::IoError;
            out.detail = std::string("exception during transaction: ") + f.what();
            last_detail_ = out.detail;
            return out;
        }
        inner.detail = detail;

        // A quarantine plan is the one non-Committed outcome that must still
        // reach disk. Section 9.2: a contradictory receipt is recorded as
        // QUARANTINED with a case and an audit line, because rolling the
        // evidence back would leave the receipt to be redelivered for ever
        // and would lose the only record that the contradiction happened.
        const bool commit_quarantine =
            plan.quarantine && inner.kind == CommitKind::Quarantined;
        if (inner.kind != CommitKind::Committed && !commit_quarantine) {
            if (is_retryable(inner.error) && attempt < kMaxAttempts) continue;
            // The guard rolls back. Nothing partial survives.
            last_detail_ = detail;
            return inner;
        }

        if (JRNL_FP(fp::point::kBeforeCommit) == fp::Action::ReturnError) {
            out.kind = CommitKind::Rejected;
            out.error = StoreError::IoError;
            out.detail = "injected failure before COMMIT";
            last_detail_ = out.detail;
            return out;
        }

        // W11. The commit is performed and then the caller is told the
        // outcome is unknown, which is precisely the situation the window
        // describes: the data may be durable and the client cannot tell.
        // JI-15 forbids reading this as either a rollback or an acceptance.
        const fp::Action commit_fault = JRNL_FP(fp::point::kDuringCommit);

        e = tx.commit();
        if (e != StoreError::None) {
            if (is_retryable(e) && attempt < kMaxAttempts) continue;
            out.kind = is_ambiguous(e) ? CommitKind::Unknown : CommitKind::Rejected;
            out.error = e;
            out.detail = "commit failed: " + db_->last_error();
            last_detail_ = out.detail;
            return out;
        }

        if (commit_fault == fp::Action::ReturnError) {
            out.kind = CommitKind::Unknown;
            out.error = StoreError::CommitUnknown;
            out.tx = plan.tx_id;
            out.detail = "commit acknowledgement lost";
            last_detail_ = out.detail;
            return out;
        }

        JRNL_FP(fp::point::kAfterCommit);

        inner.tx = plan.tx_id;
        last_detail_ = detail;
        return inner;
    }

    out.kind = CommitKind::Rejected;
    out.error = StoreError::Serialization;
    out.detail = "retry budget exhausted";
    last_detail_ = out.detail;
    return out;
}

StoreError SqliteAtomicStore::check_fence(const TxPlan& plan, std::string* detail) {
    if (plan.fence_domain.empty()) return StoreError::None;  // unfenced maintenance work
    Stmt s;
    StoreError e = db_->prepare(
        "SELECT token, lease_owner FROM owner_fences WHERE tenant_id=?1 AND domain_id=?2;", &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, plan.tenant);
    s.bind_text(2, plan.fence_domain);
    bool row = false;
    e = s.step(&row);
    if (e != StoreError::None) return e;
    if (!row) {
        *detail = "no fence held for domain " + plan.fence_domain;
        return StoreError::NotOwner;
    }
    const FenceToken current = s.col_u64(0);
    if (plan.fence_token != current) {
        // JI-21. A token that is not the current one belongs to a writer that
        // has been superseded, and its mutation must fail here rather than be
        // noticed later.
        *detail = "stale fencing token " + std::to_string(plan.fence_token) + ", current is " +
                  std::to_string(current);
        return StoreError::NotOwner;
    }
    return StoreError::None;
}

CommitOutcome SqliteAtomicStore::apply_locked(const TxPlan& plan, std::string* detail) {
    CommitOutcome out;
    const std::int64_t now = clock_->now_ms();

    StoreError e = check_fence(plan, detail);
    if (e != StoreError::None) {
        out.kind = CommitKind::Rejected;
        out.error = e;
        return out;
    }
    if (JRNL_FP(fp::point::kAfterFenceCheck) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_fence_check";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    // --- Dedup: transport receipt -----------------------------------------
    if (plan.receipt.has_value()) {
        const ReceiptRecord prior = lookup_receipt(plan.tenant, *plan.receipt);
        if (prior.found) {
            if (prior.payload_hash == plan.receipt->payload_hash) {
                // At-least-once delivery doing its job. Section 11: the
                // answer to a redelivery is the previous durable result.
                out.kind = CommitKind::Replayed;
                out.tx = prior.tx;
                *detail = "receipt already applied in state " + prior.apply_state;
                return out;
            }
            // JI-07: same key, different bytes. Picking one of them would be
            // guessing about an execution, so neither is applied.
            *detail = "receipt " + std::to_string(plan.receipt->receipt_id) +
                      " redelivered with a different payload hash";
            out.kind = CommitKind::Quarantined;
            out.error = StoreError::Quarantined;
            return out;
        }
    }

    // --- Dedup: client command --------------------------------------------
    if (plan.intent.has_value()) {
        const CommandRecord prior =
            lookup(plan.tenant, plan.intent->account, plan.intent->command_id);
        if (prior.found) {
            if (prior.canonical_hash == plan.intent->canonical_hash) {
                out.kind = CommitKind::Replayed;
                out.tx = prior.tx;
                *detail = "command already durable with decision " + prior.decision;
                return out;
            }
            // JI-05. Reusing a command id for different content is a client
            // bug or an attack; it is never a second order.
            out.kind = CommitKind::Conflict;
            out.error = StoreError::Conflict;
            *detail = "command id reused with a different canonical request hash";
            return out;
        }
    }

    // --- Dedup: business execution ----------------------------------------
    if (plan.exec_key.has_value()) {
        const ExecKeyRecord prior =
            lookup_exec_key(plan.tenant, plan.exec_key->lp_namespace,
                            plan.exec_key->semantic_exec_id);
        if (prior.found) {
            if (prior.fingerprint == plan.exec_key->fingerprint) {
                // JI-06: the same execution reported twice produces one fill.
                out.kind = CommitKind::Replayed;
                out.tx = prior.tx;
                *detail = "execution already applied";
                return out;
            }
            out.kind = CommitKind::Quarantined;
            out.error = StoreError::Quarantined;
            *detail = "execution id reused with a different business fingerprint";
            return out;
        }
    }

    // --- Quarantine path ---------------------------------------------------
    // A contradiction is recorded durably: the receipt is marked, a case is
    // opened and an audit line is written, but no domain state moves.
    if (plan.quarantine) {
        if (plan.receipt.has_value()) {
            Stmt s;
            e = db_->prepare(
                "INSERT INTO inbox_receipts(tenant_id,session_key,session_epoch,receipt_id,"
                "payload_hash,apply_state,tx_id,received_at) "
                "VALUES(?1,?2,?3,?4,?5,'QUARANTINED',?6,?7);",
                &s);
            if (e != StoreError::None) {
                out.kind = CommitKind::Rejected;
                out.error = e;
                return out;
            }
            s.bind_text(1, plan.tenant);
            s.bind_text(2, plan.receipt->session_key);
            s.bind_u64(3, plan.receipt->session_epoch);
            s.bind_u64(4, plan.receipt->receipt_id);
            s.bind_hex(5, plan.receipt->payload_hash);
            s.bind_text(6, plan.tx_id);
            s.bind_i64(7, now);
            e = s.run();
            if (e != StoreError::None) {
                out.kind = CommitKind::Rejected;
                out.error = e;
                *detail = "cannot record quarantined receipt: " + db_->last_error();
                return out;
            }
        }
        for (const RecoveryCasePlan& c : plan.cases) {
            e = open_case(plan.tenant, c);
            if (e != StoreError::None) {
                out.kind = CommitKind::Rejected;
                out.error = e;
                return out;
            }
        }
        for (const AuditPlan& a : plan.audit) {
            e = append_audit(plan.tenant, a, plan.tx_id);
            if (e != StoreError::None) {
                out.kind = CommitKind::Rejected;
                out.error = e;
                return out;
            }
        }
        out.kind = CommitKind::Quarantined;
        *detail = plan.quarantine_reason;
        return out;
    }

    // --- Transaction manifest ----------------------------------------------
    // Written before its members because the events carry a foreign key to
    // it. The digest is computed from the plan, so the manifest already
    // states what the member set must be and the verifier can check it
    // afterwards (section 8 note 6).
    if (!plan.events.empty()) {
        std::vector<Digest> member_hashes;
        member_hashes.reserve(plan.events.size());
        for (const AppendEvent& ev : plan.events) {
            member_hashes.push_back(sha256(canonical_encode(ev.payload)));
        }
        Stmt s;
        e = db_->prepare(
            "INSERT INTO journal_transactions(tenant_id,tx_id,expected_event_count,"
            "members_digest,created_at) VALUES(?1,?2,?3,?4,?5);",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, plan.tenant);
        s.bind_text(2, plan.tx_id);
        s.bind_i64(3, static_cast<std::int64_t>(plan.events.size()));
        s.bind_hex(4, members_digest(member_hashes));
        s.bind_i64(5, now);
        e = s.run();
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write transaction manifest: " + db_->last_error();
            return out;
        }
    }

    // --- Dedup rows ---------------------------------------------------------
    if (plan.receipt.has_value()) {
        Stmt s;
        e = db_->prepare(
            "INSERT INTO inbox_receipts(tenant_id,session_key,session_epoch,receipt_id,"
            "payload_hash,apply_state,tx_id,received_at) VALUES(?1,?2,?3,?4,?5,'APPLIED',?6,?7);",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, plan.tenant);
        s.bind_text(2, plan.receipt->session_key);
        s.bind_u64(3, plan.receipt->session_epoch);
        s.bind_u64(4, plan.receipt->receipt_id);
        s.bind_hex(5, plan.receipt->payload_hash);
        s.bind_text(6, plan.tx_id);
        s.bind_i64(7, now);
        e = s.run();
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write inbox receipt: " + db_->last_error();
            return out;
        }
        // W19: the receipt row exists and no business state has moved yet.
        if (JRNL_FP(fp::point::kAfterInboxInsert) == fp::Action::ReturnError) {
            *detail = "injected storage failure at inbox.after_insert";
            out.kind = CommitKind::Rejected;
            out.error = StoreError::IoError;
            return out;
        }
    }

    if (plan.exec_key.has_value()) {
        Stmt s;
        e = db_->prepare(
            "INSERT INTO business_exec_keys(tenant_id,lp_namespace,semantic_exec_id,"
            "fingerprint,order_chain_id,applied_tx,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7);",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, plan.tenant);
        s.bind_text(2, plan.exec_key->lp_namespace);
        s.bind_text(3, plan.exec_key->semantic_exec_id);
        s.bind_hex(4, plan.exec_key->fingerprint);
        s.bind_text(5, plan.exec_key->chain);
        s.bind_text(6, plan.tx_id);
        s.bind_i64(7, now);
        e = s.run();
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write execution dedup key: " + db_->last_error();
            return out;
        }
    }

    if (JRNL_FP(fp::point::kAfterDedupInsert) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_dedup_insert";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    // --- Journal and projections -------------------------------------------
    std::map<StreamId, StreamHead> heads;
    std::map<OrderChainId, OrderState> orders;
    std::map<RiskDomainId, RiskDomainState> risks;

    // Ordinals are the order within the transaction. Sorting here means the
    // caller may build the plan in any order and the manifest still matches
    // (G01-17).
    std::vector<const AppendEvent*> ordered;
    ordered.reserve(plan.events.size());
    for (const AppendEvent& ev : plan.events) ordered.push_back(&ev);
    std::sort(ordered.begin(), ordered.end(),
              [](const AppendEvent* a, const AppendEvent* b) { return a->ordinal < b->ordinal; });
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i]->ordinal == ordered[i - 1]->ordinal) {
            out.kind = CommitKind::Rejected;
            out.error = StoreError::Conflict;
            *detail = "duplicate event ordinal within one transaction";
            return out;
        }
    }

    for (const AppendEvent* ev : ordered) {
        e = write_event(plan, *ev, &heads, &orders, &risks, detail);
        if (e != StoreError::None) {
            out.kind = e == StoreError::Quarantined ? CommitKind::Quarantined
                       : (e == StoreError::Conflict || e == StoreError::StaleVersion)
                           ? CommitKind::Conflict
                           : CommitKind::Rejected;
            out.error = e;
            return out;
        }
    }

    // Projections are written once per aggregate, after every event of this
    // transaction has been folded in. Writing per event would make the
    // intermediate state visible to a CHECK that the final state satisfies.
    for (const auto& entry : orders) {
        const StreamId sid = order_stream(entry.first);
        const StreamHead& h = heads[sid];
        e = write_order_projection(plan.tenant, entry.second, h.version, h.state_hash,
                                   plan.tx_id, h.reducer_version);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write order projection: " + db_->last_error();
            return out;
        }
    }
    if (JRNL_FP(fp::point::kAfterOrderProjection) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_order_projection";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    for (const auto& entry : risks) {
        const StreamId sid = risk_stream(entry.first);
        const StreamHead& h = heads[sid];
        e = write_risk_projection(plan.tenant, entry.second, h.version, h.state_hash,
                                  plan.tx_id);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write risk projection: " + db_->last_error();
            return out;
        }
    }
    if (JRNL_FP(fp::point::kAfterRiskProjection) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_risk_projection";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    if (plan.exec_key.has_value() && plan.exec_key->last_qty > 0) {
        e = write_fill_row(plan.tenant, *plan.exec_key, plan.tx_id);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write fill ledger row: " + db_->last_error();
            return out;
        }
    }

    // --- Command intent -----------------------------------------------------
    if (plan.intent.has_value()) {
        Stmt s;
        e = db_->prepare(
            "INSERT INTO command_intents(tenant_id,account_id,command_id,order_chain_id,"
            "canonical_hash,risk_reservation_id,decision,result,created_tx,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, plan.tenant);
        s.bind_text(2, plan.intent->account);
        s.bind_text(3, plan.intent->command_id);
        s.bind_text(4, plan.intent->chain);
        s.bind_hex(5, plan.intent->canonical_hash);
        if (plan.intent->reservation.empty()) s.bind_null(6);
        else s.bind_text(6, plan.intent->reservation);
        s.bind_text(7, plan.intent->decision);
        s.bind_text(8, std::string(to_string(CommitKind::Committed)));
        s.bind_text(9, plan.tx_id);
        s.bind_i64(10, now);
        e = s.run();
        if (e != StoreError::None) {
            out.kind = e == StoreError::Conflict ? CommitKind::Conflict : CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write command intent: " + db_->last_error();
            return out;
        }
    }
    if (JRNL_FP(fp::point::kAfterCommandIntent) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_command_intent";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    // --- Outbox -------------------------------------------------------------
    for (const OutboxIntent& o : plan.effects) {
        Stmt s;
        e = db_->prepare(
            "INSERT INTO command_outbox(outbox_id,tenant_id,account_id,command_id,session_key,"
            "original_payload_hash,original_payload,state,attempts,send_attempted,created_tx,"
            "created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,'READY',0,0,?8,?9);",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, o.outbox_id);
        s.bind_text(2, plan.tenant);
        s.bind_text(3, o.account);
        s.bind_text(4, o.command_id);
        s.bind_text(5, o.session_key);
        s.bind_hex(6, o.payload_hash);
        s.bind_text(7, o.payload);
        s.bind_text(8, plan.tx_id);
        s.bind_i64(9, now);
        e = s.run();
        if (e != StoreError::None) {
            // The composite foreign key to command_intents is what stops an
            // outbox row from existing without the intent that justifies it
            // (G02-13, verification SQL 2).
            out.kind = e == StoreError::Conflict ? CommitKind::Conflict : CommitKind::Rejected;
            out.error = e;
            *detail = "cannot write outbox intent: " + db_->last_error();
            return out;
        }
    }
    if (JRNL_FP(fp::point::kAfterOutboxInsert) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_outbox_insert";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    for (const OutboxTransition& t : plan.outbox_updates) {
        Stmt s;
        e = db_->prepare(
            "UPDATE command_outbox SET state=?1, last_error=?2 WHERE outbox_id=?3 AND "
            "tenant_id=?4;",
            &s);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
        s.bind_text(1, t.new_state);
        s.bind_text(2, t.reason);
        s.bind_text(3, t.outbox_id);
        s.bind_text(4, plan.tenant);
        e = s.run();
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            return out;
        }
    }

    for (const RecoveryCasePlan& c : plan.cases) {
        e = open_case(plan.tenant, c);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot open recovery case: " + db_->last_error();
            return out;
        }
    }

    for (const AuditPlan& a : plan.audit) {
        e = append_audit(plan.tenant, a, plan.tx_id);
        if (e != StoreError::None) {
            out.kind = CommitKind::Rejected;
            out.error = e;
            *detail = "cannot append audit record: " + db_->last_error();
            return out;
        }
    }

    if (JRNL_FP(fp::point::kAfterManifest) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_manifest";
        out.kind = CommitKind::Rejected;
        out.error = StoreError::IoError;
        return out;
    }

    out.kind = CommitKind::Committed;
    out.tx = plan.tx_id;
    return out;
}

StoreError SqliteAtomicStore::write_event(const TxPlan& plan, const AppendEvent& e,
                                          std::map<StreamId, StreamHead>* heads,
                                          std::map<OrderChainId, OrderState>* orders,
                                          std::map<RiskDomainId, RiskDomainState>* risks,
                                          std::string* detail) {
    // Head: from the per-transaction cache when a previous event of this same
    // transaction already advanced the stream, otherwise from the row.
    StreamHead h;
    const auto cached = heads->find(e.stream);
    if (cached != heads->end()) {
        h = cached->second;
    } else {
        h = head(plan.tenant, e.stream);
    }

    if (h.version != e.expected_version) {
        // G01-05 and G01-04: the loser of a race sees this, and there is no
        // path that appends at "whatever version is there now".
        // Distinguished from a plain Conflict on purpose: the caller built
        // its plan from a head that has since moved, and the right answer
        // is to rebuild it around the same idempotency key (JI-27), not to
        // tell the client its command was rejected.
        *detail = "expected version " + std::to_string(e.expected_version) + " for stream " +
                  e.stream + " but head is " + std::to_string(h.version);
        return StoreError::StaleVersion;
    }
    if (h.version >= kMaxStreamVersion - 1) {
        // G01-20: refuse admission rather than wrap or reuse a version.
        *detail = "stream " + e.stream + " is at the maximum version";
        return StoreError::Rejected;
    }
    const Version version = h.version + 1;

    const bool is_order = is_order_stream(e.stream);
    const bool is_risk = is_risk_stream(e.stream);
    if (!is_order && !is_risk) {
        *detail = "unknown stream kind for " + e.stream;
        return StoreError::Rejected;
    }

    // Reducer pinned by the event, never by what is newest (JI-37).
    const IOrderReducer* ord = is_order ? reducers_->order(e.reducer_version) : nullptr;
    const IRiskReducer* rsk = is_risk ? reducers_->risk(e.reducer_version) : nullptr;
    if ((is_order && ord == nullptr) || (is_risk && rsk == nullptr)) {
        *detail = "no reducer registered for version " + e.reducer_version;
        return StoreError::Rejected;
    }
    if (h.exists && !h.reducer_version.empty() && h.reducer_version != e.reducer_version) {
        // Mixing reducer versions inside one stream would make the state
        // hash sequence meaningless. A migration is an explicit, versioned
        // rebuild, not a silent switch mid-stream.
        *detail = "stream " + e.stream + " is on reducer " + h.reducer_version +
                  " and the event claims " + e.reducer_version;
        return StoreError::SchemaMismatch;
    }

    // Current state. For a stream with history this comes from the
    // projection, which the store itself wrote from the same reducer, so the
    // two cannot drift without the verifier noticing.
    OrderState ostate;
    RiskDomainState rstate;
    const std::string suffix = stream_suffix(e.stream);
    if (is_order) {
        const auto it = orders->find(suffix);
        if (it != orders->end()) {
            ostate = it->second;
        } else if (h.version > 0) {
            if (!load_order(plan.tenant, suffix, &ostate)) {
                *detail = "order projection missing for a stream with history: " + e.stream;
                return StoreError::Corrupt;
            }
        }
    } else {
        const auto it = risks->find(suffix);
        if (it != risks->end()) {
            rstate = it->second;
        } else if (h.version > 0) {
            if (!load_risk(plan.tenant, suffix, &rstate)) {
                *detail = "risk projection missing for a stream with history: " + e.stream;
                return StoreError::Corrupt;
            }
        }
    }

    ReduceError rerr;
    if (is_order) {
        if (!ord->apply(&ostate, e.event_type, e.payload, &rerr)) {
            *detail = std::string(to_string(rerr.incident)) + ": " + rerr.detail;
            // A contradiction between the report and our state is
            // quarantined; a malformed payload is a plain rejection.
            return rerr.incident == Incident::SchemaIncompatible ? StoreError::Rejected
                                                                 : StoreError::Quarantined;
        }
    } else {
        if (!rsk->apply(&rstate, e.event_type, e.payload, &rerr)) {
            *detail = std::string(to_string(rerr.incident)) + ": " + rerr.detail;
            return rerr.incident == Incident::SchemaIncompatible ? StoreError::Rejected
                                                                 : StoreError::Quarantined;
        }
    }

    const CanonValue new_canon = is_order ? ord->to_canon(ostate) : rsk->to_canon(rstate);
    const Digest state_dig = state_digest(new_canon);
    const std::string payload_text = canonical_encode(e.payload);
    const Digest canonical_hash =
        sha256(canonical_encode(event_envelope(plan.tenant, e, version)));
    const Digest chain =
        event_chain_hash(h.state_hash, canonical_hash, state_dig, e.stream, version,
                         e.reducer_version);

    Stmt s;
    StoreError err = db_->prepare(
        "INSERT INTO journal_events(tenant_id,stream_id,stream_version,event_id,tx_id,"
        "event_ordinal,event_type,schema_version,reducer_version,policy_version,payload,"
        "canonical_hash,prev_state_hash,state_hash,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);",
        &s);
    if (err != StoreError::None) return err;
    s.bind_text(1, plan.tenant);
    s.bind_text(2, e.stream);
    s.bind_u64(3, version);
    s.bind_text(4, e.event_id);
    s.bind_text(5, plan.tx_id);
    s.bind_i64(6, e.ordinal);
    s.bind_text(7, e.event_type);
    s.bind_i64(8, e.schema_version);
    s.bind_text(9, e.reducer_version);
    s.bind_u64(10, e.policy_version);
    s.bind_text(11, payload_text);
    s.bind_hex(12, canonical_hash);
    s.bind_hex(13, h.state_hash);
    s.bind_hex(14, chain);
    s.bind_i64(15, clock_->now_ms());
    err = s.run();
    if (err != StoreError::None) {
        // A duplicate event_id with different content lands here on the
        // UNIQUE(tenant_id,event_id) constraint (G01-07).
        *detail = "cannot append journal event: " + db_->last_error();
        return err;
    }

    if (JRNL_FP(fp::point::kAfterJournalAppend) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_journal_append";
        return StoreError::IoError;
    }

    Stmt hs;
    err = db_->prepare(
        "INSERT INTO stream_heads(tenant_id,stream_id,current_version,state_hash,"
        "reducer_version,fencing_token) VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(tenant_id,stream_id) DO UPDATE SET current_version=excluded.current_version,"
        "state_hash=excluded.state_hash,reducer_version=excluded.reducer_version,"
        "fencing_token=excluded.fencing_token;",
        &hs);
    if (err != StoreError::None) return err;
    hs.bind_text(1, plan.tenant);
    hs.bind_text(2, e.stream);
    hs.bind_u64(3, version);
    hs.bind_hex(4, chain);
    hs.bind_text(5, e.reducer_version);
    hs.bind_u64(6, plan.fence_token);
    err = hs.run();
    if (err != StoreError::None) {
        *detail = "cannot update stream head: " + db_->last_error();
        return err;
    }

    if (JRNL_FP(fp::point::kAfterStreamHead) == fp::Action::ReturnError) {
        *detail = "injected storage failure at tx.after_stream_head";
        return StoreError::IoError;
    }

    StreamHead nh;
    nh.exists = true;
    nh.version = version;
    nh.state_hash = chain;
    nh.reducer_version = e.reducer_version;
    nh.fence = plan.fence_token;
    (*heads)[e.stream] = nh;
    if (is_order) (*orders)[suffix] = ostate;
    else (*risks)[suffix] = rstate;

    return StoreError::None;
}

StoreError SqliteAtomicStore::write_order_projection(const TenantId& tenant, const OrderState& s,
                                                     Version v, const Digest& hash,
                                                     const TxId& tx,
                                                     const std::string& reducer_version) {
    // The projection is stored as the canonical state produced by the same
    // reducer version the stream is on, so the comparison replay makes later
    // is byte-for-byte rather than approximate.
    const IOrderReducer* use = reducers_->order(reducer_version);
    if (use == nullptr) return StoreError::Rejected;

    Stmt st;
    StoreError e = db_->prepare(
        "INSERT INTO orders(tenant_id,order_chain_id,account_id,risk_domain,status,order_qty,"
        "cum_qty,leaves_qty,state,state_hash,stream_version,updated_tx) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) "
        "ON CONFLICT(tenant_id,order_chain_id) DO UPDATE SET account_id=excluded.account_id,"
        "risk_domain=excluded.risk_domain,status=excluded.status,order_qty=excluded.order_qty,"
        "cum_qty=excluded.cum_qty,leaves_qty=excluded.leaves_qty,state=excluded.state,"
        "state_hash=excluded.state_hash,stream_version=excluded.stream_version,"
        "updated_tx=excluded.updated_tx;",
        &st);
    if (e != StoreError::None) return e;
    st.bind_text(1, tenant);
    st.bind_text(2, s.chain);
    st.bind_text(3, s.account);
    st.bind_text(4, s.risk_domain);
    st.bind_text(5, std::string(to_string(s.effective_status())));
    st.bind_i64(6, s.order_qty);
    st.bind_i64(7, s.cum_qty);
    st.bind_i64(8, s.leaves_qty);
    st.bind_text(9, canonical_encode(use->to_canon(s)));
    st.bind_hex(10, hash);
    st.bind_u64(11, v);
    st.bind_text(12, tx);
    return st.run();
}

StoreError SqliteAtomicStore::write_risk_projection(const TenantId& tenant,
                                                    const RiskDomainState& s, Version v,
                                                    const Digest& hash, const TxId& tx) {
    const IRiskReducer* r = reducers_->risk(reducers_->current_risk_version());
    if (r == nullptr) return StoreError::Rejected;

    Stmt st;
    StoreError e = db_->prepare(
        "INSERT INTO risk_domains(tenant_id,domain_id,limit_amount,reserved,exposure,revision,"
        "state,state_hash,stream_version,updated_tx) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
        "ON CONFLICT(tenant_id,domain_id) DO UPDATE SET limit_amount=excluded.limit_amount,"
        "reserved=excluded.reserved,exposure=excluded.exposure,revision=excluded.revision,"
        "state=excluded.state,state_hash=excluded.state_hash,"
        "stream_version=excluded.stream_version,updated_tx=excluded.updated_tx;",
        &st);
    if (e != StoreError::None) return e;
    st.bind_text(1, tenant);
    st.bind_text(2, s.domain);
    st.bind_i64(3, s.limit);
    st.bind_i64(4, s.reserved);
    st.bind_i64(5, s.exposure);
    st.bind_u64(6, s.revision);
    st.bind_text(7, canonical_encode(r->to_canon(s)));
    st.bind_hex(8, hash);
    st.bind_u64(9, v);
    st.bind_text(10, tx);
    e = st.run();
    if (e != StoreError::None) return e;

    for (const auto& entry : s.reservations) {
        Stmt rs;
        e = db_->prepare(
            "INSERT INTO risk_reservations(tenant_id,domain_id,reservation_id,account_id,"
            "order_chain_id,amount,state,updated_tx) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
            "ON CONFLICT(tenant_id,domain_id,reservation_id) DO UPDATE SET "
            "amount=excluded.amount,state=excluded.state,updated_tx=excluded.updated_tx;",
            &rs);
        if (e != StoreError::None) return e;
        rs.bind_text(1, tenant);
        rs.bind_text(2, s.domain);
        rs.bind_text(3, entry.first);
        rs.bind_text(4, entry.second.account);
        rs.bind_text(5, entry.second.chain);
        rs.bind_i64(6, entry.second.amount);
        rs.bind_text(7, std::string(to_string(entry.second.state)));
        rs.bind_text(8, tx);
        e = rs.run();
        if (e != StoreError::None) return e;
    }
    return StoreError::None;
}

StoreError SqliteAtomicStore::write_fill_row(const TenantId& tenant, const ExecKeyPlan& k,
                                             const TxId& tx) {
    Stmt s;
    StoreError e = db_->prepare(
        "INSERT INTO fills(tenant_id,lp_namespace,semantic_exec_id,order_chain_id,last_qty,"
        "last_px,applied_tx,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, tenant);
    s.bind_text(2, k.lp_namespace);
    s.bind_text(3, k.semantic_exec_id);
    s.bind_text(4, k.chain);
    s.bind_i64(5, k.last_qty);
    s.bind_i64(6, k.last_px);
    s.bind_text(7, tx);
    s.bind_i64(8, clock_->now_ms());
    e = s.run();
    if (e != StoreError::None) return e;
    if (JRNL_FP(fp::point::kAfterExecKeyInsert) == fp::Action::ReturnError) {
        return StoreError::IoError;
    }
    return StoreError::None;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

StreamHead SqliteAtomicStore::head(const TenantId& tenant, const StreamId& stream) const {
    StreamHead h;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT current_version,state_hash,reducer_version,fencing_token FROM stream_heads "
            "WHERE tenant_id=?1 AND stream_id=?2;",
            &s) != StoreError::None) {
        return h;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, stream);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return h;
    h.exists = true;
    h.version = s.col_u64(0);
    s.col_hex(1, &h.state_hash);
    h.reducer_version = s.col_text(2);
    h.fence = s.col_u64(3);
    return h;
}

StoreError SqliteAtomicStore::read_stream(const TenantId& tenant, const StreamId& stream,
                                          Version from, std::size_t max,
                                          std::vector<JournalRecord>* out) const {
    out->clear();
    Stmt s;
    StoreError e = const_cast<Db&>(*db_).prepare(
        "SELECT stream_version,event_id,tx_id,event_ordinal,event_type,schema_version,"
        "reducer_version,policy_version,payload,canonical_hash,prev_state_hash,state_hash,"
        "created_at FROM journal_events WHERE tenant_id=?1 AND stream_id=?2 AND "
        "stream_version>=?3 ORDER BY stream_version ASC LIMIT ?4;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, tenant);
    s.bind_text(2, stream);
    s.bind_u64(3, from);
    s.bind_i64(4, static_cast<std::int64_t>(max == 0 ? 1000000 : max));
    while (true) {
        bool row = false;
        e = s.step(&row);
        if (e != StoreError::None) return e;
        if (!row) break;
        JournalRecord r;
        r.stream = stream;
        r.version = s.col_u64(0);
        r.event_id = s.col_text(1);
        r.tx_id = s.col_text(2);
        r.ordinal = static_cast<int>(s.col_i64(3));
        r.event_type = s.col_text(4);
        r.schema_version = static_cast<SchemaVersion>(s.col_i64(5));
        r.reducer_version = s.col_text(6);
        r.policy_version = s.col_u64(7);
        r.payload_text = s.col_text(8);
        if (!s.col_hex(9, &r.canonical_hash) || !s.col_hex(10, &r.prev_state_hash) ||
            !s.col_hex(11, &r.state_hash)) {
            // A hash column that is not 64 hex characters is corruption, and
            // the caller must not treat it as a zero digest (G01-19).
            return StoreError::Corrupt;
        }
        r.created_at = s.col_i64(12);
        out->push_back(std::move(r));
    }
    return StoreError::None;
}

std::vector<StreamId> SqliteAtomicStore::list_streams(const TenantId& tenant) const {
    std::vector<StreamId> out;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT stream_id FROM stream_heads WHERE tenant_id=?1 ORDER BY stream_id;", &s) !=
        StoreError::None) {
        return out;
    }
    s.bind_text(1, tenant);
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) break;
        out.push_back(s.col_text(0));
    }
    return out;
}

CommandRecord SqliteAtomicStore::lookup(const TenantId& tenant, const AccountId& account,
                                        const CommandId& command) const {
    CommandRecord r;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT order_chain_id,canonical_hash,risk_reservation_id,decision,result,created_tx "
            "FROM command_intents WHERE tenant_id=?1 AND account_id=?2 AND command_id=?3;",
            &s) != StoreError::None) {
        return r;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, account);
    s.bind_text(3, command);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return r;
    r.found = true;
    r.chain = s.col_text(0);
    s.col_hex(1, &r.canonical_hash);
    r.reservation = s.col_is_null(2) ? std::string() : s.col_text(2);
    r.decision = s.col_text(3);
    r.result = s.col_text(4);
    r.tx = s.col_text(5);
    return r;
}

ReceiptRecord SqliteAtomicStore::lookup_receipt(const TenantId& tenant,
                                                const InboxReceipt& rec) const {
    ReceiptRecord r;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT payload_hash,apply_state,tx_id FROM inbox_receipts WHERE tenant_id=?1 AND "
            "session_key=?2 AND session_epoch=?3 AND receipt_id=?4;",
            &s) != StoreError::None) {
        return r;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, rec.session_key);
    s.bind_u64(3, rec.session_epoch);
    s.bind_u64(4, rec.receipt_id);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return r;
    r.found = true;
    s.col_hex(0, &r.payload_hash);
    r.apply_state = s.col_text(1);
    r.tx = s.col_text(2);
    return r;
}

ExecKeyRecord SqliteAtomicStore::lookup_exec_key(const TenantId& tenant, const LpNamespace& ns,
                                                 const ExecId& id) const {
    ExecKeyRecord r;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT fingerprint,order_chain_id,applied_tx FROM business_exec_keys WHERE "
            "tenant_id=?1 AND lp_namespace=?2 AND semantic_exec_id=?3;",
            &s) != StoreError::None) {
        return r;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, ns);
    s.bind_text(3, id);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return r;
    r.found = true;
    s.col_hex(0, &r.fingerprint);
    r.chain = s.col_text(1);
    r.tx = s.col_text(2);
    return r;
}

bool SqliteAtomicStore::load_order(const TenantId& tenant, const OrderChainId& chain,
                                   OrderState* out) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT state FROM orders WHERE tenant_id=?1 AND order_chain_id=?2;", &s) !=
        StoreError::None) {
        return false;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, chain);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    CanonValue v;
    if (!canonical_decode(s.col_text(0), &v, nullptr)) return false;
    std::string ver;
    if (!v.get_str("v", &ver)) return false;
    const IOrderReducer* r = reducers_->order(ver);
    return r != nullptr && r->from_canon(v, out);
}

bool SqliteAtomicStore::load_risk(const TenantId& tenant, const RiskDomainId& domain,
                                  RiskDomainState* out) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT state FROM risk_domains WHERE tenant_id=?1 AND domain_id=?2;", &s) !=
        StoreError::None) {
        return false;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, domain);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    CanonValue v;
    if (!canonical_decode(s.col_text(0), &v, nullptr)) return false;
    std::string ver;
    if (!v.get_str("v", &ver)) return false;
    const IRiskReducer* r = reducers_->risk(ver);
    return r != nullptr && r->from_canon(v, out);
}

// ---------------------------------------------------------------------------
// Fencing
// ---------------------------------------------------------------------------

StoreError SqliteAtomicStore::acquire_fence(const TenantId& tenant, const RiskDomainId& domain,
                                            const std::string& owner, std::int64_t lease_ms,
                                            FenceToken* out) {
    std::lock_guard<std::mutex> serialise(tx_mu_);
    TxGuard tx(*db_);
    StoreError e = tx.begin();
    if (e != StoreError::None) return e;

    FenceToken next = 1;
    Stmt rd;
    e = db_->prepare("SELECT token FROM owner_fences WHERE tenant_id=?1 AND domain_id=?2;", &rd);
    if (e != StoreError::None) return e;
    rd.bind_text(1, tenant);
    rd.bind_text(2, domain);
    bool row = false;
    e = rd.step(&row);
    if (e != StoreError::None) return e;
    if (row) next = rd.col_u64(0) + 1;
    rd.reset();

    Stmt up;
    e = db_->prepare(
        "INSERT INTO owner_fences(tenant_id,domain_id,token,lease_owner,lease_until) "
        "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(tenant_id,domain_id) DO UPDATE SET "
        "token=excluded.token,lease_owner=excluded.lease_owner,lease_until=excluded.lease_until;",
        &up);
    if (e != StoreError::None) return e;
    up.bind_text(1, tenant);
    up.bind_text(2, domain);
    up.bind_u64(3, next);
    up.bind_text(4, owner);
    up.bind_i64(5, clock_->now_ms() + lease_ms);
    e = up.run();
    if (e != StoreError::None) return e;

    e = tx.commit();
    if (e != StoreError::None) return e;
    if (out != nullptr) *out = next;
    return StoreError::None;
}

FenceToken SqliteAtomicStore::current_fence(const TenantId& tenant,
                                            const RiskDomainId& domain) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT token FROM owner_fences WHERE tenant_id=?1 AND domain_id=?2;", &s) !=
        StoreError::None) {
        return 0;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, domain);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 0;
    return s.col_u64(0);
}

// ---------------------------------------------------------------------------
// Outbox
// ---------------------------------------------------------------------------

StoreError SqliteAtomicStore::claim_outbox(const TenantId& tenant, const std::string& owner,
                                           FenceToken token, std::size_t limit,
                                           std::int64_t lease_ms, std::vector<OutboxRow>* out) {
    std::lock_guard<std::mutex> serialise(tx_mu_);
    out->clear();
    const std::int64_t now = clock_->now_ms();

    TxGuard tx(*db_);
    StoreError e = tx.begin();
    if (e != StoreError::None) return e;

    std::vector<OutboxId> ids;
    {
        Stmt s;
        e = db_->prepare(
            "SELECT outbox_id FROM command_outbox WHERE tenant_id=?1 AND state='READY' "
            "ORDER BY created_at ASC, outbox_id ASC LIMIT ?2;",
            &s);
        if (e != StoreError::None) return e;
        s.bind_text(1, tenant);
        s.bind_i64(2, static_cast<std::int64_t>(limit));
        while (true) {
            bool row = false;
            e = s.step(&row);
            if (e != StoreError::None) return e;
            if (!row) break;
            ids.push_back(s.col_text(0));
        }
    }

    for (const OutboxId& id : ids) {
        Stmt s;
        // The claim carries the fence token. A dispatcher whose token has
        // been superseded cannot take work, which is the second half of
        // JI-22: ownership is checked at the resource, not in memory.
        e = db_->prepare(
            "UPDATE command_outbox SET state='CLAIMED', claim_owner=?1, claim_token=?2, "
            "claim_expires_at=?3, attempts=attempts+1 WHERE outbox_id=?4 AND tenant_id=?5 "
            "AND state='READY';",
            &s);
        if (e != StoreError::None) return e;
        s.bind_text(1, owner);
        s.bind_u64(2, token);
        s.bind_i64(3, now + lease_ms);
        s.bind_text(4, id);
        s.bind_text(5, tenant);
        e = s.run();
        if (e != StoreError::None) return e;
    }

    e = tx.commit();
    if (e != StoreError::None) return e;

    for (const OutboxId& id : ids) {
        OutboxRow r;
        if (outbox_row(tenant, id, &r) && r.state == "CLAIMED" && r.claim_owner == owner) {
            out->push_back(std::move(r));
        }
    }
    JRNL_FP(fp::point::kAfterOutboxClaim);
    return StoreError::None;
}

StoreError SqliteAtomicStore::set_outbox_state(const TenantId& tenant, const OutboxId& id,
                                               const std::string& state, FenceToken token,
                                               std::optional<std::uint64_t> original_seq,
                                               const std::string& note) {
    OutboxRow existing;
    if (!outbox_row(tenant, id, &existing)) return StoreError::Rejected;
    if (existing.claim_token != 0 && token != 0 && existing.claim_token != token) {
        // A callback from a dispatcher that lost ownership. JI-21 again: the
        // late answer of a superseded worker must not move the row.
        return StoreError::NotOwner;
    }
    // Once a stable original sequence has been bound, it never changes
    // (JI-18). A later call with a different sequence is refused.
    if (existing.original_seq.has_value() && original_seq.has_value() &&
        *existing.original_seq != *original_seq) {
        return StoreError::Conflict;
    }

    Stmt s;
    StoreError e = db_->prepare(
        "UPDATE command_outbox SET state=?1, p01_original_seq=COALESCE(?2,p01_original_seq), "
        "last_error=?3 WHERE outbox_id=?4 AND tenant_id=?5;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, state);
    if (original_seq.has_value()) s.bind_u64(2, *original_seq);
    else s.bind_null(2);
    s.bind_text(3, note);
    s.bind_text(4, id);
    s.bind_text(5, tenant);
    return s.run();
}

StoreError SqliteAtomicStore::release_expired_claims(const TenantId& tenant, std::int64_t now_ms,
                                                     int* released) {
    std::lock_guard<std::mutex> serialise(tx_mu_);
    if (released != nullptr) *released = 0;
    TxGuard tx(*db_);
    StoreError e = tx.begin();
    if (e != StoreError::None) return e;

    int expiring = 0;
    {
        Stmt c;
        e = db_->prepare(
            "SELECT COUNT(*) FROM command_outbox WHERE tenant_id=?1 AND state='CLAIMED' AND "
            "claim_expires_at IS NOT NULL AND claim_expires_at<=?2;",
            &c);
        if (e != StoreError::None) return e;
        c.bind_text(1, tenant);
        c.bind_i64(2, now_ms);
        bool row = false;
        e = c.step(&row);
        if (e != StoreError::None) return e;
        if (row) expiring = static_cast<int>(c.col_i64(0));
    }

    // Section 10, in bold: a claim timing out does not prove the send never
    // happened. The row goes back to READY only when the dispatcher never
    // reached the stable-send boundary; otherwise it becomes UNKNOWN and a
    // human or a reconciliation decides (JI-20).
    Stmt a;
    e = db_->prepare(
        "UPDATE command_outbox SET state='READY', claim_owner=NULL, claim_token=NULL, "
        "claim_expires_at=NULL WHERE tenant_id=?1 AND state='CLAIMED' AND send_attempted=0 "
        "AND claim_expires_at IS NOT NULL AND claim_expires_at<=?2;",
        &a);
    if (e != StoreError::None) return e;
    a.bind_text(1, tenant);
    a.bind_i64(2, now_ms);
    e = a.run();
    if (e != StoreError::None) return e;

    Stmt b;
    e = db_->prepare(
        "UPDATE command_outbox SET state='UNKNOWN', last_error='claim expired after send was "
        "attempted' WHERE tenant_id=?1 AND state='CLAIMED' AND send_attempted=1 "
        "AND claim_expires_at IS NOT NULL AND claim_expires_at<=?2;",
        &b);
    if (e != StoreError::None) return e;
    b.bind_text(1, tenant);
    b.bind_i64(2, now_ms);
    e = b.run();
    if (e != StoreError::None) return e;

    if (released != nullptr) *released = expiring;
    return tx.commit();
}

StoreError SqliteAtomicStore::rebuild_projections(const TenantId& tenant, std::string* why) {
    std::lock_guard<std::mutex> serialise(tx_mu_);
    TxGuard tx(*db_);
    StoreError e = tx.begin();
    if (e != StoreError::None) return e;

    for (const StreamId& stream : list_streams(tenant)) {
        const StreamHead h = head(tenant, stream);
        if (!h.exists || h.version == 0) continue;

        std::vector<JournalRecord> page;
        e = read_stream(tenant, stream, 1, 0, &page);
        if (e != StoreError::None) return e;

        const bool is_order = is_order_stream(stream);
        OrderState ostate;
        RiskDomainState rstate;
        Version expected = 1;

        for (const JournalRecord& r : page) {
            if (r.version != expected) {
                if (why != nullptr) {
                    *why = "stream " + stream + " has a gap at version " +
                           std::to_string(expected);
                }
                return StoreError::Corrupt;
            }
            CanonValue payload;
            if (!canonical_decode(r.payload_text, &payload, why)) return StoreError::Corrupt;
            ReduceError rerr;
            if (is_order) {
                const IOrderReducer* red = reducers_->order(r.reducer_version);
                if (red == nullptr || !red->apply(&ostate, r.event_type, payload, &rerr)) {
                    if (why != nullptr) *why = "cannot replay " + stream + ": " + rerr.detail;
                    return StoreError::Corrupt;
                }
            } else {
                const IRiskReducer* red = reducers_->risk(r.reducer_version);
                if (red == nullptr || !red->apply(&rstate, r.event_type, payload, &rerr)) {
                    if (why != nullptr) *why = "cannot replay " + stream + ": " + rerr.detail;
                    return StoreError::Corrupt;
                }
            }
            ++expected;
        }

        if (is_order) {
            e = write_order_projection(tenant, ostate, h.version, h.state_hash, "rebuild",
                                       h.reducer_version);
        } else {
            e = write_risk_projection(tenant, rstate, h.version, h.state_hash, "rebuild");
        }
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot write projection for " + stream;
            return e;
        }
    }
    return tx.commit();
}

StoreError SqliteAtomicStore::mark_send_attempted(const TenantId& tenant, const OutboxId& id,
                                                  FenceToken token) {
    OutboxRow existing;
    if (!outbox_row(tenant, id, &existing)) return StoreError::Rejected;
    if (token != 0 && existing.claim_token != 0 && existing.claim_token != token) {
        return StoreError::NotOwner;
    }
    Stmt s;
    StoreError e = db_->prepare(
        "UPDATE command_outbox SET send_attempted=1 WHERE outbox_id=?1 AND tenant_id=?2;", &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, id);
    s.bind_text(2, tenant);
    return s.run();
}

std::vector<OutboxRow> SqliteAtomicStore::outbox_rows(const TenantId& tenant,
                                                      const std::string& state_filter) const {
    std::vector<OutboxRow> out;
    Stmt s;
    const std::string sql = std::string("SELECT ") + kOutboxCols +
                            " FROM command_outbox WHERE tenant_id=?1" +
                            (state_filter.empty() ? "" : " AND state=?2") +
                            " ORDER BY created_at ASC, outbox_id ASC;";
    if (const_cast<Db&>(*db_).prepare(sql, &s) != StoreError::None) return out;
    s.bind_text(1, tenant);
    if (!state_filter.empty()) s.bind_text(2, state_filter);
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) break;
        OutboxRow r;
        if (read_outbox_row(s, &r)) out.push_back(std::move(r));
    }
    return out;
}

bool SqliteAtomicStore::outbox_row(const TenantId& tenant, const OutboxId& id,
                                   OutboxRow* out) const {
    Stmt s;
    const std::string sql = std::string("SELECT ") + kOutboxCols +
                            " FROM command_outbox WHERE tenant_id=?1 AND outbox_id=?2;";
    if (const_cast<Db&>(*db_).prepare(sql, &s) != StoreError::None) return false;
    s.bind_text(1, tenant);
    s.bind_text(2, id);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    return read_outbox_row(s, out);
}

bool SqliteAtomicStore::outbox_for_command(const TenantId& tenant, const AccountId& account,
                                           const CommandId& command, OutboxRow* out) const {
    Stmt s;
    const std::string sql = std::string("SELECT ") + kOutboxCols +
                            " FROM command_outbox WHERE tenant_id=?1 AND account_id=?2 AND "
                            "command_id=?3;";
    if (const_cast<Db&>(*db_).prepare(sql, &s) != StoreError::None) return false;
    s.bind_text(1, tenant);
    s.bind_text(2, account);
    s.bind_text(3, command);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    return read_outbox_row(s, out);
}

// ---------------------------------------------------------------------------
// Recovery cases and audit
// ---------------------------------------------------------------------------

StoreError SqliteAtomicStore::open_case(const TenantId& tenant, const RecoveryCasePlan& c) {
    Stmt s;
    StoreError e = db_->prepare(
        "INSERT INTO recovery_cases(tenant_id,case_id,order_chain_id,reason_code,evidence_hash,"
        "status,opened_at) VALUES(?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(tenant_id,case_id) DO NOTHING;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, tenant);
    s.bind_text(2, c.case_id);
    s.bind_text(3, c.chain);
    s.bind_text(4, c.reason_code);
    s.bind_hex(5, c.evidence_hash);
    s.bind_text(6, c.status);
    s.bind_i64(7, clock_->now_ms());
    return s.run();
}

StoreError SqliteAtomicStore::resolve_case(const TenantId& tenant, const CaseId& id,
                                           const std::string& status,
                                           const std::string& approvals) {
    // Section 14's runbook asks for evidence and dual approval before a case
    // is closed. The store refuses to close one without recorded approvals
    // rather than leaving the policy to the caller.
    if (status == "RESOLVED" && approvals.empty()) return StoreError::Rejected;
    Stmt s;
    StoreError e = db_->prepare(
        "UPDATE recovery_cases SET status=?1, approvals=?2, resolved_at=?3 WHERE tenant_id=?4 "
        "AND case_id=?5;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, status);
    s.bind_text(2, approvals);
    s.bind_i64(3, clock_->now_ms());
    s.bind_text(4, tenant);
    s.bind_text(5, id);
    return s.run();
}

std::vector<RecoveryCasePlan> SqliteAtomicStore::open_cases(const TenantId& tenant) const {
    std::vector<RecoveryCasePlan> out;
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT case_id,order_chain_id,reason_code,evidence_hash,status FROM recovery_cases "
            "WHERE tenant_id=?1 AND status<>'RESOLVED' ORDER BY case_id;",
            &s) != StoreError::None) {
        return out;
    }
    s.bind_text(1, tenant);
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) break;
        RecoveryCasePlan c;
        c.case_id = s.col_text(0);
        c.chain = s.col_text(1);
        c.reason_code = s.col_text(2);
        s.col_hex(3, &c.evidence_hash);
        c.status = s.col_text(4);
        out.push_back(std::move(c));
    }
    return out;
}

Digest SqliteAtomicStore::audit_tip(const TenantId& tenant) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT entry_hash FROM audit_log WHERE tenant_id=?1 ORDER BY audit_id DESC LIMIT 1;",
            &s) != StoreError::None) {
        return zero_digest();
    }
    s.bind_text(1, tenant);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return zero_digest();
    Digest d;
    if (!s.col_hex(0, &d)) return zero_digest();
    return d;
}

StoreError SqliteAtomicStore::append_audit(const TenantId& tenant, const AuditPlan& a,
                                           const TxId& tx) {
    const Digest prev = audit_tip(tenant);
    const std::string data = canonical_encode(a.data);
    const std::int64_t at = clock_->now_ms();

    Sha256 h;
    h.update(prev);
    h.update(tenant);
    h.update(a.category);
    h.update(a.detail);
    h.update(data);
    h.update(tx);
    const Digest entry = h.finish();

    Stmt s;
    StoreError e = db_->prepare(
        "INSERT INTO audit_log(tenant_id,at,category,detail,data,tx_id,prev_hash,entry_hash) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, tenant);
    s.bind_i64(2, at);
    s.bind_text(3, a.category);
    s.bind_text(4, a.detail);
    s.bind_text(5, data);
    s.bind_text(6, tx);
    s.bind_hex(7, prev);
    s.bind_hex(8, entry);
    return s.run();
}

bool SqliteAtomicStore::audit_chain_intact(const TenantId& tenant, std::string* why) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT audit_id,category,detail,data,tx_id,prev_hash,entry_hash FROM audit_log "
            "WHERE tenant_id=?1 ORDER BY audit_id ASC;",
            &s) != StoreError::None) {
        if (why != nullptr) *why = "cannot read audit log";
        return false;
    }
    s.bind_text(1, tenant);
    Digest expected_prev = zero_digest();
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None) {
            if (why != nullptr) *why = "audit log read failed";
            return false;
        }
        if (!row) break;
        const std::int64_t id = s.col_i64(0);
        Digest prev{}, entry{};
        if (!s.col_hex(5, &prev) || !s.col_hex(6, &entry)) {
            if (why != nullptr) *why = "audit row " + std::to_string(id) + " has a malformed hash";
            return false;
        }
        if (!(prev == expected_prev)) {
            if (why != nullptr) {
                *why = "audit chain broken at row " + std::to_string(id);
            }
            return false;
        }
        Sha256 h;
        h.update(prev);
        h.update(tenant);
        h.update(s.col_text(1));
        h.update(s.col_text(2));
        h.update(s.col_text(3));
        h.update(s.col_text(4));
        if (!(h.finish() == entry)) {
            if (why != nullptr) {
                *why = "audit row " + std::to_string(id) + " does not match its recorded hash";
            }
            return false;
        }
        expected_prev = entry;
    }
    return true;
}

std::size_t SqliteAtomicStore::audit_count(const TenantId& tenant) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT COUNT(*) FROM audit_log WHERE tenant_id=?1;", &s) != StoreError::None) {
        return 0;
    }
    s.bind_text(1, tenant);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 0;
    return static_cast<std::size_t>(s.col_i64(0));
}

}  // namespace jrnl
