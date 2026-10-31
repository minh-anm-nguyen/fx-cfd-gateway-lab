// Fixed-size object pool with generation-tagged handles.
//
// Design doc 9. The pool exists so the hot path performs no allocation once it
// is warm (LI-11), and so a payload too large for a ring slot can be handed
// across a queue as a handle rather than as a pointer whose lifetime nobody
// owns (LI-08).
//
// Handle layout is the one doc 18 specifies: (epoch, slab, slot, generation).
//   epoch       identifies the pool instance, so a handle minted before a
//               reconfigure cannot be used after it (LI-47)
//   slab/slot   locate the object without a pointer, so a business identity is
//               never a memory address (LI-12)
//   generation  changes on every lease, so a handle that outlived its object is
//               rejected instead of reading whatever now occupies the slot.
//               This is the ABA case of FI-06: lease X, hand it to an async
//               commit, free and re-lease X for another order, then deliver the
//               original callback.
//
// Generation wrap. Doc 9 requires either a generation wide enough to prove
// safety or slot retirement. Both are implemented: the generation is 32 bits,
// and when a slot's generation would wrap the slot is retired and never leased
// again, so an old handle can never alias a new object. Retirement is counted
// and reported. The width is a template parameter purely so a test can exercise
// the wrap with an 8-bit generation instead of performing 2^32 leases.
//
// Exhaustion is a refusal, never a fallback. Doc 9 and LI-10 forbid dropping
// back to malloc on the hot path, because that converts a bounded, visible
// backpressure signal into an unbounded heap and an eventual OOM.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "llx/failpoint.hpp"
#include "llx/spsc_ring.hpp"

namespace llx {

// A handle is 16 bytes and trivially copyable, so it can travel in a ring slot.
struct PoolHandle {
    std::uint32_t epoch = 0;
    std::uint32_t slab = 0;
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;  // 0 is the null handle

    bool is_null() const noexcept { return generation == 0; }
    friend bool operator==(const PoolHandle& a, const PoolHandle& b) noexcept {
        return a.epoch == b.epoch && a.slab == b.slab && a.slot == b.slot &&
               a.generation == b.generation;
    }
};
static_assert(std::is_trivially_copyable_v<PoolHandle>);

enum class LeaseStatus : std::uint8_t {
    Ok,
    Exhausted,   // POOL_EXHAUSTED: bounded refusal, no heap fallback (LI-10)
    Halted,
};

enum class ReleaseStatus : std::uint8_t {
    Ok,
    OutOfRange,       // handle does not address this pool
    WrongEpoch,       // minted against a previous pool instance (LI-47)
    StaleGeneration,  // the object was already freed and the slot re-leased
    DoubleFree,       // the same handle released twice (LI-09, FI-07)
};

enum class ResolveStatus : std::uint8_t {
    Ok,
    OutOfRange,
    WrongEpoch,
    StaleGeneration,
    NotLive,
};

struct PoolStats {
    std::size_t capacity = 0;
    std::size_t live = 0;
    std::size_t free_slots = 0;
    std::size_t retired = 0;
    std::uint64_t leases = 0;
    std::uint64_t releases = 0;
    std::uint64_t exhaustions = 0;
    std::uint64_t stale_rejections = 0;
    std::uint64_t double_free_rejections = 0;
    std::uint64_t wrong_epoch_rejections = 0;
    std::uint64_t remote_reclaims = 0;
};

// Threading contract, which doc 9 implies and which is stated here because
// getting it wrong is a data race rather than a visible failure:
//
//   lease(), release(), drain_remote_reclaims()
//       owner thread only. These touch the free list and the slot lifecycle.
//   get(), resolve_status()
//       safe from any thread that holds a handle. This is required, not a
//       convenience: doc 9 has handles travelling through a queue, with
//       ownership transferring at the release/acquire boundary, so the
//       consumer resolves a handle the producer minted. The slot's generation
//       and liveness are therefore atomic, which is what makes a *stale*
//       resolution -- the ABA case of FI-06 -- report StaleGeneration safely
//       instead of racing with the re-lease that made it stale.
//   release_remote()
//       any single non-owner thread, queued for the owner to apply.
//
// The counters a resolution touches are atomic for the same reason. The ones
// only the owner touches (leases, releases, exhaustions) are not, because
// making them atomic would put a contended write on the lease path for no
// benefit.

template <typename T, typename Gen = std::uint32_t>
class Pool {
    static_assert(std::is_unsigned_v<Gen>);

public:
    static constexpr Gen kMaxGeneration = std::numeric_limits<Gen>::max();

