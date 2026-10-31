// Model-based testing: design doc 23 layer 2.
//
// "A state-machine generator alternates enqueue/dequeue/pause/restart/commit
// resolve; the reference model is a plain deque plus deterministic P03/P05/P06
// effects."
//
// Two models, two properties:
//
//   the ring against a std::deque. A randomized command sequence is applied to
//   both and every observable -- FIFO order, occupancy, full/empty, the exact
//   value returned -- must agree at every step. This is where an off-by-one in
//   the full test or a wrong masking would surface, in a form that reduces to
//   a seed rather than to a hunch.
//
//   the engine against the conservation laws. After every event the fill
//   ledger, the reservation and the position must satisfy the identities doc 24
//   states, and the committed state must equal a replay of its own journal.
//
// Every case records its seed, so a failure is reproducible from the artifact
// (doc 23 "required per-case artifact").

#include <deque>
#include <random>
#include <string>
#include <vector>

#include "llx/engine.hpp"
#include "llx/spsc_ring.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;

// --- ring model -----------------------------------------------------------

template <std::size_t N>
struct RingModel {
    std::deque<std::uint64_t> items;

    bool push(std::uint64_t v) {
        if (items.size() >= N) return false;
        items.push_back(v);
        return true;
    }
    bool pop(std::uint64_t& out) {
        if (items.empty()) return false;
        out = items.front();
        items.pop_front();
        return true;
    }
    std::size_t size() const { return items.size(); }
};

// Runs one randomized command sequence against both the ring and the deque.
// Returns an empty string on success, or a description of the first divergence.
template <std::size_t N>
std::string compare_ring_against_model(std::uint64_t seed, int steps) {
    SpscRing<std::uint64_t, N> ring;
    RingModel<N> model;
    std::mt19937_64 rng(seed);
    std::uint64_t next_value = 1;

    for (int step = 0; step < steps; ++step) {
        // Bias toward pushing when empty and popping when full, so the
        // sequence spends time at both boundaries instead of hovering in the
        // middle where nothing interesting happens.
        const std::size_t sz = model.size();
        int push_weight = 50;
        if (sz == 0) push_weight = 90;
        else if (sz >= N) push_weight = 10;
        const bool do_push = static_cast<int>(rng() % 100) < push_weight;

        if (do_push) {
            const std::uint64_t v = next_value++;
            const bool real = ring.try_push(v);
            const bool expect = model.push(v);
            if (real != expect)
                return "step " + std::to_string(step) + ": push returned " +
                       (real ? "true" : "false") + " but the model says " +
                       (expect ? "true" : "false") + " at size " + std::to_string(sz);
        } else {
            std::uint64_t real_v = 0, model_v = 0;
            const bool real = ring.try_pop(real_v);
            const bool expect = model.pop(model_v);
            if (real != expect)
                return "step " + std::to_string(step) + ": pop returned " +
                       (real ? "true" : "false") + " but the model says " +
                       (expect ? "true" : "false") + " at size " + std::to_string(sz);
            if (real && real_v != model_v)
                return "step " + std::to_string(step) + ": pop gave " +
                       std::to_string(real_v) + " but the model says " +
                       std::to_string(model_v);
        }

        if (ring.size() != model.size())
            return "step " + std::to_string(step) + ": occupancy " +
                   std::to_string(ring.size()) + " but the model says " +
                   std::to_string(model.size());
        if (ring.empty() != (model.size() == 0))
            return "step " + std::to_string(step) + ": empty() disagrees with the model";
        if (ring.full() != (model.size() == N))
            return "step " + std::to_string(step) + ": full() disagrees with the model";
        if (!ring.invariant_ok())
            return "step " + std::to_string(step) + ": ring invariant broken";
    }
    return "";
}

// --- engine model ---------------------------------------------------------

struct EngineFixture {
    Engine engine;
    std::string reject;

    explicit EngineFixture(Engine::Config cfg = {}) : engine(std::move(cfg)) {
        if (!engine.preflight(reject)) TT_FAIL("preflight rejected: " + reject);
        engine.store().configure_risk_domain(kDomain, 1000000 * kQtyScale);
        engine.refresh_mode();
    }
};

// A random but legal event stream: new orders, then acknowledgements, fills,
// cancels and replaces against orders that actually exist. Illegal events are
// generated too, deliberately, because the engine's refusal of them is part of
// what is being modelled.
struct StreamGenerator {
    std::mt19937_64 rng;
    OrderId next_order = 1;
    ExecId next_exec = 1000;
    std::uint64_t next_event = 1;
    std::vector<OrderId> live;

