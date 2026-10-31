// Bounded single-producer / single-consumer ring.
//
// This is the queue of design doc 8. The contract, and the invariant each part
// of it serves:
//
//   * capacity is a power of two and at least 2            (LI-06, LL-025/026)
//   * exactly one producer and one consumer for the whole
//     lifetime of the object                               (LI-03, LL-003/038)
//   * T is trivially copyable and owns no external pointer
//     unless the lifetime contract is proven separately    (LI-08, doc 8)
//   * a slot is fully written before the head is published
//     with release, and is read before the tail is
//     published with release                               (LI-04, LI-05)
//   * a full ring never overwrites an unconsumed slot; the
//     producer is told and decides                         (LI-02, LL-022)
//   * try_push and try_pop always finish; neither spins    (LI-07, LL-036)
//   * counters are modular and masking cannot leave the
//     array                                                (LI-06, LL-027)
//
// Three things this class deliberately is not. It is not an MPMC queue, so a
// second producer is a contract violation and not a feature (doc 8). It is not
// durable: a slot that was never consumed is lost on restart, and the authority
// for what survives is P06, not this ring (doc 8, LI-15). And padding here is a
// performance tool only — the correctness comes from the acquire/release pairs,
// never from the separation of the cache lines (LI-34).
//
// Counter width is a template parameter purely as a test seam. Waiting for a
// 64-bit counter to wrap is not possible in a test, so LL-027 and FI-04
// instantiate the same code with an 8- or 16-bit counter and drive it across
// the wrap a thousand times. Production instantiations use std::uint64_t.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

#include "llx/failpoint.hpp"

namespace llx {

// Cache line size used for padding.
//
// Doc 8 warns that std::hardware_destructive_interference_size is
// implementation-defined and that "every CPU is 64 bytes" must not be
// hardcoded. GCC warns about the same constant from the other direction: its
// value varies with the compiler version and with -mtune, so using it in a
// layout that is part of an ABI makes the ABI depend on build flags. That
// collides with LI-43, which requires a cacheline or capacity change to be
// versioned and validated rather than to drift silently.
//
// So the padding uses a constant this project owns. It defaults to 64, is
// overridable at configure time with -DLLX_CACHE_LINE_BYTES=..., and travels
// into the run manifest. What the standard library reports is captured
// separately, for the manifest only, so a mismatch between the two is visible
// evidence rather than an invisible layout change.
#ifndef LLX_CACHE_LINE_BYTES
#define LLX_CACHE_LINE_BYTES 64
#endif
constexpr std::size_t kCacheLineBytes = LLX_CACHE_LINE_BYTES;
static_assert(kCacheLineBytes >= alignof(std::max_align_t) &&
                  (kCacheLineBytes & (kCacheLineBytes - 1)) == 0,
              "cache line size must be a power of two at least as large as max_align_t");

#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
constexpr std::size_t kStdInterferenceSize = std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
constexpr bool kStdInterferenceAvailable = true;
#else
constexpr std::size_t kStdInterferenceSize = 0;
constexpr bool kStdInterferenceAvailable = false;
#endif

// True when the padding this build compiled in matches what the toolchain
// believes the hardware wants. False is not an error, but the manifest says so
// and a false-sharing benchmark has to be re-run before its numbers are quoted
// on that target (doc 10 "padding phải đúng alignment và manifest").
constexpr bool cache_line_matches_toolchain() noexcept {
    return !kStdInterferenceAvailable || kStdInterferenceSize == kCacheLineBytes;
}

// Why a push was refused. `bool try_push` would be enough for the hot path, but
// doc 12 requires every refusal to carry a reason into metrics and audit, and
// LL-020 requires a caller that ignores a refusal to be caught rather than to
// lose the event silently.
enum class PushStatus : std::uint8_t {
    Ok,
    Full,              // bounded capacity reached; caller decides (LI-02)
    WrongProducer,     // ownership guard rejected the caller (LI-03)
    Halted,            // ring quarantined after an impossible occupancy
};

enum class PopStatus : std::uint8_t {
    Ok,
    Empty,
    WrongConsumer,
    Halted,
};

template <typename T, std::size_t N, typename Counter = std::uint64_t>
class SpscRing {
    static_assert(N >= 2, "capacity must be at least 2 (LL-026)");
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two (LL-025)");
    static_assert(std::is_unsigned_v<Counter>, "counter must be unsigned for modular wrap");
    // Full and empty are distinguished by the distance between the counters, so
    // the counter has to be able to represent a distance of N without aliasing
    // zero. Half the counter range is the safe bound and it is what makes the
    // narrow-counter wrap tests meaningful rather than a different algorithm.
    static_assert(N <= (static_cast<std::size_t>(std::numeric_limits<Counter>::max()) / 2),
                  "capacity must not exceed half the counter range (LI-06)");
    static_assert(std::is_trivially_copyable_v<T>,
                  "slot type must be trivially copyable (doc 8)");

public:
    using value_type = T;
    static constexpr std::size_t capacity_v = N;

