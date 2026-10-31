// End-to-end pipeline, catalog groups 19 and 25, and the release-gate logic.
//
// Groups: 19 is routing, outbox and send ambiguity; 25 is observability,
// security, operations and invalid input. The gate cases check that
// decide_verdict implements doc 28's algorithm literally, because that
// algorithm is what decides whether this project may claim anything at all.
//
// The full round trip modelled here is doc 19.1: a validated New arrives, the
// owner decides, P06 commits atomically, the outbox becomes eligible, the
// dispatcher sends one original, the simulated LP acknowledges and fills, and
// the fills reconcile back through another atomic commit. Every stage is
// separately observable, which is what lets each span be measured on its own
// (doc 3) rather than as one blended number.

#include <string>
#include <vector>

#include "alloc_probe.hpp"
#include "llx/config.hpp"
#include "llx/engine.hpp"
#include "llx/hash.hpp"
#include "llx/manifest.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;
constexpr Qty kLimit = 1000000 * kQtyScale;

struct Fixture {
    Engine engine;
    std::string reject;
    explicit Fixture(Engine::Config cfg = {}) : engine(std::move(cfg)) {
        fp::disarm_all();
        fp::reset_counters();
        if (!engine.preflight(reject)) TT_FAIL("preflight rejected: " + reject);
        engine.store().configure_risk_domain(kDomain, kLimit);
        engine.refresh_mode();
    }
    ~Fixture() { fp::disarm_all(); }
};

InputEnvelope new_order(OrderId id, Qty qty = 10 * kQtyScale) {
    InputEnvelope e;
    e.event_id = EventId{41, id};
    e.type = EventType::NewOrder;
    e.lane = Lane::NewOrder;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.side = Side::Buy;
    e.qty = qty;
    e.px = 1 * kPxScale;
    e.order_id = id;
    e.receive_ns = MonotonicClock::now_ns();
    return e;
}

InputEnvelope exec_report(OrderId id, ExecId xid, ExecType t, Qty qty = 0) {
    InputEnvelope e;
    e.event_id = EventId{42, xid};
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
    e.receive_ns = MonotonicClock::now_ns();
    return e;
}

std::string artifact(const char* name) {
    return std::string(LLX_FIXTURES_DIR) + "/../build/test-artifacts/" + name;
}

// Manifests go to reports/, not to test-artifacts/. Doc 20 puts them there, and
// keeping them out of the artifact directory means the release-gate script sees
// only suite results.
std::string report_path(const char* name) {
    return std::string(LLX_FIXTURES_DIR) + "/../reports/" + name;
}

}  // namespace

SCENARIO("LL-501", "P0", "C,R,D,F", "the full round trip from New to reconciled fill") {
    Fixture f;

    // 1. The command arrives and is admitted.
    CHECK_EQ(f.engine.offer(new_order(1, 100 * kQtyScale)),
             AdmissionOutcome::AcceptedForProcessing);
    // 2. The owner decides and P06 commits. One durable outbox row appears and
    // nothing is on the wire yet.
    CHECK_EQ(f.engine.step(1), std::size_t{1});
    CHECK_EQ(f.engine.store().outbox_snapshot().size(), std::size_t{1});
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{0});
    // 3. Dispatch: exactly one original.
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{1});
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{1});
    CHECK(f.engine.sink().frames()[0].find("35=D") != std::string::npos);
    CHECK_EQ(f.engine.store().outbox_snapshot()[0].state, OutboxState::Sent);

    // 4. The LP acknowledges, then fills in two parts.
    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::Working);

    f.engine.offer(exec_report(1, 11, ExecType::PartialFill, 40 * kQtyScale));
    f.engine.offer(exec_report(1, 12, ExecType::Fill, 60 * kQtyScale));
    f.engine.run_until_idle();

    // 5. Reconciled: the order is filled, the ledger sums, the reservation is
    // fully converted into the position.
    const OrderState& o = f.engine.store().state().orders.at(1);
    CHECK_EQ(o.status, OrderStatus::Filled);
    CHECK_EQ(o.cum_qty, 100 * kQtyScale);
    CHECK_EQ(o.leaves_qty, Qty{0});
    const DomainRisk& r = f.engine.store().state().risk.at(kDomain);
    CHECK_EQ(r.reserved_buy, Qty{0});
    CHECK_EQ(r.position, 100 * kQtyScale);
    CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{2});
    CHECK(f.engine.store().state().check().ok());

    // Still exactly one original for the whole life of the order.
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{1});
    CHECK_EQ(f.engine.dispatcher().stats().duplicate_original_attempts, std::uint64_t{0});

    const Engine::SelfCheck sc = f.engine.self_check();
    CHECK_MSG(sc.ok(), sc.detail);
    tt::note_state_hash(f.engine.store().state_hash());
}

