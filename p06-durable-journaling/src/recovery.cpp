#include "jrnl/recovery.hpp"

#include <algorithm>

#include "jrnl/failpoint.hpp"

namespace jrnl {

std::string_view to_string(Phase p) {
    switch (p) {
        case Phase::Stopped: return "STOPPED";
        case Phase::AcquireFence: return "ACQUIRE_FENCE";
        case Phase::VerifyStorage: return "VERIFY_STORAGE";
        case Phase::ReplayLocal: return "REPLAY_LOCAL";
        case Phase::VerifyProjections: return "VERIFY_PROJECTIONS";
        case Phase::ReconcileRemote: return "RECONCILE_REMOTE";
        case Phase::ReconOnly: return "RECON_ONLY";
        case Phase::Ready: return "READY";
        case Phase::Quarantine: return "QUARANTINE";
        case Phase::Faulted: return "FAULTED";
    }
    return "STOPPED";
}

std::string_view to_string(RecoveryMode m) {
    switch (m) {
        case RecoveryMode::ProcessRestart: return "PROCESS_RESTART";
        case RecoveryMode::RestoredFromBackup: return "RESTORED_FROM_BACKUP";
        case RecoveryMode::PromotedStandby: return "PROMOTED_STANDBY";
    }
    return "PROCESS_RESTART";
}

bool RecoveryReport::has(Incident i) const {
    return std::find(incidents.begin(), incidents.end(), i) != incidents.end();
}

RecoverySupervisor::RecoverySupervisor(SqliteAtomicStore& store, Options opt)
    : store_(store), opt_(std::move(opt)) {
    report_.phase = Phase::Stopped;
}

bool RecoverySupervisor::acquire_fences(RecoveryReport* rep) {
    tokens_.clear();
    for (const RiskDomainId& d : opt_.domains) {
        FenceToken t = 0;
        const StoreError e = store_.acquire_fence(opt_.tenant, d, opt_.owner, opt_.lease_ms, &t);
        if (e != StoreError::None || t == 0) {
            rep->incidents.push_back(Incident::StaleFence);
            rep->details.push_back("cannot acquire fence for domain " + d + ": " +
                                   std::string(to_string(e)));
            return false;
        }
        tokens_[d] = t;
    }
    return true;
}

RecoveryReport RecoverySupervisor::verify_and_replay(RecoveryMode mode) {
    const std::int64_t t0 = store_.clock().steady_ms();
    RecoveryReport rep;
    rep.mode = mode;
    rep.dispatch_enabled = false;  // JI-24: disabled until every gate passes
    demotion_reason_.clear();

    Metrics* m = opt_.metrics;

    // --- ACQUIRE_FENCE -----------------------------------------------------
    rep.phase = Phase::AcquireFence;
    if (!acquire_fences(&rep)) {
        rep.phase = Phase::Faulted;
        rep.duration_ms = store_.clock().steady_ms() - t0;
        report_ = rep;
        return report_;
    }
    rep.fencing_valid = true;

    // --- VERIFY_STORAGE, REPLAY_LOCAL, VERIFY_PROJECTIONS ------------------
    // One verifier pass covers all three: it checks the file, then every
    // stream's chain, then projection equivalence. The phases are reported
    // separately because an operator needs to know which one failed.
    rep.phase = Phase::VerifyStorage;
    const VerifyReport v = verify_all(store_, opt_.tenant);
    rep.storage_valid = v.storage_valid;
    rep.journal_valid = v.journal_valid;
    rep.projections_valid = v.projections_valid && v.manifests_valid && v.dedup_valid &&
                            v.audit_valid;
    rep.streams_checked = v.streams_checked;
    rep.events_checked = v.events_checked;
    rep.incidents.insert(rep.incidents.end(), v.incidents.begin(), v.incidents.end());
    rep.details.insert(rep.details.end(), v.details.begin(), v.details.end());
    rep.frozen_domains = v.frozen_domains;

    JRNL_FP(fp::point::kRecoveryAfterReplay);

    if (!rep.storage_valid) {
        if (m != nullptr) m->inc(metric::kRecoveryIntegrityFail);
        rep.phase = Phase::Faulted;
        rep.duration_ms = store_.clock().steady_ms() - t0;
        report_ = rep;
        return report_;
    }
    rep.phase = Phase::ReplayLocal;
    if (!rep.journal_valid) {
        if (m != nullptr) {
            m->inc(metric::kRecoveryIntegrityFail);
            if (v.has(Incident::StreamVersionGap)) m->inc(metric::kJournalVersionGap);
        }
        // JI-12 and JI-39: a history that does not verify means no trade in
        // the affected scope, and the instance does not reach READY.
        rep.phase = Phase::Quarantine;
        rep.duration_ms = store_.clock().steady_ms() - t0;
        report_ = rep;
        return report_;
    }
    rep.phase = Phase::VerifyProjections;
    if (!rep.projections_valid) {
        if (m != nullptr) {
            m->inc(metric::kRecoveryIntegrityFail);
            if (v.has(Incident::ProjectionReplayMismatch)) {
                m->inc(metric::kProjectionReplayMismatch);
            }
            if (v.has(Incident::RiskLedgerDiff)) m->inc(metric::kRiskReservationLedgerDiff);
        }
        rep.phase = Phase::Quarantine;
        rep.duration_ms = store_.clock().steady_ms() - t0;
        report_ = rep;
        return report_;
    }

    // --- RECONCILE_REMOTE ---------------------------------------------------
    rep.phase = Phase::ReconcileRemote;

    // Everything whose outcome at the counterparty is not established.
    const std::vector<OutboxRow> unknown = store_.outbox_rows(opt_.tenant, "UNKNOWN");
    const std::vector<OutboxRow> held = store_.outbox_rows(opt_.tenant, "HELD");
    const std::vector<OutboxRow> bound = store_.outbox_rows(opt_.tenant, "BOUND");
    rep.unresolved_effects = unknown.size() + held.size();
    rep.open_cases = store_.open_cases(opt_.tenant).size();
    if (m != nullptr) {
        m->set(metric::kOutboxUnknownCount, static_cast<std::int64_t>(unknown.size()));
    }

    // A BOUND row means a sequence was persisted at P01. JI-16 and JI-17 say
    // that is not evidence the counterparty acted, so these count toward the
    // reconciliation burden too.
    const bool local_unresolved =
        !unknown.empty() || !held.empty() || !bound.empty() || rep.open_cases > 0;

    const bool restart_can_trust_local = mode == RecoveryMode::ProcessRestart;
    rep.external_reconciliation_required =
        local_unresolved || (!restart_can_trust_local && opt_.require_external_evidence);

    if (rep.external_reconciliation_required && !opt_.external_evidence_provided) {
        // RECON_ONLY: readable, replayable, reconcilable. Not accepting new
        // business and not dispatching. Section 1.2.
        for (const OutboxRow& r : unknown) {
            if (!r.account.empty()) rep.frozen_domains.insert(r.account);
        }
        rep.phase = Phase::ReconOnly;
        rep.details.push_back(
            "holding in RECON_ONLY: " + std::to_string(rep.unresolved_effects) +
            " unresolved outbound effects, " + std::to_string(bound.size()) +
            " bound without counterparty evidence, " + std::to_string(rep.open_cases) +
            " open cases, mode " + std::string(to_string(mode)));
        if (m != nullptr) m->inc(metric::kAdmissionBlocked);
        rep.duration_ms = store_.clock().steady_ms() - t0;
        report_ = rep;
        return report_;
    }

    if (!rep.frozen_domains.empty()) {
        // Some domains are frozen but the rest can serve. The instance is
        // READY with a reduced scope rather than all-or-nothing, which is
        // what section 14's runbook asks for when it says to reopen per
        // tenant and domain.
        rep.details.push_back(std::to_string(rep.frozen_domains.size()) +
                              " risk domains remain frozen");
    }

    rep.phase = Phase::Ready;
    rep.dispatch_enabled = true;
    rep.duration_ms = store_.clock().steady_ms() - t0;
    report_ = rep;
    return report_;
}

bool RecoverySupervisor::may_admit(const TenantId& tenant, const RiskDomainId& domain) const {
    if (!demotion_reason_.empty()) return false;
    if (tenant != opt_.tenant) return false;  // JI-33
    if (report_.phase != Phase::Ready) return false;
    return report_.frozen_domains.find(domain) == report_.frozen_domains.end();
}

bool RecoverySupervisor::may_dispatch(const TenantId& tenant, const RiskDomainId& domain) const {
    // Dispatch is strictly narrower than admission: it also requires the
    // fence this instance holds to still be the current one, because a
    // dispatch is an outward-facing effect and JI-21 applies to it.
    if (!may_admit(tenant, domain)) return false;
    if (!report_.dispatch_enabled) return false;
    const auto it = tokens_.find(domain);
    if (it == tokens_.end()) return false;
    return store_.current_fence(tenant, domain) == it->second;
}

std::string RecoverySupervisor::why_blocked(const TenantId& tenant,
                                            const RiskDomainId& domain) const {
    if (!demotion_reason_.empty()) return "demoted: " + demotion_reason_;
    if (tenant != opt_.tenant) return "tenant " + tenant + " is not served by this instance";
    if (report_.phase != Phase::Ready) {
        return "supervisor is in " + std::string(to_string(report_.phase));
    }
    if (report_.frozen_domains.find(domain) != report_.frozen_domains.end()) {
        return "risk domain " + domain + " is frozen by an integrity incident";
    }
    const auto it = tokens_.find(domain);
    if (it == tokens_.end()) return "no fence held for domain " + domain;
    if (store_.current_fence(tenant, domain) != it->second) {
        return "fencing token for domain " + domain + " has been superseded";
    }
    return "";
}

FenceToken RecoverySupervisor::token(const RiskDomainId& domain) const {
    const auto it = tokens_.find(domain);
    return it == tokens_.end() ? 0 : it->second;
}

void RecoverySupervisor::demote(const std::string& reason) {
    demotion_reason_ = reason.empty() ? "unspecified" : reason;
    report_.phase = Phase::ReconOnly;
    report_.dispatch_enabled = false;
    report_.details.push_back("demoted: " + demotion_reason_);
    if (opt_.metrics != nullptr) opt_.metrics->inc(metric::kAdmissionBlocked);
}

void RecoverySupervisor::provide_external_evidence(const std::string& approvals) {
    // Section 14: closing a case needs evidence and dual approval. An empty
    // approvals string is refused rather than quietly accepted.
    if (approvals.empty()) return;
    approvals_ = approvals;
    opt_.external_evidence_provided = true;
}

}  // namespace jrnl
