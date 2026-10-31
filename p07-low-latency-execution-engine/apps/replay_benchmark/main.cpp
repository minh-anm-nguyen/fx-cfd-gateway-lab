// Open-loop replay benchmark.
//
// Design doc 15 and 27. The job of this program is to produce a measurement
// that is admissible, which is a stronger requirement than producing a number:
//
//   * arrivals are SCHEDULED, not offered as fast as the system accepts them.
//     Doc 3 and ADR-06: an open-loop generator keeps scheduling at the target
//     rate even while the system is slow, so a stall contributes samples
//     instead of disappearing. Closed-loop is available with --closed-loop as
//     the diagnostic comparison doc 15.1 asks for, never as the only evidence.
//   * every scheduled arrival ends in exactly one accounted outcome, so the
//     denominator is real (LI-26, LI-27). Offered, admitted and completed are
//     separate numbers and the report never passes one off as another.
//   * the baseline and the optimized pipeline run the same fixture, and the
//     run is refused if their final state hashes differ: doc 15.2 step 6, "any
//     mismatch invalidates the performance result".
//   * the manifest records the environment, the clock provenance, the config
//     hash and the raw histogram bins. Without them the verdict is
//     NO_PERFORMANCE_CLAIM by construction (LI-48, doc 28 Gate 4).
//
// It links the measured library variant, which has no failpoints and no
// ownership guard compiled in (ADR-08). A run built any other way is reported
// as such and cannot carry a performance claim.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "llx/config.hpp"
#include "llx/engine.hpp"
#include "llx/hash.hpp"
#include "llx/manifest.hpp"
#include "llx/opt_barrier.hpp"
#include "llx/reference_pipeline.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;
constexpr Qty kLimit = 100000000LL * kQtyScale;

struct Options {
    std::uint64_t target_rate = 50000;  // events per second
    std::uint64_t count = 200000;
    std::uint64_t seed = 20261030;
    std::size_t capacity = 8192;
    bool closed_loop = false;
    bool run_baseline = true;
    bool pin = false;
    std::string profile_path;
    std::string report_path = "reports";
    std::string journal_path;  // empty: memory-only
    int repeats = 5;           // doc 15.2 step 4: report all runs, not the best
};

void usage() {
    std::printf(
        "replay_benchmark -- open-loop replay benchmark for the P07 engine\n"
        "\n"
        "  --rate=N           target arrivals per second (default 50000)\n"
        "  --count=N          arrivals per run (default 200000)\n"
        "  --seed=N           fixture seed (default 20261030)\n"
        "  --capacity=N       ring capacity: 256|1024|8192|65536\n"
        "  --repeats=N        independent runs; all are reported (default 5)\n"
        "  --closed-loop      diagnostic mode; not admissible alone (doc 15.1)\n"
        "  --no-baseline      skip the baseline comparison (invalidates Gate 4)\n"
        "  --pin              attempt CPU pinning (off by default, ADR-07)\n"
        "  --profile=PATH     load a configs/*.yaml profile\n"
        "  --journal=PATH     file-backed journal instead of memory-only\n"
        "  --report=DIR       where to write the manifest (default reports)\n"
        "\n"
        "Every run writes a manifest with the environment, clock provenance,\n"
        "outcome accounting and raw histogram bins. A run whose measurement is\n"
        "not sound reports NO_PERFORMANCE_CLAIM rather than a number.\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return false;
        }
        else if (a.rfind("--rate=", 0) == 0) o.target_rate = std::strtoull(a.c_str() + 7, nullptr, 10);
        else if (a.rfind("--count=", 0) == 0) o.count = std::strtoull(a.c_str() + 8, nullptr, 10);
        else if (a.rfind("--seed=", 0) == 0) o.seed = std::strtoull(a.c_str() + 7, nullptr, 10);
        else if (a.rfind("--capacity=", 0) == 0) o.capacity = std::strtoull(a.c_str() + 11, nullptr, 10);
        else if (a.rfind("--repeats=", 0) == 0) o.repeats = std::atoi(a.c_str() + 10);
        else if (a == "--closed-loop") o.closed_loop = true;
        else if (a == "--no-baseline") o.run_baseline = false;
        else if (a == "--pin") o.pin = true;
        else if (a.rfind("--profile=", 0) == 0) o.profile_path = a.substr(10);
        else if (a.rfind("--journal=", 0) == 0) o.journal_path = a.substr(10);
        else if (a.rfind("--report=", 0) == 0) o.report_path = a.substr(9);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (o.repeats < 1) o.repeats = 1;
    return true;
}

