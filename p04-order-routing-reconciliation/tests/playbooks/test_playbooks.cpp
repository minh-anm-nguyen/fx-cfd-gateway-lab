// The P0 fault-injection playbooks from design doc 21.
//
// Each one follows the shape the document uses: a fixed fixture (21.0 -- ACC1,
// EUR/USD, tick 0.00001, qty step 1000, LP SIM01, risk APPROVE, 35=H off, order
// O1 BUY 100,000 at 1.08500, fill EX1 of 40,000), a named trigger, the PASS
// conditions, and the evidence that has to survive. The evidence is asserted
// rather than described: digests, counts and audit records.
#include <set>

#include "support/model.hpp"

using namespace router;
using lab::Lab;

namespace {

// The standard fixture: an order that exists locally, optionally already sent.
struct Fixture {
    Lab lab;
    AcceptedIntent o1;
    explicit Fixture(bool send = false) {
        o1 = lab.must_submit_new("K1", "100000", "1.08500");
        if (send) lab.drive();
    }
};

}  // namespace

SCENARIO("PB-01", "P0", "CW-05: P01 bound the sequence and the reply was lost") {
    Fixture f;
    f.lab.fp.arm(fp::kDropSendOnceReply, FaultKind::Crash);
    CHECK(f.lab.crash_during([&] { f.lab.rt->dispatch_pending(); }));
    f.lab.fp.clear();

    // Evidence: the binding before and after, the payload digest, and the
    // command left explicitly unresolved.
    const auto before = f.lab.session->lookup_binding(CommandId("K1"));
    CHECK(before.has_value());
    const MsgSeqNum n = before->original_seq;
    const Hash h1 = f.lab.intent("K1")->payload_hash;

    f.lab.drive();
    const auto after = f.lab.session->lookup_binding(CommandId("K1"));
    CHECK_EQ(after->original_seq, n);  // same N, not N+1
    CHECK_EQ(f.lab.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(f.lab.session_disk.next_out, n + 1);
    CHECK(after->payload_hash == h1);
    CHECK_EQ(f.lab.wire_count('D'), std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("PB-02", "P0", "the LP took the order and the acknowledgement was lost") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.suppress_reports = true;
    const auto o1 = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});  // exactly one order at P02

    L.session->disconnect();
    L.crash();
    L.boot();
    L.session->reconnect(false);
    L.lp_disk.suppress_reports = false;
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();

    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});  // still one, no O2
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    const ReconCase* c = L.case_for(o1.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->state, ReconState::Reconciled);
    CHECK(!c->last_evidence_hash.is_zero());
    CHECK(!c->query_ids.empty());
    CHECK_INV(L);
}

