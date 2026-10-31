// Latency histogram with retained raw counts.
//
// Design doc 14: "retain raw counts and bucket bounds, report
// p50/p95/p99/p99.9/max". Doc 15.3 adds that a small sample set does not
// establish a dependable p99.9, and LI-26 forbids publishing a histogram that
// hides how many samples it is missing. So this class:
//
//   * keeps every bucket count and can emit the bucket bounds, so the artifact
//     is reconstructible rather than a summary nobody can audit;
//   * reports a percentile as the upper bound of the bucket it falls in, which
//     is a conservative number with a stated error, instead of interpolating
//     and implying precision the buckets do not have;
//   * refuses to answer a high quantile when there are too few samples to
//     support it (sufficient_for), so a p99.9 from n=1000 is an explicit
//     INCONCLUSIVE rather than a number;
//   * counts rejected samples separately: a duration the clock check refused
//     is not silently absent, it is in invalid_samples.
//
// Bucket layout: values below 64 are exact, and above that each octave is split
// into 64 sub-buckets. The relative error of a reported percentile is therefore
// at most 1/64 (about 1.6%) above 64 ns, and zero below it. 3776 buckets of 8
// bytes is 30 KB per histogram, which is affordable per span.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

namespace llx {

class Histogram {
public:
    // 59 octaves of 64 sub-buckets covers the whole uint64 range.
    static constexpr std::size_t kSubBuckets = 64;
    static constexpr std::size_t kBuckets = 59 * kSubBuckets;

    // Index of the bucket a value falls in. Exact for v < 64; above that the
    // top seven bits of v select the bucket.
    static constexpr std::size_t bucket_index(std::uint64_t v) noexcept {
        if (v < kSubBuckets) return static_cast<std::size_t>(v);
        const int e = 63 - std::countl_zero(v);
        const auto shift = static_cast<std::size_t>(e - 6);
        const auto sub = static_cast<std::size_t>((v >> shift) & 63);
        return (shift + 1) * kSubBuckets + sub;
    }

    // Inclusive bounds of a bucket, so an artifact can be re-binned later.
    static constexpr std::uint64_t bucket_low(std::size_t idx) noexcept {
        if (idx < kSubBuckets) return idx;
        const std::size_t shift = idx / 64 - 1;
        const std::uint64_t sub = idx % 64;
        return (kSubBuckets + sub) << shift;
    }

    static constexpr std::uint64_t bucket_high(std::size_t idx) noexcept {
        if (idx < kSubBuckets) return idx;
        const std::size_t shift = idx / 64 - 1;
        return bucket_low(idx) + ((std::uint64_t{1} << shift) - 1);
    }

    void record(std::uint64_t ns) noexcept {
        ++counts_[bucket_index(ns)];
        ++count_;
        sum_ += ns;
        if (ns > max_) max_ = ns;
        if (count_ == 1 || ns < min_) min_ = ns;
    }

    // A duration the clock check rejected. Counted, never recorded as a
    // latency: doc 3 requires negatives to be investigated, and LI-25 forbids
    // accepting them as samples.
    void record_invalid() noexcept { ++invalid_; }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t invalid_samples() const noexcept { return invalid_; }
    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    std::uint64_t sum() const noexcept { return sum_; }
    double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // Upper bound of the bucket holding the requested quantile. Returns 0 for
    // an empty histogram.
    std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        if (p <= 0.0) return min();
        if (p >= 100.0) return max_;
        // Rank is the smallest index whose cumulative count reaches p% of the
        // total, counted from 1 so p=100 maps to the last sample.
        const auto target = static_cast<std::uint64_t>(
            (static_cast<double>(count_) * p) / 100.0 + 0.5);
        const std::uint64_t want = target == 0 ? 1 : target;
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= want) return bucket_high(i);
        }
        return max_;
    }

    // Whether the sample count can support the quantile at all. The rule of
    // thumb doc 15.3 implies: you need on the order of 10 samples in the tail
    // beyond the quantile for it to mean anything, so p99.9 needs ~10000.
    static constexpr std::uint64_t samples_needed(double p) noexcept {
        const double tail = (100.0 - p) / 100.0;
        if (tail <= 0.0) return ~std::uint64_t{0};
        return static_cast<std::uint64_t>(10.0 / tail + 0.5);
    }
    bool sufficient_for(double p) const noexcept { return count_ >= samples_needed(p); }

    void reset() noexcept {
        counts_.fill(0);
        count_ = 0;
        invalid_ = 0;
        sum_ = 0;
        max_ = 0;
        min_ = 0;
    }

    void merge(const Histogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
        if (other.count_ > 0) {
            if (count_ == 0 || other.min_ < min_) min_ = other.min_;
            if (other.max_ > max_) max_ = other.max_;
        }
        count_ += other.count_;
        invalid_ += other.invalid_;
        sum_ += other.sum_;
    }

    // Non-empty buckets as (low, high, count) triples: the raw artifact doc
    // 27.2 requires under "hist raw artifact".
    struct Bin {
        std::uint64_t low;
        std::uint64_t high;
        std::uint64_t count;
    };
    std::vector<Bin> bins() const {
        std::vector<Bin> out;
        for (std::size_t i = 0; i < kBuckets; ++i)
            if (counts_[i] != 0) out.push_back({bucket_low(i), bucket_high(i), counts_[i]});
        return out;
    }

    // One line of JSON holding everything needed to rebuild the distribution.
    std::string to_json() const;

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t invalid_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = 0;
};

// Outcome accounting. Doc 15.3 calls reporting a p99 without these "denominator
// cheating", and LI-27 forbids using offered load as goodput. Every number the
// benchmark reports is derived from these counters, so offered, admitted and
// completed can never be conflated.
struct OutcomeCounters {
    std::uint64_t offered = 0;            // scheduled by the generator
    std::uint64_t admitted = 0;           // accepted for processing
    std::uint64_t completed = 0;          // durably committed
    std::uint64_t rejected_overload = 0;
    std::uint64_t rejected_invalid = 0;
    std::uint64_t held_unknown = 0;       // COMMIT_UNKNOWN, awaiting reconcile
    std::uint64_t duplicate_ignored = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t stale_epoch = 0;
    std::uint64_t halted = 0;
    std::uint64_t timed_out = 0;          // never reached an outcome

    std::uint64_t accounted() const noexcept {
        return completed + rejected_overload + rejected_invalid + held_unknown +
               duplicate_ignored + out_of_order + stale_epoch + halted + timed_out;
    }
    // Every offered event must end in exactly one bucket. A mismatch means the
    // run lost track of events, which invalidates the measurement (LI-26).
    bool balanced() const noexcept { return offered == accounted(); }

    void add(const OutcomeCounters& o) noexcept {
        offered += o.offered;
        admitted += o.admitted;
        completed += o.completed;
        rejected_overload += o.rejected_overload;
        rejected_invalid += o.rejected_invalid;
        held_unknown += o.held_unknown;
        duplicate_ignored += o.duplicate_ignored;
        out_of_order += o.out_of_order;
        stale_epoch += o.stale_epoch;
        halted += o.halted;
        timed_out += o.timed_out;
    }
    std::string to_json() const;
};

}  // namespace llx
