// Restart recovery: what survives, what resumes and what refuses to resume.
//
// Matrix coverage: J-04 to J-06, A-09, A-18, I-15, I-16, C-07, C-14, D-17,
// D-18, H-10, H-20, and the recovery half of PB-01, PB-10 and PB-16.
//
// These cases restart the simulator against the SAME database, which is the
// in-process half of the crash story: the store is closed and reopened and the
// actor and session are rebuilt from what is on disk. The other half, where the
// process is actually killed at a named cutpoint, is test_crash.
//
// What every case here asserts is the same thing in different shapes: a restart
// changes the logical ledger not at all.

#include <string>
#include <vector>

#include "lp/failpoint.hpp"
#include "../support/harness.hpp"

using namespace lpsim;
using namespace tt;

namespace {

struct Live {
    LpFixture fx;
    std::vector<std::string> storage;

    explicit Live(const std::string& tag, FixtureOptions opt = {}) : fx(tag, std::move(opt)) {
        CHECK(fx.start());
        CHECK(fx.logon());
        CHECK_EQ(fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 1, 600000),
                 QuoteError::Ok);
    }

    const OrderChain* order(const std::string& id) {
        return fx.actor().find_by_cl_ord_id(id);
    }
    std::vector<DecodedMessage> of(std::string_view mt) {
        return fx.sent_of_type(mt, &storage);
    }

    // Restart and log the peer back on, the way a real reconnect does.
    void restart_and_relogon() {
        const std::uint64_t next_in = fx.session().next_in();
        CHECK(fx.restart());
        fx.connect();
        fx.peer().set_next_seq(next_in);
        const auto v = fx.feed(fx.peer().logon(30));
        CHECK_EQ(v[0], InboundVerdict::Accepted);
    }
};

}  // namespace

SCENARIO("A-09", "P0", "a restart preserves the order ledger and the sequence state") {
    Live L("rec-a09");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    const Units cum = L.order("O1")->cum_qty;
    const Ticks avg = L.order("O1")->avg_px_ticks;
    const std::uint64_t next_out = L.fx.session().next_out();
    const std::uint64_t epoch = L.fx.session().epoch();

    L.restart_and_relogon();

    // Nothing about the business state moved, and the session resumed rather
    // than resetting (ADR-09).
    CHECK(L.order("O1") != nullptr);
    CHECK_EQ(L.order("O1")->order_id, id);
    CHECK_EQ(L.order("O1")->cum_qty, cum);
    CHECK_EQ(L.order("O1")->avg_px_ticks, avg);
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
    CHECK_EQ(L.fx.session().epoch(), epoch);
    CHECK(L.fx.session().next_out() >= next_out);
}

SCENARIO("J-04", "P0", "a committed fill whose report was never sent is delivered once") {
    // PB-01: the fill and the outbox row committed, then the process ended
    // before TX-C. The restart must dispatch the report exactly once, as an
    // original, with the ledger unchanged.
    Live L("rec-j04");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // Block the peer so the trade report commits but is never written.
    L.fx.transport().stop_reading();
    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK(L.fx.store().count_outbox_pending() > 0);

    L.restart_and_relogon();

    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
    L.fx.session().dispatch_outbox();
    CHECK_EQ(L.fx.store().count_outbox_pending(), static_cast<std::size_t>(0));

    // Exactly one trade report reached the wire after the restart, and the
    // quote inventory was debited exactly once.
    int trades = 0;
    for (const DecodedMessage& m : L.of("8")) {
        if (field(m, tag::ExecType) == "F") ++trades;
    }
    CHECK_EQ(trades, 1);
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(60000));
    check_all(L.fx);
}

SCENARIO("J-06", "P0", "a report that already owns a sequence keeps it across a restart") {
    // PB-02 end to end: TX-C committed, the socket write never happened.
    Live L("rec-j06");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // The write fails, so the row stays SeqAssigned with its sequence bound.
    L.fx.transport().next_write_short(5);
    L.fx.inject_fill(id, 40000, "1.08490");

    std::vector<OutboxEntry> pending;
    CHECK_EQ(L.fx.store().pending_outbox(&pending), StoreError::None);
    std::map<std::string, std::uint64_t> bound;
    for (const OutboxEntry& e : pending) {
        if (e.outbound_seq > 0) bound[e.event_id] = e.outbound_seq;
    }
    CHECK(!bound.empty());

    CHECK(L.fx.restart());

    // The binding survived byte for byte.
    CHECK_EQ(L.fx.store().pending_outbox(&pending), StoreError::None);
    for (const OutboxEntry& e : pending) {
        const auto it = bound.find(e.event_id);
        if (it != bound.end()) CHECK_EQ(e.outbound_seq, it->second);
    }
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
}

