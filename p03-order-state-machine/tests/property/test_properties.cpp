// Property-based tests (design doc 20.3).
//
// The properties are stated over whole generated histories rather than over
// one hand-picked timeline, and the comparison is always of canonical business
// state: never of ingest time, queue depth or counters.
#include <random>
#include <set>

#include "../support/lab.hpp"

using namespace lab;

namespace {

// Generates plausible histories for one order. It keeps a shadow model only to
// choose events that are worth generating; it never asserts anything from it,
// because deciding what is legal is the system under test's job.
class TraceGen {
  public:
    explicit TraceGen(uint64_t seed) : rng_(seed) {}

    // `noise` is the chance of emitting something adversarial: a duplicate, a
    // malformed number, an out-of-lineage identifier.
    std::vector<OrderEvent> generate(size_t steps, double noise) {
        std::vector<OrderEvent> out;
        out.push_back(OrderEvent{cmd_new("O1", std::to_string(order_qty_), "1.08500")});
        out.push_back(OrderEvent{sys(SystemKind::Dispatched, "O1")});
        out.push_back(OrderEvent{new_ack("O1").r});
        acked_ = true;

        for (size_t i = 0; i < steps; ++i) {
            if (chance(noise)) {
                out.push_back(noisy());
                continue;
            }
            out.push_back(legitimate());
        }
        return out;
    }

  private:
    bool chance(double p) {
        std::uniform_real_distribution<double> d(0.0, 1.0);
        return d(rng_) < p;
    }
    int pick(int n) {
        std::uniform_int_distribution<int> d(0, n - 1);
        return d(rng_);
    }

    OrderEvent legitimate() {
        // Choose among the things that make sense next. The weights matter
        // only for coverage, not for correctness.
        const int roll = pick(10);
        if (roll < 5 && cum_ + fill_step_ <= order_qty_) {
            cum_ += fill_step_;
            const std::string id = "X" + std::to_string(++execs_);
            return OrderEvent{trade(id, accepted_)
                                  .lastq(std::to_string(fill_step_))
                                  .lastpx("1.08490")
                                  .cum(std::to_string(cum_))
                                  .r};
        }
        if (roll < 7 && !pending_) {
            pending_ = true;
            pending_cancel_ = true;
            pending_id_ = "C" + std::to_string(++requests_);
            return OrderEvent{cmd_cancel(pending_id_, accepted_)};
        }
        if (roll < 8 && pending_ && pending_cancel_) {
            return OrderEvent{pending_cancel(pending_id_).r};
        }
        if (roll < 9 && !pending_ && cum_ < order_qty_) {
            pending_ = true;
            pending_cancel_ = false;
            pending_id_ = "R" + std::to_string(++requests_);
            desired_qty_ = order_qty_ + fill_step_;
            return OrderEvent{cmd_replace(pending_id_, accepted_, std::to_string(desired_qty_))};
        }
        if (pending_ && !pending_cancel_) {
            const std::string new_accepted = pending_id_;
            pending_ = false;
            order_qty_ = desired_qty_;
            accepted_ = new_accepted;
            return OrderEvent{replaced(new_accepted, previous_accepted())
                                  .oqty(std::to_string(order_qty_))
                                  .cum(std::to_string(cum_))
                                  .r};
        }
        if (pending_ && pending_cancel_) {
            pending_ = false;
            return OrderEvent{cancel_reject(pending_id_, accepted_, "1").r};
        }
        return OrderEvent{status_snapshot(accepted_, "ST" + std::to_string(++execs_))
                              .status(cum_ > 0 ? ord_status::kPartiallyFilled : ord_status::kNew)
                              .cum(std::to_string(cum_))
                              .r};
    }

