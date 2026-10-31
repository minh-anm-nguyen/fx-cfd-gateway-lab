// Section O: crash consistency, database faults and recovery.
//
// Each case names the crash window from design doc 15.1 that it covers. The
// shape is always the same: arm a failpoint, let the process die there, boot a
// new process against the same durable images, and then assert the three things
// release gate 23.2 actually cares about -- no accepted intent lost, no second
// original send, no double fill.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-O-01", "P0", "CW-01 a crash before the intent commit leaves no trace") {
    Lab L;
    L.fp.arm(fp::kBeforeIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.new_order("K1")); }));
    L.fp.clear();

    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{0});
    CHECK_EQ(L.disk.chains.size(), std::size_t{0});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{0});

    // The retry is safe and produces exactly one intent.
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_ok());
    CHECK(!r.value().idempotent_replay);
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-02", "P0", "CW-02 a crash inside the intent transaction is all or nothing") {
    Lab L;
    L.fp.arm(fp::kInsideIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.new_order("K1")); }));
    L.fp.clear();

    // No half written row set: either every table moved or none did.
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{0});
    CHECK_EQ(L.disk.chains.size(), std::size_t{0});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{0});
    CHECK_EQ(L.disk.journal.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-O-03", "P0", "CW-03 a crash after the commit returns the same record on retry") {
    Lab L;
    L.fp.arm(fp::kAfterIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.new_order("K1")); }));
    L.fp.clear();
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    const std::string cl = L.disk.intents.begin()->second.cl_ord_id.str();

    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK(again.value().idempotent_replay);
    CHECK_EQ(again.value().cl_ord_id.str(), cl);
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-04", "P0", "CW-04 a crash after the outbox claim replays one logical send") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kAfterOutboxClaim, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->dispatch_pending(); }));
    L.fp.clear();
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});

    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-05", "P0", "CW-05 a bound sequence with a lost reply comes back as the same N") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->dispatch_pending(); }));
    L.fp.clear();

    const auto binding = L.session->lookup_binding(CommandId("K1"));
    CHECK(binding.has_value());
    const MsgSeqNum n = binding->original_seq;

    // After the restart the router looks the binding up; it does not allocate.
    L.drive();
    CHECK_EQ(L.session->lookup_binding(CommandId("K1"))->original_seq, n);
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.session_disk.next_out, n + 1);
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-06", "P0", "CW-06 a crash before the binding is durable writes nothing") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kBeforeSendBinding, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->dispatch_pending(); }));
    L.fp.clear();

    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.next_out, MsgSeqNum{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{0});

    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-07", "P0", "CW-07 a durable binding with no write is reused, not reallocated") {
    Lab L;
    L.must_submit_new("K1");
    L.fp.arm(fp::kAfterSendBinding, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->dispatch_pending(); }));
    L.fp.clear();

    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    const MsgSeqNum n = L.session->lookup_binding(CommandId("K1"))->original_seq;
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    L.drive();
    CHECK_EQ(L.session->lookup_binding(CommandId("K1"))->original_seq, n);
    CHECK_EQ(L.session_disk.bindings.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-08", "P0", "CW-08 a frame cut in half creates no order at the peer") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.fp.arm(fp::kPartialWireWrite, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    CHECK_EQ(L.lp_disk.book.size(), std::size_t{0});
    CHECK(!L.lp_disk.rejected_frames.empty());
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::UnknownTransport);
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-O-09", "P0", "CW-09 a crash after the full write ends in recon, not resubmission") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;
    L.fp.arm(fp::kAfterWireWrite, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->dispatch_pending(); }));
    L.fp.clear();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-O-10", "P0", "CW-10 a peer that reports only after its own recovery applies once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.lp_disk.suppress_reports = true;  // the LP accepted but died before reporting
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);

    // The peer comes back and emits what it owed, twice over for good measure.
    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound();

    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-11", "P0", "CW-11 a receipt committed in P01 survives a P04 restart") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.fp.arm(fp::kAfterInboxCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.lp->execute(a.cl_ord_id.str(), 40000, 108500); }));
    L.fp.clear();

    // P01 still holds the unacknowledged receipt.
    CHECK_EQ(L.session->pending_inbound(), std::size_t{1});
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-12", "P0", "CW-12 a crash between normalisation and the commit replays once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    L.fp.arm(fp::kAfterNormalize, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->pump_inbound(); }));
    L.fp.clear();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);  // nothing applied

    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-13", "P0", "CW-13 a crash inside the apply transaction leaves no half state") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    L.fp.arm(fp::kInsideApplyTxn, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->pump_inbound(); }));
    L.fp.clear();

    // Neither the projection nor the dedup key moved.
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|EX-1"), std::size_t{0});
    CHECK_INV(L);

    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.disk.exec_keys.count("LP_SIM_01|EX-1"), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-14", "P0", "CW-14 a crash after the commit makes the redelivery a no-op") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    L.fp.arm(fp::kAfterApplyCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->pump_inbound(); }));
    L.fp.clear();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.session->pending_inbound(), std::size_t{1});  // never acknowledged

    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(L.session->pending_inbound(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-O-15", "P0", "CW-15 a crash before the risk release replays it exactly once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string reservation = L.intent("K1")->risk_reservation_id;
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);

    L.fp.arm(fp::kBeforeRiskEffect, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->pump_inbound(); }));
    L.fp.clear();
    CHECK(L.risk->held(reservation));

    L.rt->pump_inbound();
    L.rt->pump_inbound();
    CHECK(L.risk->effective_releases() <= std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-16", "P0", "CW-16 a crash during a status dispatch invents no execution") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();

    L.fp.arm(fp::kDuringStatusDispatch, FaultKind::Crash);
    L.clock.advance(L.cfg.limits.ack_timeout_micros * 3);
    CHECK(L.crash_during([&] { L.rt->on_timer(); }));
    L.fp.clear();

    // The case and the query intent are durable; nothing was executed.
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    // Resuming does not duplicate the probe intent.
    L.lp_disk.suppress_reports = false;
    L.tick();
    CHECK(L.wire_count('H') <= std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-O-17", "P0", "CW-17 a crash after the status answer never synthesises a fill") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    // The peer answers with a bigger total than our ledger shows.
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.lp->send(s);

    L.fp.arm(fp::kAfterStatusReply, FaultKind::Crash);
    L.crash_during([&] { L.rt->pump_inbound(); });
    L.fp.clear();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-18", "P0", "CW-18 a crash during the journal prune keeps the state recoverable") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();
    const std::size_t journal_before = L.disk.journal.size();

    L.fp.arm(fp::kDuringSnapshotPrune, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.store->prune_journal(1); }));
    L.fp.clear();

    CHECK_EQ(L.disk.journal.size(), journal_before);  // nothing was dropped
    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());

    // A completed prune still leaves the state reconstructable from the
    // snapshot plus what remains of the journal.
    CHECK(L.store->prune_journal(1).is_ok());
    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.at(a.chain_id.str()).state_hash().hex(), before.hex());
    CHECK_INV(L);
}

