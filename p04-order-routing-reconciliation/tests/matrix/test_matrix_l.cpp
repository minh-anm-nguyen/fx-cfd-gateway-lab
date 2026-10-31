// Section L: ordering, orphan reports, replay and per-chain serialization.
//
// A note on the word "concurrent" in this section of the matrix. The engine is
// a single owner per chain by construction (design doc 5.1), so the races the
// matrix describes are modelled by interleaving the steps explicitly and, where
// the real protection is a database constraint, by driving two transactions at
// the store directly. That is stated plainly in the README limitations rather
// than dressed up as a multi-threaded run.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-L-01", "P0", "only the ordered stream P01 delivers is applied") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // Two fills enter the durable inbox in order; the consumer sees them in that
    // order and nothing is applied ahead of its predecessor.
    L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    p02::ReportSpec later = L.lp->trade(a.cl_ord_id.str(), "EX2", 60000, 108510);
    later.cum_qty = 100000;
    later.leaves_qty = 0;
    later.ord_status = OrdStatus::Filled;
    L.lp->send(later);

    // Pump one at a time to observe the intermediate state.
    L.rt->pump_inbound(1);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_INV(L);
    L.rt->pump_inbound(1);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_INV(L);
}

SCENARIO("OR-L-02", "P0", "a redelivery after a lost acknowledgement is idempotent") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const DurableReceiptId r = L.lp->execute(a.cl_ord_id.str(), 40000, 108500);

    // The commit happens, the acknowledgement to P01 is lost, so P01 redelivers.
    L.fp.arm(fp::kAfterApplyCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.rt->pump_inbound(); }));
    L.fp.clear();
    CHECK_EQ(L.session->pending_inbound(), std::size_t{1});

    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(L.session->pending_inbound(), std::size_t{0});
    (void)r;
    CHECK_INV(L);
}

SCENARIO("OR-L-03", "P0", "a fill that precedes the acknowledgement is not dropped") {
    Lab L;
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.inject(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    L.inject(L.lp->new_ack(a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::PartiallyFilled);
    CHECK_INV(L);
}

SCENARIO("OR-L-04", "P0", "a cancel confirmation before its pending report stays terminal") {
    Lab L;
    L.lp_disk.auto_pending_cancel = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    L.inject(L.lp->canceled(c.cl_ord_id.str(), a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    L.inject(L.lp->pending_cancel(c.cl_ord_id.str(), a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    // The contradictory late event is on the record.
    CHECK(L.disk.audit.count(AuditKind::InboundApplied) >= std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-L-05", "P0", "a replace acceptance before its pending report does not regress") {
    Lab L;
    L.lp_disk.auto_pending_replace = false;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "80000", "1.08490");
    L.drive();

    L.inject(L.lp->replaced(r.cl_ord_id.str(), a.cl_ord_id.str(), 80000, 108490));
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), r.cl_ord_id.str());

    L.inject(L.lp->pending_replace(r.cl_ord_id.str(), a.cl_ord_id.str()));
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), r.cl_ord_id.str());
    CHECK_EQ(L.chain(a.chain_id)->accepted_qty, 80000);
    CHECK_INV(L);
}

SCENARIO("OR-L-06", "P0", "a report that arrives before its chain is committed is kept") {
    Lab L;
    // The intent transaction dies, so no chain and no ClOrdID index entry exist.
    L.fp.arm(fp::kBeforeIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.new_order("K1")); }));
    L.fp.clear();
    CHECK_EQ(L.disk.chains.size(), std::size_t{0});

    // The LP reports a fill for the order it did receive.
    p02::ReportSpec s = L.lp->trade("O-1", "EX1", 40000, 108500);
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    s.order_qty = 100000;
    s.ord_status = OrdStatus::PartiallyFilled;
    L.inject(s);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_EQ(L.disk.quarantine.at(0).reason, HoldReason::OrphanReport);
    CHECK_INV(L);

    // Now the command is admitted properly, and the kept evidence correlates.
    const auto a = L.must_submit_new("K1");
    CHECK_EQ(a.cl_ord_id.str(), std::string("O-1"));
    L.drive();
    const std::size_t resolved = L.rt->resolve_orphans();
    CHECK_EQ(resolved, std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-L-07", "P0", "an orphan that never resolves stays in the queue, not expired away") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX9", 40000, 108500);
    s.cl_ord_id = "O-999";
    s.orig_cl_ord_id.clear();
    L.inject(s);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});

    // Time passes and nothing resolves it. The evidence is still there.
    for (int i = 0; i < 20; ++i) L.tick(60000000);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK(!L.disk.quarantine.at(0).raw_hash.is_zero());
    CHECK_EQ(L.rt->resolve_orphans(), std::size_t{0});
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-L-08", "P0", "an orphan with identical instrument and size is not merged") {
    Lab L;
    const auto a = L.must_submit_new("K1", "100000", "1.08500");
    L.drive();
    // Same symbol, same side, same quantity, same price. Only the identifier
    // differs, and that is the only thing correlation looks at.
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX9", 100000, 108500);
    s.cl_ord_id = "O-999";
    s.orig_cl_ord_id.clear();
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-L-09", "P0", "two fills whose cumulative totals disagree are held, not dropped") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // The peer sends the second execution first, claiming a cumulative total
    // that the ledger cannot justify.
    p02::ReportSpec ahead = L.lp->trade(a.cl_ord_id.str(), "EX2", 60000, 108510);
    ahead.cum_qty = 100000;  // implies a 40000 fill we have never seen
    ahead.leaves_qty = 0;
    ahead.ord_status = OrdStatus::Filled;
    L.inject(ahead);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-L-10", "P0", "two reports for one chain are serialized into one deterministic order") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    p02::ReportSpec second = L.lp->trade(a.cl_ord_id.str(), "EX2", 60000, 108510);
    second.cum_qty = 100000;
    second.leaves_qty = 0;
    second.ord_status = OrdStatus::Filled;
    L.lp->send(second);

    L.rt->pump_inbound();
    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->fills.size(), std::size_t{2});
    CHECK_EQ(c->fills.at(0).applied_seq, std::uint64_t{1});
    CHECK_EQ(c->fills.at(1).applied_seq, std::uint64_t{2});
    CHECK_EQ(c->cum_qty, 100000);
    // Running the same stream again from the journal gives the same state.
    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.at(a.chain_id.str()).state_hash().hex(), c->state_hash().hex());
    CHECK_INV(L);
}

