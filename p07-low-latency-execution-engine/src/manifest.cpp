#include "llx/manifest.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "llx/hash.hpp"
#include "llx/spsc_ring.hpp"

#if defined(_WIN32)
#include <process.h>
#define LLX_GETPID _getpid
#else
#include <unistd.h>
#define LLX_GETPID getpid
#endif

namespace llx {

std::string_view to_string(Verdict v) {
    switch (v) {
        case Verdict::Incomplete: return "INCOMPLETE";
        case Verdict::NoGo: return "NO_GO";
        case Verdict::NoPerformanceClaim: return "NO_PERFORMANCE_CLAIM";
        case Verdict::OptimizationRejected: return "OPTIMIZATION_REJECTED";
        case Verdict::CandidateForP08: return "CANDIDATE_FOR_P08_INTEGRATION";
    }
    return "UNKNOWN";
}

Verdict decide_verdict(const GateEvidence& e, const MeasurementValidity& v) {
    // Doc 28's algorithm, in its order. The order matters: an INCOMPLETE run is
    // not a NO_GO, and a run that is correct but unmeasurable is not a failure
    // of the engine, it is a failure of the measurement.
    if (!e.implemented_and_reproducible || !e.blocked.empty()) return Verdict::Incomplete;

    const bool gate1 = e.p0_cases_all_passed && e.sanitizers_clean &&
                       e.zero_invariant_violations && e.state_hash_matches_reference;
    const bool gate2 =
        e.crash_windows_complete && e.unknown_commit_safe && e.restart_replay_verified;
    const bool gate3 = e.no_queue_overwrite && e.no_untracked_hot_allocations && e.no_leaks &&
                       e.overload_explicit;
    if (!gate1 || !gate2 || !gate3 || !e.failures.empty()) return Verdict::NoGo;

    if (!e.environment_manifest_present || !e.raw_samples_retained ||
        !e.denominator_accounted || !v.performance_claim_admissible())
        return Verdict::NoPerformanceClaim;

    if (!e.same_span_baseline_and_variant || !e.open_loop_arrival)
        return Verdict::NoPerformanceClaim;

    if (!e.runbooks_present || !e.rollback_available || !e.simulator_only)
        return Verdict::Incomplete;

    return Verdict::CandidateForP08;
}

BuildInfo BuildInfo::current() {
    BuildInfo b;
    std::ostringstream c;
#if defined(__clang__)
    c << "clang " << __clang_major__ << "." << __clang_minor__;
#elif defined(__GNUC__)
    c << "gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#else
    c << "unknown";
#endif
    b.compiler = c.str();
    b.cpp_standard = std::to_string(__cplusplus);
#if defined(NDEBUG)
    b.build_type = "optimized (NDEBUG)";
#else
    b.build_type = "debug (assertions enabled)";
#endif
#if defined(LLX_ENABLE_FAILPOINTS)
    b.failpoints_compiled_in = true;
#endif
#if !defined(LLX_NO_OWNERSHIP_GUARD)
    b.ownership_guard_compiled_in = true;
#endif
#if defined(__SANITIZE_ADDRESS__)
    b.asan = true;
#endif
#if defined(__SANITIZE_THREAD__)
    b.tsan = true;
#endif
#if defined(LLX_UBSAN_TRAP_BUILD)
    b.ubsan_trap = true;
#endif
    b.cache_line_bytes = kCacheLineBytes;
    b.std_interference_size = kStdInterferenceSize;
    // Qualified: the member of the same name would otherwise shadow the
    // free function in class scope.
    b.cache_line_matches_toolchain = ::llx::cache_line_matches_toolchain();
    return b;
}

std::string BuildInfo::to_json() const {
    std::ostringstream o;
    o << "{\"compiler\":\"" << compiler << "\",\"cpp_standard\":\"" << cpp_standard
      << "\",\"build_type\":\"" << build_type << "\",\"failpoints_compiled_in\":"
      << (failpoints_compiled_in ? "true" : "false") << ",\"ownership_guard_compiled_in\":"
      << (ownership_guard_compiled_in ? "true" : "false") << ",\"asan\":"
      << (asan ? "true" : "false") << ",\"tsan\":" << (tsan ? "true" : "false")
      << ",\"ubsan_trap\":" << (ubsan_trap ? "true" : "false") << ",\"cache_line_bytes\":"
      << cache_line_bytes << ",\"std_interference_size\":" << std_interference_size
      << ",\"cache_line_matches_toolchain\":"
      << (cache_line_matches_toolchain ? "true" : "false") << "}";
    return o.str();
}

std::string WorkloadInfo::to_json() const {
    std::ostringstream o;
    o << "{\"name\":\"" << name << "\",\"seed\":" << seed << ",\"fixture_sha256\":\""
      << fixture_sha256 << "\",\"target_rate_per_sec\":" << target_rate_per_sec
      << ",\"arrival_mode\":\"" << arrival_mode << "\",\"mix\":\"" << mix
      << "\",\"duration_ns\":" << duration_ns << "}";
    return o.str();
}

std::string ResourceInfo::to_json() const {
    std::ostringstream o;
    o << "{\"allocations_in_measured_region\":" << allocations_in_measured_region
      << ",\"peak_queue_occupancy\":" << peak_queue_occupancy << ",\"queue_capacity\":"
      << queue_capacity << ",\"pool_live\":" << pool_live << ",\"pool_capacity\":"
      << pool_capacity << ",\"pool_exhaustions\":" << pool_exhaustions
      << ",\"sched\":{\"available\":" << (sched.available ? "true" : "false")
      << ",\"voluntary_switches\":" << sched.voluntary_switches
      << ",\"involuntary_switches\":" << sched.involuntary_switches
      << ",\"migrations\":" << sched.migrations << "}}";
    return o.str();
}

std::string CorrectnessInfo::to_json() const {
    std::ostringstream o;
    o << "{\"final_state_hash\":\"" << final_state_hash << "\",\"reference_state_hash\":\""
      << reference_state_hash << "\",\"hashes_equal\":" << (hashes_equal ? "true" : "false")
      << ",\"invariant_violations\":" << invariant_violations << ",\"duplicate_originals\":"
      << duplicate_originals << ",\"detail\":\"" << detail << "\"}";
    return o.str();
}

RunManifest::RunManifest(std::string run_id, std::string config_hash)
    : run_id_(std::move(run_id)), config_hash_(std::move(config_hash)) {
    build_ = BuildInfo::current();
}

void RunManifest::set_topology(const Topology& t) { topology_json_ = topology_to_json(t); }
void RunManifest::set_plan(const PlacementPlan& p) { plan_json_ = plan_to_json(p); }

void RunManifest::add_span(Span s, const Histogram& h) {
    spans_.emplace_back(s, h.to_json());
}

std::string RunManifest::to_json() const {
    std::ostringstream o;
    const ClockProvenance& cp = MonotonicClock::provenance();
    o << "{\n  \"run_id\": \"" << run_id_ << "\",\n  \"config_hash\": \"" << config_hash_
      << "\",\n  \"build\": " << build_.to_json() << ",\n  \"clock\": {\"source\":\""
      << cp.source << "\",\"monotonic_raw\":" << (cp.monotonic_raw ? "true" : "false")
      << ",\"resolution_ns\":" << cp.resolution_ns << ",\"overhead_ns\":" << cp.overhead_ns
      << ",\"target_platform\":" << (cp.target_platform ? "true" : "false")
      << ",\"note\":\"" << cp.note << "\"}";
    o << ",\n  \"topology\": " << topology_json_;
    o << ",\n  \"placement\": " << plan_json_;
    o << ",\n  \"workload\": " << workload_.to_json();
    o << ",\n  \"arrival\": " << outcomes_.to_json();
    o << ",\n  \"latency\": {";
    bool first = true;
    for (const auto& [span, json] : spans_) {
        if (!first) o << ",";
        first = false;
        o << "\"" << to_string(span) << "\":" << json;
    }
    o << "}";
    o << ",\n  \"queues_and_resources\": " << resources_.to_json();
    o << ",\n  \"correctness\": " << correctness_.to_json();
    o << ",\n  \"validity\": " << validity_.to_json();
    o << ",\n  \"gates\": {\"blocked\": [";
    first = true;
    for (const std::string& b : gates_.blocked) {
        if (!first) o << ",";
        first = false;
        o << "\"" << b << "\"";
    }
    o << "], \"failures\": [";
    first = true;
    for (const std::string& f : gates_.failures) {
        if (!first) o << ",";
        first = false;
        o << "\"" << f << "\"";
    }
    o << "]}";
    o << ",\n  \"notes\": [";
    first = true;
    for (const std::string& n : notes_) {
        if (!first) o << ",";
        first = false;
        o << "\"" << n << "\"";
    }
    o << "]";
    o << ",\n  \"verdict\": \"" << to_string(verdict()) << "\"\n}\n";
    return o.str();
}

bool RunManifest::write(const std::string& path) const {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << to_json();
    return static_cast<bool>(f);
}

std::string make_run_id() {
    const std::uint64_t t = MonotonicClock::now_ns();
    const auto pid = static_cast<std::uint64_t>(LLX_GETPID());
    const BuildInfo b = BuildInfo::current();
    const std::uint64_t h = fnv1a(b.compiler + b.build_type);
    return to_hex(t) + "-" + to_hex(pid) + "-" + to_hex(h).substr(0, 8);
}

}  // namespace llx
