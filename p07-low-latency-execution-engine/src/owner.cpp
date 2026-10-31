#include "llx/owner.hpp"

#include "llx/failpoint.hpp"

namespace llx {

OwnerActor::OwnerActor(Config cfg, IAtomicBusinessStore& store, ServiceModeController& modes,
                       ILatencySink& telemetry)
    : cfg_(cfg), store_(store), modes_(modes), telemetry_(telemetry) {}

void OwnerActor::sample(Span s, std::uint64_t a, std::uint64_t b, const EventId& id,
                        std::uint8_t outcome) {
    if (!cfg_.measure_spans || a == 0 || b == 0) return;
    // make_sample applies the duration checks of doc 3, so a reading from a
    // clock that moved backwards is marked invalid and counted rather than
    // recorded as a latency (LI-25).
    telemetry_.try_sample(make_sample(s, a, b, id, outcome));
}

void OwnerActor::promote_to_epoch(std::uint32_t epoch) {
    if (epoch <= cfg_.owner_epoch) return;
    cfg_.owner_epoch = epoch;
    store_.fence_to_epoch(epoch);
    // Anything in flight belonged to the previous epoch. It is not completed
    // and not discarded: its effect, if any, is whatever the journal says, and
    // the reconciliation path establishes that. Pretending the in-flight work
    // succeeded or failed is the inference LI-17 forbids.
    for (const auto& [domain, staged] : inflight_) {
        (void)domain;
        unresolved_unknown_.insert(staged.request.intent.idempotency_key);
    }
    inflight_.clear();
    reorder_.clear();
    ownership_uncertain_ = false;
    publish_health();
}

AdmissionOutcome OwnerActor::on_event(const InputEnvelope& ev) noexcept {
    ++stats_.events_seen;
    return process(ev);
}

AdmissionOutcome OwnerActor::process(const InputEnvelope& ev) {
    LLX_FP(fp::point::kOwnerAfterDequeue);

    // Fencing before anything else. An event minted under a superseded epoch
    // cannot be allowed to touch state, and a late callback from the displaced
    // owner is the case LI-47 names.
    if (ev.owner_epoch != 0 && ev.owner_epoch < cfg_.owner_epoch) {
        ++stats_.stale_epoch;
        return AdmissionOutcome::StaleEpoch;
    }
    if (ev.owner_epoch > cfg_.owner_epoch) {
        // Someone else has been promoted; this owner is the stale one. It stops
        // claiming authority rather than racing (doc 19.5).
        ownership_uncertain_ = true;
        publish_health();
        ++stats_.stale_epoch;
        return AdmissionOutcome::StaleEpoch;
    }

    if (!owns_domain(ev.risk_domain)) {
        // Not this shard's domain. Refused rather than handled, because two
        // owners writing one domain is the double-writer case LI-14 forbids.
        return AdmissionOutcome::InvalidInput;
    }

    // Session control carries no business effect and must be serviced in every
    // mode short of HALTED (LI-19). It is handled before capacity checks for
    // the same reason: losing the session loses the resend path.
    if (ev.type == EventType::SessionControl) {
        ++stats_.session_control;
        if (modes_.mode() == ServiceMode::Halted) return AdmissionOutcome::EngineHalted;
        return AdmissionOutcome::AcceptedForProcessing;
    }
    if (ev.type == EventType::Tick) return AdmissionOutcome::AcceptedForProcessing;

    if (modes_.mode() == ServiceMode::Halted) {
        ++stats_.rejected_business;
        return AdmissionOutcome::EngineHalted;
    }

    // Exactly one effect per identity. A duplicate wake, a retried offer and a
    // PossDup session replay all land here, and none of them produces a second
    // effect (LL-016, doc 19.3).
    if (!ev.event_id.is_null() && applied_.count(ev.event_id) != 0) {
        ++stats_.duplicates;
        return AdmissionOutcome::DuplicateIgnored;
    }

    // A New arriving while an outcome is unresolved is held, not issued
    // (LL-006). Receipts are still processed: they are the evidence that
    // resolves the unknown in the first place.
    if (!unresolved_unknown_.empty() && ev.type != EventType::ExecutionReport) {
        ++stats_.held_for_reconciliation;
        return AdmissionOutcome::HeldForReconciliation;
    }

    // One commit in flight per domain. The alternative -- an internal queue --
    // is the invisible backlog LI-22 forbids, so a second event for a busy
    // domain is refused and the producer retries.
    if (cfg_.commit_mode == CommitMode::AsyncWorker &&
        inflight_.find(ev.risk_domain) != inflight_.end()) {
        return AdmissionOutcome::RetryLaterNoAdmission;
    }

    const std::uint64_t compute_start = MonotonicClock::now_ns();

    // The reducer reads committed state only. It cannot see staged work, which
    // is what makes staged state non-authoritative by construction (doc 13).
    const bool dup_exec = (ev.exec_id != 0) && (seen_exec_.count(ev.exec_id) != 0);
    Proposal p = reduce(store_.state(), ev, dup_exec);
    LLX_FP(fp::point::kOwnerAfterReduce);

    if (!p.accepted) {
        ++stats_.rejected_business;
        // A refusal is still an outcome, and it is recorded against the
        // identity so a retry of the same event is not re-reduced.
        if (!ev.event_id.is_null()) applied_.insert(ev.event_id);
        return AdmissionOutcome::InvalidInput;
    }
    if (p.no_effect) {
        ++stats_.no_effect;
        if (!ev.event_id.is_null()) applied_.insert(ev.event_id);
        return AdmissionOutcome::DuplicateIgnored;
    }
    if (p.quarantine) ++stats_.quarantined;

    LLX_FP(fp::point::kOwnerAfterRiskCompute);

    // domain_seq is assigned here, in the order this owner processed the
    // events. The store enforces that commits follow it, which is what makes
    // the Cancel-cannot-overtake-a-fill guarantee structural rather than a
    // matter of timing (doc 7).
    InputEnvelope seq_ev = ev;
    std::uint64_t& next = next_seq_[ev.risk_domain];
    if (next == 0) next = store_.committed_domain_seq(ev.risk_domain);
    seq_ev.domain_seq = ++next;
    seq_ev.owner_epoch = cfg_.owner_epoch;

    CommitRequest req;
    req.intent = to_intent(p, seq_ev);
    req.effect = p;
    LLX_FP(fp::point::kOwnerAfterRiskStage);

    const std::uint64_t compute_end = MonotonicClock::now_ns();
    sample(Span::L2_DecisionCompute, compute_start, compute_end, ev.event_id,
           static_cast<std::uint8_t>(AdmissionOutcome::AcceptedForProcessing));
    if (ev.admission_ns != 0)
        sample(Span::L1_QueueWait, ev.admission_ns, compute_start, ev.event_id, 0);

    StagedCommit staged;
    staged.request = req;
    staged.source = seq_ev;
    staged.staged_ns = compute_end;

    if (cfg_.commit_mode == CommitMode::AsyncWorker) {
        // Handed to the persistence thread. Nothing is observable yet: the
        // outbox row does not exist until the commit lands.
        inflight_[ev.risk_domain] = staged;
        staged_.push_back(staged);
        publish_health();
        ++stats_.accepted;
        return AdmissionOutcome::AcceptedForProcessing;
    }

    LLX_FP(fp::point::kOwnerBeforeCommit);
    const std::uint64_t commit_begin = MonotonicClock::now_ns();
    CommitReceipt receipt;
    const CommitOutcome outcome = store_.commit(req, receipt);
    receipt.commit_begin_ns = commit_begin;
    receipt.commit_end_ns = MonotonicClock::now_ns();
    receipt.outcome = outcome;

    ++stats_.accepted;
    finish_commit(receipt, seq_ev, commit_begin);

    switch (outcome) {
        case CommitOutcome::Committed: return AdmissionOutcome::AcceptedForProcessing;
        case CommitOutcome::Unknown: return AdmissionOutcome::HeldForReconciliation;
        case CommitOutcome::Rejected: return AdmissionOutcome::InvalidInput;
    }
    return AdmissionOutcome::InvalidInput;
}

bool OwnerActor::take_staged(StagedCommit& out) {
    if (staged_.empty()) return false;
    out = staged_.front();
    staged_.erase(staged_.begin());
    return true;
}

void OwnerActor::finish_commit(const CommitReceipt& r, const InputEnvelope& src,
                               std::uint64_t commit_begin_ns) {
    switch (r.outcome) {
        case CommitOutcome::Committed: {
            ++stats_.committed;
            if (!src.event_id.is_null()) applied_.insert(src.event_id);
            if (src.exec_id != 0) seen_exec_.insert(src.exec_id);
            applied_seq_[src.risk_domain] = r.domain_seq;
            sample(Span::L3_DurableAdmission, commit_begin_ns, r.commit_end_ns, src.event_id,
                   static_cast<std::uint8_t>(CommitOutcome::Committed));
            // L0: the span the CV number refers to, from the validated inbound
            // event to the point a route intent exists and is eligible. It ends
            // here and not earlier, because before the commit there is no
            // eligible intent (doc 3, LI-29).
            if (src.receive_ns != 0)
                sample(Span::L0_ReceiveToRoute, src.receive_ns, r.commit_end_ns,
                       src.event_id, 0);
            if (src.type == EventType::ExecutionReport && src.receive_ns != 0)
                sample(Span::L5_Reconciliation, src.receive_ns, r.commit_end_ns,
                       src.event_id, 0);
            break;
        }
        case CommitOutcome::Unknown:
            // The transaction may or may not have committed. It is held and
            // resolved by the idempotency key; it is never retried as a fresh
            // business command (LI-17, LI-18, CW-17).
            ++stats_.commit_unknown;
            unresolved_unknown_.insert(r.idempotency_key);
            break;
        case CommitOutcome::Rejected: {
            // Nothing was written, so the staged state is simply discarded. The
            // identity is not marked applied: the event genuinely had no
            // effect and may legitimately be offered again.
            ++stats_.commit_rejected;
            // The sequence number this event consumed was never recorded by the
            // store, so it is returned. Leaving the gap would make every later
            // event for this domain fail the store's causal check, which would
            // look like a correctness failure but is really bookkeeping. Safe
            // to do here because at most one commit per domain is in flight, so
            // no later number has been handed out.
            auto it = next_seq_.find(src.risk_domain);
            if (it != next_seq_.end() && it->second == src.domain_seq) --it->second;
            break;
        }
    }
    LLX_FP(fp::point::kOwnerAfterCommitResult);
    publish_health();
}

void OwnerActor::on_commit_result(const CommitReceipt& r) noexcept {
    // A receipt from a previous epoch is stale. Applying it would let a
    // displaced owner's work mutate current state, which is exactly LI-47.
    if (r.owner_epoch < cfg_.owner_epoch) {
        ++stats_.receipts_stale;
        return;
    }

    const auto it = inflight_.find(r.risk_domain);
    if (it == inflight_.end()) {
        // No in-flight commit for this domain: the receipt is unsolicited or a
        // duplicate. Counted and ignored rather than guessed at.
        ++stats_.receipts_stale;
        return;
    }

    const std::uint64_t expected = applied_seq_[r.risk_domain] + 1;
    if (r.domain_seq != expected) {
        // Out of order. Doc 19.5: hold it, do not apply it, and do not let the
        // progression advance. The buffer is bounded by the in-flight cap.
        ++stats_.receipts_reordered;
        reorder_[r.risk_domain][r.domain_seq] = r;
        return;
    }

    StagedCommit staged = it->second;
    inflight_.erase(it);
    finish_commit(r, staged.source, staged.staged_ns);

    // Drain anything the buffer can now release, strictly in order.
    auto& buf = reorder_[r.risk_domain];
    for (;;) {
        const std::uint64_t want = applied_seq_[r.risk_domain] + 1;
        const auto bit = buf.find(want);
        if (bit == buf.end()) break;
        const CommitReceipt next = bit->second;
        buf.erase(bit);
        const auto nit = inflight_.find(next.risk_domain);
        if (nit == inflight_.end()) break;
        StagedCommit s2 = nit->second;
        inflight_.erase(nit);
        finish_commit(next, s2.source, s2.staged_ns);
    }
}

void OwnerActor::publish_health() {
    // Called by the owner thread after any change to the structures below, so
    // another thread can read the facts without walking them.
    published_unknown_.store(unresolved_unknown_.size(), std::memory_order_release);
    published_inflight_.store(inflight_.size(), std::memory_order_release);
    published_integrity_.store(integrity_violation_, std::memory_order_release);
    published_ownership_doubt_.store(ownership_uncertain_, std::memory_order_release);
}

void OwnerActor::contribute_health(HealthSnapshot& h) const {
    h.unresolved_unknown += published_unknown_.load(std::memory_order_acquire);
    h.persistence_inflight += published_inflight_.load(std::memory_order_acquire);
    if (published_integrity_.load(std::memory_order_acquire)) h.integrity_violation = true;
    if (published_ownership_doubt_.load(std::memory_order_acquire))
        h.ownership_uncertain = true;
}

}  // namespace llx
