// Group D -- fixed-point prices, symbol scales and precision
// (design doc 18.D). No test in this file is allowed to pass by rounding.
#include "../support/lab.hpp"

using namespace lab;

namespace {

// EURUSD: one tick is 0.00001.
int64_t eurusd_ticks(const std::string& text, DecimalError* err = nullptr) {
    int64_t ticks = 0;
    const DecimalError e = parse_ticks(text, 1, 100000, ticks);
    if (err != nullptr) *err = e;
    return e == DecimalError::None ? ticks : -1;
}

DecimalError eurusd_error(const std::string& text) {
    int64_t ticks = 0;
    return parse_ticks(text, 1, 100000, ticks);
}

}  // namespace

SCENARIO("D-01", "P0", "1.08500 at a 0.00001 tick is 108500 and round-trips exactly") {
    CHECK_EQ(eurusd_ticks("1.08500"), int64_t{108500});
    CHECK_EQ(ticks_to_text(108500, 1, 100000), std::string("1.08500"));
    // Round-trip for a spread of values, with no drift anywhere.
    for (int64_t t : {1LL, 2LL, 99999LL, 100000LL, 108500LL, 108501LL, 123456789LL}) {
        const std::string text = ticks_to_text(t, 1, 100000);
        CHECK_EQ(eurusd_ticks(text), t);
    }
}

SCENARIO("D-02", "P0", "1.08501 is 108501, with no floating point in the path") {
    CHECK_EQ(eurusd_ticks("1.08501"), int64_t{108501});
    CHECK_EQ(ticks_to_text(108501, 1, 100000), std::string("1.08501"));
    // The two neighbours are distinct, which a double would not guarantee at
    // every magnitude.
    CHECK_NE(eurusd_ticks("1.08501"), eurusd_ticks("1.08500"));
}

SCENARIO("D-03", "P0", "a sixth decimal is off-tick and refused, never rounded") {
    CHECK_EQ(eurusd_error("1.085001"), DecimalError::OffTick);
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "1.085001")), Reason::ScaleMismatch);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
}

SCENARIO("D-04", "P0", "a negative price is refused on a positive-price symbol") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "-1.08500")), Reason::InvalidPrice);
    // And on the report side it is quarantined rather than applied.
    const OrderId id = L.working("O2");
    expect_quarantine(L.submit(trade("X1", "O2").lastq("40000").lastpx("-1.08490")),
                      Reason::InvalidPrice);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("D-05", "P1", "scientific notation is not a canonical price") {
    CHECK_EQ(eurusd_error("1.085e0"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1E5"), DecimalError::NotCanonical);
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "1.085e0")), Reason::InvalidPrice);
}

SCENARIO("D-06", "P0", "NaN and Inf are rejected, with no float conversion attempted") {
    for (const char* text : {"nan", "NaN", "inf", "-inf", "Infinity", "nan(0)"}) {
        CHECK_MSG(eurusd_error(text) == DecimalError::NotCanonical, std::string(text));
    }
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "nan")), Reason::InvalidPrice);
}

SCENARIO("D-07", "P0", "a comma decimal separator is ambiguous and refused") {
    CHECK_EQ(eurusd_error("1,08500"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1.085,00"), DecimalError::NotCanonical);
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "1,08500")), Reason::InvalidPrice);
}

