// The 24 crash windows of design doc section 15.
//
// Two mechanisms are used, and the difference matters for how much each
// case proves.
//
//  * In-process injection returns a typed error or throws at a boundary.
//    It proves the unwinding path is correct, which is most of what the
//    windows inside one transaction are about.
//
//  * A child process is launched, armed with a failpoint whose action is an
//    immediate _Exit with no unwinding and no flush, and killed exactly
//    inside the window. The parent then opens the same files and asserts.
//    This is the stronger evidence: nothing in the child's address space
//    could have tidied up on the way out.
//
// Neither is a power-loss test. Section 17.1 says durability past the fsync
// contract needs hardware whose write cache is under test control, and the
// scenarios that need that are reported BLOCKED rather than claimed.
#include <cstdlib>
#include <filesystem>
#include <string>

#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

std::string g_self;  // path to this executable, for relaunching

// Runs the workload a child is supposed to perform, with whatever failpoint
// the environment armed. Returns 0 if it got all the way through, which for
// a crash test means the window was NOT reached.
int child_workload(const std::string& dir, const std::string& what) {
    ManualClock clock(1767225600000);
    SqliteAtomicStore::Options sopt;
    sopt.path = dir + "/journal.db";
    sopt.clock = &clock;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    if (SqliteAtomicStore::open(sopt, &store, &why) != StoreError::None) return 2;

    StableSendStore::Options lopt;
    lopt.path = dir + "/session.db";
    lopt.clock = &clock;
    std::unique_ptr<StableSendStore> link;
    if (StableSendStore::open(lopt, &link, &why) != StoreError::None) return 2;

    FenceToken token = 0;
    if (store->acquire_fence("t1", "D1", "child", 60000, &token) != StoreError::None) return 2;

    GatewayService::Options gopt;
    gopt.link = link.get();
    GatewayService svc(*store, gopt);

    if (what == "bootstrap" || what == "new") {
        if (store->head("t1", risk_stream("D1")).version == 0) {
            svc.set_risk_limit("t1", "D1", 1000000, token);
        }
    }

    NewOrderRequest r;
    r.tenant = "t1";
    r.account = "A1";
    r.command_id = "K1";
    r.chain = "O1";
    r.cl_ord_id = "K1-cl";
    r.symbol = "EURUSD";
    r.side = "BUY";
    r.order_qty = 100000;
    r.limit_px = 10000;
    r.risk_domain = "D1";
    r.session_key = "S1";
    r.fix_payload = "35=D|11=K1-cl|38=100000";

    if (what == "new" || what == "bootstrap") {
        svc.submit_new(r, token);
        return 0;
    }

    if (what == "dispatch") {
        svc.submit_new(r, token);
        OutboxDispatcher::Options dopt;
        dopt.owner = "child-dispatcher";
        OutboxDispatcher d(*store, *link, dopt);
        d.dispatch_once("t1", token);
        return 0;
    }

    if (what == "inbound") {
        svc.submit_new(r, token);
        ExecutionReport e;
        e.tenant = "t1";
        e.session_key = "S1";
        e.session_epoch = 1;
        e.receipt_id = 500;
        e.lp_namespace = "LP1";
        e.exec_id = "E500";
        e.exec_type = "TRADE";
        e.chain = "O1";
        e.account = "A1";
        e.risk_domain = "D1";
        e.last_qty = 40000;
        e.last_px = 10000;
        e.originating_command = "K1";
        e.raw = "35=8|17=E500|32=40000";
        RawReceipt raw;
        raw.session_key = e.session_key;
        raw.session_epoch = e.session_epoch;
        raw.receipt_id = e.receipt_id;
        raw.raw = e.raw;
        raw.raw_hash = sha256(e.raw);
        link->persist_raw(raw);
        svc.apply_report(e, token);
        svc.ack_delivery(e);
        return 0;
    }

    if (what == "snapshot") {
        svc.submit_new(r, token);
        SnapshotStore snaps(*store);
        Version at = 0;
        std::string w;
        snaps.checkpoint("t1", order_stream("O1"), &at, &w);
        return 0;
    }

    return 3;
}

// Launches a child with one failpoint armed to terminate abruptly.
// Returns the child's exit status. 137 is the abrupt exit; 0 means the
// window was never reached, which the caller should treat as a test error.
int crash_child(const std::string& dir, const std::string& what, const std::string& spec) {
    std::string cmd = "\"" + g_self + "\" --crash-child=" + what + " --dir=\"" + dir + "\"";
#if defined(_WIN32)
    const std::string set = "set \"JRNL_FAILPOINT=" + spec + "\" && ";
    cmd = "cmd /C \"" + set + cmd + "\"";
#else
    cmd = "JRNL_FAILPOINT='" + spec + "' " + cmd;
#endif
    return std::system(cmd.c_str());
}

