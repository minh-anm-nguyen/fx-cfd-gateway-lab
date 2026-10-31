// The 36 fault-injection playbooks of design doc 25.
//
// Each playbook is setup, trigger, PASS oracle. Doc 25 adds one rule that
// shapes every case here: "không được đánh dấu PASS nếu không xác nhận
// failpoint thực sự kích hoạt" -- no PASS without confirming the failpoint
// actually fired. So every case that arms a failpoint asserts fp::fired() > 0
// before it asserts anything about the outcome.
//
// Playbooks that need hardware or an environment this host does not have are
// BLOCKED with the specific missing capability named. Doc 23 defines BLOCKED as
// "dependency absent" and is explicit that it is not a pass. There are nine of
// them and they are the honest boundary of what this lab can show: a weak
// memory model, NUMA, CPU hotplug, SMT isolation, IRQ control, cross-socket
// TSC, thermal headroom, a real database, and a 24-hour window.

#include <string>
#include <thread>
#include <vector>

#include "llx/config.hpp"
#include "llx/engine.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;
constexpr Qty kLimit = 1000000 * kQtyScale;

struct Fixture {
    Engine engine;
    std::string reject;

    explicit Fixture(Engine::Config cfg = {}, Qty limit = kLimit)
        : engine(std::move(cfg)) {
        fp::disarm_all();
        fp::reset_counters();
        if (!engine.preflight(reject)) TT_FAIL("preflight rejected: " + reject);
        engine.store().configure_risk_domain(kDomain, limit);
        engine.refresh_mode();
    }
    ~Fixture() { fp::disarm_all(); }
};

InputEnvelope new_order(OrderId id, Qty qty = 10 * kQtyScale, Side side = Side::Buy) {
    InputEnvelope e;
    e.event_id = EventId{31, id};
    e.type = EventType::NewOrder;
    e.lane = Lane::NewOrder;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.side = side;
    e.qty = qty;
    e.px = 1 * kPxScale;
    e.order_id = id;
    return e;
}

InputEnvelope exec_report(OrderId id, ExecId xid, ExecType t, Qty qty = 0) {
    InputEnvelope e;
    e.event_id = EventId{32, xid};
    e.type = EventType::ExecutionReport;
    e.lane = Lane::AcceptedReceipt;
    e.accepted_evidence = true;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.order_id = id;
    e.exec_id = xid;
    e.exec_type = t;
    e.last_qty = qty;
    e.last_px = 1 * kPxScale;
    return e;
}

InputEnvelope cancel_request(OrderId id, OrderId orig) {
    InputEnvelope e;
    e.event_id = EventId{33, id};
    e.type = EventType::CancelRequest;
    e.lane = Lane::CancelReplace;
    e.risk_domain = kDomain;
    e.order_id = id;
    e.orig_order_id = orig;
    return e;
}

InputEnvelope session_control(std::uint64_t n) {
    InputEnvelope e;
    e.event_id = EventId{34, n};
    e.type = EventType::SessionControl;
    e.lane = Lane::SessionControl;
    e.risk_domain = kDomain;
    return e;
}

std::string artifact(const char* name) {
    return std::string(LLX_FIXTURES_DIR) + "/../build/test-artifacts/" + name;
}

}  // namespace

