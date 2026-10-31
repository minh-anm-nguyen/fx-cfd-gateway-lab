// Catalog group 03 (LL-041..060): memory ordering, atomics and race detection,
// under real concurrency.
//
// Oracle codes Q, M and C. What these cases can and cannot establish, stated
// up front because doc 23 layer 3 is specific about it:
//
//   they CAN establish that the queue is FIFO and lossless under genuine
//   parallel execution, that nothing is duplicated or phantom, that the
//   occupancy invariant holds throughout, and that a checksum computed by the
//   producer matches the one the consumer derives;
//
//   they CANNOT establish that the acquire/release pairs are correct on a weak
//   memory model. This host is x86_64, which is TSO: a missing release fence
//   would not be observable here however many schedules are run. LI-30 is
//   explicit that passing on x86 does not establish portability, so the cases
//   that need ARM64 or TSAN are BLOCKED and named rather than claimed.

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "llx/event.hpp"
#include "llx/spsc_ring.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

struct Item {
    std::uint64_t seq = 0;
    std::uint64_t a = 0, b = 0, c = 0;
    std::uint64_t check = 0;

    static Item make(std::uint64_t s) {
        Item i;
        i.seq = s;
        i.a = s * 0x9e3779b97f4a7c15ULL;
        i.b = ~s;
        i.c = s ^ 0x5555555555555555ULL;
        i.check = i.seq ^ i.a ^ i.b ^ i.c;
        return i;
    }
    bool intact() const { return check == (seq ^ a ^ b ^ c); }
};

// Sized so the whole suite finishes in a couple of minutes in a debug build
// with the ownership guard and the bounds checks compiled in. The property
// under test is loss, duplication and ordering under genuine parallel
// execution, and that does not need millions of items -- it needs the two
// threads to actually contend, which the small-capacity cases guarantee. A
// longer run belongs in the soak profile of the benchmark, not here.
constexpr std::uint64_t kStressCount = 400000;

// Bounded spin, then yield. A bare yield on every contended iteration costs a
// context switch per item and dominated the run time; the spin keeps the
// contention real and pays for it in cycles instead.
inline void back_off(int& spins) {
    if (++spins < 64) return;
    spins = 0;
    std::this_thread::yield();
}

}  // namespace

SCENARIO("LL-041", "P0", "Q,M,C",
         "four hundred thousand items cross the ring in parallel with no loss or duplication") {
    auto ring = std::make_unique<SpscRing<Item, 1024>>();
    std::atomic<std::uint64_t> consumer_xor{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> order_violation{false};
    std::atomic<bool> corruption{false};

    std::thread consumer([&] {
        std::uint64_t expect = 0, xr = 0, n = 0;
        Item out;
        while (n < kStressCount) {
            if (ring->try_pop(out)) {
                if (out.seq != expect) order_violation = true;
                if (!out.intact()) corruption = true;
                xr ^= out.seq;
                ++expect;
                ++n;
            }
        }
        consumer_xor = xr;
        consumed = n;
    });

    std::uint64_t producer_xor = 0;
    int spins = 0;
    for (std::uint64_t i = 0; i < kStressCount; ++i) {
        // A bounded wait, never an unbounded spin: LI-07.
        while (!ring->try_push(Item::make(i))) back_off(spins);
        producer_xor ^= i;
    }
    consumer.join();

    CHECK(!order_violation);   // strict FIFO
    CHECK(!corruption);        // no torn or stale slot read
    CHECK_EQ(consumed.load(), kStressCount);
    CHECK_EQ(consumer_xor.load(), producer_xor);
    CHECK(ring->empty());
    CHECK(ring->invariant_ok());
    CHECK_EQ(ring->guard_violations(), std::uint64_t{0});
}

SCENARIO("LL-042", "P0", "Q,M,C",
         "a tiny ring forces maximum contention and still loses nothing") {
    // N=2 means the producer and consumer are almost always touching adjacent
    // slots, which is the worst case for the head/tail handshake.
    auto ring = std::make_unique<SpscRing<Item, 2>>();
    constexpr std::uint64_t kCount = 60000;
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> bad{false};

    std::thread consumer([&] {
        std::uint64_t expect = 0;
        Item out;
        while (expect < kCount) {
            if (ring->try_pop(out)) {
                if (out.seq != expect || !out.intact()) bad = true;
                ++expect;
            }
        }
        consumed = expect;
    });
    int spins = 0;
    for (std::uint64_t i = 0; i < kCount; ++i)
        while (!ring->try_push(Item::make(i))) back_off(spins);
    consumer.join();

    CHECK(!bad);
    CHECK_EQ(consumed.load(), kCount);
    CHECK(ring->invariant_ok());
}

SCENARIO("LL-043", "P0", "Q,M,C",
         "the occupancy invariant holds under concurrent observation") {
    // A third thread watches while the other two run. It must never see an
    // occupancy above the capacity: that would mean the counters disagreed in a
    // way the arithmetic cannot produce.
    auto ring = std::make_unique<SpscRing<Item, 256>>();
    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};
    std::atomic<std::size_t> max_seen{0};

    std::thread observer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const std::size_t n = ring->size();
            if (n > ring->capacity()) violation = true;
            // invariant_ok() reads the counter pair with a stability check, so
            // it is sound from a third thread. Without that check this
            // observer reported violations that never happened, which is what
            // drove the fix in spsc_ring.hpp.
            if (!ring->invariant_ok()) violation = true;
            std::size_t prev = max_seen.load(std::memory_order_relaxed);
            while (n > prev && !max_seen.compare_exchange_weak(prev, n)) {}
        }
    });
    std::thread consumer([&] {
        Item out;
        std::uint64_t n = 0;
        while (n < 150000) {
            if (ring->try_pop(out)) ++n;
        }
    });
    int spins = 0;
    for (std::uint64_t i = 0; i < 150000; ++i)
        while (!ring->try_push(Item::make(i))) back_off(spins);
    consumer.join();
    stop.store(true, std::memory_order_release);
    observer.join();

    CHECK(!violation);
    CHECK_LE(max_seen.load(), ring->capacity());
}

