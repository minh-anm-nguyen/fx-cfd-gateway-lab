// Deterministic scheduling and the virtual clock.
//
// Matrix coverage: H-01 to H-05 (same-timestamp races), H-10, H-11, H-13 to
// H-18, plus the bounded-queue half of I-17.
//
// The property under test is narrow and strong: the outcome of a race is a
// function of (due_us, priority_class, insertion_seq) and of nothing else. If
// any of these tests could be made to pass or fail by thread timing, the
// cancel-versus-fill tests elsewhere in this suite would be meaningless.

#include <string>
#include <vector>

#include "lp/clock.hpp"
#include "lp/scenario.hpp"
#include "lp/scheduler.hpp"
#include "../support/tiny_test.hpp"

using namespace lpsim;

namespace {

ScheduledEvent event(const std::string& id, VirtualTimeUs due, EventKind kind) {
    ScheduledEvent e;
    e.event_id = id;
    e.due_us = due;
    e.kind = kind;
    return e;
}

// Drain the scheduler and return the identities in execution order.
std::vector<std::string> drain(Scheduler& s, VirtualTimeUs upto) {
    std::vector<std::string> order;
    ScheduledEvent e;
    while (s.pop_due(upto, &e)) order.push_back(e.event_id);
    return order;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

SCENARIO("H-01", "P0", "a fill and a cancel at the same instant are ordered by insertion") {
    // FILL_BEFORE_CANCEL
    {
        Scheduler s;
        CHECK_EQ(s.enqueue(event("fill", 100, EventKind::InjectFill)), EnqueueResult::Ok);
        CHECK_EQ(s.enqueue(event("cancel", 100, EventKind::FinalizeCancel)),
                 EnqueueResult::Ok);
        const auto order = drain(s, 100);
        CHECK_EQ(order.size(), static_cast<std::size_t>(2));
        CHECK_EQ(order[0], std::string("fill"));
        CHECK_EQ(order[1], std::string("cancel"));
    }
    // CANCEL_BEFORE_FILL: the same two events, swapped at enqueue. If fills and
    // cancels lived in different priority classes this swap would be a no-op
    // and the two named fixtures would be indistinguishable.
    {
        Scheduler s;
        CHECK_EQ(s.enqueue(event("cancel", 100, EventKind::FinalizeCancel)),
                 EnqueueResult::Ok);
        CHECK_EQ(s.enqueue(event("fill", 100, EventKind::InjectFill)), EnqueueResult::Ok);
        const auto order = drain(s, 100);
        CHECK_EQ(order[0], std::string("cancel"));
        CHECK_EQ(order[1], std::string("fill"));
    }
}

SCENARIO("H-02", "P0", "a fill and a replace at the same instant are ordered by insertion") {
    Scheduler a;
    CHECK_EQ(a.enqueue(event("fill", 50, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(a.enqueue(event("replace", 50, EventKind::FinalizeReplace)), EnqueueResult::Ok);
    CHECK_EQ(drain(a, 50)[0], std::string("fill"));

    Scheduler b;
    CHECK_EQ(b.enqueue(event("replace", 50, EventKind::FinalizeReplace)), EnqueueResult::Ok);
    CHECK_EQ(b.enqueue(event("fill", 50, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(drain(b, 50)[0], std::string("replace"));
}

SCENARIO("H-03", "P0", "a quote update at the same instant is visible before the fill") {
    // The quote class runs first regardless of insertion order, so a fill at an
    // instant always sees the book as of that instant (D-12).
    Scheduler s;
    CHECK_EQ(s.enqueue(event("fill", 100, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("quote", 100, EventKind::QuoteUpdate)), EnqueueResult::Ok);
    const auto order = drain(s, 100);
    CHECK_EQ(order[0], std::string("quote"));
    CHECK_EQ(order[1], std::string("fill"));

    // And the reverse insertion gives the same answer: the class decides.
    Scheduler t;
    CHECK_EQ(t.enqueue(event("quote", 100, EventKind::QuoteUpdate)), EnqueueResult::Ok);
    CHECK_EQ(t.enqueue(event("fill", 100, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(drain(t, 100)[0], std::string("quote"));
}

SCENARIO("H-04", "P0", "an expiry and a fill at the same instant are ordered by insertion") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("expire", 200, EventKind::Expire)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("fill", 200, EventKind::InjectFill)), EnqueueResult::Ok);
    const auto order = drain(s, 200);
    CHECK_EQ(order[0], std::string("expire"));
    CHECK_EQ(order[1], std::string("fill"));
}

SCENARIO("H-05", "P0", "a cancel and an expiry at the same instant are ordered by insertion") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("cancel", 300, EventKind::FinalizeCancel)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("expire", 300, EventKind::Expire)), EnqueueResult::Ok);
    const auto order = drain(s, 300);
    CHECK_EQ(order[0], std::string("cancel"));
    CHECK_EQ(order[1], std::string("expire"));
}

SCENARIO("H-15", "P0", "advancing past several deadlines runs every event once, in order") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("t300", 300, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("t100", 100, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("t200", 200, EventKind::InjectFill)), EnqueueResult::Ok);

    // Nothing is due yet.
    CHECK(drain(s, 50).empty());
    // One jump past all three deadlines.
    const auto order = drain(s, 1000);
    CHECK_EQ(order.size(), static_cast<std::size_t>(3));
    CHECK_EQ(order[0], std::string("t100"));
    CHECK_EQ(order[1], std::string("t200"));
    CHECK_EQ(order[2], std::string("t300"));
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(0));
}

SCENARIO("H-15b", "P0", "pop_due never returns an event before its due time") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("later", 1000, EventKind::InjectFill)), EnqueueResult::Ok);
    VirtualTimeUs due = 0;
    CHECK(s.next_due_us(&due));
    CHECK_EQ(due, static_cast<VirtualTimeUs>(1000));

    ScheduledEvent e;
    CHECK(!s.pop_due(999, &e));
    CHECK(s.pop_due(1000, &e));
    CHECK_EQ(e.event_id, std::string("later"));
}

// ---------------------------------------------------------------------------
// Durable identity
// ---------------------------------------------------------------------------

SCENARIO("H-10", "P0", "an executed event is never requeued") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("fill-1", 10, EventKind::InjectFill)), EnqueueResult::Ok);
    ScheduledEvent e;
    CHECK(s.pop_due(10, &e));
    s.mark_executed(e.event_id);
    CHECK(s.was_executed("fill-1"));

    // W9 recovery replays the pending list; an already executed identity is
    // refused rather than run a second time.
    CHECK_EQ(s.enqueue(event("fill-1", 10, EventKind::InjectFill)),
             EnqueueResult::AlreadyExecuted);
    CHECK_EQ(s.requeue(event("fill-1", 10, EventKind::InjectFill)),
             EnqueueResult::AlreadyExecuted);
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(0));
}

