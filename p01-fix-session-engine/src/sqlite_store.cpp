#include "fix/sqlite_store.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "fix/failpoint.hpp"
#include "fix/ownership.hpp"

namespace fixse {

std::string_view to_string(AuditEvent e) {
    switch (e) {
        case AuditEvent::StoreOpened: return "StoreOpened";
        case AuditEvent::OwnershipAcquired: return "OwnershipAcquired";
        case AuditEvent::OwnershipRefused: return "OwnershipRefused";
        case AuditEvent::UncleanShutdownDetected: return "UncleanShutdownDetected";
        case AuditEvent::StateTransition: return "StateTransition";
        case AuditEvent::GapDetected: return "GapDetected";
        case AuditEvent::ResendRequested: return "ResendRequested";
        case AuditEvent::ResendServed: return "ResendServed";
        case AuditEvent::GapFillSent: return "GapFillSent";
        case AuditEvent::GapFillReceived: return "GapFillReceived";
        case AuditEvent::GapFillQuarantined: return "GapFillQuarantined";
        case AuditEvent::DuplicateIgnored: return "DuplicateIgnored";
        case AuditEvent::SequenceResetRefused: return "SequenceResetRefused";
        case AuditEvent::CoordinatedResetApplied: return "CoordinatedResetApplied";
        case AuditEvent::IntegrityFailure: return "IntegrityFailure";
        case AuditEvent::Faulted: return "Faulted";
        case AuditEvent::LogonRejected: return "LogonRejected";
        case AuditEvent::TestRequestTimeout: return "TestRequestTimeout";
        case AuditEvent::InboxOverflow: return "InboxOverflow";
        case AuditEvent::ResendStormBlocked: return "ResendStormBlocked";
    }
    return "Unknown";
}

namespace {

// Map SQLite result codes onto the engine taxonomy. The distinction matters:
// Busy is retryable, Full and IoError are fail-closed, Corrupt must never lead
// to creating a fresh database (G-10, G-11, G-12, G-13, G-14).
StoreError map_sqlite(int rc) {
    switch (rc & 0xFF) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return StoreError::None;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return StoreError::Busy;
        case SQLITE_FULL:
            return StoreError::Full;
        case SQLITE_IOERR:
        case SQLITE_READONLY:
        case SQLITE_PERM:
        case SQLITE_CANTOPEN:
            return StoreError::IoError;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return StoreError::Corrupt;
        case SQLITE_CONSTRAINT:
            return StoreError::Conflict;
        default:
            return StoreError::Unknown;
    }
}

// FNV-1a over a canonical rendering of the state row. Not a cryptographic MAC:
// it detects accidental corruption and casual tampering, and the design doc
// does not claim more than tamper evidence here (section 18 of the doc).
std::string fnv1a_hex(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s != nullptr) sqlite3_finalize(s);
    }
    int prepare(sqlite3* db, const char* sql) { return sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
};

void bind_text(sqlite3_stmt* s, int i, std::string_view v) {
    sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void bind_u64(sqlite3_stmt* s, int i, uint64_t v) {
    sqlite3_bind_int64(s, i, static_cast<sqlite3_int64>(v));
}

std::string column_text(sqlite3_stmt* s, int i) {
    const unsigned char* p = sqlite3_column_text(s, i);
    const int n = sqlite3_column_bytes(s, i);
    if (p == nullptr || n <= 0) return std::string();
    return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
}

uint64_t column_u64(sqlite3_stmt* s, int i) {
    return static_cast<uint64_t>(sqlite3_column_int64(s, i));
}

}  // namespace

StoreError SqliteSessionStore::exec(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);
    return map_sqlite(rc);
}

std::string SqliteSessionStore::state_checksum(const PersistedState& st) const {
    std::string canon;
    canon.reserve(128);
    canon += session_id_;
    canon += '|';
    canon += std::to_string(st.epoch);
    canon += '|';
    canon += std::to_string(st.next_in);
    canon += '|';
    canon += std::to_string(st.next_out);
    canon += '|';
    canon += std::to_string(st.version);
    return fnv1a_hex(canon);
}