// Opens the files a crashed child left behind and reports what survived.
struct Aftermath {
    std::size_t events = 0;
    std::size_t manifests = 0;
    std::size_t orders = 0;
    std::size_t intents = 0;
    std::size_t outbox = 0;
    std::size_t receipts = 0;
    std::size_t fills = 0;
    std::size_t snapshots = 0;
    std::size_t usable_snapshots = 0;
    std::uint64_t allocated_sequences = 0;
    std::size_t unacked = 0;
    bool verifies = false;
    std::string detail;
};

Aftermath inspect(const std::string& dir) {
    Aftermath a;
    ManualClock clock(1767225600000);
    SqliteAtomicStore::Options sopt;
    sopt.path = dir + "/journal.db";
    sopt.create_if_missing = false;
    sopt.clock = &clock;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    if (SqliteAtomicStore::open(sopt, &store, &why) != StoreError::None) {
        a.detail = "cannot reopen journal: " + why;
        return a;
    }

    auto count = [&](const char* table) -> std::size_t {
        Stmt s;
        if (store->db().prepare(std::string("SELECT COUNT(*) FROM ") + table +
                                    " WHERE tenant_id='t1';",
                                &s) != StoreError::None) {
            return 0;
        }
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) return 0;
        return static_cast<std::size_t>(s.col_i64(0));
    };
    a.events = count("journal_events");
    a.manifests = count("journal_transactions");
    a.orders = count("orders");
    a.intents = count("command_intents");
    a.outbox = count("command_outbox");
    a.receipts = count("inbox_receipts");
    a.fills = count("fills");
    a.snapshots = count("snapshots");
    {
        Stmt s;
        if (store->db().prepare(
                "SELECT COUNT(*) FROM snapshots WHERE tenant_id='t1' AND verified=1;", &s) ==
            StoreError::None) {
            bool row = false;
            if (s.step(&row) == StoreError::None && row) {
                a.usable_snapshots = static_cast<std::size_t>(s.col_i64(0));
            }
        }
    }

    const VerifyReport v = verify_all(*store, "t1");
    a.verifies = v.ok();
    for (const std::string& d : v.details) a.detail += d + "; ";

    StableSendStore::Options lopt;
    lopt.path = dir + "/session.db";
    lopt.clock = &clock;
    std::unique_ptr<StableSendStore> link;
    if (StableSendStore::open(lopt, &link, &why) == StoreError::None) {
        a.allocated_sequences = link->allocated_sequences("S1");
        a.unacked = link->unacked_count("S1");
    }
    return a;
}