    OrderEvent noisy() {
        switch (pick(6)) {
            case 0:  // a duplicate of an execution already emitted
                return OrderEvent{trade("X" + std::to_string(execs_ > 0 ? execs_ : 1), accepted_)
                                      .lastq(std::to_string(fill_step_))
                                      .lastpx("1.08490")
                                      .cum(std::to_string(cum_))
                                      .r};
            case 1:  // a malformed quantity
                return OrderEvent{trade("N" + std::to_string(++execs_), accepted_)
                                      .lastq("not-a-number")
                                      .lastpx("1.08490")
                                      .r};
            case 2:  // an overfill
                return OrderEvent{trade("N" + std::to_string(++execs_), accepted_)
                                      .lastq(std::to_string(order_qty_ * 2))
                                      .lastpx("1.08490")
                                      .r};
            case 3:  // wrong lineage
                return OrderEvent{trade("N" + std::to_string(++execs_), "GHOST")
                                      .lastq(std::to_string(fill_step_))
                                      .lastpx("1.08490")
                                      .r};
            case 4:  // a FIX 4.2 ExecType
                return OrderEvent{trade("N" + std::to_string(++execs_), accepted_)
                                      .etype("1")
                                      .lastq(std::to_string(fill_step_))
                                      .lastpx("1.08490")
                                      .r};
            default:  // an unsupported correction
                return OrderEvent{trade_cancel("H" + std::to_string(++execs_), accepted_)
                                      .ref("X1")
                                      .lastq(std::to_string(fill_step_))
                                      .r};
        }
    }

    std::string previous_accepted() const { return accepted_; }

    std::mt19937_64 rng_;
    int64_t order_qty_ = 100000;
    int64_t fill_step_ = 10000;
    int64_t cum_ = 0;
    int64_t desired_qty_ = 0;
    std::string accepted_ = "O1";
    std::string pending_id_;
    bool pending_ = false;
    bool pending_cancel_ = false;
    bool acked_ = false;
    int execs_ = 0;
    int requests_ = 0;
};

// Runs a trace and returns the chain, asserting the safety set after every
// single event rather than only at the end.
OrderId run_checked(Lab& L, const std::vector<OrderEvent>& trace) {
    OrderId id;
    for (const OrderEvent& e : trace) {
        const EngineResult r = L.engine().submit(e);
        if (r.id.valid()) id = r.id;
        if (!r.id.valid()) continue;
        const OrderAggregate* a = L.engine().find(r.id);
        if (a == nullptr) continue;
        const std::vector<InvariantViolation> v = check_invariants(*a, L.policy());
        CHECK_MSG(v.empty(), "after " + std::string(to_string(r.decision)) + ": " + describe(v));
    }
    return id;
}

}  // namespace

SCENARIO("PROP-01", "P0", "replay from the journal reproduces the canonical state") {
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        Lab L;
        TraceGen gen(seed);
        const OrderId id = run_checked(L, gen.generate(30, 0.2));
        if (!id.valid()) continue;

        const uint64_t before = L.state(id).state_hash();
        const RecoveryReport rep = L.engine().recover();
        CHECK_MSG(rep.status == RecoveryStatus::Clean,
                  "seed " + std::to_string(seed) + ": " + rep.detail);
        CHECK_MSG(L.state(id).state_hash() == before,
                  "seed " + std::to_string(seed) + ": hash changed across replay");
    }
}

SCENARIO("PROP-02", "P0", "replay from a snapshot plus the suffix equals a full replay") {
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        Lab L;
        TraceGen gen(seed);
        const OrderId id = run_checked(L, gen.generate(20, 0.15));
        if (!id.valid()) continue;
        const uint64_t expected = L.state(id).state_hash();

        // Checkpoint here, add nothing, and recover: the snapshot path and the
        // journal path must agree.
        std::map<uint64_t, AggregateRow> live;
        live[id.value] = AggregateRow{L.state(id).revision, L.state(id)};
        L.store().write_snapshot(live, L.store().journal().size(), L.policy().policy_version);

        const RecoveryReport rep = L.engine().recover();
        CHECK_MSG(rep.status == RecoveryStatus::Clean, rep.detail);
        CHECK_MSG(L.state(id).state_hash() == expected,
                  "seed " + std::to_string(seed) + ": snapshot path diverged");
        CHECK_EQ(rep.records_replayed, size_t{0});
    }
}

