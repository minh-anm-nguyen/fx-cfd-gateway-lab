// Section C: ClOrdID, OrigClOrdID, OrderID and identity lineage.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-C-01", "P0", "two new orders get distinct ClOrdIDs that survive a restart") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    CHECK_NE(a.cl_ord_id.str(), b.cl_ord_id.str());
    L.crash();
    L.boot();
    const auto c = L.must_submit_new("K3");
    CHECK_NE(c.cl_ord_id.str(), a.cl_ord_id.str());
    CHECK_NE(c.cl_ord_id.str(), b.cl_ord_id.str());
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{3});
    CHECK_INV(L);
}

SCENARIO("OR-C-02", "P0", "the same user intent replayed after a reboot keeps its ClOrdID") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.crash();
    L.boot();
    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK_EQ(again.value().cl_ord_id.str(), a.cl_ord_id.str());
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-03", "P0", "a cancel uses a fresh identifier and records the lineage") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    CHECK_NE(c.cl_ord_id.str(), a.cl_ord_id.str());
    const CommandIntent* ci = L.intent("C1");
    CHECK(ci->orig_cl_ord_id.has_value());
    CHECK_EQ(ci->orig_cl_ord_id->str(), a.cl_ord_id.str());
    L.drive();
    const std::string f = L.session_disk.wire_log.back();
    CHECK(f.find("41=" + a.cl_ord_id.str() + "\x01") != std::string::npos);
    CHECK(f.find("11=" + c.cl_ord_id.str() + "\x01") != std::string::npos);
    CHECK_INV(L);
}

SCENARIO("OR-C-04", "P0", "a replace uses a fresh id and the accepted version does not move") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    CHECK_NE(r.cl_ord_id.str(), a.cl_ord_id.str());
    L.drive();  // the LP answers with Pending Replace only
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK_EQ(c->status, OrdStatus::PendingReplace);
    CHECK(c->pending_replace.has_value());
    CHECK_EQ(c->accepted_qty, 100000);  // the new size is not active yet
    CHECK_INV(L);

    L.lp->accept_replace(r.cl_ord_id.str());
    L.drive();
    c = L.chain(a.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 80000);
    CHECK_INV(L);
}

SCENARIO("OR-C-05", "P0", "a rejected cancel does not become the accepted version") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    L.inject(L.lp->cancel_reject(c1.cl_ord_id.str(), a.cl_ord_id.str(),
                                 CxlRejResponseTo::OrderCancelRequest, 0));
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK(!L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK_INV(L);

    // The next cancel still points at the order, not at the rejected request.
    const auto c2 = L.must_submit_cancel("C2", a.chain_id);
    CHECK_EQ(L.intent("C2")->orig_cl_ord_id->str(), a.cl_ord_id.str());
    CHECK_NE(L.intent("C2")->orig_cl_ord_id->str(), c1.cl_ord_id.str());
    CHECK_INV(L);
}

SCENARIO("OR-C-06", "P0", "a rejected replace does not become the accepted version") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r1 = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    L.inject(L.lp->cancel_reject(r1.cl_ord_id.str(), a.cl_ord_id.str(),
                                 CxlRejResponseTo::OrderCancelReplaceRequest, 2));
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK_EQ(L.chain(a.chain_id)->accepted_qty, 100000);
    CHECK_INV(L);

    const auto r2 = L.must_submit_replace("R2", a.chain_id, "70000", "1.08480");
    CHECK_EQ(L.intent("R2")->orig_cl_ord_id->str(), a.cl_ord_id.str());
    CHECK_NE(L.intent("R2")->orig_cl_ord_id->str(), r1.cl_ord_id.str());
    CHECK_INV(L);
}

SCENARIO("OR-C-07", "P0", "after an accepted replace the lineage follows the new version") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r1 = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    L.lp->accept_replace(r1.cl_ord_id.str());
    L.drive();
    const auto c2 = L.must_submit_cancel("C2", a.chain_id);
    CHECK_EQ(L.intent("C2")->orig_cl_ord_id->str(), r1.cl_ord_id.str());
    L.drive();
    CHECK(L.session_disk.wire_log.back().find("41=" + r1.cl_ord_id.str() + "\x01") !=
          std::string::npos);
    (void)c2;
    CHECK_INV(L);
}

SCENARIO("OR-C-08", "P0", "a second replace while one is pending is blocked, not chained") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    auto second = L.submit(L.replace_order("R2", a.chain_id, "70000", "1.08480"));
    CHECK(second.is_err());
    CHECK_EQ(second.error(), RoutingError::PendingConflict);
    CHECK_EQ(L.wire_count('G'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-09", "P0", "a replace while a cancel is pending is blocked") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    auto r = L.submit(L.replace_order("R1", a.chain_id, "80000", "1.08490"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::PendingConflict);
    CHECK_EQ(L.wire_count('G'), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-C-10", "P0", "the LP OrderID binds once and then cannot change") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK(c->lp_order_id.has_value());
    const std::string bound = c->lp_order_id->str();

    p03::ChainState before = *c;
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.order_id = "LP-999";  // a different OrderID for the same chain
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->lp_order_id->str(), bound);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, before.cum_qty);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_EQ(L.disk.quarantine.at(0).reason, HoldReason::IdentityConflict);
    CHECK_INV(L);
}