StoreError SqliteSessionStore::open(const Options& opt, std::unique_ptr<SqliteSessionStore>* out,
                                    std::string* why) {
    const auto set_why = [&](std::string m) {
        if (why != nullptr) *why = std::move(m);
    };

    const std::filesystem::path db_path(opt.path);
    const std::filesystem::path dir =
        db_path.has_parent_path() ? db_path.parent_path() : std::filesystem::path(".");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        // The durable store directory is a deployment input, not something the
        // engine conjures: a missing volume is a startup failure (L-13).
        set_why("store directory does not exist: " + dir.string());
        return StoreError::IoError;
    }
    const bool existed = std::filesystem::exists(db_path, ec);
    if (!existed && !opt.create_if_missing) {
        set_why("database does not exist and create_if_missing is false: " + db_path.string());
        return StoreError::IoError;
    }

    std::unique_ptr<SqliteSessionStore> st(new SqliteSessionStore());
    st->key_ = opt.key;
    st->session_id_ = opt.key.id();
    st->clock_ = opt.clock;

    // Ownership first: a second writer must never even open the database for
    // writing (invariant I-01, playbook PB-09).
    st->lock_ = std::make_unique<OwnershipLock>();
    if (!st->lock_->acquire(opt.path + ".lock")) {
        set_why("another process owns this session store: " + opt.path + ".lock");
        return StoreError::NotOwner;
    }

    int flags = SQLITE_OPEN_READWRITE;
    if (opt.create_if_missing) flags |= SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(opt.path.c_str(), &st->db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        set_why(std::string("sqlite3_open_v2 failed: ") +
                (st->db_ != nullptr ? sqlite3_errmsg(st->db_) : "unknown"));
        return map_sqlite(rc);
    }

    StoreError e = st->apply_pragmas(opt, why);
    if (e != StoreError::None) return e;
    e = st->check_integrity(why);
    if (e != StoreError::None) return e;
    e = st->ensure_schema(why);
    if (e != StoreError::None) return e;

    // Detect an unclean previous run and record it before anything else
    // mutates state (G-22).
    {
        Stmt q;
        if (q.prepare(st->db_, "SELECT clean_shutdown FROM owner_lease WHERE session_id = ?") ==
            SQLITE_OK) {
            bind_text(q.s, 1, st->session_id_);
            if (sqlite3_step(q.s) == SQLITE_ROW)
                st->clean_shutdown_on_open_ = column_u64(q.s, 0) != 0;
        }
    }

    {
        Stmt u;
        if (u.prepare(st->db_,
                      "INSERT INTO owner_lease(session_id, owner_token, pid, acquired_at_utc, "
                      "clean_shutdown) VALUES(?,?,?,?,0) "
                      "ON CONFLICT(session_id) DO UPDATE SET owner_token=excluded.owner_token, "
                      "pid=excluded.pid, acquired_at_utc=excluded.acquired_at_utc, "
                      "clean_shutdown=0") != SQLITE_OK) {
            set_why("cannot record ownership lease");
            return StoreError::IoError;
        }
        bind_text(u.s, 1, st->session_id_);
        bind_text(u.s, 2, fnv1a_hex(st->session_id_ + std::to_string(reinterpret_cast<uintptr_t>(st.get()))));
        bind_u64(u.s, 3, 0);
        bind_text(u.s, 4, st->clock_ != nullptr ? format_utc_timestamp(st->clock_->wall_now())
                                                : std::string("unknown"));
        if (sqlite3_step(u.s) != SQLITE_DONE) {
            set_why("cannot write ownership lease");
            return StoreError::IoError;
        }
    }

    st->append_audit(AuditRecord{AuditEvent::StoreOpened, 0, 0, 0,
                                 existed ? "opened existing store" : "created new store"});
    st->append_audit(AuditRecord{AuditEvent::OwnershipAcquired, 0, 0, 0, st->session_id_});
    if (!st->clean_shutdown_on_open_)
        st->append_audit(AuditRecord{AuditEvent::UncleanShutdownDetected, 0, 0, 0,
                                     "previous run did not flag a clean shutdown"});

    *out = std::move(st);
    return StoreError::None;
}