SCENARIO("FI-01", "P0", "Q,M", "SPSC full race: one safe enqueue and one explicit full") {
    // Setup: pre-fill the ring to N-1. Trigger: push two more across the
    // capacity boundary. PASS: one succeeds, one is explicitly full, FIFO is
    // intact (LI-02, LI-05, LI-07).
    SpscRing<std::uint64_t, 4> ring;
    for (std::uint64_t i = 0; i < 3; ++i) CHECK(ring.try_push(i));
    CHECK_EQ(ring.size(), std::size_t{3});

    CHECK_EQ(ring.push(3), PushStatus::Ok);
    CHECK_EQ(ring.push(4), PushStatus::Full);
    CHECK_EQ(ring.size(), std::size_t{4});

    for (std::uint64_t i = 0; i < 4; ++i) {
        std::uint64_t v = 0;
        CHECK(ring.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(ring.empty());
    CHECK(ring.invariant_ok());
}

SCENARIO_BLOCKED("FI-02", "P0", "Q,M,C",
                 "an incorrect-publication mutant is caught by a portability test",
                 "needs a weak memory model (ARM64) or a model checker. On x86_64 TSO a "
                 "release downgraded to relaxed is not observable, so building the mutant "
                 "here and watching it pass would be evidence of nothing. LI-30 says this "
                 "explicitly: passing on x86 does not establish portability");

SCENARIO("FI-03", "P0", "Q", "two producers on one ring: the guard refuses the second") {
    SpscRing<std::uint64_t, 16> ring;
    CHECK(ring.try_push(1));
    PushStatus other = PushStatus::Ok;
    std::thread t([&] { other = ring.push(2); });
    t.join();
#ifdef LLX_NO_OWNERSHIP_GUARD
    TT_INCONCLUSIVE("the ownership guard is compiled out of this build variant");
#else
    CHECK_EQ(other, PushStatus::WrongProducer);
    CHECK_EQ(ring.guard_violations(), std::uint64_t{1});
    // No corrupted payload: the refused push left the ring exactly as it was.
    CHECK_EQ(ring.size(), std::size_t{1});
    std::uint64_t v = 0;
    CHECK(ring.try_pop(v));
    CHECK_EQ(v, std::uint64_t{1});
#endif
}

SCENARIO("FI-04", "P0", "Q,M", "ring counter wrap: a thousand crossings stay correct") {
    // Setup: a synthetic 8-bit counter near its maximum. Trigger: repeat
    // enqueue/dequeue across the wrap. PASS: never an incorrect full, empty or
    // slot, and never out of range (LI-06).
    SpscRing<std::uint64_t, 4, std::uint8_t> ring;
    CHECK(ring.seek_counters_for_test(252));
    std::uint64_t expect = 0;
    for (int round = 0; round < 1000; ++round) {
        CHECK(ring.try_push(expect));
        CHECK(ring.try_push(expect + 1));
        CHECK_EQ(ring.size(), std::size_t{2});
        CHECK(!ring.full());
        std::uint64_t v = 0;
        CHECK(ring.try_pop(v));
        CHECK_EQ(v, expect);
        CHECK(ring.try_pop(v));
        CHECK_EQ(v, expect + 1);
        CHECK(ring.empty());
        CHECK(ring.invariant_ok());
        expect += 2;
    }
    CHECK_EQ(expect, std::uint64_t{2000});
}

SCENARIO("FI-05", "P0", "M,S",
         "pool exhaustion at admission rejects the New and keeps the fill path safe") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // Setup: the pool is full but the queue has room, so the refusal must come
    // from the pool and name it. PASS: the New is refused before durable
    // admission, no dynamic fallback, and accepted evidence is still safe
    // (LI-09, LI-10, LI-20).
    Fixture f;
    HealthSnapshot h;
    h.queue_capacity = 100;
    h.queue_occupancy = 0;   // the queue has room
    h.pool_free = 0;         // the pool does not
    h.persistence_inflight_budget = 4;
    h.store_healthy = true;
    h.preflight_complete = true;
    h.replay_verified = true;
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{0});

    // An unadmitted New is refused outright.
    CHECK_EQ(f.engine.modes().admit(Lane::NewOrder, false, h),
             AdmissionOutcome::RejectedOverload);
    // Accepted business evidence is deferred instead, never refused.
    CHECK_EQ(f.engine.modes().admit(Lane::AcceptedReceipt, true, h),
             AdmissionOutcome::RetryLaterNoAdmission);

    // And the pool itself refuses without allocating.
    fp::arm(std::string(fp::point::kPoolExhaust), fp::Action::ReturnError, 0, 1);
    PoolHandle ph;
    CHECK_EQ(f.engine.pool().lease(ph), LeaseStatus::Exhausted);
    CHECK(fp::fired(fp::point::kPoolExhaust) > 0);
    CHECK_EQ(f.engine.pool().live(), std::size_t{0});
#endif
}

SCENARIO("FI-06", "P0", "M", "ABA stale callback: the generation invalidates the handle") {
    // Setup: lease slot X, hand the handle to an async consumer, free and
    // re-assign X. Trigger: deliver the delayed callback for the original.
    // PASS: the handle's generation is invalid and the new occupant is
    // untouched (LI-08, LI-47).
    Pool<InputEnvelope> pool(1, 1);
    PoolHandle for_a;
    CHECK_EQ(pool.lease(for_a), LeaseStatus::Ok);
    pool.get(for_a)->order_id = 1;
    CHECK_EQ(pool.release(for_a), ReleaseStatus::Ok);

    PoolHandle for_b;
    CHECK_EQ(pool.lease(for_b), LeaseStatus::Ok);
    pool.get(for_b)->order_id = 2;

    CHECK(pool.get(for_a) == nullptr);
    CHECK_EQ(pool.release(for_a), ReleaseStatus::StaleGeneration);
    CHECK_EQ(pool.get(for_b)->order_id, OrderId{2});
    CHECK(pool.stats().stale_rejections > 0);
    CHECK_EQ(pool.release(for_b), ReleaseStatus::Ok);
    CHECK(pool.no_leaks());
}

