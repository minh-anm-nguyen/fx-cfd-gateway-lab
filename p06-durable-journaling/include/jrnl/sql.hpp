// Thin SQLite wrapper.
//
// The design doc targets PostgreSQL 17. This lab has no PostgreSQL, no libpq
// and no Docker, so the runnable backend here is SQLite with WAL and
// synchronous=FULL, and migrations/0001_core.sql carries the PostgreSQL
// schema that the same logic is meant to run against. The difference is
// recorded honestly in docs/P06_TEST_REPORT.md rather than papered over: the
// scenarios that test PostgreSQL's own WAL, replication and PITR machinery
// are reported BLOCKED, not PASS.
//
// What this wrapper owes the rest of the engine is the part that is not
// engine-specific: one transaction object with RAII rollback, one error
// taxonomy, and no way to leave a statement open across a commit.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jrnl/hash.hpp"
#include "jrnl/result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace jrnl {

class Db;

// One prepared statement. Binding is 1-based to match SQLite.
class Stmt {
public:
    Stmt() = default;
    ~Stmt();
    Stmt(Stmt&& o) noexcept;
    Stmt& operator=(Stmt&& o) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_text(int i, std::string_view v);
    void bind_i64(int i, std::int64_t v);
    void bind_u64(int i, std::uint64_t v);
    void bind_null(int i);
    void bind_hex(int i, const Digest& d);  // stored as 64 lowercase hex chars

    // Advances one row. Sets *have_row. Returns the mapped error.
    StoreError step(bool* have_row);
    // For statements that return no rows.
    StoreError run();

    std::string col_text(int i) const;
    std::int64_t col_i64(int i) const;
    std::uint64_t col_u64(int i) const;
    bool col_is_null(int i) const;
    // Reads a 64-hex column. A column that is not 64 valid hex characters is
    // corruption, not an empty digest (G01-19).
    bool col_hex(int i, Digest* out) const;

    void reset();
    bool valid() const { return s_ != nullptr; }

private:
    friend class Db;
    sqlite3_stmt* s_ = nullptr;
    Db* db_ = nullptr;
};

class Db {
public:
    struct Options {
        std::string path;
        int busy_timeout_ms = 5000;
        // The durability profile from section 17.1, translated to this
        // engine. Anything weaker is refused rather than silently accepted,
        // because ACCEPTED_DURABLE is defined in terms of it (JI-30).
        std::string journal_mode = "WAL";
        std::string synchronous = "FULL";
        bool create_if_missing = true;
        bool read_only = false;
    };

    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    static StoreError open(const Options& opt, std::unique_ptr<Db>* out, std::string* why);

    StoreError prepare(std::string_view sql, Stmt* out);
    StoreError exec(std::string_view sql);

    // Begins an immediate transaction: the write lock is taken at BEGIN
    // rather than at the first write, so two writers cannot both read a
    // stream head and then race to extend it.
    StoreError begin_immediate();
    StoreError commit();
    StoreError rollback();
    bool in_transaction() const;

    // Last error text from the engine, for audit rows and test messages.
    std::string last_error() const;

    // Whether the open database honours the pinned durability settings. The
    // recovery supervisor refuses to go ACTIVE when it does not (G15, JI-30).
    struct DurabilityProfile {
        std::string journal_mode;
        std::string synchronous;
        bool safe = false;
    };
    DurabilityProfile durability() const;

    sqlite3* raw() { return db_; }
    const std::string& path() const { return path_; }

    // Integrity check across the whole file. Corruption means quarantine,
    // never "start a fresh database" (JI-29).
    StoreError integrity_check(std::string* why);

private:
    friend class Stmt;
    sqlite3* db_ = nullptr;
    std::string path_;
    bool in_tx_ = false;
};

// RAII transaction guard. If the scope exits without an explicit commit the
// transaction is rolled back, which is the mechanism behind JI-28 and the
// W03..W10 windows: there is no code path that leaves a partial write set
// behind.
class TxGuard {
public:
    explicit TxGuard(Db& db) : db_(db) {}
    ~TxGuard();
    TxGuard(const TxGuard&) = delete;
    TxGuard& operator=(const TxGuard&) = delete;

    StoreError begin();
    StoreError commit();
    void disarm() { armed_ = false; }  // ownership passed elsewhere
    bool armed() const { return armed_; }

private:
    Db& db_;
    bool armed_ = false;
};

// Maps a SQLite result code onto the taxonomy in result.hpp. Exposed so the
// store and the tests classify the same code the same way.
StoreError map_sqlite(int rc);

}  // namespace jrnl