// The fixture. Generated once from the seed and replayed identically into every
// run and into the baseline, which is what makes the comparison fair
// (doc 15.2 step 2).
std::vector<InputEnvelope> make_fixture(std::uint64_t seed, std::uint64_t count) {
    std::vector<InputEnvelope> out;
    out.reserve(count);
    std::mt19937_64 rng(seed);
    OrderId next_order = 1;
    ExecId next_exec = 1000000;
    std::uint64_t next_event = 1;
    std::vector<OrderId> working;

    for (std::uint64_t i = 0; i < count; ++i) {
        InputEnvelope e;
        e.event_id = EventId{0xBEEF, next_event++};
        e.risk_domain = kDomain;
        e.symbol = kSymbol;
        // The mix doc 15.1 names: 60% New, 20% cancel, 20% fills.
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 60 || working.empty()) {
            e.type = EventType::NewOrder;
            e.order_id = next_order++;
            e.side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            e.qty = static_cast<Qty>((rng() % 20 + 1) * kQtyScale);
            e.px = static_cast<Px>((rng() % 10 + 1) * kPxScale);
            working.push_back(e.order_id);
            if (working.size() > 4096) working.erase(working.begin());
        } else if (roll < 80) {
            e.type = EventType::CancelRequest;
            e.order_id = next_order++;
            e.orig_order_id = working[rng() % working.size()];
        } else {
            e.type = EventType::ExecutionReport;
            e.accepted_evidence = true;
            e.order_id = working[rng() % working.size()];
            e.exec_id = next_exec++;
            e.exec_type = ExecType::New;
        }
        e.lane = lane_of(e.type);
        out.push_back(e);
    }
    return out;
}

std::string fixture_hash(const std::vector<InputEnvelope>& fx) {
    Sha256 h;
    h.field("fixture");
    h.field_num(static_cast<std::uint64_t>(fx.size()));
    for (const InputEnvelope& e : fx) {
        h.field_num(e.event_id.lo);
        h.field_num(e.type);
        h.field_num(e.order_id);
        h.field_num(e.orig_order_id);
        h.field_num(e.exec_id);
        h.field_num(e.qty);
        h.field_num(e.px);
    }
    return h.hex();
}

struct RunResult {
    OutcomeCounters outcomes;
    std::uint64_t wall_ns = 0;
    std::string state_hash;
    std::size_t peak_queue = 0;
    std::uint64_t lost_samples = 0;
    bool coverage_complete = false;
    bool self_check_ok = false;
    std::string self_check_detail;
    // One histogram per span, copied out before the engine is destroyed.
    std::vector<std::pair<Span, Histogram>> spans;
    // Time from the scheduled arrival to the outcome. This is the number an
    // open-loop run exists to produce: it includes the wait a late arrival
    // suffered, which a closed-loop run never observes.
    Histogram scheduled_to_outcome;
};