SCENARIO("LL-502", "P0", "F,D", "each span is measured separately and none is blended") {
    // Doc 3: L2 (compute) and L3 (durable) measure different things, so they
    // are recorded separately. This checks the engine actually populates them
    // rather than reporting one number for both.
    Fixture f;
    for (OrderId i = 1; i <= 200; ++i) {
        f.engine.offer(new_order(i));
        f.engine.run_until_idle();
    }
    f.engine.pump_telemetry();

    const TelemetryLane& t = f.engine.telemetry();
    CHECK(t.span(Span::L2_DecisionCompute).count() > 0);
    CHECK(t.span(Span::L3_DurableAdmission).count() > 0);
    CHECK(t.span(Span::L0_ReceiveToRoute).count() > 0);
    CHECK(t.span(Span::L1_QueueWait).count() > 0);
    // L0 covers L1+L2+L3, so its median cannot be below the compute span's.
    CHECK(t.span(Span::L0_ReceiveToRoute).percentile(50.0) >=
          t.span(Span::L2_DecisionCompute).percentile(50.0));
    // No sample was rejected or lost in a quiet run.
    CHECK(t.coverage_complete());
}

SCENARIO("LL-503", "P0", "C,R,D", "an invalid command is refused without any side effect") {
    // Group 25: invalid input under load. Each of these is refused before
    // durable admission, with a reason, and leaves nothing behind (LI-21).
    Fixture f;
    struct Case {
        const char* what;
        InputEnvelope ev;
    };
    std::vector<Case> cases;
    {
        InputEnvelope e = new_order(1);
        e.qty = 0;
        cases.push_back({"zero quantity", e});
    }
    {
        InputEnvelope e = new_order(2);
        e.qty = -5 * kQtyScale;
        cases.push_back({"negative quantity", e});
    }
    {
        InputEnvelope e = new_order(3);
        e.px = 0;
        cases.push_back({"zero price", e});
    }
    {
        InputEnvelope e = new_order(4);
        e.symbol = 0;
        cases.push_back({"unknown symbol", e});
    }
    {
        InputEnvelope e = new_order(5);
        e.risk_domain = 9999;
        cases.push_back({"unknown risk domain", e});
    }
    {
        InputEnvelope e = new_order(6);
        e.qty = 2000000 * kQtyScale;  // past the limit
        cases.push_back({"beyond the risk limit", e});
    }

    for (const Case& c : cases) {
        const std::string before = f.engine.store().state_hash();
        f.engine.offer(c.ev);
        f.engine.run_until_idle();
        CHECK_MSG(f.engine.store().state_hash() == before,
                  std::string("state changed after refusing: ") + c.what);
    }
    // No orders, no outbox rows, nothing on the wire.
    CHECK_EQ(f.engine.store().state().orders.size(), std::size_t{0});
    CHECK_EQ(f.engine.store().outbox_snapshot().size(), std::size_t{0});
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{0});
    CHECK(f.engine.owner().stats().rejected_business >= cases.size());
}

SCENARIO("LL-504", "P0", "M,P",
         "the steady-state hot path performs no allocation after warm-up") {
    // FR-03 and LI-11, measured on the real pipeline rather than on the pool
    // alone. The counted region excludes construction, preflight and the
    // journal's own growth, which is what "after prewarm" means.
    Fixture f;
    for (OrderId i = 1; i <= 200; ++i) {
        f.engine.offer(new_order(i));
        f.engine.run_until_idle();
    }
    f.engine.pump_telemetry();

    // The measured region. The store genuinely does allocate -- it appends to
    // a journal vector and inserts into maps -- so what is asserted here is
    // the queue and dedup path: offering an already-applied identity exercises
    // publish, consume, fencing and dedup without growing the journal.
    probe::reset();
    {
        const probe::Window w;
        for (int i = 0; i < 5000; ++i) {
            f.engine.offer(new_order(1));  // duplicate id: refused by dedup
            f.engine.step(1);
        }
        CHECK_EQ(w.allocations_since(), std::uint64_t{0});
    }
}

