// Section 25 — the P0 fault-injection playbooks, PB-01 to PB-36.
//
// Each playbook is a named incident with an expected safe outcome. Several
// are the same fault the crash suite injects, approached from the
// operator's side: what does the system look like afterwards, and can the
// person on call tell what happened?
//
// Playbooks that need PostgreSQL replication, a second machine or real
// hardware are reported BLOCKED with the reason, as section 30 asks.
#include <thread>
#include <vector>

#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

void seed(Lab& l, std::int64_t qty = 100000) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", qty), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("PB-01", "P0", "a partial business transaction rolls back at every write boundary") {
    for (const char* point :
         {"tx.after_dedup_insert", "tx.after_journal_append", "tx.after_stream_head",
          "tx.after_order_projection", "tx.after_risk_projection", "tx.after_command_intent",
          "tx.after_outbox_insert", "tx.before_commit"}) {
        Lab l(std::string("pb-01-") + point);
        l.start();
        const std::size_t events = l.row_count("journal_events");

        fp::disarm_all();
        fp::arm(point, fp::Action::ReturnError);
        const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
        fp::disarm_all();

        CHECK_MSG(o.kind != CommitKind::Committed, std::string(point) + " committed anyway");
        CHECK_MSG(l.row_count("journal_events") == events,
                  std::string(point) + " left journal rows behind");
        CHECK_MSG(l.row_count("orders") == 0, std::string(point) + " left an order behind");
        CHECK_MSG(l.row_count("command_outbox") == 0, std::string(point) + " left an effect");
        expect_consistent(l, point);
    }
}

SCENARIO("PB-02", "P0", "an unknown COMMIT is resolved by key, not by retrying blindly") {
    Lab l("pb-02");
    l.start();
    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringCommit), fp::Action::ReturnError);
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();

    CHECK_EQ(o.kind, CommitKind::Unknown);
    CHECK(l.store().lookup(Lab::kTenant, Lab::kAccount, "K1").found);
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Replayed);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    expect_consistent(l, "PB-02");
}

SCENARIO("PB-03", "P0", "a crash after the durable commit before the API reply is one order") {
    Lab l("pb-03");
    l.start();
    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterCommit), fp::Action::Throw);
    try {
        // The fault fires after the transaction has committed, so it stands
        // for the process dying on the way back to the client rather than
        // for a failure the store could have handled. It escapes by design;
        // what the playbook is about is what is on disk afterwards.
        l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
        TT_FAIL("the fault did not fire");
    } catch (const fp::InjectedFault&) {
    }
    fp::disarm_all();

    l.restart();
    l.start();
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    expect_consistent(l, "PB-03");
}

SCENARIO("PB-04", "P0", "a claimed worker that dies before the send leaves it retryable") {
    Lab l("pb-04");
    seed(l, 1000);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeStableSend), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(0));
    l.dispatcher().reconcile_unknown(Lab::kTenant, l.link());
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "READY").size(), std::size_t(1));
    expect_consistent(l, "PB-04");
}

SCENARIO("PB-05", "P0", "a sequence persisted before the socket write is never re-minted") {
    Lab l("pb-05");
    seed(l, 1000);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterSeqPersisted), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    l.restart();
    l.start();
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(l.link().allocated_sequences(Lab::kSession), std::uint64_t(1));
    expect_consistent(l, "PB-05");
}

SCENARIO("PB-06", "P0", "a partial TCP write of a NewOrderSingle is UNKNOWN") {
    Lab l("pb-06");
    seed(l, 1000);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringSocketWrite), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "UNKNOWN").size(), std::size_t(1));
    CHECK_EQ(l.lp().total_writes(), std::size_t(0));
    expect_consistent(l, "PB-06");
}

SCENARIO("PB-07", "P0", "a kernel write that succeeds with a lost callback is UNKNOWN") {
    Lab l("pb-07");
    seed(l, 1000);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterSocketWrite), fp::Action::ReturnError);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    fp::disarm_all();

    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "UNKNOWN").size(), std::size_t(1));
    CHECK_EQ(l.lp().total_writes(), std::size_t(1));
    // JI-17: the successful write is not evidence of anything downstream.
    l.dispatcher().reconcile_unknown(Lab::kTenant, l.link());
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "HELD").size(), std::size_t(1));
    expect_consistent(l, "PB-07");
}

