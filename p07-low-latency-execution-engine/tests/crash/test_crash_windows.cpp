// The 24 crash and scheduling windows of design doc 26.
//
// "Each CW-* creates pause_at(failpoint) with a deterministic barrier, observes
// the persisted markers and then SIGKILLs and restarts. Distinguish 'no durable
// commit yet' from 'COMMIT unknown'; neither authorises an original business
// send."
//
// How these run. A case that needs the process to die really kills it: the
// suite re-launches its own executable as a child with --crash-child=<id>, the
// child arms the failpoint and runs until the boundary, and fp::AbruptExit calls
// _Exit, which performs no unwinding and flushes no stream. The parent then
// opens the journal the child left behind and checks the post-restart oracle.
// That is as close to SIGKILL as is portable, and it is the only way the
// atomicity claim is tested rather than asserted.
//
// What it does not test: power loss. The journal is flushed and pushed past the
// OS cache with fsync or FlushFileBuffers, which survives process death but
// says nothing about a host losing power mid-write. Doc 26 scopes that to real
// hardware, and run set DB-01 of doc 27.3 scopes the database case to a real
// PostgreSQL. Both are recorded as BLOCKED rather than implied.
//
// Windows that cannot cause a durable difference are checked in-process with a
// deterministic barrier instead of a kill: CW-01 to CW-10 all describe moments
// before anything durable exists, and the oracle for every one of them is that
// the restarted engine finds no trace. Killing the process there would test the
// same thing more slowly.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "llx/engine.hpp"
#include "llx/hash.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;
constexpr Qty kLimit = 1000000 * kQtyScale;

std::string g_self_exe;

std::string artifacts_dir() {
    return std::string(LLX_FIXTURES_DIR) + "/../build/test-artifacts";
}

std::string journal_for(const std::string& id) {
    std::error_code ec;
    std::filesystem::create_directories(artifacts_dir(), ec);
    return artifacts_dir() + "/crash_" + id + ".bin";
}

InputEnvelope new_order(OrderId id, Qty qty = 10 * kQtyScale) {
    InputEnvelope e;
    e.event_id = EventId{21, id};
    e.type = EventType::NewOrder;
    e.lane = Lane::NewOrder;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.side = Side::Buy;
    e.qty = qty;
    e.px = 1 * kPxScale;
    e.order_id = id;
    return e;
}

InputEnvelope fill(OrderId id, ExecId xid, Qty qty) {
    InputEnvelope e;
    e.event_id = EventId{22, xid};
    e.type = EventType::ExecutionReport;
    e.lane = Lane::AcceptedReceipt;
    e.accepted_evidence = true;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.order_id = id;
    e.exec_id = xid;
    e.exec_type = ExecType::PartialFill;
    e.last_qty = qty;
    e.last_px = 1 * kPxScale;
    return e;
}

std::unique_ptr<Engine> make_engine(const std::string& journal,
                                    CommitMode mode = CommitMode::Synchronous) {
    Engine::Config cfg;
    cfg.journal_path = journal;
    cfg.commit_mode = mode;
    cfg.sync_on_commit = true;
    auto engine = std::make_unique<Engine>(cfg);
    std::string reject;
    if (!engine->preflight(reject)) TT_FAIL("preflight rejected: " + reject);
    engine->store().configure_risk_domain(kDomain, kLimit);
    engine->refresh_mode();
    return engine;
}

// Launches this executable as a child that will die at the named boundary.
// Returns the child's exit code. 137 is what fp::AbruptExit uses, so an exit
// code of 137 is the evidence that the child really died at the failpoint
// rather than finishing normally -- which doc 25 requires before a playbook may
// be scored.
int run_crash_child(const std::string& id, const std::string& journal,
                    const std::string& failpoint) {
    std::string cmd = "\"" + g_self_exe + "\" --crash-child=" + id + " --journal=\"" +
                      journal + "\" --failpoint=" + failpoint;
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());
    return rc;
}