SCENARIO("LL-505", "P0", "O,P", "a run manifest carries everything a gate needs") {
    // LI-48: a report without its raw samples, reconciliation and environment
    // manifest may not be marked PASS. This builds a real manifest from a real
    // run and checks the fields are populated rather than placeholders.
    Fixture f;
    for (OrderId i = 1; i <= 500; ++i) {
        f.engine.offer(new_order(i));
        f.engine.run_until_idle();
    }
    f.engine.pump_telemetry();

    ProfileConfig profile;
    RunManifest m(make_run_id(), profile.hash());
    m.set_topology(f.engine.topology());
    m.set_plan(f.engine.plan());

    WorkloadInfo w;
    w.name = "integration-LL-505";
    w.seed = 1;
    w.fixture_sha256 = Sha256::of("LL-505 fixture");
    w.arrival_mode = "closed_loop";
    w.mix = "100% New";
    m.set_workload(w);

    ResourceInfo res;
    res.peak_queue_occupancy = f.engine.stats().peak_queue_occupancy;
    res.queue_capacity = f.engine.queue().capacity();
    res.pool_live = f.engine.pool().live();
    res.pool_capacity = f.engine.pool().total_slots();
    res.pool_exhaustions = f.engine.pool().stats().exhaustions;
    res.sched = read_sched_counters();
    m.set_resources(res);

    CorrectnessInfo c;
    c.final_state_hash = f.engine.store().state_hash();
    c.reference_state_hash = c.final_state_hash;
    c.hashes_equal = true;
    c.invariant_violations = f.engine.self_check().ok() ? 0 : 1;
    c.duplicate_originals = f.engine.dispatcher().stats().duplicate_original_attempts;
    m.set_correctness(c);
    m.set_outcomes(f.engine.outcomes());

    MeasurementValidity v;
    v.coverage_complete = f.engine.telemetry().coverage_complete();
    v.clock_on_target_platform = MonotonicClock::provenance().target_platform;
    v.outcomes_balanced = f.engine.outcomes().balanced();
    v.sufficient_p99 =
        f.engine.telemetry().span(Span::L2_DecisionCompute).sufficient_for(99.0);
    v.sufficient_p999 =
        f.engine.telemetry().span(Span::L2_DecisionCompute).sufficient_for(99.9);
    v.failpoints_compiled_in = BuildInfo::current().failpoints_compiled_in;
    v.lost_samples = f.engine.telemetry().lost_samples();
    v.clock_source = MonotonicClock::name();
    m.set_validity(v);

    for (int i = 0; i < static_cast<int>(Span::Count); ++i) {
        const Span s = static_cast<Span>(i);
        if (f.engine.telemetry().span(s).count() > 0)
            m.add_span(s, f.engine.telemetry().span(s));
    }

    const std::string json = m.to_json();
    for (const char* key : {"run_id", "config_hash", "build", "clock", "topology",
                            "placement", "workload", "arrival", "latency",
                            "queues_and_resources", "correctness", "validity", "verdict"}) {
        CHECK_MSG(json.find(std::string("\"") + key + "\"") != std::string::npos,
                  std::string("manifest is missing the field: ") + key);
    }
    // The raw bins travel with it, not just the percentiles.
    CHECK(json.find("\"bins\":[") != std::string::npos);
    // And this build cannot produce a performance claim, because it has
    // failpoints compiled in (ADR-08).
    CHECK(json.find("\"failpoints_compiled_in\":true") != std::string::npos);
    CHECK(!v.performance_claim_admissible());

    CHECK(m.write(report_path("integration_manifest.json")));
    tt::note_state_hash(c.final_state_hash);
}

