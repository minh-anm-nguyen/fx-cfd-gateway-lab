// Group K -- trade correct, trade cancel, restatement and unsupported events
// (design doc 18.K).
//
// v1 has no reversible ledger, so every correction path fails closed. The
// point of the group is that "unsupported" means quarantine plus evidence,
// never a silent ignore and never an ad hoc `cum -= n`.
#include "../support/lab.hpp"

using namespace lab;

namespace {

// A part-filled order with two executions, which is what corrections refer to.
OrderId two_fills(Lab& L) {
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08492").cum("60000")).ok());
    return id;
}

}  // namespace

SCENARIO("K-01", "P0", "a trade correct on a known execution quarantines, adding no fill") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_correct("G1", "O1").ref("X1").lastq("50000").lastpx("1.08490")),
                      Reason::UnsupportedEvent);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK(a.reconciliation_required);
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{60000});
}

SCENARIO("K-02", "P0", "a trade cancel on a known execution quarantines, subtracting nothing") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X1").lastq("40000").lastpx("1.08490")),
                      Reason::UnsupportedEvent);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});  // no silent subtraction
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK(a.ledger[0].effective);
    CHECK(a.quarantined);
}

SCENARIO("K-03", "P0", "a trade correct without ExecRefID has no correction lineage") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_correct("G1", "O1").lastq("50000").lastpx("1.08490")),
                      Reason::MissingCorrectionReference);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-04", "P0", "a trade cancel without ExecRefID has no bust lineage") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_cancel("H1", "O1").lastq("40000").lastpx("1.08490")),
                      Reason::MissingCorrectionReference);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-05", "P0", "a trade correct naming an execution we never saw is refused") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_correct("G1", "O1").ref("X-NEVER").lastq("50000")),
                      Reason::UnknownCorrectionReference);
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("K-06", "P0", "a trade cancel naming an execution we never saw is refused") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X-NEVER").lastq("40000")),
                      Reason::UnknownCorrectionReference);
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("K-07", "P0", "a price-only correction does not overwrite the recorded fill") {
    Lab L;
    const OrderId id = two_fills(L);
    const int64_t original_px = L.state(id).ledger[0].price_ticks;
    const Int128 original_notional = L.state(id).notional_ticks_x_qty;

    expect_quarantine(
        L.submit(trade_correct("G1", "O1").ref("X1").lastq("40000").lastpx("1.08499")),
        Reason::UnsupportedEvent);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger[0].price_ticks, original_px);
    CHECK_EQ(to_string(a.notional_ticks_x_qty), to_string(original_notional));
}

SCENARIO("K-08", "P0", "a quantity correction produces no blind delta") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(
        L.submit(trade_correct("G1", "O1").ref("X1").lastq("10000").lastpx("1.08490")),
        Reason::UnsupportedEvent);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger[0].qty_units, int64_t{40000});
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
}

SCENARIO("K-09", "P0", "a bust after Filled is quarantined; terminal is not assumed immutable") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    CHECK_EQ(L.lc(id), Lifecycle::Filled);

    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X1").lastq("100000")),
                      Reason::UnsupportedEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK(a.reconciliation_required);
    // The event was not ignored: there is an incident naming it.
    CHECK_EQ(a.incidents.size(), size_t{1});
    CHECK_EQ(a.incidents[0].exec_id, std::string("H1"));
}

SCENARIO("K-10", "P0", "a bust after Canceled with partials does not resurrect the order") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());

    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X1").lastq("40000")),
                      Reason::UnsupportedEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.done_reason, DoneReason::Canceled);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
}

