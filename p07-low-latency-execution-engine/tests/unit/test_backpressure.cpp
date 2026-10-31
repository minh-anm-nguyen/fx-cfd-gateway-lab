// Catalog groups 04 and 15 (LL-061..080, LL-281..300): bounded queues,
// backpressure, fairness and service modes.
//
// Oracle code S: the safety mode and the overload response are explicit and
// correct. These cases are where the asymmetry doc 12 insists on is checked:
// an unadmitted New may be refused, accepted business evidence may not be
// dropped.

#include <string>
#include <vector>

#include "llx/backpressure.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

HealthSnapshot healthy(std::size_t occupancy = 0, std::size_t capacity = 100) {
    HealthSnapshot h;
    h.queue_occupancy = occupancy;
    h.queue_capacity = capacity;
    h.pool_free = 1000;
    h.persistence_inflight = 0;
    h.persistence_inflight_budget = 4;
    h.store_healthy = true;
    h.preflight_complete = true;
    h.replay_verified = true;
    return h;
}

// Pushes the controller through a dwell period so a transition can complete.
ServiceMode settle(ServiceModeController& c, const HealthSnapshot& h, std::uint64_t& now) {
    c.evaluate(h, now);
    now += c.watermarks().dwell_ns + 1;
    return c.evaluate(h, now);
}

}  // namespace

SCENARIO("LL-061", "P0", "S", "the engine starts in WARMUP and admits nothing") {
    // LL-019: a load generator starting before the engine is warm must be
    // gated, not measured as steady state.
    ServiceModeController c;
    CHECK_EQ(c.mode(), ServiceMode::Warmup);
    HealthSnapshot h = healthy();
    h.preflight_complete = false;
    h.replay_verified = false;
    std::uint64_t now = 1000;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::Warmup);
    CHECK_EQ(c.admit(Lane::NewOrder, false, h), AdmissionOutcome::RetryLaterNoAdmission);
    // Session control is serviced even in WARMUP: without it there is no
    // session to recover anything on (LI-19).
    CHECK_EQ(c.admit(Lane::SessionControl, false, h),
             AdmissionOutcome::AcceptedForProcessing);
}

SCENARIO("LL-062", "P0", "S", "NORMAL is entered only after preflight and replay") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    HealthSnapshot h = healthy();
    h.replay_verified = false;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::Warmup);
    h.replay_verified = true;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::Normal);
    CHECK_EQ(c.transitions().size(), std::size_t{1});
    CHECK_EQ(c.transitions()[0].to, ServiceMode::Normal);
}

SCENARIO("LL-063", "P0", "S", "the watermarks escalate only after the dwell period") {
    // Doc 12 requires hysteresis and time, not an instantaneous reaction: a
    // controller that flips on a single reading oscillates, and the oscillation
    // is itself a tail-latency source.
    ServiceModeController c;
    std::uint64_t now = 1000;
    CHECK_EQ(c.evaluate(healthy(), now), ServiceMode::Normal);

    const HealthSnapshot busy = healthy(70);  // above watch (60), below shed (85)
    // First look: the condition is noted but not acted on.
    CHECK_EQ(c.evaluate(busy, now), ServiceMode::Normal);
    now += 1000;  // less than the dwell
    CHECK_EQ(c.evaluate(busy, now), ServiceMode::Normal);
    now += c.watermarks().dwell_ns;
    CHECK_EQ(c.evaluate(busy, now), ServiceMode::Watch);
}

SCENARIO("LL-064", "P0", "S", "a flapping signal never accumulates dwell") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    for (int i = 0; i < 100; ++i) {
        // Alternating between busy and idle: the candidate keeps changing, so
        // no transition should ever complete.
        now += c.watermarks().dwell_ns / 2;
        c.evaluate(healthy(70), now);
        now += c.watermarks().dwell_ns / 2;
        c.evaluate(healthy(10), now);
    }
    CHECK_EQ(c.mode(), ServiceMode::Normal);
}

SCENARIO("LL-065", "P0", "S", "SHED_NEW refuses new orders but still admits cancels") {
    // Doc 12 keeps progress on already-accepted commands available in
    // SHED_NEW, and a cancel reduces exposure, so refusing it would be the
    // wrong trade.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    const HealthSnapshot sat = healthy(90);
    // The controller moves to the mode the occupancy argues for, rather than
    // walking up one step at a time: 90% is past the shed threshold, so making
    // it pause at WATCH first would delay shedding for no benefit.
    CHECK_EQ(settle(c, sat, now), ServiceMode::ShedNew);

    CHECK_EQ(c.admit(Lane::NewOrder, false, sat), AdmissionOutcome::RejectedOverload);
    CHECK_EQ(c.admit(Lane::CancelReplace, false, sat),
             AdmissionOutcome::AcceptedForProcessing);
    // Accepted evidence is still taken: there is capacity, and dropping it is
    // never an option (LI-20).
    CHECK_EQ(c.admit(Lane::AcceptedReceipt, true, sat),
             AdmissionOutcome::AcceptedForProcessing);
    CHECK_EQ(c.admit(Lane::SessionControl, false, sat),
             AdmissionOutcome::AcceptedForProcessing);
}