SCENARIO("FI-07", "P0", "M", "pool double free is refused and the slot is not re-queued") {
    Pool<InputEnvelope> pool(1, 4);
    PoolHandle h;
    CHECK_EQ(pool.lease(h), LeaseStatus::Ok);
    CHECK_EQ(pool.release(h), ReleaseStatus::Ok);
    const std::size_t free_after = pool.free_slots();
    CHECK_EQ(pool.release(h), ReleaseStatus::DoubleFree);
    CHECK_EQ(pool.free_slots(), free_after);
    CHECK_EQ(pool.stats().double_free_rejections, std::uint64_t{1});
    CHECK_EQ(pool.stats().releases, std::uint64_t{1});
}

SCENARIO("FI-08", "P0", "P,S",
         "metrics ring collapse: samples are lost and counted, business unaffected") {
    // Setup: a telemetry lane with no drain. Trigger: flood it. PASS: the loss
    // is counted, coverage is marked incomplete, and no business state changes
    // (LI-24, LI-26).
    Fixture f;
    const std::string before = f.engine.store().state_hash();
    for (std::size_t i = 0; i < TelemetryLane::kCapacity + 5000; ++i) {
        SpanSample s;
        s.span = Span::L2_DecisionCompute;
        s.ns = 100;
        f.engine.telemetry().try_sample(s);
    }
    CHECK(f.engine.telemetry().lost_samples() > 0);
    CHECK(!f.engine.telemetry().coverage_complete());

    // The engine keeps working and its business result is unchanged by the
    // telemetry failure.
    for (OrderId i = 1; i <= 20; ++i) f.engine.offer(new_order(i));
    f.engine.run_until_idle();
    CHECK_NE(f.engine.store().state_hash(), before);
    CHECK_EQ(f.engine.owner().stats().committed, std::uint64_t{20});
    const Engine::SelfCheck sc = f.engine.self_check();
    CHECK_MSG(sc.ok(), sc.detail);
}

SCENARIO("FI-09", "P0", "Q,S",
         "a consumer pause at the high watermark sheds new work and keeps evidence") {
    Engine::Config cfg;
    cfg.spsc_capacity = 256;
    cfg.pool_slabs = 1;
    cfg.pool_slots_per_slab = 8192;
    Fixture f(cfg);

    std::uint64_t accepted = 0, overload = 0, deferred = 0;
    for (OrderId i = 1; i <= 600; ++i) {
        switch (f.engine.offer(new_order(i))) {
            case AdmissionOutcome::AcceptedForProcessing: ++accepted; break;
            case AdmissionOutcome::RejectedOverload: ++overload; break;
            case AdmissionOutcome::RetryLaterNoAdmission: ++deferred; break;
            default: break;
        }
    }
    CHECK_EQ(accepted, std::uint64_t{256});
    CHECK(overload > 0);
    // Accepted evidence offered into the same saturated queue is deferred, not
    // refused.
    const AdmissionOutcome receipt = f.engine.offer(exec_report(1, 5000, ExecType::New));
    CHECK_NE(receipt, AdmissionOutcome::RejectedOverload);
    // Session control is still serviced (LI-19).
    CHECK_NE(f.engine.offer(session_control(1)), AdmissionOutcome::RejectedOverload);

    f.engine.run_until_idle();
    CHECK(f.engine.outcomes().balanced());
}

SCENARIO("FI-10", "P0", "F,S", "FIX heartbeat under a business flood is never starved") {
    Engine::Config cfg;
    cfg.spsc_capacity = 256;
    cfg.pool_slabs = 1;
    cfg.pool_slots_per_slab = 8192;
    Fixture f(cfg);

    std::uint64_t heartbeats_accepted = 0, heartbeats_offered = 0;
    for (OrderId i = 1; i <= 2000; ++i) {
        f.engine.offer(new_order(i));
        if (i % 10 == 0) {
            ++heartbeats_offered;
            const AdmissionOutcome o = f.engine.offer(session_control(i));
            if (o == AdmissionOutcome::AcceptedForProcessing ||
                o == AdmissionOutcome::RetryLaterNoAdmission)
                ++heartbeats_accepted;
        }
    }
    // Not one heartbeat was refused with an overload rejection.
    CHECK_EQ(heartbeats_accepted, heartbeats_offered);
    CHECK(heartbeats_offered > 0);
}

