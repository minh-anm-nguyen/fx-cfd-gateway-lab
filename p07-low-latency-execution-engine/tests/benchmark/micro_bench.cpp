// Micro-benchmarks for the primitives.
//
// Design doc 15.3 lists six benchmarking traps, and this file is built to avoid
// them rather than to produce an impressive number:
//
//   dead-code elimination   every loop's result is consumed through
//                           do_not_optimize and a final checksum is printed,
//                           so a deleted loop shows up as a wrong checksum
//                           rather than as a fast result.
//   observer effect         built against llx_measured: no failpoints, no
//                           ownership guard. It is NOT part of the correctness
//                           gates and ctest does not run it.
//   denominator cheating    each line reports the operation count it divided
//                           by, so the per-operation figure can be checked.
//   unfair comparison       the virtual-channel and direct-template variants
//                           run the same payload through the same capacity.
//                           Doc 18 asks for exactly this measurement before
//                           anyone removes the indirection.
//   small samples           repetitions are reported individually, and the
//                           spread is printed, because one number from one run
//                           is not a measurement (doc 15.2 step 4).
//   span confusion          these are primitive costs only. They are NOT the
//                           pipeline's latency and may not be added to it or
//                           compared with the CV's receive-to-route figure
//                           (doc 3, LI-29).

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "llx/clock.hpp"
#include "llx/engine.hpp"
#include "llx/manifest.hpp"
#include "llx/opt_barrier.hpp"
#include "llx/pool.hpp"
#include "llx/spsc_ring.hpp"

using namespace llx;

namespace {

constexpr int kReps = 7;
constexpr std::uint64_t kOps = 2000000;

struct Result {
    std::string name;
    std::uint64_t ops = 0;
    std::vector<double> ns_per_op;
    std::uint64_t checksum = 0;
};

void report(const Result& r) {
    std::vector<double> v = r.ns_per_op;
    std::sort(v.begin(), v.end());
    const double median = v[v.size() / 2];
    const double lo = v.front();
    const double hi = v.back();
    const double spread = lo > 0 ? 100.0 * (hi - lo) / lo : 0.0;
    std::printf("%-38s %10llu ops  median %7.2f ns/op  min %7.2f  max %7.2f  "
                "spread %5.1f%%  checksum %llu\n",
                r.name.c_str(), static_cast<unsigned long long>(r.ops), median, lo, hi,
                spread, static_cast<unsigned long long>(r.checksum));
    std::printf("    all reps:");
    for (double d : r.ns_per_op) std::printf(" %.2f", d);
    std::printf("\n");
}

template <typename Fn>
Result measure(const std::string& name, std::uint64_t ops, Fn&& body) {
    Result r;
    r.name = name;
    r.ops = ops;
    // One untimed repetition first: it warms caches, branch predictors and any
    // lazy allocation, none of which belong in a steady-state figure.
    r.checksum = body();
    for (int i = 0; i < kReps; ++i) {
        const std::uint64_t t0 = MonotonicClock::now_ns();
        const std::uint64_t sum = body();
        const std::uint64_t t1 = MonotonicClock::now_ns();
        do_not_optimize(sum);
        // Summed, not XORed: an even number of identical reps would XOR to
        // zero, which is the value a deleted loop produces, so the dead-code
        // signal would be indistinguishable from a correct run.
        r.checksum += sum;
        const Duration d = duration_ns(t0, t1);
        // A reading the clock check refuses is not recorded as a fast result.
        if (d.ok()) r.ns_per_op.push_back(static_cast<double>(d.ns) /
                                          static_cast<double>(ops));
        else std::fprintf(stderr, "  %s: rep %d produced an invalid duration\n",
                          name.c_str(), i);
    }
    if (r.ns_per_op.empty()) r.ns_per_op.push_back(0.0);
    return r;
}

struct Payload {
    std::uint64_t a = 0, b = 0, c = 0, d = 0;
};

}  // namespace

