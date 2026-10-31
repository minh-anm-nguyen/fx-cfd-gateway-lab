// Catalog groups 12 and 13 (LL-221..260): metrics integrity, percentiles,
// coordinated omission and microbenchmark validity.
//
// Oracle code P. These are the cases that stop the project reporting a number
// it has not earned, which doc 15.3 lists as six specific traps.

#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include "llx/histogram.hpp"
#include "llx/telemetry.hpp"
#include "tiny_test.hpp"

using namespace llx;

SCENARIO("LL-221", "P0", "P", "small values are binned exactly") {
    Histogram h;
    for (std::uint64_t v = 0; v < 64; ++v) {
        CHECK_EQ(Histogram::bucket_index(v), static_cast<std::size_t>(v));
        CHECK_EQ(Histogram::bucket_low(static_cast<std::size_t>(v)), v);
        CHECK_EQ(Histogram::bucket_high(static_cast<std::size_t>(v)), v);
    }
    for (std::uint64_t v = 0; v < 64; ++v) h.record(v);
    CHECK_EQ(h.count(), std::uint64_t{64});
    CHECK_EQ(h.min(), std::uint64_t{0});
    CHECK_EQ(h.max(), std::uint64_t{63});
    // Exact bins mean an exact median at this scale. For the 64 values 0..63
    // the 32nd-ranked sample is 31, which is what the rank walk returns; the
    // true median 31.5 is not representable and the reported value is the
    // enclosing bucket's upper bound, as documented.
    CHECK_EQ(h.percentile(50.0), std::uint64_t{31});
}

SCENARIO("LL-222", "P0", "P",
         "every bucket contains its own bounds and the relative error is bounded") {
    // The reported percentile is the bucket's upper bound, so the error has to
    // be known and stated rather than assumed negligible.
    for (std::uint64_t v : {std::uint64_t{64}, std::uint64_t{65}, std::uint64_t{127},
                            std::uint64_t{128}, std::uint64_t{1000},
                            std::uint64_t{1000000}, std::uint64_t{1} << 40,
                            ~std::uint64_t{0} >> 1}) {
        const std::size_t idx = Histogram::bucket_index(v);
        CHECK_LT(idx, Histogram::kBuckets);
        const std::uint64_t lo = Histogram::bucket_low(idx);
        const std::uint64_t hi = Histogram::bucket_high(idx);
        CHECK_LE(lo, v);
        CHECK_LE(v, hi);
        // Width over the lower bound is the relative error, at most 1/64.
        CHECK_LE(hi - lo + 1, (lo / 64) + 1);
    }
}

SCENARIO("LL-223", "P0", "P", "percentiles over a known distribution land in range") {
    Histogram h;
    // 1..10000 ns, uniform. The true p99 is 9900 and p50 is 5000; the reported
    // values are the enclosing bucket bounds, which must bracket them within
    // the stated error.
    for (std::uint64_t v = 1; v <= 10000; ++v) h.record(v);
    const std::uint64_t p50 = h.percentile(50.0);
    const std::uint64_t p99 = h.percentile(99.0);
    CHECK(p50 >= 5000 && p50 <= 5000 + 5000 / 64 + 2);
    CHECK(p99 >= 9900 && p99 <= 9900 + 9900 / 64 + 2);
    CHECK_EQ(h.max(), std::uint64_t{10000});
    CHECK_EQ(h.percentile(100.0), std::uint64_t{10000});
}

SCENARIO("LL-224", "P0", "P",
         "a quantile the sample count cannot support is refused, not estimated") {
    // Doc 15.3: "p99.9 requires a large sample set; small samples (n=1000) do
    // not establish a dependable p99.9".
    Histogram h;
    for (int i = 0; i < 1000; ++i) h.record(100);
    CHECK(h.sufficient_for(99.0));        // 1000 >= 1000
    CHECK(!h.sufficient_for(99.9));       // needs ~10000
    for (int i = 0; i < 9000; ++i) h.record(100);
    CHECK(h.sufficient_for(99.9));
    CHECK(!h.sufficient_for(99.999));
}

SCENARIO("LL-225", "P0", "P", "a rejected sample is counted, never silently absent") {
    // LI-25 and LI-26 together: a duration the clock check refused must still
    // appear in the accounting, or the histogram's denominator is a fiction.
    Histogram h;
    h.record(500);
    h.record_invalid();
    h.record_invalid();
    CHECK_EQ(h.count(), std::uint64_t{1});
    CHECK_EQ(h.invalid_samples(), std::uint64_t{2});
    const std::string json = h.to_json();
    CHECK(json.find("\"invalid\":2") != std::string::npos);
}

