// Group G of the what-if matrix, the parts that can be proven in a single
// process: persistence, integrity, schema and ownership. The crash windows
// themselves (G-01..G-09) live in tests/crash, where a child process is killed
// at an exact failpoint.
#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <vector>

#include "fix/clock.hpp"
#include "fix/failpoint.hpp"
#include "fix/sqlite_store.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace fixse;

namespace {

SessionKey test_key() {
    SessionKey k;
    k.sender_comp_id = "TRADER01";
    k.target_comp_id = "SIMLP01";
    k.qualifier = "TEST";
    return k;
}

SqliteSessionStore::Options opts(const std::string& path, const IClock* clock,
                                 bool create = true) {
    SqliteSessionStore::Options o;
    o.path = path;
    o.key = test_key();
    o.create_if_missing = create;
    o.clock = clock;
    return o;
}

struct Fixture {
    VirtualClock clock;
    tt::TempDir dir;
    std::unique_ptr<SqliteSessionStore> store;
    std::string path;

    explicit Fixture(const std::string& label) : dir(label) {
        path = dir.file("session.db");
        std::string why;
        const StoreError e = SqliteSessionStore::open(opts(path, &clock), &store, &why);
        if (e != StoreError::None) throw tt::AssertionFailure("store open failed: " + why);
        // The engine always loads state before it commits anything; a store
        // that has not been read yet refuses writes on purpose.
        PersistedState st;
        const StoreError le = store->load_state(&st);
        if (le != StoreError::None)
            throw tt::AssertionFailure("initial load_state failed: " + std::string(to_string(le)));
    }

    PersistedState state() {
        PersistedState st;
        const StoreError e = store->load_state(&st);
        if (e != StoreError::None)
            throw tt::AssertionFailure("load_state failed: " + std::string(to_string(e)));
        return st;
    }
};

OutboundRecord app_out(uint64_t seq) {
    OutboundRecord r;
    r.seq = seq;
    r.msg_type = "U1";
    r.payload = "8=FIX.4.4|9=0|35=U1|34=" + std::to_string(seq) + "|10=000|";
    r.original_sending_time = "20261030-05:00:00.000";
    return r;
}

InboundRecord app_in(uint64_t seq) {
    InboundRecord r;
    r.seq = seq;
    r.msg_type = "U1";
    r.payload = "inbound-payload-" + std::to_string(seq);
    return r;
}

// Reopen the database with a bare sqlite handle, which is only possible once
// the store has been destroyed and the ownership lock released.
void tamper(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
        throw tt::AssertionFailure("tamper: cannot open database");
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    const std::string msg = err != nullptr ? err : "";
    if (err != nullptr) sqlite3_free(err);
    sqlite3_close(db);
    if (rc != SQLITE_OK) throw tt::AssertionFailure("tamper failed: " + msg);
}

}  // namespace

SCENARIO("G-00", "P0", "a fresh store starts at epoch 1 with both sequences at 1") {
    Fixture f("fresh");
    const PersistedState st = f.state();
    CHECK_EQ(st.epoch, uint64_t{1});
    CHECK_EQ(st.next_in, uint64_t{1});
    CHECK_EQ(st.next_out, uint64_t{1});
    CHECK(f.store->owns_session());
    const ConsistencyReport r = f.store->verify_consistency();
    CHECK_MSG(r.ok, r.problem);
}

SCENARIO("G-01b", "P0", "an outbound commit advances NextNumOut and is readable back") {
    Fixture f("outbound");
    CHECK_EQ(f.store->commit_outbound(app_out(1), 2), StoreError::None);
    CHECK_EQ(f.state().next_out, uint64_t{2});

    std::vector<OutboundRecord> rows;
    CHECK_EQ(f.store->load_outbound_range(1, 1, &rows), StoreError::None);
    CHECK_EQ(rows.size(), size_t{1});
    CHECK_EQ(rows[0].payload, app_out(1).payload);
    const ConsistencyReport r = f.store->verify_consistency();
    CHECK_MSG(r.ok, r.problem);
}

