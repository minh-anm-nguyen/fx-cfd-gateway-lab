// Section G: New order lifecycle, acknowledgements and fills.
#include <set>

#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-G-01", "P0", "a New that is acknowledged and then filled ends Filled, once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK_EQ(c->cum_qty, 100000);
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-G-02", "P0", "an acknowledgement alone is not a fill") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->status, OrdStatus::New);
    CHECK_EQ(c->cum_qty, 0);
    CHECK_EQ(c->fills.size(), std::size_t{0});
    CHECK_EQ(c->leaves_qty, 100000);
    CHECK_INV(L);
}

SCENARIO("OR-G-03", "P0", "a rejected New releases the reservation exactly once") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string reservation = L.intent("K1")->risk_reservation_id;
    CHECK(L.risk->held(reservation));

    L.inject(L.lp->rejected(a.cl_ord_id.str(), "limit not accepted"));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Rejected);
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Rejected);
    CHECK(!L.risk->held(reservation));
    const std::size_t effective = L.risk->effective_releases();

    // A redelivery of the same rejection does not release twice.
    L.session->redeliver(1);
    L.rt->pump_inbound();
    CHECK_EQ(L.risk->effective_releases(), effective);
    CHECK_INV(L);
}

SCENARIO("OR-G-04", "P0",
         "a fill before the acknowledgement wins, and the late ack is not a regression") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();

    L.inject(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::PartiallyFilled);
    CHECK_INV(L);

    L.inject(L.lp->new_ack(a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::PartiallyFilled);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_INV(L);
}

SCENARIO("OR-G-05", "P0", "two partial fills total the order quantity with no overfill") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 60000, 108510);
    L.drive();

    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_EQ(c->cum_qty, 100000);
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_EQ(c->status, OrdStatus::Filled);
    // Weighted average, computed by P03 from the ledger.
    CHECK_EQ(c->avg_px(), (40000LL * 108500 + 60000LL * 108510) / 100000);
    CHECK_INV(L);
}

SCENARIO("OR-G-06", "P0", "a duplicated execution identity adds nothing") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec t = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    t.cum_qty = 40000;
    t.leaves_qty = 60000;
    L.inject(t);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);

    for (int i = 0; i < 10; ++i) L.inject(t);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK(L.metrics.counter(metric::kDuplicateExec) >= 10);
    CHECK_INV(L);
}

SCENARIO("OR-G-07", "P0", "a New acknowledgement after a full fill does not reopen the order") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.inject(L.lp->trade(a.cl_ord_id.str(), "EX1", 100000, 108500));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);

    L.inject(L.lp->new_ack(a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);
    CHECK_EQ(L.chain(a.chain_id)->leaves_qty, 0);
    // The late acknowledgement is on the record rather than discarded.
    CHECK(L.disk.audit.count(AuditKind::InboundApplied) >= std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-G-08", "P0", "an order the LP never acknowledges does not get resubmitted") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    for (int i = 0; i < 6; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::ManualHold);
    CHECK_INV(L);
}

SCENARIO("OR-G-09", "P0", "a lost write is never reported as a rejection") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.session_disk.blackhole = true;
    L.drive();
    L.tick(L.cfg.limits.ack_timeout_micros * 3);

    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Rejected);
    CHECK_NE(L.intent("K1")->business, BusinessOutcome::Rejected);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-G-10", "P0", "a reservation revoked between commit and dispatch stops the send") {
    Lab L;
    L.must_submit_new("K1");
    L.risk->revoke(L.intent("K1")->risk_reservation_id);
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{0});
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("OR-G-11", "P0", "a fill naming another instrument changes no money") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.symbol = "GBP/USD";
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::SemanticConflict);
    CHECK_INV(L);
}

SCENARIO("OR-G-12", "P0", "a fill on the wrong side is not sign converted") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.side = Side::Sell;
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.chain(a.chain_id)->side, Side::Buy);
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK_INV(L);
}

