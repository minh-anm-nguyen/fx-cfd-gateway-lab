#include "llx/backpressure.hpp"

#include <algorithm>

namespace llx {

std::size_t ServiceModeController::free_credits(const HealthSnapshot& h) {
    // An unhealthy durable path means no credits at all, whatever the queues
    // look like. Doc 12: "cannot claim free_credits > 0 if downstream P06 is
    // unhealthy; brake upstream."
    if (!h.store_healthy) return 0;
    const std::size_t queue_free =
        h.queue_capacity > h.queue_occupancy ? h.queue_capacity - h.queue_occupancy : 0;
    const std::size_t persist_free =
        h.persistence_inflight_budget > h.persistence_inflight
            ? h.persistence_inflight_budget - h.persistence_inflight
            : 0;
    return std::min({queue_free, h.pool_free, persist_free});
}

void ServiceModeController::set_mode(ServiceMode m, std::uint64_t now_ns,
                                     std::string reason) {
    if (m == mode_) return;
    log_.push_back(ModeTransition{mode_, m, now_ns, std::move(reason)});
    mode_ = m;
    condition_since_ns_ = 0;
    candidate_ = m;
}

void ServiceModeController::force(ServiceMode m, std::uint64_t now_ns, std::string reason) {
    set_mode(m, now_ns, std::move(reason));
}

ServiceMode ServiceModeController::evaluate(const HealthSnapshot& h, std::uint64_t now_ns) {
    // Integrity first, and with no dwell. Doc 21: an SPSC invariant mismatch or
    // a use-after-free has no safe forward path, so the engine halts
    // immediately rather than waiting to see whether it persists.
    if (h.integrity_violation) {
        set_mode(ServiceMode::Halted, now_ns,
                 "integrity violation: no safe progress, diagnostics only (doc 21)");
        return mode_;
    }

    // Ownership or durability uncertainty goes to RECON_ONLY, also without
    // dwell: continuing to dispatch while either is in doubt is the trade doc
    // 4 forbids ("liveness vs safety").
    const bool unsafe_evidence =
        h.ownership_uncertain || !h.store_healthy || h.unresolved_unknown > 0;

    switch (mode_) {
        case ServiceMode::Warmup:
            // Doc 12: NORMAL is entered only after preflight and the replay
            // check. LL-019 requires a load generator starting during warmup
            // to be gated rather than measured as steady state.
            if (h.preflight_complete && h.replay_verified && !unsafe_evidence)
                set_mode(ServiceMode::Normal, now_ns, "preflight and replay verified");
            else if (unsafe_evidence)
                set_mode(ServiceMode::ReconOnly, now_ns,
                         "durability or ownership unproven during warmup");
            return mode_;

        case ServiceMode::Halted:
            // Only a supervised restart leaves HALTED, and it goes back to
            // WARMUP so preflight and replay run again (doc 12 diagram).
            if (h.operator_authorized && h.reconciliation_complete && !h.integrity_violation)
                set_mode(ServiceMode::Warmup, now_ns,
                         "supervised restart authorized after reconciliation");
            return mode_;

        case ServiceMode::ReconOnly:
            // Doc 21: leaving RECON_ONLY needs the reconciliation finished and
            // an operator decision. It never happens on a timer.
            if (!unsafe_evidence && h.reconciliation_complete && h.operator_authorized)
                set_mode(ServiceMode::Watch, now_ns,
                         "reconciliation complete and authorized; resuming at WATCH");
            return mode_;

        default: break;
    }

    if (unsafe_evidence) {
        set_mode(ServiceMode::ReconOnly, now_ns,
                 h.ownership_uncertain
                     ? "ownership uncertain: only reconciliation may proceed (LI-47)"
                     : (h.unresolved_unknown > 0
                            ? "unresolved COMMIT_UNKNOWN: hold and reconcile (LI-17/18)"
                            : "durable store unhealthy: no new originals (LI-16)"));
        return mode_;
    }

    const int pct = h.occupancy_pct();

    // Pick the mode the current occupancy argues for.
    ServiceMode want = ServiceMode::Normal;
    if (pct >= wm_.stop_pct) want = ServiceMode::ReconOnly;
    else if (pct >= wm_.shed_new_pct) want = ServiceMode::ShedNew;
    else if (pct >= wm_.watch_pct) want = ServiceMode::Watch;

    // Relaxing requires occupancy below the entry threshold by the hysteresis
    // margin, so the controller does not flap at a boundary.
    if (static_cast<int>(want) < static_cast<int>(mode_)) {
        int entry = 0;
        switch (mode_) {
            case ServiceMode::Watch: entry = wm_.watch_pct; break;
            case ServiceMode::ShedNew: entry = wm_.shed_new_pct; break;
            case ServiceMode::ReconOnly: entry = wm_.stop_pct; break;
            default: entry = wm_.watch_pct; break;
        }
        if (pct > entry - wm_.release_hysteresis_pct) {
            // Still inside the hysteresis band: hold the current mode.
            condition_since_ns_ = 0;
            return mode_;
        }
    }

    if (want == mode_) {
        condition_since_ns_ = 0;
        return mode_;
    }

    // Dwell: the condition has to persist. Tracked per candidate so a flapping
    // signal never accumulates dwell toward a transition.
    if (candidate_ != want || condition_since_ns_ == 0) {
        candidate_ = want;
        condition_since_ns_ = now_ns;
        return mode_;
    }
    if (!dwelled(now_ns)) return mode_;

    const std::string reason =
        "occupancy " + std::to_string(pct) + "% sustained for the dwell period (watch " +
        std::to_string(wm_.watch_pct) + "%, shed " + std::to_string(wm_.shed_new_pct) +
        "%, stop " + std::to_string(wm_.stop_pct) + "%)";
    set_mode(want, now_ns, reason);
    return mode_;
}

AdmissionOutcome ServiceModeController::admit(Lane lane, bool accepted_evidence,
                                              const HealthSnapshot& h) const {
    // Session control is serviced in every mode short of HALTED. LI-19: the
    // business lane being busy must never cost the session its heartbeat,
    // because losing the session loses the resend path that recovers everything
    // else.
    if (lane == Lane::SessionControl)
        return mode_ == ServiceMode::Halted ? AdmissionOutcome::EngineHalted
                                            : AdmissionOutcome::AcceptedForProcessing;

    // Telemetry is the one lane that may be dropped. The caller counts the
    // loss; it never blocks the owner (LI-24).
    if (lane == Lane::Telemetry) {
        return free_credits(h) > 0 ? AdmissionOutcome::AcceptedForProcessing
                                   : AdmissionOutcome::RetryLaterNoAdmission;
    }

    if (mode_ == ServiceMode::Halted) return AdmissionOutcome::EngineHalted;

    // Accepted business evidence. There is no outcome here that discards it:
    // either it is taken, or the caller is told to retry while the engine
    // degrades. LI-20 forbids the third option.
    if (accepted_evidence || lane == Lane::AcceptedReceipt) {
        if (free_credits(h) == 0) {
            // Bounded safe mode rather than a drop. The caller must hold the
            // event -- P01's inbound buffer and the session resend contract are
            // what cover it meanwhile (doc 12, doc 19.2).
            return AdmissionOutcome::RetryLaterNoAdmission;
        }
        return AdmissionOutcome::AcceptedForProcessing;
    }

    switch (mode_) {
        case ServiceMode::ReconOnly:
            // LL-006: a New arriving in RECON_ONLY is not issued. It is
            // refused with a reason and audited.
            return AdmissionOutcome::HeldForReconciliation;
        case ServiceMode::ShedNew:
            // Cancels still go through: they reduce exposure, and doc 12 keeps
            // progress on already-accepted commands available in SHED_NEW. The
            // causal check lives in the owner, not here, so a cancel cannot
            // overtake its own New by being admitted earlier (LI-36).
            if (lane == Lane::CancelReplace)
                return free_credits(h) > 0 ? AdmissionOutcome::AcceptedForProcessing
                                           : AdmissionOutcome::RejectedOverload;
            return AdmissionOutcome::RejectedOverload;
        case ServiceMode::Warmup:
            // Nothing is admitted before the engine is warm: measuring or
            // routing during warmup is LL-019.
            return AdmissionOutcome::RetryLaterNoAdmission;
        default: break;
    }

    if (free_credits(h) == 0) return AdmissionOutcome::RejectedOverload;
    return AdmissionOutcome::AcceptedForProcessing;
}

}  // namespace llx