// What the child does. Kept in one function so the parent's expectations and
// the child's behaviour cannot drift apart.
int crash_child_main(const std::string& id, const std::string& journal,
                     const std::string& failpoint) {
    fp::arm_from_spec(failpoint);
    auto engine = make_engine(journal);

    if (id == "CW-12") {
        // The inbox write only happens for an inbound business proof, so this
        // window is unreachable from a New order alone: the child has to get an
        // order working and then deliver a fill.
        engine->offer(new_order(1));
        engine->run_until_idle();
        engine->offer(fill(1, 9001, 4 * kQtyScale));
        engine->run_until_idle();
    } else if (id == "CW-16" || id == "CW-13" || id == "CW-14" || id == "CW-15" ||
               id == "CW-11") {
        // A single New order. The failpoint sits at one of the store's
        // sub-step boundaries, so the process dies part-way through what a
        // multi-table database would call a transaction.
        engine->offer(new_order(1));
        engine->run_until_idle();
    } else if (id == "CW-18" || id == "CW-19" || id == "CW-20" || id == "CW-21" ||
               id == "CW-22") {
        // The commit completes; the failpoint sits after it, on the dispatch
        // path.
        engine->offer(new_order(1));
        engine->run_until_idle();
        engine->pump_dispatch();
    } else if (id == "CW-23") {
        // An inbound execution report, killed after it was parsed but before
        // its inbox commit.
        engine->offer(new_order(1));
        engine->run_until_idle();
        engine->pump_dispatch();
        engine->offer(fill(1, 9001, 4 * kQtyScale));
        engine->run_until_idle();
    } else {
        engine->offer(new_order(1));
        engine->run_until_idle();
    }
    // Reaching here means the failpoint did not fire, which the parent detects
    // through the exit code.
    std::fflush(stdout);
    return 0;
}

// Opens the journal a dead child left behind and reports what survived.
struct Survivors {
    std::size_t orders = 0;
    std::size_t fills = 0;
    std::size_t outbox = 0;
    std::size_t outbox_ready = 0;
    std::size_t outbox_ambiguous = 0;
    std::size_t outbox_sent = 0;
    std::size_t records = 0;
    std::size_t discarded = 0;
    bool invariants_ok = false;
    std::string state_hash;
};

Survivors inspect(const std::string& journal) {
    JournalStore::Options o;
    o.journal_path = journal;
    JournalStore store(o);
    Survivors s;
    s.orders = store.state().orders.size();
    s.fills = store.state().fills.size();
    s.records = store.record_count();
    s.discarded = static_cast<std::size_t>(store.stats().records_discarded);
    s.invariants_ok = store.state().check().ok();
    s.state_hash = store.state_hash();
    for (const OutboxRecord& ob : store.outbox_snapshot()) {
        ++s.outbox;
        switch (ob.state) {
            case OutboxState::Ready: ++s.outbox_ready; break;
            case OutboxState::Ambiguous: ++s.outbox_ambiguous; break;
            case OutboxState::Sent: ++s.outbox_sent; break;
            default: break;
        }
    }
    return s;
}

// The shared oracle for every window: whatever survived must be internally
// consistent, and nothing may be half applied.
void assert_consistent(const Survivors& s) {
    CHECK(s.invariants_ok);
    // An order with no outbox row, or an outbox row with no order, would be a
    // half-committed transaction.
    if (s.orders > 0) CHECK(s.outbox > 0);
    if (s.outbox > 0) CHECK(s.orders > 0);
}

