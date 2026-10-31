// Bounded, low-intrusion latency sampling.
//
// Design doc 14. The shape is forced by three constraints:
//
//   * the owner thread must never block on the exporter (LI-24, doc 12 lane
//     table), so the hot path writes into a bounded per-thread ring and a cold
//     thread drains it;
//   * when that ring is full the sample is dropped and counted, never buffered
//     into an unbounded heap (LI-22), and the count makes the coverage
//     incomplete;
//   * incomplete coverage invalidates percentile claims (LI-26, doc 14
//     "invalidate percentile claims when required coverage incomplete"). So
//     coverage_complete() exists and the release gate reads it. Dropping
//     telemetry is survivable; quoting a p99 computed from an unknown fraction
//     of the samples is not.
//
// This lane is for measurement only. Doc 12 is explicit that telemetry may be
// sampled or dropped while business evidence may not, which is why the sink
// returns a bool the hot path is free to ignore, and the engine never changes a
// business decision because of it.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "llx/clock.hpp"
#include "llx/event.hpp"
#include "llx/histogram.hpp"
#include "llx/spsc_ring.hpp"

namespace llx {

// Doc 18 contract. Virtual so a test can substitute a sink that always refuses
// and prove the engine keeps working.
struct ILatencySink {
    virtual bool try_sample(const SpanSample&) noexcept = 0;
    virtual std::uint64_t lost_samples() const noexcept = 0;
    virtual ~ILatencySink() = default;
};

// Per-thread ring plus the drained histograms. One instance per producing
// thread; the drain runs on the metrics thread (T4).
class TelemetryLane final : public ILatencySink {
public:
    static constexpr std::size_t kCapacity = 8192;  // doc 20 default

    bool try_sample(const SpanSample& s) noexcept override {
        if (!s.valid) {
            // A rejected duration still has to appear somewhere, or the
            // histogram would silently omit it (LI-25).
            invalid_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (ring_.try_push(s)) {
            offered_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        lost_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint64_t lost_samples() const noexcept override {
        return lost_.load(std::memory_order_relaxed);
    }
    std::uint64_t invalid_samples() const noexcept {
        return invalid_.load(std::memory_order_relaxed);
    }
    std::uint64_t offered_samples() const noexcept {
        return offered_.load(std::memory_order_relaxed);
    }

    // Called by the metrics thread. Returns how many samples moved.
    std::size_t drain() noexcept {
        std::size_t n = 0;
        SpanSample s;
        while (ring_.try_pop(s)) {
            const auto idx = static_cast<std::size_t>(s.span);
            if (idx < static_cast<std::size_t>(Span::Count)) {
                hist_[idx].record(s.ns);
                ++n;
            }
        }
        drained_ += n;
        return n;
    }

    const Histogram& span(Span s) const noexcept {
        return hist_[static_cast<std::size_t>(s)];
    }
    Histogram& span_mut(Span s) noexcept { return hist_[static_cast<std::size_t>(s)]; }

    // No sample was lost and no duration was rejected, so a percentile taken
    // from these histograms covers the whole measured population.
    bool coverage_complete() const noexcept {
        return lost_samples() == 0 && invalid_samples() == 0;
    }

    std::size_t pending() const noexcept { return ring_.size(); }
    std::uint64_t drained() const noexcept { return drained_; }

private:
    SpscRing<SpanSample, kCapacity> ring_;
    std::array<Histogram, static_cast<std::size_t>(Span::Count)> hist_{};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> invalid_{0};
    std::atomic<std::uint64_t> offered_{0};
    std::uint64_t drained_ = 0;
};

// A sink that accepts nothing, used to prove the engine's business behaviour is
// identical with telemetry unavailable (LL-007: "metrics exporter hangs the
// moment intake starts -> business state unchanged, telemetry loss counted").
class NullLatencySink final : public ILatencySink {
public:
    bool try_sample(const SpanSample&) noexcept override {
        lost_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::uint64_t lost_samples() const noexcept override {
        return lost_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> lost_{0};
};

// Helper that turns two timestamps into a sample, applying the duration checks
// of doc 3 so a bad reading is marked invalid instead of entering a histogram.
inline SpanSample make_sample(Span span, std::uint64_t start_ns, std::uint64_t stop_ns,
                              const EventId& id, std::uint8_t outcome = 0,
                              std::uint32_t cpu = 0) noexcept {
    SpanSample s;
    s.span = span;
    s.event_lo = id.lo;
    s.outcome = outcome;
    s.cpu = cpu;
    const Duration d = duration_ns(start_ns, stop_ns);
    s.ns = d.ns;
    s.valid = d.ok();
    return s;
}

// Everything the report needs about measurement validity, assembled once at the
// end of a run. Doc 14 and LI-48: a PASS verdict is not available when any of
// these is false.
struct MeasurementValidity {
    bool coverage_complete = false;
    bool clock_on_target_platform = false;
    bool outcomes_balanced = false;
    bool sufficient_p99 = false;
    bool sufficient_p999 = false;
    bool failpoints_compiled_in = false;  // true means this is not a clean perf run
    std::uint64_t lost_samples = 0;
    std::uint64_t invalid_samples = 0;
    std::string clock_source;
    std::string note;

    // The gate of doc 28 Gate 4: a performance claim is only admissible when
    // the measurement itself is sound. Clock platform is reported separately
    // because a run on the fallback clock is valid for same-host comparison but
    // may not be compared with a Linux baseline (LI-28, LI-29).
    bool performance_claim_admissible() const noexcept {
        return coverage_complete && outcomes_balanced && sufficient_p99 &&
               !failpoints_compiled_in;
    }
    std::string to_json() const;
};

}  // namespace llx
