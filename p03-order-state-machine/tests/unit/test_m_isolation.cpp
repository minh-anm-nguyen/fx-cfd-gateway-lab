// Group M -- multi-account, per-order isolation and the risk boundary
// (design doc 18.M).
//
// Two things are being tested. One: a fill belongs to exactly one chain and
// one account, and no identifier collision can move it. Two: the boundary with
// P05 is a durable intent with a stable key, and P03 never claims to enforce
// exposure itself (ADR-08, open question 8).
#include "../support/lab.hpp"

using namespace lab;

namespace {

size_t count_kind(const Lab& L, EffectIntent::Kind kind) {
    size_t n = 0;
    for (const OutboxRow& row : const_cast<Lab&>(L).engine().sink().dispatched()) {
        if (row.kind == kind) ++n;
    }
    return n;
}

}  // namespace

SCENARIO("M-01", "P0", "two accounts in one firm keep independent state and tags") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000", "1.08500", "EURUSD", "1", "ACC-A"));
    const EngineResult b = L.submit(cmd_new("B1", "50000", "1.08500", "EURUSD", "1", "ACC-B"));
    CHECK(a.ok());
    CHECK(b.ok());
    CHECK(L.submit(new_ack("A1").acct("ACC-A")).ok());
    CHECK(L.submit(new_ack("B1").acct("ACC-B")).ok());
    CHECK(L.submit(trade("XA", "A1").acct("ACC-A").lastq("40000").lastpx("1.08490").cum("40000"))
              .ok());

    CHECK_EQ(L.state(a.id).account, std::string("ACC-A"));
    CHECK_EQ(L.state(b.id).account, std::string("ACC-B"));
    CHECK_EQ(L.state(a.id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(b.id).cum_qty_units, int64_t{0});
}

SCENARIO("M-02", "P0", "two orders on one symbol cannot absorb each other's fills") {
    Lab L;
    const OrderId first = L.working("O1", "100000");
    const OrderId second = L.working("O2", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "O2").lastq("10000").lastpx("1.08490").cum("10000")).ok());

    CHECK_EQ(L.state(first).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(second).cum_qty_units, int64_t{10000});
    CHECK_EQ(L.state(first).ledger.size(), size_t{1});
    CHECK_EQ(L.state(second).ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), first).cum, int64_t{40000});
    CHECK_EQ(oracle(L.store(), second).cum, int64_t{10000});
}

SCENARIO("M-03", "P0", "two accounts cannot share a ClOrdID inside the firm-day namespace") {
    Lab L;
    CHECK(L.submit(cmd_new("SAME", "100000", "1.08500", "EURUSD", "1", "ACC-A")).ok());
    expect_reject(L.submit(cmd_new("SAME", "100000", "1.08500", "EURUSD", "1", "ACC-B")),
                  Reason::IdCollision);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("M-04", "P0", "a fill labelled with another account never crosses over") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000", "1.08500", "EURUSD", "1", "ACC-A"));
    const EngineResult b = L.submit(cmd_new("B1", "100000", "1.08500", "EURUSD", "1", "ACC-B"));
    CHECK(L.submit(new_ack("A1").acct("ACC-A")).ok());
    CHECK(L.submit(new_ack("B1").acct("ACC-B")).ok());

    // The report names A1 but claims account B.
    expect_quarantine(
        L.submit(trade("X1", "A1").acct("ACC-B").lastq("40000").lastpx("1.08490")),
        Reason::AccountMismatch);
    CHECK_EQ(L.state(a.id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(b.id).cum_qty_units, int64_t{0});
}

SCENARIO("M-05", "P0", "an ExecID reused across accounts under one peer is a collision") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000", "1.08500", "EURUSD", "1", "ACC-A"));
    const EngineResult b = L.submit(cmd_new("B1", "100000", "1.08500", "EURUSD", "1", "ACC-B"));
    CHECK(L.submit(new_ack("A1").acct("ACC-A")).ok());
    CHECK(L.submit(new_ack("B1").acct("ACC-B")).ok());
    CHECK(L.submit(trade("SHARED", "A1").acct("ACC-A").lastq("40000").lastpx("1.08490").cum(
                                                           "40000"))
              .ok());

    // Same ExecID, same peer namespace, different account and chain.
    expect_quarantine(
        L.submit(trade("SHARED", "B1").acct("ACC-B").lastq("10000").lastpx("1.08490").cum("10000")),
        Reason::ConflictingDuplicate);
    CHECK_EQ(L.state(a.id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(b.id).cum_qty_units, int64_t{0});
}

SCENARIO("M-06", "P0", "two counterparties may each use the same OrderID string") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000"));
    CHECK(L.submit(new_ack("A1").peer("LP1").oid("ORD-1")).ok());
    LocalCommand second = cmd_new("B1", "100000");
    second.firm = "FIRM2";
    const EngineResult b = L.submit(second);
    CHECK(b.ok());
    CHECK(L.submit(new_ack("B1").peer("LP2").oid("ORD-1")).ok());

    CHECK(L.state(a.id).has_alias("LP1", "ORD-1"));
    CHECK(L.state(b.id).has_alias("LP2", "ORD-1"));
    CHECK(!L.state(a.id).has_alias("LP2", "ORD-1"));
    CHECK_EQ(L.engine().chain_count(), size_t{2});
}

