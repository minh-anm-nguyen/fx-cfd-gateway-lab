// Group E -- request identity, order lineage and correlation
// (design doc 18.E). An identifier that resolves to the wrong chain is the
// cheapest way to lose a fill, so nothing here is allowed to resolve by
// guessing.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("E-01", "P0", "a duplicate initial ClOrdID in one firm and day is a collision") {
    Lab L;
    CHECK(L.submit(cmd_new("O1", "100000")).ok());
    expect_reject(L.submit(cmd_new("O1", "50000")), Reason::IdCollision);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("E-02", "P0", "the account does not widen the ClOrdID namespace") {
    Lab L;
    CHECK(L.submit(cmd_new("O1", "100000", "1.08500", "EURUSD", "1", "ACC1")).ok());
    // Same firm, same day, same ClOrdID, different account: still a collision
    // in the strict v1 profile.
    expect_reject(L.submit(cmd_new("O1", "100000", "1.08500", "EURUSD", "1", "ACC2")),
                  Reason::IdCollision);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("E-03", "P0", "the same ClOrdID under a different firm is a separate chain") {
    Lab L;
    CHECK(L.submit(cmd_new("O1")).ok());
    LocalCommand other = cmd_new("O1");
    other.firm = "FIRM2";
    CHECK(L.submit(other).ok());
    CHECK_EQ(L.engine().chain_count(), size_t{2});
    // And the two are indexed apart.
    CHECK(L.engine().find_by_cl_ord_id("FIRM", "2026-10-30", "O1") !=
          L.engine().find_by_cl_ord_id("FIRM2", "2026-10-30", "O1"));
}

SCENARIO("E-04", "P0", "a ClOrdID still unresolved cannot be reused on a new business day") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    CHECK_EQ(L.lc(n.id), Lifecycle::ReconciliationRequired);

    LocalCommand next_day = cmd_new("O1");
    next_day.business_day = "2026-10-31";
    expect_reject(L.submit(next_day), Reason::IdCollision);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("E-05", "P0", "a cancel referencing the accepted version creates a pending cancel") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->kind, CommandKind::Cancel);
    CHECK_EQ(a.pending->orig_cl_ord_id, std::string("O1"));
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
}

SCENARIO("E-06", "P0", "a cancel referencing an unaccepted version is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());
    // R1 is pending, not accepted, so it cannot be the OrigClOrdID of anything.
    expect_reject(L.submit(cmd_cancel("C1", "R1")), Reason::PendingMutation);
    CHECK_EQ(L.state(id).accepted_cl_ord_id(), std::string("O1"));
}

SCENARIO("E-07", "P0", "a replace referencing the last accepted version is admitted") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.pending->kind, CommandKind::Replace);
    CHECK_EQ(a.pending->orig_cl_ord_id, std::string("O1"));
    // The accepted version has not moved.
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
}

SCENARIO("E-08", "P0", "a replace naming a superseded OrigClOrdID is refused") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());
    CHECK_EQ(L.state(id).accepted_cl_ord_id(), std::string("R1"));

    // O1 is now stale; R1 is the accepted version.
    expect_reject(L.submit(cmd_replace("R2", "O1", "130000")), Reason::LineageMismatch);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{2});
    // No routing effect was produced for R2.
    CHECK_EQ(L.engine().sink().count_key("route-replace|FIRM|2026-10-30|R2"), size_t{0});
}

SCENARIO("E-09", "P0", "a cancel reject naming an unknown request is quarantined") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(cancel_reject("CX", "O1", "1")), Reason::UnknownRequest);
    // The real request is untouched.
    CHECK(L.state(id).pending.has_value());
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("C1"));
}

SCENARIO("E-10", "P0", "a cancel reject with the wrong OrigClOrdID is quarantined") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(cancel_reject("C1", "OTHER", "1")), Reason::LineageMismatch);
    CHECK(L.state(id).pending.has_value());
}

SCENARIO("E-11", "P1", "a new OrderID on a verified ClOrdID is recorded once as an alias") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK_EQ(L.state(id).order_id_aliases.size(), size_t{1});
    CHECK(L.state(id).has_order_id("LPO-O1"));

    // A later report for the same ClOrdID brings a second OrderID. It is added
    // as an alias, not treated as a new order.
    CHECK(L.submit(trade("X1", "O1").oid("LPO1-B").lastq("40000").lastpx("1.08490").cum("40000"))
              .ok());
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.order_id_aliases.size(), size_t{2});
    CHECK(a.has_order_id("LPO1-B"));
    CHECK_EQ(L.engine().chain_count(), size_t{1});

    // Recorded once: a repeat of the same alias does not grow the list.
    CHECK(L.submit(trade("X2", "O1").oid("LPO1-B").lastq("10000").lastpx("1.08490").cum("50000"))
              .ok());
    CHECK_EQ(L.state(id).order_id_aliases.size(), size_t{2});
}