SCENARIO("H-10b", "P0", "the same event identity cannot be queued twice") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("fill-1", 10, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("fill-1", 20, EventKind::InjectFill)),
             EnqueueResult::DuplicateEventId);
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(1));
}

SCENARIO("H-10c", "P0", "a rolled-back event can be requeued with its identity intact") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("fill-1", 10, EventKind::InjectFill)), EnqueueResult::Ok);
    ScheduledEvent e;
    CHECK(s.pop_due(10, &e));
    // The transaction failed, so the event was never executed: it goes back.
    CHECK_EQ(s.requeue(e), EnqueueResult::Ok);
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(1));
    CHECK(!s.was_executed("fill-1"));
}

SCENARIO("H-20", "P0", "restore reinstates only the unexecuted events, in order") {
    std::vector<ScheduledEvent> pending = {event("a", 10, EventKind::InjectFill),
                                           event("b", 20, EventKind::InjectFill),
                                           event("c", 30, EventKind::InjectFill)};
    Scheduler s;
    s.restore(pending, {"b"});
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(2));
    CHECK(s.was_executed("b"));

    const auto order = drain(s, 1000);
    CHECK_EQ(order.size(), static_cast<std::size_t>(2));
    CHECK_EQ(order[0], std::string("a"));
    CHECK_EQ(order[1], std::string("c"));
}

SCENARIO("H-11", "P0", "restore preserves the relative order of same-instant events") {
    // PB-16: a restart must not reorder two events that were due together, or
    // the logical outcome could differ from the no-crash reference run.
    std::vector<ScheduledEvent> pending = {event("first", 100, EventKind::InjectFill),
                                           event("second", 100, EventKind::FinalizeCancel)};
    Scheduler s;
    s.restore(pending, {});
    const auto order = drain(s, 100);
    CHECK_EQ(order[0], std::string("first"));
    CHECK_EQ(order[1], std::string("second"));

    // The reverse stored order restores the reverse execution order.
    std::vector<ScheduledEvent> swapped = {event("second", 100, EventKind::FinalizeCancel),
                                           event("first", 100, EventKind::InjectFill)};
    Scheduler t;
    t.restore(swapped, {});
    CHECK_EQ(drain(t, 100)[0], std::string("second"));
}

// ---------------------------------------------------------------------------
// Bounds and malformed events
// ---------------------------------------------------------------------------