SCENARIO("M-07", "P0", "one peer returning one OrderID for two chains does not overwrite") {
    Lab L;
    const OrderId first = L.working("O1");
    const EngineResult second = L.submit(cmd_new("O2", "100000"));
    CHECK(second.ok());
    expect_quarantine(L.submit(new_ack("O2").oid("LPO-O1")), Reason::AliasCollision);
    CHECK(L.state(first).has_alias("LP1", "LPO-O1"));
    CHECK(!L.state(second.id).has_order_id("LPO-O1"));
}

SCENARIO("M-08", "P0", "a reconciliation answer for another account does not mutate this chain") {
    Lab L;
    const EngineResult a = L.submit(cmd_new("A1", "100000", "1.08500", "EURUSD", "1", "ACC-A"));
    CHECK(L.submit(new_ack("A1").acct("ACC-A")).ok());
    CHECK(L.submit(cmd_cancel("CA", "A1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "CA")).ok());
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "CA")).ok());

    // The snapshot answers for account B.
    expect_quarantine(L.submit(status_snapshot("A1")
                                   .acct("ACC-B")
                                   .status(ord_status::kCanceled)
                                   .cum("0")
                                   .gen(1)),
                      Reason::AccountMismatch);
    CHECK(L.state(a.id).reconciliation_required);
    CHECK(L.state(a.id).done_reason != DoneReason::Canceled);
}

SCENARIO("M-09", "P0", "a duplicate New Ack produces exactly one risk reservation") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(n.ok());
    CHECK_EQ(count_kind(L, EffectIntent::Kind::RiskReserve), size_t{1});
    CHECK(L.submit(new_ack("O1")).ok());
    expect_decision(L.submit(new_ack("O1")), Decision::DuplicateNoop);

    // One reservation signal, whatever the counterparty repeats.
    CHECK_EQ(count_kind(L, EffectIntent::Kind::RiskReserve), size_t{1});
    CHECK_EQ(L.engine().sink().count_key("risk-reserve|FIRM|2026-10-30|O1"), size_t{1});
}