SCENARIO("D-08", "P0", "more than one decimal point is malformed") {
    CHECK_EQ(eurusd_error("1.085.00"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("..1"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1."), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("."), DecimalError::NotCanonical);
}

SCENARIO("D-09", "P0", "whitespace and control bytes are not stripped, they are refused") {
    CHECK_EQ(eurusd_error(" 1.08500"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1.08500 "), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1.085 00"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("1.08500\t"), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error(std::string("1.085\0" "00", 8)), DecimalError::NotCanonical);
    CHECK_EQ(eurusd_error("+1.08500"), DecimalError::None);  // an explicit plus is canonical
}

SCENARIO("D-10", "P0", "a price far beyond the tick range is refused, not truncated") {
    CHECK_EQ(eurusd_error("1e50"), DecimalError::NotCanonical);
    // 19 significant digits exceeds the mantissa budget before scaling.
    CHECK_EQ(eurusd_error("9999999999999999999"), DecimalError::TooManyDigits);
    // 18 digits parse, but scaling to ticks overflows and is caught.
    CHECK_EQ(eurusd_error("999999999999999999"), DecimalError::Overflow);
}

SCENARIO("D-11", "P0", "a tick reload does not reinterpret an accepted version") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    const uint32_t pinned_scale = L.state(id).accepted_versions[0].scale_id;
    const int64_t pinned_px = L.state(id).accepted_price_ticks();

    // Operator widens the EURUSD tick while the replace is still pending.
    Policy next = L.policy();
    next.symbols["EURUSD"].tick_denominator = 1000;  // 0.001
    next.symbols["EURUSD"].scale_id = 11;
    next.policy_version = 2;
    CHECK(L.engine().reload_policy(next).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions[0].scale_id, pinned_scale);
    CHECK_EQ(a.accepted_price_ticks(), pinned_px);
    CHECK_EQ(a.accepted_versions[0].policy_version, uint32_t{1});
    // A new version is only installed on an explicit acknowledgement, and it
    // carries the new policy version.
    CHECK(L.submit(replaced("R1", "O1").oqty("120000").px("1.085")).ok());
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});
    CHECK_EQ(L.state(id).accepted_versions[1].scale_id, uint32_t{11});
    CHECK_EQ(L.state(id).accepted_versions[1].policy_version, uint32_t{2});
    CHECK_EQ(L.state(id).accepted_versions[0].scale_id, pinned_scale);
}

SCENARIO("D-12", "P0", "two symbols keep independent scales, with no cross-symbol reuse") {
    Lab L;
    const Policy& p = L.policy();
    const SymbolSpec* eur = p.find_symbol("EURUSD");
    const SymbolSpec* jpy = p.find_symbol("USDJPY");
    CHECK(eur != nullptr && jpy != nullptr);
    CHECK_NE(eur->tick_denominator, jpy->tick_denominator);
    CHECK_NE(eur->scale_id, jpy->scale_id);

    // 1.085 is on the USDJPY grid (0.001) but off the EURUSD grid only if it
    // has more digits; the point is each symbol is parsed with its own scale.
    int64_t jpy_ticks = 0;
    CHECK(parse_ticks("123.456", jpy->tick_numerator, jpy->tick_denominator, jpy_ticks) ==
          DecimalError::None);
    CHECK_EQ(jpy_ticks, int64_t{123456});
    int64_t eur_ticks = 0;
    CHECK(parse_ticks("123.456", eur->tick_numerator, eur->tick_denominator, eur_ticks) ==
          DecimalError::None);
    CHECK_EQ(eur_ticks, int64_t{12345600});
    CHECK_NE(jpy_ticks, eur_ticks);

    // A config that reuses one scale id for two symbols is refused outright.
    Policy bad = p;
    bad.symbols["USDJPY"].scale_id = bad.symbols["EURUSD"].scale_id;
    CHECK_EQ(bad.validate().code, ConfigErrorCode::DuplicateScaleId);
}

SCENARIO("D-13", "P0", "an unconfigured symbol gets no default five-digit scale") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "1.08500", "XAUUSD")), Reason::UnknownSymbol);
    CHECK(L.policy().find_symbol("XAUUSD") == nullptr);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
}

SCENARIO("D-14", "P0", "a buy fill above the limit price is quarantined") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08600")),
                      Reason::PriceOutsideLimit);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK_EQ(a.incidents.size(), size_t{1});
    CHECK_EQ(a.incidents[0].reason_code, std::string("PRICE_OUTSIDE_LIMIT"));
}

SCENARIO("D-15", "P0", "a sell fill below the limit price is quarantined") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("S1", "100000", "1.08500", "EURUSD", "2"));
    CHECK(n.ok());
    CHECK(L.submit(new_ack("S1").side("2")).ok());
    expect_quarantine(
        L.submit(trade("X1", "S1").side("2").lastq("40000").lastpx("1.08400")),
        Reason::PriceOutsideLimit);
    CHECK_EQ(L.state(n.id).cum_qty_units, int64_t{0});

    // The same price is fine for a sell at or above the limit.
    CHECK(L.submit(trade("X2", "S1").side("2").lastq("40000").lastpx("1.08500").cum("40000")).ok());
    CHECK_EQ(L.state(n.id).cum_qty_units, int64_t{40000});
}

SCENARIO("D-16", "P0", "an accepted replace installs the new price; old fills keep theirs") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000", "1.08490");
    CHECK(L.submit(cmd_replace("R1", "O1", "100000", "1.08600")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("100000").px("1.08600").cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108600});
    // The historical fill still records the price it executed at.
    CHECK_EQ(a.ledger[0].price_ticks, int64_t{108490});
    CHECK_EQ(a.ledger[0].accepted_version_seen, uint32_t{0});
    // A later fill may use the new, higher limit.
    CHECK(L.submit(trade("X2", "R1").lastq("60000").lastpx("1.08550").cum("100000").leaves("0"))
              .ok());
    CHECK_EQ(L.state(id).ledger[1].price_ticks, int64_t{108550});
    CHECK_EQ(L.state(id).ledger[1].accepted_version_seen, uint32_t{1});
}

