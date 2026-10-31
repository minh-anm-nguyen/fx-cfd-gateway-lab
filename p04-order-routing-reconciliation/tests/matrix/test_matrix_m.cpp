// Section M: unknown outcome, ambiguous acknowledgement and escalation.
#include <set>

#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-M-01", "P0", "a send with no result is stored as unknown, not retried as new") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::UnknownTransport);
    // The binding is where the truth lives, and it is looked up rather than
    // guessed at.
    const auto binding = L.session->lookup_binding(CommandId("K1"));
    CHECK(binding.has_value());
    CHECK_EQ(L.intent("K1")->original_seq.value(), binding->original_seq);
    CHECK_EQ(L.case_for(a.chain_id)->cause, HoldReason::AmbiguousSend);
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-M-02", "P0", "a status probe finds the order the LP already had") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.suppress_reports = true;  // the LP took the order but said nothing
    const auto a = L.must_submit_new("K1");
    L.drive();
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});

    L.lp_disk.suppress_reports = false;  // the link is back for the probe
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();

    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->state, ReconState::Reconciled);
    CHECK_EQ(c->resolved_by, std::string("status_snapshot"));
    // One order at the LP, one D on the wire, no duplicate.
    CHECK_EQ(L.lp_disk.book.size(), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-M-03", "P0", "a send the LP never saw is held, not resubmitted") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.session_disk.blackhole = true;
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();

    CHECK_EQ(L.lp_disk.book.size(), std::size_t{0});
    for (int i = 0; i < 6; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::ManualHold);
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-M-04", "P0", "a trade is applied even though the acknowledgement was lost") {
    Lab L;
    L.lp_disk.auto_ack = false;  // no 8/150=0 ever arrives
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.intent("K1")->business, BusinessOutcome::Acknowledged);
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::LpAcked);
    CHECK_INV(L);
}

SCENARIO("OR-M-05", "P0", "a cancel with no answer stays unknown while fills continue") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp_disk.auto_pending_cancel = false;
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    // A fill arrives while the cancel is unanswered.
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK(L.chain(a.chain_id)->pending_cancel.has_value());
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);

    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK_EQ(L.intent("C1")->business, BusinessOutcome::Unknown);
    (void)c;
    CHECK_INV(L);
}

SCENARIO("OR-M-06", "P0", "a fill naming an unaccepted replace version is not a silent switch") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto r = L.must_submit_replace("R1", a.chain_id, "150000", "1.08500");
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());

    // The acceptance is suppressed and the LP fills against the new identifier.
    p02::ReportSpec s = L.lp->trade(r.cl_ord_id.str(), "EX1", 20000, 108500);
    s.cum_qty = 20000;
    s.leaves_qty = 130000;
    s.order_qty = 150000;
    s.ord_status = OrdStatus::PartiallyFilled;
    L.inject(s);

    // The accepted version did not move, and the contradiction is escalated.
    CHECK_EQ(L.chain(a.chain_id)->accepted_cl_ord_id.str(), a.cl_ord_id.str());
    CHECK(L.chain(a.chain_id)->hold.has_value() || L.quarantine_depth() >= std::size_t{1});
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-M-07", "P0", "a working-order snapshot does not settle an unknown cancel") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp_disk.auto_pending_cancel = false;
    L.must_submit_cancel("C1", a.chain_id);
    L.drive();

    // The probe comes back saying the order is still New. That says nothing
    // about what happened to the cancel request.
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_NE(c->state, ReconState::Reconciled);
    CHECK_NE(L.chain(a.chain_id)->status, OrdStatus::Canceled);
    bool noted = false;
    for (const std::string& n : c->notes)
        if (n.find("cancel or replace") != std::string::npos) noted = true;
    CHECK(noted);
    CHECK_INV(L);
}