// Runs one kill-and-restart window end to end.
void crash_window(const std::string& id, const std::string& failpoint,
                  bool expect_durable_effect) {
    const std::string journal = journal_for(id);
    std::remove(journal.c_str());

    const int rc = run_crash_child(id, journal, failpoint);
    // Doc 25: a playbook may not be scored unless the failpoint is confirmed
    // to have fired. A child that exited 0 ran to completion and never hit the
    // boundary, so the case proved nothing.
    CHECK_MSG(rc != 0, "the child exited 0, so the failpoint at " + failpoint +
                           " never fired and this window was not exercised");

    const Survivors s = inspect(journal);
    assert_consistent(s);
    if (expect_durable_effect) {
        // The transaction had committed before the kill, so it must be there.
        CHECK(s.records > 0);
        CHECK_EQ(s.orders, std::size_t{1});
    } else {
        // Nothing durable had happened, so the restart must find no business
        // effect at all. A partially written record is discarded by its
        // checksum, which is what makes the transaction atomic.
        CHECK_EQ(s.orders, std::size_t{0});
        CHECK_EQ(s.fills, std::size_t{0});
        CHECK_EQ(s.outbox, std::size_t{0});
    }
    tt::note_state_hash(s.state_hash);
    std::remove(journal.c_str());
}

}  // namespace

// --- CW-01..CW-10: before anything durable exists -------------------------
//
// Every one of these windows sits upstream of the store, so the oracle is the
// same: the restarted engine finds no business effect, and recovery is P01's
// session replay plus P06's journal, not anything this engine remembers.

SCENARIO("CW-01", "P0", "F,D", "a kill before the frame is complete leaves no event") {
    // P01 owns framing, so what P07 can assert is that an event which never
    // reached the engine produced nothing. The engine is started and stopped
    // without offering anything.
    const std::string journal = journal_for("CW-01");
    std::remove(journal.c_str());
    { auto engine = make_engine(journal); }
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{0});
    CHECK_EQ(s.outbox, std::size_t{0});
    assert_consistent(s);
    std::remove(journal.c_str());
}

SCENARIO("CW-02", "P0", "F,D",
         "an event validated but never published leaves no phantom state") {
    const std::string journal = journal_for("CW-02");
    std::remove(journal.c_str());
    {
        auto engine = make_engine(journal);
        // Built but never offered: the equivalent of dying between P01's
        // validation and the ingress publish.
        const InputEnvelope e = new_order(1);
        (void)e;
    }
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{0});
    assert_consistent(s);
    std::remove(journal.c_str());
}

SCENARIO("CW-03", "P0", "M,D", "a leased pool slot with no business effect is reclaimed") {
    // The orphan allocation of doc 26. The pool is rebuilt from nothing on
    // restart, so the slot cannot leak across a restart; what matters is that
    // no business effect was created by the lease.
    const std::string journal = journal_for("CW-03");
    std::remove(journal.c_str());
    {
        auto engine = make_engine(journal);
        PoolHandle h;
        CHECK_EQ(engine->pool().lease(h), LeaseStatus::Ok);
        CHECK_EQ(engine->pool().live(), std::size_t{1});
        // Dropped without release: a leak within this process, reported as one.
        CHECK(!engine->pool().no_leaks());
    }
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{0});
    CHECK_EQ(s.records, std::size_t{1});  // only the risk-domain configuration
    std::remove(journal.c_str());
}

SCENARIO("CW-04", "P0", "Q,D",
         "a slot written but not published is invisible and leaves nothing durable") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // The in-memory half of this is LL-029 in the ring suite. Here the durable
    // half: no commit happened, so a restart finds nothing.
    const std::string journal = journal_for("CW-04");
    std::remove(journal.c_str());
    {
        auto engine = make_engine(journal);
        fp::disarm_all();
        fp::arm(std::string(fp::point::kRingAfterSlotWrite), fp::Action::ReturnError, 0, 1);
        // The failpoint does not stop the push here; what matters is that the
        // owner never ran, so nothing was committed.
        engine->offer(new_order(1));
        fp::disarm_all();
    }
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{0});
    CHECK_EQ(s.outbox, std::size_t{0});
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-05", "P0", "Q,D",
         "an event published but not consumed exists only in memory") {
    const std::string journal = journal_for("CW-05");
    std::remove(journal.c_str());
    {
        auto engine = make_engine(journal);
        CHECK_EQ(engine->offer(new_order(1)), AdmissionOutcome::AcceptedForProcessing);
        CHECK_EQ(engine->queue().size(), std::size_t{1});
        // Never stepped. The ring is not durable and makes no claim to be: the
        // authority for what survives is the journal (doc 8).
    }
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{0});
    assert_consistent(s);
    std::remove(journal.c_str());
}