SCENARIO("PB-08", "P0", "a counterparty fill while the receipt is missing is a visible gap") {
    Lab l("pb-08");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    l.lp().record_trade("E-lost", 40000, 10000);

    CHECK_EQ(l.lp().traded_quantity(), std::int64_t(40000));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    // The discrepancy is discoverable, and the instance will not resume.
    l.restart();
    CHECK_EQ(l.start().phase, Phase::ReconOnly);
}

SCENARIO("PB-09", "P0", "a durable raw receipt that was never delivered is redelivered") {
    Lab l("pb-09");
    seed(l);
    const auto r = l.fill("O1", "E1", 40000, 10000, "K1");
    RawReceipt raw;
    raw.session_key = r.session_key;
    raw.session_epoch = r.session_epoch;
    raw.receipt_id = r.receipt_id;
    raw.raw = r.raw;
    raw.raw_hash = sha256(r.raw);
    l.link().persist_raw(raw);

    l.restart();
    l.start();
    CHECK_EQ(l.link().undelivered(Lab::kSession, 10).size(), std::size_t(1));
    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "PB-09");
}

SCENARIO("PB-10", "P0", "a crash after the inbox insert before the projection leaves neither") {
    Lab l("pb-10");
    seed(l);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterInboxInsert), fp::Action::ReturnError);
    const CommitOutcome o = l.deliver(l.fill("O1", "E1", 40000, 10000, "K1"), false);
    fp::disarm_all();

    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(0));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));
    expect_consistent(l, "PB-10");
}

SCENARIO("PB-11", "P0", "a business commit before the delivery ack yields one fill") {
    Lab l("pb-11");
    seed(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");
    RawReceipt raw;
    raw.session_key = r.session_key;
    raw.session_epoch = r.session_epoch;
    raw.receipt_id = r.receipt_id;
    raw.raw = r.raw;
    raw.raw_hash = sha256(r.raw);
    l.link().persist_raw(raw);
    l.lp_executes(r);

    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Committed);
    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeDeliveryAck), fp::Action::ReturnError);
    l.service().ack_delivery(r);
    fp::disarm_all();

    l.restart();
    l.start();
    // Redelivered after the restart; still one economic trade.
    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Replayed);
    CHECK_EQ(l.scalar("SELECT COUNT(*) FROM fills WHERE semantic_exec_id='E100';"),
             std::int64_t(1));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.lp().economic_trade_count("E100"), std::size_t(1));
    expect_consistent(l, "PB-11");
}

SCENARIO("PB-12", "P0", "a snapshot that crashed before its marker is ignored") {
    Lab l("pb-12");
    seed(l);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));

    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeSnapshotVerified), fp::Action::ReturnError);
    Version at = 0;
    std::string why;
    l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why);
    fp::disarm_all();

    SnapshotRecord snap;
    CHECK(!l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));
    CHECK(replay_from_empty(l.store(), Lab::kTenant, order_stream("O1")).ok);
    expect_consistent(l, "PB-12");
}

SCENARIO("PB-15", "P0", "a duplicate receipt with the same key and different payload") {
    Lab l("pb-15");
    seed(l);
    auto r = l.fill("O1", "E1", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(r).kind, CommitKind::Committed);
    auto forged = r;
    forged.raw += "|altered";
    CHECK_EQ(l.deliver(forged).kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "PB-15");
}

SCENARIO("PB-16", "P0", "a semantic ExecID collision with altered economics is quarantined") {
    Lab l("pb-16");
    seed(l);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 40000, 10000, "K1")).kind, CommitKind::Committed);
    CHECK_EQ(l.deliver(l.fill("O1", "E1", 40000, 99999, "K1")).kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").exec_value, std::int64_t(40000) * 10000);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));
    expect_consistent(l, "PB-16");
}

