// Catalog group 02 (LL-021..040): SPSC FIFO, boundary and wrap.
//
// Plus the group 01 and 03 cases that are properties of the ring itself. The
// oracle codes are the shorthand of design doc 23: Q is queue FIFO, count,
// ownership and capacity with no silent drop; M is no leak, no use-after-free,
// no ABA and bounded memory.
//
// Two notes on what is and is not provable here. The counter-wrap cases use the
// narrow-counter instantiation, which is a test seam rather than a different
// algorithm: the code under test is the same template, so driving a uint8_t
// counter across its wrap exercises exactly the arithmetic a uint64_t counter
// would reach after 2^64 operations (doc 8, FI-04). And the memory-ordering
// cases are honest about the limit of an x86 host: TSO will not reveal a
// missing release, so those cases are BLOCKED here and named, not passed.

#include <memory>
#include <thread>
#include <vector>

#include "llx/event.hpp"
#include "llx/spsc_ring.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

// A small trivially copyable payload with a checksum, so a torn or stale slot
// read is detectable rather than merely unlikely to be noticed.
struct Item {
    std::uint64_t seq = 0;
    std::uint64_t payload = 0;
    std::uint64_t check = 0;

    static Item make(std::uint64_t s) {
        Item i;
        i.seq = s;
        i.payload = s * 0x9e3779b97f4a7c15ULL;
        i.check = i.seq ^ i.payload;
        return i;
    }
    bool intact() const { return check == (seq ^ payload); }
};
static_assert(std::is_trivially_copyable_v<Item>);

}  // namespace

SCENARIO("LL-021", "P0", "Q,M", "an empty ring returns false from pop in finite time") {
    SpscRing<Item, 4> r;
    Item out = Item::make(99);
    const Item before = out;
    for (int i = 0; i < 1000; ++i) CHECK(!r.try_pop(out));
    // The out parameter must not have been touched: reading an unpublished slot
    // is the case this excludes.
    CHECK_EQ(out.seq, before.seq);
    CHECK_EQ(out.payload, before.payload);
    CHECK(r.empty());
    CHECK(r.invariant_ok());
}

SCENARIO("LL-022", "P0", "Q,M", "a full ring refuses the push and does not overwrite") {
    SpscRing<Item, 4> r;
    for (std::uint64_t i = 0; i < 4; ++i) CHECK(r.try_push(Item::make(i)));
    CHECK(r.full());
    CHECK_EQ(r.size(), std::size_t{4});
    const auto head_before = r.head_counter();

    // LI-02: the refusal is the whole point. Nothing unconsumed may be
    // overwritten, so head must not advance.
    for (int i = 0; i < 100; ++i) CHECK_EQ(r.push(Item::make(1000)), PushStatus::Full);
    CHECK_EQ(r.head_counter(), head_before);

    // The four original items are still there, in order and intact.
    for (std::uint64_t i = 0; i < 4; ++i) {
        Item out;
        CHECK(r.try_pop(out));
        CHECK_EQ(out.seq, i);
        CHECK(out.intact());
    }
    CHECK(r.empty());
}

SCENARIO("LL-023", "P0", "Q,M",
         "N=2 with ten million alternating push/pop keeps FIFO and never phantoms") {
    SpscRing<Item, 2> r;
    constexpr std::uint64_t kIters = 10000000;
    std::uint64_t checksum_in = 0, checksum_out = 0;
    for (std::uint64_t i = 0; i < kIters; ++i) {
        CHECK(r.try_push(Item::make(i)));
        checksum_in ^= i;
        Item out;
        CHECK(r.try_pop(out));
        CHECK_EQ(out.seq, i);
        CHECK(out.intact());
        checksum_out ^= out.seq;
    }
    CHECK_EQ(checksum_in, checksum_out);
    CHECK(r.empty());
    CHECK(r.invariant_ok());
}