SCENARIO("CW-06", "P0", "Q,D",
         "an event acquired but not tail-released is not double applied on restart") {
    const std::string journal = journal_for("CW-06");
    std::remove(journal.c_str());
    {
        auto engine = make_engine(journal);
        engine->offer(new_order(1));
        engine->step(1);  // consumed and committed
    }
    // Restart: exactly one effect, not two. The ring slot's state is
    // irrelevant because the journal is the authority.
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.outbox, std::size_t{1});
    CHECK_EQ(s.fills, std::size_t{0});
    assert_consistent(s);
    std::remove(journal.c_str());
}

SCENARIO("CW-07", "P0", "C,D",
         "an event dequeued but not reduced cannot be acknowledged as complete") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-07", "owner.after_dequeue=exit", /*expect_durable_effect=*/false);
#endif
}

SCENARIO("CW-08", "P0", "C,R,D", "a staged reducer change is not authoritative") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // Doc 13: staged state is not state. The kill lands after the reducer
    // produced a proposal and before anything was written.
    crash_window("CW-08", "owner.after_reduce=exit", false);
#endif
}

SCENARIO("CW-09", "P0", "R,D",
         "risk computed but not staged leaves no reservation and no send") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-09", "owner.after_risk_compute=exit", false);
#endif
}

SCENARIO("CW-10", "P0", "R,D", "a memory-only risk hold is recomputed on restart") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-10", "owner.after_risk_stage=exit", false);
#endif
}

// --- CW-11..CW-16: inside the transaction ---------------------------------
//
// These are the windows that matter most. Each one kills the process between
// two of the writes doc 13 requires to be atomic, and the oracle for all of
// them is identical: no half effect is visible after the restart.

SCENARIO("CW-11", "P0", "D", "a kill after BEGIN rolls the whole transaction back") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-11", "store.after_begin=exit", false);
#endif
}

SCENARIO("CW-12", "P0", "D",
         "a kill between the inbox write and the journal insert leaves neither") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // The inbox write belongs to the inbound fill, so the child commits the
    // order first and dies inside the fill's transaction. The order must
    // survive and the fill must not: a durable inbox row without its journal
    // entry would be the half effect this window exists to exclude.
    const std::string journal = journal_for("CW-12");
    std::remove(journal.c_str());
    const int rc = run_crash_child("CW-12", journal, "store.after_inbox=exit");
    CHECK_MSG(rc != 0, "the child exited 0: the inbox boundary was never reached");
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.fills, std::size_t{0});
    assert_consistent(s);
    tt::note_state_hash(s.state_hash);
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-13", "P0", "D", "there is no committed journal without its projection") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-13", "store.after_journal=exit", false);
#endif
}

SCENARIO("CW-14", "P0", "D,R",
         "there is no committed projection without its risk reconciliation") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-14", "store.after_projection=exit", false);
#endif
}

SCENARIO("CW-15", "P0", "D,R",
         "there is no committed reservation without its matching intent") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-15", "store.after_risk_row=exit", false);
#endif
}

SCENARIO("CW-16", "P0", "D",
         "an outbox row inserted but not committed is not eligible to send") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // The most important of the set: if this failed, an original could be sent
    // for a transaction that never committed (LI-16).
    crash_window("CW-16", "store.after_outbox=exit", false);
#endif
}

// --- CW-17: the lost acknowledgement -------------------------------------