SCENARIO("LL-044", "P0", "Q,M,C",
         "a slow consumer produces back pressure, never an overwrite") {
    // The producer meets a full ring repeatedly. Every refusal must be a
    // refusal: if even one push overwrote an unconsumed slot, the consumer's
    // sequence check would notice.
    auto ring = std::make_unique<SpscRing<Item, 16>>();
    constexpr std::uint64_t kCount = 20000;
    std::atomic<std::uint64_t> refusals{0};
    std::atomic<bool> bad{false};

    std::thread consumer([&] {
        std::uint64_t expect = 0;
        Item out;
        while (expect < kCount) {
            if (ring->try_pop(out)) {
                if (out.seq != expect || !out.intact()) bad = true;
                ++expect;
                // Deliberately slow, so the ring spends most of its time full.
                if (expect % 64 == 0) std::this_thread::yield();
            }
        }
    });
    std::uint64_t local_refusals = 0;
    int spins = 0;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!ring->try_push(Item::make(i))) {
            ++local_refusals;
            back_off(spins);
        }
    }
    refusals = local_refusals;
    consumer.join();

    CHECK(!bad);
    // The test is only meaningful if the ring actually filled up.
    CHECK(refusals.load() > 0);
}

SCENARIO("LL-045", "P0", "Q,M,C",
         "a handle round trip across threads keeps the pool consistent") {
    // The ring carries pool handles rather than payloads, which is the shape
    // the engine uses for anything too large for a slot. Ownership transfers at
    // the release/acquire boundary and the pool must stay balanced.
    Pool<Item> pool(4, 512);
    pool.prewarm();
    auto ring = std::make_unique<SpscRing<PoolHandle, 256>>();
    constexpr std::uint64_t kCount = 40000;
    std::atomic<std::uint64_t> seen{0};
    std::atomic<bool> bad{false};
    // Releases happen on the consumer side through the remote reclaim queue, so
    // the pool's free list stays single-owner (doc 9).
    std::thread consumer([&] {
        std::uint64_t expect = 0;
        PoolHandle h;
        while (expect < kCount) {
            if (ring->try_pop(h)) {
                const Item* it = pool.get(h);
                if (it == nullptr || it->seq != expect || !it->intact()) bad = true;
                int release_spins = 0;
                while (!pool.release_remote(h)) back_off(release_spins);
                ++expect;
            }
        }
        seen = expect;
    });

    int spins = 0;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        PoolHandle h;
        // The owner thread drains reclaims as it leases, which is what keeps
        // the pool from exhausting.
        while (pool.lease(h, Item::make(i)) != LeaseStatus::Ok) {
            pool.drain_remote_reclaims();
            back_off(spins);
        }
        while (!ring->try_push(h)) back_off(spins);
    }
    consumer.join();
    pool.drain_remote_reclaims();

    CHECK(!bad);
    CHECK_EQ(seen.load(), kCount);
    CHECK(pool.no_leaks());
    CHECK_EQ(pool.stats().double_free_rejections, std::uint64_t{0});
    CHECK_EQ(pool.stats().stale_rejections, std::uint64_t{0});
}