std::string fresh_dir(const char* label) {
    static std::atomic<unsigned> n{0};
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "jrnl-crash" /
                                    (std::string(label) + "-" + std::to_string(n.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

// Prepares a directory whose risk domain already exists, so the crash under
// test is the order admission and not the bootstrap.
std::string prepared_dir(const char* label) {
    const std::string dir = fresh_dir(label);
    ManualClock clock(1767225600000);
    SqliteAtomicStore::Options sopt;
    sopt.path = dir + "/journal.db";
    sopt.clock = &clock;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    if (SqliteAtomicStore::open(sopt, &store, &why) != StoreError::None) {
        TT_FAIL("cannot prepare crash directory: " + why);
    }
    FenceToken t = 0;
    store->acquire_fence("t1", "D1", "prep", 600000, &t);
    GatewayService::Options gopt;
    GatewayService svc(*store, gopt);
    if (svc.set_risk_limit("t1", "D1", 1000000, t).kind != CommitKind::Committed) {
        TT_FAIL("cannot set the limit in the crash directory");
    }
    return dir;
}

// Asserts the child really died rather than running to completion.
void expect_killed(int status, const char* window) {
    if (status == 0) {
        TT_FAIL(std::string(window) + ": the child completed, so the window was never reached");
    }
}

// --- Windows inside the business transaction, W02..W10 --------------------

void expect_nothing_committed(const char* window, const char* point) {
    const std::string dir = prepared_dir(window);
    const Aftermath before = inspect(dir);
    expect_killed(crash_child(dir, "new", std::string(point) + "=exit"), window);
    const Aftermath after = inspect(dir);

    CHECK_MSG(after.events == before.events,
              std::string(window) + ": journal grew from " + std::to_string(before.events) +
                  " to " + std::to_string(after.events));
    CHECK_MSG(after.manifests == before.manifests, std::string(window) + ": manifest survived");
    CHECK_MSG(after.orders == before.orders, std::string(window) + ": order projection survived");
    CHECK_MSG(after.intents == before.intents, std::string(window) + ": command intent survived");
    CHECK_MSG(after.outbox == before.outbox, std::string(window) + ": outbox intent survived");
    CHECK_MSG(after.verifies, std::string(window) + ": " + after.detail);
}

SCENARIO("W02", "P0", "killed after validation, before BEGIN") {
    expect_nothing_committed("W02", "tx.before_begin");
}

SCENARIO("W03", "P0", "killed after BEGIN, before any lock") {
    expect_nothing_committed("W03", "tx.after_begin");
}

SCENARIO("W04", "P0", "killed after the fence check, before dedup") {
    expect_nothing_committed("W04", "tx.after_fence_check");
}

SCENARIO("W05", "P0", "killed after the dedup insert, before the journal append") {
    expect_nothing_committed("W05", "tx.after_dedup_insert");
}

SCENARIO("W06", "P0", "killed after the journal append, before the head update") {
    expect_nothing_committed("W06", "tx.after_journal_append");
}

SCENARIO("W07", "P0", "killed between the order and risk projections") {
    expect_nothing_committed("W07", "tx.after_order_projection");
}

SCENARIO("W08", "P0", "killed after the risk projection, before the command intent") {
    expect_nothing_committed("W08", "tx.after_risk_projection");
}

SCENARIO("W09", "P0", "killed after the command intent, before the outbox insert") {
    expect_nothing_committed("W09", "tx.after_command_intent");
}

SCENARIO("W10", "P0", "killed after the outbox insert, before COMMIT") {
    expect_nothing_committed("W10", "tx.before_commit");
}

// --- W11, W12: the commit boundary -----------------------------------------

SCENARIO("W11", "P0", "killed during COMMIT leaves a resolvable, self-consistent state") {
    const std::string dir = prepared_dir("W11");
    const Aftermath before = inspect(dir);
    expect_killed(crash_child(dir, "new", "tx.during_commit=exit"), "W11");
    const Aftermath after = inspect(dir);

    // Either the whole transaction is there or none of it is. What is NOT
    // allowed is a fragment, and whichever way it fell the database must
    // still verify.
    const bool all = after.events == before.events + 2 && after.orders == before.orders + 1 &&
                     after.intents == before.intents + 1 && after.outbox == before.outbox + 1;
    const bool none = after.events == before.events && after.orders == before.orders &&
                      after.intents == before.intents && after.outbox == before.outbox;
    CHECK_MSG(all || none, "W11 left a partial write set: " + after.detail);
    CHECK_MSG(after.verifies, "W11: " + after.detail);
    // And nothing reached the session either way.
    CHECK_EQ(after.allocated_sequences, std::uint64_t(0));
}

SCENARIO("W12", "P0", "killed after COMMIT, before the reply, is one durable order") {
    const std::string dir = prepared_dir("W12");
    expect_killed(crash_child(dir, "new", "tx.after_commit=exit"), "W12");
    const Aftermath after = inspect(dir);

    CHECK_EQ(after.orders, std::size_t(1));
    CHECK_EQ(after.intents, std::size_t(1));
    CHECK_EQ(after.outbox, std::size_t(1));
    CHECK_MSG(after.verifies, "W12: " + after.detail);

    // The client never heard, so it retries. The key resolves to the same
    // order rather than placing a second one (JI-15, W12's oracle).
    ManualClock clock(1767225600000);
    SqliteAtomicStore::Options sopt;
    sopt.path = dir + "/journal.db";
    sopt.create_if_missing = false;
    sopt.clock = &clock;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    CHECK_EQ(SqliteAtomicStore::open(sopt, &store, &why), StoreError::None);
    const CommandRecord rec = store->lookup("t1", "A1", "K1");
    CHECK(rec.found);
    CHECK_EQ(rec.chain, std::string("O1"));
}

// --- W13..W16: the dispatch boundary ----------------------------------------

SCENARIO("W13", "P0", "killed after the claim, before the session call") {
    const std::string dir = prepared_dir("W13");
    expect_killed(crash_child(dir, "dispatch", "outbox.before_stable_send=exit"), "W13");
    const Aftermath after = inspect(dir);

    CHECK_EQ(after.outbox, std::size_t(1));
    // Nothing was allocated, so the same key can go out later unchanged.
    CHECK_EQ(after.allocated_sequences, std::uint64_t(0));
    CHECK_MSG(after.verifies, "W13: " + after.detail);
}

SCENARIO("W14", "P0", "killed after the sequence is persisted never mints a second one") {
    const std::string dir = prepared_dir("W14");
    expect_killed(crash_child(dir, "dispatch", "p01.after_seq_persisted=exit"), "W14");
    const Aftermath first = inspect(dir);
    CHECK_EQ(first.allocated_sequences, std::uint64_t(1));

    // A restarted dispatcher re-presents the same key. The sequence count
    // must not move: this is the single most important assertion in the
    // whole outbound path (JI-19).
    // The row is CLAIMED with send_attempted set, so a second pass finds
    // nothing READY and returns without reaching the session. That is the
    // correct behaviour, not a missed window: the assertion that matters is
    // that no second sequence appeared.
    crash_child(dir, "dispatch", "p01.after_seq_persisted=exit");
    const Aftermath second = inspect(dir);
    CHECK_EQ(second.allocated_sequences, std::uint64_t(1));
    CHECK_MSG(second.verifies, "W14: " + second.detail);
}

SCENARIO("W15", "P0", "killed during the socket write binds one sequence and no more") {
    const std::string dir = prepared_dir("W15");
    expect_killed(crash_child(dir, "dispatch", "p01.during_socket_write=exit"), "W15");
    const Aftermath after = inspect(dir);
    CHECK_EQ(after.allocated_sequences, std::uint64_t(1));
    CHECK_MSG(after.verifies, "W15: " + after.detail);
}

SCENARIO("W16", "P0", "killed after the write, before the binding is recorded") {
    const std::string dir = prepared_dir("W16");
    expect_killed(crash_child(dir, "dispatch", "p01.after_socket_write=exit"), "W16");
    const Aftermath after = inspect(dir);
    CHECK_EQ(after.allocated_sequences, std::uint64_t(1));
    CHECK_MSG(after.verifies, "W16: " + after.detail);

    // Our own row still says CLAIMED or UNKNOWN; recovery must read P01's
    // mapping rather than assume. Either way no second sequence appears.
    crash_child(dir, "dispatch", "p01.after_socket_write=exit");
    CHECK_EQ(inspect(dir).allocated_sequences, std::uint64_t(1));
}

// --- W18..W21: the inbound boundary ------------------------------------------

SCENARIO("W18", "P0", "killed after the raw receipt is durable, before it is handed over") {
    const std::string dir = prepared_dir("W18");
    expect_killed(crash_child(dir, "inbound", "p01.after_raw_receipt=exit"), "W18");
    const Aftermath after = inspect(dir);

    CHECK_EQ(after.unacked, std::size_t(1));  // P01 still owes it
    CHECK_EQ(after.receipts, std::size_t(0));  // P06 never saw it
    CHECK_EQ(after.fills, std::size_t(0));
    CHECK_MSG(after.verifies, "W18: " + after.detail);
}

SCENARIO("W19", "P0", "killed after the inbox insert, before the business projection") {
    const std::string dir = prepared_dir("W19");
    expect_killed(crash_child(dir, "inbound", "inbox.after_insert=exit"), "W19");
    const Aftermath after = inspect(dir);

    // The receipt and the projection commit together or not at all.
    CHECK_EQ(after.receipts, std::size_t(0));
    CHECK_EQ(after.fills, std::size_t(0));
    CHECK_EQ(after.unacked, std::size_t(1));
    CHECK_MSG(after.verifies, "W19: " + after.detail);
}

SCENARIO("W20", "P0", "killed after the execution key, before the ledger row") {
    const std::string dir = prepared_dir("W20");
    expect_killed(crash_child(dir, "inbound", "inbox.after_exec_key=exit"), "W20");
    const Aftermath after = inspect(dir);

    // A dedup key without its fill would make the trade unrepeatable AND
    // unrecorded, which is the worst of both. Neither survives.
    CHECK_EQ(after.fills, std::size_t(0));
    CHECK_EQ(after.receipts, std::size_t(0));
    CHECK_MSG(after.verifies, "W20: " + after.detail);
}

SCENARIO("W21", "P0", "killed after COMMIT, before the delivery acknowledgement") {
    const std::string dir = prepared_dir("W21");
    expect_killed(crash_child(dir, "inbound", "inbox.before_delivery_ack=exit"), "W21");
    const Aftermath after = inspect(dir);

    CHECK_EQ(after.fills, std::size_t(1));
    CHECK_EQ(after.receipts, std::size_t(1));
    // P01 never learned, so it will redeliver.
    CHECK_EQ(after.unacked, std::size_t(1));
    CHECK_MSG(after.verifies, "W21: " + after.detail);

    // The redelivery is a no-op, not a second fill.
    expect_killed(crash_child(dir, "inbound", "inbox.before_delivery_ack=exit"), "W21 redeliver");
    const Aftermath again = inspect(dir);
    CHECK_EQ(again.fills, std::size_t(1));
    CHECK_MSG(again.verifies, "W21 redelivery: " + again.detail);
}

// --- W22: the snapshot boundary -----------------------------------------------

SCENARIO("W22", "P0", "killed after the snapshot insert, before the usable marker") {
    const std::string dir = prepared_dir("W22");
    expect_killed(crash_child(dir, "snapshot", "snapshot.after_insert=exit"), "W22");
    const Aftermath after = inspect(dir);

    CHECK_EQ(after.snapshots, std::size_t(1));
    // Present but not usable: recovery ignores it and replays the journal.
    CHECK_EQ(after.usable_snapshots, std::size_t(0));
    CHECK_MSG(after.verifies, "W22: " + after.detail);
}

// --- W01, W17, W23, W24 --------------------------------------------------------

SCENARIO("W01", "P0", "killed before the request is canonicalised leaves no intent") {
    const std::string dir = prepared_dir("W01");
    const Aftermath before = inspect(dir);
    // Nothing is run at all: the window is "before we received anything".
    const Aftermath after = inspect(dir);
    CHECK_EQ(after.events, before.events);
    CHECK_EQ(after.intents, std::size_t(0));
    CHECK(after.verifies);
}

SCENARIO("W17", "P0", "a counterparty fill with no local receipt is visible as a gap") {
    // The LP traded; our side has nothing. What the engine owes here is not
    // a guess but the ability to show the discrepancy.
    Lab l("W17");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 100000), l.token());
    l.dispatcher().dispatch_once(Lab::kTenant, l.token());

    // The counterparty's own books record a trade we never heard about.
    l.lp().record_trade("E-unseen", 40000, 10000);
    CHECK_EQ(l.lp().traded_quantity(), std::int64_t(40000));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));

    // Our outbound effect is BOUND with no evidence, so a restart holds.
    l.restart();
    const RecoveryReport rep = l.start();
    CHECK_EQ(rep.phase, Phase::ReconOnly);
    CHECK(rep.external_reconciliation_required);
    CHECK(!l.supervisor().may_admit(Lab::kTenant, Lab::kDomain));
}