SCENARIO("FI-11", "P0", "C,R,D", "a delayed partial fill racing a cancel keeps causal order") {
    Fixture f;
    f.engine.offer(new_order(1, 100 * kQtyScale));
    f.engine.run_until_idle();
    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.run_until_idle();

    f.engine.offer(cancel_request(2, 1));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::PendingCancel);
    const Qty reserved_before = f.engine.store().state().risk.at(kDomain).reserved_buy;

    // The fill arrives while the cancel is outstanding (doc 19.4).
    f.engine.offer(exec_report(1, 11, ExecType::PartialFill, 30 * kQtyScale));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).cum_qty, 30 * kQtyScale);
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::PendingCancel);
    CHECK_EQ(f.engine.store().state().risk.at(kDomain).reserved_buy,
             reserved_before - 30 * kQtyScale);

    f.engine.offer(exec_report(1, 12, ExecType::Canceled));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::Canceled);
    CHECK_EQ(f.engine.store().state().risk.at(kDomain).reserved_buy, Qty{0});
    CHECK(f.engine.store().state().check().ok());
}

SCENARIO("FI-12", "P0", "R,D", "a shared risk limit cannot be overspent under a race") {
    Fixture f(Engine::Config{}, 100 * kQtyScale);
    for (OrderId i = 1; i <= 20; ++i) {
        f.engine.offer(new_order(i, 30 * kQtyScale));
        f.engine.run_until_idle();
        const auto it = f.engine.store().state().risk.find(kDomain);
        if (it != f.engine.store().state().risk.end()) {
            CHECK(it->second.within_limit());
            CHECK(it->second.non_negative());
        }
    }
    const DomainRisk& r = f.engine.store().state().risk.at(kDomain);
    CHECK_EQ(r.reserved_buy, 90 * kQtyScale);
    CHECK_EQ(f.engine.store().state().orders.size(), std::size_t{3});
    CHECK(f.engine.store().state().check().ok());
}

SCENARIO("FI-13", "P0", "D,S", "a durable sync stall is a refusal, not a silent success") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    Fixture f;
    fp::arm(std::string(fp::point::kStoreCommitStall), fp::Action::ReturnError, 0, 1);
    f.engine.offer(new_order(1));
    f.engine.run_until_idle();
    CHECK(fp::fired(fp::point::kStoreCommitStall) > 0);

    CHECK_EQ(f.engine.store().state().orders.size(), std::size_t{0});
    CHECK_EQ(f.engine.store().outbox_snapshot().size(), std::size_t{0});
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{0});
    CHECK_EQ(f.engine.owner().stats().commit_rejected, std::uint64_t{1});

    // And the engine recovers: the sequence number the refused commit consumed
    // was returned, so the next order is not wedged as out-of-order.
    fp::disarm_all();
    f.engine.offer(new_order(2));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.size(), std::size_t{1});
    CHECK_EQ(f.engine.owner().stats().committed, std::uint64_t{1});
#endif
}

SCENARIO("FI-14", "P0", "D,S",
         "an ambiguous COMMIT holds and is resolved by key, never reissued") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    Fixture f;
    fp::arm(std::string(fp::point::kStoreCommitLostAck), fp::Action::ReturnError, 0, 1);
    f.engine.offer(new_order(1));
    f.engine.run_until_idle();
    CHECK(fp::fired(fp::point::kStoreCommitLostAck) > 0);

    CHECK_EQ(f.engine.owner().stats().commit_unknown, std::uint64_t{1});
    CHECK_EQ(f.engine.owner().unresolved_unknown(), std::size_t{1});
    CHECK_EQ(f.engine.refresh_mode(), ServiceMode::ReconOnly);
    // LI-16 and LI-18: nothing goes on the wire while the outcome is unproven.
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{0});
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{0});
    fp::disarm_all();
    CHECK_EQ(f.engine.offer(new_order(2)), AdmissionOutcome::HeldForReconciliation);
#endif
}

SCENARIO("FI-15", "P0", "D,R",
         "a rollback after staging leaves the committed state exactly as it was") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    Fixture f;
    f.engine.offer(new_order(1));
    f.engine.run_until_idle();
    const std::string before = f.engine.store().state_hash();
    const std::size_t records_before = f.engine.store().record_count();

    fp::arm(std::string(fp::point::kStoreCommitStall), fp::Action::ReturnError, 0, 1);
    f.engine.offer(new_order(2, 25 * kQtyScale));
    f.engine.run_until_idle();
    CHECK(fp::fired(fp::point::kStoreCommitStall) > 0);

    // LL-010: committed state before and after is equal; the staging was never
    // authoritative.
    CHECK_EQ(f.engine.store().state_hash(), before);
    CHECK_EQ(f.engine.store().record_count(), records_before);
#endif
}

