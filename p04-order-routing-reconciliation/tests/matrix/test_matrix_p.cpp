// Section P: load, fairness, saturation and resource exhaustion.
//
// Two honest notes about scale. First, where the matrix names a figure like
// 100k commands, the property under test is the behaviour at the bound, not the
// figure; these cases lower the configured bound and then exceed it, which
// exercises the same code path in a test that finishes. The numbers are stated
// in each case so nobody reads a passing suite as a 100k benchmark.
//
// Second, four cases in this section need something this harness does not have:
// real threads, real file descriptors, a real allocator failure, a day of wall
// clock. They are recorded as BLOCKED with the reason, because design doc 20
// says a case nobody ran is not a pass.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-P-01", "P1", "a sustained stream of new commands keeps its invariants") {
    Lab L;
    // 2,000 commands across the rate limit, driven to completion.
    std::size_t accepted = 0;
    for (int batch = 0; batch < 5; ++batch) {
        for (int i = 0; i < 400; ++i) {
            auto r = L.submit(L.new_order("K" + std::to_string(batch * 400 + i)));
            if (r.is_ok()) ++accepted;
        }
        L.drive(64);
        L.advance(1100000);
    }
    CHECK_EQ(accepted, std::size_t{2000});
    CHECK_EQ(L.rt->original_sends(), std::size_t{2000});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{2000});
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    // Latency is measured and reported rather than asserted against a target.
    const Metrics::Percentiles p = L.metrics.percentiles(metric::kCommandToIntentLatency);
    CHECK(p.count > 0);
    CHECK_INV(L);
}

SCENARIO("OR-P-02", "P1", "a stream of reports produces exactly-once effects") {
    Lab L;
    std::vector<AcceptedIntent> chains;
    for (int i = 0; i < 100; ++i) chains.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(64);

    // Ten fills per chain, a thousand reports in all.
    for (int round = 0; round < 10; ++round)
        for (const AcceptedIntent& a : chains) L.lp->execute(a.cl_ord_id.str(), 10000, 108500);
    L.rt->pump_inbound(4000);

    for (const AcceptedIntent& a : chains) {
        const p03::ChainState* c = L.chain(a.chain_id);
        CHECK_EQ(c->cum_qty, 100000);
        CHECK_EQ(c->fills.size(), std::size_t{10});
        CHECK_EQ(c->status, OrdStatus::Filled);
    }
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    const Metrics::Percentiles p = L.metrics.percentiles(metric::kInboundCommitLatency);
    CHECK(p.count > 0);
    CHECK_INV(L);
}

SCENARIO("OR-P-03", "P0", "a burst past the queue bound is refused, never silently dropped") {
    Lab L;
    // The documented default is 65,536; the bound is lowered to 200 so the same
    // overflow path runs in a test that finishes.
    L.cfg.limits.global_command_queue = 200;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    L.session->disconnect();  // nothing drains, so the queue fills

    std::size_t ok = 0, overloaded = 0;
    for (int i = 0; i < 1000; ++i) {
        auto r = L.submit(L.new_order("K" + std::to_string(i)));
        if (r.is_ok())
            ++ok;
        else if (r.error() == RoutingError::Overloaded)
            ++overloaded;
    }
    CHECK_EQ(ok, std::size_t{200});
    CHECK_EQ(overloaded, std::size_t{800});
    // Everything accepted is durable; nothing accepted was thrown away.
    CHECK_EQ(L.disk.intents.size(), std::size_t{200});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{200});
    CHECK_INV(L);

    L.session->reconnect(false);
    L.drive(64);
    CHECK_EQ(L.rt->original_sends(), std::size_t{200});
    CHECK_INV(L);
}

SCENARIO("OR-P-04", "P0", "at the bound the refusal is explicit and the durable work survives") {
    Lab L;
    L.cfg.limits.global_command_queue = 50;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    L.session->disconnect();
    for (int i = 0; i < 50; ++i) CHECK(L.submit(L.new_order("K" + std::to_string(i))).is_ok());

    auto over = L.submit(L.new_order("K-over"));
    CHECK(over.is_err());
    CHECK_EQ(over.error(), RoutingError::Overloaded);
    CHECK(L.intent("K-over") == nullptr);
    CHECK_EQ(L.disk.intents.size(), std::size_t{50});
    CHECK_INV(L);
}

