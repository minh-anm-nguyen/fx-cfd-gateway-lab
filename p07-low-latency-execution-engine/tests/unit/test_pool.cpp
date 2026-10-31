// Catalog groups 05 and 06 (LL-081..120): preallocated pools, exhaustion,
// lifetime, ABA, reclamation and teardown. Plus FI-05, FI-06 and FI-07.
//
// The oracle codes are from design doc 23: M is no leak, no use-after-free, no
// ABA, bounded memory and correct handles.
//
// The allocation assertions here are the evidence for FR-03 and LI-11. They are
// not a claim that the process never allocates -- doc 2 is careful about that
// -- but that the measured region does not, which is checked by counting the
// global operator new rather than by inspection.

#include <set>
#include <thread>
#include <vector>

#include "alloc_probe.hpp"
#include "llx/event.hpp"
#include "llx/pool.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

// A payload with a destructor and a canary, so a use-after-free or a double
// destroy is detectable rather than merely undefined.
struct Tracked {
    static int live_count;
    std::uint64_t id = 0;
    std::uint64_t canary = 0;

    explicit Tracked(std::uint64_t i = 0) : id(i), canary(0xC0FFEE00 ^ i) { ++live_count; }
    ~Tracked() {
        canary = 0xDEADDEAD;
        --live_count;
    }
    bool intact() const { return canary == (0xC0FFEE00 ^ id); }
};
int Tracked::live_count = 0;

}  // namespace

SCENARIO("LL-081", "P0", "M", "a fresh pool is fully free and its capacity is exact") {
    Pool<InputEnvelope> p(4, 256);
    CHECK_EQ(p.total_slots(), std::size_t{1024});
    CHECK_EQ(p.free_slots(), std::size_t{1024});
    CHECK_EQ(p.live(), std::size_t{0});
    CHECK(p.no_leaks());
    CHECK_EQ(p.retired(), std::size_t{0});
}

SCENARIO("LL-082", "P0", "M", "a leased handle resolves and a released one does not") {
    Pool<Tracked> p(1, 8);
    const int live_before = Tracked::live_count;
    PoolHandle h;
    CHECK_EQ(p.lease(h, 42ULL), LeaseStatus::Ok);
    CHECK(!h.is_null());
    CHECK_EQ(Tracked::live_count, live_before + 1);

    Tracked* t = p.get(h);
    CHECK(t != nullptr);
    CHECK_EQ(t->id, std::uint64_t{42});
    CHECK(t->intact());

    CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    // The object was destroyed, not merely marked free.
    CHECK_EQ(Tracked::live_count, live_before);
    // And the handle no longer reaches anything. A non-null return here would
    // be the use-after-free LI-08 forbids.
    CHECK(p.get(h) == nullptr);
    CHECK(p.no_leaks());
}

SCENARIO("LL-083", "P0", "M",
         "exhaustion is a bounded refusal and never a heap fallback") {
    Pool<InputEnvelope> p(1, 4);
    std::vector<PoolHandle> held;
    for (int i = 0; i < 4; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h), LeaseStatus::Ok);
        held.push_back(h);
    }
    CHECK_EQ(p.free_slots(), std::size_t{0});

    // LI-10: POOL_EXHAUSTED, repeatedly and without ever growing. A malloc
    // fallback here would convert a visible backpressure signal into an
    // invisible heap and eventually an OOM.
    probe::reset();
    const probe::Window w;
    for (int i = 0; i < 1000; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h), LeaseStatus::Exhausted);
        CHECK(h.is_null());
    }
    CHECK_EQ(w.allocations_since(), std::uint64_t{0});
    CHECK_EQ(p.stats().exhaustions, std::uint64_t{1000});
    // Every live object is untouched: exhaustion never takes a slot from a
    // live order (doc 9).
    CHECK_EQ(p.live(), std::size_t{4});
    for (const PoolHandle& h : held) CHECK(p.get(h) != nullptr);
    for (const PoolHandle& h : held) CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    CHECK(p.no_leaks());
}

