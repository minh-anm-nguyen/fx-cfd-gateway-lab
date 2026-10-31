// Crash consistency: group G of the what-if matrix and playbooks PB-01 to
// PB-04. Design doc 15.8, 16 and release gate C.
//
// Each case spawns this same executable as a child, arms one failpoint at an
// exact durability boundary, and lets the child die there. Termination is an
// immediate exit with no unwinding and no flushing, which is the closest
// approximation to SIGKILL available on both platforms. The parent then reopens
// the store and asserts what survived.
//
// What this does NOT prove: durability below the fsync contract. A real power
// cut (G-09) needs hardware or a VM and is reported as out of lab scope.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/failpoint.hpp"
#include "tiny_test.hpp"

using namespace fixse;

namespace {

std::string g_self;  // absolute path to this executable

struct ChildRun {
    int exit_code = 0;
    bool crashed = false;  // terminated at the failpoint rather than exiting normally
    std::string command;
};

// Run the child with one armed failpoint. The failpoint is passed as an
// argument rather than through the environment so no shell state is involved.
ChildRun run_child(const std::string& db, const std::string& scenario,
                   const std::string& failpoint) {
    std::string inner;
    inner += '"' + g_self + '"';
    inner += " --child=" + scenario;
    inner += " --db=\"" + db + "\"";
    inner += " --failpoint=" + failpoint;

    ChildRun r;
#ifdef _WIN32
    // The command processor strips the outer quote pair when a command line
    // starts with a quote, so the whole thing is wrapped once more.
    r.command = '"' + inner + '"';
#else
    r.command = inner;
#endif
    r.exit_code = std::system(r.command.c_str());
    r.crashed = r.exit_code != 0;
    return r;
}

// ---------------------------------------------------------------------------
// Child side
// ---------------------------------------------------------------------------

struct ChildEngine {
    VirtualClock clock;
    SessionConfig cfg;
    std::unique_ptr<SqliteSessionStore> store;
    tt::RecordingTransport transport;
    tt::FakeController controller;
    tt::RecordingSink sink;
    Metrics metrics;
    AuditLog audit;
    std::unique_ptr<SessionEngine> engine;
    uint64_t generation = 1;

    explicit ChildEngine(const std::string& db) {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "CRASH";
        cfg.heartbeat_seconds = 30;
        cfg.database_path = db;

        SqliteSessionStore::Options o;
        o.path = db;
        o.key.sender_comp_id = cfg.sender_comp_id;
        o.key.target_comp_id = cfg.target_comp_id;
        o.key.qualifier = cfg.qualifier;
        o.create_if_missing = true;
        o.clock = &clock;
        std::string why;
        if (SqliteSessionStore::open(o, &store, &why) != StoreError::None) {
            std::fprintf(stderr, "child: store open failed: %s\n", why.c_str());
            std::exit(3);
        }

        EngineDeps d;
        d.cfg = &cfg;
        d.store = store.get();
        d.clock = &clock;
        d.transport = &transport;
        d.controller = &controller;
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<SessionEngine>(d);
        engine->start();
    }

    peer::Msg msg(const std::string& type, uint64_t seq) const {
        peer::Msg m;
        m.msg_type = type;
        m.seq = seq;
        m.sender = cfg.target_comp_id;
        m.target = cfg.sender_comp_id;
        m.sending_time = format_utc_timestamp(clock.wall_now());
        return m;
    }

    void logon() {
        transport.accept_generation = generation;
        engine->on_connected(generation);
        peer::Msg m = msg("A", engine->next_in());
        m.extra.push_back({98, "0"});
        m.extra.push_back({108, "30"});
        engine->on_bytes(m.build(), generation);
    }