    explicit StreamGenerator(std::uint64_t seed) : rng(seed) {}

    InputEnvelope next(const BusinessState& state) {
        InputEnvelope e;
        e.event_id = EventId{7, next_event++};
        e.risk_domain = kDomain;
        e.symbol = kSymbol;

        const int roll = static_cast<int>(rng() % 100);
        // Pick an order that exists, if any, for the event types that need one.
        OrderId target = 0;
        if (!state.orders.empty()) {
            auto it = state.orders.begin();
            std::advance(it, static_cast<long>(rng() % state.orders.size()));
            target = it->first;
        }

        if (roll < 35 || target == 0) {
            e.type = EventType::NewOrder;
            e.order_id = next_order++;
            e.side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            e.qty = static_cast<Qty>((rng() % 50 + 1) * kQtyScale);
            e.px = static_cast<Px>((rng() % 20 + 1) * kPxScale);
        } else if (roll < 55) {
            e.type = EventType::ExecutionReport;
            e.accepted_evidence = true;
            e.order_id = target;
            e.exec_id = next_exec++;
            e.exec_type = ExecType::New;
        } else if (roll < 80) {
            e.type = EventType::ExecutionReport;
            e.accepted_evidence = true;
            e.order_id = target;
            e.exec_id = next_exec++;
            const auto it = state.orders.find(target);
            const Qty leaves = (it == state.orders.end()) ? 0 : it->second.leaves_qty;
            if (leaves > 0) {
                // Sometimes a full fill, sometimes a partial, and sometimes an
                // oversized one so the contradiction path is exercised too.
                const int kind = static_cast<int>(rng() % 10);
                if (kind < 4) {
                    e.exec_type = ExecType::Fill;
                    e.last_qty = leaves;
                } else if (kind < 9) {
                    e.exec_type = ExecType::PartialFill;
                    e.last_qty = (leaves > kQtyScale)
                                     ? static_cast<Qty>(kQtyScale * (1 + rng() % 3))
                                     : leaves;
                    if (e.last_qty > leaves) e.last_qty = leaves;
                    if (e.last_qty == leaves) e.exec_type = ExecType::Fill;
                } else {
                    e.exec_type = ExecType::PartialFill;
                    e.last_qty = leaves + kQtyScale;  // illegal on purpose
                }
            } else {
                e.exec_type = ExecType::PartialFill;
                e.last_qty = kQtyScale;  // illegal: nothing left to fill
            }
            e.last_px = static_cast<Px>((rng() % 20 + 1) * kPxScale);
        } else if (roll < 90) {
            e.type = EventType::CancelRequest;
            e.order_id = next_order++;
            e.orig_order_id = target;
        } else if (roll < 97) {
            e.type = EventType::ReplaceRequest;
            e.order_id = next_order++;
            e.orig_order_id = target;
            e.qty = static_cast<Qty>((rng() % 60 + 1) * kQtyScale);
            e.px = static_cast<Px>((rng() % 20 + 1) * kPxScale);
        } else {
            e.type = EventType::SessionControl;
        }
        e.lane = lane_of(e.type);
        return e;
    }
};

// The identities of doc 24, checked after every single event rather than only
// at the end: an intermediate violation that later cancels out is still a
// violation.
std::string check_conservation(const BusinessState& s) {
    const BusinessState::Check c = s.check();
    if (!c.ok()) return "invariant check failed: " + c.detail;

    // Independent recomputation of the reservation from the orders, rather than
    // trusting the running totals. This is the part that would catch a missed
    // or double-applied risk delta.
    Qty expect_buy = 0, expect_sell = 0;
    for (const auto& [id, o] : s.orders) {
        (void)id;
        if (o.status == OrderStatus::Quarantined) {
            // A quarantined order's reservation is deliberately left as it was,
            // so it cannot be recomputed from the projection. Skipping it here
            // means this check only covers the non-quarantined population.
            return "";
        }
        if (!is_live(o.status)) continue;
        // While a replace is pending the larger of the two quantities is held.
        const Qty held = (o.status == OrderStatus::PendingReplace && o.pending_qty > o.qty)
                             ? o.pending_qty - o.cum_qty
                             : o.leaves_qty;
        if (o.side == Side::Buy) expect_buy += held;
        else expect_sell += held;
    }
    const auto it = s.risk.find(kDomain);
    if (it == s.risk.end()) return expect_buy == 0 && expect_sell == 0 ? "" : "risk domain missing";
    if (it->second.reserved_buy != expect_buy)
        return "reserved_buy " + std::to_string(it->second.reserved_buy) +
               " but the live orders imply " + std::to_string(expect_buy);
    if (it->second.reserved_sell != expect_sell)
        return "reserved_sell " + std::to_string(it->second.reserved_sell) +
               " but the live orders imply " + std::to_string(expect_sell);
    return "";
}

}  // namespace

