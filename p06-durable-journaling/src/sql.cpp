#include "jrnl/sql.hpp"

#include <sqlite3.h>

#include <filesystem>

#include "jrnl/failpoint.hpp"

namespace jrnl {

StoreError map_sqlite(int rc) {
    switch (rc & 0xFF) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return StoreError::None;
        case SQLITE_BUSY:
            // SQLite reports a write-write conflict as BUSY where PostgreSQL
            // would report 40001. Both mean "retry the same transaction with
            // the same key", which is the distinction JI-27 cares about.
            return StoreError::Serialization;
        case SQLITE_LOCKED:
            return StoreError::Deadlock;
        case SQLITE_FULL:
            return StoreError::Full;
        case SQLITE_IOERR:
        case SQLITE_READONLY:
        case SQLITE_PERM:
        case SQLITE_CANTOPEN:
        case SQLITE_AUTH:
            return StoreError::IoError;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return StoreError::Corrupt;
        case SQLITE_CONSTRAINT:
            return StoreError::Conflict;
        case SQLITE_MISMATCH:
        case SQLITE_SCHEMA:
            return StoreError::SchemaMismatch;
        default:
            return StoreError::Unknown;
    }
}

// --- Stmt ------------------------------------------------------------------

Stmt::~Stmt() {
    if (s_ != nullptr) sqlite3_finalize(s_);
}

Stmt::Stmt(Stmt&& o) noexcept : s_(o.s_), db_(o.db_) {
    o.s_ = nullptr;
    o.db_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& o) noexcept {
    if (this != &o) {
        if (s_ != nullptr) sqlite3_finalize(s_);
        s_ = o.s_;
        db_ = o.db_;
        o.s_ = nullptr;
        o.db_ = nullptr;
    }
    return *this;
}

void Stmt::bind_text(int i, std::string_view v) {
    sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void Stmt::bind_i64(int i, std::int64_t v) {
    sqlite3_bind_int64(s_, i, static_cast<sqlite3_int64>(v));
}

void Stmt::bind_u64(int i, std::uint64_t v) {
    sqlite3_bind_int64(s_, i, static_cast<sqlite3_int64>(v));
}

void Stmt::bind_null(int i) { sqlite3_bind_null(s_, i); }

void Stmt::bind_hex(int i, const Digest& d) { bind_text(i, to_hex(d)); }

StoreError Stmt::step(bool* have_row) {
    const int rc = sqlite3_step(s_);
    if (have_row != nullptr) *have_row = (rc == SQLITE_ROW);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return StoreError::None;
    return map_sqlite(rc);
}

StoreError Stmt::run() {
    bool row = false;
    const StoreError e = step(&row);
    reset();
    return e;
}

std::string Stmt::col_text(int i) const {
    const unsigned char* p = sqlite3_column_text(s_, i);
    const int n = sqlite3_column_bytes(s_, i);
    if (p == nullptr || n <= 0) return std::string();
    return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
}

std::int64_t Stmt::col_i64(int i) const { return sqlite3_column_int64(s_, i); }

std::uint64_t Stmt::col_u64(int i) const {
    return static_cast<std::uint64_t>(sqlite3_column_int64(s_, i));
}

bool Stmt::col_is_null(int i) const { return sqlite3_column_type(s_, i) == SQLITE_NULL; }

bool Stmt::col_hex(int i, Digest* out) const { return from_hex(col_text(i), out); }

void Stmt::reset() {
    if (s_ != nullptr) {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }
}

// --- Db --------------------------------------------------------------------

Db::~Db() {
    if (db_ != nullptr) sqlite3_close(db_);
}

StoreError Db::open(const Options& opt, std::unique_ptr<Db>* out, std::string* why) {
    auto db = std::unique_ptr<Db>(new Db());
    db->path_ = opt.path;

    if (!opt.create_if_missing && !opt.path.empty() && opt.path != ":memory:") {
        std::error_code ec;
        if (!std::filesystem::exists(opt.path, ec)) {
            // Refusing to invent a database is a safety property, not an
            // inconvenience: a process that silently starts from an empty
            // journal looks healthy while having lost every committed event.
            if (why != nullptr) *why = "database does not exist and create_if_missing is false";
            return StoreError::IoError;
        }
    }

    int flags = opt.read_only ? SQLITE_OPEN_READONLY
                              : (SQLITE_OPEN_READWRITE |
                                 (opt.create_if_missing ? SQLITE_OPEN_CREATE : 0));
    const int rc = sqlite3_open_v2(opt.path.c_str(), &db->db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (why != nullptr) {
            *why = db->db_ != nullptr ? sqlite3_errmsg(db->db_) : "sqlite3_open_v2 failed";
        }
        return map_sqlite(rc);
    }

    sqlite3_busy_timeout(db->db_, opt.busy_timeout_ms);

    if (!opt.read_only) {
        const std::string jm = "PRAGMA journal_mode=" + opt.journal_mode + ";";
        StoreError e = db->exec(jm);
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot set journal_mode: " + db->last_error();
            return e;
        }
        const std::string sy = "PRAGMA synchronous=" + opt.synchronous + ";";
        e = db->exec(sy);
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot set synchronous: " + db->last_error();
            return e;
        }
    }
    // Composite tenant-aware foreign keys only guard anything when the engine
    // enforces them. Section 8 note 5 asks for exactly this.
    StoreError e = db->exec("PRAGMA foreign_keys=ON;");
    if (e != StoreError::None) {
        if (why != nullptr) *why = "cannot enable foreign keys: " + db->last_error();
        return e;
    }

    *out = std::move(db);
    return StoreError::None;
}