SCENARIO("LL-024", "P0", "Q,M",
         "N=8192 burst then drain preserves order and never exceeds capacity") {
    // Heap allocated: a ring of 8192 items is larger than a comfortable stack
    // frame, and the engine allocates its channels the same way.
    auto r = std::make_unique<SpscRing<Item, 8192>>();
    std::size_t peak = 0;
    for (std::uint64_t i = 0; i < 8192; ++i) {
        CHECK(r->try_push(Item::make(i)));
        peak = std::max(peak, r->size());
    }
    CHECK_EQ(peak, std::size_t{8192});
    CHECK_LE(peak, r->capacity());
    CHECK(!r->try_push(Item::make(9999)));
    for (std::uint64_t i = 0; i < 8192; ++i) {
        Item out;
        CHECK(r->try_pop(out));
        CHECK_EQ(out.seq, i);
        CHECK(out.intact());
    }
    CHECK(r->empty());
}

SCENARIO_BLOCKED("LL-025", "P0", "Q,M",
                 "a non-power-of-two capacity is rejected before any allocation",
                 "enforced by static_assert((N & (N-1)) == 0) in spsc_ring.hpp. A "
                 "static_assert is a hard error, not an SFINAE-detectable one, so no "
                 "passing build can observe it from the inside. The evidence is the "
                 "negative-compilation check run as the ctest case "
                 "'static_assert_negatives'; this entry stays BLOCKED here so the verdict "
                 "is not claimed by a test that never exercised it (doc 23)");

SCENARIO_BLOCKED("LL-026", "P0", "Q,M",
                 "N=0 or N=1 must not produce a valid ring",
                 "enforced by static_assert(N >= 2) in spsc_ring.hpp, which cannot be "
                 "observed from inside a passing build. scripts/check_static_asserts.sh "
                 "compiles the negative cases and asserts the build fails; this case is "
                 "BLOCKED here rather than reported as a pass from a test that never "
                 "exercised it (doc 23)");

SCENARIO("LL-027", "P0", "Q,M",
         "counters near the wrap keep index and occupancy correct") {
    // The narrow-counter seam. With an 8-bit counter the wrap is 256 pushes
    // away instead of 2^64, and the arithmetic under test is identical.
    SpscRing<Item, 4, std::uint8_t> r;
    tt::note_seed(27);
    // Position both counters just below the wrap.
    CHECK(r.seek_counters_for_test(250));
    std::uint64_t expect = 0;
    for (int round = 0; round < 1000; ++round) {
        // Fill, drain, repeat: each round crosses a different point relative to
        // the wrap boundary.
        for (int i = 0; i < 3; ++i) {
            CHECK(r.try_push(Item::make(expect + static_cast<std::uint64_t>(i))));
            CHECK(r.invariant_ok());
            CHECK_LE(r.size(), r.capacity());
        }
        CHECK_EQ(r.size(), std::size_t{3});
        for (int i = 0; i < 3; ++i) {
            Item out;
            CHECK(r.try_pop(out));
            CHECK_EQ(out.seq, expect);
            CHECK(out.intact());
            ++expect;
        }
        CHECK(r.empty());
    }
    CHECK_EQ(expect, std::uint64_t{3000});
}

SCENARIO("LL-027b", "P0", "Q,M",
         "a 16-bit counter crossing the wrap while the ring is partly full") {
    // The harder variant: the wrap happens with items resident, so head and
    // tail are on opposite sides of the boundary and the occupancy has to come
    // from modular arithmetic rather than from a plain subtraction.
    SpscRing<Item, 8, std::uint16_t> r;
    CHECK(r.seek_counters_for_test(65530));
    std::uint64_t pushed = 0, popped = 0;
    for (int i = 0; i < 4; ++i) CHECK(r.try_push(Item::make(pushed++)));
    for (int round = 0; round < 5000; ++round) {
        CHECK(r.try_push(Item::make(pushed++)));
        CHECK_EQ(r.size(), std::size_t{5});
        CHECK(r.invariant_ok());
        Item out;
        CHECK(r.try_pop(out));
        CHECK_EQ(out.seq, popped++);
        CHECK(out.intact());
        CHECK_EQ(r.size(), std::size_t{4});
    }
    CHECK_EQ(r.size(), std::size_t{4});
}

