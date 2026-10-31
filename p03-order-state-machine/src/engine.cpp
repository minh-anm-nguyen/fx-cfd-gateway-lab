#include "order_core/engine.hpp"

#include <algorithm>
#include <chrono>

namespace order_core {

const char* to_string(RecoveryStatus s) {
    switch (s) {
        case RecoveryStatus::Clean: return "CLEAN";
        case RecoveryStatus::HashDivergence: return "HASH_DIVERGENCE";
        case RecoveryStatus::CorruptJournal: return "CORRUPT_JOURNAL";
        case RecoveryStatus::SnapshotRejected: return "SNAPSHOT_REJECTED";
        case RecoveryStatus::SchemaMismatch: return "SCHEMA_MISMATCH";
        case RecoveryStatus::Halted: return "HALTED";
    }
    return "UNKNOWN";
}

std::string AuditRecord::text() const {
    std::string s;
    const auto f = [&s](const char* k, const std::string& v) {
        s += k;
        s.push_back('=');
        s += v;
        s.push_back(' ');
    };
    f("trace", std::to_string(trace_id));
    f("chain", std::to_string(chain_id));
    f("inbox", std::to_string(source_inbox_id));
    f("session", source_session_id);
    f("src_seq", std::to_string(source_seq));
    f("exec_id", fix_exec_id);
    f("clordid", fix_cl_ord_id);
    f("rev", std::to_string(old_revision) + "->" + std::to_string(new_revision));
    f("decision", decision);
    f("reason", reason);
    f("policy", std::to_string(policy_version));
    f("recon_gen", std::to_string(reconciliation_generation));
    f("before", std::to_string(before_hash));
    f("after", std::to_string(after_hash));
    f("detail", detail);
    return s;
}

void RecordingEffectSink::dispatch(const OutboxRow& row) {
    // Issue-once at this port: a redelivered effect with the same stable key
    // is suppressed rather than turned into a second order (what-if F-20).
    size_t& n = seen_[row.idempotency_key];
    if (n > 0) {
        ++n;
        ++suppressed_;
        return;
    }
    ++n;
    dispatched_.push_back(row);
}

size_t RecordingEffectSink::count_key(const std::string& key) const {
    const auto it = seen_.find(key);
    return it == seen_.end() ? 0 : it->second;
}

Engine::Engine(MemoryStore& store, Policy policy) : store_(store), policy_(std::move(policy)) {}

const OrderAggregate* Engine::find(OrderId id) const {
    const auto it = chains_.find(id.value);
    return it == chains_.end() ? nullptr : &it->second;
}

const OrderAggregate* Engine::find_by_cl_ord_id(const std::string& firm, const std::string& day,
                                                const std::string& cl_ord_id) const {
    const auto it = request_index_.find(RequestKey{firm, day, cl_ord_id}.text());
    if (it == request_index_.end()) return nullptr;
    return find(OrderId{it->second});
}

size_t Engine::held_count(OrderId id) const {
    const auto it = held_.find(id.value);
    return it == held_.end() ? 0 : it->second.size();
}

size_t Engine::total_held() const {
    size_t n = 0;
    for (const auto& [id, parked] : held_) {
        (void)id;
        n += parked.size();
    }
    return n;
}

bool Engine::consumed(uint64_t inbox_seq) const { return store_.is_consumed(inbox_seq); }

bool Engine::ingest(const OrderEvent& event) {
    if (store_.unconsumed_inbox() >= policy_.limits.max_queue_events) {
        // Mailbox full. A local command is refused outright; an inbound peer
        // report must not be dropped, so the caller is expected to stop
        // advancing the durable inbox instead (design doc 6.2, what-if N-04,
        // N-05).
        ++metrics_.backpressure_rejections;
        return false;
    }
    store_.append_inbox(event);
    ++metrics_.events_ingested;
    metrics_.queue_high_water =
        std::max<uint64_t>(metrics_.queue_high_water, store_.unconsumed_inbox());
    return true;
}

bool Engine::pump(EngineResult* out) {
    const uint64_t seq = store_.first_unconsumed_seq();
    if (seq == 0) return false;
    // The event is copied out before processing: handling it can append to the
    // durable inbox (a held event being retried), which would invalidate a
    // reference into it.
    const InboxRow* head = store_.row(seq);
    if (head == nullptr) return false;
    const OrderEvent event = head->event;

    // W00: the process dies before the event is even parsed. The durable inbox
    // still holds it, so it is redelivered.
    store_.failpoint().check(CrashWindow::W00_BeforeParse, "pump:before parse");
    const EngineResult result = process(event, seq);
    if (out != nullptr) *out = result;
    return true;
}

size_t Engine::pump_all() {
    size_t n = 0;
    while (true) {
        const uint64_t head = store_.first_unconsumed_seq();
        if (head == 0) break;
        if (!pump()) break;
        // A row the commit boundary would not consume blocks the queue.
        // Spinning on it would be a livelock, so the drain stops and the
        // caller decides what to do about the blockage.
        if (!consumed(head)) break;
        ++n;
    }
    return n;
}

EngineResult Engine::submit(const OrderEvent& event) {
    EngineResult r;
    if (!ingest(event)) {
        r.decision = Decision::RejectLocal;
        r.reason = Reason::ResourceExhausted;
        r.detail = "ingress queue is full";
        return r;
    }
    const uint64_t target = store_.inbox().back().seq;

    // The mailbox is drained in order, so an event queued earlier is still
    // handled first. Processing continues until this event's own row has been
    // consumed, and stops early if an earlier row could not be consumed: a
    // blocked queue is reported to the caller rather than skipped over.
    while (!consumed(target)) {
        const uint64_t seq = store_.first_unconsumed_seq();
        if (seq == 0) break;
        const InboxRow* head = store_.row(seq);
        if (head == nullptr) break;
        const OrderEvent next = head->event;
        store_.failpoint().check(CrashWindow::W00_BeforeParse, "submit:before parse");
        r = process(next, seq);
        if (!consumed(seq)) return r;  // head of line is stuck; say so
    }
    return r;
}

ConfigError Engine::reload_policy(Policy p, bool dedup_index_migrated) {
    if (const ConfigError e = p.validate(); !e.ok()) return e;
    if (p.business_namespace != policy_.business_namespace && !store_.applied_keys().empty() &&
        !dedup_index_migrated) {
        // Renaming the identity namespace would make the existing dedup rows
        // unreachable, so the same execution could apply a second time under
        // the new name (what-if P-04, P-15).
        return {ConfigErrorCode::RetentionBelowResendHorizon,
                "identity namespace changed while " +
                    std::to_string(store_.applied_keys().size()) +
                    " dedup rows exist under the old one"};
    }
    policy_ = std::move(p);
    return {};
}

// Is this ClOrdID already in use by a chain that has not reached a resolved
// outcome, under a different business day? Reusing it would make two different
// orders answer to one identifier while the first is still live (what-if E-04,
// P-05). The same-day case is the request index's job.
bool Engine::cl_ord_id_unresolved_elsewhere(const LocalCommand& cmd) const {
    const auto it = cl_ord_id_index_.find(cmd.cl_ord_id);
    if (it == cl_ord_id_index_.end()) return false;
    for (const uint64_t chain : it->second) {
        const OrderAggregate* a = find(OrderId{chain});
        if (a == nullptr) continue;
        if (a->firm != cmd.firm) continue;
        if (a->business_day == cmd.business_day) continue;
        const bool resolved = a->terminal() && !a->reconciliation_required && !a->quarantined;
        if (!resolved) return true;
    }
    return false;
}

Engine::Correlation Engine::correlate(const OrderEvent& event) {
    Correlation c;

    if (const auto* cmd = std::get_if<LocalCommand>(&event)) {
        const std::string key = cmd->request_key().text();
        const auto existing = request_index_.find(key);

        // A producer that re-sends an identical intent (a retry, a restart) is
        // repeating itself, not asking for a second order. The same ClOrdID
        // carrying different business fields is a collision instead (what-if
        // A-02 against A-03, F-09, F-10).
        const auto applied = store_.applied_keys().find(key + "|" + to_string(cmd->kind));
        if (applied != store_.applied_keys().end()) {
            c.id = OrderId{applied->second.chain};
            if (applied->second.payload_hash == business_fingerprint(*cmd)) {
                c.decision = Decision::DuplicateNoop;
                c.reason = Reason::DuplicateSamePayload;
                c.detail = "identical " + std::string(to_string(cmd->kind)) + " intent for " +
                           cmd->cl_ord_id + " was already admitted";
            } else {
                c.decision = Decision::RejectLocal;
                c.reason = Reason::IdCollision;
                c.detail = "ClOrdID " + cmd->cl_ord_id +
                           " was already used with different business fields";
            }
            return c;
        }

        if (cmd->kind == CommandKind::New) {
            if (existing != request_index_.end()) {
                // The ClOrdID is already taken inside (firm, business_day).
                // Reusing it for a different order is refused; the account does
                // not widen the namespace in this profile (what-if A-03, E-01,
                // E-02, M-03).
                c.id = OrderId{existing->second};
                c.decision = Decision::RejectLocal;
                c.reason = Reason::IdCollision;
                c.detail = "ClOrdID " + cmd->cl_ord_id + " already used in " + cmd->firm + "/" +
                           cmd->business_day;
                return c;
            }
            if (cl_ord_id_unresolved_elsewhere(*cmd)) {
                c.decision = Decision::RejectLocal;
                c.reason = Reason::IdCollision;
                c.detail = "ClOrdID " + cmd->cl_ord_id +
                           " is still in use by an unresolved order from another business day";
                return c;
            }
            // The same order asked for twice while the first attempt's fate is
            // unknown. Routing this would be the duplicate-order path the
            // design doc lists as a release blocker; the operator has to
            // reconcile the first one instead (what-if F-08, section 11.3).
            //
            // Lab heuristic: a real deployment should carry an explicit client
            // retry marker rather than infer intent equality from the fields.
            const uint64_t intent = intent_fingerprint(*cmd);
            for (const auto& [chain, a] : chains_) {
                if (a.new_intent_fingerprint != intent) continue;
                if (a.confidence != Confidence::Unknown && !a.reconciliation_required) continue;
                c.decision = Decision::RejectLocal;
                c.reason = Reason::UnsafeDependentCommand;
                c.detail = "an identical order intent is outstanding on chain " +
                           std::to_string(chain) + " with an unknown outcome";
                return c;
            }
            if (chains_.size() >= policy_.limits.max_chain_count) {
                c.decision = Decision::RejectLocal;
                c.reason = Reason::ResourceExhausted;
                c.detail = "max_chain_count reached";
                return c;
            }
            c.create_new = true;
            c.id = OrderId{next_order_id_++};
            return c;
        }

        // Cancel and replace carry a new ClOrdID of their own, which must also
        // be unused, and an OrigClOrdID that names the chain.
        if (existing != request_index_.end()) {
            c.decision = Decision::RejectLocal;
            c.reason = Reason::IdCollision;
            c.detail = "request ClOrdID " + cmd->cl_ord_id + " already used";
            return c;
        }
        const auto orig = request_index_.find(
            RequestKey{cmd->firm, cmd->business_day, cmd->orig_cl_ord_id}.text());
        if (orig == request_index_.end()) {
            c.decision = Decision::RejectLocal;
            c.reason = Reason::LineageMismatch;
            c.detail = "OrigClOrdID " + cmd->orig_cl_ord_id + " names no known request";
            return c;
        }
        c.id = OrderId{orig->second};
        return c;
    }

    if (const auto* r = std::get_if<PeerReport>(&event)) {
        // Correlation order: the client's own identifier first, then the
        // request it supersedes, then the counterparty's OrderID alias. An
        // alias alone is never enough to create a chain.
        for (const std::string& candidate : {r->cl_ord_id, r->orig_cl_ord_id}) {
            if (candidate.empty()) continue;
            const auto it = cl_ord_id_index_.find(candidate);
            if (it == cl_ord_id_index_.end() || it->second.empty()) continue;
            if (it->second.size() > 1) {
                // The same ClOrdID is live on more than one chain, so the
                // report names no single order and picking one would be a
                // guess (invariant I03).
                c.id = OrderId{it->second.front()};
                c.decision = Decision::Quarantined;
                c.reason = Reason::IdentityInsufficient;
                c.detail = "ClOrdID " + candidate + " is live on " +
                           std::to_string(it->second.size()) +
                           " chains; correlation is ambiguous";
                return c;
            }
            c.id = OrderId{it->second.front()};
            return c;
        }
        if (!r->order_id.empty()) {
            const auto it = alias_index_.find(r->peer_id + "|" + r->order_id);
            if (it != alias_index_.end()) {
                c.id = OrderId{it->second};
                return c;
            }
        }
        // No chain. An ExecutionReport for an order we do not have must not
        // conjure one (what-if E-17).
        c.decision = Decision::Quarantined;
        c.reason = Reason::IdentityInsufficient;
        c.detail = "orphan report: ClOrdID '" + r->cl_ord_id + "', OrderID '" + r->order_id +
                   "' match no chain";
        return c;
    }

    const auto& sys_event = std::get<SystemEvent>(event);
    const auto it = cl_ord_id_index_.find(sys_event.target_cl_ord_id);
    if (it != cl_ord_id_index_.end() && it->second.size() == 1) {
        c.id = OrderId{it->second.front()};
        return c;
    }
    c.decision = Decision::RejectLocal;
    c.reason = Reason::UnknownRequest;
    c.detail = "system event names no single known request: '" + sys_event.target_cl_ord_id + "'";
    return c;
}

void Engine::index_chain(OrderId id, const OrderAggregate& a) {
    const auto remember = [this, id, &a](const std::string& cl_ord_id) {
        if (cl_ord_id.empty()) return;
        request_index_[RequestKey{a.firm, a.business_day, cl_ord_id}.text()] = id.value;
        std::vector<uint64_t>& chains = cl_ord_id_index_[cl_ord_id];
        for (const uint64_t existing : chains) {
            if (existing == id.value) return;
        }
        chains.push_back(id.value);
    };

    remember(a.initial_cl_ord_id);
    for (const AcceptedVersion& v : a.accepted_versions) remember(v.cl_ord_id);
    if (a.pending.has_value()) remember(a.pending->cl_ord_id);
    // Aliases are already stored as "peer|OrderID".
    for (const std::string& alias : a.order_id_aliases) {
        alias_index_[alias] = id.value;
    }
}

void Engine::record_audit(const OrderEvent& event, OrderId id, const Reduction& reduction,
                          uint64_t inbox_seq, uint64_t before_hash, uint64_t after_hash) {
    AuditRecord rec;
    rec.trace_id = next_trace_id_++;
    rec.chain_id = id.value;
    rec.source_inbox_id = inbox_seq;
    if (const auto* r = std::get_if<PeerReport>(&event)) {
        rec.source_session_id = r->session_id;
        rec.source_seq = r->source_seq;
        rec.fix_exec_id = r->exec_id;
        rec.fix_cl_ord_id = r->cl_ord_id;
    } else if (const auto* c = std::get_if<LocalCommand>(&event)) {
        rec.fix_cl_ord_id = c->cl_ord_id;
    } else {
        rec.fix_cl_ord_id = std::get<SystemEvent>(event).target_cl_ord_id;
    }
    rec.old_revision = reduction.next.revision == 0 ? 0 : reduction.next.revision - 1;
    rec.new_revision = reduction.next.revision;
    rec.decision = to_string(reduction.decision);
    rec.reason = to_string(reduction.reason);
    rec.policy_version = policy_.policy_version;
    rec.reconciliation_generation = reduction.next.reconciliation_generation;
    rec.before_hash = before_hash;
    rec.after_hash = after_hash;
    rec.detail = reduction.detail;
    audit_.push_back(std::move(rec));
}

EngineResult Engine::process(const OrderEvent& event, uint64_t inbox_seq) {
    EngineResult out;

    store_.failpoint().check(CrashWindow::W01_AfterParseBeforeCorrelation,
                             "process:after parse, before correlation");

    const Correlation c = correlate(event);
    out.id = c.id;
    if (c.decision != Decision::Applied) {
        out.decision = c.decision;
        out.reason = c.reason;
        out.detail = c.detail;
        if (c.decision == Decision::RejectLocal) ++metrics_.rejected_local;
        if (c.decision == Decision::DuplicateNoop) ++metrics_.duplicates;
        if (c.decision == Decision::Quarantined) {
            ++metrics_.orphan_reports;
            ++metrics_.quarantined;
        }
        // A refused event is still consumed: redelivering it would only be
        // refused again, and leaving it unacked would stall the inbox.
        store_.ack_inbox(inbox_seq);
        return out;
    }

    store_.failpoint().check(CrashWindow::W02_AfterCorrelationBeforeDedup,
                             "process:after correlation, before dedup");

    OrderAggregate previous;
    if (c.create_new) {
        previous.id = c.id;
    } else {
        const auto it = chains_.find(c.id.value);
        if (it == chains_.end()) {
            out.decision = Decision::RejectLocal;
            out.reason = Reason::UnknownRequest;
            out.detail = "correlated chain is not resident";
            ++metrics_.rejected_local;
            store_.ack_inbox(inbox_seq);
            return out;
        }
        previous = it->second;
    }
    const uint64_t before_hash = previous.state_hash();

    // Durable cross-chain dedup. The reducer only sees this chain, so a key
    // that belongs to a different chain has to be caught here (what-if F-04,
    // M-05, M-07).
    if (const auto* r = std::get_if<PeerReport>(&event)) {
        const std::string semantic =
            ExecKey{r->peer_id, policy_.business_namespace,
                    r->msg_type == '9' ? ("rej:" + r->cl_ord_id + ":" + r->cxl_rej_response_to)
                                       : r->exec_id}
                .text();
        const auto it = store_.applied_keys().find(semantic);
        if (it != store_.applied_keys().end() && it->second.chain != c.id.value) {
            out.decision = Decision::Quarantined;
            out.reason = Reason::ConflictingDuplicate;
            out.detail = "key " + semantic + " is already owned by chain " +
                         std::to_string(it->second.chain);
            ++metrics_.quarantined;
            ++metrics_.conflicts;
            store_.ack_inbox(inbox_seq);
            return out;
        }
        if (!r->order_id.empty()) {
            const auto alias = alias_index_.find(r->peer_id + "|" + r->order_id);
            if (alias != alias_index_.end() && alias->second != c.id.value) {
                out.decision = Decision::Quarantined;
                out.reason = Reason::AliasCollision;
                out.detail = "OrderID " + r->order_id + " already maps to chain " +
                             std::to_string(alias->second);
                ++metrics_.quarantined;
                store_.ack_inbox(inbox_seq);
                return out;
            }
        }
    }

    store_.failpoint().check(CrashWindow::W03_AfterDedupBeforeReduce,
                             "process:after dedup, before reduce");

    const uint64_t event_index = next_event_index_++;
    const Reduction reduction = reduce(previous, event, policy_, event_index);

    store_.failpoint().check(CrashWindow::W04_AfterReduceBeforeTxn,
                             "process:after reduce, before transaction");

    return finish(event, c.id, reduction, inbox_seq, before_hash);
}

EngineResult Engine::finish(const OrderEvent& event, OrderId id, const Reduction& reduction,
                            uint64_t inbox_seq, uint64_t before_hash) {
    EngineResult out;
    out.id = id;
    out.decision = reduction.decision;
    out.reason = reduction.reason;
    out.detail = reduction.detail;
    out.violations = reduction.violations;
    out.intents = reduction.intents;

    switch (reduction.decision) {
        case Decision::RejectLocal:
            ++metrics_.rejected_local;
            store_.ack_inbox(inbox_seq);
            record_audit(event, id, reduction, inbox_seq, before_hash, before_hash);
            return out;

        case Decision::DuplicateNoop:
            ++metrics_.duplicates;
            store_.ack_inbox(inbox_seq);
            record_audit(event, id, reduction, inbox_seq, before_hash, before_hash);
            return out;

        case Decision::HeldForCausality: {
            ++metrics_.held;
            std::vector<OrderEvent>& parked = held_[id.value];
            if (parked.size() + 1 > policy_.limits.max_held_events_per_chain) {
                // The gap never closed. Holding further would grow without a
                // bound, so the chain is quarantined instead (what-if G-20,
                // N-07).
                OrderAggregate a;
                if (const auto it = chains_.find(id.value); it != chains_.end()) a = it->second;
                a.quarantined = true;
                a.reconciliation_required = true;
                a.incidents.push_back({to_string(Reason::ResourceExhausted),
                                       "held-event buffer exhausted; causal gap never closed", "",
                                       "", a.revision});
                a.held_events = 0;
                chains_[id.value] = a;
                parked.clear();
                out.decision = Decision::Quarantined;
                out.reason = Reason::ResourceExhausted;
                out.detail = "held-event buffer exhausted";
                ++metrics_.quarantined;
                store_.ack_inbox(inbox_seq);
                return out;
            }
            parked.push_back(event);
            if (const auto it = chains_.find(id.value); it != chains_.end()) {
                it->second.held_events = parked.size();
            }
            store_.ack_inbox(inbox_seq);
            record_audit(event, id, reduction, inbox_seq, before_hash, before_hash);
            return out;
        }

        case Decision::RetryCommit:
            ++metrics_.retry_commits;
            store_.ack_inbox(inbox_seq);
            return out;

        case Decision::Applied:
        case Decision::Quarantined:
            break;
    }

    // Both Applied and Quarantined are committed: the incident, its evidence
    // and the reconciliation flag have to survive a restart just as a fill
    // does. What differs is that a quarantine changes no business quantity.
    const uint64_t expected =
        chains_.count(id.value) != 0 ? chains_.find(id.value)->second.revision : 0;
    const auto commit_start = std::chrono::steady_clock::now();
    CommitResult cr;
    try {
        cr = store_.commit(id, expected, reduction, reduction.next.last_applied_event_index, event,
                           "");
    } catch (const AmbiguousCommit&) {
        // The flush neither confirmed nor denied. Publishing a confirmed state
        // now would be a guess, so nothing is published and trading stops
        // (what-if L-13, window W12).
        ++metrics_.ambiguous_commits;
        out.commit_status = CommitStatus::Ambiguous;
        out.committed = false;
        out.decision = Decision::RetryCommit;
        out.reason = Reason::UnknownOutcome;
        out.detail = "commit outcome ambiguous; nothing published";
        trading_allowed_ = false;
        return out;
    }

    out.commit_status = cr.status;
    out.revision = cr.revision;

    if (cr.status == CommitStatus::DuplicateKey) {
        // The key was already applied, which is what a redelivery after window
        // W09 looks like. No second effect.
        ++metrics_.duplicates;
        out.decision = Decision::DuplicateNoop;
        out.reason = Reason::DuplicateSamePayload;
        out.detail = cr.detail;
        store_.ack_inbox(inbox_seq);
        return out;
    }
    if (cr.status == CommitStatus::RevisionConflict) {
        ++metrics_.commit_conflicts;
        out.decision = Decision::RetryCommit;
        out.reason = Reason::None;
        out.detail = cr.detail;
        return out;  // deliberately left unacked: the event must be reprocessed
    }
    if (cr.status != CommitStatus::Committed) {
        ++metrics_.commit_failures;
        out.decision = Decision::RetryCommit;
        out.reason = Reason::UnknownOutcome;
        out.detail = cr.detail;
        // Fail closed: no confirmed revision is published for this chain, and
        // the row stays unconsumed so the work is not lost.
        return out;
    }

    if (latency_ns_.size() < kMaxLatencySamples) {
        latency_ns_.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - commit_start)
                .count()));
    }

    // Committed. Only now does the in-memory view advance.
    OrderAggregate next = reduction.next;
    next.revision = cr.revision;
    chains_[id.value] = next;
    index_chain(id, next);
    out.committed = true;
    out.revision = cr.revision;

    if (reduction.decision == Decision::Applied) {
        ++metrics_.applied;
    } else {
        ++metrics_.quarantined;
    }
    if (reduction.reason == Reason::UnknownOutcome) ++metrics_.unknown_outcome;
    record_audit(event, id, reduction, inbox_seq, before_hash, next.state_hash());

    // W09: the process dies after the commit but before the inbox is acked.
    // The event is redelivered and must be recognised as already applied.
    store_.failpoint().check(CrashWindow::W09_AfterCommitBeforeInboxAck,
                             "finish:after commit, before inbox ack");
    store_.ack_inbox(inbox_seq);

    // W10: committed but the outbox has not been handed out yet. Recovery must
    // dispatch it with the same stable identity.
    store_.failpoint().check(CrashWindow::W10_AfterCommitBeforeDispatch,
                             "finish:after commit, before outbox dispatch");
    dispatch_outbox();

    if (reduction.decision == Decision::Applied && !retrying_held_) {
        retry_held(id);
    }
    return out;
}