int main() {
    const BuildInfo b = BuildInfo::current();
    const ClockProvenance& cp = MonotonicClock::provenance();

    std::printf("micro_bench -- primitive costs only\n");
    std::printf("build: %s, %s, failpoints=%s, ownership_guard=%s, cacheline=%zu\n",
                b.compiler.c_str(), b.build_type.c_str(),
                b.failpoints_compiled_in ? "ON" : "off",
                b.ownership_guard_compiled_in ? "ON" : "off", b.cache_line_bytes);
    std::printf("clock: %s, resolution %lluns, overhead %lluns, target_platform=%s\n",
                cp.source.c_str(), static_cast<unsigned long long>(cp.resolution_ns),
                static_cast<unsigned long long>(cp.overhead_ns),
                cp.target_platform ? "yes" : "no");
    if (b.failpoints_compiled_in)
        std::printf("WARNING: failpoints are compiled into this binary. The numbers below\n"
                    "         are not a clean measurement (ADR-08).\n");
    if (b.build_type.find("debug") != std::string::npos)
        std::printf("WARNING: this is a debug build. Configure with -DCMAKE_BUILD_TYPE=\n"
                    "         Release before quoting anything below.\n");
    std::printf("\n");

    std::vector<Result> results;

    // Ring: single-threaded push then pop, so the figure is the primitive's own
    // cost without any cross-core traffic. A threaded figure would measure the
    // cache-line transfer, which is a different question.
    {
        auto ring = std::make_unique<SpscRing<Payload, 1024>>();
        results.push_back(measure("SpscRing push+pop (same thread)", kOps, [&] {
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < kOps; ++i) {
                Payload p{i, i ^ 0xff, i << 1, ~i};
                if (!ring->try_push(p)) ring->try_pop(p);
                Payload out{};
                if (ring->try_pop(out)) sum += out.a;
                do_not_optimize(sum);
            }
            return sum;
        }));
    }

    // The virtual channel against the direct template, same payload and
    // capacity. Doc 18 warns that virtual dispatch on the hot path is a cost to
    // be measured rather than assumed away, and this is that measurement.
    {
        auto direct = std::make_unique<SpscChannel<1024>>();
        results.push_back(measure("SpscChannel direct (template)", kOps, [&] {
            std::uint64_t sum = 0;
            InputEnvelope e{};
            for (std::uint64_t i = 0; i < kOps; ++i) {
                e.order_id = i;
                direct->try_publish(e);
                InputEnvelope out{};
                if (direct->try_consume(out)) sum += out.order_id;
                do_not_optimize(sum);
            }
            return sum;
        }));

        std::unique_ptr<IQueueChannel> virt = make_channel(1024);
        results.push_back(measure("SpscChannel via IQueueChannel (virtual)", kOps, [&] {
            std::uint64_t sum = 0;
            InputEnvelope e{};
            for (std::uint64_t i = 0; i < kOps; ++i) {
                e.order_id = i;
                virt->try_publish(e);
                InputEnvelope out{};
                if (virt->try_consume(out)) sum += out.order_id;
                do_not_optimize(sum);
            }
            return sum;
        }));
    }

    // Pool lease and release against the heap, which is the A/B of doc 10's
    // experiment matrix: preallocated against malloc baseline.
    {
        Pool<Payload> pool(4, 1024);
        pool.prewarm();
        results.push_back(measure("Pool lease+release (preallocated)", kOps, [&] {
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < kOps; ++i) {
                PoolHandle h;
                if (pool.lease(h, Payload{i, i, i, i}) == LeaseStatus::Ok) {
                    Payload* p = pool.get(h);
                    if (p != nullptr) sum += p->a;
                    pool.release(h);
                }
                do_not_optimize(sum);
            }
            return sum;
        }));

        results.push_back(measure("new/delete (malloc baseline)", kOps, [&] {
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < kOps; ++i) {
                auto* p = new Payload{i, i, i, i};
                // The POINTER has to escape, not just the value read through
                // it. C++14 onward lets a compiler elide a new/delete pair
                // whose allocation does not escape, and GCC does: an earlier
                // version of this line measured 0.47 ns per malloc/free pair,
                // which is trap one of doc 15.3 appearing in this project's own
                // benchmark. do_not_optimize on the pointer stops it.
                do_not_optimize(p);
                sum += p->a;
                delete p;
                do_not_optimize(sum);
            }
            return sum;
        }));
    }

    // The clock itself, so the overhead of measuring is visible next to the
    // things being measured (doc 14).
    results.push_back(measure("MonotonicClock::now_ns", kOps, [&] {
        std::uint64_t sum = 0;
        for (std::uint64_t i = 0; i < kOps; ++i) {
            sum += MonotonicClock::now_ns();
            do_not_optimize(sum);
        }
        return sum;
    }));

    // The reducer, which is the business decision cost (span L2) in isolation.
    {
        JournalStore store;
        store.configure_risk_domain(1, 1000000000LL * kQtyScale);
        InputEnvelope e{};
        e.event_id = EventId{1, 1};
        e.type = EventType::NewOrder;
        e.risk_domain = 1;
        e.symbol = 7;
        e.side = Side::Buy;
        e.qty = 10 * kQtyScale;
        e.px = 1 * kPxScale;
        constexpr std::uint64_t kReduceOps = 500000;
        results.push_back(measure("reduce() new order (pure, no commit)", kReduceOps, [&] {
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < kReduceOps; ++i) {
                e.order_id = 1000000 + i;  // always a fresh id, so it is accepted
                const Proposal p = reduce(store.state(), e, false);
                sum += p.accepted ? 1u : 0u;
                do_not_optimize(sum);
            }
            return sum;
        }));
    }

    std::printf("\n");
    for (const Result& r : results) report(r);

    std::printf(
        "\nThese are primitive costs on this host and this build. They are NOT the\n"
        "pipeline's latency: that is measured per span by replay_benchmark, which\n"
        "also records the environment manifest and the outcome accounting a\n"
        "performance claim needs (doc 28 Gate 4). Nothing here may be added to a\n"
        "span figure or compared with the CV's 95 us receive-to-route number,\n"
        "whose span definition and workload are different (LI-29).\n");
    return 0;
}