SCENARIO("LL-031", "P0", "Q,M",
         "many alternating full and empty cycles keep the head-tail delta bounded") {
    SpscRing<Item, 16> r;
    std::uint64_t seq = 0;
    for (int cycle = 0; cycle < 2000; ++cycle) {
        while (r.try_push(Item::make(seq))) ++seq;
        CHECK(r.full());
        CHECK(r.invariant_ok());
        CHECK_LE(r.size(), r.capacity());
        Item out;
        std::uint64_t drained = 0;
        while (r.try_pop(out)) {
            CHECK(out.intact());
            ++drained;
            CHECK(r.invariant_ok());
        }
        CHECK_EQ(drained, std::uint64_t{16});
        CHECK(r.empty());
    }
}

SCENARIO("LL-032", "P0", "Q,M", "alternating bursts of 1 and N-1 keep order and capacity") {
    SpscRing<Item, 8> r;
    std::uint64_t push_seq = 0, pop_seq = 0;
    for (int round = 0; round < 1000; ++round) {
        const int burst = (round % 2 == 0) ? 1 : 7;
        for (int i = 0; i < burst; ++i) {
            CHECK(r.try_push(Item::make(push_seq++)));
            CHECK_LE(r.size(), r.capacity());
        }
        for (int i = 0; i < burst; ++i) {
            Item out;
            CHECK(r.try_pop(out));
            CHECK_EQ(out.seq, pop_seq++);
            CHECK(out.intact());
        }
    }
    CHECK_EQ(push_seq, pop_seq);
}

SCENARIO("LL-033", "P0", "Q,M",
         "payload ids near UINT64_MAX are not confused with the ring index") {
    // LI-12: a business identity is a value, never an index. An id at the top of
    // the range must survive unchanged.
    SpscRing<Item, 4> r;
    const std::uint64_t ids[] = {~std::uint64_t{0}, ~std::uint64_t{0} - 1,
                                 std::uint64_t{1} << 63, 0};
    for (std::uint64_t id : ids) {
        Item in;
        in.seq = id;
        in.payload = id ^ 0xffffffffffffffffULL;
        in.check = in.seq ^ in.payload;
        CHECK(r.try_push(in));
    }
    for (std::uint64_t id : ids) {
        Item out;
        CHECK(r.try_pop(out));
        CHECK_EQ(out.seq, id);
        CHECK(out.intact());
    }
}

SCENARIO("LL-034", "P0", "Q,M",
         "a consumer that starts late sees an explicit full and loses nothing admitted") {
    SpscRing<Item, 4> r;
    std::uint64_t admitted = 0, refused = 0;
    for (std::uint64_t i = 0; i < 20; ++i) {
        if (r.try_push(Item::make(i))) ++admitted;
        else ++refused;
    }
    // Four were admitted and sixteen were refused. The refusal is visible to
    // the producer, which is what lets admission control act on it; nothing was
    // accepted and then dropped.
    CHECK_EQ(admitted, std::uint64_t{4});
    CHECK_EQ(refused, std::uint64_t{16});
    for (std::uint64_t i = 0; i < 4; ++i) {
        Item out;
        CHECK(r.try_pop(out));
        CHECK_EQ(out.seq, i);
    }
    CHECK(r.empty());
}

SCENARIO("LL-035", "P0", "Q,M",
         "a producer that stops with items pending leaves them drainable in order") {
    auto r = std::make_unique<SpscRing<Item, 64>>();
    for (std::uint64_t i = 0; i < 40; ++i) CHECK(r->try_push(Item::make(i)));
    // The producer is gone. Everything it published is still there: the ring
    // holds it until the consumer takes it, and anything the drain could not
    // deliver is covered by the P06 replay contract, not by the ring.
    std::uint64_t drained = 0;
    Item out;
    while (r->try_pop(out)) {
        CHECK_EQ(out.seq, drained);
        CHECK(out.intact());
        ++drained;
    }
    CHECK_EQ(drained, std::uint64_t{40});
}