SCENARIO("J-05", "P0", "a pending outbox row is dispatched once after a restart") {
    Live L("rec-j05");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.transport().stop_reading();
    L.fx.inject_fill(id, 40000, "1.08490");

    L.restart_and_relogon();
    L.fx.session().dispatch_outbox();
    const std::size_t after_first = L.of("8").size();

    // Dispatching repeatedly must not re-send: the rows are SENT now.
    for (int i = 0; i < 5; ++i) L.fx.session().dispatch_outbox();
    CHECK_EQ(L.of("8").size(), after_first);
    CHECK_EQ(L.fx.store().count_outbox_pending(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("I-16", "P0", "business de-duplication survives a restart") {
    // PB-10: the client resends O1 after the restart. One OrderID, one ledger.
    Live L("rec-i16");
    const std::string original = L.fx.peer().new_order(NewOrderArgs{});
    L.fx.feed(original);
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    L.restart_and_relogon();

    // An application resend of the same request with a fresh sequence number.
    L.fx.feed(L.fx.peer().as_poss_resend(original));

    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
    CHECK_EQ(L.order("O1")->order_id, id);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
    check_all(L.fx);
}

SCENARIO("D-18b", "P0", "consumed liquidity does not come back after a restart") {
    Live L("rec-d18");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(60000));

    CHECK(L.fx.restart());

    // The restored quote carries what was LEFT, not what it originally offered
    // (D-17, D-18).
    CHECK_EQ(L.fx.book().inventory_units("EURUSD", Side::Buy), static_cast<Units>(60000));
    const QuoteSlice* q = L.fx.book().current("EURUSD");
    CHECK(q != nullptr);
    CHECK_EQ(q->version, static_cast<std::uint64_t>(1));
}

SCENARIO("C-14b", "P0", "identifier allocators resume above everything already issued") {
    Live L("rec-c14");
    for (int i = 1; i <= 5; ++i) {
        NewOrderArgs a;
        a.cl_ord_id = "O" + std::to_string(i);
        a.qty = "1000";
        L.fx.feed(L.fx.peer().new_order(a));
    }
    std::vector<std::string> before;
    for (const OrderChain* c : L.fx.actor().chains()) before.push_back(c->order_id);

    L.restart_and_relogon();

    // A new order after the restart must not reuse any earlier OrderID.
    NewOrderArgs fresh;
    fresh.cl_ord_id = "O99";
    fresh.qty = "1000";
    L.fx.feed(L.fx.peer().new_order(fresh));
    const OrderChain* c = L.order("O99");
    CHECK(c != nullptr);
    for (const std::string& old : before) CHECK_NE(c->order_id, old);
    check_all(L.fx);
}

SCENARIO("H-10d", "P0", "an executed scheduled event is not re-run after a restart") {
    Live L("rec-h10");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // A fill with a fixed event identity, so the restart can be asked to run it
    // again by identity.
    L.fx.inject_fill(id, 40000, "1.08490", "FIXED-FILL");
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));

    CHECK(L.fx.restart());
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));

    // Re-scheduling the same identity is refused: the scheduler remembers it
    // ran (W9, H-10).
    ScheduledEvent again;
    again.event_id = "FIXED-FILL";
    again.due_us = L.fx.clock().virtual_now_us();
    again.kind = EventKind::InjectFill;
    again.order_id = id;
    again.qty = 40000;
    again.px_ticks = 108490;
    CHECK_EQ(L.fx.actor().schedule(again), EnqueueResult::AlreadyExecuted);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
}

SCENARIO("H-20b", "P0", "a pending event against an order that went terminal is refused") {
    Live L("rec-h20");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // Schedule a fill in the future, then cancel the order, then restart.
    ScheduledEvent later;
    later.event_id = "LATE-FILL";
    later.due_us = L.fx.clock().virtual_now_us() + 1000000;
    later.kind = EventKind::InjectFill;
    later.order_id = id;
    later.qty = 10000;
    later.px_ticks = 108490;
    CHECK_EQ(L.fx.actor().schedule(later), EnqueueResult::Ok);

    L.fx.feed(L.fx.peer().cancel(CancelArgs{}));
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);

    CHECK(L.fx.restart());
    // The event is still pending after the restart, and running it now must be
    // revalidated against the terminal order rather than executed blindly.
    L.fx.clock().set_virtual_us(later.due_us);
    L.fx.actor().run_due(later.due_us);

    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(0));
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(0));
    std::string why;
    CHECK(L.fx.actor().check_invariants(&why));
}

SCENARIO("A-18", "P0", "a store belonging to another profile refuses to start") {
    tt::TmpDir dir("rec-a18");
    {
        StoreConfig cfg;
        cfg.path = dir.db();
        cfg.session_id = "LP-SESSION-1";
        cfg.sender_comp_id = "SIMLP01";
        cfg.target_comp_id = "TRADER01";
        SqliteStore s;
        CHECK_EQ(s.open(cfg), StoreError::None);
        CHECK(s.recover_and_verify().ok);
        s.close();
    }
    // The same database under different CompIDs: resuming would mean trading on
    // someone else's session history.
    StoreConfig other;
    other.path = dir.db();
    other.session_id = "LP-SESSION-1";
    other.sender_comp_id = "SIMLP01";
    other.target_comp_id = "SOMEONEELSE";
    SqliteStore s;
    CHECK_EQ(s.open(other), StoreError::None);

    VirtualClock clock;
    QuoteBook book;
    Scheduler sched;
    ActorConfig ac;
    ac.firm = "SOMEONEELSE";
    ac.symbols = {eurusd()};
    OrderActor actor(ac, s, book, sched, clock);
    CHECK(!actor.recover());
    CHECK_EQ(actor.halt_reason(), HaltReason::IdentityMismatch);
    // A halted actor produces nothing at all.
    CHECK_EQ(actor.state(), ActorState::Halted);
}