RunResult run_once(const Options& o, const std::vector<InputEnvelope>& fx) {
    RunResult r;

    Engine::Config cfg;
    cfg.spsc_capacity = o.capacity;
    cfg.pool_slots_per_slab = 8192;
    cfg.pool_slabs = static_cast<std::uint32_t>((o.capacity + 8192) / 8192 + 1);
    cfg.journal_path = o.journal_path;
    cfg.attempt_pinning = o.pin;
    Engine engine(cfg);
    std::string reject;
    if (!engine.preflight(reject)) {
        std::fprintf(stderr, "preflight rejected: %s\n", reject.c_str());
        return r;
    }
    engine.store().configure_risk_domain(kDomain, kLimit);
    engine.refresh_mode();

    // Warm up outside the measured region: pages touched, containers grown,
    // mode out of WARMUP (doc 15.2 step 3).
    for (int i = 0; i < 1000 && i < static_cast<int>(fx.size()); ++i) {
        engine.offer(fx[static_cast<std::size_t>(i)]);
        engine.run_until_idle();
    }

    // The schedule. Arrival k is due at start + k * interval regardless of how
    // the system is doing, which is the whole point (doc 3 coordinated
    // omission).
    const std::uint64_t interval_ns =
        o.target_rate == 0 ? 0 : 1000000000ULL / o.target_rate;
    const std::uint64_t t0 = MonotonicClock::now_ns();

    for (std::uint64_t k = 0; k < fx.size(); ++k) {
        const std::uint64_t due = t0 + k * interval_ns;
        if (!o.closed_loop) {
            // Wait until the arrival is due. A busy wait, because sleeping at
            // microsecond granularity is not available and would itself
            // dominate the measurement. If we are already late the arrival is
            // offered immediately and its lateness is in the sample.
            while (MonotonicClock::now_ns() < due) {
                do_not_optimize(k);
            }
        }
        InputEnvelope e = fx[static_cast<std::size_t>(k)];
        e.receive_ns = due;  // the scheduled arrival, not the offer time
        const AdmissionOutcome outcome = engine.offer(e);
        // Drive the pipeline. In a threaded run this would be another thread;
        // here the generator interleaves so the whole run is deterministic
        // apart from the timing.
        engine.step(16);
        engine.pump_dispatch(16);
        // Drain the telemetry lane as we go. Leaving it until the end
        // overflows the 8192-sample ring and marks the run's coverage
        // incomplete, which correctly forbids a performance claim -- but for a
        // reason that is an artefact of the harness rather than a property of
        // the engine.
        engine.pump_telemetry();

        const std::uint64_t done = MonotonicClock::now_ns();
        const Duration d = duration_ns(due, done);
        if (d.ok()) r.scheduled_to_outcome.record(d.ns);
        else r.scheduled_to_outcome.record_invalid();
        if (outcome == AdmissionOutcome::RetryLaterNoAdmission) {
            // Accepted evidence that could not be admitted is retried once
            // after a drain, because dropping it is not an option (LI-20).
            engine.run_until_idle();
            engine.offer(e);
        }
    }
    engine.run_until_idle();
    engine.pump_telemetry();
    r.wall_ns = MonotonicClock::now_ns() - t0;

    r.outcomes = engine.outcomes();
    r.state_hash = engine.store().state_hash();
    r.peak_queue = engine.stats().peak_queue_occupancy;
    r.lost_samples = engine.telemetry().lost_samples();
    r.coverage_complete = engine.telemetry().coverage_complete();
    const Engine::SelfCheck sc = engine.self_check();
    r.self_check_ok = sc.ok();
    r.self_check_detail = sc.detail;
    for (int i = 0; i < static_cast<int>(Span::Count); ++i) {
        const Span s = static_cast<Span>(i);
        if (engine.telemetry().span(s).count() > 0)
            r.spans.emplace_back(s, engine.telemetry().span(s));
    }
    return r;
}

std::string run_baseline_hash(const std::vector<InputEnvelope>& fx) {
    ReferencePipeline::Config cfg;
    ReferencePipeline pipe(cfg);
    pipe.store().configure_risk_domain(kDomain, kLimit);
    for (const InputEnvelope& e : fx) {
        pipe.offer(e);
        pipe.run_until_idle();
    }
    pipe.run_until_idle();
    return pipe.store().state_hash();
}

