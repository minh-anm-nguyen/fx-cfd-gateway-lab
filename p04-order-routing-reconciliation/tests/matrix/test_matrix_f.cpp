// Section F: session readiness, resend and reconnect, lifecycle.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-F-01", "P0", "an ACTIVE session lets eligible commands out") {
    Lab L;
    L.must_submit_new("K1");
    CHECK(L.session->ready_for_app());
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-02", "P0", "while the transport is still connecting nothing is sent") {
    Lab L;
    L.session->set_state(p01::SessionState::Connecting);
    L.must_submit_new("K1");
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});  // bounded and durable
    CHECK_INV(L);
}

SCENARIO("OR-F-03", "P0", "a resend gap in progress blocks application dispatch") {
    Lab L;
    L.must_submit_new("K1");
    L.session->set_state(p01::SessionState::Synchronizing);
    L.rt->dispatch_pending();
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});
    CHECK_INV(L);

    L.session->set_state(p01::SessionState::Active);
    L.drive();
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-04", "P0", "a heartbeat timeout is a transport fact, not an LP rejection") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;
    L.drive();
    L.session->disconnect();

    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK_NE(L.intent("K1")->business, BusinessOutcome::Rejected);
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Unknown);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Rejected);
    CHECK_INV(L);
}

SCENARIO("OR-F-05", "P0", "a FIN before the write preserves the outbox and invents nothing") {
    Lab L;
    L.must_submit_new("K1");
    L.session->disconnect();
    L.rt->dispatch_pending();
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});

    L.session->reconnect(false);
    L.drive();
    // One order, one binding, one ClOrdID.
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-06", "P0", "an RST after the frame landed does not duplicate the order") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;  // the acknowledgement never comes back
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    L.session->disconnect();
    L.crash();
    L.boot();
    L.session->reconnect(false);

    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.drive();
    // The LP still has exactly one order and the router made no second one.
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-F-07", "P0", "a thousand reconnect flaps leave one sequence per command") {
    Lab L;
    L.must_submit_new("K1");
    L.must_submit_new("K2");
    for (int i = 0; i < 1000; ++i) {
        L.session->disconnect();
        L.rt->dispatch_pending();
        L.session->reconnect(false);
        L.rt->dispatch_pending();
        L.advance(1000);
    }
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{2});
    CHECK_EQ(L.wire_count('D'), std::size_t{2});
    CHECK_EQ(L.rt->original_sends(), std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-F-08", "P0", "reports are applied strictly in the order P01 delivered them") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // Two fills, delivered in session order.
    L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    p02::ReportSpec second = L.lp->trade(a.cl_ord_id.str(), "EX2", 60000, 108510);
    second.cum_qty = 100000;
    second.leaves_qty = 0;
    second.ord_status = OrdStatus::Filled;
    L.lp->send(second);
    L.rt->pump_inbound();

    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_EQ(c->fills.at(0).exec_id.str(), std::string("EX1"));
    CHECK_EQ(c->fills.at(1).exec_id.str(), std::string("EX2"));
    // The journal records the same order, by receipt.
    std::vector<DurableReceiptId> applied;
    for (const JournalRecord& rec : L.disk.journal)
        if (rec.kind == JournalRecord::Kind::ReportApplied && rec.report)
            applied.push_back(rec.report->source_receipt);
    for (std::size_t i = 1; i < applied.size(); ++i) CHECK(applied[i] > applied[i - 1]);
    CHECK_INV(L);
}

SCENARIO("OR-F-09", "P0", "a session level retransmission with PossDup is a business no-op") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);

    // The same execution replayed by the session layer.
    p02::ReportSpec dup = L.lp->trade(a.cl_ord_id.str(), "EX-1", 40000, 108500);
    dup.cum_qty = 40000;
    dup.leaves_qty = 60000;
    dup.poss_dup = true;
    L.inject(dup);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK(L.disk.audit.count(AuditKind::InboundDeduplicated) >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-10", "P0", "the same execution on a fresh sequence number still dedups") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // No PossDup flag at all, and a new MsgSeqNum: dedup is by business identity.
    p02::ReportSpec dup = L.lp->trade(a.cl_ord_id.str(), "EX-1", 40000, 108500);
    dup.cum_qty = 40000;
    dup.leaves_qty = 60000;
    L.inject(dup);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-11", "P0", "PossResend is validated by identity, not treated as a new order") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec resend = L.lp->trade(a.cl_ord_id.str(), "EX-1", 40000, 108500);
    resend.cum_qty = 40000;
    resend.leaves_qty = 60000;
    resend.poss_resend = true;
    L.inject(resend);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.disk.chains.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-12", "P0", "a daily sequence reset does not clear the business ledger") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();

    L.session->reconnect(true);
    L.crash();
    L.boot();
    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());
    CHECK(!L.disk.exec_keys.empty());
    CHECK_INV(L);
}