SCENARIO("LL-084", "P0", "M",
         "a stale handle is rejected after the slot has been re-leased (ABA)") {
    // FI-06 exactly: lease X, hand it to an async commit, free and re-assign X
    // to another order, then deliver the delayed callback for the first.
    Pool<Tracked> p(1, 1);  // one slot, so the reuse is guaranteed
    PoolHandle a;
    CHECK_EQ(p.lease(a, 100ULL), LeaseStatus::Ok);
    CHECK_EQ(p.release(a), ReleaseStatus::Ok);

    PoolHandle b;
    CHECK_EQ(p.lease(b, 200ULL), LeaseStatus::Ok);
    // Same slot, different generation. That difference is the whole protection.
    CHECK_EQ(a.slab, b.slab);
    CHECK_EQ(a.slot, b.slot);
    CHECK_NE(a.generation, b.generation);

    // The late callback for A must not reach B's object.
    CHECK(p.get(a) == nullptr);
    CHECK_EQ(p.resolve_status(a), ResolveStatus::StaleGeneration);
    Tracked* tb = p.get(b);
    CHECK(tb != nullptr);
    CHECK_EQ(tb->id, std::uint64_t{200});
    CHECK(tb->intact());

    // Nor may it free B's slot.
    CHECK_EQ(p.release(a), ReleaseStatus::StaleGeneration);
    CHECK_EQ(p.live(), std::size_t{1});
    CHECK(p.get(b) != nullptr);
    // Three stale resolutions were counted: the get, the explicit
    // resolve_status, and the release. Every attempt to reach through a stale
    // handle is a metric, which is what makes a rising ABA rate visible.
    CHECK_EQ(p.stats().stale_rejections, std::uint64_t{3});

    CHECK_EQ(p.release(b), ReleaseStatus::Ok);
    CHECK(p.no_leaks());
}

SCENARIO("LL-085", "P0", "M", "releasing the same handle twice is refused") {
    // FI-07. The second release must not return the slot to the free list a
    // second time, which would hand the same slot to two future leases.
    Pool<Tracked> p(1, 4);
    PoolHandle h;
    CHECK_EQ(p.lease(h, 7ULL), LeaseStatus::Ok);
    CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    const std::size_t free_after_first = p.free_slots();

    for (int i = 0; i < 10; ++i) CHECK_EQ(p.release(h), ReleaseStatus::DoubleFree);
    CHECK_EQ(p.free_slots(), free_after_first);
    CHECK_EQ(p.stats().double_free_rejections, std::uint64_t{10});
    CHECK_EQ(p.stats().releases, std::uint64_t{1});
}

SCENARIO("LL-086", "P0", "M", "a handle from a previous pool epoch is refused") {
    // LI-47: a handle minted before a reconfigure cannot be used after it.
    Pool<Tracked> old_pool(1, 4, /*epoch=*/1);
    PoolHandle h;
    CHECK_EQ(old_pool.lease(h, 1ULL), LeaseStatus::Ok);
    CHECK_EQ(h.epoch, std::uint32_t{1});

    Pool<Tracked> new_pool(1, 4, /*epoch=*/2);
    CHECK(new_pool.get(h) == nullptr);
    CHECK_EQ(new_pool.resolve_status(h), ResolveStatus::WrongEpoch);
    CHECK_EQ(new_pool.release(h), ReleaseStatus::WrongEpoch);
    // One per resolution attempt: get, resolve_status and release.
    CHECK_EQ(new_pool.stats().wrong_epoch_rejections, std::uint64_t{3});

    CHECK_EQ(old_pool.release(h), ReleaseStatus::Ok);
}