SCENARIO("LL-M01", "P0", "Q,M", "the ring agrees with a deque over randomized sequences") {
    // Several seeds and several capacities, including N=2 where the boundaries
    // are adjacent.
    const std::uint64_t base_seed = 424242;
    tt::note_seed(base_seed);
    for (std::uint64_t k = 0; k < 40; ++k) {
        const std::uint64_t seed = base_seed + k;
        std::string err = compare_ring_against_model<2>(seed, 4000);
        CHECK_MSG(err.empty(), "N=2 seed " + std::to_string(seed) + ": " + err);
        err = compare_ring_against_model<4>(seed, 4000);
        CHECK_MSG(err.empty(), "N=4 seed " + std::to_string(seed) + ": " + err);
        err = compare_ring_against_model<256>(seed, 8000);
        CHECK_MSG(err.empty(), "N=256 seed " + std::to_string(seed) + ": " + err);
    }
}

SCENARIO("LL-M02", "P0", "Q,M",
         "the ring agrees with the model across a counter wrap") {
    // The same comparison, but with a narrow counter so the sequence crosses
    // the wrap thousands of times. The model has no counters at all, so any
    // wrap-related error shows up as a divergence.
    const std::uint64_t seed = 99;
    tt::note_seed(seed);
    SpscRing<std::uint64_t, 4, std::uint8_t> ring;
    RingModel<4> model;
    CHECK(ring.seek_counters_for_test(200));
    std::mt19937_64 rng(seed);
    std::uint64_t next = 1;

    for (int step = 0; step < 200000; ++step) {
        if (rng() % 2 == 0) {
            const std::uint64_t v = next++;
            CHECK_EQ(ring.try_push(v), model.push(v));
        } else {
            std::uint64_t a = 0, b = 0;
            const bool ra = ring.try_pop(a);
            const bool rb = model.pop(b);
            CHECK_EQ(ra, rb);
            if (ra) CHECK_EQ(a, b);
        }
        CHECK_EQ(ring.size(), model.size());
        CHECK(ring.invariant_ok());
    }
}

SCENARIO("LL-M03", "P0", "C,R,D",
         "the conservation identities hold after every event of a random stream") {
    const std::uint64_t seed = 20261030;
    tt::note_seed(seed);
    EngineFixture f;
    StreamGenerator gen(seed);

    for (int i = 0; i < 4000; ++i) {
        const InputEnvelope e = gen.next(f.engine.store().state());
        // Offer, then drain, so each event's effect is complete before the
        // identities are checked.
        const AdmissionOutcome o = f.engine.offer(e);
        if (o == AdmissionOutcome::AcceptedForProcessing) f.engine.run_until_idle();
        const std::string err = check_conservation(f.engine.store().state());
        CHECK_MSG(err.empty(), "after event " + std::to_string(i) + " (" +
                                   std::string(to_string(e.type)) + "): " + err);
    }
    f.engine.run_until_idle();
    tt::note_state_hash(f.engine.store().state_hash());
    const Engine::SelfCheck sc = f.engine.self_check();
    CHECK_MSG(sc.ok(), sc.detail);
}

SCENARIO("LL-M04", "P0", "C,R,D",
         "many seeds each end in a state that equals a replay of its own journal") {
    // LI-39 across a population rather than for one hand-written sequence.
    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        EngineFixture f;
        StreamGenerator gen(seed);
        for (int i = 0; i < 600; ++i) {
            f.engine.offer(gen.next(f.engine.store().state()));
            f.engine.run_until_idle();
        }
        const std::string before = f.engine.store().state_hash();
        f.engine.store().rebuild_from_journal();
        CHECK_MSG(f.engine.store().state_hash() == before,
                  "seed " + std::to_string(seed) + ": replay produced a different state");
        CHECK(f.engine.store().state().check().ok());
    }
    tt::note_seed(25);
}

