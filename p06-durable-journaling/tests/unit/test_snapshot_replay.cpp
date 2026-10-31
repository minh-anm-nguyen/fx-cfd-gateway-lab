// G11 — Snapshots, replay correctness and reducer versioning.
//
// Section 13's rule in one line: a snapshot is a shortcut, never a
// substitute. Each case below removes one of the things that make the
// shortcut safe and checks that recovery falls back to the journal rather
// than trusting what is in front of it.
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, int fills = 3) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
    for (int i = 0; i < fills; ++i) {
        l.deliver(l.fill("O1", "E" + std::to_string(i), 10000, 10000 + i, "K1"));
    }
}

SCENARIO("G11-01", "P0", "replay from empty reproduces the projection exactly") {
    Lab l("g11-01");
    seed(l);
    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
    CHECK_EQ(r.events_applied, std::size_t(4));  // accept + three fills

    const IOrderReducer* red = l.store().reducers().order(reducer_version::kOrderV1);
    CHECK_EQ(canonical_encode(red->to_canon(l.order("O1"))), canonical_encode(r.state));
    CHECK_EQ(to_hex(r.state_hash),
             to_hex(l.store().head(Lab::kTenant, order_stream("O1")).state_hash));
}

SCENARIO("G11-02", "P0", "a verified snapshot plus the tail equals a full replay") {
    Lab l("g11-02");
    seed(l, 2);
    Version at = 0;
    std::string why;
    CHECK_MSG(l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why) ==
                  StoreError::None,
              why);

    // More history after the checkpoint.
    l.deliver(l.fill("O1", "E-late", 5000, 10000, "K1"));

    SnapshotRecord snap;
    CHECK(l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));
    const ReplayResult from_snap =
        replay_stream(l.store(), Lab::kTenant, order_stream("O1"), &snap);
    const ReplayResult from_empty =
        replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));

    CHECK_MSG(from_snap.ok, from_snap.detail);
    CHECK_MSG(from_empty.ok, from_empty.detail);
    // Section 26.3 item 8: the two paths must agree, byte for byte.
    CHECK_EQ(canonical_encode(from_snap.state), canonical_encode(from_empty.state));
    CHECK_EQ(to_hex(from_snap.state_hash), to_hex(from_empty.state_hash));
    CHECK_EQ(from_snap.started_from, at);
    CHECK(from_snap.events_applied < from_empty.events_applied);
}

SCENARIO("G11-03", "P0", "an unverified snapshot is not used") {
    Lab l("g11-03");
    seed(l, 2);
    Version at = 0;
    CHECK_EQ(l.snapshots().create(Lab::kTenant, order_stream("O1"), &at), StoreError::None);
    // Created but never marked usable: W22 is exactly this gap.
    SnapshotRecord snap;
    CHECK(!l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));

    CHECK(l.snapshots().load(Lab::kTenant, order_stream("O1"), at, &snap));
    CHECK(!snap.verified);
    const ReplayResult r = replay_stream(l.store(), Lab::kTenant, order_stream("O1"), &snap);
    CHECK(!r.ok);
    CHECK(r.has(Incident::SnapshotUnverified));
}

SCENARIO("G11-04", "P0", "a crash before the usable marker leaves a snapshot recovery ignores") {
    Lab l("g11-04");
    seed(l, 2);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeSnapshotVerified), fp::Action::ReturnError);
    Version at = 0;
    std::string why;
    const StoreError e = l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why);
    fp::disarm_all();
    CHECK_NE(e, StoreError::None);

    SnapshotRecord snap;
    CHECK(!l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));
    // The journal alone still recovers the stream.
    CHECK(replay_from_empty(l.store(), Lab::kTenant, order_stream("O1")).ok);
}

SCENARIO("G11-05", "P0", "a snapshot whose content was altered is refused") {
    Lab l("g11-05");
    seed(l, 2);
    Version at = 0;
    std::string why;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why);

    CHECK_EQ(l.store().db().exec(
                 "UPDATE snapshots SET serialized_state='{\"exists\":false,\"v\":\"p03.v1\"}' "
                 "WHERE stream_id='ord:O1';"),
             StoreError::None);

    SnapshotRecord snap;
    // newest_usable rechecks the digest rather than trusting the flag.
    CHECK(!l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));
    CHECK(l.snapshots().load(Lab::kTenant, order_stream("O1"), at, &snap));
    const ReplayResult r = replay_stream(l.store(), Lab::kTenant, order_stream("O1"), &snap);
    CHECK(!r.ok);
    CHECK(r.has(Incident::SnapshotChecksumFailure));
}