StoreError Db::prepare(std::string_view sql, Stmt* out) {
    sqlite3_stmt* s = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &s, nullptr);
    if (rc != SQLITE_OK) {
        if (s != nullptr) sqlite3_finalize(s);
        return map_sqlite(rc);
    }
    Stmt tmp;
    tmp.s_ = s;
    tmp.db_ = this;
    *out = std::move(tmp);
    return StoreError::None;
}

StoreError Db::exec(std::string_view sql) {
    const std::string copy(sql);
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, copy.c_str(), nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);
    return map_sqlite(rc);
}

StoreError Db::begin_immediate() {
    const StoreError e = exec("BEGIN IMMEDIATE;");
    if (e == StoreError::None) in_tx_ = true;
    return e;
}

StoreError Db::commit() {
    const StoreError e = exec("COMMIT;");
    // Even a failed COMMIT ends the statement-level transaction state in
    // SQLite, so the flag is cleared either way and the caller decides what
    // the failure means.
    in_tx_ = false;
    return e;
}

StoreError Db::rollback() {
    const StoreError e = exec("ROLLBACK;");
    in_tx_ = false;
    return e;
}

bool Db::in_transaction() const { return in_tx_ && sqlite3_get_autocommit(db_) == 0; }

std::string Db::last_error() const {
    return db_ == nullptr ? std::string("no database") : std::string(sqlite3_errmsg(db_));
}

Db::DurabilityProfile Db::durability() const {
    DurabilityProfile p;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA journal_mode;", -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            if (t != nullptr) p.journal_mode = reinterpret_cast<const char*>(t);
        }
        sqlite3_finalize(s);
    }
    s = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA synchronous;", -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            switch (sqlite3_column_int(s, 0)) {
                case 0: p.synchronous = "OFF"; break;
                case 1: p.synchronous = "NORMAL"; break;
                case 2: p.synchronous = "FULL"; break;
                case 3: p.synchronous = "EXTRA"; break;
                default: p.synchronous = "UNKNOWN"; break;
            }
        }
        sqlite3_finalize(s);
    }
    // The mapping of section 17.1 onto this engine: WAL is the analogue of
    // wal_level=replica, and FULL or EXTRA is the analogue of
    // synchronous_commit=on. NORMAL in WAL mode can lose the tail of the last
    // transactions on a power cut, so it is not accepted as durable.
    p.safe = (p.journal_mode == "wal" || p.journal_mode == "delete" ||
              p.journal_mode == "truncate" || p.journal_mode == "persist") &&
             (p.synchronous == "FULL" || p.synchronous == "EXTRA");
    return p;
}

StoreError Db::integrity_check(std::string* why) {
    Stmt s;
    StoreError e = prepare("PRAGMA integrity_check;", &s);
    if (e != StoreError::None) return e;
    bool row = false;
    e = s.step(&row);
    if (e != StoreError::None) return e;
    if (!row) {
        if (why != nullptr) *why = "integrity_check returned no row";
        return StoreError::Corrupt;
    }
    const std::string verdict = s.col_text(0);
    if (verdict != "ok") {
        if (why != nullptr) *why = verdict;
        return StoreError::Corrupt;
    }
    return StoreError::None;
}

// --- TxGuard ---------------------------------------------------------------

TxGuard::~TxGuard() {
    // The unwinding path. An exception thrown anywhere between begin() and
    // commit() lands here and the transaction is discarded: this is what
    // makes "reducer threw before COMMIT leaves no side effects" (G02-11)
    // true by construction rather than by review.
    if (armed_) {
        db_.rollback();
        armed_ = false;
    }
}

StoreError TxGuard::begin() {
    const StoreError e = db_.begin_immediate();
    if (e == StoreError::None) armed_ = true;
    return e;
}

StoreError TxGuard::commit() {
    if (!armed_) return StoreError::Rejected;
    const StoreError e = db_.commit();
    armed_ = false;
    return e;
}

}  // namespace jrnl