SCENARIO("LL-036", "P0", "Q,M",
         "a stopped consumer gives the producer a bounded full response, never a spin") {
    SpscRing<Item, 4> r;
    while (r.try_push(Item::make(0))) {}
    // push_with_bounded_wait is the only waiting form offered, and it returns.
    // LI-07 and LI-22: an unbounded spin on a critical thread burns the core
    // the session thread needs and can starve the consumer it waits for.
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = push_with_bounded_wait(r, Item::make(1), 1000, 100);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(!ok);
    CHECK(elapsed < std::chrono::seconds(5));
}

SCENARIO("LL-037", "P0", "Q,M", "resetting a ring that still holds items is refused") {
    SpscRing<Item, 4> r;
    CHECK(r.try_push(Item::make(1)));
    CHECK(!r.reset_if_empty());
    // Still intact after the refused reset.
    CHECK_EQ(r.size(), std::size_t{1});
    Item out;
    CHECK(r.try_pop(out));
    CHECK_EQ(out.seq, std::uint64_t{1});
    CHECK(r.reset_if_empty());
    CHECK_EQ(r.head_counter(), std::uint64_t{0});
}

SCENARIO("LL-038", "P0", "Q,M", "a ring cannot be copied or moved between threads") {
    // LI-03 holds for the ring's whole lifetime, so the type refuses the
    // operations that would create a second consumer instead of detecting the
    // problem later.
    static_assert(!std::is_copy_constructible_v<SpscRing<Item, 4>>);
    static_assert(!std::is_move_constructible_v<SpscRing<Item, 4>>);
    static_assert(!std::is_copy_assignable_v<SpscRing<Item, 4>>);
    static_assert(!std::is_move_assignable_v<SpscRing<Item, 4>>);
    CHECK(true);
}

SCENARIO("LL-003", "P0", "C,R,D,Q,S",
         "a second producer is refused by the ownership guard, not served") {
    SpscRing<Item, 16> r;
    CHECK(r.try_push(Item::make(1)));  // this thread claims the producer role

    PushStatus from_other = PushStatus::Ok;
    std::thread intruder([&] { from_other = r.push(Item::make(2)); });
    intruder.join();

#ifdef LLX_NO_OWNERSHIP_GUARD
    TT_INCONCLUSIVE(
        "this build compiled out the ownership guard (LLX_NO_OWNERSHIP_GUARD), which is "
        "the measurement variant. The guard is exercised by the lab build");
#else
    CHECK_EQ(from_other, PushStatus::WrongProducer);
    CHECK_EQ(r.guard_violations(), std::uint64_t{1});
    // The refused push left no trace: one item in, one item out.
    CHECK_EQ(r.size(), std::size_t{1});
    Item out;
    CHECK(r.try_pop(out));
    CHECK_EQ(out.seq, std::uint64_t{1});
#endif
}

SCENARIO("LL-003b", "P0", "C,R,D,Q,S", "a second consumer is refused the same way") {
    SpscRing<Item, 16> r;
    CHECK(r.try_push(Item::make(7)));
    Item mine;
    CHECK(r.try_pop(mine));  // this thread claims the consumer role
    CHECK(r.try_push(Item::make(8)));

    PopStatus from_other = PopStatus::Ok;
    Item stolen;
    std::thread intruder([&] { from_other = r.pop(stolen); });
    intruder.join();

#ifdef LLX_NO_OWNERSHIP_GUARD
    TT_INCONCLUSIVE("ownership guard compiled out in this build variant");
#else
    CHECK_EQ(from_other, PopStatus::WrongConsumer);
    // The item is still queued for the real consumer.
    CHECK_EQ(r.size(), std::size_t{1});
    Item out;
    CHECK(r.try_pop(out));
    CHECK_EQ(out.seq, std::uint64_t{8});
#endif
}

SCENARIO("LL-040", "P0", "Q,M",
         "a corrupted occupancy halts the ring instead of being acted on") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("corruption seam is compiled only into the lab build");
