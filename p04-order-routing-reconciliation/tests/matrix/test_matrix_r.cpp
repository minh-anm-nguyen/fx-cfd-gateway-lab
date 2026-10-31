// Section R: end to end, model checking, interop and adversarial combinations.
#include <set>

#include "support/model.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-R-01", "P0", "end to end: New, acknowledged, filled, with the audit to match") {
    Lab L;
    const auto a = L.must_submit_new("K1", "100000", "1.08500");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 60000, 108520);
    L.drive();

    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK_EQ(c->cum_qty, 100000);
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_EQ(c->avg_px(), (40000LL * 108500 + 60000LL * 108520) / 100000);
    // One intent, one original send, one order at the peer.
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    // The audit trail runs from the command to the applied events.
    CHECK_EQ(L.disk.audit.count(AuditKind::CommandAdmitted), std::size_t{1});
    CHECK_EQ(L.disk.audit.count(AuditKind::OriginalSendAssigned), std::size_t{1});
    CHECK(L.disk.audit.count(AuditKind::InboundApplied) >= std::size_t{3});
    CHECK(L.disk.audit.verify());
    // The peer's own independent book agrees with P03.
    const p02::LpOrder* peer = L.lp->find(a.cl_ord_id.str());
    CHECK(peer != nullptr);
    CHECK_EQ(peer->cum_qty, c->cum_qty);
    CHECK_EQ(peer->leaves_qty, c->leaves_qty);
    CHECK_INV(L);
}

SCENARIO("OR-R-02", "P0", "end to end: partial fill then a cancel of the remainder") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    L.lp->accept_cancel(c.cl_ord_id.str());
    L.drive();

    const p03::ChainState* chain = L.chain(a.chain_id);
    CHECK_EQ(chain->status, OrdStatus::Canceled);
    CHECK_EQ(chain->cum_qty, 40000);  // the fill is kept
    CHECK_EQ(chain->leaves_qty, 0);
    CHECK_EQ(chain->fills.size(), std::size_t{1});
    const p02::LpOrder* peer = L.lp->find(a.cl_ord_id.str());
    CHECK_EQ(peer->cum_qty, 40000);
    CHECK_EQ(peer->status, std::string("Canceled"));
    CHECK_INV(L);
}

SCENARIO("OR-R-03", "P0",
         "end to end: a fill lands under the old version while a replace is pending") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "150000", "1.08520");
    L.drive();
    // A further fill while the replace is only pending.
    L.lp->execute(a.cl_ord_id.str(), 10000, 108505);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 50000);

    L.lp->accept_replace(r.cl_ord_id.str());
    L.drive();
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 150000);
    CHECK_EQ(c->cum_qty, 50000);  // cumulative, not reset
    CHECK_EQ(c->leaves_qty, 100000);
    CHECK_EQ(c->accepted_price, 108520);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-R-04", "P0", "end to end: the order fills before the cancel is processed") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    // The LP fills first and only then answers the cancel with a rejection.
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    L.inject(L.lp->cancel_reject(c.cl_ord_id.str(), a.cl_ord_id.str(),
                                 CxlRejResponseTo::OrderCancelRequest, 0));

    const p03::ChainState* chain = L.chain(a.chain_id);
    CHECK_EQ(chain->status, OrdStatus::Filled);
    CHECK(!chain->pending_cancel.has_value());
    // The cancel request was rejected; the order was not.
    CHECK_EQ(L.intent("C1")->business, BusinessOutcome::Rejected);
    CHECK_NE(chain->status, OrdStatus::Canceled);
    CHECK_INV(L);
}

SCENARIO("OR-R-05", "P0", "end to end: a reconnect after a full write leaves one order") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    L.session->disconnect();
    L.crash();
    L.boot();
    L.session->reconnect(false);
    L.lp_disk.suppress_reports = false;
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();

    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::Reconciled);
    CHECK_INV(L);
}