SqliteSessionStore::~SqliteSessionStore() {
    if (db_ != nullptr) sqlite3_close(db_);
    lock_.reset();
}

StoreError SqliteSessionStore::apply_pragmas(const Options& opt, std::string* why) {
    sqlite3_busy_timeout(db_, opt.busy_timeout_ms);
    const std::string jm = "PRAGMA journal_mode=" + opt.journal_mode + ";";
    char* err = nullptr;
    // journal_mode returns a row, so use exec with a callback-free call and then
    // read it back to confirm the mode actually took effect.
    sqlite3_exec(db_, jm.c_str(), nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);

    Stmt q;
    if (q.prepare(db_, "PRAGMA journal_mode") != SQLITE_OK) return StoreError::IoError;
    std::string actual;
    if (sqlite3_step(q.s) == SQLITE_ROW) actual = column_text(q.s, 0);
    for (char& c : actual) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    std::string want = opt.journal_mode;
    for (char& c : want) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (actual != want) {
        if (why != nullptr)
            *why = "journal_mode is " + actual + " but " + want + " was requested";
        return StoreError::IoError;
    }

    const std::string sync = "PRAGMA synchronous=" + opt.synchronous + ";";
    StoreError e = exec(sync.c_str());
    if (e != StoreError::None) return e;
    return exec("PRAGMA foreign_keys=ON;");
}

StoreError SqliteSessionStore::check_integrity(std::string* why) {
    Stmt q;
    if (q.prepare(db_, "PRAGMA integrity_check") != SQLITE_OK) return StoreError::Corrupt;
    std::string first;
    if (sqlite3_step(q.s) == SQLITE_ROW) first = column_text(q.s, 0);
    if (first != "ok") {
        if (why != nullptr) *why = "sqlite integrity_check: " + first;
        return StoreError::Corrupt;
    }
    return StoreError::None;
}

StoreError SqliteSessionStore::ensure_schema(std::string* why) {
    StoreError e = exec(
        "CREATE TABLE IF NOT EXISTS schema_meta("
        "  k TEXT PRIMARY KEY, v TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS session_state("
        "  session_id TEXT PRIMARY KEY,"
        "  epoch INTEGER NOT NULL,"
        "  next_in INTEGER NOT NULL,"
        "  next_out INTEGER NOT NULL,"
        "  last_good_connection_generation INTEGER NOT NULL,"
        "  version INTEGER NOT NULL,"
        "  updated_at_utc TEXT NOT NULL,"
        "  integrity_checksum TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS outbound_history("
        "  session_id TEXT NOT NULL,"
        "  epoch INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  msg_type TEXT NOT NULL,"
        "  canonical_payload BLOB NOT NULL,"
        "  original_sending_time TEXT NOT NULL,"
        "  created_at_utc TEXT NOT NULL,"
        "  tx_state TEXT NOT NULL,"
        "  payload_checksum TEXT NOT NULL,"
        "  PRIMARY KEY(session_id, epoch, seq));"
        "CREATE TABLE IF NOT EXISTS inbound_inbox("
        "  session_id TEXT NOT NULL,"
        "  epoch INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  msg_type TEXT NOT NULL,"
        "  canonical_payload BLOB NOT NULL,"
        "  delivery_state TEXT NOT NULL,"
        "  created_at_utc TEXT NOT NULL,"
        "  PRIMARY KEY(session_id, epoch, seq));"
        "CREATE TABLE IF NOT EXISTS recovery_audit("
        "  event_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL,"
        "  epoch INTEGER NOT NULL,"
        "  event_type TEXT NOT NULL,"
        "  seq_from INTEGER NOT NULL,"
        "  seq_to INTEGER NOT NULL,"
        "  reason TEXT NOT NULL,"
        "  created_at_utc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS owner_lease("
        "  session_id TEXT PRIMARY KEY,"
        "  owner_token TEXT NOT NULL,"
        "  pid INTEGER NOT NULL,"
        "  acquired_at_utc TEXT NOT NULL,"
        "  clean_shutdown INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_inbox_pending"
        "  ON inbound_inbox(session_id, epoch, delivery_state, seq);");
    if (e != StoreError::None) {
        if (why != nullptr) *why = "cannot create schema";
        return e;
    }

    Stmt q;
    if (q.prepare(db_, "SELECT v FROM schema_meta WHERE k = 'schema_version'") != SQLITE_OK)
        return StoreError::IoError;
    std::string found;
    if (sqlite3_step(q.s) == SQLITE_ROW) found = column_text(q.s, 0);

    if (found.empty()) {
        Stmt ins;
        if (ins.prepare(db_, "INSERT INTO schema_meta(k, v) VALUES('schema_version', ?)") !=
            SQLITE_OK)
            return StoreError::IoError;
        bind_text(ins.s, 1, std::to_string(kSchemaVersion));
        if (sqlite3_step(ins.s) != SQLITE_DONE) return StoreError::IoError;
        return StoreError::None;
    }

    // A database written by a different schema version is refused: a best
    // effort read of an unknown layout is how sequence state gets silently
    // corrupted (G-15).
    if (found != std::to_string(kSchemaVersion)) {
        if (why != nullptr)
            *why = "schema version mismatch: store has " + found + ", engine expects " +
                   std::to_string(kSchemaVersion);
        return StoreError::SchemaMismatch;
    }
    return StoreError::None;
}