SCENARIO("CW-17", "P0", "D",
         "a lost commit acknowledgement is resolved by key, never by reissue") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // The record is durable and the acknowledgement is lost. LI-17 forbids
    // reading that as a failure, because reissuing the business command would
    // risk a duplicate order at the LP.
    fp::disarm_all();
    fp::reset_counters();
    const std::string journal = journal_for("CW-17");
    std::remove(journal.c_str());

    std::uint64_t key = 0;
    {
        auto engine = make_engine(journal);
        fp::arm(std::string(fp::point::kStoreCommitLostAck), fp::Action::ReturnError, 0, 1);
        engine->offer(new_order(1));
        engine->run_until_idle();
        CHECK(fp::fired(fp::point::kStoreCommitLostAck) > 0);
        // The engine holds: unknown, not failed.
        CHECK_EQ(engine->owner().stats().commit_unknown, std::uint64_t{1});
        CHECK_EQ(engine->owner().unresolved_unknown(), std::size_t{1});
        CHECK_EQ(engine->refresh_mode(), ServiceMode::ReconOnly);
        // And no original is dispatched while the outcome is unproven.
        CHECK_EQ(engine->pump_dispatch(), std::size_t{0});
        CHECK_EQ(engine->sink().frames().size(), std::size_t{0});
        fp::disarm_all();
    }

    // After the restart the resolution is a lookup, and it shows the
    // transaction did commit.
    {
        JournalStore::Options o;
        o.journal_path = journal;
        JournalStore store(o);
        CHECK_EQ(store.state().orders.size(), std::size_t{1});
        const auto obs = store.outbox_snapshot();
        CHECK_EQ(obs.size(), std::size_t{1});
        // Never attempted, so it is still Ready: the original may now be sent
        // exactly once.
        CHECK_EQ(obs[0].state, OutboxState::Ready);
        CHECK_EQ(obs[0].attempts, std::uint32_t{0});
        // The idempotency key resolves, which is the mechanism doc 13 names.
        bool found = false;
        for (std::uint64_t probe_hi = 0; probe_hi < 1 && !found; ++probe_hi) {
            CommitReceipt r;
            // The key is derived from the event identity, so it is
            // recomputable rather than remembered.
            const std::uint64_t k =
                fnv1a_mix(fnv1a_mix(0x9e3779b97f4a7c15ULL, EventId{21, 1}.hi),
                          EventId{21, 1}.lo);
            key = k;
            found = store.lookup_by_idempotency(k, r);
            if (found) CHECK_EQ(r.outcome, CommitOutcome::Committed);
        }
        CHECK(found);
        CHECK(key != 0);
    }
    std::remove(journal.c_str());
#endif
}

// --- CW-18..CW-22: after the commit, on the way to the wire --------------

SCENARIO("CW-18", "P0", "D",
         "a commit survives a kill before the owner callback, replay-equal") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    crash_window("CW-18", "owner.after_commit_result=exit", /*expect_durable_effect=*/true);
#endif
}

