// Engine wiring and lifecycle.
//
// Design doc 6 and 7. The engine owns the pieces and the thread roles; it does
// not own business semantics. Two run shapes, deliberately:
//
//   deterministic  offer() publishes, step() consumes, nothing runs on its own.
//                  Every correctness test uses this. A crash window has to be
//                  reachable at an exact boundary (doc 26) and a differential
//                  run has to be reproducible from a seed (doc 23 layer 4);
//                  neither is possible if threads are racing in the background.
//   threaded       the roles of doc 7 on real threads. Used by the concurrency
//                  suite and the benchmark, which are the only places where
//                  thread interleaving is the thing under test.
//
// Queue capacity is a runtime choice because doc 15.1 sweeps 256/1024/8192/
// 65536, and SpscRing fixes its capacity at compile time. The indirection is
// doc 18's IQueueChannel. Note what doc 18 also says about it: virtual
// dispatch on the hot path is a cost to be measured, not assumed away, so the
// micro-benchmark compares the virtual channel against the template directly
// and the manifest records which the run used.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "llx/affinity.hpp"
#include "llx/backpressure.hpp"
#include "llx/dispatch.hpp"
#include "llx/owner.hpp"
#include "llx/pool.hpp"
#include "llx/spsc_ring.hpp"
#include "llx/store.hpp"
#include "llx/telemetry.hpp"

namespace llx {

// --- queue channel ---------------------------------------------------------

struct IQueueChannel {
    virtual bool try_publish(const InputEnvelope&) noexcept = 0;
    virtual bool try_consume(InputEnvelope&) noexcept = 0;
    virtual std::size_t capacity() const noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual bool halted() const noexcept = 0;
    virtual bool invariant_ok() const noexcept = 0;
    virtual std::uint64_t guard_violations() const noexcept = 0;
    virtual std::size_t peak_occupancy() const noexcept = 0;
    virtual ~IQueueChannel() = default;
};

template <std::size_t N>
class SpscChannel final : public IQueueChannel {
public:
    bool try_publish(const InputEnvelope& e) noexcept override {
        const bool ok = ring_.try_push(e);
        if (ok) {
            const std::size_t n = ring_.size();
            if (n > peak_) peak_ = n;
        }
        return ok;
    }
    bool try_consume(InputEnvelope& e) noexcept override { return ring_.try_pop(e); }
    std::size_t capacity() const noexcept override { return N; }
    std::size_t size() const noexcept override { return ring_.size(); }
    bool halted() const noexcept override { return ring_.halted(); }
    bool invariant_ok() const noexcept override { return ring_.invariant_ok(); }
    std::uint64_t guard_violations() const noexcept override {
        return ring_.guard_violations();
    }
    std::size_t peak_occupancy() const noexcept override { return peak_; }
    SpscRing<InputEnvelope, N>& ring() noexcept { return ring_; }

private:
    SpscRing<InputEnvelope, N> ring_;
    std::size_t peak_ = 0;
};

// Returns nullptr for a capacity that is not one of the compiled sizes, which
// is the config validation of doc 20: a capacity is rejected before any
// allocation rather than rounded silently.
std::unique_ptr<IQueueChannel> make_channel(std::size_t capacity);
std::vector<std::size_t> supported_capacities();

// Capacity of the dedicated FIX session-control channel.
//
// LI-19 requires session control to stay serviced however busy the business
// lane is, and doc 12 spells out the mechanism: "dedicated reserved processing
// budget". A single shared ring cannot provide that -- a business flood fills
// it and the next heartbeat is refused -- so control has a channel of its own.
// FI-09 and FI-10 are the cases that found this; they failed against a single
// shared ring.
//
// It is small on purpose. Session control is low volume, and a large control
// ring would only hide a session that has stopped being drained.
constexpr std::size_t kControlCapacity = 256;

// --- engine ----------------------------------------------------------------

struct EngineStats {
    std::uint64_t offered = 0;
    std::uint64_t published = 0;
    std::uint64_t control_published = 0;
    std::uint64_t control_deferred = 0;
    std::uint64_t publish_refused_full = 0;
    std::uint64_t publish_refused_mode = 0;
    // An accepted business proof that could not be published. It is NOT lost:
    // the offer returns RetryLaterNoAdmission and the caller holds it. This
    // counter exists so the test can prove the refusal happened and nothing
    // vanished (LI-20).
    std::uint64_t accepted_evidence_deferred = 0;
    std::uint64_t consumed = 0;
    std::uint64_t dispatched = 0;
    std::size_t peak_queue_occupancy = 0;
};

struct DrainReport {
    bool complete = false;
    std::uint64_t published = 0;
    std::uint64_t consumed = 0;
    std::uint64_t committed = 0;
    std::uint64_t still_staged = 0;
    std::uint64_t outbox_ready = 0;
    std::uint64_t outbox_ambiguous = 0;
    std::string detail;
};

class Engine {
public:
    struct Config {
        std::size_t spsc_capacity = 8192;
        std::uint32_t pool_slabs = 4;
        std::uint32_t pool_slots_per_slab = 8192;
        std::size_t persistence_inflight_budget = 1;
        CommitMode commit_mode = CommitMode::Synchronous;
        Watermarks watermarks{};
        std::string journal_path;      // empty: memory-only journal
        bool sync_on_commit = true;
        bool enforce_domain_sequence = true;
        bool attempt_pinning = false;  // doc 11 / ADR-07: off by default
        PlacementPolicy placement{};
        bool measure_spans = true;
        std::uint32_t initial_epoch = 1;
    };