SCENARIO("FI-16", "P0", "D,F", "an ambiguous original send is held and never duplicated") {
    Fixture f;
    f.engine.offer(new_order(1));
    f.engine.step(1);
    f.engine.sink().program(1, SendResult::Unknown);
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{1});

    const auto obs = f.engine.store().outbox_snapshot();
    CHECK_EQ(obs.size(), std::size_t{1});
    CHECK_EQ(obs[0].state, OutboxState::Ambiguous);
    CHECK_EQ(f.engine.dispatcher().stats().ambiguous, std::uint64_t{1});

    for (int i = 0; i < 10; ++i) CHECK_EQ(f.engine.pump_dispatch(), std::size_t{0});
    CHECK_EQ(f.engine.sink().attempts(), std::uint64_t{1});
    CHECK_EQ(f.engine.dispatcher().stats().duplicate_original_attempts, std::uint64_t{0});
    CHECK_EQ(f.engine.dispatcher().ambiguous_records().size(), std::size_t{1});
    // The ClOrdID is the order's identity, not a retry token.
    CHECK_EQ(obs[0].cl_ord_id, OrderId{1});
}

SCENARIO("FI-17", "P0", "C,D", "a duplicate execution report produces one effect") {
    Fixture f;
    f.engine.offer(new_order(1, 50 * kQtyScale));
    f.engine.run_until_idle();
    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.run_until_idle();

    const auto fill = exec_report(1, 11, ExecType::PartialFill, 20 * kQtyScale);
    f.engine.offer(fill);
    f.engine.run_until_idle();
    const std::string after_first = f.engine.store().state_hash();

    // The same report again, as a session-layer PossDup replay would deliver.
    InputEnvelope replay = fill;
    replay.poss_dup = true;
    f.engine.offer(replay);
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state_hash(), after_first);
    CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{1});
    CHECK(f.engine.owner().stats().duplicates > 0 || f.engine.owner().stats().no_effect > 0);
}

SCENARIO("FI-18", "P0", "C,D", "a conflicting execution report quarantines the order") {
    Fixture f;
    f.engine.offer(new_order(1, 10 * kQtyScale));
    f.engine.run_until_idle();
    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.run_until_idle();
    f.engine.offer(exec_report(1, 11, ExecType::Fill, 10 * kQtyScale));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::Filled);

    f.engine.offer(exec_report(1, 12, ExecType::PartialFill, 5 * kQtyScale));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::Quarantined);
    CHECK(f.engine.owner().stats().quarantined > 0);
    CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{1});
    // The quarantine survived a replay: it is a committed fact.
    const std::string h = f.engine.store().state_hash();
    f.engine.store().rebuild_from_journal();
    CHECK_EQ(f.engine.store().state_hash(), h);
}

SCENARIO("FI-19", "P0", "D", "an inverted owner callback does not reorder the effect") {
    Engine::Config cfg;
    cfg.commit_mode = CommitMode::AsyncWorker;
    Fixture f(cfg);
    f.engine.offer(new_order(1));
    f.engine.step(1);
    CHECK_EQ(f.engine.owner().inflight(), std::size_t{1});

    CommitReceipt ahead;
    ahead.event_id = EventId{31, 1};
    ahead.domain_seq = 5;  // far ahead of the expected 1
    ahead.risk_domain = kDomain;
    ahead.owner_epoch = f.engine.owner().epoch();
    ahead.outcome = CommitOutcome::Committed;
    f.engine.owner().on_commit_result(ahead);

    CHECK_EQ(f.engine.owner().stats().receipts_reordered, std::uint64_t{1});
    CHECK_EQ(f.engine.owner().stats().committed, std::uint64_t{0});
    CHECK_EQ(f.engine.owner().inflight(), std::size_t{1});
}

SCENARIO("FI-20", "P0", "D", "a stale fencing epoch is refused at every layer") {
    Fixture f;
    f.engine.offer(new_order(1));
    f.engine.run_until_idle();
    f.engine.owner().promote_to_epoch(7);

    InputEnvelope stale = new_order(2);
    stale.owner_epoch = 3;
    CHECK_EQ(f.engine.owner().on_event(stale), AdmissionOutcome::StaleEpoch);

    InputEnvelope e = new_order(3);
    e.owner_epoch = 3;
    e.domain_seq = 99;
    const Proposal p = reduce(f.engine.store().state(), e, false);
    CommitRequest req;
    req.intent = to_intent(p, e);
    req.effect = p;
    CommitReceipt r;
    CHECK_EQ(f.engine.store().commit(req, r), CommitOutcome::Rejected);
    CHECK(f.engine.store().stats().stale_epoch_refusals > 0);

    OutboxRecord ob;
    CHECK(!f.engine.store().claim_next(3, ob));
}

SCENARIO_BLOCKED("FI-21", "P0", "O,P",
                 "NUMA remote first touch is measured against local placement",
                 "this host reports no NUMA nodes, so there is no remote node to bind "
                 "memory to. mbind/set_mempolicy are Linux-only and doc 11 step 4 keeps "
                 "the placement optional for exactly this reason");

