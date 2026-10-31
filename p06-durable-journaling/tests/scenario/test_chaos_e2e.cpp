// G24 — End-to-end chaos and adversarial sequencing.
//
// Everything here runs the whole path: admit, dispatch through the P01
// adapter, report back from the independent counterparty ledger, restart,
// verify. The faults are chosen by a seeded generator so a failure can be
// reproduced exactly, which is what section 23 asks for.
#include <string>
#include <vector>

#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

// Deterministic, small and inspectable. A library generator would be fine
// too, but a failure report has to be able to say "seed 20261030" and have
// that mean one exact sequence on every machine.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed) {}
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

const char* const kFaults[] = {
    "",
    "tx.after_journal_append",
    "tx.after_risk_projection",
    "tx.after_outbox_insert",
    "tx.during_commit",
    "outbox.before_stable_send",
    "p01.after_seq_persisted",
    "p01.during_socket_write",
    "p01.after_socket_write",
    "inbox.after_insert",
    "inbox.before_delivery_ack",
};

SCENARIO("G24-01", "P0", "a full lifecycle end to end leaves everything reconciled") {
    Lab l("g24-01");
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 100000), l.token()).kind,
             CommitKind::Committed);
    CHECK_EQ(l.dispatcher().dispatch_once(Lab::kTenant, l.token()).bound, std::size_t(1));

    CHECK_EQ(l.deliver(l.fill("O1", "E1", 40000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.fill("O1", "E2", 60000, 10001, "K1")).kind, CommitKind::Committed);

    const OrderState o = l.order("O1");
    CHECK_EQ(o.status, OrdStatus::Filled);
    CHECK_EQ(o.cum_qty, std::int64_t(100000));
    CHECK_EQ(l.risk().reserved, std::int64_t(0));
    CHECK_EQ(l.risk().exposure, std::int64_t(100000));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "CONFIRMED").size(), std::size_t(1));
    // The independent ledger agrees on the economics.
    CHECK_EQ(l.lp().traded_quantity(), std::int64_t(100000));
    CHECK_EQ(l.lp().distinct_trades(), std::size_t(2));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));

    expect_consistent(l, "G24-01");
    l.restart();
    CHECK_EQ(l.start().phase, Phase::Ready);
}

SCENARIO("G24-02", "P0", "a seeded chaos run never breaks an invariant") {
    // Section 26.2's seed, so a failure here is reproducible verbatim.
    Rng rng(20261030);
    Lab l("g24-02", 1000000);
    l.start();

    std::size_t admitted = 0;
    std::int64_t traded = 0;

    for (int i = 0; i < 40; ++i) {
        const std::string k = "K" + std::to_string(i);
        const std::string c = "O" + std::to_string(i);

        fp::disarm_all();
        const char* fault = kFaults[rng.below(sizeof(kFaults) / sizeof(kFaults[0]))];
        if (*fault != 0) fp::arm(fault, fp::Action::ReturnError);

        const CommitOutcome o = l.service().submit_new(l.new_order(k, c, 2000), l.token());
        fp::disarm_all();
        if (o.kind == CommitKind::Committed) ++admitted;

        if (rng.below(2) == 0) l.dispatcher().dispatch_once(Lab::kTenant, l.token());

        if (l.order(c).exists && l.order(c).leaves_qty > 0) {
            const std::int64_t qty = 500 + static_cast<std::int64_t>(rng.below(1000));
            fp::disarm_all();
            const char* f2 = kFaults[rng.below(sizeof(kFaults) / sizeof(kFaults[0]))];
            if (*f2 != 0) fp::arm(f2, fp::Action::ReturnError);
            const CommitOutcome r = l.deliver(l.fill(c, "E" + std::to_string(i), qty, 10000, k));
            fp::disarm_all();
            if (r.kind == CommitKind::Committed) traded += qty;
        }

        if (rng.below(8) == 0) {
            l.restart();
            l.start();
        }
    }
    fp::disarm_all();

    // The invariants, whatever path the run took.
    expect_consistent(l, "G24-02");
    CHECK(l.risk().utilised() <= l.risk().limit);
    CHECK_EQ(l.scalar("SELECT COALESCE(SUM(last_qty),0) FROM fills WHERE tenant_id='t1';"),
             traded);
    CHECK_EQ(l.row_count("orders"), admitted);
    // One original sequence per command at most (JI-19).
    CHECK(l.link().allocated_sequences(Lab::kSession) <= admitted);
    // Nothing was ever written to the wire twice.
    for (const LpOracle::Wire& w : l.lp().writes()) {
        CHECK_EQ(l.lp().wire_count(w.payload), std::size_t(1));
    }
}