SCENARIO("OR-L-11", "P0", "a cancel submitted while a fill is being applied loses nothing") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // The fill is sitting in the inbox, not yet applied, when the cancel is
    // admitted. Both must end up in the state.
    L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    const p03::ChainState* chain = L.chain(a.chain_id);
    CHECK_EQ(chain->cum_qty, 40000);
    CHECK_EQ(chain->fills.size(), std::size_t{1});
    CHECK_EQ(chain->pending_cancel->str(), c.cl_ord_id.str());
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-L-12", "P0", "of a replace and a cancel at once, exactly one is accepted") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    auto g = L.submit(L.replace_order("R1", a.chain_id, "80000", "1.08490"));
    auto f = L.submit(L.cancel_order("C1", a.chain_id));
    CHECK(g.is_ok());
    CHECK(f.is_err());
    CHECK_EQ(f.error(), RoutingError::PendingConflict);
    L.drive();
    CHECK_EQ(L.wire_count('G'), std::size_t{1});
    CHECK_EQ(L.wire_count('F'), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-L-13", "P0", "the uniqueness constraint lets only one of two racing inserts win") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    // Two transactions, built against the same pre-state, both reserving the
    // same (lp, ClOrdID). This is the store level race the keyed actor plus the
    // unique index is there to resolve.
    CommandIntent rival = *L.intent("K1");
    rival.command_id = CommandId("K1-rival");

    Txn t = L.store->begin();
    t.put_intent(rival);
    t.reserve_cl_ord_id(rival.lp, rival.cl_ord_id, rival.key());
    auto result = t.commit();
    CHECK(result.is_err());
    CHECK_EQ(result.error(), StoreError::Conflict);

    // One intent, one index entry, one chain.
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_EQ(L.disk.chains.size(), std::size_t{1});
    (void)a;
    CHECK_INV(L);
}

SCENARIO("OR-L-14", "P0", "interleaved chains keep independent ledgers") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    L.drive();

    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.lp->execute(b.cl_ord_id.str(), 10000, 108400);
    L.lp->execute(a.cl_ord_id.str(), 60000, 108520);
    L.lp->execute(b.cl_ord_id.str(), 20000, 108410);
    L.drive();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{2});
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);
    CHECK_EQ(L.chain(b.chain_id)->cum_qty, 30000);
    CHECK_EQ(L.chain(b.chain_id)->fills.size(), std::size_t{2});
    CHECK_EQ(L.chain(b.chain_id)->status, OrdStatus::PartiallyFilled);
    CHECK_INV(L);
}

