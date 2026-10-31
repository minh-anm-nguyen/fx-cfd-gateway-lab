// Time measurement with explicit provenance.
//
// Design doc 3 and 14 are specific about what is and is not allowed here:
//   * durations come from a monotonic source, never from CLOCK_REALTIME, and
//     two timestamps are never subtracted across unsynchronized hosts (LI-25);
//   * the source name, its resolution and the measured cost of one reading are
//     recorded with every run, because a p99 without clock provenance is not
//     evidence (LI-25, LI-48);
//   * a negative or implausible duration is a reported fault, not a sample
//     that gets quietly dropped (LI-25, doc 3 "negative/outlier phải được
//     điều tra");
//   * the reading is NOT assumed to be cycle-based. TSC/RDTSCP is faster but
//     needs invariant-TSC checks, serialization and migration handling, so this
//     project uses the OS monotonic clock and says so (doc 3, ADR-05).
//
// Platform note. The design targets Linux, where the source is
// clock_gettime(CLOCK_MONOTONIC_RAW): raw is chosen so NTP slewing cannot
// stretch a measured interval. On a non-Linux host the source falls back to
// std::chrono::steady_clock and `name()` says so, which is the documented
// fallback doc 8/11 demands rather than a silent substitution. Any run on the
// fallback is tagged so a report cannot be compared against a Linux baseline.
#pragma once

#include <cstdint>
#include <string>

namespace llx {

// Where a timestamp came from, carried into the run manifest.
struct ClockProvenance {
    std::string source;            // "CLOCK_MONOTONIC_RAW", "steady_clock", "manual"
    bool monotonic_raw = false;    // true only for the Linux raw source
    std::uint64_t resolution_ns = 0;  // smallest observable non-zero step
    std::uint64_t overhead_ns = 0;    // measured median cost of one reading
    bool steady_is_monotonic = true;  // std::chrono::steady_clock::is_steady
    // Set when the platform source is not the one the design doc specifies, so
    // the benchmark report cannot claim a Linux-equivalent measurement.
    bool target_platform = false;
    std::string note;
};

// The process-local monotonic source. Stateless and thread safe.
class MonotonicClock {
public:
    static std::uint64_t now_ns() noexcept;

    // Measured once per process: resolution by sampling for the first change,
    // overhead by the median of a batch of back-to-back readings. Doc 14 wants
    // both in the manifest so an instrumented run can be told apart from an
    // uninstrumented one.
    static const ClockProvenance& provenance();

    static const char* name() noexcept;
};

// Result of subtracting two timestamps. Doc 3 forbids a negative duration from
// entering a histogram, and doc 14 wants overflow-checked subtraction, so the
// outcome is explicit rather than a wrapped unsigned value.
enum class DurationStatus : std::uint8_t {
    Ok,
    Negative,      // stop < start: clock went backwards or stamps were swapped
    Implausible,   // exceeds the configured sanity ceiling
};

struct Duration {
    std::uint64_t ns = 0;
    DurationStatus status = DurationStatus::Ok;
    bool ok() const noexcept { return status == DurationStatus::Ok; }
};

// One hour. A single span in this pipeline measured in hours means the clock or
// the stamping is wrong, and doc 3 wants that investigated, not averaged in.
constexpr std::uint64_t kImplausibleNs = 3600ULL * 1000000000ULL;

constexpr Duration duration_ns(std::uint64_t start, std::uint64_t stop,
                               std::uint64_t ceiling = kImplausibleNs) noexcept {
    if (stop < start) return Duration{0, DurationStatus::Negative};
    const std::uint64_t d = stop - start;
    if (d > ceiling) return Duration{d, DurationStatus::Implausible};
    return Duration{d, DurationStatus::Ok};
}

// Deterministic clock for tests: time only moves when a test moves it, so a
// timeout or a watermark dwell can be exercised without sleeping. Doc 23 layer
// 2 needs this to keep model-based runs reproducible.
class ManualClock {
public:
    std::uint64_t now_ns() const noexcept { return now_; }
    void advance_ns(std::uint64_t d) noexcept { now_ += d; }
    void set_ns(std::uint64_t t) noexcept { now_ = t; }

    // Deliberately available so a test can drive the clock backwards and prove
    // the engine rejects the resulting sample instead of recording it.
    void rewind_ns(std::uint64_t d) noexcept { now_ = (d > now_) ? 0 : now_ - d; }

private:
    std::uint64_t now_ = 1000000000ULL;  // start away from zero
};

}  // namespace llx