#else
    SpscRing<Item, 4> r;
    CHECK(r.try_push(Item::make(1)));
    // Drive head far past tail: an occupancy larger than the capacity is
    // impossible by construction, so observing one means a counter was
    // corrupted.
    r.corrupt_head_for_test(100);
    CHECK(!r.invariant_ok());

    // Doc 21: SPSC invariant mismatch -> HALTED. Neither side continues with a
    // false occupancy.
    CHECK_EQ(r.push(Item::make(2)), PushStatus::Halted);
    CHECK(r.halted());
    Item out;
    CHECK_EQ(r.pop(out), PopStatus::Halted);
#endif
}

SCENARIO("LL-029", "P0", "Q,M",
         "a producer paused before the head release publishes nothing visible") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    fp::disarm_all();
    fp::reset_counters();
    auto r = std::make_unique<SpscRing<Item, 16>>();
    // Park the producer after it has written the slot but before it releases
    // the head. This is CW-04: the consumer must not observe the incomplete
    // publication.
    fp::arm(std::string(fp::point::kRingAfterSlotWrite), fp::Action::Pause);

    std::thread producer([&] { r->try_push(Item::make(42)); });

    // Wait for the producer to actually reach the boundary. Without this the
    // case could pass on a machine where the thread never ran, which doc 25
    // forbids: no verdict without the failpoint firing.
    bool parked = false;
    for (int i = 0; i < 20000000 && !parked; ++i) {
        parked = fp::fired(fp::point::kRingAfterSlotWrite) > 0;
        if (!parked) std::this_thread::yield();
    }

    // The slot is written but unpublished, so the ring is still empty from the
    // consumer's side, however many times it looks.
    bool observed = false;
    Item peek;
    for (int i = 0; i < 100000 && !observed; ++i) observed = r->try_pop(peek);
    const std::size_t size_while_parked = r->size();

    fp::release(std::string(fp::point::kRingAfterSlotWrite));
    // Join before asserting: a CHECK that throws here would unwind into a
    // joinable thread's destructor and terminate the whole suite.
    producer.join();

    Item out{};
    const bool got = r->try_pop(out);
    fp::disarm_all();

    CHECK(parked);
    CHECK(!observed);
    CHECK_EQ(size_while_parked, std::size_t{0});
    CHECK(got);
    CHECK_EQ(out.seq, std::uint64_t{42});
    CHECK(out.intact());
#endif
}

SCENARIO("LL-030", "P0", "Q,M",
         "a producer paused after the head release delivers the payload exactly once") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    fp::disarm_all();
    fp::reset_counters();
    auto r = std::make_unique<SpscRing<Item, 16>>();
    fp::arm(std::string(fp::point::kRingAfterHeadRelease), fp::Action::Pause);
    std::thread producer([&] { r->try_push(Item::make(77)); });

    // CW-05: the item is published and in memory only. The consumer sees it
    // once, and only once.
    Item out{};
    int seen = 0;
    for (int i = 0; i < 20000000 && seen == 0; ++i) {
        if (r->try_pop(out)) ++seen;
        else std::this_thread::yield();
    }
    Item again{};
    const bool second = r->try_pop(again);
    const bool fired = fp::fired(fp::point::kRingAfterHeadRelease) > 0;

    fp::release(std::string(fp::point::kRingAfterHeadRelease));
    producer.join();
    fp::disarm_all();

    CHECK(fired);
    CHECK_EQ(seen, 1);
    CHECK(!second);
    CHECK_EQ(out.seq, std::uint64_t{77});
    CHECK(out.intact());
#endif
}

