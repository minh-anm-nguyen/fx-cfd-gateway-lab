// Deterministic scenario scheduler.
//
// Reference: design doc 5.1 and 11.3. Nothing in this project decides a
// cancel-versus-fill race by thread scheduling or wall-clock time; the outcome
// is a function of the canonical ordering key alone:
//
//     (due_us, priority_class, insertion_seq)
//
// priority_class exists so that a quote update at an instant is visible to the
// business events at that same instant. Everything that can race with
// everything else shares ONE class, which is what lets the matrix run
// FILL_BEFORE_CANCEL and CANCEL_BEFORE_FILL as two named fixtures that differ
// only in insertion order (H-01, H-02, H-04, H-05). Putting fills and cancels
// in separate classes would have made that swap a no-op and the two tests
// indistinguishable.
//
// Durability: each event carries an immutable event_id and is persisted before
// execution. On restart only unexecuted events are requeued, and an event that
// already ran stays executed, so a crash while marking an event cannot run it
// twice (H-10, H-17, W9).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "lp/clock.hpp"
#include "lp/domain.hpp"
#include "lp/quote_book.hpp"

namespace lpsim {

// Lower values run first at the same due time.
enum class PriorityClass : int {
    Quote = 0,     // quote installation and withdrawal
    Business = 1,  // fills, cancel/replace finalisation, expiry
    Fault = 2,     // fault triggers observe the state the business left
};

std::string_view to_string(PriorityClass c);

enum class EventKind {
    QuoteUpdate,
    InjectFill,
    AutoMatch,
    FinalizeCancel,
    FinalizeReplace,
    Expire,
    FaultTrigger,
};

std::string_view to_string(EventKind k);

PriorityClass class_of(EventKind k);

struct ScheduledEvent {
    std::string event_id;  // durable identity, assigned by the caller
    VirtualTimeUs due_us = 0;
    EventKind kind = EventKind::AutoMatch;
    std::uint64_t insertion = 0;  // assigned by the scheduler at enqueue

    // Payload. Only the fields relevant to the kind are populated.
    std::string order_id;
    std::string request_id;  // the ClOrdID of a cancel or replace request
    Units qty = 0;
    Ticks px_ticks = 0;
    std::uint64_t quote_version = 0;  // pinned at enqueue time (D-12)
    QuoteSlice quote;                 // QuoteUpdate only
    std::string fault_name;           // FaultTrigger only
    int fault_repeats = 1;            // bounded, never unlimited (H-18)

    PriorityClass klass() const { return class_of(kind); }
};

// Strict weak ordering on the canonical key. Two events can never compare
// equal: insertion_seq is unique within a scheduler instance.
struct EventOrder {
    bool operator()(const ScheduledEvent& a, const ScheduledEvent& b) const {
        if (a.due_us != b.due_us) return a.due_us < b.due_us;
        const int ca = static_cast<int>(a.klass());
        const int cb = static_cast<int>(b.klass());
        if (ca != cb) return ca < cb;
        return a.insertion < b.insertion;
    }
};

enum class EnqueueResult {
    Ok = 0,
    CapacityReached,   // bounded queue, reject rather than grow (I-17)
    DuplicateEventId,  // the same durable identity twice (H-10)
    AlreadyExecuted,   // requeueing a completed event is refused (H-20)
    BadEvent,          // empty event_id or a negative due time
};

std::string_view to_string(EnqueueResult r);

class Scheduler {
public:
    explicit Scheduler(std::size_t max_events = 100000) : max_events_(max_events) {}

    EnqueueResult enqueue(ScheduledEvent e);

    // Pop the next event whose due time is at or before upto_us. Returns false
    // when nothing is due. The event is NOT marked executed by this call: the
    // caller marks it only after the business transaction commits, which is
    // what makes W9 recoverable.
    bool pop_due(VirtualTimeUs upto_us, ScheduledEvent* out);

    // Peek the due time of the earliest pending event.
    bool next_due_us(VirtualTimeUs* out) const;

    void mark_executed(const std::string& event_id);
    bool was_executed(const std::string& event_id) const;

    // Put an event back after a transaction rolled back, so a store failure
    // does not silently drop a scheduled fill.
    EnqueueResult requeue(ScheduledEvent e);

    // Drop one pending event by identity, leaving every other event's relative
    // order untouched. Used when the durable record of an event could not be
    // written: an event that is queued but not persisted would run once and
    // then run again after a restart.
    bool remove(const std::string& event_id);

    std::size_t pending_count() const { return queue_.size(); }
    std::size_t executed_count() const { return executed_.size(); }

    // Persistence. pending() is the durable work list; restore() reinstalls it
    // together with the set of identities that already ran.
    std::vector<ScheduledEvent> pending() const;
    void restore(const std::vector<ScheduledEvent>& pending_events,
                 const std::vector<std::string>& executed_ids);

    void clear();

private:
    std::size_t max_events_;
    std::uint64_t next_insertion_ = 1;
    std::set<ScheduledEvent, EventOrder> queue_;
    std::unordered_set<std::string> known_ids_;
    std::unordered_set<std::string> executed_;
};

}  // namespace lpsim