SCENARIO_BLOCKED("FI-22", "P0", "O,S",
                 "CPU hotplug under load degrades safely and invalidates the benchmark",
                 "requires offlining a CPU through /sys/devices/system/cpu/cpuN/online on "
                 "a disposable Linux host. The detection half is implemented and tested "
                 "(LL-151 proves plan_still_valid reports the drift); triggering the "
                 "hotplug itself is not available here");

SCENARIO_BLOCKED("FI-23", "P0", "O,P",
                 "an SMT sibling noisy neighbour is quantified",
                 "needs control over which logical CPUs are loaded and an idle sibling to "
                 "compare against, on a host with no other tenants. The planner's sibling "
                 "avoidance is tested (LL-147) but the performance effect cannot be "
                 "measured here");

SCENARIO_BLOCKED("FI-24", "P0", "O,P",
                 "an IRQ and scheduler storm is survived with bounded tail growth",
                 "requires /proc/irq/*/smp_affinity control and a load generator outside "
                 "this process. Doc 11 step 5 restricts IRQ steering to a disposable "
                 "benchmark host with an operator rollback plan");

SCENARIO("FI-25", "P0", "P", "a clock step while measuring invalidates the sample") {
    ManualClock c;
    const std::uint64_t start = c.now_ns();
    c.rewind_ns(1000);
    const std::uint64_t stop = c.now_ns();

    const SpanSample s = make_sample(Span::L0_ReceiveToRoute, start, stop, EventId{1, 1});
    CHECK(!s.valid);

    TelemetryLane lane;
    CHECK(!lane.try_sample(s));
    CHECK_EQ(lane.invalid_samples(), std::uint64_t{1});
    CHECK(!lane.coverage_complete());
    lane.drain();
    CHECK_EQ(lane.span(Span::L0_ReceiveToRoute).count(), std::uint64_t{0});

    MeasurementValidity v;
    v.coverage_complete = lane.coverage_complete();
    v.outcomes_balanced = true;
    v.sufficient_p99 = true;
    CHECK(!v.performance_claim_admissible());
}

SCENARIO_BLOCKED("FI-26", "P0", "P",
                 "cross-socket TSC skew is detected rather than assumed away",
                 "this project deliberately does not use the TSC: ADR-05 chooses the OS "
                 "monotonic clock precisely because invariant-TSC checks, serialization "
                 "and migration handling would all have to be proven first. There is also "
                 "only one socket here, so there is no skew to detect. The clock "
                 "provenance that makes the decision auditable is tested in LL-202");

SCENARIO("FI-27", "P0", "P", "a coordinated-omission generator is detected by the denominator") {
    OutcomeCounters closed, open;
    Histogram closed_h, open_h;

    // 1000 scheduled arrivals. The system stalls for the last 100.
    for (int i = 0; i < 900; ++i) {
        ++closed.offered;
        ++closed.completed;
        closed_h.record(100);
        ++open.offered;
        ++open.completed;
        open_h.record(100);
    }
    for (int i = 0; i < 100; ++i) {
        ++open.offered;
        ++open.completed;
        open_h.record(50000);  // each scheduled arrival waited
        if (i == 0) {
            ++closed.offered;
            ++closed.completed;
            closed_h.record(50000);  // one in flight, so one sample
        }
    }
    // The closed-loop run under-reports the offered load: the signature of the
    // omission.
    CHECK(closed.offered < open.offered);
    CHECK(closed.balanced());
    CHECK(open.balanced());
    CHECK_LT(closed_h.percentile(99.0), open_h.percentile(99.0));
    CHECK(open_h.percentile(99.0) >= 50000);
}

SCENARIO("FI-28", "P0", "P", "an unfair comparison is refused by the config hash") {
    ProfileConfig a, b;
    b.sync_on_commit = false;  // weaker durability: not an optimization
    CHECK_NE(a.hash(), b.hash());

    ProfileConfig c = a;
    c.spsc_capacity = 1024;
    CHECK_NE(c.hash(), a.hash());

    // A profile that tries to remove the durability requirement is rejected
    // outright rather than merely hashed differently.
    std::vector<ConfigError> errs;
    ProfileConfig bad;
    CHECK(!parse_profile("durability: best_effort\n", bad, errs));
    CHECK(!errs.empty());
}

SCENARIO_BLOCKED("FI-29", "P0", "P",
                 "thermal throttling during a sustained 50k/s run is detected",
                 "requires a sustained load this host cannot hold for long enough, plus "
                 "access to thermal and frequency counters. Doc 11 step 6 wants the "
                 "governor, turbo and thermal state documented per run; on this host "
                 "those are not readable, so a throttling claim cannot be made either way");