SCENARIO("PB-03", "P0", "P01 without StableSendV1: the integration gate holds") {
    Fixture f;
    f.lab.session_disk.stable_send_supported = false;

    const DispatchStats d = f.lab.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(f.lab.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(f.lab.session_disk.bindings.size(), std::size_t{0});
    CHECK_EQ(f.lab.intent("K1")->hold.value(), HoldReason::CapabilityMissing);
    CHECK_EQ(f.lab.rt->mode(), RouterMode::Degraded);
    // The reason is on the record, in the words the release gate uses.
    bool said = false;
    for (const std::string& a : f.lab.rt->alerts())
        if (a.find("BLOCKED, not proven") != std::string::npos) said = true;
    CHECK(said);
    CHECK_INV(f.lab);
}

SCENARIO("PB-04", "P0", "a partial wire write under a crash fabricates nothing") {
    Fixture f;
    f.lab.fp.arm(fp::kPartialWireWrite, FaultKind::Error);
    f.lab.rt->dispatch_pending();
    f.lab.fp.clear();

    CHECK_EQ(f.lab.lp_disk.book.size(), std::size_t{0});
    CHECK_EQ(f.lab.lp_disk.rejected_frames.size(), std::size_t{1});
    CHECK_EQ(f.lab.intent("K1")->delivery, DeliveryState::UnknownTransport);
    f.lab.crash();
    f.lab.boot();
    f.lab.drive();
    CHECK_EQ(f.lab.disk.intents.size(), std::size_t{1});
    CHECK_EQ(f.lab.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("PB-05", "P0", "the command API is idempotent after the commit, before the ack") {
    Lab L;
    L.fp.arm(fp::kAfterIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.new_order("K1")); }));
    L.fp.clear();

    const std::string cl = L.disk.intents.begin()->second.cl_ord_id.str();
    for (int i = 0; i < 20; ++i) {
        auto r = L.submit(L.new_order("K1"));
        CHECK(r.is_ok());
        CHECK_EQ(r.value().cl_ord_id.str(), cl);
    }
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("PB-06", "P0", "EX1 replayed on a different sequence is persisted once") {
    Fixture f(true);
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    const std::size_t keys = f.lab.disk.exec_keys.size();

    f.lab.session->reconnect(true);  // new epoch, sequences restart
    p02::ReportSpec replay = f.lab.lp->trade(f.o1.cl_ord_id.str(), "EX-1", 40000, 108500);
    replay.cum_qty = 40000;
    replay.leaves_qty = 60000;
    f.lab.inject(replay);

    CHECK_EQ(f.lab.chain(f.o1.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(f.lab.disk.exec_keys.size(), keys);  // no new dedup key
    CHECK(f.lab.disk.audit.count(AuditKind::InboundDeduplicated) >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("PB-07", "P0", "the same ExecID with a different price is a conflict, held") {
    Fixture f(true);
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();

    p02::ReportSpec conflict = f.lab.lp->trade(f.o1.cl_ord_id.str(), "EX-1", 40000, 108900);
    conflict.cum_qty = 40000;
    conflict.leaves_qty = 60000;
    const std::string frame = conflict.encode();
    f.lab.inject(conflict);

    const p03::ChainState* c = f.lab.chain(f.o1.chain_id);
    CHECK_EQ(c->fills.at(0).px, 108500);  // unchanged
    CHECK_EQ(c->hold.value(), HoldReason::ExecIdConflict);
    // Evidence: the raw digest of the contradicting frame is retained.
    bool kept = false;
    for (const QuarantineEntry& q : f.lab.disk.quarantine)
        if (q.raw_hash == sha256(frame)) kept = true;
    CHECK(kept);
    CHECK(f.lab.metrics.counter(metric::kExecIdConflicts) >= 1);
    CHECK_INV(f.lab);
}

SCENARIO("PB-08", "P0", "pending cancel, a fill, then the cancel confirmed") {
    Fixture f(true);
    const auto c1 = f.lab.must_submit_cancel("C1", f.o1.chain_id);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->status, OrdStatus::PendingCancel);

    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->status, OrdStatus::PendingCancel);

    f.lab.lp->accept_cancel(c1.cl_ord_id.str());
    f.lab.drive();
    const p03::ChainState* c = f.lab.chain(f.o1.chain_id);
    CHECK_EQ(c->status, OrdStatus::Canceled);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK(c->cancel_proven);
    CHECK_INV(f.lab);
}

SCENARIO("PB-09", "P0", "a cancel that arrives too late: the order is Filled") {
    Fixture f(true);
    const auto c1 = f.lab.must_submit_cancel("C1", f.o1.chain_id);
    f.lab.drive();
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 100000, 108500);
    f.lab.drive();
    f.lab.inject(f.lab.lp->cancel_reject(c1.cl_ord_id.str(), f.o1.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelRequest, 0));

    const p03::ChainState* c = f.lab.chain(f.o1.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK(!c->cancel_proven);
    CHECK(!c->pending_cancel.has_value());
    CHECK_EQ(f.lab.intent("C1")->business, BusinessOutcome::Rejected);
    CHECK_INV(f.lab);
}

SCENARIO("PB-10", "P0", "pending replace, a fill on the old version, then the new one accepted") {
    Fixture f(true);
    const auto r1 = f.lab.must_submit_replace("R1", f.o1.chain_id, "150000", "1.08500");
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->accepted_cl_ord_id.str(), f.o1.cl_ord_id.str());

    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);

    f.lab.lp->accept_replace(r1.cl_ord_id.str());
    f.lab.drive();
    const p03::ChainState* c = f.lab.chain(f.o1.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), r1.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 150000);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->leaves_qty, 110000);

    // A further fill now names the new accepted version.
    f.lab.lp->execute(r1.cl_ord_id.str(), 10000, 108510);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 50000);
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->fills.size(), std::size_t{2});
    CHECK_INV(f.lab);
}