    // All storage is taken here, once, before the engine starts admitting work.
    // slabs are a flat partition of the same allocation; they exist so a handle
    // stays small and so a future NUMA-aware build can bind one slab per node
    // without changing the handle layout (doc 9, doc 11 step 4).
    Pool(std::uint32_t slab_count, std::uint32_t slots_per_slab, std::uint32_t epoch = 1)
        : slab_count_(slab_count), slots_per_slab_(slots_per_slab), epoch_(epoch),
          reclaim_(std::make_unique<ReclaimRing>()) {
        const std::size_t total = static_cast<std::size_t>(slab_count) * slots_per_slab;
        storage_ = std::make_unique<Slot[]>(total);
        free_list_.reserve(total);
        // Highest index first, so the first leases come from index 0 upward and
        // a trace is easier to read.
        for (std::size_t i = total; i > 0; --i)
            free_list_.push_back(static_cast<std::uint32_t>(i - 1));
    }

    ~Pool() {
        // Teardown destroys whatever is still live rather than leaking it, and
        // the live count is observable first so a test can assert the drain
        // happened (doc 9 "teardown drains/refcounts").
        for (std::size_t i = 0; i < total_slots(); ++i) {
            T* obj = storage_[i].live_ptr.load(std::memory_order_relaxed);
            if (obj != nullptr) {
                std::destroy_at(obj);
                storage_[i].live_ptr.store(nullptr, std::memory_order_relaxed);
            }
        }
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    std::size_t total_slots() const noexcept {
        return static_cast<std::size_t>(slab_count_) * slots_per_slab_;
    }
    std::uint32_t epoch() const noexcept { return epoch_; }

    // Writes every page of the pool once so the first real lease does not take
    // a page fault inside a measured region. Doc 15.2 step 3 requires this
    // before a benchmark, and doc 9 calls it first-touch.
    void prewarm() noexcept {
        for (std::size_t i = 0; i < total_slots(); ++i) {
            // Only free slots: writing raw bytes over a live object would
            // corrupt it. prewarm is meant to run before any admission anyway.
            if (storage_[i].live_ptr.load(std::memory_order_relaxed) != nullptr) continue;
            volatile unsigned char* p =
                reinterpret_cast<volatile unsigned char*>(storage_[i].bytes);
            constexpr std::size_t kBytes = sizeof(Slot::bytes);
            for (std::size_t off = 0; off < kBytes; off += 512) p[off] = 0;
            p[kBytes - 1] = 0;
        }
        prewarmed_ = true;
    }
    bool prewarmed() const noexcept { return prewarmed_; }

    // --- lease / release ---------------------------------------------------

    template <typename... Args>
    LeaseStatus lease(PoolHandle& out, Args&&... args) {
        drain_remote_reclaims();
        if (LLX_FP(fp::point::kPoolExhaust) == fp::Action::ReturnError) {
            ++stats_.exhaustions;
            return LeaseStatus::Exhausted;
        }
        // Take the first slot that is not at the end of its generation range.
        // Retiring rather than wrapping is what makes the handle check sound: a
        // wrapped generation would let a handle minted 2^N leases ago resolve
        // against a different object, which is the aliasing the generation
        // exists to prevent. The loop terminates because each iteration either
        // leases or permanently removes a slot from the free list.
        std::uint32_t idx = 0;
        Slot* found = nullptr;
        while (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
            Slot& cand = storage_[idx];
            if (cand.generation.load(std::memory_order_relaxed) == kMaxGeneration) {
                cand.retired = true;
                ++retired_;
                continue;
            }
            found = &cand;
            break;
        }
        if (found == nullptr) {
            // No heap fallback. The caller turns this into an admission
            // refusal with a reason (LI-10, doc 12).
            ++stats_.exhaustions;
            return LeaseStatus::Exhausted;
        }
        Slot& s = *found;

        // Generation 0 is the null handle, so it is skipped on wrap-around.
        Gen g = static_cast<Gen>(s.generation.load(std::memory_order_relaxed) + 1);
        if (g == 0) ++g;
        s.generation.store(g, std::memory_order_relaxed);

        T* obj = std::construct_at(reinterpret_cast<T*>(&s.bytes),
                                   std::forward<Args>(args)...);
        // Release: the constructed object must be visible to any thread that
        // later resolves this handle.
        s.live_ptr.store(obj, std::memory_order_release);
        out.epoch = epoch_;
        out.slab = idx / slots_per_slab_;
        out.slot = idx % slots_per_slab_;
        out.generation = g;
        ++live_;
        ++stats_.leases;
        LLX_FP(fp::point::kPoolAfterLease);
        return LeaseStatus::Ok;
    }

    ReleaseStatus release(const PoolHandle& h) {
        Slot* s = nullptr;
        const ResolveStatus rs = locate(h, s);
        switch (rs) {
            case ResolveStatus::OutOfRange: return ReleaseStatus::OutOfRange;
            // Stale and wrong-epoch are counted by locate(), which sees every
            // resolution attempt and not just the ones that came through a
            // release. Doc 9 asks for the metric on the resolution, because a
            // rising stale count is how an ABA problem announces itself.
            case ResolveStatus::WrongEpoch: return ReleaseStatus::WrongEpoch;
            case ResolveStatus::StaleGeneration: return ReleaseStatus::StaleGeneration;
            case ResolveStatus::NotLive:
                // The slot is free and the generation still matches, so this is
                // the same handle arriving a second time (FI-07). Counted here
                // rather than in locate(): resolving a freed slot is a normal
                // negative answer, whereas releasing one twice is a bug.
                ++stats_.double_free_rejections;
                return ReleaseStatus::DoubleFree;
            case ResolveStatus::Ok: break;
        }

        LLX_FP(fp::point::kPoolBeforeRelease);
        std::destroy_at(s->live_ptr.load(std::memory_order_relaxed));
        s->live_ptr.store(nullptr, std::memory_order_release);
        const std::size_t idx = index_of(h);
        if (!s->retired) free_list_.push_back(static_cast<std::uint32_t>(idx));
        --live_;
        ++stats_.releases;
        LLX_FP(fp::point::kPoolAfterRelease);
        return ReleaseStatus::Ok;
    }

    // Release from a thread that does not own the pool. Doc 9 forbids an
    // unsynchronized free list shared between threads, so a remote release is
    // queued and the owner performs the actual reclaim on its next lease or
    // drain. Returns false when the reclaim queue is full, which is bounded
    // backpressure rather than a leak: the caller retries.
    //
    // The reclaim queue is itself an SPSC ring, so there is exactly one remote
    // releasing thread. A second one is refused by the ring's ownership guard
    // and sees false here, which is a detected refusal rather than a corrupted
    // free list. A topology that needs more than one remote releaser needs a
    // real MPSC adapter, which doc 6 puts outside the v1 optimization scope.
    bool release_remote(const PoolHandle& h) noexcept {
        return reclaim_->try_push(h);
    }

    std::size_t drain_remote_reclaims() {
        std::size_t n = 0;
        PoolHandle h;
        while (reclaim_->try_pop(h)) {
            release(h);
            ++stats_.remote_reclaims;
            ++n;
        }
        return n;
    }

    // --- resolution --------------------------------------------------------

    // Returns nullptr for any handle that is not currently valid. Every caller
    // must check: a stale handle reaching through to the object is the
    // use-after-free LI-08 forbids.
    T* get(const PoolHandle& h) const noexcept {
        Slot* s = nullptr;
        if (locate(h, s) != ResolveStatus::Ok) return nullptr;
        return s->live_ptr.load(std::memory_order_relaxed);
    }

    ResolveStatus resolve_status(const PoolHandle& h) const noexcept {
        Slot* s = nullptr;
        return locate(h, s);
    }

    // --- observation -------------------------------------------------------

    PoolStats stats() const noexcept {
        PoolStats s = stats_;
        s.capacity = total_slots();
        s.live = live_;
        s.free_slots = free_list_.size();
        s.retired = retired_;
        s.stale_rejections = stale_rejections_.load(std::memory_order_relaxed);
        s.wrong_epoch_rejections = wrong_epoch_rejections_.load(std::memory_order_relaxed);
        return s;
    }
    std::size_t live() const noexcept { return live_; }
    std::size_t free_slots() const noexcept { return free_list_.size(); }
    std::size_t retired() const noexcept { return retired_; }

    // Leak scan for the end of a test: every slot must be free and nothing may
    // still hold an object (doc 23 layer 1, LI-08).
    bool no_leaks() const noexcept { return live_ == 0; }

    // Test seam: force a slot's generation close to its maximum so the wrap and
    // retirement path can be reached in a few leases rather than 2^32.
    bool set_generation_for_test(std::uint32_t slab, std::uint32_t slot, Gen g) noexcept {
        const std::size_t idx = static_cast<std::size_t>(slab) * slots_per_slab_ + slot;
        if (idx >= total_slots() ||
            storage_[idx].live_ptr.load(std::memory_order_relaxed) != nullptr)
            return false;
        storage_[idx].generation.store(g, std::memory_order_relaxed);
        return true;
    }

private:
    // One slot: raw storage plus its lifecycle proof. live_ptr doubles as the
    // liveness flag and as the laundered pointer returned by construct_at, so
    // nothing in this class reinterprets storage that holds no object.
    struct Slot {
        alignas(T) unsigned char bytes[sizeof(T)];
        // Atomic because a consumer thread resolves handles concurrently with
        // the owner re-leasing slots. Relaxed is sufficient: the handle itself
        // was published through the queue's release/acquire pair, so the
        // payload is already visible, and all these two fields have to do is
        // give a coherent answer to "is this handle still the live one".
        std::atomic<T*> live_ptr{nullptr};
        std::atomic<Gen> generation{0};
        bool retired = false;  // owner only
    };

    using ReclaimRing = SpscRing<PoolHandle, 1024>;

    std::size_t index_of(const PoolHandle& h) const noexcept {
        return static_cast<std::size_t>(h.slab) * slots_per_slab_ + h.slot;
    }

    ResolveStatus locate(const PoolHandle& h, Slot*& out) const noexcept {
        if (h.is_null()) return ResolveStatus::OutOfRange;
        if (h.slab >= slab_count_ || h.slot >= slots_per_slab_) return ResolveStatus::OutOfRange;
        if (h.epoch != epoch_) {
            wrong_epoch_rejections_.fetch_add(1, std::memory_order_relaxed);
            return ResolveStatus::WrongEpoch;
        }
        Slot& s = storage_[index_of(h)];
        out = &s;
        if (s.generation.load(std::memory_order_relaxed) != h.generation) {
            stale_rejections_.fetch_add(1, std::memory_order_relaxed);
            return ResolveStatus::StaleGeneration;
        }
        if (s.live_ptr.load(std::memory_order_relaxed) == nullptr) return ResolveStatus::NotLive;
        return ResolveStatus::Ok;
    }

    std::uint32_t slab_count_;
    std::uint32_t slots_per_slab_;
    std::uint32_t epoch_;
    std::unique_ptr<Slot[]> storage_;
    std::vector<std::uint32_t> free_list_;
    std::unique_ptr<ReclaimRing> reclaim_;
    std::size_t live_ = 0;
    std::size_t retired_ = 0;
    bool prewarmed_ = false;
    PoolStats stats_{};
    // Touched by locate(), which runs on any thread holding a handle.
    mutable std::atomic<std::uint64_t> stale_rejections_{0};
    mutable std::atomic<std::uint64_t> wrong_epoch_rejections_{0};
};

}  // namespace llx