SCENARIO("G-05b", "P0", "an inbound commit advances NextNumIn exactly once per sequence") {
    Fixture f("inbound");
    const InboundRecord r1 = app_in(1);
    CHECK_EQ(f.store->commit_inbound(&r1, 2), StoreError::None);
    CHECK_EQ(f.state().next_in, uint64_t{2});

    // Replaying the same record cannot advance sequencing a second time: the
    // precondition rejects it before the primary key would (invariant I-04).
    CHECK_EQ(f.store->commit_inbound(&r1, 2), StoreError::Inconsistent);
    // A record out of step with NextNumIn is refused rather than reordered.
    const InboundRecord r3 = app_in(3);
    CHECK_EQ(f.store->commit_inbound(&r3, 4), StoreError::Inconsistent);

    // A session-level message advances sequencing without creating an inbox row.
    CHECK_EQ(f.store->commit_inbound(nullptr, 3), StoreError::None);
    CHECK_EQ(f.state().next_in, uint64_t{3});

    std::vector<InboundRecord> pending;
    CHECK_EQ(f.store->load_pending_inbox(10, &pending), StoreError::None);
    CHECK_EQ(pending.size(), size_t{1});
    CHECK_EQ(pending[0].seq, uint64_t{1});
}

SCENARIO("I-02s", "P0", "NextNumOut can never move backwards inside an epoch") {
    Fixture f("monotonic");
    CHECK_EQ(f.store->commit_outbound(app_out(1), 2), StoreError::None);
    CHECK_EQ(f.store->commit_outbound(app_out(2), 3), StoreError::None);
    // A replay of an already issued sequence is rejected (invariant I-02, I-05).
    CHECK_EQ(f.store->commit_outbound(app_out(1), 2), StoreError::Inconsistent);
    CHECK_EQ(f.store->commit_outbound(app_out(2), 3), StoreError::Inconsistent);
    // And so is a commit that would leave NextNumOut unchanged or lower.
    CHECK_EQ(f.store->commit_outbound(app_out(3), 3), StoreError::Inconsistent);
    CHECK_EQ(f.state().next_out, uint64_t{3});
}

SCENARIO("G-06b", "P0", "a pending inbox row survives a reopen and is delivered in order") {
    tt::TempDir d("inbox-restart");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        for (uint64_t i = 1; i <= 5; ++i) {
            const InboundRecord r = app_in(i);
            CHECK_EQ(s->commit_inbound(&r, i + 1), StoreError::None);
        }
        CHECK_EQ(s->mark_delivered(1), StoreError::None);
        CHECK_EQ(s->mark_delivered(2), StoreError::None);
        s->mark_clean_shutdown();
    }
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        CHECK_EQ(st.next_in, uint64_t{6});
        std::vector<InboundRecord> pending;
        CHECK_EQ(s->load_pending_inbox(100, &pending), StoreError::None);
        CHECK_EQ(pending.size(), size_t{3});
        CHECK_EQ(pending[0].seq, uint64_t{3});
        CHECK_EQ(pending[1].seq, uint64_t{4});
        CHECK_EQ(pending[2].seq, uint64_t{5});
    }
}

SCENARIO("G-10", "P0", "a store that reports a full disk does not advance sequence state") {
    Fixture f("disk-full");
    fp::disarm_all();
    // The failpoint stands in for ENOSPC / SQLITE_FULL at the transaction
    // boundary. What is proven here is the engine-visible contract: a failed
    // commit leaves NextNumOut untouched, so no frame can claim that sequence.
    fp::arm(std::string(fp::point::kStoreOutboundBeforeCommit), fp::Action::ReturnError);
    CHECK_EQ(f.store->commit_outbound(app_out(1), 2), StoreError::IoError);
    fp::disarm_all();
    CHECK_EQ(f.state().next_out, uint64_t{1});

    std::vector<OutboundRecord> rows;
    CHECK_EQ(f.store->load_outbound_range(1, 1, &rows), StoreError::None);
    CHECK_EQ(rows.size(), size_t{0});
    const ConsistencyReport r = f.store->verify_consistency();
    CHECK_MSG(r.ok, r.problem);
}

SCENARIO("G-11", "P0", "a failed inbound commit does not advance NextNumIn") {
    Fixture f("inbound-full");
    fp::disarm_all();
    fp::arm(std::string(fp::point::kStoreInboundBeforeCommit), fp::Action::ReturnError);
    const InboundRecord r1 = app_in(1);
    CHECK_EQ(f.store->commit_inbound(&r1, 2), StoreError::IoError);
    fp::disarm_all();
    CHECK_EQ(f.state().next_in, uint64_t{1});
    std::vector<InboundRecord> pending;
    CHECK_EQ(f.store->load_pending_inbox(10, &pending), StoreError::None);
    CHECK_EQ(pending.size(), size_t{0});
}

SCENARIO("G-14", "P0", "a tampered state checksum is refused, never repaired") {
    tt::TempDir d("corrupt-state");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        Fixture* unused = nullptr;
        (void)unused;
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        CHECK_EQ(s->commit_outbound(app_out(1), 2), StoreError::None);
        s->mark_clean_shutdown();
    }
    // Move NextNumOut backwards without recomputing the checksum, the shape of
    // a bit flip or a careless manual fix.
    tamper(path, "UPDATE session_state SET next_out = 1");
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::Corrupt);
    }
}