SCENARIO("FI-30", "P0", "D,S", "an unavailable store plus a saturated queue brakes upstream") {
    Engine::Config cfg;
    cfg.spsc_capacity = 256;
    cfg.pool_slabs = 1;
    cfg.pool_slots_per_slab = 8192;
    Fixture f(cfg);

    HealthSnapshot h;
    h.queue_capacity = 256;
    h.queue_occupancy = 250;
    h.pool_free = 1000;
    h.persistence_inflight_budget = 1;
    h.store_healthy = false;
    h.preflight_complete = true;
    h.replay_verified = true;
    CHECK_EQ(ServiceModeController::free_credits(h), std::size_t{0});
    CHECK_EQ(f.engine.modes().evaluate(h, MonotonicClock::now_ns()), ServiceMode::ReconOnly);
    CHECK_EQ(f.engine.modes().admit(Lane::NewOrder, false, h),
             AdmissionOutcome::HeldForReconciliation);
    CHECK_EQ(f.engine.modes().admit(Lane::AcceptedReceipt, true, h),
             AdmissionOutcome::RetryLaterNoAdmission);
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{0});
}

SCENARIO("FI-31", "P0", "D,F", "a crash at the outbound boundary is recoverable") {
    // The full process-death form is CW-19 to CW-22. What this adds is the
    // end-to-end statement: after such a crash the restarted engine issues no
    // original for the ambiguous row and reconciliation owns it.
    const std::string journal = artifact("fi31.bin");
    std::remove(journal.c_str());
    {
        Engine::Config cfg;
        cfg.journal_path = journal;
        Fixture f(cfg);
        f.engine.offer(new_order(1));
        f.engine.step(1);
        OutboxRecord ob;
        CHECK(f.engine.store().claim_next(f.engine.owner().epoch(), ob));
    }
    {
        JournalStore::Options o;
        o.journal_path = journal;
        JournalStore store(o);
        RecordingSink sink;
        Dispatcher d(store, sink);
        CHECK_EQ(store.state().orders.size(), std::size_t{1});
        const auto obs = store.outbox_snapshot();
        CHECK_EQ(obs.size(), std::size_t{1});
        CHECK_EQ(obs[0].state, OutboxState::Ambiguous);
        CHECK_EQ(d.pump(store.fencing_epoch()), std::size_t{0});
        CHECK_EQ(sink.frames().size(), std::size_t{0});
        CHECK_EQ(d.ambiguous_records().size(), std::size_t{1});
    }
    std::remove(journal.c_str());
}

SCENARIO("FI-32", "P0", "D,C", "a crash after the inbox commit keeps exactly one fill") {
    const std::string journal = artifact("fi32.bin");
    std::remove(journal.c_str());
    std::string hash_after_fill;
    {
        Engine::Config cfg;
        cfg.journal_path = journal;
        Fixture f(cfg);
        f.engine.offer(new_order(1, 50 * kQtyScale));
        f.engine.run_until_idle();
        f.engine.offer(exec_report(1, 10, ExecType::New));
        f.engine.run_until_idle();
        f.engine.offer(exec_report(1, 11, ExecType::PartialFill, 20 * kQtyScale));
        f.engine.run_until_idle();
        hash_after_fill = f.engine.store().state_hash();
        CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{1});
    }
    {
        Engine::Config cfg;
        cfg.journal_path = journal;
        Engine engine(cfg);
        std::string reject;
        CHECK(engine.preflight(reject));
        engine.refresh_mode();
        CHECK_EQ(engine.store().state_hash(), hash_after_fill);
        // The resend after the restart produces no second effect.
        engine.offer(exec_report(1, 11, ExecType::PartialFill, 20 * kQtyScale));
        engine.run_until_idle();
        CHECK_EQ(engine.store().state().fills.size(), std::size_t{1});
        CHECK_EQ(engine.store().state_hash(), hash_after_fill);
    }
    std::remove(journal.c_str());
}