SCENARIO("OR-R-06", "P0", "every crash window keeps the three release-gate properties") {
    // The named windows, each exercised with the same assertions: nothing
    // accepted is lost, no command gets two original sequences, no fill is
    // counted twice (design doc 23.2 points 1 and 2).
    const char* windows[] = {
        fp::kBeforeIntentCommit, fp::kInsideIntentCommit, fp::kAfterIntentCommit,
        fp::kAfterOutboxClaim,   fp::kDropSendOnceReply,  fp::kBeforeSendBinding,
        fp::kAfterSendBinding,   fp::kAfterWireWrite,     fp::kAfterInboxCommit,
        fp::kAfterNormalize,     fp::kInsideApplyTxn,     fp::kAfterApplyCommit,
        fp::kBeforeRiskEffect,   fp::kCommitAckLost,
    };
    for (const char* window : windows) {
        Lab L;
        L.fp.arm(window, FaultKind::Crash);
        L.crash_during([&] {
            auto r = L.submit(L.new_order("K1"));
            if (r.is_err()) return;
            L.rt->dispatch_pending();
            L.lp->execute(r.value().cl_ord_id.str(), 40000, 108500);
            L.rt->pump_inbound();
        });
        L.fp.clear();

        auto again = L.submit(L.new_order("K1"));
        if (again.is_err()) {
            CHECK_MSG(L.disk.intents.size() <= std::size_t{1}, std::string(window));
            continue;
        }
        L.drive(16);
        const AcceptedIntent acc = again.value();
        L.lp->execute(acc.cl_ord_id.str(), 60000, 108510);
        L.drive(16);

        CHECK_MSG(L.disk.intents.size() == std::size_t{1}, std::string(window));
        CHECK_MSG(L.session_disk.bindings.size() <= std::size_t{1}, std::string(window));
        CHECK_MSG(L.lp_disk.book.size() <= std::size_t{1}, std::string(window));
        const p03::ChainState* c = L.chain(acc.chain_id);
        if (c != nullptr) {
            std::int64_t sum = 0;
            std::set<std::string> ids;
            for (const p03::Fill& f : c->fills) {
                sum += f.qty;
                CHECK_MSG(ids.insert(f.exec_id.str()).second, std::string(window));
            }
            CHECK_MSG(sum == c->cum_qty, std::string(window));
            CHECK_MSG(c->cum_qty <= c->accepted_qty || c->hold.has_value(), std::string(window));
        }
        const auto v = L.violations();
        CHECK_MSG(v.empty(), std::string(window) + ": " + render_violations(v));
    }
}

SCENARIO("OR-R-07", "P0", "end to end: a replayed execution on a new sequence is audited once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    L.session->reconnect(true);
    p02::ReportSpec replay = L.lp->trade(a.cl_ord_id.str(), "EX-1", 40000, 108500);
    replay.cum_qty = 40000;
    replay.leaves_qty = 60000;
    L.inject(replay);

    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK(L.disk.audit.count(AuditKind::InboundDeduplicated) >= std::size_t{1});
    CHECK(L.metrics.counter(metric::kDuplicateExec) >= 1);
    CHECK_INV(L);
}

SCENARIO("OR-R-08", "P0", "end to end: a status-only recovery with missing fills is held") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // The trade events for the rest are never sent, only the summary.
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Filled;
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    for (int i = 0; i < 6; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK_NE(L.case_for(a.chain_id)->state, ReconState::Reconciled);
    CHECK_INV(L);
}

SCENARIO("OR-R-09", "P0", "end to end: an unknown order with no status capability just holds") {
    Lab L;
    CHECK(!L.cfg.capabilities.order_status_request);
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 6; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);

    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::ManualHold);
    CHECK_EQ(L.wire_count('H'), std::size_t{0});
    CHECK_EQ(L.lp_disk.status_requests_seen, std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-R-10", "P0", "end to end: risk down during a reconnect blocks New but not a cancel") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.session->disconnect();
    L.risk->set_verdict(p05::Verdict::Unavailable);

    CHECK_EQ(L.submit(L.new_order("K2")).error(), RoutingError::RiskUnavailable);
    auto c = L.submit(L.cancel_order("C1", a.chain_id));
    CHECK(c.is_ok());
    CHECK_EQ(L.wire_count('F'), std::size_t{0});  // nothing confirmed yet
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);

    L.session->reconnect(false);
    L.drive();
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::PendingCancel);
    CHECK_INV(L);
}

