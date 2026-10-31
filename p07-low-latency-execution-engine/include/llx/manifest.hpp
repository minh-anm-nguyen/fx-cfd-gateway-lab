// Run manifest and performance report.
//
// Design doc 27.2 lists the fields a performance report must carry, and doc
// 15.2 step 1 requires the environment to be frozen and recorded before any
// comparison. LI-48 makes the consequence explicit: a report missing its raw
// samples, its reconciliation or its environment manifest may not be marked
// PASS.
//
// So the verdict is computed, not asserted. `decide_verdict` implements the
// gating algorithm of doc 28 literally, and the only way to get
// CANDIDATE_FOR_P08_INTEGRATION out of it is to supply evidence for every gate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llx/affinity.hpp"
#include "llx/clock.hpp"
#include "llx/histogram.hpp"
#include "llx/telemetry.hpp"

namespace llx {

// Doc 28's gating algorithm outcomes, in its own words.
enum class Verdict : std::uint8_t {
    Incomplete,            // sources or tests NOT_RUN, or a required adapter BLOCKED
    NoGo,                  // a P0 failed, an LI was violated, or an unknown was sent unsafely
    NoPerformanceClaim,    // manifest missing or sample accounting invalid
    OptimizationRejected,  // regression beyond the accepted budget
    CandidateForP08,       // every gate satisfied
};

std::string_view to_string(Verdict v);

struct GateEvidence {
    // Gate 0: it builds and the tests are reproducible.
    bool implemented_and_reproducible = false;
    // Gate 1: correctness. Hard block.
    bool p0_cases_all_passed = false;
    bool sanitizers_clean = false;       // in the modes this host supports
    bool zero_invariant_violations = false;
    bool state_hash_matches_reference = false;
    // Gate 2: durability. Hard block.
    bool crash_windows_complete = false;
    bool unknown_commit_safe = false;
    bool restart_replay_verified = false;
    // Gate 3: resource bounds. Hard block.
    bool no_queue_overwrite = false;
    bool no_untracked_hot_allocations = false;
    bool no_leaks = false;
    bool overload_explicit = false;
    // Gate 4: performance evidence. Report plus policy.
    bool same_span_baseline_and_variant = false;
    bool open_loop_arrival = false;
    bool denominator_accounted = false;
    bool environment_manifest_present = false;
    bool raw_samples_retained = false;
    // Gate 5: operating readiness.
    bool runbooks_present = false;
    bool rollback_available = false;
    bool simulator_only = false;

    // Set when something listed above could not be executed on this host at
    // all. Doc 23: BLOCKED is a verdict, and it is not a pass.
    std::vector<std::string> blocked;
    std::vector<std::string> failures;
};

Verdict decide_verdict(const GateEvidence& e, const MeasurementValidity& v);

struct BuildInfo {
    std::string compiler;
    std::string cpp_standard;
    std::string build_type;
    bool failpoints_compiled_in = false;
    bool ownership_guard_compiled_in = false;
    bool asan = false;
    bool tsan = false;
    bool ubsan_trap = false;
    std::size_t cache_line_bytes = 0;
    std::size_t std_interference_size = 0;
    bool cache_line_matches_toolchain = true;
    static BuildInfo current();
    std::string to_json() const;
};

struct WorkloadInfo {
    std::string name;
    std::uint64_t seed = 0;
    std::string fixture_sha256;
    std::uint64_t target_rate_per_sec = 0;
    std::string arrival_mode;  // "open_loop", "closed_loop", "burst"
    std::string mix;
    std::uint64_t duration_ns = 0;
    std::string to_json() const;
};

struct ResourceInfo {
    std::uint64_t allocations_in_measured_region = 0;
    std::uint64_t peak_queue_occupancy = 0;
    std::uint64_t queue_capacity = 0;
    std::uint64_t pool_live = 0;
    std::uint64_t pool_capacity = 0;
    std::uint64_t pool_exhaustions = 0;
    SchedCounters sched{};
    std::string to_json() const;
};

struct CorrectnessInfo {
    std::string final_state_hash;
    std::string reference_state_hash;
    bool hashes_equal = false;
    std::uint64_t invariant_violations = 0;
    std::uint64_t duplicate_originals = 0;
    std::string detail;
    std::string to_json() const;
};

class RunManifest {
public:
    RunManifest(std::string run_id, std::string config_hash);

    void set_build(BuildInfo b) { build_ = std::move(b); }
    void set_topology(const Topology& t);
    void set_plan(const PlacementPlan& p);
    void set_workload(WorkloadInfo w) { workload_ = std::move(w); }
    void set_resources(ResourceInfo r) { resources_ = std::move(r); }
    void set_correctness(CorrectnessInfo c) { correctness_ = std::move(c); }
    void set_outcomes(OutcomeCounters o) { outcomes_ = o; }
    void set_validity(MeasurementValidity v) { validity_ = std::move(v); }
    void set_gates(GateEvidence g) { gates_ = std::move(g); }
    void add_span(Span s, const Histogram& h);
    void add_note(std::string n) { notes_.push_back(std::move(n)); }

    Verdict verdict() const { return decide_verdict(gates_, validity_); }

    std::string to_json() const;
    // Writes the manifest to `path`, creating parent directories.
    bool write(const std::string& path) const;

private:
    std::string run_id_;
    std::string config_hash_;
    BuildInfo build_{};
    std::string topology_json_ = "null";
    std::string plan_json_ = "null";
    WorkloadInfo workload_{};
    ResourceInfo resources_{};
    CorrectnessInfo correctness_{};
    OutcomeCounters outcomes_{};
    MeasurementValidity validity_{};
    GateEvidence gates_{};
    std::vector<std::pair<Span, std::string>> spans_;
    std::vector<std::string> notes_;
};

// A run id that is unique per process start without needing a UUID library:
// the monotonic clock reading, the process id and a hash of the build.
std::string make_run_id();

}  // namespace llx