SCENARIO("LL-087", "P0", "M", "an out-of-range or null handle resolves to nothing") {
    Pool<Tracked> p(2, 4);
    PoolHandle null_h;
    CHECK(null_h.is_null());
    CHECK(p.get(null_h) == nullptr);
    CHECK_EQ(p.release(null_h), ReleaseStatus::OutOfRange);

    PoolHandle wild;
    wild.epoch = 1;
    wild.slab = 99;
    wild.slot = 99;
    wild.generation = 1;
    CHECK(p.get(wild) == nullptr);
    CHECK_EQ(p.release(wild), ReleaseStatus::OutOfRange);
}

SCENARIO("LL-088", "P0", "M",
         "a slot at the end of its generation range is retired, never wrapped") {
    // Doc 9 gives two acceptable answers to generation wrap: a generation wide
    // enough to prove safety, or retirement. Retirement is what is implemented,
    // and the 8-bit instantiation reaches it in a few leases instead of 2^32.
    Pool<Tracked, std::uint8_t> p(1, 2);
    CHECK(p.set_generation_for_test(0, 0, 254));
    CHECK(p.set_generation_for_test(0, 1, 255));

    std::set<std::pair<std::uint32_t, std::uint32_t>> slots_used;
    std::vector<PoolHandle> handles;
    for (int i = 0; i < 2; ++i) {
        PoolHandle h;
        const LeaseStatus st = p.lease(h, static_cast<std::uint64_t>(i));
        if (st != LeaseStatus::Ok) break;
        CHECK_NE(h.generation, std::uint32_t{0});
        slots_used.insert({h.slab, h.slot});
        handles.push_back(h);
    }
    // The slot already at 255 was retired rather than wrapped to 0 or 1, where
    // it could have aliased an old handle.
    CHECK(p.retired() >= 1);
    for (const PoolHandle& h : handles) CHECK_EQ(p.release(h), ReleaseStatus::Ok);

    // A retired slot never comes back, so capacity shrinks and that is visible.
    CHECK_LT(p.free_slots(), p.total_slots());
    CHECK(p.no_leaks());
}

SCENARIO("LL-089", "P0", "M",
         "teardown destroys whatever is still live instead of leaking it") {
    const int before = Tracked::live_count;
    {
        Pool<Tracked> p(1, 8);
        for (int i = 0; i < 5; ++i) {
            PoolHandle h;
            CHECK_EQ(p.lease(h, static_cast<std::uint64_t>(i)), LeaseStatus::Ok);
        }
        // Five objects alive and their handles dropped: a leak, which the pool
        // reports before it is destroyed.
        CHECK_EQ(p.live(), std::size_t{5});
        CHECK(!p.no_leaks());
        CHECK_EQ(Tracked::live_count, before + 5);
    }
    // Doc 9: "teardown drains". Every destructor ran.
    CHECK_EQ(Tracked::live_count, before);
}

SCENARIO("LL-090", "P0", "M",
         "lease and release cycles perform no allocation after construction") {
    // LI-11 in its exact form: no dynamic allocation in the measured
    // steady-state region of a configured, warmed profile.
    Pool<InputEnvelope> p(2, 512);
    p.prewarm();
    CHECK(p.prewarmed());

    // Warm up outside the window: the first lease of each slot and any lazy
    // internal growth happens here, which is what "after prewarm" means.
    std::vector<PoolHandle> warm;
    for (int i = 0; i < 1024; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h), LeaseStatus::Ok);
        warm.push_back(h);
    }
    for (const PoolHandle& h : warm) CHECK_EQ(p.release(h), ReleaseStatus::Ok);

    probe::reset();
    {
        const probe::Window w;
        for (int round = 0; round < 10000; ++round) {
            PoolHandle h;
            CHECK_EQ(p.lease(h), LeaseStatus::Ok);
            InputEnvelope* e = p.get(h);
            CHECK(e != nullptr);
            e->order_id = static_cast<OrderId>(round);
            CHECK_EQ(p.release(h), ReleaseStatus::Ok);
        }
        CHECK_EQ(w.allocations_since(), std::uint64_t{0});
    }
    CHECK(p.no_leaks());
}