SCENARIO("OR-P-05", "P0", "a per-chain flood is bounded while the chain stays correct") {
    Lab L;
    L.cfg.limits.per_chain_mailbox = 8;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // Many events for one chain. The per-chain bound applies to undispatched
    // commands; the inbound side is drained in order and stays correct.
    for (int i = 0; i < 50; ++i) {
        p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX" + std::to_string(i), 1000, 108500);
        s.cum_qty = 1000 * (i + 1);
        s.leaves_qty = 100000 - s.cum_qty;
        s.ord_status = OrdStatus::PartiallyFilled;
        L.lp->send(s);
    }
    L.rt->pump_inbound(200);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{50});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 50000);
    CHECK_INV(L);
}

SCENARIO("OR-P-06", "P0", "cancels are not starved by a flood of new orders") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 200; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(64);

    L.session->disconnect();
    for (int i = 200; i < 500; ++i) L.must_submit_new("K" + std::to_string(i));
    for (int i = 0; i < 10; ++i)
        L.must_submit_cancel("C" + std::to_string(i),
                             orders.at(static_cast<std::size_t>(i)).chain_id);

    // With the session back, the first ten application frames out are the
    // cancels, even though three hundred new orders were queued first.
    const std::size_t before = L.session_disk.wire_log.size();
    L.session->reconnect(false);
    L.rt->dispatch_pending(10);
    std::size_t cancels = 0;
    for (std::size_t i = before; i < L.session_disk.wire_log.size(); ++i)
        if (L.session_disk.wire_log[i].rfind("35=F\x01", 0) == 0) ++cancels;
    CHECK_EQ(cancels, std::size_t{10});
    CHECK_INV(L);
}

SCENARIO("OR-P-07", "P1", "reconciliation probes stay inside their budget under a backlog") {
    Lab L;
    L.enable_status_queries();
    L.cfg.limits.status_queries_in_flight = 5;
    L.lp_disk.suppress_reports = true;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;

    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 40; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(64);
    L.clock.advance(L.cfg.limits.ack_timeout_micros * 3);
    L.rt->on_timer();

    CHECK(L.wire_count('H') <= std::size_t{5});
    CHECK(L.rt->open_recon_cases() >= std::size_t{40});
    CHECK_INV(L);
}

SCENARIO("OR-P-08", "P0", "a store that keeps failing never produces an accepted-durable lie") {
    Lab L;
    // Every commit attempt fails, as a disk that has given up would.
    L.fp.arm(fp::kStoreIoError, FaultKind::Error, /*skip=*/0, /*times=*/-1);
    std::size_t accepted = 0;
    for (int i = 0; i < 20; ++i)
        if (L.submit(L.new_order("K" + std::to_string(i))).is_ok()) ++accepted;
    L.fp.clear();

    CHECK_EQ(accepted, std::size_t{0});
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_INV(L);
}

SCENARIO("OR-P-09", "P0", "a receipt is never acknowledged before its own commit") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    // The apply transaction fails, standing in for a reducer or store too slow
    // or too broken to finish.
    L.fp.arm(fp::kStoreIoError, FaultKind::Error, 0, 1);
    L.rt->pump_inbound();
    L.fp.clear();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.session->pending_inbound(), std::size_t{1});  // still owed to us
    L.boot();
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.session->pending_inbound(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-P-10", "P0",
         "a dispatcher that cannot send grows the outbox and acknowledges nothing") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    L.session->disconnect();
    for (int i = 0; i < 300; ++i) {
        L.must_submit_new("K" + std::to_string(i));
        L.rt->dispatch_pending();
    }
    CHECK_EQ(L.disk.outbox.size(), std::size_t{300});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});
    for (const auto& [k, in] : L.disk.intents) {
        (void)k;
        CHECK_EQ(in.business, BusinessOutcome::Unknown);
        CHECK_NE(in.delivery, DeliveryState::LpAcked);
    }
    CHECK(L.disk.outbox.size() <= L.cfg.limits.global_command_queue);
    CHECK_INV(L);
}

SCENARIO("OR-P-11", "P0", "a slow peer produces a case, never a second order") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 30; ++i) L.tick(1000000);  // thirty seconds of silence
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);

    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_INV(L);
}

SCENARIO_BLOCKED("OR-P-12", "P1", "behaviour under CPU starvation",
                 "needs a real scheduler and a contended core; this harness is a single "
                 "deterministic thread with a virtual clock, so there is no CPU contention to "
                 "observe. Belongs to the soak profile on real hardware.")

