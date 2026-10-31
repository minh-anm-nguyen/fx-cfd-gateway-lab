#include "gw/execution_pipeline.hpp"

#include <chrono>
#include <utility>

namespace gw {

std::string_view to_string(SubmitStatus s) {
    switch (s) {
        case SubmitStatus::Accepted: return "ACCEPTED";
        case SubmitStatus::QueueFull: return "QUEUE_FULL";
        case SubmitStatus::PoolExhausted: return "POOL_EXHAUSTED";
        case SubmitStatus::WrongProducer: return "WRONG_PRODUCER";
        case SubmitStatus::RingHalted: return "RING_HALTED";
        case SubmitStatus::NotRunning: return "NOT_RUNNING";
    }
    return "SUBMIT_INVALID";
}

ExecutionPipeline::ExecutionPipeline(router::Router& rt, Options options)
    : rt_(rt),
      options_(options),
      pool_(options.pool_slabs, options.pool_slots_per_slab) {}

ExecutionPipeline::~ExecutionPipeline() { stop(); }

void ExecutionPipeline::start() {
    if (running_.exchange(true)) return;
    draining_ = false;
    consumer_ = std::thread([this] { consume(); });
}

void ExecutionPipeline::stop() {
    if (!running_.load()) {
        if (consumer_.joinable()) consumer_.join();
        return;
    }
    // Drain first, then stop. Joining while work is still queued would leave
    // `accepted` ahead of `processed` for reasons that have nothing to do with
    // a lost event, which is the only thing the accounting is meant to detect.
    draining_ = true;
    if (consumer_.joinable()) consumer_.join();
    running_ = false;
}

SubmitStatus ExecutionPipeline::submit(const router::CommandEnvelope& cmd) {
    if (!running_.load() || draining_.load()) return SubmitStatus::NotRunning;

    // The slot is taken before the ring, so a pool exhaustion is reported as
    // itself rather than as a full queue. The two have different remedies: one
    // is in-flight work, the other is memory.
    llx::PoolHandle handle{};
    const llx::LeaseStatus leased = pool_.lease(handle, cmd);
    if (leased != llx::LeaseStatus::Ok) {
        pool_exhausted_.fetch_add(1);
        return SubmitStatus::PoolExhausted;
    }

    AdmissionTicket ticket;
    ticket.command = handle;
    ticket.submitted_seq = submitted_seq_.fetch_add(1) + 1;

    const llx::PushStatus pushed = ring_.push(ticket);
    if (pushed == llx::PushStatus::Ok) {
        accepted_.fetch_add(1);
        return SubmitStatus::Accepted;
    }

    // Refused, so the slot goes back. `release_remote` rather than `release`
    // because the pool's owner is whichever thread drains it, and this is the
    // producer handing a slot back across that boundary.
    pool_.release_remote(handle);

    switch (pushed) {
        case llx::PushStatus::Full:
            queue_full_.fetch_add(1);
            return SubmitStatus::QueueFull;
        case llx::PushStatus::WrongProducer:
            // EI-42: a second producer is refused outright. The ring also
            // counts it, so a scenario can assert the guard fired rather than
            // trusting that it would have.
            return SubmitStatus::WrongProducer;
        case llx::PushStatus::Halted:
            return SubmitStatus::RingHalted;
        case llx::PushStatus::Ok:
            break;
    }
    return SubmitStatus::RingHalted;
}

void ExecutionPipeline::consume() {
    // This thread owns everything that is single-owner: the router, and through
    // it P03's reducer, P05's coordinator and P06's stores. Nothing below is
    // reachable from the producer.
    ring_.bind_consumer();

    while (true) {
        std::size_t drained = 0;
        AdmissionTicket ticket{};
        while (drained < options_.drain_batch && ring_.try_pop(ticket)) {
            if (const router::CommandEnvelope* cmd = pool_.get(ticket.command)) {
                // Copied out before the slot is released, so the router never
                // reads a slot the producer may have reused.
                const router::CommandEnvelope work = *cmd;
                const auto result = rt_.submit(work);
                if (result.is_ok()) {
                    admitted_.fetch_add(1);
                } else {
                    // A refusal is an answer. It is counted, not dropped: the
                    // client was told, and EI-45's accounting has to balance
                    // whichever way the router decided.
                    refused_.fetch_add(1);
                }
            } else {
                // A handle that will not resolve is a defect, not a refusal.
                // It still counts as answered so the imbalance shows up as a
                // router answer that never happened rather than as a lost
                // event, which is the more accurate complaint.
                refused_.fetch_add(1);
            }
            pool_.release_remote(ticket.command);
            processed_.fetch_add(1);
            ++drained;
        }

        // The rest of the gateway's loop, on the same thread and after the
        // admissions, so a burst of new orders cannot starve the dispatch of
        // the ones already accepted.
        rt_.dispatch_pending();
        rt_.pump_inbound();

        if (draining_.load() && ring_.empty() && drained == 0) break;
        if (drained == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    // Slots the producer handed back while this thread was busy.
    pool_.drain_remote_reclaims();
}

}  // namespace gw