SCENARIO("LL-028", "P0", "Q,M",
         "a consumer paused before the tail release stops the producer reusing the slot") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    fp::disarm_all();
    fp::reset_counters();
    SpscRing<Item, 2> r;
    CHECK(r.try_push(Item::make(1)));
    CHECK(r.try_push(Item::make(2)));
    CHECK(r.full());

    // Park the consumer after it copied the slot but before it released the
    // tail. LI-05: until the tail moves, the producer has no free slot.
    //
    // Every pop happens on the consumer thread and every push on this one.
    // That is not tidiness: the ring binds each role to the first thread that
    // uses it for the object's whole lifetime (LI-03), so a pop from the
    // producer thread is refused as WrongConsumer -- which is how an earlier
    // draft of this very case was caught.
    fp::arm(std::string(fp::point::kRingBeforeTailRelease), fp::Action::Pause);
    Item first{}, second{};
    bool got_first = false, got_second = false;
    std::thread consumer([&] {
        got_first = (r.pop(first) == PopStatus::Ok);
        for (int i = 0; i < 20000000 && !got_second; ++i) {
            got_second = (r.pop(second) == PopStatus::Ok);
            if (!got_second) std::this_thread::yield();
        }
    });

    // Wait until the consumer is genuinely parked at the boundary. Asserting
    // before that would make the case depend on thread start-up timing.
    bool parked = false;
    for (int i = 0; i < 20000000 && !parked; ++i) {
        parked = fp::fired(fp::point::kRingBeforeTailRelease) > 0;
        if (!parked) std::this_thread::yield();
    }

    // While it is parked the producer must see a full ring. If the tail had
    // been released early this push would succeed and would overwrite a slot
    // that is still being read.
    bool only_full = true;
    for (int i = 0; i < 10000; ++i)
        if (r.push(Item::make(3)) != PushStatus::Full) only_full = false;

    fp::release(std::string(fp::point::kRingBeforeTailRelease));
    // Now a slot frees up, so the refill succeeds. Generous bound because the
    // consumer's tail release is concurrent with this push and the suite may
    // be running alongside others.
    const bool refilled = push_with_bounded_wait(r, Item::make(3), 1000000, 1000000);
    consumer.join();
    fp::disarm_all();

    CHECK(parked);
    CHECK(only_full);
    CHECK(refilled);
    CHECK(got_first);
    CHECK_EQ(first.seq, std::uint64_t{1});
    CHECK(first.intact());
    // The second item was never disturbed by the producer's attempts.
    CHECK(got_second);
    CHECK_EQ(second.seq, std::uint64_t{2});
    CHECK(second.intact());
#endif
}

SCENARIO("LL-041", "P0", "Q,M,C",
         "the cache-line padding actually separates the producer and consumer counters") {
    // Doc 10: the padding is a performance tool and its presence is part of the
    // ABI, so it is asserted rather than assumed. This does not claim the
    // padding makes anything faster -- that is the benchmark's job.
    SpscRing<Item, 4> r;
    const auto head_addr = reinterpret_cast<std::uintptr_t>(&r);
    CHECK_EQ(head_addr % kCacheLineBytes, std::uintptr_t{0});
    CHECK(kCacheLineBytes >= 64);
    CHECK((kCacheLineBytes & (kCacheLineBytes - 1)) == 0);
    if (!cache_line_matches_toolchain()) {
        // Not a failure: it means this build's padding differs from what the
        // toolchain believes the hardware wants, and the manifest has to say so
        // before a false-sharing number from it is quoted.
        tt::note_state_hash("cache_line_mismatch:" + std::to_string(kCacheLineBytes) +
                            "/" + std::to_string(kStdInterferenceSize));
    }
}

SCENARIO_BLOCKED("LL-042", "P0", "Q,M,C",
                 "a relaxed-publication mutant must be caught by a portability test",
                 "FI-02 requires a weak memory model. x86_64 is TSO, so a missing release "
                 "fence is not observable here however many schedules are run, and TSAN is "
                 "unavailable on this mingw-w64 toolchain. Needs an ARM64 runner or a "
                 "model checker; reporting a pass from this host would be false evidence "
                 "(LI-30, doc 23 layer 3)");

SCENARIO_BLOCKED("LL-043", "P0", "Q,M,C",
                 "ThreadSanitizer reports no race on the ring under stress",
                 "mingw-w64 ships no libtsan, so -fsanitize=thread cannot link on this "
                 "host. The CMake option LLX_TSAN exists for a Linux runner. BLOCKED, not "
                 "passed (doc 28 Gate 1 requires sanitizer evidence in the supported "
                 "modes)");

int main(int argc, char** argv) { return tt::run_all(argc, argv, "spsc_ring"); }