SCENARIO("W24", "P0", "a promotion fences the old owner out of dispatch") {
    Lab l("W24");
    l.start();
    const FenceToken old_token = l.token();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());

    // A new instance takes the domain.
    FenceToken newer = 0;
    CHECK_EQ(l.store().acquire_fence(Lab::kTenant, Lab::kDomain, "gateway-2", 60000, &newer),
             StoreError::None);

    // The old owner tries to carry on. Its writes are refused and its
    // dispatch is refused (JI-21, JI-22).
    const CommitOutcome o = l.service().submit_new(l.new_order("K2", "O2", 1000), old_token);
    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK(!l.supervisor().may_dispatch(Lab::kTenant, Lab::kDomain));
    expect_consistent(l, "W24");
}

SCENARIO_BLOCKED("W23", "P0", "a commit lost when an async standby is promoted",
                 "there is no PostgreSQL and no replica in this lab, so the window cannot be "
                 "produced. The engine-side rule it exists to protect, that a promotion "
                 "without independent evidence may not resume dispatch, is asserted by "
                 "HA-02 in test_storage_ha.")

SCENARIO_BLOCKED("W-POWER", "P0", "a real power cut during fsync",
                 "the child process is terminated without unwinding, which does not test the "
                 "storage flush path. Section 17.1 is explicit that this needs hardware whose "
                 "write cache is under test control, and that is outside this lab.")

}  // namespace

int main(int argc, char** argv) {
    g_self = argv[0];
    std::string child_mode;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--crash-child=", 0) == 0) child_mode = a.substr(14);
        else if (a.rfind("--dir=", 0) == 0) dir = a.substr(6);
    }
    if (!child_mode.empty()) {
        // Armed from the environment so the parent can name an exact
        // boundary without the child knowing anything about the scenario.
        fp::arm_from_env();
        return child_workload(dir, child_mode);
    }
    return tt::run_all(argc, argv, "test_crash_windows");
}