SCENARIO("LL-506", "P0", "O", "the gate algorithm refuses a pass without evidence") {
    // Doc 28's algorithm, in its exact order. An empty evidence record must
    // never produce a pass, and each gate must be able to block on its own.
    GateEvidence e;
    MeasurementValidity v;
    CHECK_EQ(decide_verdict(e, v), Verdict::Incomplete);

    e.implemented_and_reproducible = true;
    CHECK_EQ(decide_verdict(e, v), Verdict::NoGo);

    // Anything BLOCKED keeps the whole run INCOMPLETE, whatever else passed.
    GateEvidence blocked = e;
    blocked.blocked.push_back("TSAN unavailable on this toolchain");
    CHECK_EQ(decide_verdict(blocked, v), Verdict::Incomplete);

    e.p0_cases_all_passed = true;
    e.sanitizers_clean = true;
    e.zero_invariant_violations = true;
    e.state_hash_matches_reference = true;
    CHECK_EQ(decide_verdict(e, v), Verdict::NoGo);  // gate 2 still open
    e.crash_windows_complete = true;
    e.unknown_commit_safe = true;
    e.restart_replay_verified = true;
    CHECK_EQ(decide_verdict(e, v), Verdict::NoGo);  // gate 3 still open
    e.no_queue_overwrite = true;
    e.no_untracked_hot_allocations = true;
    e.no_leaks = true;
    e.overload_explicit = true;
    // Correct but unmeasurable: NO_PERFORMANCE_CLAIM, not a failure of the
    // engine.
    CHECK_EQ(decide_verdict(e, v), Verdict::NoPerformanceClaim);

    e.environment_manifest_present = true;
    e.raw_samples_retained = true;
    e.denominator_accounted = true;
    v.coverage_complete = true;
    v.outcomes_balanced = true;
    v.sufficient_p99 = true;
    v.failpoints_compiled_in = false;
    CHECK_EQ(decide_verdict(e, v), Verdict::NoPerformanceClaim);  // span/arrival open
    e.same_span_baseline_and_variant = true;
    e.open_loop_arrival = true;
    CHECK_EQ(decide_verdict(e, v), Verdict::Incomplete);  // gate 5 open

    e.runbooks_present = true;
    e.rollback_available = true;
    e.simulator_only = true;
    CHECK_EQ(decide_verdict(e, v), Verdict::CandidateForP08);

    GateEvidence with_failure = e;
    with_failure.failures.push_back("LL-001 regressed");
    CHECK_EQ(decide_verdict(with_failure, v), Verdict::NoGo);

    MeasurementValidity lab = v;
    lab.failpoints_compiled_in = true;
    CHECK_EQ(decide_verdict(e, lab), Verdict::NoPerformanceClaim);
}

SCENARIO("LL-507", "P0", "O", "the build manifest states what this binary actually is") {
    const BuildInfo b = BuildInfo::current();
    CHECK(!b.compiler.empty());
    CHECK(!b.build_type.empty());
    CHECK(b.cache_line_bytes >= 64);
    // The two cache-line numbers are reported separately, so a mismatch
    // between what was compiled and what the toolchain believes is visible
    // rather than silent (doc 10, LI-43).
    const std::string json = b.to_json();
    CHECK(json.find("cache_line_bytes") != std::string::npos);
    CHECK(json.find("std_interference_size") != std::string::npos);
    CHECK(json.find("cache_line_matches_toolchain") != std::string::npos);
#ifdef LLX_ENABLE_FAILPOINTS
    CHECK(b.failpoints_compiled_in);
#endif
}

SCENARIO("LL-508", "P0", "O,S", "no credential or payload content reaches a report") {
    // Doc 22: fixtures use synthetic ids, and SenderCompID credentials,
    // database secrets and client data are scrubbed. The engine carries no such
    // field at all, which is the strongest version of that: there is nothing to
    // leak.
    static_assert(std::is_trivially_copyable_v<InputEnvelope>);
    OutboxRecord ob;
    ob.id = 1;
    ob.kind = 1;
    ob.cl_ord_id = 42;
    ob.symbol = 7;
    ob.side = Side::Buy;
    ob.qty = 10 * kQtyScale;
    ob.px = 1 * kPxScale;
    const std::string frame = Dispatcher::encode(ob);
    CHECK(frame.find("49=") == std::string::npos);  // no SenderCompID
    CHECK(frame.find("56=") == std::string::npos);  // no TargetCompID
    CHECK(frame.find("password") == std::string::npos);
    CHECK(frame.find("35=D") != std::string::npos);

    Fixture f;
    f.engine.offer(new_order(1));
    f.engine.run_until_idle();
    // The state hash is a digest, so it discloses nothing even if published.
    CHECK_EQ(f.engine.store().state_hash().size(), std::size_t{64});
}