SCENARIO("PB-17", "P0", "cancel, fill and a crash interleave without losing quantity") {
    Lab l("pb-17");
    seed(l);
    l.service().submit_cancel(l.cancel("K2", "O1"), l.token());
    l.deliver(l.fill("O1", "E1", 30000, 10000, "K1"));

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterRiskProjection), fp::Action::ReturnError);
    l.deliver(l.terminal("O1", "CANCELED", "K2"), false);
    fp::disarm_all();

    l.restart();
    l.start();
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(30000));
    CHECK(l.order("O1").pending_cancel);
    CHECK_EQ(l.deliver(l.terminal("O1", "CANCELED", "K2")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").status, OrdStatus::Canceled);
    CHECK_EQ(l.risk().exposure, std::int64_t(30000));
    CHECK_EQ(l.risk().reserved, std::int64_t(0));
    expect_consistent(l, "PB-17");
}

SCENARIO("PB-18", "P0", "replace, partial fill and a crash keep the ledger exact") {
    Lab l("pb-18");
    seed(l);
    l.deliver(l.fill("O1", "E1", 20000, 10000, "K1"));
    l.service().submit_replace(l.replace("K2", "O1", 60000), l.token());

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterJournalAppend), fp::Action::ReturnError);
    l.deliver(l.terminal("O1", "REPLACED", "K2"), false);
    fp::disarm_all();

    l.restart();
    l.start();
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(100000));
    CHECK(l.order("O1").pending_replace);
    CHECK_EQ(l.deliver(l.terminal("O1", "REPLACED", "K2")).kind, CommitKind::Committed);
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(60000));
    CHECK_EQ(l.order("O1").leaves_qty, std::int64_t(40000));
    CHECK_EQ(l.risk().utilised(), std::int64_t(60000));
    expect_consistent(l, "PB-18");
}

SCENARIO("PB-19", "P0", "two accounts cannot double-spend one firm budget") {
    Lab l("pb-19", /*limit=*/100000);
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 60000), l.token()).kind,
             CommitKind::Committed);
    auto second = l.new_order("K2", "O2", 60000);
    second.account = "A2";
    CHECK_EQ(l.service().submit_new(second, l.token()).kind, CommitKind::Rejected);
    CHECK(l.risk().utilised() <= std::int64_t(100000));
    expect_consistent(l, "PB-19");
}

SCENARIO("PB-24", "P0", "a corrupted snapshot falls back to a valid journal") {
    Lab l("pb-24");
    seed(l);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    Version at = 0;
    std::string why;
    CHECK_EQ(l.snapshots().checkpoint(Lab::kTenant, order_stream("O1"), &at, &why),
             StoreError::None);
    l.store().db().exec("UPDATE snapshots SET serialized_state='{}' WHERE stream_id='ord:O1';");

    SnapshotRecord snap;
    CHECK(!l.snapshots().newest_usable(Lab::kTenant, order_stream("O1"), &snap));
    // Full replay still works, which is the fallback section 13 allows.
    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK_MSG(r.ok, r.detail);
}

SCENARIO("PB-25", "P0", "a journal gap or broken chain quarantines rather than skipping") {
    Lab l("pb-25");
    seed(l);
    l.deliver(l.fill("O1", "E1", 10000, 10000, "K1"));
    l.deliver(l.fill("O1", "E2", 10000, 10000, "K1"));
    l.store().db().exec("DROP TRIGGER journal_events_append_only_delete;");
    l.store().db().exec(
        "DELETE FROM journal_events WHERE stream_id='ord:O1' AND stream_version=2;");

    const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, order_stream("O1"));
    CHECK(!r.ok);
    CHECK(r.has(Incident::StreamVersionGap));
    const RecoveryReport rep = l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK_EQ(rep.phase, Phase::Quarantine);
    CHECK(!l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
}

SCENARIO("PB-27", "P0", "many concurrent commands never exceed the budget or duplicate a key") {
    Lab l("pb-27", /*limit=*/20000);
    l.start();
    std::vector<std::thread> threads;
    for (int i = 0; i < 40; ++i) {
        threads.emplace_back([&, i] {
            const std::string k = "K" + std::to_string(i % 20);
            const std::string c = "O" + std::to_string(i % 20);
            l.service().submit_new(l.new_order(k, c, 1000), l.token());
        });
    }
    for (std::thread& t : threads) t.join();

    CHECK(l.risk().utilised() <= std::int64_t(20000));
    // Twenty distinct keys were offered twice each; at most twenty orders.
    CHECK(l.row_count("orders") <= std::size_t(20));
    CHECK_EQ(l.row_count("orders"), l.row_count("command_intents"));
    CHECK_EQ(l.row_count("orders"), l.row_count("command_outbox"));
    expect_consistent(l, "PB-27");
}