SCENARIO("OR-M-08", "P0", "without the status capability an unknown New goes to manual hold") {
    Lab L;
    CHECK(!L.cfg.capabilities.order_status_request);
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);

    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::ManualHold);
    CHECK_EQ(L.wire_count('H'), std::size_t{0});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-M-09", "P0", "a peer that denies the order while we hold a fill never erases it") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);

    // The probe answers that the order is unknown: rejected, nothing filled.
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.ord_status = OrdStatus::Rejected;
    s.cum_qty = 0;
    s.leaves_qty = 0;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::SnapshotMismatch);
    CHECK_INV(L);
}

SCENARIO("OR-M-10", "P0", "an unknown order is only closed by an authorised, evidenced decision") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK_EQ(c->state, ReconState::ManualHold);
    const ReconCaseId id = c->case_id;

    const Hash evidence = sha256("p02 audit export: no order O-1, no executions");
    auto r = L.rt->operator_resolve_case(Lab::operator_principal(), id,
                                         "peer audit proves absence", evidence,
                                         {"ops1", "risk2"});
    CHECK(r.is_ok());
    CHECK_EQ(L.store->find_recon(id)->state, ReconState::Reconciled);
    CHECK_EQ(L.store->find_recon(id)->approvals.size(), std::size_t{2});
    CHECK(L.store->find_recon(id)->last_evidence_hash == evidence);
    // No fill was invented in the process.
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-M-11", "P0", "an acknowledgement that arrives late still closes the case properly") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    CHECK(L.case_for(a.chain_id) != nullptr);

    // Ten seconds later the real acknowledgement shows up.
    L.advance(10000000);
    L.lp_disk.suppress_reports = false;
    L.lp->send(L.lp->new_ack(a.cl_ord_id.str()));
    L.rt->pump_inbound();

    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::New);
    CHECK_EQ(L.case_for(a.chain_id)->state, ReconState::Reconciled);
    CHECK_EQ(L.case_for(a.chain_id)->resolved_by, std::string("evidence"));
    CHECK_EQ(L.wire_count('D'), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-M-12", "P0", "two causes on one chain are tracked as separate audited cases") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // First cause: an ambiguous cancel send.
    L.must_submit_cancel("C1", a.chain_id);
    L.fp.arm(fp::kDropSendOnceReply, FaultKind::Error);
    L.rt->dispatch_pending();
    L.fp.clear();
    // Second cause: a contradictory report on the same chain.
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EXBAD", 40000, 108500);
    s.symbol = "GBP/USD";
    L.inject(s);

    std::size_t cases = 0;
    std::set<std::string> causes;
    for (const auto& [k, c] : L.disk.recon_cases) {
        (void)k;
        if (c.chain_id != a.chain_id) continue;
        ++cases;
        causes.insert(std::string(to_string(c.cause)));
    }
    CHECK(cases >= std::size_t{2});
    CHECK(causes.size() >= std::size_t{2});
    CHECK(L.disk.audit.count(AuditKind::ReconOpened) >= std::size_t{2});
    CHECK_INV(L);
}

SCENARIO("OR-M-13", "P0", "the probe budget is finite and ends in a manual hold") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.reject_status_requests = true;  // the peer never answers
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();

    for (int i = 0; i < 20; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 8);
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK_EQ(c->state, ReconState::ManualHold);
    CHECK(c->attempt_count <= L.cfg.limits.recon_max_probes);
    CHECK(L.wire_count('H') <= std::size_t{3});
    CHECK_INV(L);
}

SCENARIO("OR-M-14", "P0", "a snapshot about another chain is not evidence about this one") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    L.drive();
    L.lp_disk.suppress_reports = true;
    L.tick(L.cfg.limits.ack_timeout_micros * 3);

    // A snapshot arrives describing chain B while chain A is the one in doubt.
    L.lp_disk.suppress_reports = false;
    L.inject(L.lp->status_snapshot(b.cl_ord_id.str(), std::nullopt));

    const ReconCase* ca = L.case_for(a.chain_id);
    if (ca != nullptr) CHECK_NE(ca->state, ReconState::Reconciled);
    CHECK_INV(L);
}

