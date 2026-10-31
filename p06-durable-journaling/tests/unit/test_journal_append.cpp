// G01 — Event journal: append-only, integrity and version ordering.
//
// These exercise the store directly rather than through the service, so the
// assertions are about the journal contract itself: contiguous versions, a
// chain that cannot be reordered, and an append that refuses rather than
// guesses.
#include "lab.hpp"

using namespace lab;

namespace {

// A minimal plan that appends one risk event to the lab's domain stream.
TxPlan risk_plan(Lab& l, const std::string& tx, Version expected, std::int64_t amount,
                 const std::string& reservation, const std::string& chain = "O1") {
    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = tx;
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = risk_stream(Lab::kDomain);
    e.expected_version = expected;
    e.event_id = tx + ":0";
    e.event_type = std::string(ev::kRiskReserved);
    e.reducer_version = std::string(reducer_version::kRiskV1);
    e.ordinal = 0;
    e.payload = Payload()
                    .s("reservation_id", reservation)
                    .s("account", Lab::kAccount)
                    .s("chain", chain)
                    .i("amount", amount)
                    .build();
    p.events.push_back(std::move(e));
    return p;
}

SCENARIO("G01-01", "P0", "first append to an empty stream lands at version 1") {
    Lab l("g01-01");
    l.start();
    // The lab bootstraps the risk domain, so a fresh order stream is the
    // genuinely empty one.
    const StreamId s = order_stream("O-new");
    CHECK_EQ(l.store().head(Lab::kTenant, s).version, Version(0));

    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O-new", 1000), l.token());
    CHECK_EQ(o.kind, CommitKind::Committed);

    const StreamHead h = l.store().head(Lab::kTenant, s);
    CHECK_EQ(h.version, Version(1));
    CHECK(h.exists);

    std::vector<JournalRecord> page;
    CHECK_EQ(l.store().read_stream(Lab::kTenant, s, 1, 0, &page), StoreError::None);
    CHECK_EQ(page.size(), std::size_t(1));
    CHECK_EQ(page[0].version, Version(1));
    // The first event chains onto the zero digest, not onto nothing.
    CHECK_EQ(to_hex(page[0].prev_state_hash), to_hex(zero_digest()));
    expect_consistent(l, "G01-01");
}

SCENARIO("G01-02", "P0", "version 2 chains onto version 1") {
    Lab l("g01-02");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    CHECK_EQ(l.store().apply(risk_plan(l, "tx-a", base, 100, "r1")).kind, CommitKind::Committed);
    CHECK_EQ(l.store().apply(risk_plan(l, "tx-b", base + 1, 200, "r2")).kind,
             CommitKind::Committed);

    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, s, base + 1, 0, &page);
    CHECK_EQ(page.size(), std::size_t(2));
    CHECK_EQ(page[1].version, base + 2);
    // The declared predecessor is exactly the previous row's chain value.
    CHECK_EQ(to_hex(page[1].prev_state_hash), to_hex(page[0].state_hash));
    expect_consistent(l, "G01-02");
}

SCENARIO("G01-04", "P0", "two writers at the same expected version produce one winner") {
    Lab l("g01-04");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    const CommitOutcome first = l.store().apply(risk_plan(l, "tx-w1", base, 100, "r1"));
    const CommitOutcome second = l.store().apply(risk_plan(l, "tx-w2", base, 100, "r2"));

    CHECK_EQ(first.kind, CommitKind::Committed);
    // The loser is told it conflicted. It is emphatically NOT appended at
    // whatever version happens to be current now.
    CHECK_EQ(second.kind, CommitKind::Conflict);
    CHECK_EQ(l.store().head(Lab::kTenant, s).version, base + 1);
    expect_consistent(l, "G01-04");
}