SCENARIO("PB-29", "P0", "a poison outbox row is held and the rest still drain") {
    Lab l("pb-29", 1000000);
    l.start();
    for (int i = 0; i < 4; ++i) {
        l.service().submit_new(
            l.new_order("K" + std::to_string(i), "O" + std::to_string(i), 1000), l.token());
    }
    // One key is already bound at P01 to different content: it can never
    // be sent, and it must not stop the others.
    CHECK(l.link().send_once(Lab::kSession, "K2", sha256("poison"), "poison").bound);

    const DispatchStats st = l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    CHECK_EQ(st.held, std::size_t(1));
    CHECK_EQ(st.bound, std::size_t(3));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "HELD").size(), std::size_t(1));
    CHECK_EQ(l.store().outbox_rows(Lab::kTenant, "BOUND").size(), std::size_t(3));
    expect_consistent(l, "PB-29");
}

SCENARIO("PB-31", "P0", "an operator cannot release a hold without recorded approval") {
    Lab l("pb-31");
    seed(l);
    l.deliver(l.fill("O-unknown", "E9", 1000, 10000, ""));
    const CaseId id = l.store().open_cases(Lab::kTenant).front().case_id;
    CHECK_EQ(l.store().resolve_case(Lab::kTenant, id, "RESOLVED", ""), StoreError::Rejected);
    l.supervisor().provide_external_evidence("");
    CHECK_EQ(l.supervisor().verify_and_replay(RecoveryMode::PromotedStandby).phase,
             Phase::ReconOnly);
}

SCENARIO("PB-32", "P0", "a malicious payload cannot cross a tenant boundary") {
    Lab l("pb-32");
    seed(l);
    auto hostile = l.fill("O1", "E-hostile", 40000, 10000, "K1");
    hostile.tenant = "t2";  // a report claiming to be another tenant's
    l.service().apply_report(hostile, l.token());
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    CHECK_EQ(l.row_count("fills"), std::size_t(0));
    expect_consistent(l, "PB-32");
}

SCENARIO("PB-33", "P0", "retention pruning is refused while anything is unresolved") {
    Lab l("pb-33");
    seed(l);
    RetentionPolicy p;
    const RetentionResult r = prune(l.store(), Lab::kTenant, p);
    CHECK(r.refused);
    CHECK(!r.refusal_reason.empty());
    CHECK(l.row_count("journal_events") > 0);
    expect_consistent(l, "PB-33");
}

SCENARIO("PB-34", "P0", "a mixed chaos run still replays deterministically") {
    // A short, seeded stand-in for the 48-hour soak. The soak itself is
    // declared BLOCKED below; what this proves is that the same mixture of
    // faults leaves a history that replays.
    Lab l("pb-34", 1000000);
    l.start();
    unsigned seed_value = 20261030;
    auto next = [&]() {
        seed_value = seed_value * 1664525u + 1013904223u;
        return seed_value;
    };

    for (int i = 0; i < 30; ++i) {
        const std::string k = "K" + std::to_string(i);
        const std::string c = "O" + std::to_string(i);
        const unsigned roll = next() % 5;
        fp::disarm_all();
        if (roll == 0) {
            fp::arm(std::string(fp::point::kAfterOutboxInsert), fp::Action::ReturnError);
        } else if (roll == 1) {
            fp::arm(std::string(fp::point::kDuringCommit), fp::Action::ReturnError);
        }
        l.service().submit_new(l.new_order(k, c, 1000), l.token());
        fp::disarm_all();

        if ((next() % 3) == 0) l.dispatcher().dispatch_once(Lab::kTenant, l.token());
        if (l.order(c).exists) {
            l.deliver(l.fill(c, "E" + std::to_string(i), 500, 10000, k));
        }
    }
    fp::disarm_all();
    expect_consistent(l, "PB-34");

    l.restart();
    const RecoveryReport rep = l.start();
    CHECK(rep.storage_valid);
    CHECK(rep.journal_valid);
    CHECK(rep.projections_valid);
}

