// Section E: outbound intent, the outbox, and the original-send-once property.
//
// These are the integration-gated cases of design doc 20: the property only
// holds if P01 actually implements StableSendV1, which is why OR-E-05 and
// friends assert against the binding table rather than against a hopeful log
// line. PB-03 covers the build where the contract is absent.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-E-01", "P0", "the outbox row is durable before anything reaches the wire") {
    Lab L;
    L.must_submit_new("K1");
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_EQ(L.disk.outbox.begin()->second.state, OutboxState::Pending);
    CHECK(!L.disk.outbox.begin()->second.payload_hash.is_zero());
    // Nothing has been sent and no sequence has been consumed.
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-E-02", "P0", "a crash between commit and the client answer costs nothing") {
    Lab L;
    L.fp.arm(fp::kAfterIntentCommit, FaultKind::Crash);
    const bool died = L.crash_during([&] { L.submit(L.new_order("K1")); });
    CHECK(died);
    L.fp.clear();

    // The client retries and gets the same record back.
    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK(again.value().idempotent_replay);
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-03", "P0", "a crash after the outbox claim resumes the same command") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kAfterOutboxClaim, FaultKind::Crash);
    const bool died = L.crash_during([&] { L.rt->dispatch_pending(); });
    CHECK(died);
    L.fp.clear();
    // Nothing was sent, and the row is still work to do.
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    L.drive();
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-04", "P0", "a second dispatcher pass does not issue the command twice") {
    Lab L;
    L.must_submit_new("K1");
    L.rt->dispatch_pending();
    L.rt->dispatch_pending();
    L.rt->dispatch_pending();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-05", "P0", "a lost send_once reply returns the same sequence, not the next one") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    const auto binding = L.session->lookup_binding(CommandId("K1"));
    CHECK(binding.has_value());
    const MsgSeqNum first = binding->original_seq;
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::UnknownTransport);

    // Retrying the same command returns N, and next_out has not moved past it.
    L.rt->dispatch_pending();
    const auto again = L.session->lookup_binding(CommandId("K1"));
    CHECK_EQ(again->original_seq, first);
    CHECK_EQ(L.session_disk.next_out, first + 1);
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-06", "P0", "a thousand send_once calls with one payload give one sequence") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    const Hash payload = L.intent("K1")->payload_hash;
    const std::string body = L.intent("K1")->fix_body;
    for (int i = 0; i < 1000; ++i) {
        const auto r = L.session->send_once(CommandId("K1"), payload, body);
        CHECK(r.state == p01::StableSendResult::State::AssignedOriginalSeq ||
              r.state == p01::StableSendResult::State::AlreadyAssigned);
        CHECK_EQ(r.original_seq.value(), MsgSeqNum{1});
    }
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{2});
    // The bytes are identical every time.
    for (const std::string& f : L.session_disk.wire_log) CHECK_EQ(f, body);
    (void)a;
    CHECK_INV(L);
}

SCENARIO("OR-E-07", "P0", "the same command id with different bytes is a fatal conflict") {
    Lab L;
    L.must_submit_new("K1");
    const Hash payload = L.intent("K1")->payload_hash;
    const std::string body = L.intent("K1")->fix_body;
    L.drive();
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{2});

    Hash other = payload;
    other.bytes[0] ^= 0xff;
    const auto r = L.session->send_once(CommandId("K1"), other, body + "\x01" "99=1\x01");
    CHECK_EQ(r.state, p01::StableSendResult::State::Fatal);
    CHECK(!r.original_seq.has_value());
    // No new sequence was consumed.
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{2});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-08", "P0", "a half written frame creates no new business intent") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.fp.arm(fp::kPartialWireWrite, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    // The peer could not parse it, so it has no order.
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{0});
    CHECK(!L.lp_disk.rejected_frames.empty());
    // The router knows only that the outcome is unknown.
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::UnknownTransport);
    CHECK(L.case_for(a.chain_id) != nullptr);
    // One intent, one binding, and no second D.
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-09", "P0", "a successful write with a lost reply does not mint a new ClOrdID") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    // The LP did receive it.
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::UnknownTransport);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_EQ(L.case_for(a.chain_id)->cause, HoldReason::AmbiguousSend);

    // Driving the router further must not produce a second order.
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-10", "P0", "while the session is synchronising no sequence is reserved") {
    Lab L;
    L.must_submit_new("K1");
    L.session->set_state(p01::SessionState::Synchronizing);
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{1});
    // The backlog is still durable.
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_INV(L);

    L.session->set_state(p01::SessionState::Active);
    L.drive();
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-11", "P0", "a payload that changed after commit is held, not sent") {
    Lab L;
    L.must_submit_new("K1", "100000", "1.08500");
    // Someone rewrites the committed intent, as a config drift or a bug would.
    CommandIntent tampered = *L.intent("K1");
    tampered.fix_body = tampered.fix_body + "\x01";
    L.disk.intents[tampered.key()] = tampered;

    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.intent("K1")->hold.value(), HoldReason::StoreFailClosed);
    CHECK_INV(L);
}