SCENARIO("E-12", "P0", "the same OrderID from a different peer does not cross-attach") {
    Lab L;
    const OrderId a_id = L.working("O1");
    LocalCommand second = cmd_new("P1", "50000");
    second.firm = "FIRM2";
    const EngineResult b = L.submit(second);
    CHECK(b.ok());
    CHECK(L.submit(new_ack("P1").peer("LP2").oid("SHARED").acct("ACC1")).ok());

    // LP1 now reports the same OrderID string. Scoped by peer, so it is a
    // different alias namespace and must not reach chain B.
    CHECK(L.submit(trade("X1", "O1").oid("SHARED").lastq("40000").lastpx("1.08490").cum("40000"))
              .ok());
    CHECK_EQ(L.state(a_id).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(b.id).cum_qty_units, int64_t{0});
}

SCENARIO("E-13", "P0", "one peer returning one OrderID for two chains is an alias collision") {
    Lab L;
    const OrderId first = L.working("O1");
    const EngineResult second = L.submit(cmd_new("O2", "50000"));
    CHECK(second.ok());
    // The same peer hands out LPO1 again, for a different ClOrdID.
    expect_quarantine(L.submit(new_ack("O2").oid("LPO-O1")), Reason::AliasCollision);
    // The original mapping is not reassigned.
    CHECK(L.state(first).has_alias("LP1", "LPO-O1"));
    CHECK(!L.state(second.id).has_order_id("LPO-O1"));
}

SCENARIO("E-14", "P0", "an account mismatch is quarantined even when the OrderID matches") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(
        L.submit(trade("X1", "O1").oid("LPO-O1").acct("ACC9").lastq("40000").lastpx("1.08490")),
        Reason::AccountMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("E-15", "P0", "a symbol mismatch is quarantined even when the OrderID matches") {
    Lab L;
    const OrderId id = L.working("O1");
    expect_quarantine(
        L.submit(trade("X1", "O1").oid("LPO-O1").sym("USDJPY").lastq("40000").lastpx("1.08490")),
        Reason::SymbolMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("E-16", "P0", "a side mismatch on an otherwise valid execution is quarantined") {
    Lab L;
    const OrderId id = L.working("O1");  // buy
    expect_quarantine(L.submit(trade("X1", "O1").side("2").lastq("40000").lastpx("1.08490")),
                      Reason::SideMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).side, Side::Buy);
}

SCENARIO("E-17", "P0", "a report matching no chain is an orphan, not a new order") {
    Lab L;
    CHECK(L.working("O1").valid());
    const EngineResult r =
        L.submit(trade("X9", "GHOST").oid("UNKNOWN-OID").lastq("40000").lastpx("1.08490"));
    expect_quarantine(r, Reason::IdentityInsufficient);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    CHECK_EQ(L.engine().metrics().orphan_reports, uint64_t{1});
}

SCENARIO("E-18", "P0", "a known ClOrdID with an unknown OrderID does not mutate other chains") {
    Lab L;
    const OrderId first = L.working("O1");
    const OrderId second = L.working("O2", "50000");

    // Correlates by ClOrdID; the unfamiliar OrderID becomes an audited alias
    // on the correct chain and touches nothing else.
    CHECK(L.submit(trade("X1", "O1").oid("AFTER-RECONNECT").lastq("40000").lastpx("1.08490").cum(
                                                                "40000"))
              .ok());
    CHECK_EQ(L.state(first).cum_qty_units, int64_t{40000});
    CHECK(L.state(first).has_order_id("AFTER-RECONNECT"));
    CHECK_EQ(L.state(second).cum_qty_units, int64_t{0});
    CHECK(!L.state(second).has_order_id("AFTER-RECONNECT"));
}

SCENARIO("E-19", "P1", "ClOrdID comparison is byte exact, with no silent case folding") {
    Lab L;
    CHECK(L.working("O1").valid());
    CHECK(!L.policy().normalize_cl_ord_id_case);
    // "o1" is a different identifier from "O1" unless the Rules of Engagement
    // say otherwise, so it opens its own chain rather than aliasing.
    CHECK(L.submit(cmd_new("o1", "50000")).ok());
    CHECK_EQ(L.engine().chain_count(), size_t{2});
    CHECK(L.engine().find_by_cl_ord_id("FIRM", "2026-10-30", "o1") != nullptr);
    CHECK(L.engine().find_by_cl_ord_id("FIRM", "2026-10-30", "O1") !=
          L.engine().find_by_cl_ord_id("FIRM", "2026-10-30", "o1"));
}

SCENARIO("E-20", "P0", "an empty or oversized identifier is refused before any map lookup") {
    Lab L;
    expect_reject(L.submit(cmd_new("", "100000")), Reason::IdTooLong);
    expect_reject(L.submit(cmd_new(std::string(200, 'x'))), Reason::IdTooLong);
    CHECK_EQ(L.engine().chain_count(), size_t{0});

    // And on the report side.
    const OrderId id = L.working("O1");
    expect_quarantine(
        L.submit(trade(std::string(200, 'e'), "O1").lastq("40000").lastpx("1.08490")),
        Reason::IdTooLong);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}