SCENARIO("OR-C-11", "P0", "the same OrderID at two LPs stays two separate identities") {
    // Two chains cannot be created against two LPs through one session, so the
    // namespace rule is proved where it lives: in the identity key itself.
    Lab L;
    p03::ChainState a = p03::default_order_reducer().create(
        OrderChainId("ord-A"), TenantId("T1"), AccountId("ACC1"), LpId("LP_A"), "EUR/USD",
        Side::Buy, 5, ClientOrderId("O-A"), 100000, 108500);
    p03::ChainState b = p03::default_order_reducer().create(
        OrderChainId("ord-B"), TenantId("T1"), AccountId("ACC1"), LpId("LP_B"), "EUR/USD",
        Side::Buy, 5, ClientOrderId("O-B"), 100000, 108500);
    a.lp_order_id = OrderId("77");
    b.lp_order_id = OrderId("77");
    L.disk.chains["ord-A"] = a;
    L.disk.chains["ord-B"] = b;
    // Different LPs: no conflict, because the key is LP scoped.
    CHECK(L.violations().empty());

    // The same LP with the same OrderID on two chains is a real conflict.
    b.lp = LpId("LP_A");
    L.disk.chains["ord-B"] = b;
    const auto v = L.violations();
    bool found = false;
    for (const Violation& x : v)
        if (x.invariant == "R-I13") found = true;
    CHECK(found);
}

SCENARIO("OR-C-12", "P0", "an OrderID already bound to another chain is quarantined") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto b = L.must_submit_new("K2");
    L.drive();
    const std::string a_order = L.chain(a.chain_id)->lp_order_id->str();

    // The LP now claims chain B carries chain A's OrderID.
    p02::ReportSpec s = L.lp->trade(b.cl_ord_id.str(), "EX-X", 40000, 108500);
    s.order_id = a_order;
    L.inject(s);

    CHECK_EQ(L.chain(b.chain_id)->cum_qty, 0);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-13", "P0", "a report naming an unrelated ClOrdID becomes an orphan") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.cl_ord_id = "O-9999";
    s.orig_cl_ord_id.clear();
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_EQ(L.disk.quarantine.at(0).reason, HoldReason::OrphanReport);
    CHECK(!L.disk.quarantine.at(0).raw_hash.is_zero());
    CHECK_INV(L);
}

SCENARIO("OR-C-14", "P0", "a cancel acknowledgement naming a superseded version is quarantined") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r1 = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    L.lp->accept_replace(r1.cl_ord_id.str());
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    // The peer answers naming the old, no longer accepted version.
    p02::ReportSpec s = L.lp->canceled(c1.cl_ord_id.str(), a.cl_ord_id.str());
    L.inject(s);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-15", "P0", "a cancel reject whose 434 names the wrong request kind is refused") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    // 434=2 claims a replace was rejected, but only a cancel is outstanding.
    L.inject(L.lp->cancel_reject(c1.cl_ord_id.str(), a.cl_ord_id.str(),
                                 CxlRejResponseTo::OrderCancelReplaceRequest, 2));
    CHECK(L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-16", "P0", "a cancel reject echoing an unknown ClOrdID touches nothing") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    p02::ReportSpec s = L.lp->cancel_reject(c1.cl_ord_id.str(), a.cl_ord_id.str(),
                                            CxlRejResponseTo::OrderCancelRequest, 0);
    s.cl_ord_id = "C-other";
    L.inject(s);
    CHECK(L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-17", "P0", "an allocator seeded backwards is caught by the uniqueness index") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    CHECK_EQ(a.cl_ord_id.str(), std::string("O-1"));
    // Simulate a restore that rolled the counter back under the live index.
    L.disk.next_cl_ord_seq = 1;
    auto r = L.submit(L.new_order("K2"));
    CHECK(r.is_err());
    // Allocation halts rather than reusing an identifier.
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-18", "P0", "a report for another account cannot touch this chain") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.account = "ACC2";
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_EQ(L.disk.quarantine.at(0).reason, HoldReason::SecurityQuarantine);
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("OR-C-19", "P1", "an optimistic vendor lineage is quarantined under strict v1") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r1 = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();
    // The peer treats the pending replace as if it were already accepted and
    // sends a cancel acknowledgement against it.
    p02::ReportSpec s = L.lp->canceled("C-phantom", r1.cl_ord_id.str());
    L.inject(s);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-C-20", "P0", "OrderID NONE is only accepted for the agreed unknown-order reason") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    // Reason 1 is "unknown order", where the ROE permits 37=NONE.
    p02::ReportSpec ok = L.lp->cancel_reject(c1.cl_ord_id.str(), a.cl_ord_id.str(),
                                             CxlRejResponseTo::OrderCancelRequest, 1);
    ok.order_id = "NONE";
    L.inject(ok);
    // The request is closed and NONE was never bound as a real OrderID.
    CHECK(!L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK(!L.chain(a.chain_id)->lp_order_id.has_value() ||
          L.chain(a.chain_id)->lp_order_id->str() != "NONE");
    CHECK_INV(L);

    // Any other reason with 37=NONE is a lineage contradiction.
    const auto c2 = L.must_submit_cancel("C2", a.chain_id);
    L.drive();
    p02::ReportSpec bad = L.lp->cancel_reject(c2.cl_ord_id.str(), a.cl_ord_id.str(),
                                              CxlRejResponseTo::OrderCancelRequest, 0);
    bad.order_id = "NONE";
    L.inject(bad);
    CHECK(L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}