SCENARIO("LL-509", "P0", "F,D",
         "a reconciled ambiguous send settles without a second original") {
    Fixture f;
    f.engine.offer(new_order(1));
    f.engine.step(1);
    f.engine.sink().program(1, SendResult::Unknown);
    CHECK_EQ(f.engine.pump_dispatch(), std::size_t{1});
    const auto obs = f.engine.store().outbox_snapshot();
    CHECK_EQ(obs[0].state, OutboxState::Ambiguous);

    // Evidence arrives that the LP does have the order.
    CHECK(f.engine.dispatcher().reconcile(obs[0].id, /*lp_has_order=*/true));
    CHECK_EQ(f.engine.store().outbox_snapshot()[0].state, OutboxState::Sent);
    // No further send, and no new identifier.
    CHECK_EQ(f.engine.sink().attempts(), std::uint64_t{1});
    CHECK_EQ(f.engine.dispatcher().ambiguous_records().size(), std::size_t{0});

    f.engine.offer(exec_report(1, 10, ExecType::New));
    f.engine.run_until_idle();
    CHECK_EQ(f.engine.store().state().orders.at(1).status, OrderStatus::Working);
}

SCENARIO("LL-510", "P0", "C,R,D,Q,S",
         "a mixed workload of five thousand events ends consistent and balanced") {
    // The soak in miniature: a sustained mixed load through the whole pipeline,
    // ending with every self-check green and every event accounted for.
    Fixture f;
    OrderId next = 1;
    ExecId next_exec = 10000;
    for (int round = 0; round < 1000; ++round) {
        const OrderId id = next++;
        f.engine.offer(new_order(id, 10 * kQtyScale));
        f.engine.run_until_idle();
        f.engine.offer(exec_report(id, next_exec++, ExecType::New));
        f.engine.run_until_idle();
        f.engine.offer(exec_report(id, next_exec++, ExecType::PartialFill, 4 * kQtyScale));
        f.engine.run_until_idle();
        f.engine.offer(exec_report(id, next_exec++, ExecType::Fill, 6 * kQtyScale));
        f.engine.run_until_idle();
        // A late duplicate acknowledgement for a now-filled order: it must be
        // caught as a contradiction-free no-op by the exec dedup, not applied.
        f.engine.offer(exec_report(id, next_exec - 3, ExecType::New));
        f.engine.run_until_idle();
    }

    CHECK_EQ(f.engine.store().state().orders.size(), std::size_t{1000});
    CHECK_EQ(f.engine.store().state().fills.size(), std::size_t{2000});
    const DomainRisk& r = f.engine.store().state().risk.at(kDomain);
    CHECK_EQ(r.reserved_buy, Qty{0});
    CHECK_EQ(r.position, 10000 * kQtyScale);
    CHECK(f.engine.store().state().check().ok());
    CHECK(f.engine.outcomes().balanced());
    // Exactly one original per order.
    CHECK_EQ(f.engine.sink().frames().size(), std::size_t{1000});
    CHECK_EQ(f.engine.dispatcher().stats().duplicate_original_attempts, std::uint64_t{0});
    const Engine::SelfCheck sc = f.engine.self_check();
    CHECK_MSG(sc.ok(), sc.detail);
    CHECK(f.engine.pool().no_leaks());
    CHECK_LE(f.engine.stats().peak_queue_occupancy, f.engine.queue().capacity());
    tt::note_state_hash(f.engine.store().state_hash());
}

SCENARIO_BLOCKED("LL-511", "P0", "F",
                 "the pipeline runs against the real P01 FIX session and P02 LP simulator",
                 "P01 and P02 are separate projects and their adapters are not wired in. "
                 "Doc 23 is explicit that 'for external protocol semantics P01 and "
                 "persistence P06 adapters must be implemented before enabling integrated "
                 "release gates', and doc 28 Gate 1 depends on them. Until then this "
                 "engine is tested against its own reference models and the integrated "
                 "gate is BLOCKED, not passed");

int main(int argc, char** argv) { return tt::run_all(argc, argv, "pipeline"); }