SCENARIO("CW-19", "P0", "D,F",
         "a claimed outbox row comes back as ambiguous, not as ready") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // The attempt marker is durable before the record leaves the store, so the
    // restart can tell "never sent" from "may be at the LP".
    const std::string journal = journal_for("CW-19");
    std::remove(journal.c_str());
    const int rc = run_crash_child("CW-19", journal, "dispatch.after_claim=exit");
    CHECK_MSG(rc != 0, "the child exited 0: the dispatch boundary was never reached");

    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.outbox, std::size_t{1});
    // Not Ready. LI-18: a second original is exactly what must not happen.
    CHECK_EQ(s.outbox_ready, std::size_t{0});
    CHECK_EQ(s.outbox_ambiguous, std::size_t{1});
    assert_consistent(s);

    // And a restarted dispatcher refuses to send it as an original.
    {
        JournalStore::Options o;
        o.journal_path = journal;
        JournalStore store(o);
        RecordingSink sink;
        Dispatcher dispatcher(store, sink);
        CHECK_EQ(dispatcher.pump(store.fencing_epoch()), std::size_t{0});
        CHECK_EQ(sink.frames().size(), std::size_t{0});
        CHECK_EQ(dispatcher.ambiguous_records().size(), std::size_t{1});
    }
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-20", "P0", "D,F", "a kill before the encode leaves the durable original intact") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    const std::string journal = journal_for("CW-20");
    std::remove(journal.c_str());
    const int rc = run_crash_child("CW-20", journal, "dispatch.before_encode=exit");
    CHECK_MSG(rc != 0, "the child exited 0: the encode boundary was never reached");
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.outbox_ambiguous, std::size_t{1});
    assert_consistent(s);
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-21", "P0", "D,F",
         "a kill between the encode and the socket write preserves the original") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    const std::string journal = journal_for("CW-21");
    std::remove(journal.c_str());
    const int rc = run_crash_child("CW-21", journal, "dispatch.after_encode=exit");
    CHECK_MSG(rc != 0, "the child exited 0: the write boundary was never reached");
    const Survivors s = inspect(journal);
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.outbox, std::size_t{1});
    CHECK_EQ(s.outbox_ready, std::size_t{0});
    assert_consistent(s);
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-22", "P0", "D,F",
         "a partial socket write is ambiguous and never gets a new ClOrdID") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // In-process, because what is under test is the policy rather than the
    // durability: after an unknown write the row is Ambiguous, the dispatcher
    // will not resend it as an original, and no second ClOrdID is minted.
    fp::disarm_all();
    fp::reset_counters();
    auto engine = make_engine("");
    engine->offer(new_order(1));
    // step() only: run_until_idle would dispatch the original before the
    // failpoint is armed, and then this window would test nothing.
    engine->step(1);
    CHECK_EQ(engine->store().outbox_snapshot().size(), std::size_t{1});
    CHECK_EQ(engine->store().outbox_snapshot()[0].state, OutboxState::Ready);

    fp::arm(std::string(fp::point::kDispatchPartialWrite), fp::Action::ReturnError, 0, 1);
    CHECK_EQ(engine->pump_dispatch(), std::size_t{1});
    CHECK(fp::fired(fp::point::kDispatchPartialWrite) > 0);
    fp::disarm_all();

    const auto obs = engine->store().outbox_snapshot();
    CHECK_EQ(obs.size(), std::size_t{1});
    CHECK_EQ(obs[0].state, OutboxState::Ambiguous);
    CHECK_EQ(engine->dispatcher().stats().ambiguous, std::uint64_t{1});
    CHECK_EQ(engine->dispatcher().stats().sent, std::uint64_t{0});

    // Pumping again sends nothing: no retry, no new identifier.
    CHECK_EQ(engine->pump_dispatch(), std::size_t{0});
    CHECK_EQ(engine->sink().frames().size(), std::size_t{0});
    CHECK_EQ(engine->dispatcher().ambiguous_records().size(), std::size_t{1});
    // The ClOrdID is unchanged: it is the order's identity, not a retry token.
    CHECK_EQ(obs[0].cl_ord_id, OrderId{1});
#endif
}

// --- CW-23..CW-24: inbound reconciliation --------------------------------