SCENARIO("G01-05", "P0", "an append that skips a version is refused") {
    Lab l("g01-05");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    const CommitOutcome o = l.store().apply(risk_plan(l, "tx-skip", base + 2, 100, "r1"));
    CHECK_EQ(o.kind, CommitKind::Conflict);
    CHECK_EQ(l.store().head(Lab::kTenant, s).version, base);
    // No gap was created, so replay still verifies.
    expect_consistent(l, "G01-05");
}

SCENARIO("G01-06", "P0", "replaying the same command is a no-op, not a second version") {
    Lab l("g01-06");
    l.start();
    const auto req = l.new_order("K1", "O1", 1000);

    const CommitOutcome a = l.service().submit_new(req, l.token());
    CHECK_EQ(a.kind, CommitKind::Committed);
    const Version after_first = l.store().head(Lab::kTenant, order_stream("O1")).version;

    const CommitOutcome b = l.service().submit_new(req, l.token());
    CHECK_EQ(b.kind, CommitKind::Replayed);
    CHECK_EQ(l.store().head(Lab::kTenant, order_stream("O1")).version, after_first);
    CHECK_EQ(l.row_count("journal_events"), std::size_t(3));  // limit + reserve + accept
    expect_consistent(l, "G01-06");
}

SCENARIO("G01-07", "P0", "the same command id with new content conflicts") {
    Lab l("g01-07");
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Committed);

    // Same key, different quantity. JI-05: this is a conflict, never a
    // second order and never a silent overwrite.
    auto altered = l.new_order("K1", "O1", 2000);
    const CommitOutcome o = l.service().submit_new(altered, l.token());
    CHECK_EQ(o.kind, CommitKind::Conflict);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(1000));
    expect_consistent(l, "G01-07");
}

SCENARIO("G01-08", "P0", "an UPDATE against a committed journal row is refused") {
    Lab l("g01-08");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    // The append-only trigger is defence in depth, not a defence against a
    // superuser; section 8 says so and the test asserts only what it does.
    const StoreError e = l.store().db().exec(
        "UPDATE journal_events SET payload='{}' WHERE stream_id='ord:O1';");
    CHECK_NE(e, StoreError::None);
    expect_consistent(l, "G01-08");
}

SCENARIO("G01-09", "P0", "a DELETE against a committed journal row is refused") {
    Lab l("g01-09");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    const std::size_t before = l.row_count("journal_events");

    const StoreError e =
        l.store().db().exec("DELETE FROM journal_events WHERE stream_id='ord:O1';");
    CHECK_NE(e, StoreError::None);
    CHECK_EQ(l.row_count("journal_events"), before);
    expect_consistent(l, "G01-09");
}

SCENARIO("G01-15", "P0", "a head whose hash disagrees with the last event freezes the domain") {
    Lab l("g01-15");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    // Tamper with the head only. Nothing in the journal changed, so only a
    // verifier that compares the two can see it.
    CHECK_EQ(l.store().db().exec("UPDATE stream_heads SET state_hash='" + std::string(64, 'a') +
                                 "' WHERE stream_id='ord:O1';"),
             StoreError::None);

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.has(Incident::HeadMismatch));
    // And the risk domain the order draws on stops admitting (JI-39).
    CHECK(v.frozen_domains.count(Lab::kDomain) == 1);
}

SCENARIO("G01-10b", "P0", "an edited payload breaks the chain at that version") {
    Lab l("g01-10b");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    // Bypass the trigger the way a privileged actor would: drop it first.
    // This is exactly the threat section 18 says a hash chain detects and
    // access control does not.
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    CHECK_EQ(l.store().db().exec(
                 "UPDATE journal_events SET payload='{\"amount\":1,\"reservation_id\":\"x\"}' "
                 "WHERE stream_id='ord:O1' AND stream_version=1;"),
             StoreError::None);

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK(!r.ok);
    CHECK(r.has(Incident::HashChainBroken));
}

