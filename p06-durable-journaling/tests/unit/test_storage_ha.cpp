// G15 and G17 — Storage durability, and replication/failover policy.
//
// This is the group where the gap between the design doc and this lab is
// widest, and the honest thing is to say exactly where the line falls.
//
// PostgreSQL's WAL, its checkpointer, its replication and its PITR are
// PostgreSQL's to get right, and there is no PostgreSQL here. What belongs
// to P06, and is tested below, is the decision logic layered on top: refuse
// to call a weak profile durable, quarantine on corruption instead of
// repairing, and never resume outbound traffic after a promotion or a
// restore without independent evidence.
//
// Everything that needs a real second node, a real disk or a real power
// cut is reported BLOCKED with the reason, not quietly passed.
#include <filesystem>
#include <fstream>

#include "lab.hpp"

using namespace lab;

namespace {

SCENARIO("G15-01", "P0", "the pinned durability profile is what the engine opens with") {
    Lab l("g15-01");
    l.start();
    const Db::DurabilityProfile p = l.store().db().durability();
    CHECK_EQ(p.journal_mode, std::string("wal"));
    CHECK_EQ(p.synchronous, std::string("FULL"));
    CHECK(p.safe);
}

SCENARIO("G15-02", "P0", "a weaker profile is refused as a durability claim") {
    tt::TempDir dir("g15-02");
    for (const char* sync : {"OFF", "NORMAL"}) {
        SqliteAtomicStore::Options opt;
        opt.path = dir.file(std::string("weak-") + sync + ".db");
        opt.synchronous = sync;
        std::unique_ptr<SqliteAtomicStore> store;
        std::string why;
        CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::None);
        const Db::DurabilityProfile p = store->db().durability();
        CHECK_MSG(!p.safe, std::string("synchronous=") + sync + " must not count as durable");

        // And the supervisor will not go READY on it.
        RecoverySupervisor::Options ropt;
        ropt.tenant = Lab::kTenant;
        ropt.domains = {Lab::kDomain};
        RecoverySupervisor sup(*store, ropt);
        const RecoveryReport rep = sup.verify_and_replay(RecoveryMode::ProcessRestart);
        CHECK_EQ(rep.phase, Phase::Faulted);
        CHECK(rep.has(Incident::DurabilityProfileUnsafe));
        CHECK(!rep.dispatch_enabled);
    }
}

SCENARIO("G15-03", "P0", "a missing database is not silently recreated") {
    tt::TempDir dir("g15-03");
    SqliteAtomicStore::Options opt;
    opt.path = dir.file("absent.db");
    opt.create_if_missing = false;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    // An engine that starts from an empty journal looks healthy while
    // having lost every committed event. It refuses instead.
    CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::IoError);
    CHECK(!why.empty());
}

SCENARIO("G15-04", "P0", "a corrupted file is quarantined, not repaired") {
    tt::TempDir dir("g15-04");
    const std::string path = dir.file("corrupt.db");
    {
        Lab seedlab("g15-04-seed");
        seedlab.start();
        seedlab.service().submit_new(seedlab.new_order("K1", "O1", 1000), seedlab.token());
        std::error_code ec;
        std::filesystem::copy_file(seedlab.db_path(), path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    // Overwrite the header, which is the bluntest corruption there is.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(f.good());
        f.seekp(0);
        const std::string junk(64, '\xAA');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    SqliteAtomicStore::Options opt;
    opt.path = path;
    opt.create_if_missing = false;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    const StoreError e = SqliteAtomicStore::open(opt, &store, &why);
    if (e == StoreError::None) {
        // Some corruptions only surface on the first read. Either way the
        // verifier must refuse, and recovery must not go READY.
        const VerifyReport v = verify_all(*store, Lab::kTenant);
        CHECK(!v.ok());
    } else {
        CHECK(is_fail_closed(e));
    }
}

SCENARIO("G15-05", "P0", "a database from another schema version is refused") {
    tt::TempDir dir("g15-05");
    const std::string path = dir.file("future.db");
    {
        SqliteAtomicStore::Options opt;
        opt.path = path;
        std::unique_ptr<SqliteAtomicStore> store;
        std::string why;
        CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::None);
        CHECK_EQ(store->db().exec(
                     "UPDATE schema_meta SET value='99' WHERE key='schema_version';"),
                 StoreError::None);
    }
    SqliteAtomicStore::Options opt;
    opt.path = path;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(opt, &store, &why), StoreError::SchemaMismatch);
    CHECK(why.find("99") != std::string::npos);
}

