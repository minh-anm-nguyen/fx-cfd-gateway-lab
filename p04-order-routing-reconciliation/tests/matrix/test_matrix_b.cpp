// Section B: route policy, Rules of Engagement and configuration.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-B-01", "P0", "one account, one route, pinned for the whole lifecycle") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    CHECK_EQ(a.route.str(), std::string("LP_SIM_01"));
    L.drive();
    const auto c = L.must_submit_cancel("C1", a.chain_id);
    CHECK_EQ(c.route.str(), std::string("LP_SIM_01"));
    for (const auto& [k, in] : L.disk.intents) {
        (void)k;
        CHECK_EQ(in.lp.str(), std::string("LP_SIM_01"));
    }
    CHECK_INV(L);
}

SCENARIO("OR-B-02", "P0", "an account with no active route is refused, not sent anywhere") {
    Lab L;
    L.cfg.set_route_active(AccountId("ACC1"), false);
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::RouteUnavailable);
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-03", "P0", "two active routes at equal priority fail validation") {
    Lab L;
    Route rival;
    rival.account = AccountId("ACC1");
    rival.lp = LpId("LP_SIM_01");
    rival.sender_comp_id = "ROUTER04";
    rival.target_comp_id = "LPSIM01";
    rival.session_qualifier = "PRIMARY";
    rival.endpoint = "127.0.0.1:19004";
    rival.active = true;
    rival.priority = 0;  // the same priority as the existing route
    L.cfg.add_route(rival);

    CHECK(L.cfg.validate().is_err());
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ConfigInvalid);
    CHECK_INV(L);
}

SCENARIO("OR-B-04", "P0", "a route change after commit does not move a pending command") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    CHECK_EQ(L.intent("K1")->lp.str(), std::string("LP_SIM_01"));

    L.cfg.set_route_lp(AccountId("ACC1"), LpId("LP_SIM_02"), "LPSIM01");
    const DispatchStats d = L.rt->dispatch_pending();

    // The committed command keeps LP_SIM_01 and is held rather than failed over.
    CHECK_EQ(L.intent("K1")->lp.str(), std::string("LP_SIM_01"));
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{0});
    CHECK_EQ(L.chain(a.chain_id)->lp.str(), std::string("LP_SIM_01"));
    CHECK_INV(L);
}

SCENARIO("OR-B-05", "P0", "a command carries the config version it was admitted under") {
    Lab L;
    const std::uint64_t v0 = L.cfg.route_config_version;
    L.must_submit_new("K1");
    CHECK_EQ(L.intent("K1")->route_config_version, v0);

    // Moving the configuration afterwards must not rewrite the record, and the
    // dispatcher must refuse to mix the two (design doc 8.2).
    L.cfg.set_symbol_price_scale("EUR/USD", 4, 1);
    CHECK_EQ(L.intent("K1")->route_config_version, v0);
    CHECK_NE(L.cfg.profile_version, L.intent("K1")->profile_version);
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-B-06", "P0", "a route whose CompIDs belong to another peer cannot be used") {
    Lab L;
    // Point ACC1 at a target CompID that is not the attached session.
    for (Route& r : L.cfg.routes)
        if (r.account.str() == "ACC1") r.target_comp_id = "OTHERLP";
    ++L.cfg.route_config_version;
    L.cfg.finalize();

    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    // Either the configuration is rejected outright or the route does not match
    // the session; both refuse before anything is sent.
    CHECK(r.error() == RoutingError::ConfigInvalid ||
          r.error() == RoutingError::RouteUnavailable);
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-07", "P0", "with the status capability off, reconciliation emits zero 35=H") {
    Lab L;
    CHECK(!L.cfg.capabilities.order_status_request);
    L.lp_disk.suppress_reports = true;  // the LP says nothing at all
    const auto a = L.must_submit_new("K1");
    L.drive();
    // Let the acknowledgement budget expire and the coordinator run.
    L.tick(L.cfg.limits.ack_timeout_micros * 3);
    L.tick();

    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->state, ReconState::ManualHold);
    CHECK_EQ(L.wire_count('H'), std::size_t{0});
    CHECK_EQ(L.lp_disk.status_requests_seen, std::size_t{0});
    CHECK(L.disk.audit.count(AuditKind::HoldOpened) >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-B-08", "P1", "a peer that refuses status requests does not cause a query loop") {
    Lab L;
    L.cfg.capabilities.order_status_request = true;
    L.cfg.finalize();
    L.lp_disk.capability_status_request = true;
    L.lp_disk.reject_status_requests = true;  // answers nothing
    L.lp_disk.suppress_reports = true;

    const auto a = L.must_submit_new("K1");
    L.drive();
    for (int i = 0; i < 12; ++i) L.tick(L.cfg.limits.ack_timeout_micros * 4);

    const ReconCase* c = L.case_for(a.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->state, ReconState::ManualHold);
    // The probe budget is three, so the wire must not show more than that.
    CHECK(L.wire_count('H') <= std::size_t{3});
    CHECK_INV(L);
}