void Engine::retry_held(OrderId id) {
    const auto it = held_.find(id.value);
    if (it == held_.end() || it->second.empty()) return;
    std::vector<OrderEvent> parked;
    parked.swap(it->second);
    if (const auto chain = chains_.find(id.value); chain != chains_.end()) {
        chain->second.held_events = 0;
    }

    retrying_held_ = true;
    for (const OrderEvent& e : parked) {
        const uint64_t seq = store_.append_inbox(e);
        process(e, seq);
    }
    retrying_held_ = false;
}

size_t Engine::dispatch_outbox() {
    size_t n = 0;
    for (OutboxRow& row : store_.outbox()) {
        if (row.marked_sent) continue;
        sink_.dispatch(row);
        row.dispatched = true;
        ++n;
        ++metrics_.effects_dispatched;
        // W11: the send went out but the row was never marked. On restart the
        // row is still pending, so the port's idempotency key is the only
        // thing standing between a replay and a duplicate order.
        store_.failpoint().check(CrashWindow::W11_AfterDispatchBeforeMarkSent,
                                 "dispatch:after send, before mark sent");
        row.marked_sent = true;
    }
    metrics_.effects_suppressed_duplicate = sink_.suppressed();
    return n;
}

CommitResult Engine::commit_as_second_writer(OrderId id, uint64_t expected_revision,
                                             const OrderEvent& event) {
    OrderAggregate previous;
    if (const auto it = chains_.find(id.value); it != chains_.end()) previous = it->second;
    const Reduction r = reduce(previous, event, policy_, next_event_index_++);
    // Deliberately commits at the revision the caller believes in, which is
    // how a stale second owner would behave.
    return store_.commit(id, expected_revision, r, r.next.last_applied_event_index, event, "");
}