SCENARIO("G01-13", "P0", "the same event id in two tenants does not cross over") {
    Lab l("g01-13");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    // A second tenant with its own domain, its own fence and the same ids.
    FenceToken t2 = 0;
    CHECK_EQ(l.store().acquire_fence("t2", Lab::kDomain, "other", 60000, &t2), StoreError::None);
    GatewayService::Options gopt;  // no gate: this is a store-level check
    GatewayService svc2(l.store(), gopt);
    CHECK_EQ(svc2.set_risk_limit("t2", Lab::kDomain, 1000000, t2).kind, CommitKind::Committed);

    NewOrderRequest r2 = l.new_order("K1", "O1", 7777);
    r2.tenant = "t2";
    CHECK_EQ(svc2.submit_new(r2, t2).kind, CommitKind::Committed);

    // Each tenant sees only its own order (JI-33).
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(1000));
    OrderState other;
    CHECK(l.store().load_order("t2", "O1", &other));
    CHECK_EQ(other.order_qty, std::int64_t(7777));

    expect_consistent(l, "G01-13 tenant 1");
    const VerifyReport v2 = verify_all(l.store(), "t2");
    CHECK(v2.ok());
}

SCENARIO("G01-14", "P0", "the same stream id in two tenants keeps separate hashes") {
    Lab l("g01-14");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    FenceToken t2 = 0;
    l.store().acquire_fence("t2", Lab::kDomain, "other", 60000, &t2);
    GatewayService::Options gopt;
    GatewayService svc2(l.store(), gopt);
    svc2.set_risk_limit("t2", Lab::kDomain, 1000000, t2);
    NewOrderRequest r2 = l.new_order("K1", "O1", 1000);
    r2.tenant = "t2";
    svc2.submit_new(r2, t2);

    const StreamHead a = l.store().head(Lab::kTenant, order_stream("O1"));
    const StreamHead b = l.store().head("t2", order_stream("O1"));
    CHECK_EQ(a.version, b.version);
    // Same events, same versions, different tenants: the tenant is part of
    // the hash preimage, so the chains must differ.
    CHECK_NE(to_hex(a.state_hash), to_hex(b.state_hash));
}

SCENARIO("G01-17", "P0", "events of one transaction may be built in any order") {
    Lab l("g01-17");
    l.start();
    const StreamId rs = risk_stream(Lab::kDomain);
    const StreamId os = order_stream("O1");
    const Version rbase = l.store().head(Lab::kTenant, rs).version;

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-unordered";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();

    // Deliberately pushed with the higher ordinal first.
    AppendEvent accept;
    accept.stream = os;
    accept.expected_version = 0;
    accept.event_id = "e-accept";
    accept.event_type = std::string(ev::kOrderAccepted);
    accept.reducer_version = std::string(reducer_version::kOrderV1);
    accept.ordinal = 1;
    accept.payload = Payload()
                         .s("chain", "O1")
                         .s("account", Lab::kAccount)
                         .s("cl_ord_id", "c1")
                         .s("symbol", "EURUSD")
                         .s("side", "BUY")
                         .s("command_id", "K1")
                         .s("risk_domain", Lab::kDomain)
                         .i("order_qty", 500)
                         .i("limit_px", 10000)
                         .build();
    p.events.push_back(std::move(accept));

    AppendEvent reserve;
    reserve.stream = rs;
    reserve.expected_version = rbase;
    reserve.event_id = "e-reserve";
    reserve.event_type = std::string(ev::kRiskReserved);
    reserve.reducer_version = std::string(reducer_version::kRiskV1);
    reserve.ordinal = 0;
    reserve.payload = Payload()
                          .s("reservation_id", "res:O1")
                          .s("account", Lab::kAccount)
                          .s("chain", "O1")
                          .i("amount", 500)
                          .build();
    p.events.push_back(std::move(reserve));

    CHECK_EQ(l.store().apply(p).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(500));
    CHECK_EQ(l.risk().reserved, std::int64_t(500));
    expect_consistent(l, "G01-17");
}