SCENARIO("PB-11", "P0", "a rejected replace keeps the original accepted version") {
    Fixture f(true);
    const auto r1 = f.lab.must_submit_replace("R1", f.o1.chain_id, "80000", "1.08490");
    f.lab.drive();
    f.lab.inject(f.lab.lp->cancel_reject(r1.cl_ord_id.str(), f.o1.cl_ord_id.str(),
                                         CxlRejResponseTo::OrderCancelReplaceRequest, 2));

    const p03::ChainState* c = f.lab.chain(f.o1.chain_id);
    CHECK_EQ(c->accepted_cl_ord_id.str(), f.o1.cl_ord_id.str());
    CHECK_EQ(c->accepted_qty, 100000);
    CHECK_EQ(c->accepted_price, 108500);
    CHECK(!c->pending_replace.has_value());
    CHECK_EQ(f.lab.intent("R1")->business, BusinessOutcome::Rejected);
    CHECK_INV(f.lab);
}

SCENARIO("PB-12", "P0", "the peer says Filled and our fill history does not agree") {
    Lab L;
    L.enable_status_queries();
    const auto o1 = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(o1.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(o1.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Filled;
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    L.inject(s);

    const p03::ChainState* c = L.chain(o1.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->hold.value(), HoldReason::HistoryRequired);
    CHECK_EQ(L.disk.exec_keys.size(), std::size_t{2});  // the ack and the one fill
    // Evidence: the escalation bundle states the mismatch.
    const ReconCase* rc = L.case_for(o1.chain_id);
    CHECK(rc != nullptr);
    const std::string bundle = L.rt->escalation_bundle(Lab::operator_principal(), rc->case_id);
    CHECK(bundle.find("\"cum_qty\": 40000") != std::string::npos);
    CHECK(bundle.find("HistoryRequired") != std::string::npos);
    CHECK_INV(L);
}

SCENARIO("PB-13", "P0", "no status capability plus an ambiguous New: manual hold, zero H") {
    Lab L;
    CHECK(!L.cfg.capabilities.order_status_request);
    // The peer stays silent, so nothing ever resolves the ambiguity by
    // evidence. (When a real acknowledgement does arrive it closes the case,
    // which is OR-M-11; this playbook is about the case where none does.)
    L.lp_disk.suppress_reports = true;
    const auto o1 = L.must_submit_new("K1");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    for (int i = 0; i < 6; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCase* c = L.case_for(o1.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->state, ReconState::ManualHold);
    CHECK_EQ(L.wire_count('H'), std::size_t{0});
    CHECK_EQ(L.lp_disk.status_requests_seen, std::size_t{0});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK(L.disk.audit.count(AuditKind::SendAmbiguous) >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("PB-14", "P0", "repeated status snapshots with ExecID 0, including a changed one") {
    Lab L;
    L.enable_status_queries();
    const auto o1 = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(o1.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // The same sentinel, five times, agreeing.
    p02::ReportSpec agree = L.lp->status_snapshot(o1.cl_ord_id.str(), std::nullopt);
    agree.cum_qty = 40000;
    agree.leaves_qty = 60000;
    agree.ord_status = OrdStatus::PartiallyFilled;
    for (int i = 0; i < 5; ++i) L.inject(agree);
    CHECK_EQ(L.chain(o1.chain_id)->fills.size(), std::size_t{1});
    CHECK(!L.chain(o1.chain_id)->hold.has_value());
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|0"), std::size_t{0});

    // Then the same sentinel with different content: evaluated, not deduped.
    p02::ReportSpec changed = agree;
    changed.cum_qty = 100000;
    changed.leaves_qty = 0;
    changed.ord_status = OrdStatus::Filled;
    L.inject(changed);
    CHECK_EQ(L.chain(o1.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(o1.chain_id)->hold.value(), HoldReason::HistoryRequired);
    CHECK_INV(L);
}

SCENARIO("PB-15", "P0", "an orphan and a cross-account report are both quarantined") {
    Fixture f(true);
    // Cross account.
    p02::ReportSpec cross = f.lab.lp->trade(f.o1.cl_ord_id.str(), "EXX", 40000, 108500);
    cross.account = "ACC2";
    f.lab.inject(cross);
    // Orphan.
    p02::ReportSpec orphan = f.lab.lp->trade(f.o1.cl_ord_id.str(), "EXY", 40000, 108500);
    orphan.cl_ord_id = "O-999";
    orphan.orig_cl_ord_id.clear();
    f.lab.inject(orphan);

    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.quarantine_depth(), std::size_t{2});
    std::set<std::string> reasons;
    for (const QuarantineEntry& q : f.lab.disk.quarantine) {
        reasons.insert(std::string(to_string(q.reason)));
        CHECK(!q.raw_hash.is_zero());
        CHECK(q.receipt_id != 0);
    }
    CHECK(reasons.count("SecurityQuarantine") == std::size_t{1});
    CHECK(reasons.count("OrphanReport") == std::size_t{1});
    CHECK(!f.lab.rt->alerts().empty());
    CHECK_INV(f.lab);
}

SCENARIO("PB-16", "P0", "a crash inside the apply transaction commits state and dedup together") {
    Fixture f(true);
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);

    f.lab.fp.arm(fp::kInsideApplyTxn, FaultKind::Crash);
    CHECK(f.lab.crash_during([&] { f.lab.rt->pump_inbound(); }));
    f.lab.fp.clear();
    // Neither side of the pair exists.
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.disk.exec_keys.count("LP_SIM_01|EX-1"), std::size_t{0});

    f.lab.rt->pump_inbound();
    // Now both do, exactly once.
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.disk.exec_keys.count("LP_SIM_01|EX-1"), std::size_t{1});
    f.lab.rt->pump_inbound();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("PB-17", "P0", "P01 committed the receipt and the P04 worker died first") {
    Fixture f(true);
    f.lab.fp.arm(fp::kAfterInboxCommit, FaultKind::Crash);
    CHECK(f.lab.crash_during([&] { f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500); }));
    f.lab.fp.clear();

    CHECK_EQ(f.lab.session->pending_inbound(), std::size_t{1});
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 0);
    f.lab.rt->pump_inbound();
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.session->pending_inbound(), std::size_t{0});
    CHECK_INV(f.lab);
}

SCENARIO("PB-18", "P0", "the disk fills during a burst of commands") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    for (int i = 0; i < 10; ++i) L.must_submit_new("K" + std::to_string(i));
    L.drive(32);
    const std::size_t good = L.disk.intents.size();

    L.disk.full = true;
    std::size_t refused = 0;
    for (int i = 10; i < 200; ++i)
        if (L.submit(L.new_order("K" + std::to_string(i))).is_err()) ++refused;

    CHECK_EQ(refused, std::size_t{190});
    CHECK_EQ(L.disk.intents.size(), good);  // nothing half written
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK(L.disk.audit.verify());
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("PB-19", "P0", "P02 accepted the order then died before reporting") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.suppress_reports = true;
    const auto o1 = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    // The peer recovers and emits what it owed.
    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(o1.cl_ord_id.str()));
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(o1.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("PB-20", "P0", "two router instances against one store: one owner, one send") {
    Lab L;
    L.must_submit_new("K1");

    Store rival_store(L.disk, L.fp, L.clock);
    Metrics m2;
    p01::SessionEngine s2(L.session_disk, L.fp);
    p02::LpSimulator lp2(L.lp_disk, s2);
    Router rival(L.cfg, rival_store, s2, *L.risk, m2, L.fp, L.clock);
    CHECK(rival.boot().is_ok());

    L.rt->dispatch_pending();  // fenced out
    rival.dispatch_pending();  // the owner
    L.rt->dispatch_pending();

    CHECK_EQ(L.rt->original_sends(), std::size_t{0});
    CHECK_EQ(rival.original_sends(), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    (void)lp2;
    CHECK_INV(L);
}

SCENARIO("PB-21", "P0", "risk is down and the cancel is the safety-critical path") {
    Fixture f(true);
    f.lab.risk->set_verdict(p05::Verdict::Unavailable);

    auto c = f.lab.submit(f.lab.cancel_order("C1", f.o1.chain_id));
    CHECK(c.is_ok());
    auto n = f.lab.submit(f.lab.new_order("K2"));
    CHECK(n.is_err());
    CHECK_EQ(n.error(), RoutingError::RiskUnavailable);

    f.lab.drive();
    CHECK_EQ(f.lab.wire_count('F'), std::size_t{1});
    CHECK_EQ(f.lab.wire_count('D'), std::size_t{1});  // only the original
    // The cancel reached the LP and was acknowledged as a request. That is not
    // a cancellation: the chain is pending and the outcome is anything but
    // Canceled, which is the property the playbook is about.
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->status, OrdStatus::PendingCancel);
    CHECK_NE(f.lab.intent("C1")->business, BusinessOutcome::Canceled);
    CHECK(!f.lab.chain(f.o1.chain_id)->cancel_proven);
    CHECK_INV(f.lab);
}

SCENARIO("PB-22", "P0", "a large New backlog does not delay the cancels") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 100000;
    std::vector<AcceptedIntent> orders;
    for (int i = 0; i < 100; ++i) orders.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);

    L.session->disconnect();
    for (int i = 100; i < 600; ++i) L.must_submit_new("K" + std::to_string(i));
    for (int i = 0; i < 20; ++i)
        L.must_submit_cancel("C" + std::to_string(i),
                             orders.at(static_cast<std::size_t>(i)).chain_id);

    const std::size_t before = L.session_disk.wire_log.size();
    L.session->reconnect(false);
    L.rt->dispatch_pending(20);
    std::size_t cancels = 0;
    for (std::size_t i = before; i < L.session_disk.wire_log.size(); ++i)
        if (L.session_disk.wire_log[i].rfind("35=F\x01", 0) == 0) ++cancels;
    CHECK_EQ(cancels, std::size_t{20});
    CHECK_INV(L);
}