SCENARIO("LL-226", "P0", "P", "the json artifact carries the raw bins, not just a summary") {
    // Doc 14 and 27.2: "retain raw counts and bucket bounds". A summary alone
    // cannot be re-binned or audited.
    Histogram h;
    h.record(10);
    h.record(10);
    h.record(1000);
    const std::string json = h.to_json();
    CHECK(json.find("\"bins\":[") != std::string::npos);
    CHECK(json.find("[10,10,2]") != std::string::npos);
    CHECK(json.find("\"sufficient_p999\":false") != std::string::npos);
    CHECK(json.find("\"bucket_relative_error\":") != std::string::npos);

    const auto bins = h.bins();
    CHECK_EQ(bins.size(), std::size_t{2});
    std::uint64_t total = 0;
    for (const auto& b : bins) total += b.count;
    CHECK_EQ(total, h.count());
}

SCENARIO("LL-227", "P0", "P", "merging histograms preserves every count") {
    Histogram a, b;
    for (int i = 0; i < 100; ++i) a.record(static_cast<std::uint64_t>(i + 1));
    for (int i = 0; i < 50; ++i) b.record(static_cast<std::uint64_t>(i + 1000));
    a.record_invalid();
    b.record_invalid();
    const std::uint64_t expect = a.count() + b.count();
    a.merge(b);
    CHECK_EQ(a.count(), expect);
    CHECK_EQ(a.invalid_samples(), std::uint64_t{2});
    CHECK_EQ(a.max(), std::uint64_t{1049});
    CHECK_EQ(a.min(), std::uint64_t{1});
}

SCENARIO("LL-241", "P0", "P",
         "offered, admitted and completed are separate and must balance") {
    // Doc 15.3's "denominator cheating" made structural: an event that reached
    // no bucket means the run lost track of it, which invalidates the
    // measurement rather than improving it.
    OutcomeCounters c;
    c.offered = 100;
    c.admitted = 90;
    c.completed = 70;
    c.rejected_overload = 10;
    c.held_unknown = 5;
    c.duplicate_ignored = 5;
    c.rejected_invalid = 10;
    CHECK_EQ(c.accounted(), std::uint64_t{100});
    CHECK(c.balanced());

    c.timed_out = 1;  // now 101 accounted for 100 offered
    CHECK(!c.balanced());
    CHECK(c.to_json().find("\"balanced\":false") != std::string::npos);
}

SCENARIO("LL-242", "P0", "P", "goodput is completed over elapsed, never offered load") {
    // LI-27. The arithmetic is trivial; the point is that the two numbers are
    // never the same field, so a report cannot quote one as the other.
    OutcomeCounters c;
    c.offered = 50000;
    c.admitted = 20000;
    c.completed = 18000;
    c.rejected_overload = 30000;
    c.rejected_invalid = 2000;
    CHECK(c.balanced());
    // Offered was 50k but only 18k outcomes were achieved. A report that called
    // this "50k/s throughput" would be wrong by a factor of nearly three.
    CHECK_NE(c.offered, c.completed);
    CHECK_LT(c.completed, c.admitted);
}

SCENARIO("LL-243", "P0", "P",
         "a telemetry lane that fills drops samples and marks coverage incomplete") {
    // LI-26 and doc 14: dropping telemetry is survivable; quoting a percentile
    // computed from an unknown fraction of samples is not.
    TelemetryLane lane;
    const std::size_t cap = TelemetryLane::kCapacity;
    for (std::size_t i = 0; i < cap; ++i) {
        SpanSample s;
        s.span = Span::L2_DecisionCompute;
        s.ns = 100;
        CHECK(lane.try_sample(s));
    }
    CHECK(lane.coverage_complete());

    // One past capacity: refused and counted.
    SpanSample overflow;
    overflow.span = Span::L2_DecisionCompute;
    overflow.ns = 100;
    CHECK(!lane.try_sample(overflow));
    CHECK_EQ(lane.lost_samples(), std::uint64_t{1});
    CHECK(!lane.coverage_complete());

    // Draining does not undo the loss: the run's coverage is permanently
    // incomplete and the gate must see that.
    CHECK_EQ(lane.drain(), cap);
    CHECK(!lane.coverage_complete());
    CHECK_EQ(lane.span(Span::L2_DecisionCompute).count(), cap);
}

SCENARIO("LL-244", "P0", "P",
         "an invalid duration never enters a histogram through the lane") {
    TelemetryLane lane;
    SpanSample bad;
    bad.span = Span::L1_QueueWait;
    bad.ns = 999999;
    bad.valid = false;
    CHECK(!lane.try_sample(bad));
    CHECK_EQ(lane.invalid_samples(), std::uint64_t{1});
    CHECK(!lane.coverage_complete());
    lane.drain();
    CHECK_EQ(lane.span(Span::L1_QueueWait).count(), std::uint64_t{0});
}

SCENARIO("LL-245", "P0", "P", "make_sample applies the clock checks at the boundary") {
    const EventId id{1, 2};
    const SpanSample good = make_sample(Span::L0_ReceiveToRoute, 1000, 2000, id);
    CHECK(good.valid);
    CHECK_EQ(good.ns, std::uint64_t{1000});
    CHECK_EQ(good.event_lo, std::uint64_t{2});

    // Swapped stamps: rejected at the point of construction, so no caller can
    // forget to check.
    const SpanSample bad = make_sample(Span::L0_ReceiveToRoute, 2000, 1000, id);
    CHECK(!bad.valid);
}