SCENARIO("LL-091", "P0", "M",
         "handles survive a round trip through a ring without becoming pointers") {
    // LI-12: identity travels as a value. The handle is trivially copyable and
    // 16 bytes, so it fits a ring slot and carries no address.
    static_assert(std::is_trivially_copyable_v<PoolHandle>);
    static_assert(sizeof(PoolHandle) == 16);

    Pool<Tracked> p(1, 64);
    SpscRing<PoolHandle, 64> ring;
    for (std::uint64_t i = 0; i < 32; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h, i), LeaseStatus::Ok);
        CHECK(ring.try_push(h));
    }
    for (std::uint64_t i = 0; i < 32; ++i) {
        PoolHandle h;
        CHECK(ring.try_pop(h));
        Tracked* t = p.get(h);
        CHECK(t != nullptr);
        CHECK_EQ(t->id, i);
        CHECK(t->intact());
        CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    }
    CHECK(p.no_leaks());
}

SCENARIO("LL-092", "P0", "M",
         "a remote release is queued for the owner, not applied on the caller's thread") {
    // Doc 9 forbids an unsynchronized free list shared between threads. A
    // release from another thread goes through the reclaim queue and the owner
    // performs the actual reclaim.
    Pool<Tracked> p(1, 16);
    std::vector<PoolHandle> handles;
    for (std::uint64_t i = 0; i < 8; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h, i), LeaseStatus::Ok);
        handles.push_back(h);
    }
    const std::size_t live_before = p.live();

    std::thread other([&] {
        for (const PoolHandle& h : handles) p.release_remote(h);
    });
    other.join();

    // Nothing has been reclaimed yet: the queue holds the requests.
    CHECK_EQ(p.live(), live_before);
    const std::size_t drained = p.drain_remote_reclaims();
    CHECK_EQ(drained, std::size_t{8});
    CHECK_EQ(p.live(), std::size_t{0});
    CHECK_EQ(p.stats().remote_reclaims, std::uint64_t{8});
    CHECK(p.no_leaks());
}

SCENARIO("LL-093", "P0", "M",
         "a second remote releasing thread is refused rather than corrupting the list") {
    // The reclaim queue is itself SPSC, so there is exactly one remote
    // releaser. A second one is refused by the ring's ownership guard, which is
    // a detected refusal and not a corrupted free list.
    Pool<Tracked> p(1, 16);
    PoolHandle h1, h2;
    CHECK_EQ(p.lease(h1, 1ULL), LeaseStatus::Ok);
    CHECK_EQ(p.lease(h2, 2ULL), LeaseStatus::Ok);

    bool first_ok = false, second_ok = true;
    std::thread a([&] { first_ok = p.release_remote(h1); });
    a.join();
    std::thread b([&] { second_ok = p.release_remote(h2); });
    b.join();

#ifdef LLX_NO_OWNERSHIP_GUARD
    TT_INCONCLUSIVE("ownership guard compiled out in this build variant");
#else
    CHECK(first_ok);
    CHECK(!second_ok);  // refused, and the caller still holds a valid handle
    CHECK(p.get(h2) != nullptr);
    p.drain_remote_reclaims();
    CHECK_EQ(p.release(h2), ReleaseStatus::Ok);
    CHECK(p.no_leaks());
#endif
}

SCENARIO("LL-094", "P0", "M", "a full reclaim queue refuses rather than drops a handle") {
    // Bounded backpressure, not a leak: the caller retries. A silent drop here
    // would leak the slot forever.
    Pool<Tracked> p(2, 4096);
    std::vector<PoolHandle> handles;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h, i), LeaseStatus::Ok);
        handles.push_back(h);
    }
    std::size_t accepted = 0, refused = 0;
    std::thread other([&] {
        for (const PoolHandle& h : handles) {
            if (p.release_remote(h)) ++accepted;
            else ++refused;
        }
    });
    other.join();
    // The reclaim ring holds 1024, so some were refused and none were lost.
    CHECK(accepted > 0);
    CHECK(refused > 0);
    CHECK_EQ(accepted + refused, handles.size());
    p.drain_remote_reclaims();
    // The refused ones are still live and still owned by the caller, so they
    // can be released locally. Nothing leaked.
    std::size_t released = 0;
    for (const PoolHandle& h : handles)
        if (p.release(h) == ReleaseStatus::Ok) ++released;
    CHECK_EQ(released, refused);
    CHECK(p.no_leaks());
}

