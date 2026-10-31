// Group C -- fill math, quantities, rounding and conservation
// (design doc 18.C). Quantities are integers in base units throughout; the
// only rounding in the whole group happens once, when an average price is
// rendered.
#include "../support/lab.hpp"

using namespace lab;

namespace {

// A profile with single-unit granularity and no quantity ceiling, for the
// boundary cases where the lab EURUSD lot step would get in the way.
Policy wide_policy() {
    Policy p = default_lab_policy();
    SymbolSpec wide;
    wide.tick_numerator = 1;
    wide.tick_denominator = 10;
    wide.qty_step = 1;
    wide.min_qty = 1;
    wide.max_qty = 0;  // no explicit ceiling; only the checked arithmetic bounds it
    wide.scale_id = 9;
    p.symbols["WIDEFX"] = wide;
    return p;
}

}  // namespace

SCENARIO("C-01", "P0", "a single fill for the whole quantity completes exactly") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    // 100000 x 108490 ticks, exact in the wide type.
    CHECK_EQ(to_string(a.notional_ticks_x_qty), std::string("10849000000"));
    int64_t avg = 0;
    CHECK(a.avg_px_ticks_rounded(avg));
    CHECK_EQ(avg, int64_t{108490});
}

SCENARIO("C-02", "P0", "two fills of 40k and 60k give a weighted average, counted once") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").leaves("60000"))
              .ok());
    CHECK(L.submit(trade("X2", "O1")
                       .status(ord_status::kFilled)
                       .lastq("60000")
                       .lastpx("1.08495")
                       .cum("100000")
                       .leaves("0"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    // (40000*108490 + 60000*108495) / 100000 = 108493 exactly, which is the
    // worked example in design doc 9.2.
    int64_t avg = 0;
    CHECK(a.avg_px_ticks_rounded(avg));
    CHECK_EQ(avg, int64_t{108493});
    CHECK_EQ(ticks_to_text(avg, 1, 100000), std::string("1.08493"));

    // The independent journal oracle agrees, and saw each ExecID once.
    const JournalOracle o = oracle(L.store(), id);
    CHECK_EQ(o.cum, int64_t{100000});
    CHECK_EQ(o.exec_ids.size(), size_t{2});
    CHECK_EQ(o.applied_trades, size_t{2});
    CHECK_EQ(to_string(o.notional), to_string(a.notional_ticks_x_qty));
}

SCENARIO("C-03", "P0", "three fills short of the quantity leave the remainder working") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(trade("X1", "O1").lastq("10000").lastpx("1.08490").cum("10000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("30000")).ok());
    CHECK(L.submit(trade("X3", "O1").lastq("30000").lastpx("1.08492").cum("60000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{40000});
    CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{60000});
}

SCENARIO("C-04", "P0", "LastQty zero on ExecType=F is quarantined, CumQty untouched") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(L.submit(trade("X1", "O1").lastq("0").lastpx("1.08490")),
                      Reason::InvalidLastQty);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
}

SCENARIO("C-05", "P0", "a negative LastQty is quarantined, with no underflow on the cast") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(L.submit(trade("X1", "O1").lastq("-40000").lastpx("1.08490")),
                      Reason::InvalidLastQty);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("C-06", "P0", "a fill beyond the accepted leaves is quarantined as an overfill") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(L.submit(trade("X2", "O1").lastq("70000").lastpx("1.08490")),
                      Reason::Overfill);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});  // no partial mutation
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{40000});
}

SCENARIO("C-07", "P0", "the last fill for exactly the remainder completes with no rounding") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(trade("X2", "O1")
                       .status(ord_status::kFilled)
                       .lastq("60000")
                       .lastpx("1.08495")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, a.accepted_qty_units());
}

