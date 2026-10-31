// Group G -- ordering, gaps, causal watermarks and late messages
// (design doc 18.G). Arrival order is evidence, not truth. Where causality
// cannot be established the only answer is to hold or to quarantine.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("G-01", "P0", "an out-of-order source sequence is never processed ahead of the gap") {
    // P01 owns FIX session sequencing; what P03 must guarantee is that it
    // never advances its own watermark past an event it has not applied.
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").seq(10).lastq("10000").lastpx("1.08490").cum("10000")).ok());
    CHECK_EQ(L.state(id).max_source_seq, uint64_t{10});

    // An event from before the watermark does not move it backwards.
    CHECK(L.submit(trade("X2", "O1").seq(4).lastq("10000").lastpx("1.08490").cum("20000")).ok());
    CHECK_EQ(L.state(id).max_source_seq, uint64_t{10});
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{20000});
}

SCENARIO("G-02", "P0", "an event without a source cursor is held, never silently accepted") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect(L.submit(trade("X1", "O1").no_cursor().lastq("40000").lastpx("1.08490")),
           Decision::HeldForCausality, Reason::MissingSourceCursor);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.engine().held_count(id), size_t{1});
}

SCENARIO("G-03", "P0", "a trade before the New Ack is held, then applied once") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(n.ok());
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());

    // The fill arrives on a path that overtook the acknowledgement.
    expect(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
           Decision::HeldForCausality, Reason::AwaitingCorrelation);
    CHECK_EQ(L.state(n.id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.engine().held_count(n.id), size_t{1});

    // The acknowledgement establishes correlation and the held fill is retried.
    CHECK(L.submit(new_ack("O1")).ok());
    const OrderAggregate& a = L.state(n.id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK_EQ(L.engine().held_count(n.id), size_t{0});
    CHECK_EQ(oracle(L.store(), n.id).applied_trades, size_t{1});
}

SCENARIO("G-04", "P0", "a final cancel ahead of the pending report resolves, and does not regress") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    // The final arrives first.
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
    // The pending report turns up afterwards and changes nothing.
    expect(L.submit(pending_cancel("C1")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("G-05", "P0", "a replace acknowledgement ahead of its pending report still resolves") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());
    CHECK_EQ(L.state(id).accepted_cl_ord_id(), std::string("R1"));

    expect(L.submit(pending_replace("R1")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});
    CHECK_EQ(L.lc(id), Lifecycle::Working);
}

SCENARIO("G-06", "P0", "a cancel reject correlates by the committed request key") {
    Lab L;
    const OrderId id = L.working("O1");
    // The intent is committed; no Dispatched callback has come back yet.
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK_EQ(L.state(id).pending->phase, PendingRequest::Phase::Prepared);
    CHECK(L.submit(cancel_reject("C1", "O1", "1")).ok());

    const OrderAggregate& a = L.state(id);
    CHECK(!a.pending.has_value());
    CHECK_EQ(a.lifecycle(), Lifecycle::Working);
}

SCENARIO("G-07", "P0", "a resent trade behind the watermark applies at most once") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").seq(7).lastq("40000").lastpx("1.08490").cum("40000")).ok());
    // The same execution replayed from an earlier point in the session.
    expect_decision(
        L.submit(trade("X1", "O1").seq(3).lastq("40000").lastpx("1.08490").cum("40000")),
        Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(id).ledger.size(), size_t{1});
}

SCENARIO("G-08", "P0", "a delayed fill on the old version applies while the replace is pending") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());

    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger[0].accepted_version_seen, uint32_t{0});
    // The pending replace survived the fill.
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingReplace);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
}

SCENARIO("G-09", "P0", "a fill on a superseded version after the replace is quarantined") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());

    // O1 is no longer the accepted version. Placing this fill in time would
    // require proof the engine does not have.
    expect_quarantine(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490")),
                      Reason::LineageMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});
}

SCENARIO("G-10", "P0", "a snapshot from a superseded reconciliation generation is ignored") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "C1")).ok());
    CHECK_EQ(L.state(id).reconciliation_generation, uint64_t{2});

    // An answer to query generation one arrives after generation two was
    // issued. It must not resolve anything.
    expect(L.submit(status_snapshot("O1", "ST-OLD").status(ord_status::kCanceled).cum("0").gen(1)),
           Decision::DuplicateNoop, Reason::StaleGeneration);
    CHECK(L.state(id).reconciliation_required);
    CHECK(L.lc(id) != Lifecycle::Canceled);
}

SCENARIO("G-11", "P0", "generation, not wall-clock order, decides which snapshot wins") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "C1")).ok());
    CHECK(L.submit(sys(SystemKind::ReconcileStarted, "C1")).ok());

    // An older SendingTime but the current generation: it is the answer we
    // asked for, so it resolves.
    CHECK(L.submit(status_snapshot("O1", "ST-NEW")
                       .status(ord_status::kCanceled)
                       .cum("0")
                       .gen(1)
                       .sending("20200101-00:00:00.000"))
              .ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);
}