SCENARIO("OR-R-11", "P0", "end to end: a store outage halts, then replay proves the state") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 20; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);
    for (const AcceptedIntent& a : orders) L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive(32);

    // The disk fills in the middle of the burst.
    L.disk.full = true;
    for (int i = 20; i < 40; ++i) CHECK(L.submit(L.new_order("K" + std::to_string(i))).is_err());
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_EQ(L.disk.intents.size(), std::size_t{20});

    // Space comes back; integrity and replay are checked before resuming.
    L.disk.full = false;
    CHECK(L.store->check_health().is_ok());
    const auto replayed = replay_chains_from_journal(L.disk);
    for (const auto& [id, live] : L.disk.chains)
        CHECK_EQ(replayed.at(id).state_hash().hex(), live.state_hash().hex());
    CHECK(L.rt->operator_resume(Lab::admin(), "disk reclaimed, replay verified").is_ok());
    CHECK(L.submit(L.new_order("K100")).is_ok());
    CHECK_INV(L);
}

SCENARIO("OR-R-12", "P0", "end to end: two router instances cannot both issue") {
    Lab L;
    L.must_submit_new("K1");
    L.must_submit_new("K2");

    Store second(L.disk, L.fp, L.clock);
    Metrics m2;
    p01::SessionEngine s2(L.session_disk, L.fp);
    p02::LpSimulator lp2(L.lp_disk, s2);
    Router rival(L.cfg, second, s2, *L.risk, m2, L.fp, L.clock);
    CHECK(rival.boot().is_ok());

    // Both are driven; only the fenced-in owner gets anywhere.
    L.rt->dispatch_pending();
    rival.dispatch_pending();
    L.rt->dispatch_pending();

    CHECK_EQ(L.rt->original_sends(), std::size_t{0});
    CHECK_EQ(rival.original_sends(), std::size_t{2});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{2});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{2});
    (void)lp2;
    CHECK_INV(L);
}

SCENARIO("OR-R-13", "P1", "a random walk of mixed commands and reports holds every invariant") {
    lab::Model::Options o;
    o.seed = 20261030;
    o.max_chains = 10;
    o.enable_status_queries = true;
    lab::Model m(o);
    m.run(5000);
    CHECK(m.applied_fills > 0);
    CHECK(m.lab.disk.audit.verify());
}

SCENARIO("OR-R-14", "P0", "forced identifier collisions never cross-credit a chain") {
    lab::Model::Options o;
    o.seed = 9091;
    o.inject_id_collisions = true;
    o.max_chains = 6;
    lab::Model m(o);
    m.run(4000);

    for (const auto& [k, ek] : m.lab.disk.exec_keys) {
        (void)k;
        CHECK(m.lab.chain(ek.chain_id) != nullptr);
        std::size_t holders = 0;
        for (const auto& [id, other] : m.lab.disk.chains) {
            (void)id;
            for (const p03::Fill& f : other.fills)
                if (f.exec_id.str() == ek.semantic_exec_id) ++holders;
        }
        CHECK(holders <= std::size_t{1});
    }
}

SCENARIO("OR-R-15", "P0", "quantities and prices at the integer boundary never wrap") {
    lab::Model::Options o;
    o.seed = 60606;
    o.inject_extreme_numbers = true;
    o.max_chains = 6;
    lab::Model m(o);
    m.run(4000);

    for (const auto& [id, c] : m.lab.disk.chains) {
        (void)id;
        CHECK(c.cum_qty >= 0);
        CHECK(c.cum_qty <= c.accepted_qty || c.hold.has_value());
        std::int64_t sum = 0;
        for (const p03::Fill& f : c.fills) sum += f.qty;
        CHECK_EQ(sum, c.cum_qty);
    }
}

SCENARIO_BLOCKED("OR-R-16", "P0", "golden frames cross-checked against an independent FIX engine",
                 "needs a third-party FIX 4.4 implementation (QuickFIX or similar) on the build "
                 "host; none is available offline here. Partial mitigation: every frame in "
                 "OR-R-01 to OR-R-05 is parsed by the P02 simulator's own field splitter and "
                 "book, which shares no code with router::fix, so a one-sided parser bug would "
                 "show up as a disagreement. That is a second implementation, not an independent "
                 "vendor, so this case stays BLOCKED rather than PASS.")

