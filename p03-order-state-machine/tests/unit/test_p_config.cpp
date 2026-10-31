// Group P -- config, version upgrades, compatibility and lifecycle controls
// (design doc 18.P).
//
// The recurring rule: an order is judged under the policy it was accepted
// under, and a configuration that would make trading unsafe is refused before
// any order is accepted rather than discovered halfway through one.
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("P-01", "P0", "a quantity-step reload keeps the pinned scale on an active order") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    const uint32_t pinned = L.state(id).accepted_versions[0].scale_id;

    Policy next = L.policy();
    next.symbols["EURUSD"].qty_step = 500;
    next.symbols["EURUSD"].scale_id = 21;
    next.policy_version = 2;
    CHECK(L.engine().reload_policy(next).ok());

    // The existing version is untouched.
    CHECK_EQ(L.state(id).accepted_versions[0].scale_id, pinned);
    CHECK_EQ(L.state(id).accepted_versions[0].policy_version, uint32_t{1});
    // A new order is admitted under the new policy version.
    const EngineResult fresh = L.submit(cmd_new("O2", "100500", "1.08500"));
    CHECK(fresh.ok());
    CHECK(L.submit(new_ack("O2")).ok());
    CHECK_EQ(L.state(fresh.id).accepted_versions[0].policy_version, uint32_t{2});
    CHECK_EQ(L.state(fresh.id).accepted_versions[0].scale_id, uint32_t{21});
}

SCENARIO("P-02", "P0", "a tick reload during a pending replace does not pre-apply the new scale") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000")).ok());
    const int64_t desired_before = L.state(id).pending->desired_price_ticks;

    Policy next = L.policy();
    next.symbols["EURUSD"].tick_denominator = 1000;
    next.symbols["EURUSD"].scale_id = 22;
    next.policy_version = 3;
    CHECK(L.engine().reload_policy(next).ok());

    CHECK_EQ(L.state(id).accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(L.state(id).pending->desired_price_ticks, desired_before);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});

    // Only the explicit acknowledgement installs a version, and it carries the
    // new scale.
    CHECK(L.submit(replaced("R1", "O1").oqty("120000")).ok());
    CHECK_EQ(L.state(id).accepted_versions[1].scale_id, uint32_t{22});
    CHECK_EQ(L.state(id).accepted_versions[0].scale_id, uint32_t{1});
}

SCENARIO("P-03", "P0", "the v2 correction engine cannot be activated without its migration") {
    Policy p = default_lab_policy();
    p.corrections_engine_v2_enabled = true;
    CHECK_EQ(p.validate().code, ConfigErrorCode::CorrectionsEnabledWithoutMigration);

    Lab L;
    CHECK(L.working("O1").valid());
    Policy bad = L.policy();
    bad.corrections_engine_v2_enabled = true;
    const ConfigError e = L.engine().reload_policy(bad);
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::CorrectionsEnabledWithoutMigration);
    CHECK(!L.policy().corrections_engine_v2_enabled);
}

SCENARIO("P-04", "P0", "the identity namespace cannot change while dedup rows exist") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.store().applied_key_count() > 0);

    Policy renamed = L.policy();
    renamed.business_namespace = "FIRM-DEMO:2026-10-31";
    const ConfigError refused = L.engine().reload_policy(renamed, false);
    CHECK(!refused.ok());
    CHECK_EQ(L.policy().business_namespace, std::string("FIRM-DEMO:2026-10-30"));

    // The old key still protects the fill from a resend.
    expect_decision(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
                    Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});

    // Declaring the index migrated is the operator's explicit act.
    CHECK(L.engine().reload_policy(renamed, true).ok());
    CHECK_EQ(L.engine().policy().business_namespace, std::string("FIRM-DEMO:2026-10-31"));
}