SCENARIO("OR-F-13", "P0", "a reset that wiped the resend history blocks dispatch") {
    Lab L;
    L.session_disk.reset_on_logon = true;
    L.must_submit_new("K1");
    L.session->reconnect(true);  // the reset destroys the history
    CHECK(L.session_disk.resend_history_lost);

    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("OR-F-14", "P0", "a changed SenderCompID under an existing outbox is held") {
    Lab L;
    L.must_submit_new("K1");
    L.session_disk.identity.sender_comp_id = "ROUTER99";
    // The route no longer describes the attached session.
    auto r = L.submit(L.new_order("K2"));
    CHECK(r.is_err());
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-F-15", "P0", "two engines on one session cannot both write") {
    Lab L;
    L.must_submit_new("K1");
    const Hash payload = L.intent("K1")->payload_hash;
    const std::string body = L.intent("K1")->fix_body;

    // A second engine attaches to the same session image.
    p01::SessionEngine rival(L.session_disk, L.fp);
    CHECK(!L.session->owns_session());
    CHECK(rival.owns_session());

    const auto stale = L.session->send_once(CommandId("K1"), payload, body);
    CHECK_EQ(stale.state, p01::StableSendResult::State::Fatal);
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{1});

    const auto fresh = rival.send_once(CommandId("K1"), payload, body);
    CHECK_EQ(fresh.state, p01::StableSendResult::State::AssignedOriginalSeq);
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-F-16", "P0", "an application frame outside the session never becomes an event") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.session->set_state(p01::SessionState::Down);

    const DurableReceiptId id =
        L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    CHECK_EQ(id, DurableReceiptId{0});
    CHECK_EQ(L.session_disk.refused_inbound.size(), std::size_t{1});
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_INV(L);
}

SCENARIO("OR-F-17", "P0", "a report from the previous epoch still correlates by chain") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.session->reconnect(true);  // epoch moves on

    // The delayed execution arrives after the rollover.
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    // Its provenance records the epoch it actually arrived in.
    bool seen = false;
    for (const JournalRecord& rec : L.disk.journal) {
        if (rec.kind != JournalRecord::Kind::ReportApplied || !rec.report) continue;
        if (rec.report->exec_type == ExecType::Trade) {
            CHECK_EQ(rec.report->epoch, L.session_disk.epoch);
            seen = true;
        }
    }
    CHECK(seen);
    CHECK_INV(L);
}

SCENARIO("OR-F-18", "P1", "administrative traffic does not consume router resources") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // Session level messages are refused by the application parser, so a flood
    // of them cannot grow the chain projection or the dedup ledger.
    const std::size_t chains = L.disk.chains.size();
    const std::size_t keys = L.disk.exec_keys.size();
    for (int i = 0; i < 2000; ++i) {
        p02::ReportSpec s;
        s.raw[35] = "0";  // Heartbeat
        s.cl_ord_id = a.cl_ord_id.str();
        L.lp->send(s);
    }
    L.rt->pump_inbound(4000);
    CHECK_EQ(L.disk.chains.size(), chains);
    CHECK_EQ(L.disk.exec_keys.size(), keys);
    // Every one of them is accounted for, none silently dropped.
    CHECK_EQ(L.quarantine_depth(), std::size_t{2000});
    CHECK_INV(L);
}

SCENARIO("OR-F-19", "P1", "a ten second round trip creates a case, not a failure claim") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;
    L.drive();
    L.tick(10000000);  // ten seconds

    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Unknown);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Rejected);

    // When the report finally arrives it is applied and the case closes.
    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::Reconciled);
    CHECK_INV(L);
}

SCENARIO("OR-F-20", "P1", "a long outage keeps every durable intent and bounds the queue") {
    Lab L;
    L.session->set_state(p01::SessionState::Down);
    for (int i = 0; i < 500; ++i) {
        auto r = L.submit(L.new_order("K" + std::to_string(i)));
        CHECK(r.is_ok());
        L.advance(1000);
        L.rt->dispatch_pending();
    }
    CHECK_EQ(L.disk.intents.size(), std::size_t{500});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK(L.disk.outbox.size() <= L.cfg.limits.global_command_queue);
    CHECK_INV(L);

    // The queue drains without duplicating anything once the session is back.
    L.session->set_state(p01::SessionState::Active);
    L.drive(40);
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{500});
    CHECK_EQ(L.rt->original_sends(), std::size_t{500});
    CHECK_INV(L);
}