SCENARIO("OR-P-13", "P0", "a failure inside the commit leaves no partial route") {
    Lab L;
    L.fp.arm(fp::kStoreIoError, FaultKind::Error, 0, 1);
    auto r = L.submit(L.new_order("K1"));
    L.fp.clear();
    CHECK(r.is_err());
    // Not one of the five tables moved.
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{0});
    CHECK_EQ(L.disk.chains.size(), std::size_t{0});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{0});
    CHECK_EQ(L.disk.journal.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO_BLOCKED("OR-P-14", "P0", "critical paths remain available when the thread pool is full",
                 "needs a real thread pool to exhaust. The engine here is a single owner per "
                 "chain driven by explicit steps, so there is no pool. The related property that "
                 "can be tested, a cancel never starved by new orders, is OR-P-06.")

SCENARIO_BLOCKED("OR-P-15", "P0", "admission fails closed on file descriptor exhaustion",
                 "needs real sockets and files; P01 owns both in this design and the lab peer is "
                 "in process. Belongs to P01's suite, not provable here.")

SCENARIO("OR-P-16", "P1", "rising latency bounds the unknown rate and claims no rejections") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 20; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);

    // 1ms, then 500ms, then 5s of delay before anything comes back.
    for (std::uint64_t delay : {std::uint64_t{1000}, std::uint64_t{500000},
                                std::uint64_t{5000000}}) {
        L.advance(delay);
        L.rt->on_timer();
    }
    CHECK(L.rt->open_unknown_outcomes() <= orders.size());
    for (const AcceptedIntent& a : orders)
        CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Rejected);

    L.lp_disk.suppress_reports = false;
    for (const AcceptedIntent& a : orders) L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound(100);
    for (const AcceptedIntent& a : orders) CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.wire_count('D'), orders.size());
    CHECK_INV(L);
}

SCENARIO_BLOCKED("OR-P-17", "P1", "a twenty-four hour soak shows no resource growth",
                 "needs a day of wall clock and RSS and FD sampling from the operating system. "
                 "The scaled deterministic equivalent runs as LOAD-01; the full soak belongs to "
                 "the weekly gate in design doc 22.4.")

SCENARIO("OR-P-18", "P1", "a hot chain and cold chains are both served") {
    Lab L;
    std::vector<AcceptedIntent> chains;
    for (int i = 0; i < 50; ++i) chains.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(64);

    const AcceptedIntent& hot = chains.at(0);
    for (int i = 0; i < 300; ++i) {
        p02::ReportSpec s =
            L.lp->trade(hot.cl_ord_id.str(), "HOT" + std::to_string(i), 100, 108500);
        s.cum_qty = 100 * (i + 1);
        s.leaves_qty = 100000 - s.cum_qty;
        s.ord_status = OrdStatus::PartiallyFilled;
        L.lp->send(s);
    }
    for (std::size_t i = 1; i < chains.size(); ++i)
        L.lp->send(
            L.lp->trade(chains[i].cl_ord_id.str(), "COLD" + std::to_string(i), 1000, 108500));
    L.rt->pump_inbound(2000);

    CHECK_EQ(L.chain(hot.chain_id)->fills.size(), std::size_t{300});
    for (std::size_t i = 1; i < chains.size(); ++i)
        CHECK_EQ(L.chain(chains[i].chain_id)->cum_qty, 1000);
    CHECK_INV(L);
}

SCENARIO("OR-P-19", "P0", "past the quarantine quota the drain halts and loses no evidence") {
    Lab L;
    // The documented default is 10,000 entries; lowered to 20 here so the same
    // halt path runs quickly.
    L.cfg.limits.orphan_quota = 20;
    const auto a = L.must_submit_new("K1");
    L.drive();

    for (int i = 0; i < 40; ++i) {
        p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX" + std::to_string(i), 1000, 108500);
        s.cl_ord_id = "O-unknown-" + std::to_string(i);
        s.orig_cl_ord_id.clear();
        L.lp->send(s);
    }
    L.rt->pump_inbound(100);

    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK(L.quarantine_depth() <= L.cfg.limits.orphan_quota);
    // The events that were not processed are still owed to us: none vanished.
    CHECK(L.session->pending_inbound() > 0);
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("OR-P-20", "P1", "a large backlog drains without changing route or duplicating") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    L.session->disconnect();
    for (int i = 0; i < 500; ++i) L.must_submit_new("K" + std::to_string(i));
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    L.session->reconnect(false);
    L.drive(128);

    CHECK_EQ(L.rt->original_sends(), std::size_t{500});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{500});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{500});
    for (const auto& [k, in] : L.disk.intents) {
        (void)k;
        CHECK_EQ(in.lp.str(), std::string("LP_SIM_01"));
    }
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_INV(L);
}