SCENARIO("C-08", "P0", "a reported CumQty below the ledger total is a regression") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    // The fill itself is fine; the cumulative figure claims less than the
    // ledger already proves.
    expect_quarantine(L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08490").cum("30000")),
                      Reason::CumRegression);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("C-09", "P0", "a reported CumQty above the ledger total means missing executions") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08490").cum("70000")),
                      Reason::MissingFillEvidence);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("C-10", "P0", "a reported LeavesQty above OrderQty is quarantined") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(
        L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").leaves("200000")),
        Reason::LeavesInconsistent);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("C-11", "P0", "a negative or overflowing LeavesQty is refused by checked parsing") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(
        L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").leaves("-1")),
        Reason::LeavesInconsistent);
    expect_quarantine(L.submit(trade("X2", "O1")
                                   .lastq("40000")
                                   .lastpx("1.08490")
                                   .cum("40000")
                                   .leaves("99999999999999999999999")),
                      Reason::MalformedNumber);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("C-12", "P0", "a trade echoing OrderQty zero against an executed order is quarantined") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(
        L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08490").oqty("0")),
        Reason::CumMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("C-13", "P0", "an average whose denominator does not divide stays rational") {
    Lab L;
    const OrderId id = L.working("O1", "30000", "1.08500");
    CHECK(L.submit(trade("X1", "O1").lastq("10000").lastpx("1.08490").cum("10000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08491").cum("20000")).ok());
    CHECK(L.submit(trade("X3", "O1").lastq("10000").lastpx("1.08493").cum("30000")).ok());

    const OrderAggregate& a = L.state(id);
    // Sum of ticks is 325474, which is not divisible by 3: the exact value is
    // kept as a rational and rounded only here.
    Int128 num = 0;
    Int128 den = 0;
    CHECK(a.avg_px_rational(num, den));
    CHECK_EQ(to_string(num), std::string("3254740000"));
    CHECK_EQ(to_string(den), std::string("30000"));
    CHECK(num % den != 0);

    int64_t avg = 0;
    CHECK(a.avg_px_ticks_rounded(avg));
    // 3254740000 / 30000 = 108491.33..., round-half-even gives 108491.
    CHECK_EQ(avg, int64_t{108491});
    // And the rounding is a pure function of the stored rational.
    int64_t again = 0;
    CHECK(a.avg_px_ticks_rounded(again));
    CHECK_EQ(again, avg);
}

SCENARIO("C-14", "P0", "recomputing from the ledger reproduces the cached totals") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(trade("X1", "O1").lastq("30000").lastpx("1.08490").cum("30000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("25000").lastpx("1.08492").cum("55000")).ok());
    CHECK(L.submit(trade("X3", "O1").lastq("45000").lastpx("1.08499").cum("100000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
    CHECK_EQ(to_string(a.notional_ticks_x_qty), to_string(a.ledger_notional()));
    const JournalOracle o = oracle(L.store(), id);
    CHECK_EQ(o.cum, a.cum_qty_units);
    CHECK_EQ(to_string(o.notional), to_string(a.notional_ticks_x_qty));
    CHECK(check_invariants(a, L.policy()).empty());
}

SCENARIO("C-15", "P0", "a quantity sum near the int64 ceiling is detected before it wraps") {
    // First the arithmetic itself: the helper the reducer uses must refuse,
    // not wrap.
    int64_t out = 0;
    CHECK(!checked_add(kInt64Max, 1, out));
    CHECK(!checked_add(kInt64Max - 5, 10, out));
    CHECK(checked_add(kInt64Max - 5, 5, out));
    CHECK_EQ(out, kInt64Max);

    // Then through the reducer: a second fill that would take CumQty past the
    // accepted quantity is refused with the ledger intact, whatever the scale.
    Lab L(wide_policy());
    const OrderId id = L.working("O1", "999999999999999999", "1.5", "WIDEFX");
    CHECK(L.submit(trade("X1", "O1").sym("WIDEFX").lastq("999999999999999998").lastpx("1.4").cum(
                                                                "999999999999999998"))
              .ok());
    expect_quarantine(
        L.submit(trade("X2", "O1").sym("WIDEFX").lastq("999999999999999998").lastpx("1.4")),
                      Reason::Overfill);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{999999999999999998});
    CHECK(a.cum_qty_units > 0);  // never wrapped negative
}

SCENARIO("C-16", "P0", "quantity times price uses a wide intermediate, not int64") {
    Lab L(wide_policy());
    const OrderId id = L.working("O1", "999999999999999999", "1.5", "WIDEFX");
    // 999999999999999998 units x 14 ticks overflows int64 (1.4e19 > 9.2e18) and
    // must still be exact.
    CHECK(L.submit(trade("X1", "O1").sym("WIDEFX").lastq("999999999999999998").lastpx("1.4").cum(
                                                                "999999999999999998"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(to_string(a.notional_ticks_x_qty), std::string("13999999999999999972"));
    CHECK_EQ(to_string(a.ledger_notional()), to_string(a.notional_ticks_x_qty));
    // The product genuinely does not fit in int64.
    CHECK(!int128_fits_int64(a.notional_ticks_x_qty));
    int64_t bad = 0;
    CHECK(!checked_mul(999999999999999998LL, 14LL, bad));
}

SCENARIO("C-17", "P0", "a one-unit fill at the lower boundary is not discarded") {
    Policy p = default_lab_policy();
    Lab L(p);
    const EngineResult n = L.submit(cmd_new("U1", "3", "1.08", "UNITFX"));
    CHECK(n.ok());
    CHECK(L.submit(new_ack("U1").sym("UNITFX")).ok());
    CHECK(L.submit(trade("X1", "U1").sym("UNITFX").lastq("1").lastpx("1.07").cum("1").leaves("2"))
              .ok());
    const OrderAggregate& a = L.state(n.id);
    CHECK_EQ(a.cum_qty_units, int64_t{1});
    CHECK_EQ(a.leaves_qty_units(), int64_t{2});
    CHECK_EQ(a.ledger.size(), size_t{1});
}

SCENARIO("C-18", "P0", "a fill quantity off the configured lot step is quarantined") {
    Lab L;  // EURUSD qty_step is 1000
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40500").lastpx("1.08490")),
                      Reason::QtyStepMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("C-19", "P0", "a Filled report carrying nonzero LeavesQty is refused") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(L.submit(trade("X1", "O1")
                                   .status(ord_status::kFilled)
                                   .lastq("100000")
                                   .lastpx("1.08490")
                                   .cum("100000")
                                   .leaves("5000")),
                      Reason::LeavesInconsistent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK(a.done_reason != DoneReason::Filled);
}

SCENARIO("C-20", "P0", "Canceled below OrderQty with leaves zero is a terminal exception") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    // Qty = Cum + Leaves does NOT hold here, and must not be asserted anywhere
    // in the library: 100000 != 40000 + 0.
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_NE(a.accepted_qty_units(), a.cum_qty_units + a.leaves_qty_units());
    // And the state is still invariant-clean.
    CHECK(check_invariants(a, L.policy()).empty());
}