SCENARIO("G01-17b", "P0", "two events in one transaction cannot share an ordinal") {
    Lab l("g01-17b");
    l.start();
    TxPlan p = risk_plan(l, "tx-dup-ord", l.store().head(Lab::kTenant,
                                                          risk_stream(Lab::kDomain)).version,
                         100, "r1");
    AppendEvent second = p.events[0];
    second.event_id = "other";
    second.payload = Payload()
                         .s("reservation_id", "r2")
                         .s("account", Lab::kAccount)
                         .s("chain", "O1")
                         .i("amount", 100)
                         .build();
    second.expected_version = p.events[0].expected_version + 1;
    second.ordinal = 0;  // collision
    p.events.push_back(std::move(second));

    const CommitOutcome o = l.store().apply(p);
    CHECK_EQ(o.kind, CommitKind::Rejected);
    expect_consistent(l, "G01-17b");
}

SCENARIO("G01-18", "P1", "a backward clock jump does not change ordering") {
    Lab l("g01-18");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    l.clock().advance(10000);
    CHECK_EQ(l.store().apply(risk_plan(l, "tx-later", base, 100, "r1")).kind,
             CommitKind::Committed);
    // Wall clock steps back by an hour between the two appends.
    l.clock().set_wall(l.clock().now_ms() - 3600000);
    CHECK_EQ(l.store().apply(risk_plan(l, "tx-earlier", base + 1, 100, "r2")).kind,
             CommitKind::Committed);

    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, s, base + 1, 0, &page);
    CHECK_EQ(page.size(), std::size_t(2));
    // created_at went backwards; stream_version did not, and replay follows
    // stream_version (section 8 note 8).
    CHECK(page[1].created_at < page[0].created_at);
    CHECK(page[1].version > page[0].version);
    expect_consistent(l, "G01-18");
}

SCENARIO("G01-20", "P0", "a stream at the maximum version refuses admission") {
    Lab l("g01-20");
    l.start();
    // Drive the head to the ceiling directly: reaching it by appending is
    // not possible in a test, and the behaviour at the boundary is what
    // matters, not the journey.
    CHECK_EQ(l.store().db().exec(
                 "UPDATE stream_heads SET current_version=9223372036854775806 WHERE "
                 "stream_id='rsk:D1';"),
             StoreError::None);

    const CommitOutcome o =
        l.store().apply(risk_plan(l, "tx-ceiling", 9223372036854775806ull, 100, "r1"));
    // Refused rather than wrapped or reused.
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK_EQ(l.store().head(Lab::kTenant, risk_stream(Lab::kDomain)).version,
             Version(9223372036854775806ull));
}

SCENARIO("G01-03", "P0", "an uncommitted append is invisible and leaves no gap") {
    Lab l("g01-03");
    l.start();
    const StreamId s = risk_stream(Lab::kDomain);
    const Version base = l.store().head(Lab::kTenant, s).version;

    // A plan that fails inside the transaction: the reservation exceeds the
    // domain limit, so the reducer refuses after the manifest row was
    // already written.
    const CommitOutcome o = l.store().apply(risk_plan(l, "tx-too-big", base, 99999999, "r-big"));
    CHECK_EQ(o.kind, CommitKind::Quarantined);

    // Nothing survives: no manifest, no event, no head movement.
    CHECK_EQ(l.store().head(Lab::kTenant, s).version, base);
    CHECK_EQ(l.scalar("SELECT COUNT(*) FROM journal_transactions WHERE tx_id='tx-too-big';"),
             std::int64_t(0));
    expect_consistent(l, "G01-03");
}

SCENARIO_BLOCKED("G01-19b", "P0", "an audit row altered by a privileged actor is detected",
                 "covered by test_security SEC-AUDIT-01 against the audit hash chain; this id "
                 "duplicates it and is reported once rather than twice")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_journal_append"); }