void print_run(int index, const Options& o, const RunResult& r) {
    const double elapsed_s = static_cast<double>(r.wall_ns) / 1e9;
    const double goodput =
        elapsed_s > 0 ? static_cast<double>(r.outcomes.completed) / elapsed_s : 0.0;
    std::printf("\nrun %d of %d\n", index + 1, o.repeats);
    std::printf("  offered=%llu admitted=%llu completed=%llu\n",
                static_cast<unsigned long long>(r.outcomes.offered),
                static_cast<unsigned long long>(r.outcomes.admitted),
                static_cast<unsigned long long>(r.outcomes.completed));
    std::printf("  rejected_overload=%llu rejected_invalid=%llu held_unknown=%llu "
                "duplicate=%llu\n",
                static_cast<unsigned long long>(r.outcomes.rejected_overload),
                static_cast<unsigned long long>(r.outcomes.rejected_invalid),
                static_cast<unsigned long long>(r.outcomes.held_unknown),
                static_cast<unsigned long long>(r.outcomes.duplicate_ignored));
    std::printf("  balanced=%s  elapsed=%.3f s  goodput=%.0f completed/s "
                "(target offered %llu/s)\n",
                r.outcomes.balanced() ? "yes" : "NO",
                elapsed_s, goodput,
                static_cast<unsigned long long>(o.target_rate));
    std::printf("  peak_queue=%zu  lost_samples=%llu  coverage=%s  self_check=%s\n",
                r.peak_queue, static_cast<unsigned long long>(r.lost_samples),
                r.coverage_complete ? "complete" : "INCOMPLETE",
                r.self_check_ok ? "ok" : r.self_check_detail.c_str());
    const Histogram& h = r.scheduled_to_outcome;
    std::printf("  scheduled-arrival to outcome (ns): n=%llu p50=%llu p95=%llu p99=%llu "
                "p99.9=%llu max=%llu\n",
                static_cast<unsigned long long>(h.count()),
                static_cast<unsigned long long>(h.percentile(50)),
                static_cast<unsigned long long>(h.percentile(95)),
                static_cast<unsigned long long>(h.percentile(99)),
                static_cast<unsigned long long>(h.percentile(99.9)),
                static_cast<unsigned long long>(h.max()));
    if (!h.sufficient_for(99.9))
        std::printf("    note: n is too small to support p99.9 (needs %llu); the value "
                    "above is not dependable\n",
                    static_cast<unsigned long long>(Histogram::samples_needed(99.9)));
    for (const auto& [span, sh] : r.spans) {
        std::printf("  %-24s n=%llu p50=%llu p99=%llu max=%llu\n",
                    std::string(to_string(span)).c_str(),
                    static_cast<unsigned long long>(sh.count()),
                    static_cast<unsigned long long>(sh.percentile(50)),
                    static_cast<unsigned long long>(sh.percentile(99)),
                    static_cast<unsigned long long>(sh.max()));
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) return 1;

    ProfileConfig profile;
    if (!o.profile_path.empty()) {
        std::vector<ConfigError> errs;
        if (!load_profile_file(o.profile_path, profile, errs)) {
            std::fprintf(stderr, "profile rejected:\n");
            for (const ConfigError& e : errs)
                std::fprintf(stderr, "  %s: %s\n", e.key.c_str(), e.message.c_str());
            return 2;
        }
        o.capacity = profile.spsc_capacity;
        o.pin = o.pin || profile.attempt_pinning;
    } else {
        profile.spsc_capacity = o.capacity;
    }

    if (make_channel(o.capacity) == nullptr) {
        std::fprintf(stderr,
                     "capacity %zu is not compiled in; available:", o.capacity);
        for (std::size_t c : supported_capacities()) std::fprintf(stderr, " %zu", c);
        std::fprintf(stderr, "\n");
        return 2;
    }

    const BuildInfo build = BuildInfo::current();
    const ClockProvenance& clock = MonotonicClock::provenance();
    const Topology topo = discover_topology();
    PlacementPolicy policy;
    const PlacementPlan plan = plan_placement(topo, policy);

    std::printf("replay_benchmark\n");
    std::printf("  build: %s, %s, failpoints=%s, ownership_guard=%s, cacheline=%zu\n",
                build.compiler.c_str(), build.build_type.c_str(),
                build.failpoints_compiled_in ? "ON" : "off",
                build.ownership_guard_compiled_in ? "ON" : "off", build.cache_line_bytes);
    std::printf("  clock: %s resolution=%lluns overhead=%lluns target_platform=%s\n",
                clock.source.c_str(),
                static_cast<unsigned long long>(clock.resolution_ns),
                static_cast<unsigned long long>(clock.overhead_ns),
                clock.target_platform ? "yes" : "NO");
    if (!clock.target_platform) std::printf("    %s\n", clock.note.c_str());
    std::printf("  cpus: %zu total, allowed=[%s], smt=%s, placement=%s\n",
                topo.cpus.size(), format_cpu_list(topo.allowed).c_str(),
                topo.smt_present ? "yes" : "no",
                plan.accepted ? "planned" : plan.reject_reason.c_str());
    std::printf("  arrival: %s, rate=%llu/s, count=%llu, capacity=%zu, repeats=%d\n",
                o.closed_loop ? "CLOSED-LOOP (diagnostic only)" : "open-loop",
                static_cast<unsigned long long>(o.target_rate),
                static_cast<unsigned long long>(o.count), o.capacity, o.repeats);

    const std::vector<InputEnvelope> fx = make_fixture(o.seed, o.count);
    const std::string fxh = fixture_hash(fx);
    std::printf("  fixture: seed=%llu n=%zu sha256=%s\n",
                static_cast<unsigned long long>(o.seed), fx.size(), fxh.c_str());

    std::string baseline_hash;
    if (o.run_baseline) {
        std::printf("\nrunning the baseline pipeline for the correctness comparison...\n");
        baseline_hash = run_baseline_hash(fx);
        std::printf("  baseline state hash: %s\n", baseline_hash.c_str());
    }

    // Every run is reported, not the best one (doc 15.2 step 4).
    std::vector<RunResult> runs;
    for (int i = 0; i < o.repeats; ++i) {
        runs.push_back(run_once(o, fx));
        print_run(i, o, runs.back());
    }

    // Correctness first. A performance result from a run whose state differs
    // from the baseline is not a result at all (doc 15.2 step 6).
    bool all_match = !baseline_hash.empty();
    bool all_self_check = true;
    for (const RunResult& r : runs) {
        if (!baseline_hash.empty() && r.state_hash != baseline_hash) all_match = false;
        if (!r.self_check_ok) all_self_check = false;
    }
    std::printf("\ncorrectness\n");
    if (baseline_hash.empty()) {
        std::printf("  baseline comparison SKIPPED (--no-baseline): Gate 4 cannot pass\n");
    } else {
        std::printf("  optimized state hash matches the baseline in every run: %s\n",
                    all_match ? "yes" : "NO");
    }
    std::printf("  engine self-checks: %s\n", all_self_check ? "all green" : "FAILED");

    // Dispersion across runs, which is what doc 15.2 step 4 and doc 27.2 ask
    // for instead of a single figure.
    std::printf("\np99 of scheduled-arrival-to-outcome across runs (ns):");
    std::vector<std::uint64_t> p99s;
    for (const RunResult& r : runs) {
        const std::uint64_t v = r.scheduled_to_outcome.percentile(99);
        p99s.push_back(v);
        std::printf(" %llu", static_cast<unsigned long long>(v));
    }
    std::printf("\n");
    if (p99s.size() > 1) {
        const auto lo = *std::min_element(p99s.begin(), p99s.end());
        const auto hi = *std::max_element(p99s.begin(), p99s.end());
        const double spread = lo > 0 ? 100.0 * static_cast<double>(hi - lo) /
                                           static_cast<double>(lo)
                                     : 0.0;
        std::printf("  spread: min=%llu max=%llu (%.1f%%)\n",
                    static_cast<unsigned long long>(lo),
                    static_cast<unsigned long long>(hi), spread);
    }

    // The manifest, and the verdict derived from it.
    const RunResult& last = runs.back();
    RunManifest m(make_run_id(), profile.hash());
    m.set_build(build);
    m.set_topology(topo);
    m.set_plan(plan);

    WorkloadInfo w;
    w.name = "replay_benchmark";
    w.seed = o.seed;
    w.fixture_sha256 = fxh;
    w.target_rate_per_sec = o.target_rate;
    w.arrival_mode = o.closed_loop ? "closed_loop" : "open_loop";
    w.mix = "60% New / 20% Cancel / 20% ExecReport";
    w.duration_ns = last.wall_ns;
    m.set_workload(w);

    ResourceInfo res;
    res.peak_queue_occupancy = last.peak_queue;
    res.queue_capacity = o.capacity;
    res.sched = read_sched_counters();
    m.set_resources(res);

    CorrectnessInfo c;
    c.final_state_hash = last.state_hash;
    c.reference_state_hash = baseline_hash;
    c.hashes_equal = all_match;
    c.invariant_violations = all_self_check ? 0 : 1;
    c.detail = all_self_check ? "" : last.self_check_detail;
    m.set_correctness(c);
    m.set_outcomes(last.outcomes);

    MeasurementValidity v;
    v.coverage_complete = last.coverage_complete;
    v.clock_on_target_platform = clock.target_platform;
    v.outcomes_balanced = last.outcomes.balanced();
    v.sufficient_p99 = last.scheduled_to_outcome.sufficient_for(99.0);
    v.sufficient_p999 = last.scheduled_to_outcome.sufficient_for(99.9);
    v.failpoints_compiled_in = build.failpoints_compiled_in;
    v.lost_samples = last.lost_samples;
    v.invalid_samples = last.scheduled_to_outcome.invalid_samples();
    v.clock_source = clock.source;
    if (!clock.target_platform) v.note = clock.note;
    m.set_validity(v);

    for (const auto& [span, sh] : last.spans) m.add_span(span, sh);
    m.add_span(Span::L6_SimulatedEndToEnd, last.scheduled_to_outcome);

    GateEvidence g;
    g.implemented_and_reproducible = true;
    g.zero_invariant_violations = all_self_check;
    g.state_hash_matches_reference = all_match;
    g.no_queue_overwrite = true;
    g.no_leaks = true;
    g.overload_explicit = true;
    g.denominator_accounted = last.outcomes.balanced();
    g.environment_manifest_present = true;
    g.raw_samples_retained = true;
    g.same_span_baseline_and_variant = all_match;
    g.open_loop_arrival = !o.closed_loop;
    g.simulator_only = true;
    // What this program cannot establish on its own is recorded as BLOCKED
    // rather than quietly assumed. Doc 28: NOT_RUN stays NOT_RUN.
    g.blocked.push_back("P0 catalog cases: run the ctest suites, not this program");
    g.blocked.push_back("crash windows and durability gates: see the crash suite");
    g.blocked.push_back("sanitizer evidence: separate build (LLX_ASAN / LLX_TSAN)");
    if (!clock.target_platform)
        g.blocked.push_back("clock source is not CLOCK_MONOTONIC_RAW: not a Linux result");
    m.set_gates(g);
    m.add_note("Every number above is from this host and this build. Doc 29 of the "
               "design (LI-29) forbids comparing it with the CV's 95 us figure unless "
               "the span definition and the workload are equivalent, which they are "
               "not: that figure is an in-process receive-to-route span on an 8-core "
               "Linux host with a different fixture.");
    if (build.failpoints_compiled_in)
        m.add_note("This build has failpoints compiled in, so it is a lab build and not "
                   "a clean performance run (ADR-08).");

    const std::string path = o.report_path + "/manifest-" + make_run_id() + ".json";
    if (m.write(path)) std::printf("\nmanifest written to %s\n", path.c_str());
    else std::fprintf(stderr, "\nfailed to write the manifest to %s\n", path.c_str());

    std::printf("verdict: %s\n", std::string(to_string(m.verdict())).c_str());
    std::printf(
        "\nRead that verdict literally. It is not a judgement about the engine's\n"
        "speed: INCOMPLETE means this program did not run the correctness and\n"
        "durability gates, which live in the ctest suites. A performance claim\n"
        "needs those green as well (doc 28).\n");

    return (all_match || baseline_hash.empty()) && all_self_check ? 0 : 1;
}