SCENARIO("LL-246", "P0", "P",
         "a sink that refuses everything does not stop the producer") {
    // LL-007: the metrics exporter hanging must leave business behaviour
    // unchanged and only increment a loss counter.
    NullLatencySink sink;
    for (int i = 0; i < 10000; ++i) {
        SpanSample s;
        s.ns = 1;
        CHECK(!sink.try_sample(s));
    }
    CHECK_EQ(sink.lost_samples(), std::uint64_t{10000});
}

SCENARIO("LL-247", "P0", "P",
         "the lane is SPSC: the producer writes and the metrics thread drains") {
    TelemetryLane lane;
    constexpr int kCount = 4000;
    std::uint64_t drained = 0;
    std::thread metrics([&] {
        // The drain thread claims the consumer role for the lane's lifetime.
        while (drained < kCount) {
            drained += lane.drain();
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < kCount; ++i) {
        SpanSample s;
        s.span = Span::L3_DurableAdmission;
        s.ns = static_cast<std::uint64_t>(i % 1000) + 1;
        while (!lane.try_sample(s)) std::this_thread::yield();
    }
    metrics.join();
    CHECK_EQ(drained, std::uint64_t{kCount});
    CHECK_EQ(lane.span(Span::L3_DurableAdmission).count(), std::uint64_t{kCount});
    CHECK_EQ(lane.lost_samples(), std::uint64_t{0});
    CHECK(lane.coverage_complete());
}

SCENARIO("LL-248", "P0", "P",
         "a performance claim is inadmissible when coverage or accounting is broken") {
    // Doc 28 Gate 4 as a computed predicate rather than a reviewer's judgement.
    MeasurementValidity v;
    v.coverage_complete = true;
    v.outcomes_balanced = true;
    v.sufficient_p99 = true;
    v.failpoints_compiled_in = false;
    CHECK(v.performance_claim_admissible());

    v.coverage_complete = false;
    CHECK(!v.performance_claim_admissible());
    v.coverage_complete = true;

    v.outcomes_balanced = false;
    CHECK(!v.performance_claim_admissible());
    v.outcomes_balanced = true;

    v.sufficient_p99 = false;
    CHECK(!v.performance_claim_admissible());
    v.sufficient_p99 = true;

    // A build with failpoints compiled in is a lab build, not a clean
    // performance run (ADR-08).
    v.failpoints_compiled_in = true;
    CHECK(!v.performance_claim_admissible());
}

SCENARIO("LL-249", "P0", "P",
         "spans are kept separate so they cannot be added together") {
    // Doc 3: the spans measure different things and must not be summed or
    // averaged across. The lane keeps one histogram per span, and the names
    // carry the definition.
    TelemetryLane lane;
    for (int i = 0; i < 100; ++i) {
        SpanSample a;
        a.span = Span::L2_DecisionCompute;
        a.ns = 10;
        lane.try_sample(a);
        SpanSample b;
        b.span = Span::L3_DurableAdmission;
        b.ns = 10000;
        lane.try_sample(b);
    }
    lane.drain();
    CHECK_EQ(lane.span(Span::L2_DecisionCompute).count(), std::uint64_t{100});
    CHECK_EQ(lane.span(Span::L3_DurableAdmission).count(), std::uint64_t{100});
    // The compute span is three orders of magnitude cheaper than the durable
    // one. A single blended "p99" would be meaningless, which is exactly why
    // doc 3 forbids it.
    CHECK_LT(lane.span(Span::L2_DecisionCompute).max(),
             lane.span(Span::L3_DurableAdmission).min());
    CHECK(std::string(to_string(Span::L3_DurableAdmission)).find("durable") !=
          std::string::npos);
}

SCENARIO("LL-250", "P0", "P",
         "an open-loop denominator keeps slow-period arrivals in the sample") {
    // Coordinated omission, doc 3 and ADR-06. A closed-loop generator stops
    // offering while the system is slow, so the slow period contributes no
    // samples and the tail disappears. This models both and shows the
    // difference, which is why the engine's generator schedules arrivals.
    Histogram closed, open;
    OutcomeCounters open_counts;

    // The system is fast for 900 ticks and 100x slower for 100 ticks.
    for (int i = 0; i < 900; ++i) {
        closed.record(100);
        open.record(100);
        ++open_counts.offered;
        ++open_counts.completed;
    }
    for (int i = 0; i < 100; ++i) {
        // Closed loop: one in-flight request, so the slow period yields one
        // sample, not a hundred.
        if (i == 0) closed.record(10000);
        // Open loop: the arrival was scheduled regardless, so every one of them
        // waited and every one is a sample.
        open.record(10000);
        ++open_counts.offered;
        ++open_counts.completed;
    }
    CHECK(open_counts.balanced());
    // The closed-loop p99 hides the stall almost entirely; the open-loop one
    // reports it.
    CHECK_LT(closed.percentile(99.0), open.percentile(99.0));
    CHECK(open.percentile(99.0) >= 10000);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "histogram_telemetry"); }