SCENARIO("P-05", "P0", "a business-day rollover does not let an unresolved key be reused") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    CHECK(L.state(n.id).reconciliation_required);

    LocalCommand tomorrow = cmd_new("O1", "100000");
    tomorrow.business_day = "2026-10-31";
    expect_reject(L.submit(tomorrow), Reason::IdCollision);

    // Once resolved, the next day may reuse it.
    SystemEvent resolved = sys(SystemKind::ReconcileResolved, "O1");
    resolved.evidence_provided = true;
    resolved.resolved_status = "REJECTED";
    CHECK(L.submit(resolved).ok());
    CHECK(L.submit(tomorrow).ok());
    CHECK_EQ(L.engine().chain_count(), size_t{2});
}

SCENARIO("P-06", "P0", "an unknown event schema version halts replay, casting nothing") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    L.store().set_schema_version(7);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SchemaMismatch);
    CHECK(!rep.trading_allowed);
    CHECK_EQ(rep.records_replayed, size_t{0});
    CHECK_EQ(rep.chains_recovered, size_t{0});
    (void)id;
}

SCENARIO("P-07", "P0", "an unknown snapshot version is ignored in favour of a safe replay") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const uint64_t good = L.state(id).state_hash();

    Snapshot foreign;
    foreign.complete = true;
    foreign.schema_version = 42;
    foreign.journal_watermark = 0;
    foreign.aggregates[id.value] = OrderAggregate{};  // deliberately wrong content
    L.store().install_snapshot(foreign);

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SnapshotRejected);
    CHECK(rep.trading_allowed);
    CHECK_EQ(L.state(id).state_hash(), good);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("P-08", "P1", "a conformant and an adversarial peer give the same state for valid events") {
    // The same valid trace from a strict peer and from one that interleaves
    // noise. The valid events produce identical business state; the noise is
    // quarantined and changes no quantity.
    const auto strict = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
        CHECK(L.submit(trade("X2", "O1").lastq("60000").lastpx("1.08495").cum("100000").leaves("0"))
                  .ok());
        return std::pair<int64_t, size_t>{L.state(id).cum_qty_units, L.state(id).ledger.size()};
    };
    const auto adversarial = [] {
        Lab L;
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
        expect_decision(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
                        Decision::DuplicateNoop);
        expect_decision(L.submit(trade("BAD", "O1").lastq("0").lastpx("1.08490")),
                        Decision::Quarantined);
        expect_decision(L.submit(trade("HUGE", "O1").lastq("900000").lastpx("1.08490")),
                        Decision::Quarantined);
        CHECK(L.submit(trade("X2", "O1").lastq("60000").lastpx("1.08495").cum("100000").leaves("0"))
                  .ok());
        return std::pair<int64_t, size_t>{L.state(id).cum_qty_units, L.state(id).ledger.size()};
    };
    CHECK(strict() == adversarial());
}

SCENARIO("P-09", "P0", "a FIX session restart with a sequence reset does not reset business state") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const size_t keys = L.store().applied_key_count();

    // P01 reconnects with ResetSeqNumFlag; epoch two starts numbering again.
    CHECK(L.submit(trade("X2", "O1").epoch(2).seq(1).lastq("20000").lastpx("1.08491").cum("60000"))
              .ok());
    // The identical report, field for field, with only the transport fields
    // different. Omitting LeavesQty would make it a different report, and the
    // conflict detector would be right to say so.
    expect_decision(L.submit(trade("X1", "O1")
                                 .epoch(2)
                                 .seq(2)
                                 .resend()
                                 .lastq("40000")
                                 .lastpx("1.08490")
                                 .cum("40000")
                                 .leaves("60000")),
                    Decision::DuplicateNoop);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{60000});
    CHECK_EQ(a.ledger.size(), size_t{2});
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK(L.store().applied_key_count() > keys);
}