SCENARIO("G-17", "P0", "state and outbound history disagreeing is detected before Logon") {
    tt::TempDir d("inconsistent-out");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    std::string checksum_sql;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        for (uint64_t i = 1; i <= 3; ++i)
            CHECK_EQ(s->commit_outbound(app_out(i), i + 1), StoreError::None);
        s->mark_clean_shutdown();
    }
    // Add a history row beyond NextNumOut: either a sequence was issued twice
    // or the state row was rolled back to an older snapshot (G-16, G-17).
    tamper(path,
           "INSERT INTO outbound_history(session_id, epoch, seq, msg_type, canonical_payload, "
           "original_sending_time, created_at_utc, tx_state, payload_checksum) "
           "SELECT session_id, epoch, 99, 'U1', x'00', '', '', 'COMMITTED', '0' "
           "FROM session_state LIMIT 1");
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        const ConsistencyReport r = s->verify_consistency();
        CHECK(!r.ok);
        CHECK(r.problem.find("outbound_history") != std::string::npos);
    }
}

SCENARIO("G-18", "P0", "an inbox row beyond NextNumIn is an integrity failure") {
    tt::TempDir d("inconsistent-in");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        const InboundRecord r = app_in(1);
        CHECK_EQ(s->commit_inbound(&r, 2), StoreError::None);
        s->mark_clean_shutdown();
    }
    tamper(path,
           "INSERT INTO inbound_inbox(session_id, epoch, seq, msg_type, canonical_payload, "
           "delivery_state, created_at_utc) "
           "SELECT session_id, epoch, 50, 'U1', x'00', 'PENDING', '' FROM session_state LIMIT 1");
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        const ConsistencyReport r = s->verify_consistency();
        CHECK(!r.ok);
        CHECK(r.problem.find("inbound_inbox") != std::string::npos);
    }
}

SCENARIO("F-17", "P0", "a history row whose payload checksum fails is never replayed") {
    tt::TempDir d("corrupt-history");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        CHECK_EQ(s->commit_outbound(app_out(1), 2), StoreError::None);
        s->mark_clean_shutdown();
    }
    tamper(path, "UPDATE outbound_history SET canonical_payload = x'DEADBEEF' WHERE seq = 1");
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        std::vector<OutboundRecord> rows;
        CHECK_EQ(s->load_outbound_range(1, 1, &rows), StoreError::Corrupt);
        CHECK_EQ(rows.size(), size_t{0});
    }
}

SCENARIO("G-15", "P1", "a database written by another schema version is refused") {
    tt::TempDir d("schema");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        s->mark_clean_shutdown();
    }
    tamper(path, "UPDATE schema_meta SET v = '99' WHERE k = 'schema_version'");
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why),
                 StoreError::SchemaMismatch);
        CHECK(why.find("schema version mismatch") != std::string::npos);
    }
}

SCENARIO("G-21", "P0", "a second process cannot own the same session store") {
    tt::TempDir d("ownership");
    const std::string path = d.file("session.db");
    VirtualClock clock;

    std::unique_ptr<SqliteSessionStore> first;
    std::string why;
    CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &first, &why), StoreError::None);
    CHECK(first->owns_session());

    // Invariant I-01: a second writer is refused, and it is refused BEFORE it
    // can open the database for writing (playbook PB-09).
    std::unique_ptr<SqliteSessionStore> second;
    std::string why2;
    CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &second, &why2),
             StoreError::NotOwner);
    CHECK(second == nullptr);
    CHECK(why2.find("another process owns") != std::string::npos);

    // Once the first releases the session, a new owner may take over.
    first->mark_clean_shutdown();
    first.reset();
    std::unique_ptr<SqliteSessionStore> third;
    CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &third, &why), StoreError::None);
    CHECK(third->owns_session());
}

SCENARIO("G-22", "P0", "an unclean shutdown is visible on the next start") {
    tt::TempDir d("unclean");
    const std::string path = d.file("session.db");
    VirtualClock clock;
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock), &s, &why), StoreError::None);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        CHECK_EQ(s->commit_outbound(app_out(1), 2), StoreError::None);
        // No mark_clean_shutdown: this models a process that died.
    }
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        const ConsistencyReport r = s->verify_consistency();
        CHECK_MSG(r.ok, r.problem);
        // Committed work survived, and the unclean exit is recorded.
        CHECK(!r.clean_shutdown);
        PersistedState st;
        CHECK_EQ(s->load_state(&st), StoreError::None);
        CHECK_EQ(st.next_out, uint64_t{2});
        s->mark_clean_shutdown();
    }
    {
        std::unique_ptr<SqliteSessionStore> s;
        std::string why;
        CHECK_EQ(SqliteSessionStore::open(opts(path, &clock, false), &s, &why), StoreError::None);
        CHECK(s->verify_consistency().clean_shutdown);
    }
}