SCENARIO("PROP-03", "P0", "CumQty and notional always equal the sum over unique executions") {
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        Lab L;
        TraceGen gen(seed);
        const OrderId id = run_checked(L, gen.generate(30, 0.3));
        if (!id.valid()) continue;
        const OrderAggregate& a = L.state(id);

        // Independent recomputation from the ledger identities.
        std::set<std::string> ids;
        int64_t cum = 0;
        Int128 notional = 0;
        for (const FillLedgerEntry& f : a.ledger) {
            CHECK_MSG(ids.insert(f.exec_id).second, "duplicate ExecID " + f.exec_id);
            cum += f.qty_units;
            notional += static_cast<Int128>(f.qty_units) * f.price_ticks;
        }
        CHECK_EQ(cum, a.cum_qty_units);
        CHECK_EQ(to_string(notional), to_string(a.notional_ticks_x_qty));
        CHECK(a.cum_qty_units >= 0);
        if (a.has_accepted()) CHECK(a.cum_qty_units <= a.accepted_qty_units());

        // And the journal oracle, which shares no field with the aggregate.
        CHECK_EQ(oracle(L.store(), id).cum, a.cum_qty_units);
    }
}

SCENARIO("PROP-04", "P0", "injecting duplicates anywhere changes no business state") {
    for (uint64_t seed = 1; seed <= 150; ++seed) {
        TraceGen gen(seed);
        const std::vector<OrderEvent> trace = gen.generate(20, 0.1);

        Lab clean;
        const OrderId a = run_checked(clean, trace);

        // The same trace with every peer report delivered twice, and some
        // three times.
        std::vector<OrderEvent> doubled;
        std::mt19937_64 rng(seed * 7919);
        for (const OrderEvent& e : trace) {
            doubled.push_back(e);
            if (std::holds_alternative<PeerReport>(e)) {
                doubled.push_back(e);
                if (rng() % 3 == 0) doubled.push_back(e);
            }
        }
        Lab dup;
        const OrderId b = run_checked(dup, doubled);

        if (!a.valid() || !b.valid()) continue;
        CHECK_MSG(clean.state(a).state_hash() == dup.state(b).state_hash(),
                  "seed " + std::to_string(seed) + ": duplicates changed the state");
        CHECK_EQ(clean.state(a).cum_qty_units, dup.state(b).cum_qty_units);
        CHECK_EQ(clean.state(a).ledger.size(), dup.state(b).ledger.size());
    }
}

SCENARIO("PROP-05", "P0", "a crash at any window leaves the state before or after, never between") {
    const CrashWindow windows[] = {
        CrashWindow::W00_BeforeParse,
        CrashWindow::W01_AfterParseBeforeCorrelation,
        CrashWindow::W02_AfterCorrelationBeforeDedup,
        CrashWindow::W03_AfterDedupBeforeReduce,
        CrashWindow::W04_AfterReduceBeforeTxn,
        CrashWindow::W05_InTxnBeforeJournalInsert,
        CrashWindow::W06_AfterDedupInsertBeforeState,
        CrashWindow::W07_AfterStateBeforeOutbox,
        CrashWindow::W08_AfterOutboxBeforeCommit,
        CrashWindow::W09_AfterCommitBeforeInboxAck,
        CrashWindow::W10_AfterCommitBeforeDispatch,
        CrashWindow::W11_AfterDispatchBeforeMarkSent,
    };

    for (const CrashWindow w : windows) {
        for (uint64_t seed = 1; seed <= 12; ++seed) {
            Lab L;
            TraceGen gen(seed);
            const std::vector<OrderEvent> trace = gen.generate(12, 0.1);

            // Run the trace, then determine the two admissible answers for one
            // more event by running it on an untouched twin.
            OrderId id;
            for (size_t i = 0; i + 1 < trace.size(); ++i) {
                const EngineResult r = L.engine().submit(trace[i]);
                if (r.id.valid()) id = r.id;
            }
            if (!id.valid()) continue;
            const int64_t cum_before = L.state(id).cum_qty_units;

            Lab twin;
            OrderId twin_id;
            for (const OrderEvent& e : trace) {
                const EngineResult r = twin.engine().submit(e);
                if (r.id.valid()) twin_id = r.id;
            }
            const int64_t cum_after = twin_id.valid() ? twin.state(twin_id).cum_qty_units : -1;

            // Now crash on that last event.
            L.store().failpoint().arm(w);
            try {
                L.engine().submit(trace.back());
            } catch (const SimulatedCrash&) {
            } catch (const AmbiguousCommit&) {
            }
            L.store().failpoint().disarm();

            const RecoveryReport rep = L.engine().recover();
            CHECK_MSG(rep.status == RecoveryStatus::Clean,
                      std::string(to_string(w)) + " seed " + std::to_string(seed) + ": " +
                          rep.detail);
            const int64_t cum_recovered = L.state(id).cum_qty_units;
            CHECK_MSG(cum_recovered == cum_before || cum_recovered == cum_after,
                      std::string(to_string(w)) + " seed " + std::to_string(seed) +
                          ": recovered cum " + std::to_string(cum_recovered) + " is neither " +
                          std::to_string(cum_before) + " nor " + std::to_string(cum_after));
            const std::vector<InvariantViolation> v = check_invariants(L.state(id), L.policy());
            CHECK_MSG(v.empty(), describe(v));
        }
    }
}