SCENARIO("G15-06", "P0", "a committed transaction survives reopening the files") {
    Lab l("g15-06");
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Committed);
    const OrderState before = l.order("O1");
    const Digest head = l.store().head(Lab::kTenant, order_stream("O1")).state_hash;

    l.restart();
    l.start();
    CHECK(l.order("O1") == before);
    CHECK_EQ(to_hex(l.store().head(Lab::kTenant, order_stream("O1")).state_hash), to_hex(head));
    expect_consistent(l, "G15-06");
}

SCENARIO("G15-07", "P0", "an integrity check failure stops the supervisor before READY") {
    Lab l("g15-07");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    // Break the chain so the journal no longer verifies.
    l.store().db().exec("DROP TRIGGER journal_events_append_only_update;");
    l.store().db().exec("UPDATE journal_events SET state_hash='" + std::string(64, 'd') +
                        "' WHERE stream_id='ord:O1' AND stream_version=1;");

    const RecoveryReport rep = l.supervisor().verify_and_replay(RecoveryMode::ProcessRestart);
    CHECK_EQ(rep.phase, Phase::Quarantine);
    CHECK(!rep.dispatch_enabled);
    CHECK(!l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
    CHECK_EQ(l.metrics().get(metric::kRecoveryIntegrityFail), std::int64_t(1));
}

// --- G17: what a promotion or a restore may and may not claim ---------------

SCENARIO("HA-01", "P0", "a clean process restart may reach READY") {
    Lab l("ha-01");
    l.start();
    // Nothing unresolved: an ordinary restart is allowed to serve again.
    l.restart();
    const RecoveryReport rep = l.start();
    CHECK_EQ(rep.phase, Phase::Ready);
    CHECK(rep.dispatch_enabled);
    CHECK(l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
}

SCENARIO("HA-02", "P0", "a promoted standby does not reach READY on local evidence") {
    // JI-32. Local history verifying says nothing about what the previous
    // primary acknowledged and this node never received.
    Lab l("ha-02");
    l.start();
    l.restart();
    const RecoveryReport rep = l.start(RecoveryMode::PromotedStandby);
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK(rep.external_reconciliation_required);
    CHECK(!rep.dispatch_enabled);
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));
}

SCENARIO("HA-03", "P0", "a restored instance does not resume dispatch on its own") {
    // JI-31. The restore may predate orders the counterparty already filled.
    Lab l("ha-03");
    l.start();
    l.restart();
    const RecoveryReport rep = l.start(RecoveryMode::RestoredFromBackup);
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK(!rep.dispatch_enabled);
}

SCENARIO("HA-04", "P0", "external evidence is what lifts RECON_ONLY, and it is recorded") {
    Lab l("ha-04");
    l.start();
    l.restart();
    CHECK_EQ(l.start(RecoveryMode::PromotedStandby).phase, Phase::ReconOnly);

    // An empty approval is not evidence.
    l.supervisor().provide_external_evidence("");
    CHECK_EQ(l.supervisor().verify_and_replay(RecoveryMode::PromotedStandby).phase,
             Phase::ReconOnly);

    l.supervisor().provide_external_evidence("risk-officer+ops-lead");
    const RecoveryReport after =
        l.supervisor().verify_and_replay(RecoveryMode::PromotedStandby);
    CHECK_EQ(after.phase, Phase::Ready);
    CHECK(after.dispatch_enabled);
}