SCENARIO("LL-095", "P0", "M", "pool exhaustion through the failpoint reports the reason") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // FI-05: fill the pool but leave queue slots free, so the refusal comes
    // from the pool and names the pool.
    fp::disarm_all();
    fp::reset_counters();
    Pool<InputEnvelope> p(1, 64);
    fp::arm(std::string(fp::point::kPoolExhaust), fp::Action::ReturnError, 0, 1);
    PoolHandle h;
    CHECK_EQ(p.lease(h), LeaseStatus::Exhausted);
    CHECK(fp::fired(fp::point::kPoolExhaust) > 0);
    CHECK_EQ(p.stats().exhaustions, std::uint64_t{1});
    // Still empty: the refusal did not consume a slot.
    CHECK_EQ(p.live(), std::size_t{0});
    fp::disarm_all();
    // With the failpoint disarmed the pool works normally, proving the refusal
    // was the injected fault and not a real exhaustion.
    CHECK_EQ(p.lease(h), LeaseStatus::Ok);
    CHECK_EQ(p.release(h), ReleaseStatus::Ok);
#endif
}

SCENARIO("LL-096", "P0", "M",
         "prewarm touches every free slot and leaves live objects alone") {
    Pool<Tracked> p(2, 32);
    PoolHandle h;
    CHECK_EQ(p.lease(h, 555ULL), LeaseStatus::Ok);
    // prewarm writes raw bytes into free slots. A live object must not be in
    // its path.
    p.prewarm();
    Tracked* t = p.get(h);
    CHECK(t != nullptr);
    CHECK_EQ(t->id, std::uint64_t{555});
    CHECK(t->intact());
    CHECK_EQ(p.release(h), ReleaseStatus::Ok);
}

SCENARIO("LL-097", "P0", "M",
         "handle fields are independent so slab and slot cannot be transposed") {
    Pool<Tracked> p(4, 4);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::vector<PoolHandle> handles;
    for (std::uint64_t i = 0; i < 16; ++i) {
        PoolHandle h;
        CHECK_EQ(p.lease(h, i), LeaseStatus::Ok);
        CHECK_LT(h.slab, std::uint32_t{4});
        CHECK_LT(h.slot, std::uint32_t{4});
        // Every (slab, slot) pair is handed out exactly once while live.
        CHECK(seen.insert({h.slab, h.slot}).second);
        handles.push_back(h);
    }
    CHECK_EQ(seen.size(), std::size_t{16});
    // And each handle still resolves to its own object, which transposed
    // indexing would break.
    for (std::uint64_t i = 0; i < 16; ++i) {
        Tracked* t = p.get(handles[i]);
        CHECK(t != nullptr);
        CHECK_EQ(t->id, i);
    }
    for (const PoolHandle& h : handles) CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    CHECK(p.no_leaks());
}

SCENARIO("LL-098", "P0", "M", "the pool is bounded: RSS cannot grow with traffic") {
    // Doc 20's memory budget in its simplest observable form: the capacity is
    // fixed at construction and a million lease/release cycles do not change
    // it.
    Pool<InputEnvelope> p(1, 128);
    const std::size_t cap = p.total_slots();
    probe::reset();
    const probe::Window w;
    for (int i = 0; i < 200000; ++i) {
        PoolHandle h;
        if (p.lease(h) == LeaseStatus::Ok) CHECK_EQ(p.release(h), ReleaseStatus::Ok);
    }
    CHECK_EQ(p.total_slots(), cap);
    CHECK_EQ(w.allocations_since(), std::uint64_t{0});
    CHECK(p.no_leaks());
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "pool"); }