LatencySummary Engine::event_to_commit_latency() const {
    LatencySummary out;
    out.samples = latency_ns_.size();
    if (latency_ns_.empty()) return out;
    std::vector<uint64_t> sorted = latency_ns_;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&sorted](double q) {
        const size_t i = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
        return sorted[i];
    };
    out.p50_ns = at(0.50);
    out.p95_ns = at(0.95);
    out.p99_ns = at(0.99);
    out.max_ns = sorted.back();
    return out;
}

RecoveryReport Engine::recover() {
    RecoveryReport report;
    chains_.clear();
    request_index_.clear();
    cl_ord_id_index_.clear();
    alias_index_.clear();
    held_.clear();

    if (store_.schema_version() != policy_.event_schema_version) {
        // An unknown schema version must not be reinterpreted by casting old
        // payloads into new structures (what-if P-06, P-19, L-19).
        report.status = RecoveryStatus::SchemaMismatch;
        report.detail = "journal schema " + std::to_string(store_.schema_version()) +
                        " != expected " + std::to_string(policy_.event_schema_version);
        report.trading_allowed = false;
        trading_allowed_ = false;
        return report;
    }

    // A snapshot is only a shortcut, never the source of truth. It is adopted
    // only if it is complete, on this schema, and not ahead of the journal
    // (what-if L-17, L-18).
    size_t start_record = 0;
    if (const std::optional<Snapshot>& snap = store_.snapshot(); snap.has_value()) {
        const bool usable = snap->complete &&
                            snap->schema_version == policy_.event_schema_version &&
                            snap->journal_watermark <= store_.journal().size();
        if (usable) {
            for (const auto& [id, state] : snap->aggregates) {
                chains_[id] = state;
                index_chain(OrderId{id}, state);
            }
            start_record = static_cast<size_t>(snap->journal_watermark);
        } else {
            report.status = RecoveryStatus::SnapshotRejected;
            report.detail = snap->complete
                                ? "snapshot is ahead of the journal or on a foreign schema"
                                : "snapshot was written only partially";
        }
    }

    // Replay the journaled events through the same reducer rather than trusting
    // the stored state blob, then compare against the hash each record wrote.
    uint64_t max_event_index = 0;
    uint64_t max_order_id = 0;
    for (size_t i = start_record; i < store_.journal().size(); ++i) {
        store_.failpoint().check(CrashWindow::W15_DuringRecoveryReplay, "recover:mid-replay");
        const JournalRecord& rec = store_.journal()[i];
        ++report.records_replayed;
        max_event_index = std::max(max_event_index, rec.event_index);
        max_order_id = std::max(max_order_id, rec.id.value);

        OrderAggregate previous;
        if (const auto it = chains_.find(rec.id.value); it != chains_.end()) {
            previous = it->second;
        } else {
            previous.id = rec.id;
        }
        const Reduction r = reduce(previous, rec.event, policy_, rec.event_index);
        if (r.decision != Decision::Applied && r.decision != Decision::Quarantined) {
            report.status = RecoveryStatus::HashDivergence;
            report.first_divergent_event = rec.event_index;
            report.detail = "replay of event " + std::to_string(rec.event_index) + " returned " +
                            to_string(r.decision) + ", not the committed " +
                            to_string(rec.decision);
            report.trading_allowed = false;
            trading_allowed_ = false;
            return report;
        }
        OrderAggregate next = r.next;
        next.revision = rec.revision_after;
        if (next.state_hash() != rec.state_hash_after) {
            // Either the journal was altered or the reducer changed behaviour.
            // Trading stays blocked and the first divergent event is named
            // (what-if L-15, Q-06).
            report.status = RecoveryStatus::HashDivergence;
            report.first_divergent_event = rec.event_index;
            report.detail = "state hash after event " + std::to_string(rec.event_index) + " is " +
                            std::to_string(next.state_hash()) + ", journal recorded " +
                            std::to_string(rec.state_hash_after);
            report.trading_allowed = false;
            trading_allowed_ = false;
            return report;
        }
        chains_[rec.id.value] = next;
        index_chain(rec.id, next);
    }

    next_event_index_ = std::max(next_event_index_, max_event_index + 1);
    next_order_id_ = std::max(next_order_id_, max_order_id + 1);
    report.chains_recovered = chains_.size();
    if (report.status == RecoveryStatus::Clean ||
        report.status == RecoveryStatus::SnapshotRejected) {
        report.trading_allowed = true;
        trading_allowed_ = true;
    }

    // Effects a crash left pending are handed out again with their original
    // stable identity; the port decides whether that is a resend or a no-op.
    dispatch_outbox();
    return report;
}

}  // namespace order_core
