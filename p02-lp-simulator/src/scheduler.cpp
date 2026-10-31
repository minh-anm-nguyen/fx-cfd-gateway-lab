#include "lp/scheduler.hpp"

#include "lp/failpoint.hpp"

namespace lpsim {

std::string_view to_string(PriorityClass c) {
    switch (c) {
        case PriorityClass::Quote: return "QUOTE";
        case PriorityClass::Business: return "BUSINESS";
        case PriorityClass::Fault: return "FAULT";
    }
    return "?";
}

std::string_view to_string(EventKind k) {
    switch (k) {
        case EventKind::QuoteUpdate: return "QUOTE_UPDATE";
        case EventKind::InjectFill: return "INJECT_FILL";
        case EventKind::AutoMatch: return "AUTO_MATCH";
        case EventKind::FinalizeCancel: return "FINALIZE_CANCEL";
        case EventKind::FinalizeReplace: return "FINALIZE_REPLACE";
        case EventKind::Expire: return "EXPIRE";
        case EventKind::FaultTrigger: return "FAULT_TRIGGER";
    }
    return "?";
}

PriorityClass class_of(EventKind k) {
    switch (k) {
        case EventKind::QuoteUpdate:
            return PriorityClass::Quote;
        case EventKind::FaultTrigger:
            return PriorityClass::Fault;
        // Everything that can race with everything else shares one class, so
        // that insertion order is the only tie-break (see the header).
        case EventKind::InjectFill:
        case EventKind::AutoMatch:
        case EventKind::FinalizeCancel:
        case EventKind::FinalizeReplace:
        case EventKind::Expire:
            return PriorityClass::Business;
    }
    return PriorityClass::Business;
}

std::string_view to_string(EnqueueResult r) {
    switch (r) {
        case EnqueueResult::Ok: return "Ok";
        case EnqueueResult::CapacityReached: return "CapacityReached";
        case EnqueueResult::DuplicateEventId: return "DuplicateEventId";
        case EnqueueResult::AlreadyExecuted: return "AlreadyExecuted";
        case EnqueueResult::BadEvent: return "BadEvent";
    }
    return "Unknown";
}

EnqueueResult Scheduler::enqueue(ScheduledEvent e) {
    if (e.event_id.empty() || e.due_us < 0) return EnqueueResult::BadEvent;
    if (e.fault_repeats < 1) return EnqueueResult::BadEvent;  // H-18
    if (executed_.find(e.event_id) != executed_.end()) return EnqueueResult::AlreadyExecuted;
    if (known_ids_.find(e.event_id) != known_ids_.end()) return EnqueueResult::DuplicateEventId;
    if (queue_.size() >= max_events_) return EnqueueResult::CapacityReached;

    e.insertion = next_insertion_++;
    known_ids_.insert(e.event_id);
    queue_.insert(std::move(e));
    return EnqueueResult::Ok;
}

EnqueueResult Scheduler::requeue(ScheduledEvent e) {
    // A rolled-back event keeps its durable identity, so the duplicate check
    // has to be relaxed for exactly this path.
    if (e.event_id.empty() || e.due_us < 0) return EnqueueResult::BadEvent;
    if (executed_.find(e.event_id) != executed_.end()) return EnqueueResult::AlreadyExecuted;
    if (queue_.size() >= max_events_) return EnqueueResult::CapacityReached;
    e.insertion = next_insertion_++;
    known_ids_.insert(e.event_id);
    queue_.insert(std::move(e));
    return EnqueueResult::Ok;
}

bool Scheduler::pop_due(VirtualTimeUs upto_us, ScheduledEvent* out) {
    if (queue_.empty()) return false;
    const auto it = queue_.begin();
    if (it->due_us > upto_us) return false;
    *out = *it;
    queue_.erase(it);
    known_ids_.erase(out->event_id);
    return true;
}

bool Scheduler::remove(const std::string& event_id) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->event_id == event_id) {
            queue_.erase(it);
            known_ids_.erase(event_id);
            return true;
        }
    }
    return false;
}

bool Scheduler::next_due_us(VirtualTimeUs* out) const {
    if (queue_.empty()) return false;
    *out = queue_.begin()->due_us;
    return true;
}

void Scheduler::mark_executed(const std::string& event_id) {
    // W9: a crash here leaves the event unmarked, so recovery requeues it and
    // the idempotency check in the business layer stops a second effect.
    LPSIM_FP(fp::point::kSchedulerMarkExecuted);
    executed_.insert(event_id);
}

bool Scheduler::was_executed(const std::string& event_id) const {
    return executed_.find(event_id) != executed_.end();
}

std::vector<ScheduledEvent> Scheduler::pending() const {
    std::vector<ScheduledEvent> out;
    out.reserve(queue_.size());
    for (const ScheduledEvent& e : queue_) out.push_back(e);
    return out;
}

void Scheduler::restore(const std::vector<ScheduledEvent>& pending_events,
                        const std::vector<std::string>& executed_ids) {
    clear();
    for (const std::string& id : executed_ids) executed_.insert(id);
    // Reinstalling in stored order preserves the original relative insertion
    // sequence, so a restart does not reorder two events that were due at the
    // same instant (H-10, PB-16).
    for (const ScheduledEvent& e : pending_events) {
        if (executed_.find(e.event_id) != executed_.end()) continue;
        ScheduledEvent copy = e;
        copy.insertion = next_insertion_++;
        known_ids_.insert(copy.event_id);
        queue_.insert(std::move(copy));
    }
}

void Scheduler::clear() {
    queue_.clear();
    known_ids_.clear();
    executed_.clear();
    next_insertion_ = 1;
}

}  // namespace lpsim