SCENARIO("OR-G-13", "P0", "quantity fields that contradict each other are held") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 60000, 108500);
    s.raw[14] = "10000";  // CumQty below LastQty
    s.raw[151] = "90000";
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.chain(a.chain_id)->hold.has_value() || L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-G-14", "P0", "a Trade with zero LastQty is not a status event") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 0, 108500);
    s.raw[32] = "0";
    s.cum_qty = 0;
    s.leaves_qty = 100000;
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-G-15", "P0", "a price outside the fixed point range leaves the state alone") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[31] = "99999999.99999";  // far above the symbol bound
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-G-16", "P0", "a client retry while the LP already has the order sends nothing new") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    for (int i = 0; i < 20; ++i) {
        auto again = L.submit(L.new_order("K1"));
        CHECK(again.is_ok());
        CHECK_EQ(again.value().cl_ord_id.str(), a.cl_ord_id.str());
        L.drive();
    }
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-G-17", "P1", "a thousand distinct chains keep distinct identities") {
    Lab L;
    std::vector<AcceptedIntent> accepted;
    // Two limits shape this loop and both are correct behaviour rather than the
    // subject of this case: admission is capped at 500 per account per second
    // (OR-P-04), and a risk verdict expires after two seconds (OR-A-18). So the
    // thousand orders go out in batches that are dispatched before the clock
    // moves on.
    for (int batch = 0; batch < 3; ++batch) {
        const int count = batch < 2 ? 400 : 200;
        for (int i = 0; i < count; ++i)
            accepted.push_back(
                L.must_submit_new("K" + std::to_string(batch * 400 + i)));
        L.drive(64);
        L.advance(1100000);
    }
    L.drive(64);

    std::set<std::string> ids;
    std::set<std::string> chains;
    for (const AcceptedIntent& a : accepted) {
        ids.insert(a.cl_ord_id.str());
        chains.insert(a.chain_id.str());
    }
    CHECK_EQ(ids.size(), std::size_t{1000});
    CHECK_EQ(chains.size(), std::size_t{1000});
    CHECK_EQ(L.disk.chains.size(), std::size_t{1000});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1000});
    for (const AcceptedIntent& a : accepted) CHECK(L.chain(a.chain_id)->lp_acked);
    CHECK_INV(L);
}

SCENARIO("OR-G-18", "P0", "a rejection after a fill is a contradiction, not a silent discard") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    L.inject(L.lp->rejected(a.cl_ord_id.str(), "late reject"));
    // The fill is kept, the order is not quietly marked rejected, and a human is
    // told about the contradiction.
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Rejected);
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-G-19", "P1", "a price exactly on the configured bound follows an explicit rule") {
    Lab L;
    CHECK(L.submit(L.new_order("K1", "100000", "0.00001")).is_ok());
    CHECK_EQ(L.intent("K1")->price_ticks, 1);
    CHECK(L.submit(L.new_order("K2", "100000", "10.00000")).is_ok());
    CHECK_EQ(L.intent("K2")->price_ticks, 1000000);
    // One tick past the bound is refused rather than clamped.
    CHECK(L.submit(L.new_order("K3", "100000", "10.00001")).is_err());
    // The quantity bounds behave the same way.
    CHECK(L.submit(L.new_order("K4", "1000", "1.08500")).is_ok());
    CHECK(L.submit(L.new_order("K5", "999", "1.08500")).is_err());
    CHECK_INV(L);
}

SCENARIO("OR-G-20", "P0", "a hundred identical orders are told apart only by their identifiers") {
    Lab L;
    std::vector<AcceptedIntent> accepted;
    for (int i = 0; i < 100; ++i) accepted.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);

    const AcceptedIntent& target = accepted.at(42);
    L.lp->execute(target.cl_ord_id.str(), 100000, 108500);
    L.drive();

    for (std::size_t i = 0; i < accepted.size(); ++i) {
        const p03::ChainState* c = L.chain(accepted[i].chain_id);
        if (i == 42) {
            CHECK_EQ(c->cum_qty, 100000);
            CHECK_EQ(c->status, OrdStatus::Filled);
        } else {
            CHECK_EQ(c->cum_qty, 0);
            CHECK_EQ(c->status, OrdStatus::New);
        }
    }
    CHECK_INV(L);
}