SCENARIO("OR-B-09", "P0", "a MARKET order is refused, not converted to a limit") {
    Lab L;
    CommandEnvelope c = L.new_order("K1");
    c.ord_type = OrdType::Market;
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::UnsupportedOrdType);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-10", "P0", "a GTC order is refused, not silently turned into DAY") {
    Lab L;
    CommandEnvelope c = L.new_order("K1");
    c.tif = TimeInForce::GoodTillCancel;
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::UnsupportedTif);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-11", "P0", "an alias that maps to two symbols is refused before serialising") {
    Lab L;
    L.cfg.add_symbol_alias("EURUSD", "EUR/USD");
    L.cfg.add_symbol_alias("EURUSD", "GBP/USD");  // now ambiguous
    CommandEnvelope c = L.new_order("K1");
    c.symbol = "EURUSD";
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::AmbiguousSymbol);
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-12", "P0", "the wire symbol is exactly what the profile pins") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const std::string frame = L.session_disk.wire_log.at(0);
    CHECK(frame.find("55=EUR/USD\x01") != std::string::npos);
    CHECK(frame.find("55=EURUSD\x01") == std::string::npos);
    // An unmapped alias is not quietly accepted either.
    CommandEnvelope c = L.new_order("K2");
    c.symbol = "EURUSD";
    CHECK(L.submit(c).is_err());
    (void)a;
    CHECK_INV(L);
}

SCENARIO("OR-B-13", "P0", "a tick size change does not mutate a committed command") {
    Lab L;
    L.must_submit_new("K1", "100000", "1.08500");
    const std::string body_before = L.intent("K1")->fix_body;
    const Hash hash_before = L.intent("K1")->payload_hash;

    L.cfg.set_symbol_price_scale("EUR/USD", 4, 1);  // tick 0.00001 -> 0.0001
    CHECK_EQ(L.intent("K1")->fix_body, body_before);
    CHECK(L.intent("K1")->payload_hash == hash_before);

    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-14", "P0", "a quantity off the lot step is refused, not rounded") {
    Lab L;
    auto r = L.submit(L.new_order("K1", "1500"));  // step is 1000
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK(L.submit(L.new_order("K2", "2000")).is_ok());
    CHECK_EQ(L.intent("K2")->qty, 2000);
    CHECK_INV(L);
}

SCENARIO("OR-B-15", "P0", "an entitlement revoked before dispatch stops the send and alerts") {
    Lab L;
    L.must_submit_new("K1");
    L.cfg.revoke_entitlement(AccountId("ACC1"));
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{0});
    CHECK(!L.rt->alerts().empty());
    // And a fresh command for that account is refused outright.
    auto r = L.submit(L.new_order("K2"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::EntitlementRevoked);
    CHECK_INV(L);
}

SCENARIO("OR-B-16", "P0", "a configuration with no checksum fails closed at startup") {
    Lab L;
    L.cfg.checksum_present = false;
    CHECK(L.cfg.validate().is_err());
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ConfigInvalid);
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});

    // A tampered checksum is refused the same way.
    L.cfg.checksum_present = true;
    L.cfg.checksum.bytes[0] ^= 0xff;
    CHECK(L.cfg.validate().is_err());
    CHECK_INV(L);
}

SCENARIO("OR-B-17", "P0", "a production endpoint is blocked by the lab allowlist") {
    Lab L;
    for (Route& r : L.cfg.routes)
        if (r.account.str() == "ACC1") r.endpoint = "fix.real-broker.example.com:9443";
    ++L.cfg.route_config_version;
    L.cfg.finalize();

    CHECK(L.cfg.validate().is_err());
    L.boot();
    CHECK(!L.last_boot_ok);
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK(L.submit(L.new_order("K1")).is_err());
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-B-18", "P1", "a custom tag is only accepted when the profile tolerates it") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();

    // Not in the tolerated list: must-understand violation, quarantined.
    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.extra.push_back({9001, "vendor-value"});
    L.inject(s);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 0);
    CHECK_EQ(L.quarantine_depth(), std::size_t{1});
    // The raw frame is retained with its provenance.
    CHECK(!L.disk.quarantine.at(0).raw_hash.is_zero());

    // Now tolerate it and the same shape of report is accepted.
    L.cfg.order_profile.tolerated_custom_tags.push_back(9001);
    L.cfg.finalize();
    p02::ReportSpec s2 = L.lp->trade(a.cl_ord_id.str(), "EX2", 40000, 108500);
    s2.extra.push_back({9001, "vendor-value"});
    L.inject(s2);
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_INV(L);
}

SCENARIO("OR-B-19", "P1", "a route change applies to future chains, not to historical ones") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 100000, 108500);
    L.drive();
    CHECK_EQ(L.chain(a.chain_id)->status, OrdStatus::Filled);

    // Re-point the account; the finished chain keeps its own namespace.
    L.cfg.set_route_lp(AccountId("ACC1"), LpId("LP_SIM_02"), "LPSIM01");
    CHECK_EQ(L.chain(a.chain_id)->lp.str(), std::string("LP_SIM_01"));
    for (const auto& [k, ek] : L.disk.exec_keys) {
        (void)k;
        CHECK_EQ(ek.lp_namespace, std::string("LP_SIM_01"));
    }
    CHECK_INV(L);
}

SCENARIO("OR-B-20", "P0", "a session reset keeps the identifier and dedup scopes intact") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    std::vector<std::string> keys_before;
    for (const auto& [k, ek] : L.disk.exec_keys) {
        (void)ek;
        keys_before.push_back(k);
    }
    CHECK(!keys_before.empty());
    const std::string first_cl = a.cl_ord_id.str();

    // Day rollover: the FIX session resets its sequences and bumps its epoch.
    L.session->reconnect(true);
    CHECK(L.session_disk.epoch > 1);

    const auto b = L.must_submit_new("K2");
    L.drive();
    // A fresh identifier, not a reused one, and every dedup key from before the
    // reset is still in the ledger: the negotiated scope spans the rollover.
    CHECK_NE(b.cl_ord_id.str(), first_cl);
    for (const std::string& k : keys_before)
        CHECK(L.disk.exec_keys.find(k) != L.disk.exec_keys.end());
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK_INV(L);
}