SCENARIO("P-10", "P0", "an upgrade during a pending cancel reloads the request, not a new one") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    const std::string key = "route-cancel|FIRM|2026-10-30|C1";
    CHECK_EQ(L.engine().sink().count_key(key), size_t{1});

    // The binary restarts: every index is rebuilt from the journal.
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::Clean);

    const OrderAggregate& a = L.state(id);
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->kind, CommandKind::Cancel);
    CHECK_EQ(a.pending->cl_ord_id, std::string("C1"));
    CHECK_EQ(a.lifecycle(), Lifecycle::PendingCancel);
    // No second cancel request was produced by the restart.
    size_t real_cancels = 0;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.idempotency_key == key) ++real_cancels;
    }
    CHECK_EQ(real_cancels, size_t{1});
    // And the request is still the only one admissible.
    expect_reject(L.submit(cmd_cancel("C2", "O1")), Reason::PendingMutation);
}

SCENARIO("P-11", "P0", "an upgrade during a pending replace applies nothing prematurely") {
    Lab L;
    const OrderId id = L.working("O1", "100000", "1.08500");
    CHECK(L.submit(cmd_replace("R1", "O1", "120000", "1.08600")).ok());
    CHECK(L.submit(pending_replace("R1")).ok());

    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->desired_qty_units, int64_t{120000});
    CHECK_EQ(a.pending->desired_price_ticks, int64_t{108600});
    CHECK(a.peer_pending_replace);
}

SCENARIO("P-12", "P0", "two symbols sharing a scale id fail startup validation loudly") {
    Policy p = default_lab_policy();
    p.symbols["GBPUSD"] = p.symbols["EURUSD"];  // same scale_id
    const ConfigError e = p.validate();
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::DuplicateScaleId);
    CHECK(!e.detail.empty());
}

SCENARIO("P-13", "P0", "a zero quantity step fails startup validation") {
    Policy p = default_lab_policy();
    p.symbols["EURUSD"].qty_step = 0;
    const ConfigError e = p.validate();
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::ZeroQtyStep);

    p.symbols["EURUSD"].qty_step = -1000;
    CHECK_EQ(p.validate().code, ConfigErrorCode::ZeroQtyStep);
}

SCENARIO("P-14", "P0", "an unsafe tick configuration fails startup validation") {
    Policy zero_den = default_lab_policy();
    zero_den.symbols["EURUSD"].tick_denominator = 0;
    CHECK_EQ(zero_den.validate().code, ConfigErrorCode::BadTick);

    Policy zero_num = default_lab_policy();
    zero_num.symbols["EURUSD"].tick_numerator = 0;
    CHECK_EQ(zero_num.validate().code, ConfigErrorCode::BadTick);

    Policy overflowing = default_lab_policy();
    overflowing.symbols["EURUSD"].tick_denominator = 1000000000000000000LL;
    CHECK_EQ(overflowing.validate().code, ConfigErrorCode::TickOverflow);

    Policy negative = default_lab_policy();
    negative.symbols["EURUSD"].tick_numerator = -1;
    CHECK_EQ(negative.validate().code, ConfigErrorCode::BadTick);
}