SCENARIO("M-10", "P0", "a risk adjustment keeps its stable identity across a crash") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    L.store().failpoint().arm(CrashWindow::W11_AfterDispatchBeforeMarkSent);
    bool crashed = false;
    try {
        L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000"));
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    CHECK(L.store().failpoint().fired());
    L.store().failpoint().disarm();

    const std::string key = "risk-adjust|1|X1";
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    // Offered again with the same key, so an idempotent P05 applies it once.
    CHECK_EQ(L.engine().sink().count_key(key), size_t{2});
    CHECK_EQ(count_kind(L, EffectIntent::Kind::RiskAdjust), size_t{1});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO_BLOCKED("M-11", "P0",
                 "a cancel reject after a provisional risk release is compensated",
                 "needs the P05 reservation transaction contract: P03 emits the intent but "
                 "has no inbound risk event to reconcile against yet")

SCENARIO("M-12", "P0", "a cancel is terminal only after commit; the release intent is durable") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());

    // The commit and the release intent are one transaction; the dispatch is
    // afterwards. A crash in between leaves the intent durable, not lost.
    L.store().failpoint().arm(CrashWindow::W10_AfterCommitBeforeDispatch);
    bool crashed = false;
    try {
        L.submit(canceled("C1").cum("40000"));
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    CHECK(crashed);
    L.store().failpoint().disarm();

    const std::string key = "risk-release|1|canceled";
    CHECK_EQ(L.engine().sink().count_key(key), size_t{0});  // not yet observable
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);                // the commit stood
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});  // and the intent surfaced
}

SCENARIO("M-13", "P0", "a fill is recorded first and the risk update is queued behind it") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    // Nothing about the risk service's availability can block the ledger: the
    // reducer produces an intent and never calls out.
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    bool queued = false;
    for (const OutboxRow& row : L.store().outbox()) {
        if (row.kind == EffectIntent::Kind::RiskAdjust && row.idempotency_key == "risk-adjust|1|X1") {
            queued = true;
        }
    }
    CHECK(queued);
    // The outbox is bounded and durable, which is what "queue with a bounded
    // durable outbox" means at this boundary.
    CHECK(L.store().outbox().size() <= L.policy().limits.max_queue_events);
}

SCENARIO_BLOCKED("M-14", "P0", "a stale risk revision never overwrites order state",
                 "needs the P05 inbound contract: there is no risk-originated event in the v1 "
                 "event model, so there is nothing to feed the stale revision through")

SCENARIO("M-15", "P0", "a config change mid-order does not reinterpret the accepted version") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    const uint32_t pinned_policy = L.state(id).accepted_versions[0].policy_version;
    const uint32_t pinned_scale = L.state(id).accepted_versions[0].scale_id;
    CHECK_EQ(pinned_policy, uint32_t{1});

    // The lab policy has no per-account base currency field in v1; the pinning
    // mechanism under test is the same one that would protect it.
    Policy next = L.policy();
    next.policy_version = 7;
    next.symbols["EURUSD"].scale_id = 42;
    next.symbols["EURUSD"].tick_denominator = 1000;
    CHECK(L.engine().reload_policy(next).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions[0].policy_version, pinned_policy);
    CHECK_EQ(a.accepted_versions[0].scale_id, pinned_scale);
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
}

SCENARIO("M-16", "P0", "a symbol removed from config keeps the order's scale and blocks commands") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    Policy stripped = L.policy();
    stripped.symbols.erase("EURUSD");
    CHECK(L.engine().reload_policy(stripped).ok());

    // The working order keeps the scale it was accepted under.
    CHECK_EQ(L.state(id).accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(L.state(id).accepted_versions[0].scale_id, uint32_t{1});
    // New commands on that symbol are refused until the config is safe again.
    expect_reject(L.submit(cmd_new("O2", "100000", "1.08500", "EURUSD")), Reason::UnknownSymbol);
    expect_reject(L.submit(cmd_replace("R1", "O1", "120000")), Reason::UnknownSymbol);
    // And a fill cannot be judged without the scale, so it fails closed.
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490")),
                      Reason::UnknownSymbol);
}

