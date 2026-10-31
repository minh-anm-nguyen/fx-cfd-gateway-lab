// The durability port and an in-memory implementation of it.
//
// What this is: an atomic commit boundary that state, dedup keys, the event
// journal and the effect outbox all pass through together, so the crash
// windows W05..W08 can be shown to roll back as one unit.
//
// What this is not: proof that the system is crash safe. The design doc is
// blunt about it (section 13, ADR-08): that claim belongs to P06 with a real
// PostgreSQL transaction behind it, and nothing here may be read as evidence
// for it. `durability_mode()` reports InMemory so a release gate can refuse to
// pass G3 on this store.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "order_core/failpoint.hpp"
#include "order_core/reducer.hpp"

namespace order_core {

enum class DurabilityMode { InMemory, DurableJournal };
const char* to_string(DurabilityMode m);

// One committed event, kept so recovery can re-derive state rather than trust
// a stored blob. The event is held as a value here because this store is
// in-memory; P06 serialises it.
struct JournalRecord {
    OrderId id;
    uint64_t event_index = 0;
    uint64_t revision_after = 0;
    OrderEvent event;
    Decision decision = Decision::Applied;
    uint64_t state_hash_after = 0;
    AppliedKey applied_key;
    std::string audit;
};

struct OutboxRow {
    uint64_t seq = 0;
    OrderId id;
    EffectIntent::Kind kind = EffectIntent::Kind::Audit;
    std::string idempotency_key;
    std::string detail;
    bool dispatched = false;
    bool marked_sent = false;
};

struct InboxRow {
    uint64_t seq = 0;
    OrderEvent event;
    bool consumed = false;
};

struct AggregateRow {
    uint64_t revision = 0;
    OrderAggregate state;
};

struct Snapshot {
    std::map<uint64_t, OrderAggregate> aggregates;  // keyed by OrderId::value
    uint64_t journal_watermark = 0;                 // records already folded in
    uint32_t schema_version = 1;
    uint32_t policy_version = 1;
    uint64_t digest = 0;
    bool complete = false;
};

// One row of the durable dedup index. The payload hash is what turns a second
// delivery of the same key into either a no-op or a conflict; the chain tells
// a cross-chain reuse of an ExecID apart from a legitimate redelivery.
struct AppliedKeyRow {
    uint64_t chain = 0;
    uint64_t payload_hash = 0;
    uint64_t event_index = 0;
};

enum class CommitStatus {
    Committed,
    RevisionConflict,  // another writer moved the revision; retry with fresh state
    DuplicateKey,      // this semantic key was already applied
    Failed,            // storage refused (disk full, constraint violation)
    Ambiguous,         // flush neither confirmed nor denied; must not publish
};
const char* to_string(CommitStatus s);

struct CommitResult {
    CommitStatus status = CommitStatus::Failed;
    uint64_t revision = 0;
    std::string detail;
};

struct IAtomicCommit {
    virtual ~IAtomicCommit() = default;
    virtual CommitResult commit(OrderId id, uint64_t expected_revision,
                                const Reduction& reduction, uint64_t event_index,
                                const OrderEvent& event, const std::string& audit) = 0;
    virtual DurabilityMode durability_mode() const = 0;
};

// Simulated durable storage. Everything in `durable_` survives a
// SimulatedCrash; everything an Engine holds does not.
class MemoryStore : public IAtomicCommit {
  public:
    CommitResult commit(OrderId id, uint64_t expected_revision, const Reduction& reduction,
                        uint64_t event_index, const OrderEvent& event,
                        const std::string& audit) override;

    DurabilityMode durability_mode() const override { return DurabilityMode::InMemory; }

    FailPoint& failpoint() { return failpoint_; }

    // --- durable inbox (the P01 -> P04 boundary) ----------------------------
    //
    // Rows are addressed by sequence, consumed in order, and the fully
    // consumed prefix is pruned so a long run does not hold every event it has
    // ever seen. The accessors below are all constant time: a linear scan per
    // event would make ingest quadratic, which is a resource bound of its own
    // (invariant I23).
    uint64_t append_inbox(const OrderEvent& e);
    std::vector<InboxRow>& inbox() { return inbox_; }
    void ack_inbox(uint64_t seq);
    [[nodiscard]] size_t unconsumed_inbox() const { return unconsumed_count_; }
    [[nodiscard]] bool is_consumed(uint64_t seq) const;
    // Sequence of the oldest unconsumed row, or 0 when the inbox is drained.
    [[nodiscard]] uint64_t first_unconsumed_seq();
    [[nodiscard]] const InboxRow* row(uint64_t seq) const;

    // --- reads used by recovery and by tests --------------------------------
    [[nodiscard]] const std::vector<JournalRecord>& journal() const { return journal_; }
    [[nodiscard]] const std::map<uint64_t, AggregateRow>& aggregates() const {
        return aggregates_;
    }
    [[nodiscard]] std::vector<OutboxRow>& outbox() { return outbox_; }
    [[nodiscard]] const std::map<std::string, AppliedKeyRow>& applied_keys() const {
        return applied_keys_;
    }
    [[nodiscard]] const std::optional<Snapshot>& snapshot() const { return snapshot_; }

    // --- operational controls used by the failure suite ---------------------
    void set_fail_next_commit(bool on, std::string detail = "WAL disk full") {
        fail_next_commit_ = on;
        fail_detail_ = std::move(detail);
    }
    // Corrupts the recorded hash of a journal record so startup verification
    // has something real to catch (what-if L-15).
    bool corrupt_journal_hash(size_t index);
    void set_schema_version(uint32_t v) { schema_version_ = v; }
    [[nodiscard]] uint32_t schema_version() const { return schema_version_; }

    // Writes a checkpoint. Fires W13 mid-way to show a half-written snapshot
    // is never adopted.
    void write_snapshot(const std::map<uint64_t, AggregateRow>& live, uint64_t watermark,
                        uint32_t policy_version);
    void install_snapshot(Snapshot s) { snapshot_ = std::move(s); }
    void clear_snapshot() { snapshot_.reset(); }

    [[nodiscard]] size_t applied_key_count() const { return applied_keys_.size(); }
    // Eviction is refused unless the caller can prove the resend horizon has
    // closed; retention shorter than that horizon reopens double-fill risk.
    bool evict_applied_keys_before(uint64_t watermark, bool safe_watermark_proven);

  private:
    FailPoint failpoint_;
    std::vector<JournalRecord> journal_;
    std::map<uint64_t, AggregateRow> aggregates_;
    std::map<std::string, AppliedKeyRow> applied_keys_;
    std::vector<OutboxRow> outbox_;
    std::vector<InboxRow> inbox_;
    uint64_t inbox_base_seq_ = 1;   // sequence of inbox_[0]
    size_t inbox_cursor_ = 0;       // index of the first possibly-unconsumed row
    size_t unconsumed_count_ = 0;
    std::optional<Snapshot> snapshot_;
    uint64_t next_outbox_seq_ = 1;
    uint64_t next_inbox_seq_ = 1;
    uint32_t schema_version_ = 1;
    bool fail_next_commit_ = false;
    std::string fail_detail_;
};

}  // namespace order_core