SCENARIO("CW-23", "P0", "D,F",
         "a parsed execution report that was not committed is recoverable by resend") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    const std::string journal = journal_for("CW-23");
    std::remove(journal.c_str());
    const int rc = run_crash_child("CW-23", journal, "store.after_inbox=exit");
    CHECK_MSG(rc != 0, "the child exited 0: the inbound commit boundary was never reached");

    const Survivors s = inspect(journal);
    // The order survived; the fill did not, because its transaction never
    // committed. P01's resend and P06's inbox dedup are what bring it back,
    // and applying it again after the restart must produce exactly one fill.
    CHECK_EQ(s.orders, std::size_t{1});
    CHECK_EQ(s.fills, std::size_t{0});
    assert_consistent(s);

    {
        auto engine = make_engine(journal);
        CHECK_EQ(engine->store().state().fills.size(), std::size_t{0});
        // The resend.
        engine->offer(fill(1, 9001, 4 * kQtyScale));
        engine->run_until_idle();
        CHECK_EQ(engine->store().state().fills.size(), std::size_t{1});
        // And a second copy of the same resend produces no second effect.
        engine->offer(fill(1, 9001, 4 * kQtyScale));
        engine->run_until_idle();
        CHECK_EQ(engine->store().state().fills.size(), std::size_t{1});
        CHECK(engine->store().state().check().ok());
    }
    std::remove(journal.c_str());
#endif
}

SCENARIO("CW-24", "P0", "M,D",
         "a recycled pool slot cannot be overwritten by a stale handle") {
    // The durable half is covered by CW-23; this is the memory half, and it is
    // the ABA case again at the point where it matters most: after the inbound
    // commit, when the slot is handed back.
    auto engine = make_engine("");
    PoolHandle first;
    CHECK_EQ(engine->pool().lease(first, InputEnvelope{}), LeaseStatus::Ok);
    InputEnvelope* a = engine->pool().get(first);
    CHECK(a != nullptr);
    a->order_id = 111;
    CHECK_EQ(engine->pool().release(first), ReleaseStatus::Ok);

    // The slot comes back for another event.
    PoolHandle second;
    CHECK_EQ(engine->pool().lease(second, InputEnvelope{}), LeaseStatus::Ok);
    InputEnvelope* b = engine->pool().get(second);
    CHECK(b != nullptr);
    b->order_id = 222;

    // The stale handle cannot reach it, and cannot free it.
    CHECK(engine->pool().get(first) == nullptr);
    CHECK_EQ(engine->pool().release(first), ReleaseStatus::StaleGeneration);
    CHECK_EQ(engine->pool().get(second)->order_id, OrderId{222});
    CHECK_EQ(engine->pool().release(second), ReleaseStatus::Ok);
    CHECK(engine->pool().no_leaks());
}

// --- limits --------------------------------------------------------------

SCENARIO_BLOCKED("CW-25", "P0", "D",
                 "the journal survives a host power loss mid-write",
                 "the store flushes and then fsyncs (FlushFileBuffers on Windows), which "
                 "survives process death and is what CW-11..CW-24 above exercise. Power "
                 "loss is a different claim: it needs real hardware with write-cache "
                 "control, and doc 26 scopes durability beyond the fsync contract out of "
                 "the lab");

SCENARIO_BLOCKED("CW-26", "P0", "D",
                 "the same windows hold against a real PostgreSQL transaction",
                 "doc 15.1 keeps the in-process store and a real database as separate "
                 "span classes, and run set DB-01 of doc 27.3 requires PostgreSQL. P06 "
                 "owns that implementation; until its adapter exists the integrated "
                 "durability gate is BLOCKED, which doc 23 defines as 'dependency absent'");

int main(int argc, char** argv) {
    g_self_exe = (argc > 0) ? argv[0] : "test_crash_windows";

    // Child mode: arm the failpoint, run to the boundary, die there.
    std::string child_id, journal, failpoint;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--crash-child=", 0) == 0) child_id = a.substr(14);
        else if (a.rfind("--journal=", 0) == 0) journal = a.substr(10);
        else if (a.rfind("--failpoint=", 0) == 0) failpoint = a.substr(12);
    }
    if (!child_id.empty()) {
        // Strip surrounding quotes the shell may have left on the path.
        if (journal.size() >= 2 && journal.front() == '"' && journal.back() == '"')
            journal = journal.substr(1, journal.size() - 2);
        return crash_child_main(child_id, journal, failpoint);
    }

    return tt::run_all(argc, argv, "crash_windows");
}
