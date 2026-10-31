// The baseline pipeline, for differential testing and A/B measurement.
//
// Design doc 23 layer 4: "same deterministic event stream into baseline vs
// optimized pipeline, compare orders, fills, risk_hold, outbox, journal hash
// after every committed step and restart". Doc 29 milestone M1 puts this first,
// before the SPSC ring and the pools, and gives the reason: pinning and
// lock-free structures built before a correctness baseline exists tend to hide
// correctness bugs rather than reveal them.
//
// So this is the deliberately plain implementation:
//   * a std::deque behind a std::mutex instead of the bounded SPSC ring;
//   * a heap allocation per event instead of a preallocated pool;
//   * no cache-line padding, no staging, no async worker, no telemetry lane.
//
// What it shares with the optimized engine is everything that must not differ:
// the same reduce(), the same to_intent(), the same JournalStore. That is the
// point of the comparison. Holding the business semantics fixed means a
// difference in the final state hash can only have come from the parts P07
// changed, which is what makes the differential test evidence about P07 rather
// than about the domain model (LI-01).
//
// It is also the fair-comparison control doc 15.3 asks for: the baseline is not
// handicapped with a shorter payload or weaker persistence settings, because
// "new allocator with shorter payload or lower persistence settings is not an
// optimization proof".
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "llx/domain.hpp"
#include "llx/event.hpp"
#include "llx/histogram.hpp"
#include "llx/store.hpp"

namespace llx {

class ReferencePipeline {
public:
    struct Config {
        std::string journal_path;  // empty: memory-only
        bool sync_on_commit = true;
        bool enforce_domain_sequence = true;
        // The baseline is unbounded by construction, which is exactly why it is
        // only a baseline: doc ADR-02 notes an unlimited queue absorbs bursts
        // but hides risk and eventually runs the process out of memory. A cap
        // is applied so a test cannot wedge the machine, and reaching it is
        // reported rather than ignored.
        std::size_t max_backlog = 1000000;
    };

    explicit ReferencePipeline(Config cfg);
    ~ReferencePipeline();

    AdmissionOutcome offer(const InputEnvelope& ev);
    std::size_t step(std::size_t max = 64);
    std::size_t run_until_idle(std::size_t budget = 1000000);

    JournalStore& store() { return *store_; }
    const JournalStore& store() const { return *store_; }
    std::string state_hash() const { return store_->state().state_hash(); }
    std::size_t backlog() const;

    struct Stats {
        std::uint64_t offered = 0;
        std::uint64_t consumed = 0;
        std::uint64_t committed = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t rejected = 0;
        std::uint64_t unknown = 0;
        std::uint64_t backlog_refused = 0;
        std::uint64_t heap_allocations = 0;
    };
    const Stats& stats() const { return stats_; }
    const OutcomeCounters& outcomes() const { return outcomes_; }

private:
    Config cfg_;
    std::unique_ptr<JournalStore> store_;
    mutable std::mutex mu_;
    // Heap-allocated per event on purpose: this is the allocation behaviour the
    // pooled path is measured against.
    std::deque<std::unique_ptr<InputEnvelope>> queue_;
    std::set<EventId> applied_;
    std::set<ExecId> seen_exec_;
    std::map<DomainId, std::uint64_t> next_seq_;
    Stats stats_{};
    OutcomeCounters outcomes_{};
};

}  // namespace llx