    SpscRing() = default;

    // Neither copyable nor movable. LL-038: moving a bound ring between threads
    // would create a second consumer, so the type refuses to allow it at all
    // rather than detecting it later.
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    static constexpr std::size_t capacity() noexcept { return N; }

    // --- ownership guard ---------------------------------------------------
    //
    // LI-03 holds for the lifetime of the ring, so the first thread to push
    // claims the producer role and the first to pop claims the consumer role.
    // A different thread is then refused. The check costs one relaxed load and
    // a comparison; it is compiled in for lab builds and can be switched off
    // with LLX_NO_OWNERSHIP_GUARD for a measurement run, in which case the
    // benchmark manifest records that the guard was absent.
    bool bind_producer() noexcept { return claim(producer_id_); }
    bool bind_consumer() noexcept { return claim(consumer_id_); }
    std::uint64_t guard_violations() const noexcept {
        return guard_violations_.load(std::memory_order_relaxed);
    }

    // --- producer side -----------------------------------------------------

    PushStatus push(const T& value) noexcept {
        if (halted_.load(std::memory_order_relaxed)) return PushStatus::Halted;
#ifndef LLX_NO_OWNERSHIP_GUARD
        if (!claim(producer_id_)) {
            guard_violations_.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::WrongProducer;
        }
#endif
        const Counter h = head_.load(std::memory_order_relaxed);
        const Counter t = tail_.load(std::memory_order_acquire);
        const Counter used = static_cast<Counter>(h - t);
        if (used > static_cast<Counter>(N)) {
            // Impossible by construction. Reaching it means a counter was
            // corrupted, so the ring quarantines itself instead of continuing
            // with a false occupancy (LL-040, doc 21 "SPSC invariant mismatch
            // -> HALTED").
            halted_.store(true, std::memory_order_relaxed);
            return PushStatus::Halted;
        }
        if (used == static_cast<Counter>(N)) return PushStatus::Full;

        LLX_FP(fp::point::kRingBeforeSlotWrite);
        slots_[static_cast<std::size_t>(h) & (N - 1)] = value;
        LLX_FP(fp::point::kRingAfterSlotWrite);
        // Release: everything written to the slot above is visible to a
        // consumer that acquires this head (LI-04).
        head_.store(static_cast<Counter>(h + 1), std::memory_order_release);
        LLX_FP(fp::point::kRingAfterHeadRelease);
        return PushStatus::Ok;
    }

    // Doc 8 spells the API as try_push/try_pop; kept as the primary names with
    // the status-returning form available for callers that record a reason.
    bool try_push(const T& value) noexcept { return push(value) == PushStatus::Ok; }

    // --- consumer side -----------------------------------------------------

    PopStatus pop(T& out) noexcept {
        if (halted_.load(std::memory_order_relaxed)) return PopStatus::Halted;
#ifndef LLX_NO_OWNERSHIP_GUARD
        if (!claim(consumer_id_)) {
            guard_violations_.fetch_add(1, std::memory_order_relaxed);
            return PopStatus::WrongConsumer;
        }
#endif
        const Counter t = tail_.load(std::memory_order_relaxed);
        const Counter h = head_.load(std::memory_order_acquire);
        if (h == t) return PopStatus::Empty;
        if (static_cast<Counter>(h - t) > static_cast<Counter>(N)) {
            halted_.store(true, std::memory_order_relaxed);
            return PopStatus::Halted;
        }

        LLX_FP(fp::point::kRingAfterConsumerAcquire);
        out = slots_[static_cast<std::size_t>(t) & (N - 1)];
        LLX_FP(fp::point::kRingBeforeTailRelease);
        // Release only after the copy is complete, so the producer cannot
        // reuse the slot while it is still being read (LI-05).
        tail_.store(static_cast<Counter>(t + 1), std::memory_order_release);
        return PopStatus::Ok;
    }

    bool try_pop(T& out) noexcept { return pop(out) == PopStatus::Ok; }

    // --- observation -------------------------------------------------------
    //
    // Approximate for an outside observer, exact when called by the producer or
    // the consumer about its own side. Used for watermark decisions (doc 12),
    // which is why it is clamped: a watermark must never be computed from an
    // occupancy larger than the capacity.
    std::size_t size() const noexcept {
        const Counter h = head_.load(std::memory_order_acquire);
        const Counter t = tail_.load(std::memory_order_acquire);
        const auto used = static_cast<std::size_t>(static_cast<Counter>(h - t));
        return used > N ? N : used;
    }
    bool empty() const noexcept { return size() == 0; }
    bool full() const noexcept { return size() == N; }
    std::size_t free_slots() const noexcept { return N - size(); }

    bool halted() const noexcept { return halted_.load(std::memory_order_relaxed); }

