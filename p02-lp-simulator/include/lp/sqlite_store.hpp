// SQLite implementation of IBusinessStore.
//
// Reference: design doc 10.1 (schema) and 10.2 (transaction boundaries).
//
// Configuration: WAL journal mode with synchronous=FULL, which is the lab
// baseline for crash testing. Design doc 10.1 is careful about what that does
// and does not buy: it makes a committed transaction survive a process kill,
// and it does NOT prove survival of every physical power loss, because that
// depends on the filesystem and the drive honouring their own fsync contract.
// The crash suites therefore test process death, and the limitation is recorded
// rather than papered over.
//
// Everything the business layer reads back on recovery is recomputed from the
// ledger and compared against the stored snapshot. A disagreement is a HALT,
// not a repair: silently trusting either side is how a duplicate ExecID or a
// rolled-back CumQty reaches a client (E-13, J-15, J-17).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lp/ownership.hpp"
#include "lp/store.hpp"

struct sqlite3;

namespace lpsim {

// Schema version written into the database. A mismatch refuses startup instead
// of attempting an implicit migration (J-19, J-20).
constexpr int kSchemaVersion = 1;

class SqliteStore final : public IBusinessStore {
public:
    SqliteStore() = default;
    ~SqliteStore() override;

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    StoreError open(const StoreConfig& cfg) override;
    void close() override;

    RecoveryReport recover_and_verify() override;

    StoreError commit_inbound(const InboundIdentity& id, std::string_view raw_frame,
                              bool needs_delivery, bool* duplicate) override;
    StoreError pending_inbox(std::vector<std::pair<std::uint64_t, std::string>>* out) override;

    StoreError commit_business(const BusinessTxn& txn) override;

    StoreError send_once(const std::string& event_id, const CanonicalBody& body,
                         const FrameEncoder& encode, SendOnceResult* out) override;
    StoreError mark_sent(const std::string& event_id) override;
    StoreError take_admin_seq(std::uint64_t* seq) override;

    StoreError pending_outbox(std::vector<OutboxEntry>* out) override;
    StoreError outbound_history(
        std::uint64_t begin_seq, std::uint64_t end_seq,
        std::vector<std::pair<std::uint64_t, std::string>>* out) override;

    StoreError load_session_state(SessionState* out) override;
    StoreError apply_coordinated_reset(std::uint64_t new_epoch) override;

    std::size_t count_fills() const override;
    std::size_t count_outbox_pending() const override;

    // Test seams. These exist so the corruption and rollback scenarios can be
    // produced through a supported door rather than by poking the file: a test
    // that mutates bytes blindly often produces a database SQLite refuses to
    // open, which proves nothing about the integrity checks.
    StoreError test_force_fill_cum_qty(const std::string& order_id, Units cum_qty);
    StoreError test_delete_outbox_row(const std::string& event_id);
    StoreError test_delete_registry_row(const BusinessKey& key);
    StoreError test_set_allocator(const std::string& name, std::uint64_t next_counter);
    StoreError test_corrupt_outbound_frame(std::uint64_t seq, const std::string& frame);
    StoreError test_delete_outbound_history(std::uint64_t seq);

private:
    StoreError exec(const char* sql);
    StoreError begin();
    StoreError commit();
    void rollback();
    StoreError last_error() const;

    StoreError write_schema();
    StoreError load_allocators(RecoveryReport* out);

    sqlite3* db_ = nullptr;
    StoreConfig cfg_;
    OwnershipLock lock_;
    bool open_ = false;
};

}  // namespace lpsim
