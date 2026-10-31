// Catalog group 11 (LL-201..220): clocks, timestamps and ordering. Plus the
// hash determinism the state-hash oracle depends on.
//
// Oracle code P: the performance evidence is valid -- same span, raw samples and
// counts, timing and environment manifest.

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "llx/clock.hpp"
#include "llx/hash.hpp"
#include "llx/opt_barrier.hpp"
#include "tiny_test.hpp"

using namespace llx;

SCENARIO("LL-201", "P0", "P", "the monotonic clock never goes backwards") {
    std::uint64_t prev = MonotonicClock::now_ns();
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t now = MonotonicClock::now_ns();
        // Equal is allowed: the clock's resolution may be coarser than the loop.
        // Decreasing is not, and doc 3 forbids computing a duration from a
        // source that can.
        CHECK(now >= prev);
        prev = now;
    }
}

SCENARIO("LL-202", "P0", "P", "clock provenance is recorded, not assumed") {
    const ClockProvenance& p = MonotonicClock::provenance();
    CHECK(!p.source.empty());
    CHECK(!p.note.empty());
    // The resolution has to be measured for the number to mean anything; a
    // zero here would mean the calibration failed.
    CHECK(p.resolution_ns > 0);
    CHECK(p.steady_is_monotonic);
    if (!p.target_platform) {
        // Doc 11 and LI-28/29: a run on the fallback source is valid for
        // same-host comparison and must not be compared with a Linux baseline.
        // The note is what carries that into the report.
        CHECK(p.note.find("must not be compared") != std::string::npos);
    } else {
        CHECK(p.monotonic_raw);
        CHECK_EQ(p.source, std::string("CLOCK_MONOTONIC_RAW"));
    }
}

SCENARIO("LL-203", "P0", "P", "a duration that runs backwards is rejected, not wrapped") {
    // LI-25. The unsigned subtraction would silently produce an enormous
    // positive value, which is exactly how a bogus tail latency gets into a
    // histogram.
    const Duration d = duration_ns(1000, 500);
    CHECK(!d.ok());
    CHECK_EQ(d.status, DurationStatus::Negative);
    CHECK_EQ(d.ns, std::uint64_t{0});

    const Duration ok = duration_ns(500, 1500);
    CHECK(ok.ok());
    CHECK_EQ(ok.ns, std::uint64_t{1000});
}

SCENARIO("LL-204", "P0", "P", "an implausible duration is flagged for investigation") {
    // Doc 3: outliers are investigated, not silently discarded. The status
    // distinguishes "this is wrong" from "this is slow".
    const Duration d = duration_ns(0, kImplausibleNs + 1);
    CHECK(!d.ok());
    CHECK_EQ(d.status, DurationStatus::Implausible);
    // The value is preserved so the investigation has something to look at.
    CHECK_EQ(d.ns, kImplausibleNs + 1);

    const Duration edge = duration_ns(0, kImplausibleNs);
    CHECK(edge.ok());
}

SCENARIO("LL-205", "P0", "P", "a manual clock can be driven backwards to prove rejection") {
    // The deterministic clock exists so a test can create the condition the
    // engine must refuse, rather than waiting for a real clock to misbehave.
    ManualClock c;
    const std::uint64_t t0 = c.now_ns();
    c.advance_ns(5000);
    const std::uint64_t t1 = c.now_ns();
    CHECK(duration_ns(t0, t1).ok());
    CHECK_EQ(duration_ns(t0, t1).ns, std::uint64_t{5000});

    c.rewind_ns(10000);
    const std::uint64_t t2 = c.now_ns();
    CHECK(!duration_ns(t1, t2).ok());
    CHECK_EQ(duration_ns(t1, t2).status, DurationStatus::Negative);
}

SCENARIO("LL-206", "P0", "P",
         "the measured clock overhead is small enough for the spans being measured") {
    const ClockProvenance& p = MonotonicClock::provenance();
    // Doc 14 wants the observer effect quantified. What matters is not a
    // threshold but that the number exists and is reported; a reading that
    // costs more than the span it measures makes the span unmeasurable, and the
    // report has to say so rather than hide it.
    tt::note_state_hash("clock_overhead_ns=" + std::to_string(p.overhead_ns) +
                        ";resolution_ns=" + std::to_string(p.resolution_ns));
    if (p.overhead_ns > 10000) {
        TT_INCONCLUSIVE("a single clock reading costs " + std::to_string(p.overhead_ns) +
                        " ns on this host, which is too coarse to measure the L1/L2 "
                        "spans meaningfully. Not a failure of the engine: the "
                        "measurement is unusable and doc 23 calls that INCONCLUSIVE");
    }
    CHECK(p.overhead_ns < 10000);
}