SCENARIO("OR-R-17", "P1", "mixed chaos of loss, reorder, duplication and disconnects") {
    lab::Model::Options o;
    o.seed = 5150;
    o.max_chains = 10;
    o.inject_faults = true;
    o.inject_id_collisions = true;
    lab::Model m(o);
    m.run(8000);

    for (const auto& [id, c] : m.lab.disk.chains) {
        (void)id;
        std::set<std::string> ids;
        for (const p03::Fill& f : c.fills) CHECK(ids.insert(f.exec_id.str()).second);
    }
    CHECK(m.lab.store->pending_outbox().size() <= m.lab.cfg.limits.global_command_queue);
    CHECK(m.lab.disk.audit.verify());
}

SCENARIO("OR-R-18", "P0", "a live run and a journal replay produce identical state hashes") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const auto r = L.must_submit_replace("R1", b.chain_id, "150000", "1.08490");
    L.drive();
    L.lp->accept_replace(r.cl_ord_id.str());
    L.drive();
    L.lp->execute(b.cl_ord_id.str(), 20000, 108495);
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();
    L.lp->accept_cancel(c.cl_ord_id.str());
    L.drive();

    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.size(), L.disk.chains.size());
    for (const auto& [id, live] : L.disk.chains) {
        auto it = replayed.find(id);
        CHECK(it != replayed.end());
        CHECK_EQ(it->second.state_hash().hex(), live.state_hash().hex());
    }
    CHECK_INV(L);
}

SCENARIO("OR-R-19", "P0", "load mixed with hostile input mutates nothing it should not") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 100; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(64);

    // Valid fills interleaved with wrong accounts, malformed reports and
    // cross-tenant commands.
    for (int i = 0; i < 100; ++i) {
        const AcceptedIntent& a = orders.at(static_cast<std::size_t>(i));
        L.lp->execute(a.cl_ord_id.str(), 1000, 108500);

        p02::ReportSpec hostile =
            L.lp->trade(a.cl_ord_id.str(), "BAD" + std::to_string(i), 1000, 108500);
        hostile.account = "ACC2";
        L.lp->send(hostile);

        p02::ReportSpec malformed =
            L.lp->trade(a.cl_ord_id.str(), "MAL" + std::to_string(i), 1000, 108500);
        malformed.omit.insert(32);
        L.lp->send(malformed);

        CommandEnvelope spoof = L.cancel_order("X" + std::to_string(i), a.chain_id);
        spoof.principal = Lab::trader("T2", "bob");
        spoof.account = AccountId("ACC2");
        CHECK(L.submit(spoof).is_err());
    }
    L.rt->pump_inbound(2000);

    // Exactly the valid fills landed.
    for (const AcceptedIntent& a : orders) {
        const p03::ChainState* c = L.chain(a.chain_id);
        CHECK_EQ(c->cum_qty, 1000);
        CHECK_EQ(c->fills.size(), std::size_t{1});
    }
    // Everything hostile is accounted for, bounded, and on the record.
    CHECK_EQ(L.quarantine_depth(), std::size_t{200});
    CHECK(L.metrics.series_count() < std::size_t{100});
    CHECK(L.disk.audit.verify());
    CHECK_INV(L);
}

SCENARIO("OR-R-20", "P1", "a long adversarial run from a retained seed stays explainable") {
    lab::Model::Options o;
    o.seed = 20262030;
    o.max_chains = 14;
    o.inject_faults = true;
    o.inject_id_collisions = true;
    o.inject_extreme_numbers = true;
    o.enable_status_queries = true;
    lab::Model m(o);
    m.run(12000);

    // Every unresolved thing has a reason attached to it. Nothing is simply
    // unexplained, which is the property design doc 23.2 point 7 asks for.
    for (const auto& [id, c] : m.lab.disk.chains) {
        if (!c.hold) continue;
        bool tracked = false;
        for (const auto& [k, rc] : m.lab.disk.recon_cases) {
            (void)k;
            if (rc.chain_id.str() == id) tracked = true;
        }
        for (const QuarantineEntry& q : m.lab.disk.quarantine)
            if (q.chain_hint && q.chain_hint->str() == id) tracked = true;
        CHECK(tracked);
    }
    for (const auto& [k, c] : m.lab.disk.recon_cases) {
        (void)k;
        CHECK_NE(c.cause, HoldReason::None);
    }
    CHECK(m.lab.disk.audit.verify());
}