StoreError SqliteSessionStore::load_state(PersistedState* out) {
    Stmt q;
    if (q.prepare(db_,
                  "SELECT epoch, next_in, next_out, last_good_connection_generation, version, "
                  "integrity_checksum FROM session_state WHERE session_id = ?") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(q.s, 1, session_id_);

    const int rc = sqlite3_step(q.s);
    if (rc == SQLITE_ROW) {
        PersistedState st;
        st.epoch = column_u64(q.s, 0);
        st.next_in = column_u64(q.s, 1);
        st.next_out = column_u64(q.s, 2);
        st.last_good_connection_generation = column_u64(q.s, 3);
        st.version = column_u64(q.s, 4);
        const std::string stored = column_text(q.s, 5);

        if (stored != state_checksum(st)) {
            // Do not repair, do not start fresh: a state row that fails its own
            // checksum means the sequence numbers cannot be trusted (G-14).
            append_audit(AuditRecord{AuditEvent::IntegrityFailure, st.epoch, st.next_in,
                                     st.next_out, "session_state checksum mismatch"});
            return StoreError::Corrupt;
        }
        if (st.epoch < 1 || st.next_in < 1 || st.next_out < 1) {
            append_audit(AuditRecord{AuditEvent::IntegrityFailure, st.epoch, st.next_in,
                                     st.next_out, "sequence or epoch below one"});
            return StoreError::Inconsistent;
        }
        state_ = st;
        state_loaded_ = true;
        *out = st;
        return StoreError::None;
    }
    if (rc != SQLITE_DONE) return map_sqlite(rc);

    // First start for this session identity.
    PersistedState st;
    const StoreError e = write_state_locked(st);
    if (e != StoreError::None) return e;
    state_ = st;
    state_loaded_ = true;
    *out = st;
    return StoreError::None;
}

StoreError SqliteSessionStore::write_state_locked(const PersistedState& st) {
    Stmt u;
    if (u.prepare(db_,
                  "INSERT INTO session_state(session_id, epoch, next_in, next_out, "
                  "last_good_connection_generation, version, updated_at_utc, integrity_checksum) "
                  "VALUES(?,?,?,?,?,?,?,?) "
                  "ON CONFLICT(session_id) DO UPDATE SET epoch=excluded.epoch, "
                  "next_in=excluded.next_in, next_out=excluded.next_out, "
                  "last_good_connection_generation=excluded.last_good_connection_generation, "
                  "version=excluded.version, updated_at_utc=excluded.updated_at_utc, "
                  "integrity_checksum=excluded.integrity_checksum") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(u.s, 1, session_id_);
    bind_u64(u.s, 2, st.epoch);
    bind_u64(u.s, 3, st.next_in);
    bind_u64(u.s, 4, st.next_out);
    bind_u64(u.s, 5, st.last_good_connection_generation);
    bind_u64(u.s, 6, st.version);
    bind_text(u.s, 7,
              clock_ != nullptr ? format_utc_timestamp(clock_->wall_now()) : std::string("unknown"));
    bind_text(u.s, 8, state_checksum(st));
    const int rc = sqlite3_step(u.s);
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    return StoreError::None;
}

StoreError SqliteSessionStore::commit_outbound(const OutboundRecord& rec, uint64_t next_out) {
    if (!state_loaded_) return StoreError::Inconsistent;
    // NextNumOut only ever moves forward inside an epoch (invariant I-02), and
    // an original frame always consumes exactly the sequence it was given.
    if (next_out <= state_.next_out || rec.seq != state_.next_out) return StoreError::Inconsistent;

    if (fp::fire(fp::point::kStoreOutboundBeforeBegin) == fp::Action::ReturnError)
        return StoreError::IoError;

    StoreError e = exec("BEGIN IMMEDIATE");
    if (e != StoreError::None) return e;

    const auto rollback = [&](StoreError err) {
        exec("ROLLBACK");
        return err;
    };

    {
        Stmt ins;
        if (ins.prepare(db_,
                        "INSERT INTO outbound_history(session_id, epoch, seq, msg_type, "
                        "canonical_payload, original_sending_time, created_at_utc, tx_state, "
                        "payload_checksum) VALUES(?,?,?,?,?,?,?,'COMMITTED',?)") != SQLITE_OK)
            return rollback(StoreError::IoError);
        bind_text(ins.s, 1, session_id_);
        bind_u64(ins.s, 2, state_.epoch);
        bind_u64(ins.s, 3, rec.seq);
        bind_text(ins.s, 4, rec.msg_type);
        sqlite3_bind_blob(ins.s, 5, rec.payload.data(), static_cast<int>(rec.payload.size()),
                          SQLITE_TRANSIENT);
        bind_text(ins.s, 6, rec.original_sending_time);
        bind_text(ins.s, 7,
                  clock_ != nullptr ? format_utc_timestamp(clock_->wall_now())
                                    : std::string("unknown"));
        bind_text(ins.s, 8, fnv1a_hex(rec.payload));
        const int rc = sqlite3_step(ins.s);
        if (rc != SQLITE_DONE) return rollback(map_sqlite(rc));
    }

    if (fp::fire(fp::point::kStoreOutboundAfterInsert) == fp::Action::ReturnError)
        return rollback(StoreError::IoError);

    PersistedState next = state_;
    next.next_out = next_out;
    next.version = state_.version + 1;
    e = write_state_locked(next);
    if (e != StoreError::None) return rollback(e);

    if (fp::fire(fp::point::kStoreOutboundAfterUpdate) == fp::Action::ReturnError)
        return rollback(StoreError::IoError);
    if (fp::fire(fp::point::kStoreOutboundBeforeCommit) == fp::Action::ReturnError)
        return rollback(StoreError::IoError);

    e = exec("COMMIT");
    if (e != StoreError::None) return rollback(e);

    state_ = next;
    // A crash here has already made the record durable: the restart must find
    // seq in history and NextNumOut advanced, and must not reissue the sequence
    // with new content (playbook PB-01).
    fp::fire(fp::point::kStoreOutboundAfterCommit);
    return StoreError::None;
}

StoreError SqliteSessionStore::commit_inbound(const InboundRecord* rec, uint64_t next_in) {
    if (!state_loaded_) return StoreError::Inconsistent;
    if (next_in <= state_.next_in) return StoreError::Inconsistent;
    if (rec != nullptr && rec->seq != state_.next_in) return StoreError::Inconsistent;

    if (fp::fire(fp::point::kStoreInboundBeforeBegin) == fp::Action::ReturnError)
        return StoreError::IoError;

    StoreError e = exec("BEGIN IMMEDIATE");
    if (e != StoreError::None) return e;
    const auto rollback = [&](StoreError err) {
        exec("ROLLBACK");
        return err;
    };

    if (rec != nullptr) {
        Stmt ins;
        if (ins.prepare(db_,
                        "INSERT INTO inbound_inbox(session_id, epoch, seq, msg_type, "
                        "canonical_payload, delivery_state, created_at_utc) "
                        "VALUES(?,?,?,?,?,'PENDING',?)") != SQLITE_OK)
            return rollback(StoreError::IoError);
        bind_text(ins.s, 1, session_id_);
        bind_u64(ins.s, 2, state_.epoch);
        bind_u64(ins.s, 3, rec->seq);
        bind_text(ins.s, 4, rec->msg_type);
        sqlite3_bind_blob(ins.s, 5, rec->payload.data(), static_cast<int>(rec->payload.size()),
                          SQLITE_TRANSIENT);
        bind_text(ins.s, 6,
                  clock_ != nullptr ? format_utc_timestamp(clock_->wall_now())
                                    : std::string("unknown"));
        const int rc = sqlite3_step(ins.s);
        // The primary key makes a second row for the same (session, epoch, seq)
        // impossible, which is what makes duplicate suppression a database
        // guarantee rather than an in-memory hope (invariant I-04).
        if (rc != SQLITE_DONE) return rollback(map_sqlite(rc));
    }

    if (fp::fire(fp::point::kStoreInboundAfterInsert) == fp::Action::ReturnError)
        return rollback(StoreError::IoError);

    PersistedState next = state_;
    next.next_in = next_in;
    next.version = state_.version + 1;
    e = write_state_locked(next);
    if (e != StoreError::None) return rollback(e);

    if (fp::fire(fp::point::kStoreInboundBeforeCommit) == fp::Action::ReturnError)
        return rollback(StoreError::IoError);

    e = exec("COMMIT");
    if (e != StoreError::None) return rollback(e);

    state_ = next;
    fp::fire(fp::point::kStoreInboundAfterCommit);
    return StoreError::None;
}

StoreError SqliteSessionStore::mark_delivered(uint64_t seq) {
    if (fp::fire(fp::point::kStoreMarkDelivered) == fp::Action::ReturnError)
        return StoreError::IoError;
    Stmt u;
    if (u.prepare(db_,
                  "UPDATE inbound_inbox SET delivery_state='DELIVERED' "
                  "WHERE session_id=? AND epoch=? AND seq=?") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(u.s, 1, session_id_);
    bind_u64(u.s, 2, state_.epoch);
    bind_u64(u.s, 3, seq);
    const int rc = sqlite3_step(u.s);
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    return StoreError::None;
}

StoreError SqliteSessionStore::load_outbound_range(uint64_t from_seq, uint64_t to_seq,
                                                   std::vector<OutboundRecord>* out) {
    out->clear();
    if (from_seq == 0 || to_seq < from_seq) return StoreError::Inconsistent;
    Stmt q;
    if (q.prepare(db_,
                  "SELECT seq, msg_type, canonical_payload, original_sending_time, "
                  "payload_checksum FROM outbound_history "
                  "WHERE session_id=? AND epoch=? AND seq>=? AND seq<=? ORDER BY seq ASC") !=
        SQLITE_OK)
        return StoreError::IoError;
    bind_text(q.s, 1, session_id_);
    bind_u64(q.s, 2, state_.epoch);
    bind_u64(q.s, 3, from_seq);
    bind_u64(q.s, 4, to_seq);

    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) {
        OutboundRecord r;
        r.seq = column_u64(q.s, 0);
        r.msg_type = column_text(q.s, 1);
        const void* blob = sqlite3_column_blob(q.s, 2);
        const int n = sqlite3_column_bytes(q.s, 2);
        if (blob != nullptr && n > 0)
            r.payload.assign(static_cast<const char*>(blob), static_cast<size_t>(n));
        r.original_sending_time = column_text(q.s, 3);
        const std::string stored_sum = column_text(q.s, 4);
        if (stored_sum != fnv1a_hex(r.payload)) {
            // Never retransmit a history row that fails its own checksum: a
            // corrupt replay is worse than an admitted gap (F-17).
            out->clear();
            append_audit(AuditRecord{AuditEvent::IntegrityFailure, state_.epoch, r.seq, r.seq,
                                     "outbound_history payload checksum mismatch"});
            return StoreError::Corrupt;
        }
        out->push_back(std::move(r));
    }
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    return StoreError::None;
}

StoreError SqliteSessionStore::load_pending_inbox(size_t max_rows, std::vector<InboundRecord>* out) {
    out->clear();
    Stmt q;
    if (q.prepare(db_,
                  "SELECT seq, msg_type, canonical_payload FROM inbound_inbox "
                  "WHERE session_id=? AND epoch=? AND delivery_state='PENDING' "
                  "ORDER BY seq ASC LIMIT ?") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(q.s, 1, session_id_);
    bind_u64(q.s, 2, state_.epoch);
    bind_u64(q.s, 3, max_rows);

    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) {
        InboundRecord r;
        r.seq = column_u64(q.s, 0);
        r.msg_type = column_text(q.s, 1);
        const void* blob = sqlite3_column_blob(q.s, 2);
        const int n = sqlite3_column_bytes(q.s, 2);
        if (blob != nullptr && n > 0)
            r.payload.assign(static_cast<const char*>(blob), static_cast<size_t>(n));
        r.delivery_state = DeliveryState::Pending;
        out->push_back(std::move(r));
    }
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    return StoreError::None;
}

StoreError SqliteSessionStore::load_inbox_record(uint64_t seq, InboundRecord* out,
                                                 bool* found) {
    *found = false;
    Stmt q;
    if (q.prepare(db_,
                  "SELECT seq, msg_type, canonical_payload, delivery_state FROM inbound_inbox "
                  "WHERE session_id=? AND epoch=? AND seq=?") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(q.s, 1, session_id_);
    bind_u64(q.s, 2, state_.epoch);
    bind_u64(q.s, 3, seq);
    const int rc = sqlite3_step(q.s);
    if (rc == SQLITE_DONE) return StoreError::None;
    if (rc != SQLITE_ROW) return map_sqlite(rc);

    out->seq = column_u64(q.s, 0);
    out->msg_type = column_text(q.s, 1);
    const void* blob = sqlite3_column_blob(q.s, 2);
    const int n = sqlite3_column_bytes(q.s, 2);
    out->payload.clear();
    if (blob != nullptr && n > 0)
        out->payload.assign(static_cast<const char*>(blob), static_cast<size_t>(n));
    out->delivery_state =
        column_text(q.s, 3) == "DELIVERED" ? DeliveryState::Delivered : DeliveryState::Pending;
    *found = true;
    return StoreError::None;
}

StoreError SqliteSessionStore::append_audit(const AuditRecord& rec) {
    Stmt ins;
    if (ins.prepare(db_,
                    "INSERT INTO recovery_audit(session_id, epoch, event_type, seq_from, seq_to, "
                    "reason, created_at_utc) VALUES(?,?,?,?,?,?,?)") != SQLITE_OK)
        return StoreError::IoError;
    bind_text(ins.s, 1, session_id_);
    bind_u64(ins.s, 2, rec.epoch);
    bind_text(ins.s, 3, to_string(rec.event));
    bind_u64(ins.s, 4, rec.seq_from);
    bind_u64(ins.s, 5, rec.seq_to);
    bind_text(ins.s, 6, rec.reason);
    bind_text(ins.s, 7,
              clock_ != nullptr ? format_utc_timestamp(clock_->wall_now()) : std::string("unknown"));
    const int rc = sqlite3_step(ins.s);
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    ++audit_rows_;
    return StoreError::None;
}

StoreError SqliteSessionStore::record_connection_generation(uint64_t generation) {
    if (!state_loaded_) return StoreError::Inconsistent;
    PersistedState next = state_;
    next.last_good_connection_generation = generation;
    next.version = state_.version + 1;
    const StoreError e = write_state_locked(next);
    if (e != StoreError::None) return e;
    state_ = next;
    return StoreError::None;
}

StoreError SqliteSessionStore::coordinated_reset(const std::string& approver_a,
                                                 const std::string& approver_b,
                                                 const std::string& reason) {
    // Dual control: one operator cannot reset a session alone, and the reset is
    // always visible in the audit trail (J-15, K-11).
    if (approver_a.empty() || approver_b.empty() || approver_a == approver_b)
        return StoreError::NotOwner;
    if (!state_loaded_) return StoreError::Inconsistent;

    StoreError e = exec("BEGIN IMMEDIATE");
    if (e != StoreError::None) return e;

    PersistedState next;
    next.epoch = state_.epoch + 1;
    next.next_in = 1;
    next.next_out = 1;
    next.last_good_connection_generation = state_.last_good_connection_generation;
    next.version = state_.version + 1;
    e = write_state_locked(next);
    if (e != StoreError::None) {
        exec("ROLLBACK");
        return e;
    }
    e = exec("COMMIT");
    if (e != StoreError::None) {
        exec("ROLLBACK");
        return e;
    }
    const PersistedState before = state_;
    state_ = next;
    append_audit(AuditRecord{AuditEvent::CoordinatedResetApplied, next.epoch, before.next_in,
                             before.next_out,
                             "approved by " + approver_a + " and " + approver_b + ": " + reason});
    return StoreError::None;
}

ConsistencyReport SqliteSessionStore::verify_consistency() {
    ConsistencyReport r;
    r.clean_shutdown = clean_shutdown_on_open_;

    PersistedState st;
    const StoreError e = load_state(&st);
    if (e != StoreError::None) {
        r.problem = "state unreadable or failed its checksum: " + std::string(to_string(e));
        return r;
    }
    r.integrity_checksum_valid = true;

    const auto scalar = [&](const char* sql, uint64_t* out) {
        Stmt q;
        if (q.prepare(db_, sql) != SQLITE_OK) return false;
        bind_text(q.s, 1, session_id_);
        bind_u64(q.s, 2, st.epoch);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        *out = column_u64(q.s, 0);
        return true;
    };

    uint64_t pending = 0;
    if (!scalar("SELECT COALESCE(MAX(seq),0) FROM outbound_history WHERE session_id=? AND epoch=?",
                &r.max_outbound_seq) ||
        !scalar("SELECT COALESCE(MAX(seq),0) FROM inbound_inbox WHERE session_id=? AND epoch=?",
                &r.max_inbox_seq) ||
        !scalar("SELECT COUNT(*) FROM inbound_inbox WHERE session_id=? AND epoch=? AND "
                "delivery_state='PENDING'",
                &pending)) {
        r.problem = "cannot scan history tables";
        return r;
    }
    r.pending_inbox_rows = static_cast<size_t>(pending);

    // State and history must agree. A history row at or beyond NextNumOut means
    // a sequence number was issued twice or the state row was rolled back
    // (G-16, G-17); an inbox row at or beyond NextNumIn means the same on the
    // inbound side (G-18).
    if (r.max_outbound_seq >= st.next_out) {
        r.problem = "outbound_history holds seq " + std::to_string(r.max_outbound_seq) +
                    " but next_out is " + std::to_string(st.next_out);
        return r;
    }
    if (r.max_inbox_seq >= st.next_in) {
        r.problem = "inbound_inbox holds seq " + std::to_string(r.max_inbox_seq) +
                    " but next_in is " + std::to_string(st.next_in);
        return r;
    }

    r.ok = true;
    return r;
}

bool SqliteSessionStore::owns_session() const { return lock_ != nullptr && lock_->held(); }

void SqliteSessionStore::mark_clean_shutdown() {
    Stmt u;
    if (u.prepare(db_, "UPDATE owner_lease SET clean_shutdown=1 WHERE session_id=?") != SQLITE_OK)
        return;
    bind_text(u.s, 1, session_id_);
    sqlite3_step(u.s);
}

size_t SqliteSessionStore::audit_count() const { return audit_rows_; }

}  // namespace fixse