SCENARIO("M-17", "P0", "a quarantined chain does not stop the healthy ones") {
    Lab L;
    const OrderId sick = L.working("O1", "100000");
    const OrderId well = L.working("O2", "100000");

    expect_quarantine(L.submit(trade("X1", "O1").lastq("70000").lastpx("1.08600")),
                      Reason::PriceOutsideLimit);
    CHECK(L.state(sick).quarantined);

    // The other chain continues normally.
    CHECK(L.submit(trade("X2", "O2").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(!L.state(well).quarantined);
    CHECK_EQ(L.state(well).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.lc(well), Lifecycle::PartFilled);
    // And the sick chain refuses further commands rather than drifting.
    expect_reject(L.submit(cmd_cancel("C1", "O1")), Reason::TerminalState);
    CHECK(L.submit(cmd_cancel("C2", "O2")).ok());
}

SCENARIO("M-18", "P0", "a storage failure publishes no state and the retry applies once") {
    Lab L;
    const OrderId a = L.working("O1", "100000");
    const OrderId b = L.working("O2", "100000");
    const uint64_t a_hash = L.state(a).state_hash();

    // The commit for chain A fails. Nothing is published for it: the in-memory
    // state does not move ahead of the journal, and the row stays unconsumed
    // so the mailbox is head-of-line blocked rather than silently skipped.
    L.store().set_fail_next_commit(true, "global DB unavailable");
    const EngineResult failed =
        L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000"));
    CHECK(!failed.committed);
    CHECK_EQ(failed.decision, Decision::RetryCommit);
    CHECK_EQ(failed.commit_status, CommitStatus::Failed);
    CHECK_EQ(L.state(a).state_hash(), a_hash);
    CHECK_EQ(L.state(a).cum_qty_units, int64_t{0});
    CHECK(L.store().unconsumed_inbox() > 0);
    CHECK_EQ(L.engine().metrics().commit_failures, uint64_t{1});

    // Storage recovers. The queued work drains in order: A's fill applies
    // exactly once, then B's.
    CHECK(L.engine().pump_all() > 0);
    CHECK(L.submit(trade("X2", "O2").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    CHECK_EQ(L.state(a).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(a).ledger.size(), size_t{1});
    CHECK_EQ(L.state(b).cum_qty_units, int64_t{40000});
    CHECK_EQ(oracle(L.store(), a).applied_trades, size_t{1});
    CHECK_EQ(oracle(L.store(), b).applied_trades, size_t{1});
}

SCENARIO("M-19", "P0", "an event for the wrong chain is refused before the reducer") {
    Lab L;
    const OrderId a = L.working("O1", "100000");
    const OrderId b = L.working("O2", "100000");

    // A report whose identifiers belong to neither chain.
    expect_quarantine(
        L.submit(trade("X1", "WRONG").oid("NOT-OURS").lastq("40000").lastpx("1.08490")),
        Reason::IdentityInsufficient);
    CHECK_EQ(L.state(a).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(b).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(a).incidents.size(), size_t{0});  // neither chain was touched
    CHECK_EQ(L.state(b).incidents.size(), size_t{0});
    CHECK_EQ(L.engine().metrics().orphan_reports, uint64_t{1});
}

SCENARIO("M-20", "P1", "P03 does not claim to enforce account exposure") {
    Lab L;
    // Three large orders on one account. P03 admits all three: it has no
    // exposure model and must not pretend to. Enforcement is P05's, serialised
    // per account (design doc 6.1, open question 8).
    for (int i = 0; i < 3; ++i) {
        const EngineResult r =
            L.submit(cmd_new("BIG" + std::to_string(i), "100000000", "1.08500"));
        CHECK_MSG(r.ok(), "order " + std::to_string(i) + " was refused: " + r.detail);
    }
    CHECK_EQ(L.engine().chain_count(), size_t{3});
    // What P03 does guarantee is that each one produced its own reservation
    // intent for P05 to decide on.
    CHECK_EQ(count_kind(L, EffectIntent::Kind::RiskReserve), size_t{3});
}