SCENARIO("G-12", "P0", "a source clock moving backwards causes no backward transition") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .seq(5)
                       .sending("20261030-10:00:00.000")
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000"))
              .ok());
    // A later event carrying an earlier timestamp. The commit order, not the
    // timestamp, decides.
    CHECK(L.submit(trade("X2", "O1")
                       .seq(6)
                       .sending("20261029-08:00:00.000")
                       .lastq("20000")
                       .lastpx("1.08491")
                       .cum("60000"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK_EQ(a.ledger[0].exec_id, std::string("X1"));
    CHECK_EQ(a.ledger[1].exec_id, std::string("X2"));
}

SCENARIO("G-13", "P0", "the same semantic event under a new session epoch is a no-op") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").epoch(1).seq(9).lastq("40000").lastpx("1.08490").cum("40000"))
              .ok());
    expect_decision(L.submit(trade("X1", "O1")
                                 .epoch(2)
                                 .seq(1)
                                 .session("S2")
                                 .lastq("40000")
                                 .lastpx("1.08490")
                                 .cum("40000")),
                    Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("G-14", "P0", "two peers reporting the same chain in parallel is ambiguous") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").peer("LP1").lastq("40000").lastpx("1.08490").cum("40000"))
              .ok());

    // A different counterparty now claims to be executing the same order. The
    // chain belongs to one peer; this is not a fill to accept.
    const EngineResult r =
        L.submit(trade("X2", "O1").peer("LP9").lastq("20000").lastpx("1.08490").cum("60000"));
    CHECK_MSG(r.decision == Decision::Quarantined,
              std::string("expected quarantine, got ") + to_string(r.decision));
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("G-15", "P0", "a status claiming more CumQty than the ledger is held for evidence") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(
        L.submit(status_snapshot("O1").status(ord_status::kPartiallyFilled).cum("70000")),
        Reason::MissingFillEvidence);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.reconciliation_required);
}

SCENARIO("G-16", "P0", "a late New Ack after a reconciled Filled does not regress") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    expect(L.submit(new_ack("O1", "E-ACK-LATE")), Decision::DuplicateNoop,
           Reason::LateReportNoRegression);
    CHECK_EQ(L.lc(id), Lifecycle::Filled);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{100000});
}

SCENARIO("G-17", "P0", "a cancel reject after the cancel was accepted is a contradiction") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    CHECK(L.submit(canceled("C1").cum("0")).ok());
    CHECK_EQ(L.lc(id), Lifecycle::Canceled);

    expect_quarantine(L.submit(cancel_reject("C1", "O1", "1")), Reason::ContradictoryResponse);
    // The order is not reopened.
    CHECK(L.state(id).terminal());
    CHECK_EQ(L.state(id).done_reason, DoneReason::Canceled);
}

SCENARIO("G-18", "P0", "a replace reject after the replace was accepted is a contradiction") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());

    expect_quarantine(L.submit(cancel_reject("R1", "O1", "2")), Reason::UnknownRequest);
    // The accepted version is retained.
    CHECK_EQ(L.state(id).accepted_cl_ord_id(), std::string("R1"));
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{120000});
}

SCENARIO("G-19", "P0", "an Expired report after a Filled terminal loses no fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1")
                       .status(ord_status::kFilled)
                       .lastq("100000")
                       .lastpx("1.08490")
                       .cum("100000")
                       .leaves("0"))
              .ok());
    expect_quarantine(L.submit(expired("O1")), Reason::PostTerminalEvent);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{100000});
    CHECK_EQ(a.done_reason, DoneReason::Filled);
    CHECK_EQ(a.ledger.size(), size_t{1});
}

SCENARIO("G-20", "P0", "a gap that never closes ends in quarantine, not unbounded memory") {
    Policy p = default_lab_policy();
    p.limits.max_held_events_per_chain = 4;
    Lab L(p);
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(n.ok());

    // Fills keep arriving while the acknowledgement never does.
    for (int i = 0; i < 4; ++i) {
        expect_decision(L.submit(trade("X" + std::to_string(i), "O1")
                                     .lastq("10000")
                                     .lastpx("1.08490")),
                        Decision::HeldForCausality);
    }
    CHECK_EQ(L.engine().held_count(n.id), size_t{4});

    // One more exceeds the bound: the chain is quarantined and the buffer is
    // released rather than grown.
    expect_quarantine(L.submit(trade("X9", "O1").lastq("10000").lastpx("1.08490")),
                      Reason::ResourceExhausted);
    CHECK(L.state(n.id).quarantined);
    CHECK_EQ(L.engine().held_count(n.id), size_t{0});
    CHECK(L.engine().total_held() <= p.limits.max_held_events_per_chain);
    CHECK_EQ(L.state(n.id).cum_qty_units, int64_t{0});
}