    explicit Engine(Config cfg);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Topology discovery, placement planning, pool prewarm, config validation
    // and the replay check that lets the mode controller leave WARMUP. Returns
    // false when the configuration is rejected; doc 20 requires that to happen
    // before anything is allocated or admitted.
    bool preflight(std::string& reject_reason);

    // Producer side (T0). Stamps the enqueue timestamp before publishing, as
    // doc 3 requires, and applies the lane policy before the ring so an
    // overload refusal happens before durable admission (LI-21).
    AdmissionOutcome offer(InputEnvelope ev);

    // Owner side (T1). Consumes up to `max` events. Returns how many.
    std::size_t step(std::size_t max = 64);

    // Persistence side (T2), AsyncWorker mode only.
    std::size_t pump_persistence(std::size_t max = 64);

    // Dispatch side (T3).
    std::size_t pump_dispatch(std::size_t max = 64);

    // Metrics side (T4).
    std::size_t pump_telemetry();

    // Runs every stage until nothing moves. The deterministic driver every
    // correctness test uses.
    std::size_t run_until_idle(std::size_t budget = 1000000);

    // Re-evaluates the service mode from the current health.
    //
    // OWNER THREAD ONLY. ServiceModeController keeps a transition log and a
    // non-atomic mode, so two threads evaluating it concurrently is a race.
    // step() calls it after processing, which keeps the published mode current
    // as of the last event -- that is what CW-17 needs, and it is why
    // pump_dispatch reads the published value instead of evaluating again.
    ServiceMode refresh_mode();

    // The mode as last published by refresh_mode(). Safe from any thread.
    ServiceMode published_mode() const {
        return mode_cache_.load(std::memory_order_acquire);
    }

    // Walks the whole committed business state and checks its invariants.
    // O(orders + fills), so it is NOT part of health(): a per-event health
    // evaluation that audits the entire projection is a performance bug as
    // well as a threading one. Called by preflight, by self_check and by the
    // deterministic driver.
    bool business_state_ok() const;

    // Graceful stop: drain the queue and account for every accepted event, so
    // the report either shows a complete drain or says exactly what is still
    // outstanding (LI-23).
    DrainReport graceful_stop();

    // --- threaded runtime --------------------------------------------------
    void start_threads();
    void request_stop() { stop_requested_.store(true, std::memory_order_release); }
    void join_threads();
    bool threads_running() const { return !workers_.empty(); }