SCENARIO("LL-046", "P0", "Q,M,C",
         "randomized start offsets exercise many interleavings with the same oracle") {
    // Doc 23 layer 3 asks for schedule perturbation. The seed is recorded so a
    // failure is reproducible.
    const std::uint64_t seed = 20261030;
    tt::note_seed(seed);
    std::mt19937_64 rng(seed);

    for (int round = 0; round < 12; ++round) {
        auto ring = std::make_unique<SpscRing<Item, 64>>();
        const std::uint64_t count = 8000 + (rng() % 8000);
        const int producer_delay = static_cast<int>(rng() % 3);
        const int consumer_delay = static_cast<int>(rng() % 3);
        std::atomic<bool> bad{false};
        std::atomic<std::uint64_t> done{0};

        std::thread consumer([&] {
            std::uint64_t expect = 0;
            Item out;
            while (expect < count) {
                if (ring->try_pop(out)) {
                    if (out.seq != expect || !out.intact()) bad = true;
                    ++expect;
                    if (consumer_delay != 0 && expect % 128 == 0)
                        std::this_thread::yield();
                }
            }
            done = expect;
        });
        int spins = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!ring->try_push(Item::make(i))) back_off(spins);
            if (producer_delay != 0 && i % 128 == 0) std::this_thread::yield();
        }
        consumer.join();
        CHECK(!bad);
        CHECK_EQ(done.load(), count);
        CHECK(ring->invariant_ok());
    }
}

SCENARIO("LL-047", "P0", "Q,M,C",
         "the publication is release-acquire so every field is visible together") {
    // On x86 this cannot fail, which is the point of saying so. What it does
    // check is that the consumer never observes a partially initialized slot
    // through the ring's own API: the four payload fields and the checksum are
    // written before the head release, so an intact checksum on every one of
    // two million items is consistent with correct publication.
    //
    // It is evidence, not proof. LI-30 and LL-042 (BLOCKED, below) record what
    // would be needed for proof.
    auto ring = std::make_unique<SpscRing<Item, 512>>();
    constexpr std::uint64_t kCount = 300000;
    std::atomic<std::uint64_t> torn{0};
    std::thread consumer([&] {
        std::uint64_t n = 0, bad = 0;
        Item out;
        while (n < kCount) {
            if (ring->try_pop(out)) {
                if (!out.intact()) ++bad;
                ++n;
            }
        }
        torn = bad;
    });
    int spins = 0;
    for (std::uint64_t i = 0; i < kCount; ++i)
        while (!ring->try_push(Item::make(i))) back_off(spins);
    consumer.join();
    CHECK_EQ(torn.load(), std::uint64_t{0});
}

SCENARIO("LL-048", "P0", "Q,M,C",
         "a producer and consumer running at very different rates stay correct") {
    // Asymmetric rates keep the ring pinned near empty or near full for long
    // stretches, which is where an off-by-one in the full or empty test would
    // show up.
    for (int mode = 0; mode < 2; ++mode) {
        auto ring = std::make_unique<SpscRing<Item, 128>>();
        constexpr std::uint64_t kCount = 40000;
        std::atomic<bool> bad{false};
        std::thread consumer([&] {
            std::uint64_t expect = 0;
            Item out;
            while (expect < kCount) {
                if (ring->try_pop(out)) {
                    if (out.seq != expect || !out.intact()) bad = true;
                    ++expect;
                } else if (mode == 0) {
                    // Fast consumer: spins on an almost-always-empty ring.
                    std::this_thread::yield();
                }
                if (mode == 1 && expect % 32 == 0) std::this_thread::yield();
            }
        });
        int spins = 0;
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring->try_push(Item::make(i))) back_off(spins);
            if (mode == 0 && i % 32 == 0) std::this_thread::yield();
        }
        consumer.join();
        CHECK(!bad);
        CHECK(ring->empty());
    }
}

SCENARIO_BLOCKED("LL-049", "P0", "Q,M,C",
                 "the same stress passes on a weak memory model (ARM64)",
                 "LI-30: passing on x86_64 does not establish portability, because TSO "
                 "hides a missing release fence. Needs an ARM64 runner, which doc 27.3 "
                 "lists as the CROSS-ARCH run set. This host is x86_64 only");

SCENARIO_BLOCKED("LL-050", "P0", "Q,M,C",
                 "ThreadSanitizer finds no data race in the stress run",
                 "mingw-w64 ships no libtsan, so -fsanitize=thread cannot link here. The "
                 "CMake option LLX_TSAN is wired for a Linux runner. Doc 28 Gate 1 "
                 "requires sanitizer evidence in the supported modes, and this mode is "
                 "not supported on this host");

SCENARIO_BLOCKED("LL-051", "P0", "Q,M,C",
                 "a model checker explores the full interleaving space of push/pop",
                 "needs CDSChecker, GenMC or a similar tool. Doc 23 layer 2 lists "
                 "model-based coverage as a separate layer from the randomized stress "
                 "above; the reference-model comparison in the model suite is the part "
                 "that is runnable here");

int main(int argc, char** argv) { return tt::run_all(argc, argv, "spsc_stress"); }
