#include "llx/engine.hpp"

#include <algorithm>
#include <sstream>

#include "llx/failpoint.hpp"

namespace llx {

// --- channel factory -------------------------------------------------------

std::vector<std::size_t> supported_capacities() {
    // The sweep of doc 15.1. Each one is a separate template instantiation, so
    // the set is fixed at build time and a capacity outside it is a config
    // error rather than a silent rounding.
    return {256, 1024, 8192, 65536};
}

std::unique_ptr<IQueueChannel> make_channel(std::size_t capacity) {
    switch (capacity) {
        case 256: return std::make_unique<SpscChannel<256>>();
        case 1024: return std::make_unique<SpscChannel<1024>>();
        case 8192: return std::make_unique<SpscChannel<8192>>();
        case 65536: return std::make_unique<SpscChannel<65536>>();
        default: return nullptr;
    }
}

// --- construction ----------------------------------------------------------

Engine::Engine(Config cfg) : cfg_(std::move(cfg)) {}

Engine::~Engine() {
    request_stop();
    join_threads();
}

bool Engine::preflight(std::string& reject_reason) {
    reject_reason.clear();

    // Config validation first, before anything is allocated. Doc 20: a bad
    // capacity is rejected before allocation, not after.
    if (!cfg_.watermarks.valid()) {
        reject_reason =
            "watermarks must satisfy 0 < watch < shed_new < stop <= 100 with a "
            "hysteresis margin smaller than the watch threshold";
        return false;
    }
    control_queue_ = make_channel(kControlCapacity);
    queue_ = make_channel(cfg_.spsc_capacity);
    if (queue_ == nullptr) {
        std::ostringstream o;
        o << "spsc_capacity " << cfg_.spsc_capacity
          << " is not one of the compiled power-of-two capacities (";
        bool first = true;
        for (std::size_t c : supported_capacities()) {
            if (!first) o << ", ";
            first = false;
            o << c;
        }
        o << ")";
        reject_reason = o.str();
        return false;
    }
    if (cfg_.pool_slabs == 0 || cfg_.pool_slots_per_slab == 0) {
        reject_reason = "pool must have at least one slab and one slot per slab";
        return false;
    }
    // Doc 20: M >= max_queue_references + outstanding async handles + margin.
    const std::size_t pool_total =
        static_cast<std::size_t>(cfg_.pool_slabs) * cfg_.pool_slots_per_slab;
    const std::size_t needed = cfg_.spsc_capacity + cfg_.persistence_inflight_budget + 64;
    if (pool_total < needed) {
        std::ostringstream o;
        o << "pool of " << pool_total << " slots is smaller than the queue capacity plus "
          << "in-flight budget plus margin (" << needed
          << "); a full queue would then exhaust the pool and the refusal would come "
             "from the wrong resource (doc 20)";
        reject_reason = o.str();
        return false;
    }
    if (cfg_.persistence_inflight_budget == 0) {
        reject_reason = "persistence_inflight_budget must be at least 1";
        return false;
    }

    pool_ = std::make_unique<Pool<InputEnvelope>>(cfg_.pool_slabs, cfg_.pool_slots_per_slab);
    // First touch before any measured region, so the first real lease does not
    // take a page fault inside it (doc 15.2 step 3).
    pool_->prewarm();

    JournalStore::Options so;
    so.journal_path = cfg_.journal_path;
    so.sync_on_commit = cfg_.sync_on_commit;
    so.enforce_domain_sequence = cfg_.enforce_domain_sequence;
    so.initial_epoch = cfg_.initial_epoch;
    store_ = std::make_unique<JournalStore>(so);

    modes_ = std::make_unique<ServiceModeController>(cfg_.watermarks);
    telemetry_ = std::make_unique<TelemetryLane>();

    OwnerActor::Config oc;
    oc.commit_mode = cfg_.commit_mode;
    oc.owner_epoch = std::max(cfg_.initial_epoch, store_->fencing_epoch());
    oc.measure_spans = cfg_.measure_spans;
    owner_ = std::make_unique<OwnerActor>(oc, *store_, *modes_, *telemetry_);

    sink_ = std::make_unique<RecordingSink>();
    dispatcher_ = std::make_unique<Dispatcher>(*store_, *sink_);

    // Topology and placement. Discovery always runs, because the manifest needs
    // it either way; pinning only when asked, because doc 11 and ADR-07 keep
    // it opt-in and host-specific.
    topo_ = discover_topology();
    plan_ = plan_placement(topo_, cfg_.placement);
    if (cfg_.attempt_pinning && plan_.accepted) {
        // The owner's own pin is applied on the owner thread, not here. What
        // this records is that the plan was accepted; the PinResults are filled
        // in by the threads themselves, where the affinity call has to happen.
    }

    preflight_complete_ = true;
    // A journal with records has to be replayed and checked before the engine
    // admits anything: doc 12 gates the exit from WARMUP on it, and LL-018
    // requires a restart with pending orders not to assume an empty book.
    const BusinessState::Check c = store_->state().check();
    if (!c.ok()) {
        reject_reason = "replayed business state fails its invariants: " + c.detail;
        replay_verified_ = false;
        return false;
    }
    replay_verified_ = true;
    refresh_mode();
    return true;
}

// --- producer side ---------------------------------------------------------

void Engine::record_outcome(AdmissionOutcome o, bool accepted_evidence) {
    switch (o) {
        case AdmissionOutcome::AcceptedForProcessing: ++outcomes_.admitted; break;
        case AdmissionOutcome::RejectedOverload: ++outcomes_.rejected_overload; break;
        case AdmissionOutcome::InvalidInput: ++outcomes_.rejected_invalid; break;
        case AdmissionOutcome::HeldForReconciliation: ++outcomes_.held_unknown; break;
        case AdmissionOutcome::DuplicateIgnored: ++outcomes_.duplicate_ignored; break;
        case AdmissionOutcome::OutOfOrder: ++outcomes_.out_of_order; break;
        case AdmissionOutcome::StaleEpoch: ++outcomes_.stale_epoch; break;
        case AdmissionOutcome::EngineHalted: ++outcomes_.halted; break;
        case AdmissionOutcome::RetryLaterNoAdmission:
            // Not an outcome yet: the caller still holds the event. Counted
            // separately so the denominator stays balanced (LI-26).
            if (accepted_evidence) ++stats_.accepted_evidence_deferred;
            break;
    }
}

AdmissionOutcome Engine::offer(InputEnvelope ev) {
    // Before preflight there is no queue, no pool and no mode controller, so
    // nothing could safely accept an event. Refusing is the honest answer, and
    // it is what makes LL-019 checkable: a generator that starts before the
    // engine is ready gets a refusal rather than a crash or an admission.
    if (queue_ == nullptr || control_queue_ == nullptr || modes_ == nullptr ||
        owner_ == nullptr) {
        ++stats_.offered;
        ++stats_.publish_refused_mode;
        return AdmissionOutcome::EngineHalted;
    }

    ++stats_.offered;
    ++outcomes_.offered;

    const HealthSnapshot h = health();
    const Lane lane = (ev.lane == Lane::NewOrder) ? lane_of(ev.type) : ev.lane;
    ev.lane = lane;

    const AdmissionOutcome policy = modes_->admit(lane, ev.accepted_evidence, h);
    if (policy != AdmissionOutcome::AcceptedForProcessing) {
        ++stats_.publish_refused_mode;
        // The refusal happens here, before the ring and before any durable
        // write. That ordering is LI-21: a drop or reject may only happen
        // upstream of durable admission, and it carries a reason.
        record_outcome(policy, ev.accepted_evidence);
        if (policy == AdmissionOutcome::RetryLaterNoAdmission) --outcomes_.offered;
        return policy;
    }

    // The enqueue timestamp is taken before the publish, so the queue-wait span
    // starts where the event actually entered the queue (doc 3).
    ev.admission_ns = MonotonicClock::now_ns();
    if (ev.receive_ns == 0) ev.receive_ns = ev.admission_ns;
    ev.owner_epoch = owner_->epoch();

    // Session control goes to its own channel, so a saturated business lane
    // cannot cost the session its liveness (LI-19). If even the control
    // channel is full the answer is a deferral, never a rejection: losing a
    // heartbeat loses the session, and with it the resend path that recovers
    // everything else.
    if (lane == Lane::SessionControl) {
        if (!control_queue_->try_publish(ev)) {
            ++stats_.control_deferred;
            --outcomes_.offered;
            return AdmissionOutcome::RetryLaterNoAdmission;
        }
        ++stats_.control_published;
        return AdmissionOutcome::AcceptedForProcessing;
    }

    if (!queue_->try_publish(ev)) {
        ++stats_.publish_refused_full;
        // A full ring is never overwritten (LI-02). For an unaccepted command
        // this is an explicit overload rejection; for accepted business
        // evidence it is a deferral the caller must honour, because dropping it
        // is what LI-20 forbids.
        if (ev.accepted_evidence || lane == Lane::AcceptedReceipt) {
            ++stats_.accepted_evidence_deferred;
            --outcomes_.offered;
            return AdmissionOutcome::RetryLaterNoAdmission;
        }
        record_outcome(AdmissionOutcome::RejectedOverload, false);
        return AdmissionOutcome::RejectedOverload;
    }
    ++stats_.published;
    const std::size_t peak = queue_->peak_occupancy();
    std::size_t seen = stats_.peak_queue_occupancy.load(std::memory_order_relaxed);
    while (peak > seen &&
           !stats_.peak_queue_occupancy.compare_exchange_weak(seen, peak)) {}
    return AdmissionOutcome::AcceptedForProcessing;
}

// --- owner side ------------------------------------------------------------

std::size_t Engine::step(std::size_t max) {
    std::size_t n = 0;
    InputEnvelope ev;
    // Control first, and without a budget cap of its own: it is low volume and
    // carries no business effect, so draining it fully cannot starve the
    // business lane for long. Doc 12's lane table puts it above everything
    // else for exactly this reason.
    while (control_queue_->try_consume(ev)) {
        ++stats_.consumed;
        const AdmissionOutcome o = owner_->on_event(ev);
        record_outcome(o, false);
        // A serviced control event is a terminal outcome: it carries no
        // business effect, so being handled is all there is. Without this it
        // would be offered and never accounted, and the denominator would not
        // balance -- which is what FI-09 caught.
        if (o == AdmissionOutcome::AcceptedForProcessing) ++outcomes_.completed;
        ++n;
    }
    while (n < max && queue_->try_consume(ev)) {
        ++stats_.consumed;
        const AdmissionOutcome o = owner_->on_event(ev);
        record_outcome(o, ev.accepted_evidence);
        if (o == AdmissionOutcome::AcceptedForProcessing &&
            cfg_.commit_mode == CommitMode::Synchronous)
            ++outcomes_.completed;
        ++n;
    }
    // Publish the mode while the commit results are fresh. CW-17: an UNKNOWN
    // commit has to be reflected before the dispatcher looks, or an original
    // goes out for a transaction whose outcome is unproven.
    if (n > 0) refresh_mode();
    return n;
}

std::size_t Engine::pump_persistence(std::size_t max) {
    if (cfg_.commit_mode != CommitMode::AsyncWorker) return 0;
    std::size_t n = 0;
    OwnerActor::StagedCommit staged;
    while (n < max && owner_->take_staged(staged)) {
        const std::uint64_t begin = MonotonicClock::now_ns();
        CommitReceipt r;
        const CommitOutcome outcome = store_->commit(staged.request, r);
        r.outcome = outcome;
        r.commit_begin_ns = begin;
        r.commit_end_ns = MonotonicClock::now_ns();
        owner_->on_commit_result(r);
        if (outcome == CommitOutcome::Committed) ++outcomes_.completed;
        ++n;
    }
    return n;
}

std::size_t Engine::pump_dispatch(std::size_t max) {
    // Eligibility is decided here, at dispatch time, from current health --
    // never from a mode that was evaluated earlier in the same pass.
    //
    // This is not a stylistic preference. CW-17 found the bug: run_until_idle
    // evaluated the mode, then step() processed an event whose commit came back
    // UNKNOWN, and the dispatcher in that same iteration still saw the stale
    // NORMAL mode and put the original on the wire. An original for a
    // transaction whose outcome is unproven is precisely what LI-16 and LI-18
    // forbid, so the mode is refreshed before the decision is taken.
    // The mode published by the owner after it processed the last event. Not
    // re-evaluated here: the controller is the owner's, and evaluating it from
    // the dispatch thread raced the owner's own maps.
    const ServiceMode m = published_mode();
    if (m == ServiceMode::Halted || m == ServiceMode::ReconOnly) return 0;
    const std::size_t n = dispatcher_->pump(owner_->epoch(), max);
    stats_.dispatched += n;
    return n;
}

std::size_t Engine::pump_telemetry() { return telemetry_->drain(); }

ServiceMode Engine::refresh_mode() {
    const ServiceMode m = modes_->evaluate(health(), MonotonicClock::now_ns());
    mode_cache_.store(m, std::memory_order_release);
    return m;
}

std::size_t Engine::run_until_idle(std::size_t budget) {
    std::size_t total = 0;
    refresh_mode();
    while (total < budget) {
        std::size_t moved = step(64);
        moved += pump_persistence(64);
        // step() refreshed the mode if it moved anything, so the dispatcher
        // sees the state after the commits rather than before them.
        moved += pump_dispatch(64);
        moved += pump_telemetry();
        if (moved == 0) break;
        total += moved;
        refresh_mode();
    }
    return total;
}

EngineStats Engine::stats() const {
    EngineStats s;
    s.offered = stats_.offered.load(std::memory_order_relaxed);
    s.published = stats_.published.load(std::memory_order_relaxed);
    s.control_published = stats_.control_published.load(std::memory_order_relaxed);
    s.control_deferred = stats_.control_deferred.load(std::memory_order_relaxed);
    s.publish_refused_full = stats_.publish_refused_full.load(std::memory_order_relaxed);
    s.publish_refused_mode = stats_.publish_refused_mode.load(std::memory_order_relaxed);
    s.accepted_evidence_deferred =
        stats_.accepted_evidence_deferred.load(std::memory_order_relaxed);
    s.consumed = stats_.consumed.load(std::memory_order_relaxed);
    s.dispatched = stats_.dispatched.load(std::memory_order_relaxed);
    s.peak_queue_occupancy = stats_.peak_queue_occupancy.load(std::memory_order_relaxed);
    return s;
}

OutcomeCounters Engine::outcomes() const {
    OutcomeCounters o;
    o.offered = outcomes_.offered.load(std::memory_order_relaxed);
    o.admitted = outcomes_.admitted.load(std::memory_order_relaxed);
    o.completed = outcomes_.completed.load(std::memory_order_relaxed);
    o.rejected_overload = outcomes_.rejected_overload.load(std::memory_order_relaxed);
    o.rejected_invalid = outcomes_.rejected_invalid.load(std::memory_order_relaxed);
    o.held_unknown = outcomes_.held_unknown.load(std::memory_order_relaxed);
    o.duplicate_ignored = outcomes_.duplicate_ignored.load(std::memory_order_relaxed);
    o.out_of_order = outcomes_.out_of_order.load(std::memory_order_relaxed);
    o.stale_epoch = outcomes_.stale_epoch.load(std::memory_order_relaxed);
    o.halted = outcomes_.halted.load(std::memory_order_relaxed);
    o.timed_out = outcomes_.timed_out.load(std::memory_order_relaxed);
    return o;
}

HealthSnapshot Engine::health() const {
    HealthSnapshot h;
    if (queue_ != nullptr) {
        // Watermarks are computed from the business queue only. The control
        // channel is deliberately excluded: a full control channel is a session
        // problem, not a reason to shed business work.
        h.queue_occupancy = queue_->size();
        h.queue_capacity = queue_->capacity();
        // A ring that quarantined itself is an integrity violation, not a
        // capacity problem (LL-040, doc 21).
        if (queue_->halted() || !queue_->invariant_ok()) h.integrity_violation = true;
        if (queue_->guard_violations() != 0) h.integrity_violation = true;
    }
    if (control_queue_ != nullptr) {
        if (control_queue_->halted() || !control_queue_->invariant_ok())
            h.integrity_violation = true;
        if (control_queue_->guard_violations() != 0) h.integrity_violation = true;
    }
    h.pool_free = (pool_ != nullptr) ? pool_->free_slots() : 0;
    h.persistence_inflight_budget = cfg_.persistence_inflight_budget;
    h.store_healthy = true;
    h.preflight_complete = preflight_complete_;
    h.replay_verified = replay_verified_;
    h.operator_authorized = operator_authorized_;
    h.reconciliation_complete = reconciliation_complete_;
    // Only atomics from here: contribute_health reads published values, never
    // the owner's maps. The full business-state audit deliberately does not
    // happen here -- it walks every order and fill, which is both too slow for
    // a per-event evaluation and unsafe from any thread but the owner's.
    if (owner_ != nullptr) owner_->contribute_health(h);
    return h;
}

bool Engine::business_state_ok() const {
    if (store_ == nullptr) return true;
    return store_->state().check().ok();
}

Engine::SelfCheck Engine::self_check() const {
    SelfCheck s;
    std::ostringstream d;
    if (queue_ != nullptr) {
        s.queue_invariant = queue_->invariant_ok() && !queue_->halted();
        if (!s.queue_invariant) d << "queue invariant or halt flag; ";
        s.no_queue_guard_violations = (queue_->guard_violations() == 0);
        if (!s.no_queue_guard_violations)
            d << "ring ownership guard fired " << queue_->guard_violations() << " times; ";
    }
    if (pool_ != nullptr) {
        s.pool_no_leaks = pool_->no_leaks();
        if (!s.pool_no_leaks) d << pool_->live() << " pool slots still live; ";
    }
    if (store_ != nullptr) {
        // Safe here: self_check is called from the owning thread, or after the
        // workers have been joined.
        const BusinessState::Check c = store_->state().check();
        s.business_state_ok = c.ok();
        if (!c.ok()) d << c.detail;
    }
    if (dispatcher_ != nullptr) {
        s.no_duplicate_originals = (dispatcher_->stats().duplicate_original_attempts == 0);
        if (!s.no_duplicate_originals)
            d << dispatcher_->stats().duplicate_original_attempts
              << " duplicate original send attempts were refused; ";
    }
    const OutcomeCounters oc = outcomes();
    s.outcomes_balanced = oc.balanced();
    if (!s.outcomes_balanced)
        d << "outcome accounting unbalanced: offered " << oc.offered << " vs "
          << oc.accounted() << " accounted; ";
    s.detail = d.str();
    return s;
}

DrainReport Engine::graceful_stop() {
    DrainReport r;
    // Drain everything that was accepted. LI-23: a graceful stop either drains
    // or states exactly what is still outstanding, and the proof for anything
    // outstanding is the durable journal.
    run_until_idle();
    r.published = stats_.published.load(std::memory_order_relaxed);
    r.consumed = stats_.consumed.load(std::memory_order_relaxed);
    r.committed = owner_->stats().committed;
    r.still_staged = owner_->inflight();
    for (const OutboxRecord& ob : store_->outbox_snapshot()) {
        if (ob.state == OutboxState::Ready) ++r.outbox_ready;
        if (ob.state == OutboxState::Ambiguous) ++r.outbox_ambiguous;
    }
    r.complete = (queue_->size() == 0) && (control_queue_->size() == 0) &&
                 (r.still_staged == 0);
    std::ostringstream d;
    if (!r.complete)
        d << "drain incomplete: " << queue_->size() << " queued, " << r.still_staged
          << " staged. Their authority is the journal, which has "
          << store_->record_count() << " records; ";
    if (r.outbox_ambiguous != 0)
        d << r.outbox_ambiguous
          << " outbox rows are ambiguous and remain owned by reconciliation (LI-18); ";
    r.detail = d.str();
    return r;
}

// --- threaded runtime ------------------------------------------------------

void Engine::start_threads() {
    if (!workers_.empty()) return;
    stop_requested_.store(false, std::memory_order_release);
    pins_.clear();
    pins_.resize(static_cast<std::size_t>(ThreadRole::Count));

    auto pin_for = [this](ThreadRole role) {
        if (!cfg_.attempt_pinning || !plan_.accepted) return;
        const RolePlacement* rp = plan_.for_role(role);
        if (rp == nullptr) return;
        // The pin happens on the thread it applies to, and the result is kept
        // so the manifest can state whether the placement was verified (LI-31).
        pins_[static_cast<std::size_t>(role)] = pin_current_thread(rp->cpu);
    };

    // T1 owner: consumes the ingress ring. One consumer for the ring's whole
    // lifetime, which is the SPSC contract (LI-03).
    workers_.emplace_back([this, pin_for] {
        pin_for(ThreadRole::Owner);
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (step(64) == 0) std::this_thread::yield();
        }
        step(1000000);  // final drain
    });

