#include "llx/clock.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include "llx/opt_barrier.hpp"

#if defined(__linux__)
#include <time.h>
#define LLX_CLOCK_MONOTONIC_RAW 1
#else
#define LLX_CLOCK_MONOTONIC_RAW 0
#endif

namespace llx {
namespace {

#if LLX_CLOCK_MONOTONIC_RAW
std::uint64_t read_raw() noexcept {
    struct timespec ts;
    // CLOCK_MONOTONIC_RAW is not slewed by NTP, so a measured interval is the
    // interval that elapsed (doc 3).
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}
#else
std::uint64_t read_raw() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
#endif

// Smallest observable non-zero step: read until the value changes, repeat, and
// keep the smallest delta seen. This measures what the process can actually
// observe, which is what matters for a latency histogram, rather than the
// nominal resolution the API advertises.
std::uint64_t measure_resolution_ns() {
    std::uint64_t best = ~std::uint64_t{0};
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint64_t a = read_raw();
        std::uint64_t b = a;
        // Bounded: on a coarse clock this spins for one tick, not forever.
        for (int i = 0; i < 1000000 && b == a; ++i) b = read_raw();
        if (b > a) best = std::min(best, b - a);
    }
    return best == ~std::uint64_t{0} ? 0 : best;
}

// Median cost of one reading. Doc 14 wants the observer effect quantified; the
// median rather than the mean so one scheduler preemption does not dominate.
std::uint64_t measure_overhead_ns() {
    constexpr int kBatch = 256;
    constexpr int kReps = 64;
    std::vector<std::uint64_t> samples;
    samples.reserve(kReps);
    for (int r = 0; r < kReps; ++r) {
        const std::uint64_t t0 = read_raw();
        std::uint64_t sink = 0;
        for (int i = 0; i < kBatch; ++i) sink += read_raw();
        const std::uint64_t t1 = read_raw();
        // Keep the accumulator observable so the loop is not elided; this is
        // the DoNotOptimize concern doc 15.3 lists first among benchmark traps.
        do_not_optimize(sink);
        if (t1 > t0) samples.push_back((t1 - t0) / kBatch);
    }
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

ClockProvenance build_provenance() {
    ClockProvenance p;
#if LLX_CLOCK_MONOTONIC_RAW
    p.source = "CLOCK_MONOTONIC_RAW";
    p.monotonic_raw = true;
    p.target_platform = true;
    p.note = "Linux clock_gettime(CLOCK_MONOTONIC_RAW), the source design doc 14 specifies";
#else
    p.source = "steady_clock";
    p.monotonic_raw = false;
    p.target_platform = false;
    p.note =
        "fallback source: this host is not Linux, so CLOCK_MONOTONIC_RAW is unavailable. "
        "Durations are valid for comparisons made on this host only and must not be "
        "compared with a Linux baseline (design doc 11, LI-28)";
#endif
    p.steady_is_monotonic = std::chrono::steady_clock::is_steady;
    p.resolution_ns = measure_resolution_ns();
    p.overhead_ns = measure_overhead_ns();
    return p;
}

}  // namespace

std::uint64_t MonotonicClock::now_ns() noexcept { return read_raw(); }

const ClockProvenance& MonotonicClock::provenance() {
    // Calibrated once, on first use, off any hot path.
    static const ClockProvenance p = build_provenance();
    return p;
}

const char* MonotonicClock::name() noexcept {
#if LLX_CLOCK_MONOTONIC_RAW
    return "CLOCK_MONOTONIC_RAW";
#else
    return "steady_clock";
#endif
}

}  // namespace llx