SCENARIO("OR-L-15", "P0", "an event from the previous epoch is not aliased onto a new one") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const DurableReceiptId old_receipt = L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();
    const std::string old_key =
        L.session_disk.identity.key() + "|1|" + std::to_string(old_receipt);
    CHECK_EQ(L.disk.applied_receipts.count(old_key), std::size_t{1});

    // After the reset, receipt numbering restarts; the applied key is different
    // because it carries the epoch.
    L.session->reconnect(true);
    CHECK_EQ(L.session_disk.next_receipt, DurableReceiptId{1});  // numbering restarted
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX-NEW", 60000, 108510);
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{2});
    CHECK(L.disk.applied_receipts.size() >= std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-L-16", "P0", "a restored mapping that disagrees with the peer is not remapped") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string bound = L.chain(a.chain_id)->lp_order_id->str();

    L.crash();
    L.boot();
    // The peer now uses a different OrderID for the same chain.
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.order_id = "LP-REMAPPED";
    s.cum_qty = 40000;
    s.leaves_qty = 60000;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->lp_order_id->str(), bound);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-L-17", "P0", "a cancel reject arriving before its request is committed is kept") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // The cancel admission dies before the commit, so no pending request exists.
    L.fp.arm(fp::kBeforeIntentCommit, FaultKind::Crash);
    CHECK(L.crash_during([&] { L.submit(L.cancel_order("C1", a.chain_id)); }));
    L.fp.clear();
    CHECK(!L.chain(a.chain_id)->pending_cancel.has_value());

    p02::ReportSpec s = L.lp->cancel_reject("C-2", a.cl_ord_id.str(),
                                            CxlRejResponseTo::OrderCancelRequest, 0);
    L.inject(s);
    // Durably kept and escalated, not applied and not dropped.
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-L-18", "P0", "a poison event repeated a hundred times does not block the pipeline") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();

    p02::ReportSpec poison = L.lp->trade(a.cl_ord_id.str(), "EXBAD", 40000, 108500);
    poison.omit.insert(32);  // a Trade with no LastQty, forever invalid
    for (int i = 0; i < 100; ++i) L.lp->send(poison);
    L.rt->pump_inbound(200);

    // Every copy is durably quarantined and marked consumed, so the queue is
    // empty and a good event behind them still gets through.
    CHECK_EQ(L.quarantine_depth(), std::size_t{100});
    CHECK_EQ(L.session->pending_inbound(), std::size_t{0});
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_INV(L);
}

SCENARIO("OR-L-19", "P0", "a gap fill that restores a Trade applies it exactly once") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const DurableReceiptId r = L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.rt->pump_inbound();

    // The session recovers a gap and replays the same application message.
    L.session->redeliver(r);
    p02::ReportSpec gap = L.lp->trade(a.cl_ord_id.str(), "EX-1", 40000, 108500);
    gap.cum_qty = 40000;
    gap.leaves_qty = 60000;
    gap.poss_dup = true;
    L.lp->send(gap);
    L.rt->pump_inbound();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-L-20", "P1", "a hot chain does not starve the cold ones") {
    Lab L;
    std::vector<AcceptedIntent> chains;
    for (int i = 0; i < 20; ++i) chains.push_back(L.must_submit_new("K" + std::to_string(i)));
    L.drive(32);

    // One chain produces a flood of events; the others produce one each.
    const AcceptedIntent& hot = chains.at(0);
    for (int i = 0; i < 500; ++i) {
        p02::ReportSpec s = L.lp->trade(hot.cl_ord_id.str(), "HOT-" + std::to_string(i), 100,
                                        108500);
        s.cum_qty = 100 * (i + 1);
        s.leaves_qty = 100000 - s.cum_qty;
        s.ord_status = OrdStatus::PartiallyFilled;
        L.lp->send(s);
    }
    for (std::size_t i = 1; i < chains.size(); ++i)
        L.lp->send(L.lp->trade(chains[i].cl_ord_id.str(), "COLD-" + std::to_string(i), 1000,
                               108500));

    L.rt->pump_inbound(2000);
    // The hot chain is correct and ordered, and every cold chain was served.
    CHECK_EQ(L.chain(hot.chain_id)->fills.size(), std::size_t{500});
    CHECK_EQ(L.chain(hot.chain_id)->cum_qty, 50000);
    for (std::size_t i = 1; i < chains.size(); ++i)
        CHECK_EQ(L.chain(chains[i].chain_id)->cum_qty, 1000);
    CHECK_INV(L);
}
