// Section A of the what-if matrix: command admission, idempotency, risk gate.
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

SCENARIO("OR-A-01", "P0", "a valid first New commits one intent and one outbox row") {
    Lab L;
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_ok());
    CHECK_EQ(r.value().delivery, DeliveryState::AcceptedDurable);
    // Section 1.4: accepted-durable is a local fact, not an LP acknowledgement.
    CHECK_EQ(r.value().business, BusinessOutcome::Unknown);
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_EQ(L.rt->original_sends(), std::size_t{0});  // nothing sent yet
    CHECK_EQ(L.disk.audit.count(AuditKind::CommandAdmitted), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-02", "P0", "a hundred retries of the same command, including after a restart") {
    Lab L;
    const auto first = L.must_submit_new("K1");
    for (int i = 0; i < 100; ++i) {
        auto again = L.submit(L.new_order("K1"));
        CHECK(again.is_ok());
        CHECK(again.value().idempotent_replay);
        CHECK_EQ(again.value().cl_ord_id.str(), first.cl_ord_id.str());
        CHECK_EQ(again.value().chain_id.str(), first.chain_id.str());
        CHECK_EQ(again.value().route.str(), first.route.str());
    }
    L.crash();
    L.boot();
    auto after = L.submit(L.new_order("K1"));
    CHECK(after.is_ok());
    CHECK_EQ(after.value().cl_ord_id.str(), first.cl_ord_id.str());
    // One intent, one outbox row, one chain: the retries added nothing.
    CHECK_EQ(L.disk.intents.size(), std::size_t{1});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_EQ(L.disk.chains.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-03", "P0", "the same command id with a different quantity is a conflict") {
    Lab L;
    const auto first = L.must_submit_new("K1", "100000");
    auto second = L.submit(L.new_order("K1", "90000"));
    CHECK(second.is_err());
    CHECK_EQ(second.error(), RoutingError::IdempotencyConflict);
    // The accepted intent is untouched.
    const CommandIntent* stored = L.intent("K1");
    CHECK(stored != nullptr);
    CHECK_EQ(stored->qty, 100000);
    CHECK_EQ(stored->cl_ord_id.str(), first.cl_ord_id.str());
    CHECK_EQ(L.disk.outbox.size(), std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-04", "P0", "the same command id with a different price is a conflict") {
    Lab L;
    L.must_submit_new("K1", "100000", "1.08500");
    auto second = L.submit(L.new_order("K1", "100000", "1.08600"));
    CHECK(second.is_err());
    CHECK_EQ(second.error(), RoutingError::IdempotencyConflict);
    const CommandIntent* stored = L.intent("K1");
    CHECK_EQ(stored->price_ticks, 108500);
    // The wire payload was not silently rewritten.
    CHECK(stored->fix_body.find("44=1.08500") != std::string::npos);
    CHECK_INV(L);
}

SCENARIO("OR-A-05", "P0", "a tenant cannot submit for an account it does not own") {
    Lab L;
    CommandEnvelope c = L.new_order("K1");
    c.principal = Lab::trader("T2", "bob");  // T2 owns ACC2, not ACC1
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::AuthzDenied);
    // Zero reservations and zero routing decisions.
    CHECK_EQ(L.risk->reserve_calls(), std::size_t{0});
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.audit.count(AuditKind::RouteDecided), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-A-06", "P0", "a zero quantity is refused before any identifier is allocated") {
    Lab L;
    auto r = L.submit(L.new_order("K1", "0"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.cl_ord_index.size(), std::size_t{0});
    CHECK_EQ(L.disk.next_cl_ord_seq, std::uint64_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-07", "P0", "a negative quantity never reaches the reducer") {
    Lab L;
    auto r = L.submit(L.new_order("K1", "-1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(L.disk.chains.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-A-08", "P0", "a quantity past int64 fails closed instead of wrapping") {
    Lab L;
    auto r = L.submit(L.new_order("K1", "99999999999999999999"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    // And a value that fits but is absurd is still outside the profile bound.
    auto r2 = L.submit(L.new_order("K2", "9223372036854775807"));
    CHECK(r2.is_err());
    CHECK_INV(L);
}

SCENARIO("OR-A-09", "P0", "a zero or negative limit price is refused") {
    Lab L;
    CHECK(L.submit(L.new_order("K1", "100000", "0")).is_err());
    CHECK(L.submit(L.new_order("K2", "100000", "-1")).is_err());
    CHECK(L.submit(L.new_order("K3", "100000", "0.00000")).is_err());
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-A-10", "P0", "a price finer than the tick is refused, never rounded") {
    Lab L;
    auto r = L.submit(L.new_order("K1", "100000", "1.085001"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::ValidationError);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    // The on-tick neighbour is accepted, so the refusal was about precision.
    CHECK(L.submit(L.new_order("K2", "100000", "1.08501")).is_ok());
    CHECK_INV(L);
}

SCENARIO("OR-A-11", "P0", "a symbol outside the allowlist does not pick a fallback route") {
    Lab L;
    CommandEnvelope c = L.new_order("K1");
    c.symbol = "XZY/QQQ";
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::UnsupportedSymbol);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-A-12", "P0", "a caller cannot cancel a chain it does not own, and learns nothing") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    // The LP has bound an OrderID by now; the denial must not disclose it.
    CHECK(L.chain(a.chain_id)->lp_order_id.has_value());

    CommandEnvelope c = L.cancel_order("C1", a.chain_id);
    c.principal = Lab::trader("T2", "bob");
    c.account = AccountId("ACC2");
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::AuthzDenied);
    // No cancel intent, no F on the wire.
    CHECK_EQ(L.wire_count('F'), std::size_t{0});
    CHECK(L.intent("C1", "T2") == nullptr);
    CHECK_INV(L);
}

SCENARIO("OR-A-13", "P0", "fifty retries of one cancel produce one cancel request") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const auto c1 = L.must_submit_cancel("C1", a.chain_id);
    for (int i = 0; i < 50; ++i) {
        auto again = L.submit(L.cancel_order("C1", a.chain_id));
        CHECK(again.is_ok());
        CHECK_EQ(again.value().cl_ord_id.str(), c1.cl_ord_id.str());
    }
    L.drive();
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    // One cancel intent exists and the chain has exactly one pending cancel.
    std::size_t cancels = 0;
    for (const auto& [k, in] : L.disk.intents) {
        (void)k;
        if (in.kind == CommandKind::Cancel) ++cancels;
    }
    CHECK_EQ(cancels, std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-14", "P0", "a replace naming a stale aggregate version is refused") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    const StateVersion live = L.chain(a.chain_id)->version;
    CommandEnvelope c = L.replace_order("R1", a.chain_id, "80000", "1.08490");
    c.expected_version = live + 7;
    auto r = L.submit(c);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::StaleVersion);
    CHECK(!L.chain(a.chain_id)->pending_replace.has_value());
    CHECK_EQ(L.wire_count('G'), std::size_t{0});
    CHECK_INV(L);
}

SCENARIO("OR-A-15", "P0", "a risk rejection means no order leaves and the refusal is audited") {
    Lab L;
    L.risk->set_verdict(p05::Verdict::Reject);
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::RiskRejected);
    L.drive();
    CHECK_EQ(L.session_disk.wire_log.size(), std::size_t{0});
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK(L.disk.audit.count(AuditKind::CommandRejected) >= std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-16", "P0", "an unavailable risk service fails closed and never self-approves") {
    Lab L;
    L.risk->set_verdict(p05::Verdict::Unavailable);
    auto r = L.submit(L.new_order("K1"));
    CHECK(r.is_err());
    CHECK_EQ(r.error(), RoutingError::RiskUnavailable);
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});

    // A replace that increases exposure is blocked for the same reason.
    L.risk->set_verdict(p05::Verdict::Approve);
    const auto a = L.must_submit_new("K2");
    L.drive();
    L.risk->set_verdict(p05::Verdict::Unavailable);
    auto up = L.submit(L.replace_order("R1", a.chain_id, "150000", "1.08500"));
    CHECK(up.is_err());
    CHECK_EQ(up.error(), RoutingError::RiskUnavailable);
    CHECK_INV(L);
}

SCENARIO("OR-A-17", "P1", "a duplicated risk call reuses one reservation") {
    Lab L;
    L.must_submit_new("K1");
    const std::size_t before = L.risk->distinct_reservations();
    for (int i = 0; i < 10; ++i) L.submit(L.new_order("K1"));
    CHECK_EQ(L.risk->distinct_reservations(), before);
    CHECK_EQ(before, std::size_t{1});
    CHECK_INV(L);
}

SCENARIO("OR-A-18", "P0", "an expired risk verdict is rechecked before dispatch, not reused") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    // The verdict TTL is two seconds in the lab profile; wait past it before the
    // dispatcher gets a turn.
    L.advance(L.cfg.limits.risk_verdict_ttl_micros + 1000);
    const DispatchStats d = L.rt->dispatch_pending();
    CHECK_EQ(d.sent, std::size_t{0});
    CHECK_EQ(d.held, std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{0});
    CHECK_EQ(L.intent("K1")->delivery, DeliveryState::ManualHold);
    CHECK(L.chain(a.chain_id)->local_only);
    CHECK_INV(L);
}

SCENARIO("OR-A-19", "P0", "a cancel still works while risk is down, and New does not") {
    Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.risk->set_verdict(p05::Verdict::Unavailable);

    // The cancel reduces exposure, so it is admitted and made durable.
    auto c = L.submit(L.cancel_order("C1", a.chain_id));
    CHECK(c.is_ok());
    CHECK_EQ(c.value().delivery, DeliveryState::AcceptedDurable);
    // A New in the same breath is still refused: no bypass (R-I14).
    auto n = L.submit(L.new_order("K2"));
    CHECK(n.is_err());
    CHECK_EQ(n.error(), RoutingError::RiskUnavailable);
    L.drive();
    CHECK_EQ(L.wire_count('F'), std::size_t{1});
    CHECK_EQ(L.wire_count('D'), std::size_t{1});  // only the original K1
    CHECK_INV(L);
}

SCENARIO("OR-A-20", "P1", "a flood of commands against a full disk accepts none of them") {
    Lab L;
    L.disk.full = true;  // ENOSPC
    for (int i = 0; i < 10000; ++i) {
        auto r = L.submit(L.new_order("K" + std::to_string(i)));
        CHECK(r.is_err());
    }
    // Nothing is durable, nothing is queued, and the router said so.
    CHECK_EQ(L.disk.intents.size(), std::size_t{0});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{0});
    CHECK_EQ(L.rt->mode(), RouterMode::Halt);
    CHECK_INV(L);
}