    void feed_app(uint64_t seq, const std::string& payload) {
        peer::Msg m = msg("U1", seq);
        m.extra.push_back({20001, payload});
        engine->on_bytes(m.build(), generation);
    }
};

int run_child_scenario(const std::string& scenario, const std::string& db,
                       const std::string& failpoint) {
    fp::arm_from_spec(failpoint);
    ChildEngine c(db);
    c.logon();

    if (scenario == "outbound") {
        // The failpoint targets the SECOND outbound commit, which is this
        // application message; the first was the Logon.
        c.engine->submit_app_message("U1", {{20001, "the-order"}});
    } else if (scenario == "partial_write") {
        c.transport.max_bytes_per_write = 7;
        c.engine->submit_app_message("U1", {{20001, "the-order"}});
    } else if (scenario == "inbound") {
        c.feed_app(c.engine->next_in(), "inbound-event");
    } else {
        std::fprintf(stderr, "child: unknown scenario %s\n", scenario.c_str());
        return 4;
    }

    // Reached only when the failpoint did not fire.
    c.store->mark_clean_shutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Parent side helpers
// ---------------------------------------------------------------------------

struct Inspection {
    PersistedState state;
    std::vector<OutboundRecord> outbound;
    std::vector<InboundRecord> pending_inbox;
    ConsistencyReport report;
};

Inspection inspect(const std::string& db) {
    VirtualClock clock;
    SqliteSessionStore::Options o;
    o.path = db;
    o.key.sender_comp_id = "TRADER01";
    o.key.target_comp_id = "SIMLP01";
    o.key.qualifier = "CRASH";
    o.create_if_missing = false;
    o.clock = &clock;
    std::unique_ptr<SqliteSessionStore> store;
    std::string why;
    const StoreError e = SqliteSessionStore::open(o, &store, &why);
    if (e != StoreError::None) throw tt::AssertionFailure("inspect: store open failed: " + why);

    Inspection out;
    if (store->load_state(&out.state) != StoreError::None)
        throw tt::AssertionFailure("inspect: load_state failed");
    out.report = store->verify_consistency();
    store->load_outbound_range(1, out.state.next_out == 0 ? 1 : out.state.next_out + 50,
                               &out.outbound);
    store->load_pending_inbox(100, &out.pending_inbox);
    return out;
}

const OutboundRecord* find_seq(const Inspection& i, uint64_t seq) {
    for (const OutboundRecord& r : i.outbound)
        if (r.seq == seq) return &r;
    return nullptr;
}

std::string fresh_db(const std::string& label) {
    static tt::TempDir dir("crash");
    const std::string path = dir.file(label + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-wal", ec);
    std::filesystem::remove(path + "-shm", ec);
    std::filesystem::remove(path + ".lock", ec);
    return path;
}

// A fresh engine on an existing store, the shape of a restart after a crash.
struct ReopenedEngine {
    VirtualClock clock;
    SessionConfig cfg;
    std::unique_ptr<SqliteSessionStore> store;
    tt::RecordingTransport transport;
    tt::FakeController controller;
    tt::RecordingSink sink;
    Metrics metrics;
    AuditLog audit;
    std::unique_ptr<SessionEngine> engine;

    explicit ReopenedEngine(const std::string& db) {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "CRASH";
        cfg.database_path = db;

        SqliteSessionStore::Options o;
        o.path = db;
        o.key.sender_comp_id = cfg.sender_comp_id;
        o.key.target_comp_id = cfg.target_comp_id;
        o.key.qualifier = cfg.qualifier;
        o.create_if_missing = false;
        o.clock = &clock;
        std::string why;
        if (SqliteSessionStore::open(o, &store, &why) != StoreError::None)
            throw tt::AssertionFailure("reopen failed: " + why);

        EngineDeps d;
        d.cfg = &cfg;
        d.store = store.get();
        d.clock = &clock;
        d.transport = &transport;
        d.controller = &controller;
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<SessionEngine>(d);
    }
};

}  // namespace

SCENARIO("G-01", "P0", "a crash before the outbound commit leaves no trace of the message") {
    const std::string db = fresh_db("g01");
    // Kill at the start of the SECOND outbound transaction, which is the
    // application message; the first was the Logon.
    const ChildRun r = run_child(db, "outbound", "store.outbound.before_begin=exit@2");
    CHECK_MSG(r.crashed, "child did not reach the failpoint: " + r.command);

    const Inspection i = inspect(db);
    // The Logon is durable; the application message never existed.
    CHECK_EQ(i.state.next_out, uint64_t{2});
    CHECK(find_seq(i, 1) != nullptr);
    CHECK(find_seq(i, 2) == nullptr);
    CHECK_MSG(i.report.ok, i.report.problem);
    CHECK(!i.report.clean_shutdown);
}

SCENARIO("PB-01", "P0", "a crash after the outbound commit keeps the sequence reserved") {
    const std::string db = fresh_db("pb01");
    const ChildRun r = run_child(db, "outbound", "store.outbound.after_commit=exit@2");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    const OutboundRecord* rec = find_seq(i, 2);
    CHECK(rec != nullptr);
    CHECK_EQ(rec->msg_type, std::string("U1"));
    CHECK(rec->payload.find("the-order") != std::string::npos);
    CHECK_EQ(i.state.next_out, uint64_t{3});
    CHECK_MSG(i.report.ok, i.report.problem);

    // After the restart the next original takes sequence 3: the engine never
    // issues sequence 2 a second time with different content (I-02, I-05).
    ReopenedEngine re(db);
    CHECK_EQ(re.engine->start(), StoreError::None);
    re.transport.accept_generation = 7;
    re.engine->on_connected(7);
    const auto frames = re.transport.frames();
    CHECK_EQ(frames.size(), size_t{1});
    CHECK_EQ(peer::get(frames[0], 35).value_or(""), std::string("A"));
    CHECK_EQ(peer::get(frames[0], 34).value_or(""), std::string("3"));
}

SCENARIO("PB-02", "P0", "a crash in the middle of a socket write does not reuse the sequence") {
    const std::string db = fresh_db("pb02");
    // The transport takes seven bytes at a time and the kill lands after the
    // first short write, so the peer holds a partial frame and nothing more.
    const ChildRun r = run_child(db, "partial_write", "socket.after_partial_write=exit@1");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    const OutboundRecord* rec = find_seq(i, 2);
    CHECK(rec != nullptr);
    CHECK_EQ(i.state.next_out, uint64_t{3});
    CHECK_MSG(i.report.ok, i.report.problem);
    // The stored frame is the complete original, so a resend can answer for it
    // even though the peer only ever saw a prefix.
    std::string why;
    CHECK_MSG(peer::verify_frame(rec->payload, &why), why);
}

SCENARIO("G-04", "P0", "a crash after the write but before telemetry changes nothing durable") {
    const std::string db = fresh_db("g04");
    const ChildRun r = run_child(db, "outbound", "socket.after_write=exit@2");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    CHECK(find_seq(i, 2) != nullptr);
    CHECK_EQ(i.state.next_out, uint64_t{3});
    CHECK_MSG(i.report.ok, i.report.problem);
}

SCENARIO("G-05", "P0", "a crash before the inbound commit leaves NextNumIn untouched") {
    const std::string db = fresh_db("g05");
    // The Logon is the first inbound commit, the application message the second.
    const ChildRun r = run_child(db, "inbound", "store.inbound.before_begin=exit@2");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    // Only the Logon advanced sequencing, so the peer can resend the
    // application message: we never acknowledged it.
    CHECK_EQ(i.state.next_in, uint64_t{2});
    CHECK_EQ(i.pending_inbox.size(), size_t{0});
    CHECK_MSG(i.report.ok, i.report.problem);
}

SCENARIO("PB-03", "P0", "a crash between the inbound commit and delivery keeps exactly one row") {
    const std::string db = fresh_db("pb03");
    const ChildRun r = run_child(db, "inbound", "store.inbound.after_commit=exit@2");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    CHECK_EQ(i.pending_inbox.size(), size_t{1});
    CHECK_EQ(i.pending_inbox[0].seq, uint64_t{2});
    CHECK(i.pending_inbox[0].payload.find("inbound-event") != std::string::npos);
    CHECK_EQ(i.state.next_in, uint64_t{3});
    CHECK_MSG(i.report.ok, i.report.problem);

    // On restart the engine dispatches it from the durable inbox, once.
    ReopenedEngine re(db);
    CHECK_EQ(re.engine->start(), StoreError::None);
    CHECK_EQ(re.sink.delivered.size(), size_t{1});
    CHECK_EQ(re.sink.delivered[0], uint64_t{2});
    re.engine->on_timer(re.clock.steady_now());
    CHECK_EQ(re.sink.delivered.size(), size_t{1});
}

SCENARIO("G-07", "P0", "a crash after delivery but before the mark means at-least-once") {
    const std::string db = fresh_db("g07");
    const ChildRun r = run_child(db, "inbound", "sink.after_deliver=exit@1");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    // The row is still pending, so the consumer sees the event again. That is
    // the honest guarantee: at-least-once with an idempotent consumer (I-11).
    CHECK_EQ(i.pending_inbox.size(), size_t{1});
    CHECK_EQ(i.state.next_in, uint64_t{3});
    CHECK_MSG(i.report.ok, i.report.problem);

    ReopenedEngine re(db);
    CHECK_EQ(re.engine->start(), StoreError::None);
    CHECK_EQ(re.sink.delivered.size(), size_t{1});
    CHECK_EQ(re.sink.delivered[0], uint64_t{2});
}

SCENARIO("G-08", "P0", "a crash inside the inbound transaction rolls the whole thing back") {
    const std::string db = fresh_db("g08");
    const ChildRun r = run_child(db, "inbound", "store.inbound.before_commit=exit@2");
    CHECK(r.crashed);

    const Inspection i = inspect(db);
    // Neither the inbox row nor the new NextNumIn survived: the transaction was
    // atomic (invariant I-07).
    CHECK_EQ(i.pending_inbox.size(), size_t{0});
    CHECK_EQ(i.state.next_in, uint64_t{2});
    CHECK_MSG(i.report.ok, i.report.problem);
}

SCENARIO("G-02b", "P0", "reopening twice after an abrupt kill gives the same answer") {
    const std::string db = fresh_db("g02b");
    const ChildRun r = run_child(db, "outbound", "store.outbound.after_commit=exit@2");
    CHECK(r.crashed);
    // No repair, no drift: recovery is a read, not a rewrite.
    const Inspection first = inspect(db);
    const Inspection second = inspect(db);
    CHECK_EQ(first.state.next_out, second.state.next_out);
    CHECK_EQ(first.state.next_in, second.state.next_in);
    CHECK_EQ(first.state.version, second.state.version);
    CHECK(second.report.ok);
}

SCENARIO("PB-04", "P0", "a store failure at any transaction boundary sends nothing") {
    // The error variant of the same failpoints, in process against the real
    // SQLite store: a refused commit leaves no frame on the wire and no
    // sequence consumed.
    const char* points[] = {"store.outbound.before_begin", "store.outbound.after_insert",
                            "store.outbound.after_update", "store.outbound.before_commit"};
    for (int i = 0; i < 4; ++i) {
        tt::EngineFixture f("pb04-" + std::to_string(i));
        f.connect_and_logon();
        f.out();
        const uint64_t out_before = f.engine->next_out();
        const size_t written_before = f.transport.written.size();

        fp::disarm_all();
        fp::arm(points[i], fp::Action::ReturnError);
        const SubmitResult r = f.engine->submit_app_message("U1", {{20001, "order"}});
        fp::disarm_all();

        CHECK_MSG(r == SubmitResult::RejectedStoreFailure, points[i]);
        CHECK_MSG(f.engine->next_out() == out_before, points[i]);
        CHECK_MSG(f.transport.written.size() == written_before, points[i]);
        CHECK_MSG(f.engine->state() == SessionState::Faulted, points[i]);
        // And the store agrees: no half written history row.
        const ConsistencyReport rep = f.store->verify_consistency();
        CHECK_MSG(rep.ok, std::string(points[i]) + ": " + rep.problem);
    }
}

SCENARIO_BLOCKED("G-09", "P0", "a simulated power loss respects the fsync contract",
                 "the crash suite terminates a process without unwinding, which does not test "
                 "hardware durability. A real power cut needs a VM or a machine with the write "
                 "cache under test control: it stays out of lab scope and is declared rather "
                 "than assumed.")

int main(int argc, char** argv) {
    std::string child;
    std::string db;
    std::string failpoint;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--child=", 0) == 0) child = a.substr(8);
        else if (a.rfind("--db=", 0) == 0) db = a.substr(5);
        else if (a.rfind("--failpoint=", 0) == 0) failpoint = a.substr(12);
    }
    if (!child.empty()) return run_child_scenario(child, db, failpoint);

    std::error_code ec;
    g_self = std::filesystem::absolute(argv[0], ec).string();
    return tt::run_all(argc, argv, "crash_windows");
}