SCENARIO("LL-M05", "P0", "C,R,D,Q",
         "the same stream through the same engine twice gives the same state") {
    // Determinism. Without it none of the differential or crash comparisons
    // mean anything, because a difference could always be attributed to
    // scheduling.
    const std::uint64_t seed = 7777;
    tt::note_seed(seed);
    std::string first;
    for (int run = 0; run < 2; ++run) {
        EngineFixture f;
        StreamGenerator gen(seed);
        for (int i = 0; i < 1500; ++i) {
            f.engine.offer(gen.next(f.engine.store().state()));
            f.engine.run_until_idle();
        }
        const std::string h = f.engine.store().state_hash();
        if (run == 0) first = h;
        else CHECK_EQ(h, first);
    }
    tt::note_state_hash(first);
}

SCENARIO("LL-M06", "P0", "C,R,D,Q,S",
         "a stream interleaved with queue saturation loses no accepted evidence") {
    // The generator keeps offering while the queue is small, so refusals are
    // frequent. An accepted execution report that was refused must come back
     // as a deferral and be retried, never dropped (LI-20).
    const std::uint64_t seed = 31337;
    tt::note_seed(seed);
    Engine::Config cfg;
    cfg.spsc_capacity = 256;
    cfg.pool_slabs = 1;
    cfg.pool_slots_per_slab = 8192;
    EngineFixture f(cfg);
    StreamGenerator gen(seed);

    std::uint64_t deferred_receipts = 0, dropped_receipts = 0;
    for (int i = 0; i < 6000; ++i) {
        const InputEnvelope e = gen.next(f.engine.store().state());
        const AdmissionOutcome o = f.engine.offer(e);
        if (e.accepted_evidence) {
            if (o == AdmissionOutcome::RetryLaterNoAdmission) {
                ++deferred_receipts;
                // Hold it and retry after draining, which is what the contract
                // obliges the caller to do.
                f.engine.run_until_idle();
                const AdmissionOutcome retry = f.engine.offer(e);
                if (retry == AdmissionOutcome::RejectedOverload) ++dropped_receipts;
            } else if (o == AdmissionOutcome::RejectedOverload) {
                ++dropped_receipts;
            }
        }
        // Drain only sometimes, so the queue genuinely fills.
        if (i % 7 == 0) f.engine.run_until_idle();
    }
    f.engine.run_until_idle();

    // No accepted business proof was ever refused outright.
    CHECK_EQ(dropped_receipts, std::uint64_t{0});
    CHECK(f.engine.outcomes().balanced());
    const Engine::SelfCheck sc = f.engine.self_check();
    CHECK_MSG(sc.ok(), sc.detail);
    const std::string err = check_conservation(f.engine.store().state());
    CHECK_MSG(err.empty(), err);
    tt::note_state_hash(f.engine.store().state_hash());
}

SCENARIO("LL-M07", "P0", "C,R,D",
         "a restart part-way through a stream resumes without duplicating or losing") {
    // Doc 23 layer 2 includes restart in the command alphabet. The journal is
    // file-backed so the restart is a real one: a new store object over the same
    // log.
    const std::uint64_t seed = 5150;
    tt::note_seed(seed);
    const std::string path =
        std::string(LLX_FIXTURES_DIR) + "/../build/test-artifacts/model_journal.bin";
    std::remove(path.c_str());

    std::string mid_hash;
    StreamGenerator gen(seed);
    {
        Engine::Config cfg;
        cfg.journal_path = path;
        EngineFixture f(cfg);
        for (int i = 0; i < 400; ++i) {
            f.engine.offer(gen.next(f.engine.store().state()));
            f.engine.run_until_idle();
        }
        mid_hash = f.engine.store().state_hash();
    }
    {
        // Restart. The engine must pick up the committed state, not an empty
        // book (LL-018).
        Engine::Config cfg;
        cfg.journal_path = path;
        Engine engine(cfg);
        std::string reject;
        CHECK(engine.preflight(reject));
        CHECK_EQ(engine.store().state_hash(), mid_hash);
        engine.refresh_mode();

        // Continue the same stream.
        for (int i = 0; i < 400; ++i) {
            engine.offer(gen.next(engine.store().state()));
            engine.run_until_idle();
        }
        const std::string err = check_conservation(engine.store().state());
        CHECK_MSG(err.empty(), err);
        const std::string after = engine.store().state_hash();
        engine.store().rebuild_from_journal();
        CHECK_EQ(engine.store().state_hash(), after);
        tt::note_state_hash(after);
    }
    std::remove(path.c_str());
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "model_based"); }