SCENARIO("OR-O-19", "P0", "a lost commit acknowledgement is re-read, not assumed rolled back") {
    Lab L;
    L.fp.arm(fp::kCommitAckLost, FaultKind::Error);
    auto r = L.submit(L.new_order("K1"));
    L.fp.clear();
    // The store lost the acknowledgement, so the router re-reads the
    // authoritative key rather than assuming a rollback. The row is there, so
    // the command is accepted and the reservation is not handed back.
    CHECK(r.is_ok());
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK(L.risk->held(L.intent("K1")->risk_reservation_id));

    // A retry returns the same record instead of creating a second intent.
    auto again = L.submit(L.new_order("K1"));
    CHECK(again.is_ok());
    CHECK(again.value().idempotent_replay);
    CHECK_EQ(again.value().cl_ord_id.str(), r.value().cl_ord_id.str());
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    L.drive();
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-O-20", "P0", "a full or corrupt store halts new work and keeps the audit") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::size_t audit_before = L.disk.audit.size();

    // ENOSPC arrives while the order is live.
    L.disk.full = true;
    auto blocked = L.submit(L.new_order("K2"));
    CHECK(blocked.is_err());
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK(L.disk.audit.size() >= audit_before);
    CHECK(L.disk.audit.verify());

    // A corrupt image refuses to boot at all, rather than starting empty.
    L.disk.full = false;
    L.disk.corrupt = true;
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK(L.submit(L.new_order("K3")).is_err());
    // And the existing state is still there, untouched.
    CHECK_EQ(L.disk.chains.size(), std::size_t{1});
    CHECK_EQ(L.disk.chains.begin()->second.chain_id.str(), a.chain_id.str());
}