SCENARIO("LL-066", "P0", "S",
         "relaxing requires occupancy below the entry threshold by the hysteresis margin") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    CHECK_EQ(settle(c, healthy(70), now), ServiceMode::Watch);

    // 55% is below the 60% entry but inside the 10-point hysteresis band, so
    // the mode holds.
    CHECK_EQ(settle(c, healthy(55), now), ServiceMode::Watch);
    // 45% is clear of the band.
    CHECK_EQ(settle(c, healthy(45), now), ServiceMode::Normal);
}

SCENARIO("LL-067", "P0", "S", "an integrity violation halts immediately, with no dwell") {
    // Doc 21: an SPSC invariant mismatch or a use-after-free has no safe
    // forward path, so waiting to see whether it persists would be wrong.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot bad = healthy();
    bad.integrity_violation = true;
    CHECK_EQ(c.evaluate(bad, now), ServiceMode::Halted);
    CHECK_EQ(c.admit(Lane::NewOrder, false, bad), AdmissionOutcome::EngineHalted);
    CHECK_EQ(c.admit(Lane::AcceptedReceipt, true, bad), AdmissionOutcome::EngineHalted);
    // Even session control stops: there is nothing safe left to do.
    CHECK_EQ(c.admit(Lane::SessionControl, false, bad), AdmissionOutcome::EngineHalted);
}

SCENARIO("LL-068", "P0", "S", "an unresolved unknown commit forces RECON_ONLY") {
    // LI-17 and LI-18: a COMMIT_UNKNOWN holds. New work is not issued while an
    // outcome is unproven.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot unknown = healthy();
    unknown.unresolved_unknown = 1;
    CHECK_EQ(c.evaluate(unknown, now), ServiceMode::ReconOnly);
    CHECK_EQ(c.admit(Lane::NewOrder, false, unknown),
             AdmissionOutcome::HeldForReconciliation);
    // Receipts are still processed: they are the evidence that resolves it.
    CHECK_EQ(c.admit(Lane::AcceptedReceipt, true, unknown),
             AdmissionOutcome::AcceptedForProcessing);
}

SCENARIO("LL-069", "P0", "S", "an unhealthy store brakes upstream to zero credits") {
    // Doc 12: "cannot claim free_credits > 0 if downstream P06 is unhealthy".
    HealthSnapshot h = healthy();
    CHECK(ServiceModeController::free_credits(h) > 0);
    h.store_healthy = false;
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{0});

    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    CHECK_EQ(c.evaluate(h, now), ServiceMode::ReconOnly);
}

SCENARIO("LL-070", "P0", "S", "credits are the minimum across every bounded resource") {
    // A credit only one resource can honour is not a credit.
    HealthSnapshot h = healthy();
    h.queue_capacity = 100;
    h.queue_occupancy = 10;   // 90 free
    h.pool_free = 5;          // the binding constraint
    h.persistence_inflight_budget = 50;
    h.persistence_inflight = 0;
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{5});

    h.pool_free = 1000;
    h.persistence_inflight = 48;  // 2 free
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{2});

    h.persistence_inflight = 50;  // nothing free
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{0});
}

SCENARIO("LL-071", "P0", "S",
         "accepted evidence with no capacity is deferred, never rejected") {
    // The single most important asymmetry in doc 12. An unadmitted New gets
    // REJECTED_OVERLOAD; accepted evidence gets RETRY_LATER, which obliges the
    // caller to hold it rather than discard it.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot none = healthy();
    none.pool_free = 0;  // no credits at all

    CHECK_EQ(c.admit(Lane::NewOrder, false, none), AdmissionOutcome::RejectedOverload);
    CHECK_EQ(c.admit(Lane::AcceptedReceipt, true, none),
             AdmissionOutcome::RetryLaterNoAdmission);
    CHECK_EQ(c.admit(Lane::NewOrder, true, none), AdmissionOutcome::RetryLaterNoAdmission);
    // And there is no outcome in the enumeration that means "dropped".
    CHECK_NE(c.admit(Lane::AcceptedReceipt, true, none), AdmissionOutcome::RejectedOverload);
}

SCENARIO("LL-072", "P0", "S", "telemetry is the only lane that may be dropped") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot none = healthy();
    none.pool_free = 0;
    CHECK_EQ(c.admit(Lane::Telemetry, false, none),
             AdmissionOutcome::RetryLaterNoAdmission);
    // With capacity it is taken; without, the caller just counts the loss and
    // carries on. It never blocks the owner (LI-24).
    CHECK_EQ(c.admit(Lane::Telemetry, false, healthy()),
             AdmissionOutcome::AcceptedForProcessing);
}