SCENARIO("HA-05", "P0", "an unresolved outbound effect holds even a plain restart") {
    Lab l("ha-05");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());

    l.restart();
    const RecoveryReport rep = l.start(RecoveryMode::ProcessRestart);
    // BOUND is not evidence the counterparty acted (JI-16), so the instance
    // holds rather than resuming.
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK(rep.external_reconciliation_required);
}

SCENARIO("HA-06", "P0", "an open reconciliation case holds the instance") {
    Lab l("ha-06");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    l.deliver(l.fill("O-nonexistent", "E1", 1000, 10000, ""));  // opens a case
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));

    l.restart();
    const RecoveryReport rep = l.start();
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK_EQ(rep.open_cases, std::size_t(1));
}

SCENARIO("HA-07", "P0", "closing a case needs recorded approvals") {
    Lab l("ha-07");
    l.start();
    RecoveryCasePlan c;
    c.case_id = "case-1";
    c.chain = "O1";
    c.reason_code = "UNKNOWN_OUTCOME";
    c.status = "OPEN";
    CHECK_EQ(l.store().open_case(Lab::kTenant, c), StoreError::None);

    // Section 14's runbook: evidence and dual approval, or the case stays.
    CHECK_EQ(l.store().resolve_case(Lab::kTenant, "case-1", "RESOLVED", ""),
             StoreError::Rejected);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));

    CHECK_EQ(l.store().resolve_case(Lab::kTenant, "case-1", "RESOLVED",
                                    "risk-officer+ops-lead"),
             StoreError::None);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(0));
}

SCENARIO("HA-08", "P0", "recovery reports what it actually checked") {
    Lab l("ha-08");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    l.restart();
    const RecoveryReport rep = l.start();
    CHECK(rep.storage_valid);
    CHECK(rep.journal_valid);
    CHECK(rep.projections_valid);
    CHECK(rep.fencing_valid);
    CHECK(rep.streams_checked >= std::size_t(2));
    CHECK(rep.events_checked >= std::size_t(3));
    CHECK(rep.duration_ms >= 0);
}

SCENARIO_BLOCKED("G15-08", "P0", "a power cut during fsync",
                 "section 17.1 says this needs hardware whose write cache is under test "
                 "control. A process terminated without unwinding, which the crash suite does "
                 "use, is a weaker thing and is not presented as this.")

SCENARIO_BLOCKED("G15-09", "P0", "a disk that reports a successful flush without performing one",
                 "a lying fsync cannot be detected from inside the process that issued it. "
                 "Section 2 lists trusting the storage flush path as an explicit precondition "
                 "of the ACCEPTED_DURABLE definition, not something the engine verifies.")

SCENARIO_BLOCKED("G15-10", "P0", "ENOSPC partway through a transaction",
                 "filling a real volume is not something this suite may do to the host. The "
                 "typed Full error it would produce is mapped and treated as fail-closed, and "
                 "the all-or-nothing behaviour under an injected write failure is covered by "
                 "G02-01 through G02-15.")

SCENARIO_BLOCKED("G17-01", "P0", "an acknowledged commit lost on an async standby",
                 "there is no replica in this lab. The rule the window protects, that a "
                 "promotion may not resume dispatch without independent evidence, is asserted "
                 "by HA-02.")

SCENARIO_BLOCKED("G17-02", "P0", "two primaries accept writes after a split promotion",
                 "needs two PostgreSQL nodes. The fencing that makes a split brain safe at "
                 "the resource boundary is covered by G14-13.")

SCENARIO_BLOCKED("G17-03", "P0", "a diverged timeline after promoting the wrong node",
                 "timeline selection is PostgreSQL recovery machinery. The lineage comparison "
                 "P06 does own is covered by BK-05 in test_backup_restore.")

SCENARIO_BLOCKED("G17-04", "P1", "measured RPO and RTO under forced failover",
                 "a number produced without the real topology would be an invented SLO, which "
                 "section 19 explicitly warns against. The replay rate that can be measured "
                 "here is reported by the journal_bench application instead.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_storage_ha"); }