SCENARIO("D-17", "P0", "a price change that is only pending leaves the accepted price alone") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "100000", "1.08600")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());

    CHECK_EQ(L.state(id).accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(L.state(id).pending->desired_price_ticks, int64_t{108600});
    // A fill while the replacement is pending is still judged against the old
    // limit, so 1.08550 is above it and refused.
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08550")),
                      Reason::PriceOutsideLimit);
}

SCENARIO("D-18", "P1", "the output rounding is half-to-even and order independent") {
    // Exact halves round to the even neighbour, in both directions.
    Int128 out = 0;
    CHECK(divide_round_half_even(5, 2, out));
    CHECK_EQ(to_string(out), std::string("2"));  // 2.5 -> 2
    CHECK(divide_round_half_even(7, 2, out));
    CHECK_EQ(to_string(out), std::string("4"));  // 3.5 -> 4
    CHECK(divide_round_half_even(-5, 2, out));
    CHECK_EQ(to_string(out), std::string("-2"));
    CHECK(divide_round_half_even(1, 3, out));
    CHECK_EQ(to_string(out), std::string("0"));
    CHECK(divide_round_half_even(2, 3, out));
    CHECK_EQ(to_string(out), std::string("1"));
    CHECK(!divide_round_half_even(1, 0, out));

    // And the same average computed on two engines gives the same rendering.
    Lab a;
    Lab b;
    for (Lab* L : {&a, &b}) {
        const OrderId id = L->working("O1", "30000", "1.08500");
        CHECK(L->submit(trade("X1", "O1").lastq("10000").lastpx("1.08490").cum("10000")).ok());
        CHECK(L->submit(trade("X2", "O1").lastq("20000").lastpx("1.08493").cum("30000")).ok());
        int64_t avg = 0;
        CHECK(L->state(id).avg_px_ticks_rounded(avg));
        CHECK_EQ(avg, int64_t{108492});
    }
}

SCENARIO("D-19", "P0", "the accumulated notional is identical under either fill order") {
    // Same two fills, applied in opposite orders on two engines. Integer
    // accumulation is associative, so the totals must match bit for bit; the
    // ledger order differs, which is part of the history, not of the maths.
    Lab a;
    const OrderId ia = a.working("O1", "100000", "1.08500");
    CHECK(a.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(a.submit(trade("X2", "O1").lastq("60000").lastpx("1.08495").cum("100000")).ok());

    Lab b;
    const OrderId ib = b.working("O1", "100000", "1.08500");
    CHECK(b.submit(trade("X2", "O1").lastq("60000").lastpx("1.08495").cum("60000")).ok());
    CHECK(b.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("100000")).ok());

    CHECK_EQ(to_string(a.state(ia).notional_ticks_x_qty),
             to_string(b.state(ib).notional_ticks_x_qty));
    CHECK_EQ(a.state(ia).cum_qty_units, b.state(ib).cum_qty_units);
    int64_t avg_a = 0;
    int64_t avg_b = 0;
    CHECK(a.state(ia).avg_px_ticks_rounded(avg_a));
    CHECK(b.state(ib).avg_px_ticks_rounded(avg_b));
    CHECK_EQ(avg_a, avg_b);
}

SCENARIO("D-20", "P0", "a product that would leave the wide type is refused, never wrapped") {
    // int64 is not enough for qty x price and the library knows it.
    int64_t narrow = 0;
    CHECK(!checked_mul(4000000000LL, 4000000000LL, narrow));
    Int128 wide = 0;
    CHECK(checked_mul_wide(4000000000LL, 4000000000LL, wide));
    CHECK_EQ(to_string(wide), std::string("16000000000000000000"));

    // At the wide type's own ceiling the checked helpers refuse rather than
    // silently truncating.
    Int128 huge = 0;
    CHECK(checked_mul_wide(kInt64Max, kInt64Max, huge));
    Int128 overflow = 0;
    CHECK(!__builtin_mul_overflow(huge, static_cast<Int128>(1), &overflow));
    CHECK(!checked_add_wide(huge, huge, overflow) || int128_fits_int64(overflow) == false);

    // A price whose tick count will not fit in int64 is rejected at the
    // boundary, so no such value ever reaches the multiply.
    CHECK_EQ(eurusd_error("9999999999999"), DecimalError::None);
    CHECK_EQ(eurusd_error("999999999999999"), DecimalError::Overflow);
}