SCENARIO("G24-03", "P0", "repeated restarts mid-flight converge on one consistent state") {
    Lab l("g24-03", 1000000);
    l.start();
    for (int i = 0; i < 10; ++i) {
        const std::string k = "K" + std::to_string(i);
        const std::string c = "O" + std::to_string(i);
        CHECK_EQ(l.service().submit_new(l.new_order(k, c, 1000), l.token()).kind,
                 CommitKind::Committed);
        l.dispatcher().dispatch_once(Lab::kTenant, l.token());
        // The counterparty confirms, which is what turns a BOUND effect
        // into evidence and lets a restarted instance serve again.
        CHECK_EQ(l.deliver(l.fill(c, "E" + std::to_string(i), 1000, 10000, k)).kind,
                 CommitKind::Committed);
        if (i % 3 == 0) {
            l.restart();
            CHECK_EQ(l.start().phase, Phase::Ready);
        }
    }
    CHECK_EQ(l.row_count("orders"), std::size_t(10));
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(10));
    CHECK_EQ(l.lp().distinct_sequences(Lab::kSession), std::size_t(10));
    expect_consistent(l, "G24-03");
}

SCENARIO("G24-04", "P0", "an adversarial report stream cannot corrupt the ledger") {
    Lab l("g24-04");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 50000), l.token());

    // A counterparty that behaves badly in every way the design doc names.
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 20000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 20000, 10000, "K1")).kind, CommitKind::Replayed);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 30000, 10000, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.deliver(l.fill("O1", "0", 10000, 10000, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.deliver(l.fill("O1", "E2", 999999, 10000, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.deliver(l.fill("O-nowhere", "E3", 1000, 10000, "")).kind,
             CommitKind::Quarantined);

    // Exactly one good fill got through.
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(20000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    CHECK_EQ(l.risk().exposure, std::int64_t(20000));
    CHECK(l.store().open_cases(Lab::kTenant).size() >= std::size_t(3));
    expect_consistent(l, "G24-04");
}

SCENARIO("G24-05", "P0", "a long history replays to the same hash every time") {
    Lab l("g24-05", 1000000);
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 300000), l.token());
    for (int i = 0; i < 100; ++i) {
        l.deliver(l.fill("O1", "E" + std::to_string(i), 3000, 10000 + i, "K1"));
    }

    const Digest first =
        replay_from_empty(l.store(), Lab::kTenant, order_stream("O1")).state_hash;
    for (int pass = 0; pass < 3; ++pass) {
        const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
        CHECK_MSG(r.ok, r.detail);
        CHECK_EQ(to_hex(r.state_hash), to_hex(first));
    }
    CHECK_EQ(to_hex(first),
             to_hex(l.store().head(Lab::kTenant, order_stream("O1")).state_hash));
    expect_consistent(l, "G24-05");
}

SCENARIO("G24-06", "P0", "a chaos run followed by a restore drill still reconciles") {
    Rng rng(777);
    Lab l("g24-06", 1000000);
    l.start();
    for (int i = 0; i < 15; ++i) {
        const std::string k = "K" + std::to_string(i);
        const std::string c = "O" + std::to_string(i);
        l.service().submit_new(l.new_order(k, c, 2000), l.token());
        if (rng.below(2) == 0) l.dispatcher().dispatch_once(Lab::kTenant, l.token());
        if (l.order(c).exists) l.deliver(l.fill(c, "E" + std::to_string(i), 1000, 10000, k));
    }
    expect_consistent(l, "G24-06 live");

    const RestoreDrillResult d =
        restore_drill(l.store(), Lab::kTenant, l.dir().file("arc"), l.dir().file("r.db"));
    CHECK_MSG(d.verified, d.detail);
    CHECK(!d.dispatch_enabled);

    SqliteAtomicStore::Options opt;
    opt.path = l.dir().file("r.db");
    opt.create_if_missing = false;
    opt.clock = &l.clock();
    std::unique_ptr<SqliteAtomicStore> restored;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &restored, &why), StoreError::None);

    // Every stream comes back with the same head the live database has.
    for (const StreamId& s : l.store().list_streams(Lab::kTenant)) {
        CHECK_EQ(to_hex(restored->head(Lab::kTenant, s).state_hash),
                 to_hex(l.store().head(Lab::kTenant, s).state_hash));
    }
    const VerifyReport v = verify_all(*restored, Lab::kTenant);
    CHECK_MSG(v.ok(), v.details.empty() ? std::string() : v.details.front());
}

SCENARIO_BLOCKED("G24-07", "P0", "a 48-hour mixed chaos soak",
                 "the soak is a scheduled campaign, not a unit test: section 26.2 gives it "
                 "its own runner and a duration flag. scripts/run_soak.sh drives this suite "
                 "in a loop for that purpose, and its result belongs in a run report rather "
                 "than in a per-commit verdict.")

SCENARIO_BLOCKED("G24-08", "P1", "a measured latency and throughput profile",
                 "section 19.1 is explicit that a number measured anywhere other than the "
                 "target host is not an SLO. apps/journal_bench measures replay rate on "
                 "whatever host it is run on and reports it as that, not as a target.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_chaos_e2e"); }
