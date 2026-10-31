// SQLite implementation of ISessionStore.
//
// Baseline durability per design doc 9.1: journal_mode=WAL with
// synchronous=FULL. The schema is the one sketched in the doc, plus a
// payload checksum column so that a tampered or bit-rotted history row is
// detected before it is retransmitted (scenario F-17), and an owner_lease table
// so an unclean shutdown is visible on the next start (G-22).
#pragma once

#include <memory>
#include <string>

#include "fix/clock.hpp"
#include "fix/store.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace fixse {

class OwnershipLock;

class SqliteSessionStore final : public ISessionStore {
public:
    struct Options {
        std::string path;
        SessionKey key;
        int busy_timeout_ms = 5000;
        std::string journal_mode = "WAL";
        std::string synchronous = "FULL";
        // Refuse to invent a database on a missing volume: an engine that
        // silently starts from scratch looks healthy while having lost every
        // sequence number (scenario L-13).
        bool create_if_missing = false;
        const IClock* clock = nullptr;
    };

    static StoreError open(const Options& opt, std::unique_ptr<SqliteSessionStore>* out,
                           std::string* why);

    ~SqliteSessionStore() override;
    SqliteSessionStore(const SqliteSessionStore&) = delete;
    SqliteSessionStore& operator=(const SqliteSessionStore&) = delete;

    StoreError load_state(PersistedState* out) override;
    StoreError commit_outbound(const OutboundRecord& rec, uint64_t next_out) override;
    StoreError commit_inbound(const InboundRecord* rec, uint64_t next_in) override;
    StoreError mark_delivered(uint64_t seq) override;
    StoreError load_outbound_range(uint64_t from_seq, uint64_t to_seq,
                                   std::vector<OutboundRecord>* out) override;
    StoreError load_pending_inbox(size_t max_rows, std::vector<InboundRecord>* out) override;
    StoreError load_inbox_record(uint64_t seq, InboundRecord* out, bool* found) override;
    StoreError append_audit(const AuditRecord& rec) override;
    StoreError record_connection_generation(uint64_t generation) override;
    StoreError coordinated_reset(const std::string& approver_a, const std::string& approver_b,
                                 const std::string& reason) override;
    ConsistencyReport verify_consistency() override;
    bool owns_session() const override;
    void mark_clean_shutdown() override;
    size_t audit_count() const override;

    // Current schema version. A database written by a newer engine is refused
    // rather than read with a guessed layout (G-15).
    static constexpr int kSchemaVersion = 1;

    // Exposed for tests that need to assert on raw rows.
    sqlite3* raw_handle() { return db_; }
    const PersistedState& cached_state() const { return state_; }

private:
    SqliteSessionStore() = default;

    StoreError apply_pragmas(const Options& opt, std::string* why);
    StoreError ensure_schema(std::string* why);
    StoreError check_integrity(std::string* why);
    StoreError write_state_locked(const PersistedState& st);
    StoreError exec(const char* sql);
    std::string state_checksum(const PersistedState& st) const;

    sqlite3* db_ = nullptr;
    std::unique_ptr<OwnershipLock> lock_;
    SessionKey key_;
    std::string session_id_;
    const IClock* clock_ = nullptr;
    PersistedState state_;
    bool state_loaded_ = false;
    bool clean_shutdown_on_open_ = true;
    size_t audit_rows_ = 0;
};

}  // namespace fixse