SCENARIO("G11-06", "P0", "a snapshot ahead of the journal head is refused") {
    Lab l("g11-06");
    seed(l, 2);
    Version at = 0;
    std::string why;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why);

    // Pretend the head fell behind, as a partial restore would leave it.
    CHECK_EQ(l.store().db().exec(
                 "UPDATE stream_heads SET current_version=1 WHERE stream_id='ord:O1';"),
             StoreError::None);

    SnapshotRecord snap;
    CHECK(l.snapshots().load(Lab::kTenant, order_stream("O1"), at, &snap));
    const ReplayResult r = replay_stream(l.store(), Lab::kTenant, order_stream("O1"), &snap);
    CHECK(!r.ok);
    CHECK(r.has(Incident::SnapshotAheadOfJournal));
}

SCENARIO("G11-07", "P0", "a snapshot whose journal anchor does not match is refused") {
    Lab l("g11-07");
    seed(l, 2);
    Version at = 0;
    std::string why;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why);

    CHECK_EQ(l.store().db().exec("UPDATE snapshots SET journal_anchor_hash='" +
                                 std::string(64, 'b') + "' WHERE stream_id='ord:O1';"),
             StoreError::None);
    std::string reason;
    CHECK_NE(l.snapshots().verify(Lab::kTenant, order_stream("O1"), at, &reason),
             StoreError::None);
    CHECK(!reason.empty());
}

SCENARIO("G11-08", "P0", "a version gap in the journal fails closed") {
    Lab l("g11-08");
    seed(l, 3);
    // Remove a middle event the way a bad migration or a partial restore
    // would. The triggers are defence in depth, so they come off first.
    l.store().db().exec("DROP TRIGGER journal_events_append_only_delete;");
    CHECK_EQ(l.store().db().exec(
                 "DELETE FROM journal_events WHERE stream_id='ord:O1' AND stream_version=3;"),
             StoreError::None);

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK(!r.ok);
    CHECK(r.has(Incident::StreamVersionGap));

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    CHECK(!v.ok());
    CHECK(v.frozen_domains.count(Lab::kDomain) == 1);
}

SCENARIO("G11-09", "P0", "a reducer version named on an event is the one replay uses") {
    Lab l("g11-09");
    seed(l, 1);
    // The stream was written by p03.v1 and must stay on it. p03.v2 tracks
    // an extra field, so replaying old events with it would produce a
    // different state hash: JI-37 is why the version is pinned per event.
    std::vector<JournalRecord> page;
    l.store().read_stream(Lab::kTenant, order_stream("O1"), 1, 0, &page);
    for (const JournalRecord& r : page) {
        CHECK_EQ(r.reducer_version, std::string(reducer_version::kOrderV1));
    }
    const IOrderReducer* v1 = l.store().reducers().order(reducer_version::kOrderV1);
    const IOrderReducer* v2 = l.store().reducers().order(reducer_version::kOrderV2);
    CHECK(v1 != nullptr);
    CHECK(v2 != nullptr);
    // The two produce different canonical states for the same order, which
    // is what makes the pinning observable rather than theoretical.
    CHECK_NE(canonical_encode(v1->to_canon(l.order("O1"))),
             canonical_encode(v2->to_canon(l.order("O1"))));
}

SCENARIO("G11-10", "P0", "an event naming an unknown reducer fails closed") {
    Lab l("g11-10");
    seed(l, 1);
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    CHECK_EQ(l.store().db().exec(
                 "UPDATE journal_events SET reducer_version='p03.v99' WHERE "
                 "stream_id='ord:O1' AND stream_version=1;"),
             StoreError::None);

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK(!r.ok);
    // The hash covers reducer_version, so this is caught as a broken chain
    // before the registry is even consulted. Either way it fails closed.
    CHECK(r.has(Incident::HashChainBroken) || r.has(Incident::ReducerUnavailable));
}