SCENARIO("PROP-06", "P0", "an unsupported correction never moves a quantity") {
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        Lab L;
        TraceGen gen(seed);
        // A trace saturated with corrections and busts.
        const OrderId id = run_checked(L, gen.generate(25, 0.5));
        if (!id.valid()) continue;
        const OrderAggregate& a = L.state(id);

        // Every ledger entry is still effective: v1 never flips one.
        for (const FillLedgerEntry& f : a.ledger) CHECK(f.effective);
        // And the journal shows no correction was ever applied.
        for (const JournalRecord& rec : L.store().journal()) {
            const auto* r = std::get_if<PeerReport>(&rec.event);
            if (r == nullptr) continue;
            const bool correction = r->exec_type == exec_type::kTradeCorrect ||
                                    r->exec_type == exec_type::kTradeCancel ||
                                    r->exec_type == exec_type::kRestated;
            if (correction) CHECK(rec.decision != Decision::Applied);
        }
        CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
    }
}

SCENARIO("PROP-07", "P0", "no generated history ever overfills or reports a false terminal") {
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        Lab L;
        TraceGen gen(seed);
        const OrderId id = run_checked(L, gen.generate(25, 0.35));
        if (!id.valid()) continue;
        const OrderAggregate& a = L.state(id);

        if (a.has_accepted()) {
            CHECK_MSG(a.cum_qty_units <= a.accepted_qty_units(),
                      "seed " + std::to_string(seed) + ": overfill");
        }
        // A terminal state is never asserted while the outcome is unknown.
        if (a.confidence == Confidence::Unknown) {
            CHECK_MSG(!a.terminal(), "seed " + std::to_string(seed) + ": terminal while unknown");
        }
        // Filled means the full accepted quantity really executed.
        if (a.done_reason == DoneReason::Filled) {
            CHECK_EQ(a.cum_qty_units, a.accepted_qty_units());
        }
        // A terminal order has no working remainder.
        if (a.terminal()) CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    }
}

SCENARIO("PROP-08", "P0", "the same seed produces the same canonical state every run") {
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        TraceGen g1(seed);
        TraceGen g2(seed);
        const std::vector<OrderEvent> a = g1.generate(20, 0.25);
        const std::vector<OrderEvent> b = g2.generate(20, 0.25);
        CHECK_EQ(a.size(), b.size());

        Lab la;
        Lab lb;
        const OrderId ia = run_checked(la, a);
        const OrderId ib = run_checked(lb, b);
        if (!ia.valid() || !ib.valid()) continue;
        CHECK_MSG(la.state(ia).state_hash() == lb.state(ib).state_hash(),
                  "seed " + std::to_string(seed) + ": not reproducible");
        CHECK_EQ(la.state(ia).canonical_text(), lb.state(ib).canonical_text());
    }
}