    // True while the distance invariant holds.
    //
    // Reading head and tail as two separate loads can give a torn pair: a
    // third-party observer may read head, then have the consumer advance tail
    // past that value, and compute a modular difference far larger than the
    // capacity even though the invariant held at every instant. LL-043 found
    // exactly that -- an observer thread reporting a violation that never
    // happened.
    //
    // So the pair is read with a stability check: head, then tail, then head
    // again. If head did not move, tail was read while head held that value,
    // which makes the pair consistent and the comparison sound. If head keeps
    // moving the observation is inconclusive and the answer is true, because
    // reporting a violation from a read that could not be completed would be
    // worse than reporting nothing -- an unprovable violation would halt the
    // engine (doc 21).
    bool invariant_ok() const noexcept {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const Counter h1 = head_.load(std::memory_order_acquire);
            const Counter t = tail_.load(std::memory_order_acquire);
            const Counter h2 = head_.load(std::memory_order_acquire);
            if (h1 != h2) continue;
            return static_cast<Counter>(h1 - t) <= static_cast<Counter>(N);
        }
        return true;
    }

    Counter head_counter() const noexcept { return head_.load(std::memory_order_acquire); }
    Counter tail_counter() const noexcept { return tail_.load(std::memory_order_acquire); }

    // --- test seams --------------------------------------------------------
    //
    // Advancing both counters together leaves the ring empty but positions it
    // just below a wrap, which is how LL-027 and FI-04 reach the boundary of a
    // 64-bit counter without performing 2^64 operations. Refused unless the
    // ring is empty, because moving the counters under live data would
    // fabricate the very corruption the invariant is meant to catch.
    bool seek_counters_for_test(Counter position) noexcept {
        if (!empty()) return false;
        head_.store(position, std::memory_order_relaxed);
        tail_.store(position, std::memory_order_relaxed);
        return true;
    }

    // LL-037: resetting a ring that still holds items is refused. Draining is
    // the caller's job and the P06 replay contract is what covers anything the
    // drain could not deliver.
    bool reset_if_empty() noexcept {
        if (!empty()) return false;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        halted_.store(false, std::memory_order_relaxed);
        producer_id_.store(std::uintptr_t{0}, std::memory_order_relaxed);
        consumer_id_.store(std::uintptr_t{0}, std::memory_order_relaxed);
        return true;
    }

    // Used by LL-040 to simulate a corrupted counter and prove the engine
    // halts. Only compiled where failpoints are, so a release build has no way
    // to reach it.
#ifdef LLX_ENABLE_FAILPOINTS
    void corrupt_head_for_test(Counter delta) noexcept {
        head_.store(static_cast<Counter>(head_.load(std::memory_order_relaxed) + delta),
                    std::memory_order_relaxed);
    }
#endif

private:
    static std::uintptr_t this_thread_token() noexcept {
        // A stable non-zero token for the calling thread. Taken from the
        // address of a thread_local, which is unique per thread and cheaper to
        // compare than std::thread::id.
        static thread_local char anchor = 0;
        return reinterpret_cast<std::uintptr_t>(&anchor);
    }

    static bool claim(std::atomic<std::uintptr_t>& slot) noexcept {
        const std::uintptr_t me = this_thread_token();
        // Steady state is one relaxed load and a compare. The read-modify-write
        // happens only on the first call, when the role is still unclaimed, so
        // the guard does not put an atomic RMW on the hot path.
        const std::uintptr_t cur = slot.load(std::memory_order_relaxed);
        if (cur == me) return true;
        if (cur != 0) return false;
        std::uintptr_t expected = 0;
        if (slot.compare_exchange_strong(expected, me, std::memory_order_relaxed,
                                         std::memory_order_relaxed))
            return true;
        return expected == me;
    }

    // head_ and tail_ are on separate cache lines so the producer's store to
    // head_ does not invalidate the line the consumer reads tail_ from. This is
    // the false-sharing fix of doc 10; it changes performance only. The
    // acquire/release pairs above are what make the queue correct (LI-34).
    alignas(kCacheLineBytes) std::atomic<Counter> head_{0};
    alignas(kCacheLineBytes) std::atomic<Counter> tail_{0};
    alignas(kCacheLineBytes) std::atomic<std::uintptr_t> producer_id_{0};
    std::atomic<std::uintptr_t> consumer_id_{0};
    std::atomic<std::uint64_t> guard_violations_{0};
    std::atomic<bool> halted_{false};
    alignas(kCacheLineBytes) std::array<T, N> slots_{};
};

// Bounded spin-then-yield helper for a producer that wants to wait for space.
//
// Doc 8 forbids an unbounded spin on a critical thread (LI-07, LI-22): it burns
// a core the session thread needs and can starve the very consumer it waits
// for. So the wait is bounded and returns false on expiry, leaving the caller
// to apply an admission decision instead of looping forever.
template <typename Ring>
bool push_with_bounded_wait(Ring& ring, const typename Ring::value_type& v,
                            int spin_iterations, int yield_iterations) noexcept {
    for (int i = 0; i < spin_iterations; ++i) {
        if (ring.try_push(v)) return true;
    }
    for (int i = 0; i < yield_iterations; ++i) {
        if (ring.try_push(v)) return true;
        std::this_thread::yield();
    }
    return false;
}

}  // namespace llx
