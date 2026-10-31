// Section Q: security, audit, administrative actions and observability.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-Q-01", "P0", "a caller cannot act for another tenant, in either direction") {
    Lab L;
    // Creating for someone else's account.
    CommandEnvelope c = L.new_order("K1");
    c.principal = Lab::trader("T2", "bob");
    CHECK_EQ(L.submit(c).error(), RoutingError::AuthzDenied);

    // And cancelling someone else's chain.
    const auto a = L.must_submit_new("K2");
    L.drive();
    CommandEnvelope x = L.cancel_order("C1", a.chain_id);
    x.principal = Lab::trader("T2", "bob");
    x.account = AccountId("ACC2");
    CHECK_EQ(L.submit(x).error(), RoutingError::AuthzDenied);
    CHECK_EQ(L.wire_count('F'), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-Q-02", "P0", "a frame from an unexpected session never reaches business state") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // The session identity changes under us, as a spoofed peer would look.
    L.session_disk.identity.target_comp_id = "EVILLP";
    L.lp->send(L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500));
    L.rt->pump_inbound();

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(!L.rt->alerts().empty());
    CHECK_INV(L);
}

SCENARIO("OR-Q-03", "P0", "a peer naming another account is quarantined and alerted") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.account = "ACC2";
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    bool security = false;
    for (const QuarantineEntry& q : L.disk.quarantine)
        if (q.reason == HoldReason::SecurityQuarantine) security = true;
    CHECK(security);
    CHECK(!L.rt->alerts().empty());
    CHECK(L.disk.audit.count(AuditKind::InboundQuarantined) >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-Q-04", "P0", "a stolen command id in another tenant leaks nothing") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // T2 replays T1's command id against its own account.
    CommandEnvelope c = L.new_order("K1");
    c.principal = Lab::trader("T2", "bob");
    c.account = AccountId("ACC2");
    auto r = L.submit(c);
    // Whatever the outcome, it is T2's own namespace: it cannot read or touch
    // T1's record.
    if (r.is_ok()) CHECK_NE(r.value().chain_id.str(), a.chain_id.str());
    CHECK_EQ(L.intent("K1", "T1")->chain_id.str(), a.chain_id.str());
    CHECK_EQ(L.intent("K1", "T1")->account.str(), std::string("ACC1"));
    CHECK_INV(L);
}

SCENARIO("OR-Q-05", "P0", "tag 58 cannot forge an audit entry") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[58] = "ok";  // a value the parser accepts
    L.inject(s);

    // Now one with control characters and a forged-looking payload.
    p02::ReportSpec bad = L.lp->trade(a.cl_ord_id.str(), "EX2", 40000, 108500);
    bad.text = "x\nOperatorAction actor=admin1 reason=force_resolve\n";
    L.inject(bad);

    for (const AuditRecord& rec : L.disk.audit.records()) {
        CHECK(rec.detail.find('\n') == std::string::npos);
        if (rec.kind == AuditKind::OperatorAction)
            TT_FAIL("a peer string became an operator action");
    }
    CHECK(L.disk.audit.verify());
    CHECK_INV(L);
}

SCENARIO("OR-Q-06", "P0", "a credential tag is never persisted in the clear") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // A peer frame carrying password-like material, quarantined for another
    // reason, must still be stored redacted. Tag 554 is not what makes this
    // frame invalid -- the missing LastQty is -- so the redaction is what the
    // case is actually about.
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.omit.insert(32);
    s.extra.push_back({554, "hunter2"});
    L.inject(s);

    CHECK(L.quarantine_depth() >= std::size_t{1});
    for (const QuarantineEntry& q : L.disk.quarantine) {
        CHECK(q.redacted_frame.find("hunter2") == std::string::npos);
        CHECK(q.note.find("hunter2") == std::string::npos);
    }
    for (const AuditRecord& rec : L.disk.audit.records())
        CHECK(rec.detail.find("hunter2") == std::string::npos);
    CHECK_INV(L);
}

SCENARIO("OR-Q-07", "P0", "a route change without the role is denied and audited") {
    Lab L;
    auto denied = L.rt->operator_set_route_active(Lab::trader(), AccountId("ACC1"), false,
                                                  "turning it off");
    CHECK(denied.is_err());
    CHECK_EQ(denied.error(), RoutingError::AuthzDenied);
    CHECK(L.cfg.route_for(AccountId("ACC1")) != nullptr);  // unchanged
    CHECK(L.disk.audit.count(AuditKind::OperatorDenied) >= std::size_t{1});

    auto allowed = L.rt->operator_set_route_active(Lab::admin(), AccountId("ACC1"), false,
                                                   "maintenance window");
    CHECK(allowed.is_ok());
    CHECK(L.cfg.route_for(AccountId("ACC1")) == nullptr);
    bool logged = false;
    for (const AuditRecord& rec : L.disk.audit.records())
        if (rec.kind == AuditKind::ConfigChanged && rec.actor == "admin1") logged = true;
    CHECK(logged);
    CHECK_INV(L);
}