SCENARIO("P-15", "P1", "a peer identity change requires an explicit namespace migration") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // Changing the peer id silently would make "LP1|X1" and "lp1|X1" two keys
    // for one execution, so the namespace change is gated the same way.
    Policy recased = L.policy();
    recased.peer_id = "lp1";
    recased.business_namespace = "FIRM-DEMO:recased";
    CHECK(!L.engine().reload_policy(recased, false).ok());
    CHECK_EQ(L.policy().peer_id, std::string("LP1"));
    expect_decision(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")),
                    Decision::DuplicateNoop);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("P-16", "P0", "retention shorter than the resend horizon is refused") {
    Policy p = default_lab_policy();
    p.dedup_retention_units = 5;
    p.fix_resend_horizon_units = 100;
    CHECK_EQ(p.validate().code, ConfigErrorCode::RetentionBelowResendHorizon);

    Lab L;
    CHECK(!L.engine().reload_policy(p).ok());
    // And the eviction action itself is refused without a proven watermark.
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    const size_t before = L.store().applied_key_count();
    CHECK(!L.store().evict_applied_keys_before(999999, false));
    CHECK_EQ(L.store().applied_key_count(), before);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("P-17", "P0", "a manual override without evidence and dual control is denied") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    CHECK_EQ(L.lc(n.id), Lifecycle::ReconciliationRequired);
    const uint64_t before = L.state(n.id).state_hash();

    SystemEvent bare = sys(SystemKind::ManualOverride, "O1");
    bare.resolved_status = "CANCELED";
    expect_reject(L.submit(bare), Reason::NotAuthorized);
    CHECK_EQ(L.state(n.id).state_hash(), before);

    SystemEvent evidence_only = bare;
    evidence_only.evidence_provided = true;
    expect_reject(L.submit(evidence_only), Reason::NotAuthorized);

    SystemEvent signed_only = bare;
    signed_only.dual_control_signed = true;
    expect_reject(L.submit(signed_only), Reason::NotAuthorized);
    CHECK_EQ(L.state(n.id).state_hash(), before);
    CHECK(L.state(n.id).done_reason != DoneReason::Canceled);

    // Both together, and only then.
    SystemEvent authorized = bare;
    authorized.evidence_provided = true;
    authorized.dual_control_signed = true;
    CHECK(L.submit(authorized).ok());
    CHECK_EQ(L.lc(n.id), Lifecycle::Canceled);
}

SCENARIO("P-18", "P0", "a manual replay of an old cursor leaves the state stable") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    CHECK(L.submit(trade("X2", "O1").seq(50).lastq("20000").lastpx("1.08491").cum("60000")).ok());
    const uint64_t stable = L.state(id).state_hash();

    // An operator re-feeds the session from an earlier cursor.
    for (int pass = 0; pass < 3; ++pass) {
        expect_decision(L.submit(trade("X1", "O1")
                                     .seq(1)
                                     .lastq("40000")
                                     .lastpx("1.08490")
                                     .cum("40000")
                                     .leaves("60000")),
                        Decision::DuplicateNoop);
        expect_decision(
            L.submit(trade("X2", "O1").seq(2).lastq("20000").lastpx("1.08491").cum("60000")),
            Decision::DuplicateNoop);
        CHECK_EQ(L.state(id).state_hash(), stable);
    }
    CHECK_EQ(L.state(id).ledger.size(), size_t{2});
    CHECK_EQ(oracle(L.store(), id).applied_trades, size_t{2});
}

SCENARIO("P-19", "P0", "a rollback to code that cannot read the journal halts startup") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    // The journal was written at schema 2; this binary understands 1.
    L.store().set_schema_version(2);
    Policy older = L.policy();
    older.event_schema_version = 1;
    CHECK(L.engine().reload_policy(older).ok());

    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::SchemaMismatch);
    CHECK(!rep.trading_allowed);
    CHECK(!L.engine().trading_allowed());
    CHECK(rep.detail.find("schema") != std::string::npos);
    (void)id;
}

SCENARIO("P-20", "P1", "the same standard trace gives identical state across two engines") {
    const auto run = [](uint32_t policy_version) {
        Policy p = default_lab_policy();
        p.policy_version = policy_version;
        Lab L(p);
        const OrderId id = L.working("O1", "100000");
        CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
        CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
        CHECK(L.submit(canceled("C1").cum("40000")).ok());
        return L.state(id);
    };
    const OrderAggregate a = run(1);
    const OrderAggregate b = run(1);
    CHECK_EQ(a.state_hash(), b.state_hash());

    // A declared policy-version difference shows up in the version chain and
    // nowhere else, which is what makes the difference reviewable.
    const OrderAggregate c = run(9);
    CHECK_EQ(c.cum_qty_units, a.cum_qty_units);
    CHECK_EQ(c.lifecycle(), a.lifecycle());
    CHECK_EQ(c.accepted_versions[0].policy_version, uint32_t{9});
    CHECK_NE(c.state_hash(), a.state_hash());
}
