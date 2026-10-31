// Group B -- lifecycle, OrdStatus precedence and transition invariants
// (design doc 18.B). The theme of the group: OrdStatus(39) says what the
// counterparty is reporting, ExecType(150) says why. Neither is a command to
// overwrite local state with.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("B-01", "P0", "ExecType=F with OrdStatus=1 records a partial fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000")
                       .leaves("60000"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{60000});
    CHECK_EQ(a.ledger.size(), size_t{1});
}

SCENARIO("B-02", "P0", "ExecType=F with OrdStatus=2 completes the order") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
}

SCENARIO("B-03", "P0", "a fill reported as OrdStatus=6 counts but stays pending cancel") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "20000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());

    CHECK(L.submit(trade("X2", "O1")
                       .status(ord_status::kPendingCancel)
                       .lastq("30000")
                       .lastpx("1.08495")
                       .cum("50000")
                       .leaves("50000"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{50000});
    // The fill landed, the cancel is still unresolved.
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->kind, CommandKind::Cancel);
    CHECK(a.peer_pending_cancel);
    CHECK(a.done_reason != DoneReason::Canceled);
}

SCENARIO("B-04", "P0", "a fill reported as OrdStatus=E counts and leaves the version alone") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());

    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kPendingReplace)
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000")
                       .leaves("60000"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    // Still the original accepted version: the replacement is only pending.
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingReplace);
}

SCENARIO("B-05", "P0", "a pending cancel report alone never reaches Canceled") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
    CHECK(!a.terminal());
    CHECK(a.done_reason != DoneReason::Canceled);
    // Leaves is still the working remainder, not zero.
    CHECK_EQ(a.leaves_qty_units(), int64_t{100000});
}

SCENARIO("B-06", "P0", "a pending replace report changes no accepted field") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000", "1.08510")).ok());
    const uint64_t before = L.state(id).state_hash();
    CHECK(L.submit(pending_replace("R1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_NE(a.state_hash(), before);  // the peer pending flag did change
}

SCENARIO("B-07", "P0", "an accepted replace appends a version and keeps CumQty") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    CHECK(L.submit(replaced("R1", "O1")
                       .status(ord_status::kPartiallyFilled)
                       .oqty("120000")
                       .cum("40000")
                       .leaves("80000"))
              .ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_versions.size(), size_t{2});
    CHECK_EQ(a.accepted_qty_units(), int64_t{120000});
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("R1"));
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.leaves_qty_units(), int64_t{80000});
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
}

SCENARIO("B-08", "P0", "a valid final cancel is terminal with leaves zero and fills kept") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(pending_cancel("C1")).ok());
    CHECK(L.submit(canceled("C1").cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Canceled);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(!a.pending.has_value());
}

SCENARIO("B-09", "P0", "Expired with a partial fill keeps the fill and zeroes leaves") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(expired("O1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Expired);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
}

SCENARIO("B-10", "P0", "DoneForDay stops working for the day with leaves zero") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(done_for_day("O1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::DoneForDay);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK(a.terminal());
}

SCENARIO("B-11", "P0", "Rejected with CumQty zero is terminal with leaves zero") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1"));
    CHECK(L.submit(rejected("O1").cum("0")).ok());
    const OrderAggregate& a = L.state(n.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Rejected);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, int64_t{0});
}

SCENARIO("B-12", "P0", "OrdStatus=5 Replaced is not a FIX 4.4 state and is quarantined") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    expect_quarantine(L.submit(replaced("R1", "O1").status(ord_status::kReplacedObsolete)),
                      Reason::ObsoleteOrdStatus);
    // No version was installed from the obsolete status.
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
}

SCENARIO("B-13", "P0", "FIX 4.2 ExecType 1 and 2 are not trades under a 4.4 dictionary") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(
        L.submit(trade("X1", "O1").etype("1").lastq("40000").lastpx("1.08490")),
        Reason::WrongFixVersionSemantics);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});

    Lab L2;
    const OrderId id2 = L2.working("O1");
    expect_quarantine(
        L2.submit(trade("X2", "O1").etype("2").lastq("40000").lastpx("1.08490")),
        Reason::WrongFixVersionSemantics);
    CHECK_EQ(L2.state(id2).cum_qty_units, int64_t{0});
}

SCENARIO("B-14", "P0", "a status-only report consistent with local state adds no fill") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const size_t fills_before = L.state(id).ledger.size();
    CHECK(L.submit(status_snapshot("O1").status(ord_status::kPartiallyFilled).cum("40000")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger.size(), fills_before);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.lifecycle(), Lifecycle::PartFilled);
}

SCENARIO("B-15", "P0", "a status report claiming Filled without executions is quarantined") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(
        L.submit(status_snapshot("O1").status(ord_status::kFilled).cum("40000")),
        Reason::MissingFillEvidence);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.done_reason != DoneReason::Filled);
}

SCENARIO("B-16", "P0", "an unsolicited pending-cancel status is quarantined, never canceled") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(L.submit(pending_cancel("O1")), Reason::UnexpectedPending);

    const OrderAggregate& a = L.state(id);
    CHECK(a.done_reason != DoneReason::Canceled);
    CHECK(!a.peer_pending_cancel);
}

SCENARIO("B-17", "P0", "a Filled order receiving a late New Ack does not regress") {
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

    expect(L.submit(new_ack("O1", "E-ACK-LATE")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Filled);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
}

SCENARIO("B-18", "P0", "a Canceled order receiving an old pending-cancel does not regress") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);

    expect(L.submit(pending_cancel("C1", "E-PC-LATE")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("B-19", "P1", "Calculated (39=B) is outside the v1 profile and fails closed") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(
        L.submit(R('8', exec_type::kCalculated, ord_status::kCalculated).id("E-CALC").cl("O1")),
        Reason::UnsupportedEvent);
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("B-20", "P1", "Suspended and Stopped never map to Working by default") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(
        L.submit(R('8', exec_type::kSuspended, ord_status::kSuspended).id("E-SUS").cl("O1")),
        Reason::UnsupportedEvent);
    CHECK_EQ(L.lc(id), Lifecycle::Quarantined);

    Lab L2;
    const OrderId id2 = L2.working("O1");
    expect_quarantine(
        L2.submit(R('8', exec_type::kStopped, ord_status::kStopped).id("E-STP").cl("O1")),
        Reason::UnsupportedEvent);
    CHECK_EQ(L2.lc(id2), Lifecycle::Quarantined);
}