    // --- accessors ---------------------------------------------------------
    JournalStore& store() { return *store_; }
    OwnerActor& owner() { return *owner_; }
    Dispatcher& dispatcher() { return *dispatcher_; }
    RecordingSink& sink() { return *sink_; }
    TelemetryLane& telemetry() { return *telemetry_; }
    ServiceModeController& modes() { return *modes_; }
    IQueueChannel& queue() { return *queue_; }
    IQueueChannel& control_queue() { return *control_queue_; }
    Pool<InputEnvelope>& pool() { return *pool_; }
    const Topology& topology() const { return topo_; }
    const PlacementPlan& plan() const { return plan_; }
    const std::vector<PinResult>& pin_results() const { return pins_; }
    // Both are assembled from atomics, so they are exact in the deterministic
    // driver and consistent-per-field in the threaded runtime. The producer,
    // owner and dispatch roles all count, and plain integers shared between
    // them would be a data race (LI-26 cares about the numbers being right,
    // not only about them being collected).
    EngineStats stats() const;
    OutcomeCounters outcomes() const;
    const Config& config() const { return cfg_; }

    HealthSnapshot health() const;

    // The checks a release gate reads: every invariant this engine can observe
    // about itself, in one place.
    struct SelfCheck {
        bool queue_invariant = true;
        bool no_queue_guard_violations = true;
        bool pool_no_leaks = true;
        bool business_state_ok = true;
        bool no_duplicate_originals = true;
        bool outcomes_balanced = true;
        std::string detail;
        bool ok() const {
            return queue_invariant && no_queue_guard_violations && pool_no_leaks &&
                   business_state_ok && no_duplicate_originals && outcomes_balanced;
        }
    };
    SelfCheck self_check() const;

    // Marks the preflight and replay gates satisfied. Separated so a test can
    // show that the engine refuses to admit anything before they are.
    void mark_preflight_complete(bool v) { preflight_complete_ = v; }
    void mark_replay_verified(bool v) { replay_verified_ = v; }
    void authorize_operator(bool v) { operator_authorized_ = v; }
    void mark_reconciliation_complete(bool v) { reconciliation_complete_ = v; }

private:
    void record_outcome(AdmissionOutcome o, bool accepted_evidence);

    Config cfg_;
    std::unique_ptr<IQueueChannel> queue_;
    // Dedicated session-control channel, drained ahead of the business queue.
    std::unique_ptr<IQueueChannel> control_queue_;
    std::unique_ptr<Pool<InputEnvelope>> pool_;
    std::unique_ptr<JournalStore> store_;
    std::unique_ptr<ServiceModeController> modes_;
    std::unique_ptr<TelemetryLane> telemetry_;
    std::unique_ptr<OwnerActor> owner_;
    std::unique_ptr<RecordingSink> sink_;
    std::unique_ptr<Dispatcher> dispatcher_;

    Topology topo_;
    PlacementPlan plan_;
    std::vector<PinResult> pins_;

    // One atomic per counter. Relaxed throughout: these are statistics, and
    // no control decision is taken on them -- the mode controller reads the
    // queue and the owner's published health instead.
    struct AtomicStats {
        std::atomic<std::uint64_t> offered{0};
        std::atomic<std::uint64_t> published{0};
        std::atomic<std::uint64_t> control_published{0};
        std::atomic<std::uint64_t> control_deferred{0};
        std::atomic<std::uint64_t> publish_refused_full{0};
        std::atomic<std::uint64_t> publish_refused_mode{0};
        std::atomic<std::uint64_t> accepted_evidence_deferred{0};
        std::atomic<std::uint64_t> consumed{0};
        std::atomic<std::uint64_t> dispatched{0};
        std::atomic<std::size_t> peak_queue_occupancy{0};
    };
    struct AtomicOutcomes {
        std::atomic<std::uint64_t> offered{0};
        std::atomic<std::uint64_t> admitted{0};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> rejected_overload{0};
        std::atomic<std::uint64_t> rejected_invalid{0};
        std::atomic<std::uint64_t> held_unknown{0};
        std::atomic<std::uint64_t> duplicate_ignored{0};
        std::atomic<std::uint64_t> out_of_order{0};
        std::atomic<std::uint64_t> stale_epoch{0};
        std::atomic<std::uint64_t> halted{0};
        std::atomic<std::uint64_t> timed_out{0};
    };
    AtomicStats stats_{};
    AtomicOutcomes outcomes_{};
    bool preflight_complete_ = false;
    bool replay_verified_ = false;
    bool operator_authorized_ = false;
    bool reconciliation_complete_ = false;

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_requested_{false};
    // Published by refresh_mode() for threads that may not evaluate the
    // controller themselves.
    std::atomic<ServiceMode> mode_cache_{ServiceMode::Warmup};
};

}  // namespace llx