SCENARIO("OR-E-12", "P0", "a worker that dies after send_once does not issue twice") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kAfterWireWrite, FaultKind::Crash);
    const bool died = L.crash_during([&] { L.rt->dispatch_pending(); });
    CHECK(died);
    L.fp.clear();

    // The binding is in P01's store, so recovery finds it instead of allocating.
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    const MsgSeqNum seq = L.session->lookup_binding(CommandId("K1"))->original_seq;
    L.drive();
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.session->lookup_binding(CommandId("K1"))->original_seq, seq);
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-13", "P0", "a corrupt resend store cannot claim a successful issue-once") {
    Lab L;
    L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});

    // Damage P01's own history for that sequence.
    L.session_disk.bindings["K1"].frame = "corrupted";
    const auto r = L.session->send_once(CommandId("K1"), L.intent("K1")->payload_hash,
                                        L.intent("K1")->fix_body);
    CHECK_EQ(r.state, p01::StableSendResult::State::Fatal);

    // The router escalates rather than reissuing.
    L.must_submit_new("K2");
    L.session_disk.resend_history_lost = true;
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-14", "P0", "a retry across a session reset is still protected from a duplicate") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    L.session->reconnect(true);  // day boundary, sequences restart
    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK_EQ(again.value().cl_ord_id.str(), a.cl_ord_id.str());
    L.drive();
    // Still one order at the LP and one ClOrdID in the index.
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-15", "P0", "an ambiguous cancel is not retried with a fresh identifier") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();
    CHECK_EQ(L.intent("C1")->delivery, DeliveryState::UnknownTransport);

    L.crash();
    L.boot();
    // Nothing invents a second cancel, and the chain still shows one pending.
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->pending_cancel->str(), c.cl_ord_id.str());
    // And a second cancel command is refused while one is pending.
    auto second = L.submit(L.cancel_order("C2", a.chain_id));
    CHECK(second.is_err());
    CHECK_INV(L);
}

SCENARIO("OR-E-16", "P0", "an ambiguous replace is not reissued with different parameters") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();
    const std::string body = L.intent("R1")->fix_body;

    L.crash();
    L.boot();
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK_EQ(L.wire_count('G'), std::size_t{1});
    CHECK_EQ(L.intent("R1")->fix_body, body);
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    (void)r;
    CHECK_INV(L);
}

SCENARIO("OR-E-17", "P0", "a session that keeps refusing leaves the backlog durable and unsent") {
    Lab L;
    L.must_submit_new("K1");
    L.session->set_state(p01::SessionState::Down);
    for (int i = 0; i < 20; ++i) {
        L.advance(500000);
        L.rt->dispatch_pending();
    }
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Unknown);
    CHECK_NE(L.intent("K1")->delivery, DeliveryState::LpAcked);
    CHECK_INV(L);
}

SCENARIO("OR-E-18", "P0", "a second router instance cannot issue against the same store") {
    Lab L;
    L.must_submit_new("K1");

    // A competing instance takes the lease.
    Store second(L.disk, L.fp, L.clock);
    Metrics m2;
    p01::SessionEngine s2(L.session_disk, L.fp);
    Router rival(L.cfg, second, s2, *L.risk, m2, L.fp, L.clock);
    CHECK(rival.boot().is_ok());

    // The original instance is fenced out: its writes fail and nothing is sent.
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    // The new owner can dispatch, exactly once.
    rival.dispatch_pending();
    CHECK_EQ(rival.original_sends(), std::size_t{1});
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-E-19", "P0", "an outbox row with no payload hash is repaired, never issued") {
    Lab L;
    L.must_submit_new("K1");
    OutboxRow row = L.disk.outbox.begin()->second;
    row.payload_hash = Hash{};  // a row from an older schema
    L.disk.outbox[row.id] = row;

    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    // And a boot refuses to start against such a store.
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
}

SCENARIO("OR-E-20", "P0", "a network blackhole produces an unknown outcome, not a false ack") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.session_disk.blackhole = true;  // the socket takes the bytes, nothing arrives
    L.drive();

    CHECK_EQ(L.lp_disk.book.size(), std::size_t{0});
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Unknown);
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_EQ(L.rt->open_unknown_outcomes(), std::size_t{1});
    // Bounded: no retry storm and no second order.
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}