SCENARIO("L-13", "P0", "a missing store volume fails startup instead of starting empty") {
    tt::TempDir d("novolume");
    VirtualClock clock;
    std::unique_ptr<SqliteSessionStore> s;
    std::string why;
    SqliteSessionStore::Options o = opts(d.missing_dir() + "/session.db", &clock);
    CHECK_EQ(SqliteSessionStore::open(o, &s, &why), StoreError::IoError);
    CHECK(why.find("store directory does not exist") != std::string::npos);

    // And without create_if_missing, an absent database is also a failure
    // rather than a silent fresh start.
    std::unique_ptr<SqliteSessionStore> s2;
    std::string why2;
    CHECK_EQ(SqliteSessionStore::open(opts(d.file("absent.db"), &clock, false), &s2, &why2),
             StoreError::IoError);
    CHECK(why2.find("create_if_missing is false") != std::string::npos);
}

SCENARIO("PB-12s", "P0", "a coordinated reset needs two distinct approvers and bumps the epoch") {
    Fixture f("reset");
    CHECK_EQ(f.store->commit_outbound(app_out(1), 2), StoreError::None);
    const InboundRecord r = app_in(1);
    CHECK_EQ(f.store->commit_inbound(&r, 2), StoreError::None);

    // One operator alone, or the same name twice, cannot reset a session.
    CHECK_EQ(f.store->coordinated_reset("alice", "", "end of day"), StoreError::NotOwner);
    CHECK_EQ(f.store->coordinated_reset("alice", "alice", "end of day"), StoreError::NotOwner);
    CHECK_EQ(f.state().epoch, uint64_t{1});
    CHECK_EQ(f.state().next_out, uint64_t{2});

    CHECK_EQ(f.store->coordinated_reset("alice", "bob", "agreed weekly reset"),
             StoreError::None);
    const PersistedState st = f.state();
    CHECK_EQ(st.epoch, uint64_t{2});
    CHECK_EQ(st.next_in, uint64_t{1});
    CHECK_EQ(st.next_out, uint64_t{1});

    // History from the previous epoch is retained, not deleted: business
    // reconciliation may still need it (design doc 3.4 of project 02).
    std::vector<OutboundRecord> rows;
    CHECK_EQ(f.store->load_outbound_range(1, 10, &rows), StoreError::None);
    CHECK_EQ(rows.size(), size_t{0});  // scoped to the new epoch
    const ConsistencyReport rep = f.store->verify_consistency();
    CHECK_MSG(rep.ok, rep.problem);
}

SCENARIO("G-23", "P1", "a cold restart with ten thousand pending inbox rows is bounded") {
    Fixture f("backlog");
    for (uint64_t i = 1; i <= 10000; ++i) {
        const InboundRecord r = app_in(i);
        CHECK_EQ(f.store->commit_inbound(&r, i + 1), StoreError::None);
    }
    std::vector<InboundRecord> batch;
    CHECK_EQ(f.store->load_pending_inbox(256, &batch), StoreError::None);
    // The dispatcher reads in bounded batches and in sequence order rather than
    // loading the whole backlog into memory (invariant I-09).
    CHECK_EQ(batch.size(), size_t{256});
    CHECK_EQ(batch.front().seq, uint64_t{1});
    CHECK_EQ(batch.back().seq, uint64_t{256});
}

SCENARIO("G-19", "P1", "the write ahead log does not grow without bound under sustained commits") {
    Fixture f("wal");
    for (uint64_t i = 1; i <= 2000; ++i)
        CHECK_EQ(f.store->commit_outbound(app_out(i), i + 1), StoreError::None);
    std::error_code ec;
    const auto wal = std::filesystem::path(f.path + "-wal");
    const uintmax_t wal_bytes = std::filesystem::exists(wal, ec)
                                    ? std::filesystem::file_size(wal, ec)
                                    : uintmax_t{0};
    // SQLite checkpoints automatically; the assertion is that the log plateaus
    // rather than tracking total commits.
    CHECK_MSG(wal_bytes < 8u * 1024u * 1024u,
              std::string("wal bytes = ") + std::to_string(wal_bytes));
    CHECK_EQ(f.state().next_out, uint64_t{2001});
}

int main(int argc, char** argv) {
    fixse::fp::arm_from_env();
    return tt::run_all(argc, argv, "store");
}
