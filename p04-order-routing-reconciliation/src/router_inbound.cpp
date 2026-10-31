// Inbound apply: receipt to business event to P03, commit, then acknowledge
// (design doc 11.3).
//
// The ordering here is the whole point. Dedup is checked before the reducer,
// the reducer runs on a copy, everything lands in one transaction, and only
// after the commit succeeds is the P01 receipt acknowledged. A crash anywhere
// in between leaves a redelivery that dedup turns into a no-op (CW-11 to
// CW-14).
#include <algorithm>

#include "router/router.hpp"

namespace router {

// Records a chain mutation the reducer did not make -- a hold or a quarantine
// -- so that a journal replay reproduces it exactly (OR-R-18, PB-28).
void journal_chain_mutation(Txn& txn, const p03::ChainState& chain) {
    JournalRecord jr;
    jr.kind = JournalRecord::Kind::ChainMutated;
    jr.chain_id = chain.chain_id;
    jr.chain_after = chain;
    jr.resulting_state_hash = chain.state_hash();
    txn.add_journal(jr);
}

void Router::open_recon(Txn& txn, const OrderChainId& chain,
                        const std::optional<CommandId>& cmd, HoldReason cause,
                        const Hash& evidence) {
    // One case per (chain, cause): design doc 14 wants several probes against
    // one incident, not one incident per probe (OR-M-12).
    for (const auto& [k, existing] : store_.image().recon_cases) {
        (void)k;
        if (existing.chain_id == chain && existing.cause == cause &&
            existing.state != ReconState::Reconciled)
            return;
    }
    ReconCase c;
    c.case_id = ReconCaseId("rc-" + std::to_string(store_.image().next_case_seq++));
    c.chain_id = chain;
    c.command_id = cmd;
    c.cause = cause;
    c.state = ReconState::Open;
    c.opened_mono = clock_.mono_now();
    c.next_due_mono = clock_.mono_now();
    c.last_evidence_hash = evidence;
    txn.put_recon(c);

    AuditRecord rec;
    rec.kind = AuditKind::ReconOpened;
    rec.actor = "system";
    rec.subject = c.case_id.str();
    rec.reason = std::string(to_string(cause));
    rec.detail = "chain=" + chain.str();
    rec.evidence = evidence;
    rec.wall = clock_.wall_now();
    rec.mono = clock_.mono_now();
    txn.add_audit(rec);
    metrics_.inc(metric::kReconOpen);
}

InboundStats Router::pump_inbound(std::size_t max) {
    InboundStats stats;

    const Route* route = nullptr;
    for (const Route& r : cfg_.routes) {
        const std::string key =
            r.sender_comp_id + ":" + r.target_comp_id + ":" + r.session_qualifier;
        if (key == session_.identity().key()) {
            route = &r;
            break;
        }
    }
    if (route == nullptr) {
        alert("no configured route matches the attached session");
        return stats;
    }

    for (std::size_t n = 0; n < max; ++n) {
        auto inbound = session_.peek_inbound();
        if (!inbound) break;

        const MonoMicros started = clock_.mono_now();

        // Transport dedup. A receipt already applied is acknowledged again and
        // produces no second effect (R-I07, CW-14, OR-K-01, OR-L-02).
        if (store_.receipt_applied(inbound->session_key, inbound->epoch,
                                   inbound->durable_inbox_id)) {
            session_.ack_inbound_after_commit(inbound->durable_inbox_id);
            ++stats.deduplicated;
            metrics_.inc(metric::kDuplicateExec, 1);
            continue;
        }

        // The quarantine quota is a hard boundary. Past it the router stops
        // draining and halts rather than losing evidence (OR-P-19, R-I26).
        if (store_.quarantine_depth() >= cfg_.limits.orphan_quota) {
            set_mode(RouterMode::Halt, "orphan_quota");
            alert("quarantine quota reached; inbound drain stopped");
            metrics_.inc(metric::kOrphanReports, "quota");
            break;
        }

        const NormalizeResult nr = normalizer_.parse_and_correlate(*inbound, *route);

        // CW-12: dying between normalization and the apply transaction. The
        // receipt is still unacknowledged, so it comes back.
        fp_.check(fp::kAfterNormalize);

        if (nr.outcome != NormalizeOutcome::Ok) {
            QuarantineEntry q;
            q.reason = nr.reason;
            q.session_key = inbound->session_key;
            q.epoch = inbound->epoch;
            q.receipt_id = inbound->durable_inbox_id;
            q.redacted_frame = redact_fix_frame(inbound->frame);
            q.raw_hash = inbound->raw_sha256;
            q.chain_hint = nr.chain_hint;
            q.created_mono = clock_.mono_now();
            q.note = sanitize_text(nr.note);

            AppliedReceipt ar;
            ar.session_key = inbound->session_key;
            ar.epoch = inbound->epoch;
            ar.receipt_id = inbound->durable_inbox_id;
            ar.raw_hash = inbound->raw_sha256;
            ar.outcome = "Quarantined";

            AuditRecord rec;
            rec.kind = AuditKind::InboundQuarantined;
            rec.actor = "peer";
            rec.subject = nr.chain_hint ? nr.chain_hint->str()
                                        : ("receipt-" + std::to_string(inbound->durable_inbox_id));
            rec.reason = std::string(to_string(nr.reason));
            rec.detail = sanitize_text(nr.note);
            rec.evidence = inbound->raw_sha256;
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();

            Txn txn = store_.begin();
            txn.add_quarantine(q);
            txn.add_applied_receipt(ar);
            txn.add_audit(rec);
            // A contradiction about a known chain also opens a case, so a human
            // sees it rather than only a counter moving.
            if (nr.chain_hint) {
                open_recon(txn, *nr.chain_hint, std::nullopt, nr.reason, inbound->raw_sha256);
                auto chain = store_.find_chain(*nr.chain_hint);
                if (chain != nullptr && !chain->hold &&
                    (nr.reason == HoldReason::SecurityQuarantine ||
                     nr.reason == HoldReason::IdentityConflict)) {
                    p03::ChainState held = *chain;
                    held.hold = nr.reason;
                    ++held.version;
                    txn.put_chain(held);
                    journal_chain_mutation(txn, held);
                }
            }
            auto c = txn.commit();
            if (c.is_err()) {
                // Fail closed: without a durable quarantine record the receipt
                // must not be acknowledged (R-I25).
                set_mode(RouterMode::Halt, "quarantine_commit");
                break;
            }
            session_.ack_inbound_after_commit(inbound->durable_inbox_id);

            if (nr.outcome == NormalizeOutcome::Orphan) {
                ++stats.orphaned;
                metrics_.inc(metric::kOrphanReports,
                             nr.note.find("not committed") != std::string::npos
                                 ? "chain_uncommitted"
                                 : "no_match");
            } else if (nr.outcome == NormalizeOutcome::Invalid) {
                ++stats.invalid;
                metrics_.inc(metric::kBusinessReject);
            } else {
                ++stats.quarantined;
            }
            metrics_.inc(metric::kHoldTotal, std::string(to_string(nr.reason)));
            alert("inbound quarantined: " + std::string(to_string(nr.reason)));
            continue;
        }

        const NormalizedReport& report = *nr.report;

        // Business dedup, before the reducer. Same key and same content is a
        // no-op; same key and different content is a conflict that must not be
        // resolved by overwriting (R-I07, OR-K-05, OR-K-06, OR-K-17).
        bool business_duplicate = false;
        bool execid_conflict = false;
        bool chain_conflict = false;
        if (report.has_business_exec_identity()) {
            const BusinessExecKey* existing =
                store_.find_exec_key(report.lp.str(), report.exec_id->str());
            if (existing != nullptr) {
                if (existing->payload_fingerprint != report.fingerprint)
                    execid_conflict = true;
                else if (existing->chain_id != report.chain_id)
                    chain_conflict = true;  // OR-K-16
                else
                    business_duplicate = true;
            }
        }

        if (execid_conflict || chain_conflict) {
            const HoldReason reason =
                execid_conflict ? HoldReason::ExecIdConflict : HoldReason::DuplicateExecIdChain;
            QuarantineEntry q;
            q.reason = reason;
            q.session_key = inbound->session_key;
            q.epoch = inbound->epoch;
            q.receipt_id = inbound->durable_inbox_id;
            q.redacted_frame = redact_fix_frame(inbound->frame);
            q.raw_hash = inbound->raw_sha256;
            q.chain_hint = report.chain_id;
            q.created_mono = clock_.mono_now();
            q.note = "ExecID " + report.exec_id->str() + " contradicts an applied execution";

            AppliedReceipt ar;
            ar.session_key = inbound->session_key;
            ar.epoch = inbound->epoch;
            ar.receipt_id = inbound->durable_inbox_id;
            ar.raw_hash = inbound->raw_sha256;
            ar.outcome = "Quarantined";

            AuditRecord rec;
            rec.kind = AuditKind::InboundQuarantined;
            rec.actor = "peer";
            rec.subject = report.chain_id.str();
            rec.reason = std::string(to_string(reason));
            rec.detail = q.note;
            rec.evidence = inbound->raw_sha256;
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();

            Txn txn = store_.begin();
            txn.add_quarantine(q);
            txn.add_applied_receipt(ar);
            txn.add_audit(rec);
            open_recon(txn, report.chain_id, std::nullopt, reason, inbound->raw_sha256);
            if (const p03::ChainState* chain = store_.find_chain(report.chain_id)) {
                p03::ChainState held = *chain;
                held.hold = reason;
                ++held.version;
                txn.put_chain(held);
                journal_chain_mutation(txn, held);
            }
            auto c = txn.commit();
            if (c.is_err()) {
                set_mode(RouterMode::Halt, "conflict_commit");
                break;
            }
            session_.ack_inbound_after_commit(inbound->durable_inbox_id);
            metrics_.inc(metric::kExecIdConflicts);
            metrics_.inc(metric::kHoldTotal, std::string(to_string(reason)));
            ++stats.quarantined;
            alert("ExecID conflict on chain " + report.chain_id.str());
            continue;
        }

        if (business_duplicate) {
            AppliedReceipt ar;
            ar.session_key = inbound->session_key;
            ar.epoch = inbound->epoch;
            ar.receipt_id = inbound->durable_inbox_id;
            ar.raw_hash = inbound->raw_sha256;
            ar.outcome = "Deduplicated";

            AuditRecord rec;
            rec.kind = AuditKind::InboundDeduplicated;
            rec.actor = "peer";
            rec.subject = report.chain_id.str();
            rec.reason = "business_execid_duplicate";
            rec.detail = "exec_id=" + report.exec_id->str() +
                         (report.poss_dup ? " poss_dup=Y" : "");
            rec.evidence = inbound->raw_sha256;
            rec.wall = clock_.wall_now();
            rec.mono = clock_.mono_now();

            Txn txn = store_.begin();
            txn.add_applied_receipt(ar);
            txn.add_audit(rec);
            auto c = txn.commit();
            if (c.is_err()) break;
            session_.ack_inbound_after_commit(inbound->durable_inbox_id);
            metrics_.inc(metric::kDuplicateExec);
            ++stats.deduplicated;
            continue;
        }

        // The reducer runs on a copy, so a failed commit cannot leave a mutated
        // projection behind in memory (design doc 5.2, commit-before-ack).
        const p03::ChainState* current = store_.find_chain(report.chain_id);
        if (current == nullptr) {
            ++stats.orphaned;
            session_.ack_inbound_after_commit(inbound->durable_inbox_id);
            continue;
        }
        p03::ChainState next = *current;
        const p03::ReduceResult rr = store_.reducer().apply(next, report);

        AppliedReceipt ar;
        ar.session_key = inbound->session_key;
        ar.epoch = inbound->epoch;
        ar.receipt_id = inbound->durable_inbox_id;
        ar.raw_hash = inbound->raw_sha256;

        Txn txn = store_.begin();
        AuditRecord rec;
        rec.actor = "peer";
        rec.subject = report.chain_id.str();
        rec.evidence = inbound->raw_sha256;
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();

        bool apply_state = false;
        switch (rr.outcome) {
            case p03::ReduceOutcome::Applied:
            case p03::ReduceOutcome::Evidence: {
                apply_state = true;
                ar.outcome = "Applied";
                rec.kind = AuditKind::InboundApplied;
                rec.reason = std::string(to_string(report.exec_type));
                rec.detail = sanitize_text(rr.note) + " version=" + std::to_string(next.version);
                ++stats.applied;
                break;
            }
            case p03::ReduceOutcome::Noop: {
                ar.outcome = "Deduplicated";
                rec.kind = AuditKind::InboundDeduplicated;
                rec.reason = "reducer_noop";
                ++stats.deduplicated;
                break;
            }
            case p03::ReduceOutcome::Hold: {
                ar.outcome = "Held";
                rec.kind = AuditKind::HoldOpened;
                rec.reason = std::string(to_string(rr.reason));
                rec.detail = sanitize_text(rr.note);
                p03::ChainState held = *current;
                held.hold = rr.reason;
                ++held.version;
                txn.put_chain(held);
                journal_chain_mutation(txn, held);
                open_recon(txn, report.chain_id, std::nullopt, rr.reason, inbound->raw_sha256);
                if (report.is_status_snapshot()) apply_status_evidence(txn, report, *current);
                metrics_.inc(metric::kHoldTotal, std::string(to_string(rr.reason)));
                ++stats.held;
                alert("chain " + report.chain_id.str() + " held: " +
                      std::string(to_string(rr.reason)));
                break;
            }
            case p03::ReduceOutcome::Quarantine: {
                ar.outcome = "Quarantined";
                rec.kind = AuditKind::InboundQuarantined;
                rec.reason = std::string(to_string(rr.reason));
                rec.detail = sanitize_text(rr.note);
                QuarantineEntry q;
                q.reason = rr.reason;
                q.session_key = inbound->session_key;
                q.epoch = inbound->epoch;
                q.receipt_id = inbound->durable_inbox_id;
                q.redacted_frame = redact_fix_frame(inbound->frame);
                q.raw_hash = inbound->raw_sha256;
                q.chain_hint = report.chain_id;
                q.created_mono = clock_.mono_now();
                q.note = sanitize_text(rr.note);
                txn.add_quarantine(q);
                open_recon(txn, report.chain_id, std::nullopt, rr.reason, inbound->raw_sha256);
                metrics_.inc(metric::kHoldTotal, std::string(to_string(rr.reason)));
                ++stats.quarantined;
                break;
            }
        }

        if (apply_state) {
            txn.put_chain(next);
            // The business dedup key is written in the same transaction as the
            // state change, which is what makes the pair atomic (R-I15, CW-13).
            if (report.has_business_exec_identity()) {
                BusinessExecKey bk;
                bk.lp_namespace = report.lp.str();
                bk.semantic_exec_id = report.exec_id->str();
                bk.payload_fingerprint = report.fingerprint;
                bk.chain_id = report.chain_id;
                bk.first_receipt = report.source_receipt;
                txn.put_exec_key(bk);
            }
            JournalRecord jr;
            jr.kind = JournalRecord::Kind::ReportApplied;
            jr.report = report;
            jr.chain_id = report.chain_id;
            jr.resulting_state_hash = next.state_hash();
            txn.add_journal(jr);

            // Command level consequences: a valid business report resolves the
            // delivery ambiguity of the request it answers.
            // A status snapshot answers a probe, not a request, so it never
            // decides the business outcome of a command. That stays with the
            // reconciliation coordinator (design doc 10.3: a snapshot is the
            // peer's state at a moment, not an acknowledgement).
            const CommandIntent* ci =
                report.is_status_snapshot()
                    ? nullptr
                    : store_.find_intent_by_cl_ord_id(report.lp, report.cl_ord_id);
            if (ci != nullptr) {
                CommandIntent updated = *ci;
                if (report.kind == ReportKind::CancelReject) {
                    // A 35=9 rejects the cancel or replace *request* named in
                    // tag 11. It says nothing about the order, which is why the
                    // outcome lands on the request's own command record and
                    // never on the chain (R-I11, R-I12).
                    updated.delivery = DeliveryState::LpRejected;
                    updated.business = BusinessOutcome::Rejected;
                } else {
                    updated.delivery = report.exec_type == ExecType::Rejected
                                           ? DeliveryState::LpRejected
                                           : DeliveryState::LpAcked;
                    switch (report.exec_type) {
                        case ExecType::Rejected:
                            updated.business = BusinessOutcome::Rejected;
                            break;
                        case ExecType::Canceled:
                            updated.business = BusinessOutcome::Canceled;
                            break;
                        case ExecType::Replaced:
                            updated.business = BusinessOutcome::Replaced;
                            break;
                        case ExecType::Trade:
                            updated.business = next.status == OrdStatus::Filled
                                                   ? BusinessOutcome::Filled
                                                   : BusinessOutcome::Acknowledged;
                            break;
                        default:
                            updated.business = BusinessOutcome::Acknowledged;
                            break;
                    }
                }
                txn.put_intent(updated);
                // Any open ambiguity for this command is now settled by
                // evidence, which is the only way a case closes (R-I17). A
                // status snapshot is handled separately below, because a summary
                // does not prove the outcome of a cancel or a replace.
                for (const auto& [k, c] : store_.image().recon_cases) {
                    if (report.is_status_snapshot()) break;
                    (void)k;
                    if (c.chain_id != report.chain_id) continue;
                    if (c.cause != HoldReason::AmbiguousSend && c.cause != HoldReason::AckTimeout)
                        continue;
                    if (c.state == ReconState::Reconciled) continue;
                    ReconCase closed = c;
                    closed.state = ReconState::Reconciled;
                    closed.resolved_by = "evidence";
                    closed.last_evidence_hash = inbound->raw_sha256;
                    closed.notes.push_back("closed by correlated business evidence");
                    txn.put_recon(closed);
                    AuditRecord cr;
                    cr.kind = AuditKind::ReconResolved;
                    cr.actor = "system";
                    cr.subject = closed.case_id.str();
                    cr.reason = "business_evidence";
                    cr.evidence = inbound->raw_sha256;
                    cr.wall = clock_.wall_now();
                    cr.mono = clock_.mono_now();
                    txn.add_audit(cr);
                }
            }

            // A status snapshot is evidence about the peer's view. The
            // comparison and the case transition live in router_recon.cpp.
            if (report.is_status_snapshot()) apply_status_evidence(txn, report, next);

            // Effects run after the commit, each with a stable idempotent key.
            for (const p03::Effect& e : rr.effects) {
                if (e.kind != p03::EffectKind::ReleaseRiskReservation) continue;
                if (const CommandIntent* origin = store_.find_intent_by_cl_ord_id(
                        report.lp, current->accepted_cl_ord_id)) {
                    if (!origin->risk_reservation_id.empty()) {
                        // CW-15: dying before the release means the saga replays
                        // it, and the release is idempotent so it lands once.
                        fp_.check(fp::kBeforeRiskEffect);
                        risk_.release(origin->risk_reservation_id);
                    }
                }
            }
        }

        txn.add_applied_receipt(ar);
        txn.add_audit(rec);

        // CW-13: dying inside the transaction leaves no half state.
        fp_.check(fp::kInsideApplyTxn);
        auto committed = txn.commit();
        if (committed.is_err()) {
            // R-I20: no acknowledgement without a commit. The receipt stays and
            // will be redelivered.
            alert("inbound apply commit failed: " + std::string(to_string(committed.error())));
            if (committed.error() != StoreError::CommitAckLost)
                set_mode(RouterMode::Halt, "apply_commit");
            break;
        }

        // Only now. CW-14 kills the process here, and the redelivery is a no-op.
        session_.ack_inbound_after_commit(inbound->durable_inbox_id);
        metrics_.observe(metric::kInboundCommitLatency, clock_.mono_now() - started);
    }

    metrics_.set_gauge(metric::kQuarantineDepth,
                       static_cast<std::int64_t>(store_.quarantine_depth()));
    return stats;
}

std::size_t Router::resolve_orphans() {
    // OR-L-06: a report that arrived before its chain was committed is not
    // lost. Correlation is attempted again, and anything that now matches goes
    // through the whole pipeline rather than being applied directly.
    std::size_t resolved = 0;
    const Route* route = nullptr;
    for (const Route& r : cfg_.routes) {
        const std::string key =
            r.sender_comp_id + ":" + r.target_comp_id + ":" + r.session_qualifier;
        if (key == session_.identity().key()) route = &r;
    }
    if (route == nullptr) return 0;

    std::vector<QuarantineEntry> retry;
    for (const QuarantineEntry& q : store_.image().quarantine) {
        if (q.resolved) continue;
        if (q.reason != HoldReason::OrphanReport) continue;
        retry.push_back(q);
    }

    for (const QuarantineEntry& q : retry) {
        p01::DurableFixInbound in;
        in.session_key = q.session_key;
        in.epoch = q.epoch;
        in.durable_inbox_id = q.receipt_id;
        in.frame = q.redacted_frame;
        in.raw_sha256 = q.raw_hash;
        const NormalizeResult nr = normalizer_.parse_and_correlate(in, *route);
        if (nr.outcome != NormalizeOutcome::Ok) continue;

        const NormalizedReport& report = *nr.report;
        const p03::ChainState* current = store_.find_chain(report.chain_id);
        if (current == nullptr) continue;
        if (report.has_business_exec_identity() &&
            store_.find_exec_key(report.lp.str(), report.exec_id->str()) != nullptr)
            continue;

        p03::ChainState next = *current;
        const p03::ReduceResult rr = store_.reducer().apply(next, report);
        if (rr.outcome != p03::ReduceOutcome::Applied &&
            rr.outcome != p03::ReduceOutcome::Evidence)
            continue;

        Txn txn = store_.begin();
        txn.put_chain(next);
        if (report.has_business_exec_identity()) {
            BusinessExecKey bk;
            bk.lp_namespace = report.lp.str();
            bk.semantic_exec_id = report.exec_id->str();
            bk.payload_fingerprint = report.fingerprint;
            bk.chain_id = report.chain_id;
            bk.first_receipt = report.source_receipt;
            txn.put_exec_key(bk);
        }
        JournalRecord jr;
        jr.kind = JournalRecord::Kind::ReportApplied;
        jr.report = report;
        jr.chain_id = report.chain_id;
        jr.resulting_state_hash = next.state_hash();
        txn.add_journal(jr);

        txn.resolve_quarantine(q.id, "resolved after the chain was committed");

        AuditRecord rec;
        rec.kind = AuditKind::InboundApplied;
        rec.actor = "system";
        rec.subject = report.chain_id.str();
        rec.reason = "orphan_resolved";
        rec.detail = sanitize_text(rr.note);
        rec.evidence = q.raw_hash;
        rec.wall = clock_.wall_now();
        rec.mono = clock_.mono_now();
        txn.add_audit(rec);
        if (txn.commit().is_ok()) ++resolved;
    }
    return resolved;
}

}  // namespace router
