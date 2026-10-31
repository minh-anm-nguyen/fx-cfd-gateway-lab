// P07's queue, pool and thread topology, carrying the gateway's work.
//
// §2 gives P07 "Queue/pool/thread execution and measurement" with the contract
// `Submit/TryPush`, `BoundedResource`, `LatencySpan`. Those are primitives, not
// order semantics, and the distinction decided how this is built.
//
// `llx::Engine` is a complete engine of its own, and it is deliberately not
// used here. Its `InputEnvelope` is P07's own numeric order model -- integer
// OrderId, SymbolId, Qty, Px, its own ExecType -- and `step()` processes orders
// against it. Driving the gateway through that would make P07 a second
// implementation of what P03 and P05 already own, which is exactly what §2
// forbids: "P07 không skip P05/P06 để cải thiện latency". So what is used is
// what P07 is given to own: `SpscRing` for the bounded hand-off, `Pool` for
// fixed-size slot reuse, and nothing that decides what an order means.
//
// The topology is the one the single-owner rule forces. `router::Router`,
// `order_core`, the risk coordinator and the journal are all single-owner, so
// all of them live on the consumer thread and none is touched from anywhere
// else. The producer only mints a slot and publishes a handle.
//
//   producer thread              SpscRing              consumer thread
//   ---------------              --------              ---------------
//   lease a pool slot    -->   AdmissionTicket   -->   pop, resolve
//   copy the envelope          (handle only,           Router::submit
//   push the handle             24 bytes)              dispatch_pending
//   refuse explicitly                                  pump_inbound
//   when full                                          release the slot
//
// The ring carries a handle rather than the command because `SpscRing` requires
// a trivially copyable slot type and `CommandEnvelope` holds strings. That is
// not a workaround: a fixed-size pool with handle reuse is what bounds the
// memory, and bounding it is the point (LI-02).
//
// Two invariants are the reason this file has counters at all. EI-43: a full
// queue or an exhausted pool must produce an explicit refusal, never a silent
// drop. EI-45: accepted work equals work that finished plus work still in
// flight -- nothing may simply disappear.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#include "llx/pool.hpp"
#include "llx/spsc_ring.hpp"
#include "router/router.hpp"

namespace gw {

// What crosses the ring. A handle and the submission ordinal, so the consumer
// can detect a gap rather than infer one from counts.
struct AdmissionTicket {
    llx::PoolHandle command{};
    std::uint64_t submitted_seq = 0;
};
static_assert(std::is_trivially_copyable_v<AdmissionTicket>,
              "the ring's slot type has to be trivially copyable (P07 doc 8)");

// Why a submission was refused. Every value is a refusal the caller is told
// about; there is no code path that accepts work and loses it.
enum class SubmitStatus : std::uint8_t {
    Accepted,
    QueueFull,       // the ring is at capacity (EI-43)
    PoolExhausted,   // no free slot (EI-43)
    WrongProducer,   // a second thread tried to produce (EI-42)
    RingHalted,      // the ring quarantined itself after impossible occupancy
    NotRunning,      // stopped, or never started
};

std::string_view to_string(SubmitStatus s);

class ExecutionPipeline {
public:
    // Capacity is a compile-time power of two because `SpscRing` requires it,
    // and small because a bounded queue nobody can fill is not a bounded queue
    // that has been tested.
    static constexpr std::size_t kRingCapacity = 64;
    using Ring = llx::SpscRing<AdmissionTicket, kRingCapacity>;

    struct Options {
        // Events the consumer drains per wake. Bounded so one burst cannot
        // starve the dispatch and inbound pumps that follow it.
        std::size_t drain_batch = 16;
        // Slots for in-flight commands. Deliberately independent of the ring:
        // the ring bounds the hand-off, the pool bounds the memory.
        std::uint32_t pool_slabs = 1;
        std::uint32_t pool_slots_per_slab = 128;
    };

    // `rt` is touched only from the consumer thread, and must outlive this.
    //
    // Options has no default argument: a nested aggregate's own member
    // initializers are not available to one inside the same enclosing class,
    // and spelling `{}` at the call site is clearer than a second constructor.
    ExecutionPipeline(router::Router& rt, Options options);
    ~ExecutionPipeline();

    ExecutionPipeline(const ExecutionPipeline&) = delete;
    ExecutionPipeline& operator=(const ExecutionPipeline&) = delete;

    void start();
    // Drains what is already queued, then joins. Returning before the ring is
    // empty would make the accounting below unverifiable.
    void stop();

    // Producer thread. The first thread to call this claims the producer role
    // for the lifetime of the ring; a second one is refused rather than
    // silently corrupting the queue (EI-42).
    SubmitStatus submit(const router::CommandEnvelope& cmd);

    // --- accounting (EI-45) -------------------------------------------------
    [[nodiscard]] std::uint64_t accepted() const { return accepted_.load(); }
    [[nodiscard]] std::uint64_t processed() const { return processed_.load(); }
    [[nodiscard]] std::uint64_t admitted_by_router() const { return admitted_.load(); }
    [[nodiscard]] std::uint64_t refused_by_router() const { return refused_.load(); }
    [[nodiscard]] std::uint64_t rejected_queue_full() const { return queue_full_.load(); }
    [[nodiscard]] std::uint64_t rejected_pool_exhausted() const { return pool_exhausted_.load(); }
    [[nodiscard]] std::uint64_t guard_violations() const { return ring_.guard_violations(); }

    // Everything the producer was told was accepted has either been processed
    // or is still in the ring. The only way this can fail is a lost event,
    // which is the thing EI-45 exists to catch.
    [[nodiscard]] bool accounting_balances() const {
        return accepted() == processed() + ring_.size();
    }
    // Every processed command got an answer from the router, one way or other.
    [[nodiscard]] bool every_processed_command_was_answered() const {
        return processed() == admitted_by_router() + refused_by_router();
    }

    [[nodiscard]] std::size_t queue_depth() const { return ring_.size(); }

private:
    void consume();

    router::Router& rt_;
    Options options_;

    Ring ring_;
    llx::Pool<router::CommandEnvelope> pool_;

    std::thread consumer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> draining_{false};

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> admitted_{0};
    std::atomic<std::uint64_t> refused_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> pool_exhausted_{0};
    std::atomic<std::uint64_t> submitted_seq_{0};
};

}  // namespace gw