SCENARIO("K-11", "P0", "a duplicate bust keeps one incident and adds no compensation") {
    Lab L;
    const OrderId id = two_fills(L);
    const auto bust = trade_cancel("H1", "O1").ref("X1").lastq("40000").lastpx("1.08490");
    expect_quarantine(L.submit(bust), Reason::UnsupportedEvent);
    const size_t incidents = L.state(id).incidents.size();
    CHECK_EQ(incidents, size_t{1});

    // The same contradiction again is not a second incident and not a second
    // compensation.
    expect_decision(L.submit(bust), Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).incidents.size(), incidents);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-12", "P0", "a repeated correction with changed values mutates nothing twice") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(
        L.submit(trade_correct("G1", "O1").ref("X1").lastq("50000").lastpx("1.08490")),
        Reason::UnsupportedEvent);
    const uint64_t cum_after_first = static_cast<uint64_t>(L.state(id).cum_qty_units);

    // Same ExecID, different values: a conflict, detected, with no second
    // mutation of any kind.
    expect_quarantine(
        L.submit(trade_correct("G1", "O1").ref("X1").lastq("55000").lastpx("1.08491")),
        Reason::ConflictingDuplicate);
    CHECK_EQ(static_cast<uint64_t>(L.state(id).cum_qty_units), cum_after_first);
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("K-13", "P0", "a correction chain is not chased recursively") {
    Lab L;
    const OrderId id = two_fills(L);
    // X1 corrected by G1, G1 corrected by G2, and so on. Each link is refused
    // on its own; nothing follows the chain.
    expect_quarantine(L.submit(trade_correct("G1", "O1").ref("X1").lastq("45000")),
                      Reason::UnsupportedEvent);
    expect_quarantine(L.submit(trade_correct("G2", "O1").ref("G1").lastq("46000")),
                      Reason::UnknownCorrectionReference);
    expect_quarantine(L.submit(trade_correct("G3", "O1").ref("G2").lastq("47000")),
                      Reason::UnknownCorrectionReference);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK(a.incidents.size() <= L.policy().limits.max_quarantine_incidents);
}

SCENARIO("K-14", "P1", "a bust referencing a bust is an invalid reference") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X1").lastq("40000")),
                      Reason::UnsupportedEvent);
    // H1 is not a ledger execution, so a bust of the bust has nothing to name.
    expect_quarantine(L.submit(trade_cancel("H2", "O1").ref("H1").lastq("40000")),
                      Reason::UnknownCorrectionReference);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-15", "P1", "a correction of an execution on a canceled order is still refused") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());

    expect_quarantine(L.submit(trade_correct("G1", "O1").ref("X1").lastq("30000")),
                      Reason::UnsupportedEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.done_reason, DoneReason::Canceled);
}

SCENARIO("K-16", "P1", "an execution restatement is outside the approved profile") {
    Lab L;
    const OrderId id = two_fills(L);
    expect_quarantine(L.submit(restated("D1", "O1").text("corporate action")),
                      Reason::UnsupportedEvent);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
}

SCENARIO("K-17", "P0", "a restatement with no reason at all is malformed as well as unsupported") {
    Lab L;
    const OrderId id = two_fills(L);
    const EngineResult r = L.submit(restated("D1", "O1"));
    expect_quarantine(r, Reason::UnsupportedEvent);
    CHECK(r.detail.find("without a reason") != std::string::npos);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-18", "P0", "retention shorter than the resend horizon blocks startup") {
    Policy p = default_lab_policy();
    p.dedup_retention_units = 10;
    p.fix_resend_horizon_units = 50;
    const ConfigError e = p.validate();
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::RetentionBelowResendHorizon);

    // And a running engine refuses to drop dedup rows without a proven
    // watermark, so a correction arriving late still finds its evidence.
    Lab L;
    const OrderId id = two_fills(L);
    CHECK(!L.store().evict_applied_keys_before(1000000, false));
    expect_quarantine(L.submit(trade_cancel("H1", "O1").ref("X1").lastq("40000")),
                      Reason::UnsupportedEvent);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{60000});
}

SCENARIO("K-19", "P0", "an original and a corrected trade sent as two fills cannot overcount") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // The counterparty resends the same economic trade under a new ExecID with
    // a corrected price, still claiming a total of 40000. Applying it would
    // make the ledger total 80000 against a reported 40000.
    expect_quarantine(L.submit(trade("X1-COR", "O1").lastq("40000").lastpx("1.08495").cum("40000")),
                      Reason::CumRegression);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{1});
}

SCENARIO("K-20", "P1", "enabling the v2 correction engine requires a certified migration") {
    Policy p = default_lab_policy();
    p.corrections_engine_v2_enabled = true;
    p.corrections_schema_migrated = false;
    const ConfigError e = p.validate();
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::CorrectionsEnabledWithoutMigration);

    // With the migration certified the configuration is admissible, and a
    // running engine still refuses the reload path's unsafe variant.
    p.corrections_schema_migrated = true;
    CHECK(p.validate().ok());

    Lab L;
    CHECK(L.working("O1").valid());
    Policy bad = L.policy();
    bad.corrections_engine_v2_enabled = true;
    CHECK(!L.engine().reload_policy(bad).ok());
    // The v1 behaviour is unchanged by the refused reload.
    CHECK(!L.policy().corrections_engine_v2_enabled);
}