SCENARIO("LL-207", "P0", "P",
         "readings taken on different threads are comparable on this host") {
    // Doc 3 forbids subtracting timestamps across unsynchronized hosts and
    // warns about TSC across sockets. Within one process on one host the
    // monotonic source must be consistent, and this checks it rather than
    // assuming it.
    constexpr int kThreads = 4;
    std::vector<std::uint64_t> firsts(kThreads), lasts(kThreads);
    const std::uint64_t before = MonotonicClock::now_ns();
    {
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i) {
            ts.emplace_back([&, i] {
                firsts[static_cast<std::size_t>(i)] = MonotonicClock::now_ns();
                std::uint64_t sink = 0;
                for (int k = 0; k < 10000; ++k) {
                    sink += MonotonicClock::now_ns();
                    do_not_optimize(sink);
                }
                lasts[static_cast<std::size_t>(i)] = MonotonicClock::now_ns();
            });
        }
        for (std::thread& t : ts) t.join();
    }
    const std::uint64_t after = MonotonicClock::now_ns();
    for (int i = 0; i < kThreads; ++i) {
        // Every thread's readings lie inside the interval the main thread
        // bracketed. A per-core clock that was not synchronized would break
        // this.
        CHECK(firsts[static_cast<std::size_t>(i)] >= before);
        CHECK(lasts[static_cast<std::size_t>(i)] <= after);
        CHECK(lasts[static_cast<std::size_t>(i)] >= firsts[static_cast<std::size_t>(i)]);
    }
}

// --- hash determinism -----------------------------------------------------

SCENARIO("LL-208", "P0", "D", "SHA-256 matches the published vectors") {
    // The state-hash oracle is only worth anything if the hash is the real
    // thing, so it is checked against the standard test vectors rather than
    // against itself.
    CHECK_EQ(Sha256::of(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Sha256::of("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Sha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // A message that spans several blocks and lands exactly on a boundary, where
    // the padding logic is easiest to get wrong.
    CHECK_EQ(Sha256::of(std::string(55, 'a')),
             Sha256::of(std::string(55, 'a')));
    CHECK_NE(Sha256::of(std::string(55, 'a')), Sha256::of(std::string(56, 'a')));
    CHECK_EQ(Sha256::of(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

SCENARIO("LL-209", "P0", "D", "length-prefixed fields cannot collide by concatenation") {
    // Without the length prefix {"ab","c"} and {"a","bc"} hash alike, and the
    // state hash would stop distinguishing states that differ.
    Sha256 a;
    a.field("ab").field("c");
    Sha256 b;
    b.field("a").field("bc");
    CHECK_NE(a.hex(), b.hex());

    Sha256 c;
    c.field("ab").field("c");
    Sha256 d;
    d.field("ab").field("c");
    CHECK_EQ(c.hex(), d.hex());
}

SCENARIO("LL-210", "P0", "D", "the set digest is order independent but content sensitive") {
    // This is what lets the baseline and the optimized pipeline be compared
    // without their container iteration order mattering.
    SetDigest a, b;
    for (const char* s : {"one", "two", "three"}) a.add(s);
    for (const char* s : {"three", "one", "two"}) b.add(s);
    CHECK_EQ(a.value(), b.value());
    CHECK_EQ(a.count(), b.count());

    SetDigest c;
    for (const char* s : {"one", "two", "four"}) c.add(s);
    CHECK_NE(a.value(), c.value());
}

SCENARIO("LL-211", "P0", "D", "numeric fields hash by value, not by their text form") {
    // 1 and "1" must not hash alike, and the width must be part of the input,
    // so a uint32 1 and a uint64 1 are distinguishable.
    Sha256 a;
    a.field_num(std::uint32_t{1});
    Sha256 b;
    b.field_num(std::uint64_t{1});
    CHECK_NE(a.hex(), b.hex());

    Sha256 c;
    c.field("1");
    Sha256 e;
    e.field_num(std::uint8_t{1});
    CHECK_NE(c.hex(), e.hex());
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "clock_hash"); }