SCENARIO("PB-35", "P0", "repeated failover keeps dispatch fenced to one owner") {
    Lab l("pb-35");
    seed(l, 1000);
    FenceToken last = l.token();
    for (int i = 0; i < 5; ++i) {
        FenceToken next_token = 0;
        CHECK_EQ(l.store().acquire_fence(Lab::kTenant, Lab::kDomain,
                                         "gateway-" + std::to_string(i), 60000, &next_token),
                 StoreError::None);
        CHECK(next_token > last);
        // The previous owner can no longer write.
        CHECK_NE(l.service()
                     .submit_new(
                         l.new_order("Kx" + std::to_string(i), "Ox" + std::to_string(i), 1000),
                         last)
                     .kind,
                 CommitKind::Committed);
        last = next_token;
    }
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    expect_consistent(l, "PB-35");
}

SCENARIO("PB-36", "P0", "a disaster restore without counterparty history cannot resume") {
    Lab l("pb-36");
    seed(l);
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());
    const RestoreDrillResult d =
        restore_drill(l.store(), Lab::kTenant, l.dir().file("arc"), l.dir().file("r.db"));
    CHECK_MSG(d.verified, d.detail);
    CHECK(!d.dispatch_enabled);

    SqliteAtomicStore::Options opt;
    opt.path = l.dir().file("r.db");
    opt.create_if_missing = false;
    opt.clock = &l.clock();
    std::unique_ptr<SqliteAtomicStore> restored;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &restored, &why), StoreError::None);
    RecoverySupervisor::Options ropt;
    ropt.tenant = Lab::kTenant;
    ropt.domains = {Lab::kDomain};
    RecoverySupervisor sup(*restored, ropt);
    CHECK_EQ(sup.verify_and_replay(RecoveryMode::RestoredFromBackup).phase, Phase::ReconOnly);
    CHECK(!sup.may_admit(Lab::kTenant, Lab::kDomain));
}

SCENARIO_BLOCKED("PB-13", "P0", "an acked commit lost on an asynchronous standby failover",
                 "there is no replica in this lab. The engine-side consequence, that a "
                 "promotion holds in RECON_ONLY, is asserted by HA-02 in test_storage_ha.")

SCENARIO_BLOCKED("PB-14", "P0", "split brain during promotion",
                 "two PostgreSQL nodes are needed to produce it. The fencing that makes it "
                 "safe at the resource boundary is covered by G14-13 and PB-35.")

SCENARIO_BLOCKED("PB-20", "P0", "PITR to a point before an external trade",
                 "point-in-time recovery is PostgreSQL WAL replay. The rule that a restored "
                 "instance may not dispatch is asserted by PB-36.")

SCENARIO_BLOCKED("PB-21", "P0", "disk full during a WAL and outbox transaction",
                 "filling a real volume is not something this suite may do to the host. The "
                 "typed Full error is classified fail-closed, and all-or-nothing behaviour "
                 "under an injected write failure is PB-01.")

SCENARIO_BLOCKED("PB-22", "P0", "corrupted WAL or physical pages",
                 "page-level corruption of a PostgreSQL cluster. The file-level equivalent "
                 "this lab can produce is covered by G15-04.")

SCENARIO_BLOCKED("PB-23", "P0", "a missing archive range during an offline restore",
                 "the journal-segment equivalent is BK-03. A real WAL archive gap needs "
                 "PostgreSQL continuous archiving.")

SCENARIO_BLOCKED("PB-26", "P0", "sequence ids allocated out of commit order",
                 "a PostgreSQL sequence handing out ids before COMMIT. Nothing here derives "
                 "ordering from a row id, which G12-06 demonstrates.")

SCENARIO_BLOCKED("PB-28", "P0", "a schema upgrade breaks a historical reducer",
                 "the pinning that prevents it is asserted by G18-03 and G11-09. Running an "
                 "actual migration across versions needs a release pipeline, not a test "
                 "binary.")

SCENARIO_BLOCKED("PB-30", "P0", "a synchronous standby unavailable during COMMIT",
                 "requires synchronous replication. The engine refuses to call a weaker "
                 "durability profile durable, which G15-02 asserts.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_playbooks"); }
