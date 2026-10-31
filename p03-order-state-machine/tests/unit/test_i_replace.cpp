// Group I -- replace flow, accepted versions and CumQty preservation
// (design doc 18.I). Executed quantity accumulates across versions. Nothing in
// this group is allowed to reset it.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("I-01", "P0", "raising the quantity leaves the accepted quantity alone until the ack") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.pending->desired_qty_units, int64_t{120000});
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingReplace);
    CHECK_EQ(a.leaves_qty_units(), int64_t{100000});
}

SCENARIO("I-02", "P0", "lowering the quantity on an unfilled order only applies on 150=5") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "80000")).ok());
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});
    CHECK(L.submit(replaced("R1", "O1").oqty("80000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{80000});
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.leaves_qty_units(), int64_t{80000});
}

SCENARIO("I-03", "P0", "raising the quantity on a part-filled order preserves CumQty") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000").cum("40000").leaves("80000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{120000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{80000});
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{40000});
}

SCENARIO("I-04", "P0", "lowering the quantity above CumQty leaves the executed part intact") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_replace("R1", "O1", "70000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("70000").cum("40000").leaves("30000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{70000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{30000});
}

SCENARIO("I-05", "P1", "a replace down to exactly CumQty is a policy boundary, refused in v1") {
    Lab L;
    CHECK(L.policy().reject_replace_qty_equal_cum);
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_reject(L.submit(cmd_replace("R1", "O1", "40000")), Reason::ZeroWorkingLeaves);
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});

    // With the boundary configured the other way the request is admitted, and
    // the decision is explicit either way rather than accidental.
    Policy permissive = default_lab_policy();
    permissive.reject_replace_qty_equal_cum = false;
    Lab L2(permissive);
    const OrderId id2 = L2.part_filled("O1", "100000", "40000");
    CHECK(L2.submit(cmd_replace("R1", "O1", "40000")).ok());
    CHECK_EQ(L2.state(id2).pending->desired_qty_units, int64_t{40000});
}

SCENARIO("I-06", "P0", "a replace below CumQty is refused with no new version") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_reject(L.submit(cmd_replace("R1", "O1", "30000")), Reason::QtyBelowCum);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{0});
}

SCENARIO("I-07", "P0", "a price-only replace uses the old price until the final ack") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "", "1.08600")).ok());
    CHECK_EQ(L.state(id).accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(L.state(id).pending->desired_price_ticks, int64_t{108600});
    CHECK(L.submit(replaced("R1", "O1").px("1.08600")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108600});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});  // quantity unchanged
}

SCENARIO("I-08", "P0", "old fills keep their execution price after a price replace") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000", "1.08490");
    CHECK(L.submit(cmd_replace("R1", "O1", "", "1.08600")).ok());
    CHECK(L.submit(replaced("R1", "O1").px("1.08600").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "R1").lastq("20000").lastpx("1.08560").cum("60000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger[0].price_ticks, int64_t{108490});
    CHECK_EQ(a.ledger[1].price_ticks, int64_t{108560});
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    // The weighted average spans both versions.
    int64_t avg = 0;
    CHECK(a.avg_px_ticks_rounded(avg));
    CHECK_EQ(avg, int64_t{108513});  // (40000*108490 + 20000*108560)/60000
}

SCENARIO("I-09", "P0", "a replace naming the wrong OrigClOrdID is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_reject(L.submit(cmd_replace("R1", "NOPE", "120000")), Reason::LineageMismatch);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
    CHECK(!L.state(id).pending.has_value());
}

SCENARIO("I-10", "P0", "a pending-replace report never adopts the desired fields") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000", "1.08600")).ok());
    // The peer echoes the requested values on the pending report.
    CHECK(L.submit(pending_replace("R1").oqty("120000").px("1.08600")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
}

SCENARIO("I-11", "P0", "a valid final replace appends exactly one version") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.accepted_versions[0].version, uint32_t{0});
    CHECK_EQ(a.accepted_versions[1].version, uint32_t{1});
    CHECK(!a.pending.has_value());
    CHECK(!a.peer_pending_replace);
    CHECK(check_invariants(a, L.policy()).empty());
}

SCENARIO("I-12", "P0", "a replace reject resolves the request and keeps the old version") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(cancel_reject("R1", "O1", "2")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK(!a.pending.has_value());
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
}

SCENARIO("I-13", "P0", "a replace ack below CumQty is quarantined, never committed") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_replace("R1", "O1", "70000")).ok());
    // The peer acknowledges a quantity below what has already executed.
    expect_quarantine(L.submit(replaced("R1", "O1").oqty("30000")), Reason::QtyBelowCum);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
}

SCENARIO("I-14", "P0", "a replace ack with an off-grid price is quarantined") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "", "1.08600")).ok());
    expect_quarantine(L.submit(replaced("R1", "O1").px("1.086001")), Reason::ScaleMismatch);
    CHECK_EQ(L.state(id).accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
}

SCENARIO("I-15", "P0", "a replace ack for an unknown request changes no version") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    expect_quarantine(L.submit(replaced("R9", "O1").oqty("500000")), Reason::UnknownRequest);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});
}

SCENARIO("I-16", "P0", "chaining a replace onto an unaccepted replace is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(!L.policy().allow_optimistic_chaining);
    expect_reject(L.submit(cmd_replace("R2", "R1", "130000")), Reason::PendingMutation);
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("R1"));
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R2"), size_t{0});
}

SCENARIO("I-17", "P0", "a replace while a cancel is pending is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_reject(L.submit(cmd_replace("R1", "O1", "120000")), Reason::PendingMutation);
    CHECK_EQ(L.state(id).pending->kind, CommandKind::Cancel);
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R1"), size_t{0});
}

SCENARIO("I-18", "P0", "a replace timeout keeps the old accepted version") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000", "1.08600")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "R1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK(a.pending->outcome_unknown);
    CHECK_EQ(a.lifecycle(), Lifecycle::ReconciliationRequired);
}

SCENARIO("I-19", "P0", "a fill before the replace ack lands on the old version") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK_EQ(L.state(id).ledger[0].accepted_version_seen, uint32_t{0});
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});

    CHECK(L.submit(replaced("R1", "O1").oqty("120000").cum("40000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger[0].accepted_version_seen, uint32_t{0});
}

SCENARIO("I-20", "P0", "CumQty accumulates across versions and the ledger agrees") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(cmd_replace("R1", "O1", "150000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("150000").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "R1").lastq("60000").lastpx("1.08495").cum("100000")).ok());
    CHECK(L.submit(cmd_replace("R2", "R1", "200000")).ok());
    CHECK(L.submit(replaced("R2", "R1").oqty("200000").cum("100000")).ok());
    CHECK(L.submit(trade("X3", "R2").lastq("50000").lastpx("1.08499").cum("150000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{3});
    CHECK_EQ(a.cum_qty_units, int64_t{150000});  // never reset
    CHECK_EQ(a.accepted_qty_units(), int64_t{200000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{50000});
    CHECK_EQ(a.ledger.size(), size_t{3});
    CHECK_EQ(a.cum_qty_units, a.ledger_cum_qty());
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{150000});
    CHECK(check_invariants(a, L.policy()).empty());
}