    if (cfg_.commit_mode == CommitMode::AsyncWorker) {
        workers_.emplace_back([this, pin_for] {
            pin_for(ThreadRole::Persistence);
            while (!stop_requested_.load(std::memory_order_acquire)) {
                if (pump_persistence(64) == 0) std::this_thread::yield();
            }
            pump_persistence(1000000);
        });
    }

    workers_.emplace_back([this, pin_for] {
        pin_for(ThreadRole::Dispatch);
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (pump_dispatch(64) == 0) std::this_thread::yield();
        }
        pump_dispatch(1000000);
    });

    // T4 metrics: drains telemetry off the hot path. Its failure must be
    // visible but must never block the owner (LI-24), which is why it only ever
    // reads the lane's ring.
    workers_.emplace_back([this, pin_for] {
        pin_for(ThreadRole::Metrics);
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (pump_telemetry() == 0) std::this_thread::yield();
        }
        pump_telemetry();
    });

    // T5 maintenance: mode evaluation and plan drift detection, both cold path.
    // T5 maintenance: plan drift detection only. It deliberately does NOT
    // evaluate the mode controller -- that belongs to the owner thread, and
    // having two threads do it raced the controller's transition log and the
    // owner's maps.
    workers_.emplace_back([this, pin_for] {
        pin_for(ThreadRole::Maintenance);
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::string drift;
            if (cfg_.attempt_pinning && plan_.accepted && !plan_still_valid(plan_, drift)) {
                // The allowed CPU set moved under us. Doc 21: degrade rather
                // than keep quoting a placement that no longer exists.
                plan_.accepted = false;
                plan_.reject_reason = drift;
            }
            std::this_thread::yield();
        }
    });
}

void Engine::join_threads() {
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
}

}  // namespace llx