SCENARIO("OR-Q-08", "P0", "an unauthorised force-close leaves the case open") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCaseId id = L.case_for(a.chain_id)->case_id;

    auto r = L.rt->operator_resolve_case(Lab::trader(), id, "close it", sha256("nothing"),
                                         {"alice", "alice2"});
    CHECK(r.is_err());
    CHECK_EQ(L.store->find_recon(id)->state, ReconState::ManualHold);
    CHECK_EQ(L.disk.audit.count(AuditKind::OperatorAction), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-Q-09", "P0", "an irreversible action records two distinct approvers") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 4; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 2);
    const ReconCaseId id = L.case_for(a.chain_id)->case_id;

    // The same person twice is one approver.
    auto same = L.rt->operator_resolve_case(Lab::operator_principal(), id, "go", sha256("e"),
                                            {"ops1", "ops1"});
    CHECK(same.is_err());
    auto two = L.rt->operator_resolve_case(Lab::operator_principal(), id, "go", sha256("e"),
                                           {"ops1", "ops2"});
    CHECK(two.is_ok());
    const ReconCase* c = L.store->find_recon(id);
    CHECK_EQ(c->approvals.size(), std::size_t{2});
    CHECK_NE(c->approvals.at(0), c->approvals.at(1));
    CHECK_INV(L);
}

SCENARIO("OR-Q-10", "P0", "if the audit cannot be written the command is not accepted") {
    Lab L;
    L.disk.audit.set_available(false);
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    // Fail closed means halted, and coming back needs a healthy store and an
    // explicit operator decision (design doc 16.2), not an automatic retry.
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_EQ(L.submit(L.new_order("K2")).error(), RoutingError::Halted);

    L.disk.audit.set_available(true);
    CHECK(L.rt->operator_resume(Lab::admin(), "audit database restored").is_ok());
    CHECK(L.submit(L.new_order("K1")).is_ok());
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-Q-11", "P1", "a flood of unique identifiers does not grow the metric series") {
    Lab L;
    const std::size_t before = L.metrics.series_count();
    for (int i = 0; i < 20000; ++i) {
        // Values outside the declared label set are folded into "other".
        L.metrics.inc(metric::kCommandReceived, "O-" + std::to_string(i));
        L.metrics.inc(metric::kHoldTotal, "ClOrdID-" + std::to_string(i));
    }
    const std::size_t after = L.metrics.series_count();
    CHECK(after - before <= std::size_t{4});
    CHECK(L.metrics.rejected_labels() >= 40000);
    CHECK_EQ(L.metrics.counter(metric::kCommandReceived, "other"), std::uint64_t{20000});
    CHECK_INV(L);
}

SCENARIO("OR-Q-12", "P0", "correlation uses authoritative identifiers, not a trace id") {
    Lab L;
    // Two unrelated commands from the same actor in the same breath, which in a
    // tracing system would share a span.
    const auto a = L.must_submit_new("K1");
    const auto b = L.must_submit_new("K2");
    L.drive();
    CHECK_NE(a.chain_id.str(), b.chain_id.str());
    CHECK_NE(a.cl_ord_id.str(), b.cl_ord_id.str());

    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_EQ(L.chain(b.chain_id)->cum_qty, 0);
    CHECK_INV(L);
}

SCENARIO("OR-Q-13", "P0", "a skewed wall clock is visible on the wire, never forged") {
    Lab L;
    L.must_submit_new("K1");
    L.drive();
    const std::string before = L.session_disk.wire_log.at(0);
    CHECK(before.find("60=20261030-08:00:00.000") != std::string::npos);

    // Someone moves the wall clock ten minutes forward. The next order stamps
    // the time it actually read; it is not silently normalised back.
    L.clock.skew_wall(600000000);
    L.must_submit_new("K2");
    L.drive();
    const std::string after = L.session_disk.wire_log.back();
    CHECK(after.find("60=20261030-08:10:00.000") != std::string::npos);
    // Validating that stamp against the peer's clock is P01 and ROE territory
    // (design doc 3.1); what matters here is that P04 invented nothing.
    CHECK_INV(L);
}