SCENARIO("E-13b", "P0", "a corrupted snapshot halts the actor instead of trading on it") {
    Live L("rec-e13");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    // Make the snapshot claim more filled than the ledger supports.
    CHECK_EQ(L.fx.store().test_force_fill_cum_qty(id, 90000), StoreError::None);

    // The restart refuses to continue (I-19: fail closed, operator recovery).
    CHECK(!L.fx.restart());
}

SCENARIO("J-17c", "P0", "a fill whose report row vanished halts the restart") {
    Live L("rec-j17");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    // Keep the peer from reading so the trade's outbox row stays pending and
    // can still be found by identity. Once a row is SENT it leaves the pending
    // set, which is correct behaviour and simply not what this case needs.
    L.fx.transport().stop_reading();
    L.fx.inject_fill(id, 40000, "1.08490");

    std::vector<OutboxEntry> pending;
    CHECK_EQ(L.fx.store().pending_outbox(&pending), StoreError::None);
    // Find the trade's event and delete it.
    std::string trade_event;
    for (const OutboxEntry& e : pending) {
        if (e.type == ReportType::ExecTrade) trade_event = e.event_id;
    }
    CHECK(!trade_event.empty());
    CHECK_EQ(L.fx.store().test_delete_outbox_row(trade_event), StoreError::None);

    CHECK(!L.fx.restart());
}

SCENARIO("PB-16", "P0", "a restart between two fills gives the same ledger as no restart") {
    // The reference run: two fills, no restart.
    Units reference_cum = 0;
    Ticks reference_avg = 0;
    std::size_t reference_fills = 0;
    {
        Live L("rec-pb16-ref");
        L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
        const std::string id = L.order("O1")->order_id;
        L.fx.inject_fill(id, 40000, "1.08490");
        CHECK_EQ(L.fx.set_quote("EURUSD", "1.08490", "1.08500", 80000, 60000, 2, 600000),
                 QuoteError::Ok);
        L.fx.inject_fill(id, 60000, "1.08500");
        reference_cum = L.order("O1")->cum_qty;
        reference_avg = L.order("O1")->avg_px_ticks;
        reference_fills = L.fx.actor().fill_count_for(id);
    }

    // The same timeline with a restart in the middle.
    Live L("rec-pb16-crash");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    L.restart_and_relogon();

    CHECK_EQ(L.fx.set_quote("EURUSD", "1.08490", "1.08500", 80000, 60000, 2, 600000),
             QuoteError::Ok);
    L.fx.inject_fill(id, 60000, "1.08500");

    // The wire timings differ; the logical ledger does not.
    CHECK_EQ(L.order("O1")->cum_qty, reference_cum);
    CHECK_EQ(L.order("O1")->avg_px_ticks, reference_avg);
    CHECK_EQ(L.fx.actor().fill_count_for(id), reference_fills);
    CHECK_EQ(L.order("O1")->base, BaseState::Filled);
    check_all(L.fx);
}

SCENARIO("A-16b", "P0", "a cancel committed just before a restart stays committed") {
    // F-18: the result of a cancel survives and is not re-decided.
    Live L("rec-a16");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.transport().stop_reading();  // the client never sees the answer
    L.fx.feed(L.fx.peer().cancel(CancelArgs{}));
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);

    L.restart_and_relogon();

    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    CHECK_EQ(L.order("O1")->leaves_qty, static_cast<Units>(0));
    // And the report is delivered after the reconnect rather than lost.
    L.fx.session().dispatch_outbox();
    CHECK_EQ(L.fx.store().count_outbox_pending(), static_cast<std::size_t>(0));
    check_all(L.fx);
}

SCENARIO("J-03", "P0", "a store that refuses every commit halts without side effects") {
    Live L("rec-j03");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    const Units cum = L.order("O1")->cum_qty;

    // Fail the fill's own TX-B. Occurrence 2 rather than 1 because scheduling
    // the event is itself a commit now: the planned event is persisted before
    // it can run, so the first commit is the schedule and the second is the
    // business decision this case is about.
    fp::arm(std::string(fp::point::kTxBBeforeCommit), fp::Action::ReturnError, 0, 2);
    L.fx.inject_fill(id, 40000, "1.08490");
    fp::disarm_all();

    CHECK_EQ(L.fx.actor().state(), ActorState::Halted);
    CHECK_EQ(L.fx.actor().halt_reason(), HaltReason::StoreCommitFailed);
    // No fill, and the in-memory chain was not advanced.
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(0));
    CHECK_EQ(L.order("O1")->cum_qty, cum);

    // A halted actor accepts nothing further.
    NewOrderArgs another;
    another.cl_ord_id = "O2";
    L.fx.feed(L.fx.peer().new_order(another));
    CHECK(L.order("O2") == nullptr);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "recovery"); }
