// Admission control, service modes and the lane policy.
//
// Design doc 12. The rule the whole module exists to enforce is the first line
// of that section: no silently dropped accepted evidence. Two categories, and
// they are not interchangeable:
//
//   * a New command that has not reached durable admission may be refused
//     outright, with a reason, and counted (LI-21);
//   * an execution receipt P01 has already accepted is business evidence. If
//     there is no capacity for it the engine degrades -- stops intake, enters
//     RECON_ONLY -- but it does not drop it (LI-20).
//
// Watermarks are not thresholds on their own. Doc 12 requires hysteresis and a
// dwell time, because a controller that reacts to instantaneous occupancy
// oscillates between modes and the oscillation is itself a tail-latency source.
// So entering a worse mode needs the condition to persist for a dwell period,
// and leaving it needs occupancy to fall well below the entry threshold.
//
// The watermark values here (60/85/95) are the ones doc 12 gives as a lab
// policy and explicitly labels "not production thresholds". They are config,
// and the config hash in the manifest records what a run actually used.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llx/event.hpp"

namespace llx {

struct Watermarks {
    int watch_pct = 60;
    int shed_new_pct = 85;
    int stop_pct = 95;
    // How far below the entry threshold occupancy must fall before the mode is
    // relaxed again. Without this the controller flaps at the boundary.
    int release_hysteresis_pct = 10;
    // How long a condition must persist before it changes the mode. Doc 12:
    // "trigger based on hysteresis/time and downstream latency, not occupancy
    // alone".
    std::uint64_t dwell_ns = 5000000;  // 5 ms
    bool valid() const {
        return watch_pct > 0 && watch_pct < shed_new_pct && shed_new_pct < stop_pct &&
               stop_pct <= 100 && release_hysteresis_pct >= 0 &&
               release_hysteresis_pct < watch_pct;
    }
};

// Everything the controller looks at. Assembled by the engine each time it
// evaluates; passing it in keeps the controller a pure function of observable
// state, which is what makes its transitions testable without a running
// pipeline.
struct HealthSnapshot {
    std::size_t queue_occupancy = 0;
    std::size_t queue_capacity = 1;
    std::size_t pool_free = 0;
    std::size_t persistence_inflight = 0;
    std::size_t persistence_inflight_budget = 1;
    // False whenever the durable path cannot be trusted: a stalled store, an
    // unresolved COMMIT_UNKNOWN, a journal that failed to write. Doc 12: you
    // cannot claim free credits while P06 is unhealthy.
    bool store_healthy = true;
    // An outcome we have no evidence for. While this is non-zero the engine
    // may not issue new originals (LI-18).
    std::size_t unresolved_unknown = 0;
    // Set when an invariant has been violated: a corrupted ring counter, a
    // use-after-free, a projection that disagrees with the journal. There is no
    // safe forward path from here (doc 21).
    bool integrity_violation = false;
    // Set when ownership can no longer be proven: a stale epoch callback
    // touched state, or a fencing check failed.
    bool ownership_uncertain = false;
    // Preflight and the replay check that gate the exit from WARMUP.
    bool preflight_complete = false;
    bool replay_verified = false;
    // Operator authorization, required to leave RECON_ONLY (doc 21 runbook).
    bool reconciliation_complete = false;
    bool operator_authorized = false;

    int occupancy_pct() const {
        if (queue_capacity == 0) return 100;
        return static_cast<int>((queue_occupancy * 100) / queue_capacity);
    }
};

struct ModeTransition {
    ServiceMode from = ServiceMode::Warmup;
    ServiceMode to = ServiceMode::Warmup;
    std::uint64_t at_ns = 0;
    std::string reason;
};

class ServiceModeController {
public:
    explicit ServiceModeController(Watermarks wm = Watermarks{}) : wm_(wm) {}

    ServiceMode mode() const { return mode_; }
    const Watermarks& watermarks() const { return wm_; }

    // Evaluates the snapshot and returns the mode in force afterwards. Called
    // with a monotonic timestamp so a test can drive dwell periods exactly
    // rather than sleeping.
    ServiceMode evaluate(const HealthSnapshot& h, std::uint64_t now_ns);

    const std::vector<ModeTransition>& transitions() const { return log_; }

    // Forces a mode. Only two callers are legitimate: the integrity path, which
    // halts, and a supervised restart, which returns to WARMUP. Both are
    // recorded.
    void force(ServiceMode m, std::uint64_t now_ns, std::string reason);

    // --- lane policy -------------------------------------------------------
    //
    // The decision for one event, given the mode and the available credits.
    // This is where the asymmetry between an unaccepted command and accepted
    // evidence is implemented.
    AdmissionOutcome admit(Lane lane, bool accepted_evidence, const HealthSnapshot& h) const;

    // Credits available right now: the minimum across every bounded resource
    // in the path, because a credit that only one of them can honour is not a
    // credit (doc 12 "admission credit formula").
    static std::size_t free_credits(const HealthSnapshot& h);

private:
    bool dwelled(std::uint64_t now_ns) const {
        return condition_since_ns_ != 0 && now_ns >= condition_since_ns_ + wm_.dwell_ns;
    }
    void set_mode(ServiceMode m, std::uint64_t now_ns, std::string reason);

    Watermarks wm_;
    ServiceMode mode_ = ServiceMode::Warmup;
    // When the currently pending candidate condition was first observed.
    std::uint64_t condition_since_ns_ = 0;
    ServiceMode candidate_ = ServiceMode::Warmup;
    std::vector<ModeTransition> log_;
};

// Reserved credit held for the lifetime of one admission. Doc 12 requires a
// credit to be reserved atomically, consumed once and released once; an RAII
// holder is how that is made hard to get wrong.
class CreditLease {
public:
    CreditLease() = default;
    explicit CreditLease(std::size_t* counter) : counter_(counter) {
        if (counter_ != nullptr) ++(*counter_);
    }
    ~CreditLease() { release(); }
    CreditLease(const CreditLease&) = delete;
    CreditLease& operator=(const CreditLease&) = delete;
    CreditLease(CreditLease&& o) noexcept : counter_(o.counter_) { o.counter_ = nullptr; }
    CreditLease& operator=(CreditLease&& o) noexcept {
        if (this != &o) {
            release();
            counter_ = o.counter_;
            o.counter_ = nullptr;
        }
        return *this;
    }
    void release() {
        if (counter_ != nullptr) {
            --(*counter_);
            counter_ = nullptr;
        }
    }
    bool held() const { return counter_ != nullptr; }

private:
    std::size_t* counter_ = nullptr;
};

}  // namespace llx