SCENARIO("I-17s", "P0", "the queue is bounded and refuses rather than growing") {
    Scheduler s(3);
    CHECK_EQ(s.enqueue(event("a", 1, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("b", 2, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("c", 3, EventKind::InjectFill)), EnqueueResult::Ok);
    CHECK_EQ(s.enqueue(event("d", 4, EventKind::InjectFill)), EnqueueResult::CapacityReached);
    // The three already accepted are untouched: a full queue does not drop work
    // it has already committed to (I-17).
    CHECK_EQ(s.pending_count(), static_cast<std::size_t>(3));
}

SCENARIO("H-18", "P1", "an event with no identity or an unbounded repeat count is refused") {
    Scheduler s;
    CHECK_EQ(s.enqueue(event("", 10, EventKind::InjectFill)), EnqueueResult::BadEvent);
    ScheduledEvent negative = event("neg", -1, EventKind::InjectFill);
    CHECK_EQ(s.enqueue(negative), EnqueueResult::BadEvent);

    // A fault trigger must declare a finite number of firings, so a scenario
    // cannot describe an infinite kill loop (H-17, H-18).
    ScheduledEvent unbounded = event("fault", 10, EventKind::FaultTrigger);
    unbounded.fault_repeats = 0;
    CHECK_EQ(s.enqueue(unbounded), EnqueueResult::BadEvent);
    unbounded.fault_repeats = 1;
    CHECK_EQ(s.enqueue(unbounded), EnqueueResult::Ok);
}

SCENARIO("H-16", "P1", "the pending list survives a round trip through persistence") {
    Scheduler s;
    ScheduledEvent fill = event("f1", 100, EventKind::InjectFill);
    fill.order_id = "SIM-1001";
    fill.qty = 40000;
    fill.px_ticks = 108490;
    fill.quote_version = 7;
    CHECK_EQ(s.enqueue(fill), EnqueueResult::Ok);

    const std::vector<ScheduledEvent> saved = s.pending();
    CHECK_EQ(saved.size(), static_cast<std::size_t>(1));
    CHECK_EQ(saved[0].order_id, std::string("SIM-1001"));
    CHECK_EQ(saved[0].qty, static_cast<Units>(40000));
    CHECK_EQ(saved[0].px_ticks, static_cast<Ticks>(108490));
    // The pinned quote version travels with the event, so a restart cannot
    // match it against a different version (D-12).
    CHECK_EQ(saved[0].quote_version, static_cast<std::uint64_t>(7));
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

SCENARIO("H-13", "P1", "a wall-clock step backwards leaves monotonic time alone") {
    VirtualClock c;
    const SteadyTime before_steady = c.steady_now();
    const VirtualTimeUs before_virtual = c.virtual_now_us();
    const WallTime before_wall = c.wall_now();

    c.jump_wall(std::chrono::seconds(-3600));

    // Liveness detection and scheduling are unaffected by an NTP step, which is
    // what makes them testable independently of clock discipline.
    CHECK(c.steady_now() == before_steady);
    CHECK_EQ(c.virtual_now_us(), before_virtual);
    CHECK(c.wall_now() < before_wall);
}

SCENARIO("H-14", "P1", "a wall-clock step forwards does not advance scheduling") {
    VirtualClock c;
    const VirtualTimeUs before = c.virtual_now_us();
    const std::string day_before = business_day_utc(c.wall_now());

    c.jump_wall(std::chrono::seconds(86400));

    CHECK_EQ(c.virtual_now_us(), before);
    // The business day derived from the wall clock does move, which is exactly
    // why a rollover has to be an explicit event rather than a side effect
    // (design doc 12.2): the identity namespace must not change silently.
    CHECK_NE(business_day_utc(c.wall_now()), day_before);
}

SCENARIO("H-16b", "P1", "monotonic time can advance without the scheduler ordinate") {
    VirtualClock c;
    const VirtualTimeUs before = c.virtual_now_us();
    c.advance_steady(Millis(1000));  // CPU starvation
    CHECK_EQ(c.virtual_now_us(), before);
    CHECK(c.steady_now() > SteadyTime{});
}

SCENARIO("H-15c", "P0", "setting virtual time forwards moves all three clocks together") {
    VirtualClock c;
    const WallTime w0 = c.wall_now();
    c.set_virtual_us(5000000);  // five seconds
    CHECK_EQ(c.virtual_now_us(), static_cast<VirtualTimeUs>(5000000));
    CHECK_EQ(std::chrono::duration_cast<std::chrono::seconds>(c.wall_now() - w0).count(),
             static_cast<long long>(5));

    // It never moves backwards, so a scenario step out of order cannot rewind
    // the timeline.
    c.set_virtual_us(1000);
    CHECK_EQ(c.virtual_now_us(), static_cast<VirtualTimeUs>(5000000));
}

SCENARIO("M-13", "P1", "the clock reports its own mode so a mismatch fails fast") {
    VirtualClock v;
    SystemClock w;
    CHECK_EQ(v.mode(), ClockMode::Virtual);
    CHECK_EQ(w.mode(), ClockMode::Wall);
    // Comparing latency measured under the two modes is invalid, so the mode is
    // part of what a run reports rather than an implicit property.
    CHECK_NE(std::string(to_string(v.mode())), std::string(to_string(w.mode())));
}

SCENARIO("H-11b", "P0", "a canonical trace hash is stable and sensitive") {
    const std::vector<std::string> a = {"NEW O1", "FILL 40000", "CANCEL C1"};
    const std::vector<std::string> b = {"NEW O1", "FILL 40000", "CANCEL C1"};
    const std::vector<std::string> c = {"NEW O1", "CANCEL C1", "FILL 40000"};
    CHECK_EQ(trace_hash(a), trace_hash(b));
    // Reordering the log changes the hash, which is what makes it useful as a
    // determinism check (H-11, M-05).
    CHECK_NE(trace_hash(a), trace_hash(c));
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "scheduler"); }