SCENARIO("PB-23", "P0", "a clock jump during a timeout does not change the schedule") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto o1 = L.must_submit_new("K1");
    L.drive();

    // The wall clock jumps forward an hour and then back two hours. The
    // monotonic clock, which the budget is measured on, does not move.
    L.clock.skew_wall(3600000000LL);
    L.rt->on_timer();
    CHECK(L.case_for(o1.chain_id) == nullptr);
    L.clock.skew_wall(-7200000000LL);
    L.rt->on_timer();
    CHECK(L.case_for(o1.chain_id) == nullptr);

    // Only monotonic time opens the case.
    L.advance(L.cfg.limits.ack_timeout_micros * 2);
    L.rt->on_timer();
    CHECK(L.case_for(o1.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("PB-24", "P0", "a trade correction or bust outside v1 fails closed") {
    Fixture f(true);
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();

    for (ExecType t : {ExecType::TradeCorrect, ExecType::TradeCancel, ExecType::Restated}) {
        p02::ReportSpec s = f.lab.lp->trade(f.o1.cl_ord_id.str(),
                                            "EXU" + std::string(to_string(t)), 40000, 108500);
        s.exec_type = t;
        s.exec_ref_id = "EX-1";
        f.lab.inject(s);
    }

    // The ledger did not move and every one of them is on the record.
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->fills.size(), std::size_t{1});
    std::size_t unsupported = 0;
    for (const QuarantineEntry& q : f.lab.disk.quarantine)
        if (q.reason == HoldReason::UnsupportedEvent) ++unsupported;
    CHECK(unsupported >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("PB-25", "P0", "a day rollover that reuses FIX sequence numbers") {
    Fixture f(true);
    f.lab.lp->execute(f.o1.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    const MsgSeqNum first = f.lab.session->lookup_binding(CommandId("K1"))->original_seq;
    const std::size_t keys = f.lab.disk.exec_keys.size();

    f.lab.session->reconnect(true);
    CHECK_EQ(f.lab.session_disk.next_out, MsgSeqNum{1});  // numbering restarted
    const auto second = f.lab.must_submit_new("K2");
    f.lab.drive();
    const MsgSeqNum reused = f.lab.session->lookup_binding(CommandId("K2"))->original_seq;

    // The sequence number repeats, legitimately, and nothing aliases.
    CHECK_EQ(reused, first);
    CHECK_NE(second.cl_ord_id.str(), f.o1.cl_ord_id.str());
    CHECK_EQ(f.lab.disk.exec_keys.size(), keys + 1);  // only the new ack
    CHECK_EQ(f.lab.chain(f.o1.chain_id)->cum_qty, 40000);
    CHECK_INV(f.lab);
}

SCENARIO("PB-26", "P0", "a hundred thousand property steps from a retained seed") {
    lab::Model::Options o;
    o.seed = 42026;  // retained so a failure is replayable
    o.max_chains = 12;
    o.inject_faults = true;
    o.inject_id_collisions = true;
    o.enable_status_queries = true;
    // The chain, ledger and dedup invariants are checked after every one of the
    // hundred thousand steps; the command-table invariants, which scan a table
    // that only grows, run every two hundred and fiftieth step and again in
    // full at the end. See Model::Options::full_check_every for why.
    o.full_check_every = 250;
    lab::Model m(o);
    m.run(100000);

    CHECK_EQ(m.steps_run, std::size_t{100000});
    CHECK(m.crashes > 0);
    CHECK(m.applied_fills > 0);
    CHECK(m.lab.disk.audit.verify());
    // No chain ended with a duplicated execution or an unexplained hold.
    for (const auto& [id, c] : m.lab.disk.chains) {
        std::set<std::string> ids;
        for (const p03::Fill& fl : c.fills) CHECK(ids.insert(fl.exec_id.str()).second);
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
}

SCENARIO_BLOCKED("PB-27", "P0", "interop against an independently implemented FIX 4.4 peer",
                 "needs a third-party engine (QuickFIX or a vendor acceptor) reachable from the "
                 "build host; none is available offline here. The P02 simulator is a second "
                 "implementation inside this project and cross-checks every frame the end to end "
                 "cases send, which is a weaker claim and is recorded as such, not as a pass.")

SCENARIO("PB-28", "P0", "snapshot and journal replay agree, including after failures") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // A crash in the middle of the second chain's apply.
    L.lp->execute(b.cl_ord_id.str(), 50000, 108400);
    L.fp.arm(fp::kInsideApplyTxn, FaultKind::Crash);
    L.crash_during([&] { L.rt->pump_inbound(); });
    L.fp.clear();
    L.drive();

    // A contradiction that puts a chain on hold.
    p02::ReportSpec bad = L.lp->trade(a.cl_ord_id.str(), "EXBAD", 40000, 109000);
    bad.symbol = "GBP/USD";
    L.inject(bad);

    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.size(), L.disk.chains.size());
    for (const auto& [id, live] : L.disk.chains) {
        auto it = replayed.find(id);
        CHECK(it != replayed.end());
        CHECK_EQ(it->second.state_hash().hex(), live.state_hash().hex());
    }
    // And the same holds after a prune, from the snapshot plus the tail.
    CHECK(L.store->prune_journal(2).is_ok());
    const auto after_prune = replay_chains_from_journal(L.disk);
    for (const auto& [id, live] : L.disk.chains)
        CHECK_EQ(after_prune.at(id).state_hash().hex(), live.state_hash().hex());
    CHECK_INV(L);
}

SCENARIO("PB-29", "P0", "an operator override needs evidence, a role and two approvers") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto o1 = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCaseId id = L.case_for(o1.chain_id)->case_id;
    CHECK_EQ(L.store->find_recon(id)->state, ReconState::ManualHold);

    // Three ways to get it wrong, each denied and each audited.
    CHECK(L.rt->operator_resolve_case(Lab::viewer(), id, "ok", sha256("e"), {"a", "b"}).is_err());
    CHECK(L.rt->operator_resolve_case(Lab::operator_principal(), id, "", sha256("e"), {"a", "b"})
              .is_err());
    CHECK(L.rt->operator_resolve_case(Lab::operator_principal(), id, "ok", Hash{}, {"a", "b"})
              .is_err());
    CHECK_EQ(L.store->find_recon(id)->state, ReconState::ManualHold);
    CHECK(L.disk.audit.count(AuditKind::OperatorDenied) >= std::size_t{3});

    // And the right way, which leaves a complete record.
    const Hash evidence = sha256("signed p02 audit bundle 2026-10-30");
    CHECK(L.rt->operator_resolve_case(Lab::operator_principal(), id,
                                      "peer audit proves the order never existed", evidence,
                                      {"ops1", "risk2"})
              .is_ok());
    const ReconCase* c = L.store->find_recon(id);
    CHECK_EQ(c->state, ReconState::Reconciled);
    CHECK_EQ(c->resolved_by, std::string("ops1"));
    CHECK(c->last_evidence_hash == evidence);
    CHECK_EQ(c->approvals.size(), std::size_t{2});
    CHECK(L.disk.audit.verify());
    // No fill was conjured by the resolution.
    CHECK_EQ(L.chain(o1.chain_id)->fills.size(), std::size_t{0});
    CHECK_EQ(L.chain(o1.chain_id)->cum_qty, 0);
    CHECK_INV(L);
}