SCENARIO("OR-M-15", "P0", "a snapshot for another account is a security hold") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.account = "ACC2";
    L.inject(s);

    bool security = false;
    for (const QuarantineEntry& q : L.disk.quarantine)
        if (q.reason == HoldReason::SecurityQuarantine) security = true;
    CHECK(security);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_INV(L);
}

SCENARIO("OR-M-16", "P0", "a snapshot the ledger cannot justify stays unresolved") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 100000;  // the peer claims more than our history shows
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_NE(c->state, ReconState::Reconciled);
    CHECK_INV(L);
}

SCENARIO("OR-M-17", "P0", "an older snapshot does not overwrite newer canonical state") {
    Lab L;
    L.enable_status_queries();
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);

    // A snapshot generated before the fill arrives afterwards.
    p02::ReportSpec stale = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    stale.cum_qty = 0;
    stale.leaves_qty = 100000;
    stale.ord_status = OrdStatus::New;
    L.inject(stale);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{1});
    CHECK(L.chain(a.chain_id)->hold.has_value());
    CHECK_INV(L);
}

SCENARIO("OR-M-18", "P0", "a terminal snapshot with an empty ledger creates no trades") {
    Lab L;
    L.enable_status_queries();
    L.lp_disk.auto_ack = false;
    const auto a = L.must_submit_new("K1");
    L.drive();

    p02::ReportSpec s = L.lp->status_snapshot(a.cl_ord_id.str(), std::nullopt);
    s.cum_qty = 100000;
    s.leaves_qty = 0;
    s.ord_status = OrdStatus::Filled;
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.chain(a.chain_id)->hold.value(), HoldReason::HistoryRequired);
    CHECK(L.disk.exec_keys.empty());
    CHECK_INV(L);
}

SCENARIO("OR-M-19", "P0", "an unauthorised or unevidenced force-resolve is denied") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCaseId id = L.case_for(a.chain_id)->case_id;

    // Wrong role.
    auto r1 = L.rt->operator_resolve_case(Lab::viewer(), id, "trust me", sha256("x"),
                                          {"guest", "guest2"});
    CHECK(r1.is_err());
    CHECK_EQ(r1.error(), RoutingError::AuthzDenied);
    // No evidence.
    auto r2 = L.rt->operator_resolve_case(Lab::operator_principal(), id, "trust me", Hash{},
                                          {"ops1", "ops2"});
    CHECK(r2.is_err());
    // Only one approver.
    auto r3 = L.rt->operator_resolve_case(Lab::operator_principal(), id, "trust me",
                                          sha256("x"), {"ops1"});
    CHECK(r3.is_err());

    CHECK_EQ(L.store->find_recon(id)->state, ReconState::ManualHold);
    CHECK(L.disk.audit.count(AuditKind::OperatorDenied) >= std::size_t{3});
    CHECK_EQ(L.disk.audit.count(AuditKind::OperatorAction), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-M-20", "P0", "an evidenced resolution records who, why and what they relied on") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCaseId id = L.case_for(a.chain_id)->case_id;

    const Hash evidence = sha256("signed drop copy bundle");
    auto r = L.rt->operator_resolve_case(Lab::admin(), id, "drop copy shows no execution",
                                         evidence, {"admin1", "ops7"});
    CHECK(r.is_ok());

    bool recorded = false;
    for (const AuditRecord& rec : L.disk.audit.records()) {
        if (rec.kind != AuditKind::OperatorAction) continue;
        CHECK_EQ(rec.actor, std::string("admin1"));
        CHECK(rec.detail.find("admin1") != std::string::npos);
        CHECK(rec.detail.find("ops7") != std::string::npos);
        CHECK(rec.detail.find("drop copy") != std::string::npos);
        CHECK(rec.evidence == evidence);
        recorded = true;
    }
    CHECK(recorded);
    CHECK(L.disk.audit.verify());
    CHECK_EQ(L.chain(a.chain_id)->fills.size(), std::size_t{0});
    CHECK_INV(L);
}