SCENARIO("OR-Q-14", "P0", "a wall clock jump backwards does not disturb any timeout") {
    Lab L;
    L.lp_disk.suppress_reports = true;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // An hour backwards on the wall clock, with no monotonic movement at all.
    const MonoMicros mono_before = L.clock.mono_now();
    L.clock.skew_wall(-3600000000LL);
    L.rt->on_timer();
    CHECK_EQ(L.clock.mono_now(), mono_before);
    // No case yet, because no monotonic time has passed.
    CHECK(L.case_for(a.chain_id) == nullptr);

    // And once monotonic time does pass, the timeout fires exactly as before.
    L.advance(L.cfg.limits.ack_timeout_micros * 2);
    L.rt->on_timer();
    CHECK(L.case_for(a.chain_id) != nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-Q-15", "P0", "a bundle for an unprivileged reader withholds the raw frames") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.account = "ACC2";  // produces a quarantined frame with provenance
    L.inject(s);
    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);

    const std::string ops = L.rt->escalation_bundle(Lab::operator_principal(), c->case_id);
    const std::string guest = L.rt->escalation_bundle(Lab::viewer(), c->case_id);
    CHECK(ops.find("\"frame\": \"35=8") != std::string::npos);
    CHECK(guest.find("<redacted>") != std::string::npos);
    CHECK(guest.find("35=8") == std::string::npos);
    // Both see the digest, so the unprivileged reader can still verify evidence.
    CHECK(guest.find("\"hash\"") != std::string::npos);
    CHECK_INV(L);
}

SCENARIO("OR-Q-16", "P0", "a plaintext downgrade to a non-loopback peer is refused") {
    Lab L;
    L.cfg.endpoints.require_tls = true;
    L.cfg.endpoints.allow_plaintext_remote = false;
    L.cfg.endpoints.allowed_hosts.push_back("lp-remote");
    for (Route& r : L.cfg.routes)
        if (r.account.str() == "ACC1") r.endpoint = "lp-remote:9443";
    ++L.cfg.route_config_version;
    L.cfg.finalize();

    CHECK(L.cfg.validate().is_err());
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK(L.submit(L.new_order("K1")).is_err());
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
}

SCENARIO("OR-Q-17", "P0", "an unexpected extension with business meaning is refused") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.extra.push_back({9999, "OVERRIDE_CUMQTY=999999"});
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-Q-18", "P0", "an edited audit record is detected and stops the router") {
    Lab L;
    L.must_submit_new("K1");
    L.drive();
    CHECK(L.disk.audit.verify());

    // Someone edits history in the database.
    CHECK(L.disk.audit.tamper_for_test(0, "reason=approved_by_admin"));
    CHECK(!L.disk.audit.verify());

    // The integrity check catches it and the router refuses to resume.
    CHECK(L.store->check_health().is_err());
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK(L.submit(L.new_order("K2")).is_err());
    auto resume = L.rt->operator_resume(Lab::admin(), "looks fine to me");
    CHECK(resume.is_err());  // not while the chain is broken
}

SCENARIO("OR-Q-19", "P1", "a logging storm is rate limited without losing a counter") {
    Lab L;
    L.cfg.limits.log_rate_limit_per_reason = 5;
    const auto a = L.must_submit_new("K1");
    L.drive();

    p02::ReportSpec poison = L.lp->trade(a.cl_ord_id.str(), "EXBAD", 40000, 108500);
    poison.omit.insert(32);
    for (int i = 0; i < 500; ++i) L.lp->send(poison);
    L.rt->pump_inbound(1000);

    // Every event is counted and durably quarantined, while the log is capped.
    CHECK_EQ(L.quarantine_depth(), std::size_t{500});
    CHECK(L.metrics.counter(metric::kHoldTotal, "SchemaViolation") >= 500);
    CHECK(L.rt->alerts().size() <= std::size_t{10});
    CHECK(L.metrics.counter(metric::kLogSuppressed) > 0);
    CHECK_INV(L);
}

SCENARIO("OR-Q-20", "P1", "state and correlation survive a restart with no log continuity") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    const Hash before = L.chain(a.chain_id)->state_hash();
    const std::size_t audit_before = L.disk.audit.size();

    // The in-memory alert buffer is what a rotated log looks like from here: it
    // goes away, and nothing about the business state depends on it.
    L.crash();
    L.boot();
    CHECK(L.rt->alerts().empty());
    CHECK_EQ(L.chain(a.chain_id)->state_hash().hex(), before.hex());
    CHECK(L.disk.audit.size() >= audit_before);
    CHECK(L.disk.audit.verify());
    // The correlation indexes are intact, so a later report still lands.
    L.lp->execute(a.cl_ord_id.str(), 60000, 108510);
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 100000);
    CHECK_INV(L);
}