SCENARIO("LL-073", "P0", "S", "leaving RECON_ONLY needs reconciliation and authorization") {
    // Doc 21: it never happens on a timer.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot h = healthy();
    h.unresolved_unknown = 1;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::ReconOnly);

    // The unknown is resolved but nobody has signed off.
    h.unresolved_unknown = 0;
    now += 1000000000ULL;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::ReconOnly);

    h.reconciliation_complete = true;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::ReconOnly);
    h.operator_authorized = true;
    // And it resumes at WATCH, not NORMAL: the engine re-earns its confidence.
    CHECK_EQ(c.evaluate(h, now), ServiceMode::Watch);
}

SCENARIO("LL-074", "P0", "S", "leaving HALTED returns to WARMUP, not to service") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    HealthSnapshot bad = healthy();
    bad.integrity_violation = true;
    c.evaluate(bad, now);
    CHECK_EQ(c.mode(), ServiceMode::Halted);

    bad.integrity_violation = false;
    bad.reconciliation_complete = true;
    bad.operator_authorized = true;
    // Back to WARMUP so preflight and replay run again before anything is
    // admitted (doc 12 diagram).
    CHECK_EQ(c.evaluate(bad, now), ServiceMode::Warmup);
}

SCENARIO("LL-075", "P0", "S", "ownership uncertainty forces RECON_ONLY") {
    // LI-47: a late callback from a displaced owner may have touched state, so
    // only reconciliation may proceed.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    HealthSnapshot h = healthy();
    h.ownership_uncertain = true;
    CHECK_EQ(c.evaluate(h, now), ServiceMode::ReconOnly);
    CHECK(c.transitions().back().reason.find("ownership") != std::string::npos);
}

SCENARIO("LL-076", "P0", "S", "every transition is recorded with a reason") {
    // Doc 12 and LI-37: the quality mode is recorded and controlled before
    // trading continues. A mode change with no reason is unauditable.
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    settle(c, healthy(70), now);
    settle(c, healthy(90), now);
    CHECK(c.transitions().size() >= 3);
    for (const ModeTransition& t : c.transitions()) {
        CHECK(!t.reason.empty());
        CHECK_NE(t.from, t.to);
        CHECK(t.at_ns > 0);
    }
}

SCENARIO("LL-077", "P0", "S", "an invalid watermark configuration is rejected") {
    // LI-43 and doc 20: a capacity or threshold change is validated, not
    // absorbed.
    Watermarks ok;
    CHECK(ok.valid());

    Watermarks inverted;
    inverted.watch_pct = 90;
    inverted.shed_new_pct = 60;
    CHECK(!inverted.valid());

    Watermarks over;
    over.stop_pct = 101;
    CHECK(!over.valid());

    Watermarks no_room;
    no_room.release_hysteresis_pct = 60;  // equal to watch_pct: no stable band
    CHECK(!no_room.valid());

    Watermarks zero;
    zero.watch_pct = 0;
    CHECK(!zero.valid());
}

SCENARIO("LL-078", "P0", "S", "occupancy above the stop watermark goes to RECON_ONLY") {
    ServiceModeController c;
    std::uint64_t now = 1000;
    c.evaluate(healthy(), now);
    const HealthSnapshot jammed = healthy(98);
    settle(c, jammed, now);
    settle(c, jammed, now);
    settle(c, jammed, now);
    CHECK_EQ(c.mode(), ServiceMode::ReconOnly);
    CHECK_EQ(c.admit(Lane::NewOrder, false, jammed),
             AdmissionOutcome::HeldForReconciliation);
}

SCENARIO("LL-079", "P0", "S",
         "a credit lease is reserved once, consumed once and released once") {
    // Doc 12. The RAII holder makes the double-release impossible rather than
    // merely discouraged.
    std::size_t inflight = 0;
    {
        CreditLease a(&inflight);
        CHECK_EQ(inflight, std::size_t{1});
        CHECK(a.held());
        {
            CreditLease b(&inflight);
            CHECK_EQ(inflight, std::size_t{2});
        }
        CHECK_EQ(inflight, std::size_t{1});
        // Releasing twice by hand is a no-op, not a double decrement.
        a.release();
        CHECK_EQ(inflight, std::size_t{0});
        a.release();
        CHECK_EQ(inflight, std::size_t{0});
        CHECK(!a.held());
    }
    CHECK_EQ(inflight, std::size_t{0});

    // Moving transfers the obligation without duplicating it.
    {
        CreditLease a(&inflight);
        CreditLease b = std::move(a);
        CHECK_EQ(inflight, std::size_t{1});
        CHECK(!a.held());
        CHECK(b.held());
    }
    CHECK_EQ(inflight, std::size_t{0});
}

SCENARIO("LL-080", "P0", "S",
         "occupancy is clamped so a watermark cannot be computed from a bad reading") {
    HealthSnapshot h = healthy();
    h.queue_capacity = 0;  // nonsense: treated as fully saturated, not divided by
    CHECK_EQ(h.occupancy_pct(), 100);
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{0});
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "backpressure"); }