SCENARIO("G11-11", "P0", "mixing reducer versions inside one stream is refused") {
    Lab l("g11-11");
    seed(l, 1);
    const Version base = l.store().head(Lab::kTenant, order_stream("O1")).version;

    TxPlan p;
    p.tenant = Lab::kTenant;
    p.tx_id = "tx-mixed-reducer";
    p.fence_domain = Lab::kDomain;
    p.fence_token = l.token();
    AppendEvent e;
    e.stream = order_stream("O1");
    e.expected_version = base;
    e.event_id = "e-v2";
    e.event_type = std::string(ev::kFillApplied);
    e.reducer_version = std::string(reducer_version::kOrderV2);  // the stream is on v1
    e.ordinal = 0;
    e.payload = Payload().s("exec_id", "E-x").i("last_qty", 1000).i("last_px", 10000).build();
    p.events.push_back(std::move(e));

    const CommitOutcome o = l.store().apply(p);
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK_EQ(o.error, StoreError::SchemaMismatch);
    expect_consistent(l, "G11-11");
}

SCENARIO("G11-12", "P0", "snapshotting does not change the journal or the projection") {
    Lab l("g11-12");
    seed(l, 2);
    const OrderState before = l.order("O1");
    const std::size_t events = l.row_count("journal_events");

    Version at = 0;
    std::string why;
    CHECK_EQ(l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why),
             StoreError::None);
    CHECK(l.order("O1") == before);
    CHECK_EQ(l.row_count("journal_events"), events);
    expect_consistent(l, "G11-12");
}

SCENARIO("G11-13", "P0", "a snapshot of a risk domain replays identically") {
    Lab l("g11-13");
    seed(l, 2);
    Version at = 0;
    std::string why;
    CHECK_MSG(l.snapshots().checkpoint(Lab::kTenant, risk_stream(Lab::kDomain), &at, &why) ==
                  StoreError::None,
              why);
    l.deliver(l.fill("O1", "E-after", 5000, 10000, "K1"));

    SnapshotRecord snap;
    CHECK(l.snapshots().newest_usable(Lab::kTenant, risk_stream(Lab::kDomain), &snap));
    const ReplayResult a =
        replay_stream(l.store(), Lab::kTenant, risk_stream(Lab::kDomain), &snap);
    const ReplayResult b = replay_from_empty(l.store(), Lab::kTenant, risk_stream(Lab::kDomain));
    CHECK_MSG(a.ok, a.detail);
    CHECK_MSG(b.ok, b.detail);
    CHECK_EQ(canonical_encode(a.state), canonical_encode(b.state));
}

SCENARIO("G11-14", "P0", "replay performs no writes and emits no effects") {
    Lab l("g11-14");
    seed(l, 2);
    const std::size_t events = l.row_count("journal_events");
    const std::size_t outbox = l.row_count("command_outbox");
    const std::size_t writes = l.lp().total_writes();

    for (const StreamId& s : l.store().list_streams(Lab::kTenant)) {
        CHECK(replay_from_empty(l.store(), Lab::kTenant, s).ok);
    }
    CHECK_EQ(l.row_count("journal_events"), events);
    CHECK_EQ(l.row_count("command_outbox"), outbox);
    CHECK_EQ(l.lp().total_writes(), writes);
}

SCENARIO("G11-15", "P1", "an older verified snapshot is still usable") {
    Lab l("g11-15");
    seed(l, 2);
    Version first = 0;
    std::string why;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &first, &why);
    l.deliver(l.fill("O1", "E-x", 5000, 10000, "K1"));
    Version second = 0;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &second, &why);
    CHECK(second > first);

    SnapshotRecord older;
    CHECK(l.snapshots().load(Lab::kTenant, order_stream("O1"), first, &older));
    CHECK(older.verified);
    const ReplayResult r = replay_stream(l.store(), Lab::kTenant, order_stream("O1"), &older);
    CHECK_MSG(r.ok, r.detail);
    CHECK_EQ(to_hex(r.state_hash),
             to_hex(l.store().head(Lab::kTenant, order_stream("O1")).state_hash));
}

SCENARIO_BLOCKED("G11-16", "P0", "a torn snapshot spanning several streams",
                 "a snapshot here covers one stream and is anchored to one committed event, "
                 "so the multi-stream version vector section 13 warns about does not exist to "
                 "tear. Introducing one in order to test it would be adding the hazard.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_snapshot_replay"); }