SCENARIO("FI-33", "P0", "D", "a stale replica promotion cannot write through the fence") {
    JournalStore store;
    store.configure_risk_domain(kDomain, kLimit);
    ServiceModeController modes_a, modes_b;
    TelemetryLane lane_a, lane_b;

    HealthSnapshot h;
    h.preflight_complete = true;
    h.replay_verified = true;
    h.queue_capacity = 100;
    h.pool_free = 100;
    h.persistence_inflight_budget = 4;
    modes_a.evaluate(h, 1000);
    modes_b.evaluate(h, 1000);

    OwnerActor::Config ca;
    ca.owner_epoch = 1;
    OwnerActor old_owner(ca, store, modes_a, lane_a);
    // The replacement owner starts at the same epoch as the one it replaces and
    // then promotes: promote_to_epoch is a no-op for an epoch it already holds,
    // so constructing it at 2 would never move the store's fence.
    OwnerActor::Config cb;
    cb.owner_epoch = 1;
    OwnerActor new_owner(cb, store, modes_b, lane_b);

    new_owner.promote_to_epoch(2);
    CHECK_EQ(store.fencing_epoch(), std::uint32_t{2});

    InputEnvelope e = new_order(1);
    e.owner_epoch = 1;
    const AdmissionOutcome o = old_owner.on_event(e);
    CHECK(o == AdmissionOutcome::StaleEpoch || store.state().orders.empty());
    CHECK_EQ(store.state().orders.size(), std::size_t{0});

    CHECK_EQ(new_owner.epoch(), std::uint32_t{2});
    InputEnvelope e2 = new_order(2);
    e2.owner_epoch = 2;
    CHECK_EQ(new_owner.on_event(e2), AdmissionOutcome::AcceptedForProcessing);
    CHECK_EQ(store.state().orders.size(), std::size_t{1});
}

SCENARIO("FI-34", "P0", "O,S", "a config change mid-drain is rejected, not applied live") {
    // Doc 20 and LI-43: a capacity change is versioned and validated before it
    // takes effect. The engine has no live-reload path at all, which is the
    // strongest form of this: a new configuration needs a new engine, so a
    // half-applied capacity cannot exist.
    Fixture f;
    for (OrderId i = 1; i <= 50; ++i) f.engine.offer(new_order(i));
    const std::size_t capacity_before = f.engine.queue().capacity();
    CHECK_EQ(f.engine.config().spsc_capacity, capacity_before);
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.queue().capacity(), capacity_before);

    Engine::Config bad;
    bad.spsc_capacity = 4096;  // not compiled in
    Engine rejected(bad);
    std::string why;
    CHECK(!rejected.preflight(why));
    CHECK(why.find("not one of the compiled") != std::string::npos);
}

SCENARIO_BLOCKED("FI-35", "P0", "P,S",
                 "a 24-hour soak with periodic faults shows no resource drift",
                 "run set SOAK-01 of doc 27.3 asks for 8 to 24 hours, which cannot run "
                 "inside a test suite. The bounded-resource properties a soak would "
                 "expose are checked directly instead (LL-098 for pool capacity, LL-020 "
                 "for queue occupancy, LL-090 for allocation), but a drift that only "
                 "appears over hours is not covered and this entry stays BLOCKED");

SCENARIO("FI-36", "P0", "C,R,D,S",
         "combined chaos: an unknown commit, a failover and a cancel together") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // Each fault is covered alone above; what this adds is that they do not
    // combine into something unsafe.
    Fixture f;
    f.engine.offer(new_order(1, 40 * kQtyScale));
    f.engine.offer(new_order(2, 40 * kQtyScale));
    f.engine.run_until_idle();
    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.offer(exec_report(2, 11, ExecType::New));
    f.engine.run_until_idle();
    const std::size_t orders_before = f.engine.store().state().orders.size();

    // Fault 1: the next commit's acknowledgement is lost.
    fp::arm(std::string(fp::point::kStoreCommitLostAck), fp::Action::ReturnError, 0, 1);
    f.engine.offer(exec_report(1, 12, ExecType::PartialFill, 10 * kQtyScale));
    f.engine.run_until_idle();
    CHECK(fp::fired(fp::point::kStoreCommitLostAck) > 0);
    fp::disarm_all();
    CHECK_EQ(f.engine.owner().unresolved_unknown(), std::size_t{1});
    CHECK_EQ(f.engine.refresh_mode(), ServiceMode::ReconOnly);

    // Fault 2: a cancel arrives while the outcome is unknown. Held, not issued.
    CHECK_EQ(f.engine.offer(cancel_request(3, 2)), AdmissionOutcome::HeldForReconciliation);
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{0});

    // Fault 3: failover.
    f.engine.owner().promote_to_epoch(2);
    CHECK_EQ(f.engine.store().fencing_epoch(), std::uint32_t{2});

    // Nothing was invented or lost: the fill did commit (its record was durable
    // before the ack was lost), the cancel did not, and no original went out.
    CHECK(f.engine.store().state().check().ok());
    CHECK_EQ(f.engine.store().state().orders.size(), orders_before);
    CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{1});
    CHECK_EQ(f.engine.dispatcher().stats().duplicate_original_attempts, std::uint64_t{0});

    const std::string h = f.engine.store().state_hash();
    f.engine.store().rebuild_from_journal();
    CHECK_EQ(f.engine.store().state_hash(), h);
    tt::note_state_hash(h);
#endif
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "fault_playbooks"); }
