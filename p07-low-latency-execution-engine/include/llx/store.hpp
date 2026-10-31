// Atomic business store: the P06 port, and a journal-backed implementation.
//
// P07 does not own durability. Design doc 13 is the chokepoint of the whole
// project: a business decision may be computed on a fast actor, but an outbound
// original 35=D/F/G becomes eligible only after a transaction has atomically
// written the inbox dedup row, the journal event, the order projection, the
// fill ledger, the risk reservation, the command intent and the outbox
// (LI-15, LI-16). Moving the commit onto a worker thread does not shorten the
// time to durable; it only stops the owner from blocking, and doc 13 requires
// compute, commit and dispatch to be measured separately for exactly that
// reason.
//
// Interface deviation from doc 18, stated rather than hidden: the doc sketches
// `commit(const DurableBusinessIntent&)`. The intent as sketched carries only
// part of the projection, which is not enough to rebuild state by replay. So
// the request carries the reducer's full post-image alongside it, and the
// journal record is that post-image. Replay is then exactly `apply()`, which is
// what makes "restart yields the same business state" (LI-39) checkable instead
// of approximate.
//
// What the journal-backed store does and does not prove:
//   * it does prove atomicity. One transaction is one length-prefixed,
//     checksummed record, appended and flushed. A crash before the flush
//     leaves no record; a torn tail fails its checksum and is discarded on
//     replay. There is no state in which half a transaction is visible
//     (CW-11..CW-16).
//   * it does prove the send gate. The outbox lives in the same record, so
//     nothing is claimable for dispatch that is not already durable (LI-16).
//   * it does NOT prove durability against power loss, and it is not
//     PostgreSQL. Doc 15.1 keeps "mock in-process deterministic P06" and "real
//     PostgreSQL sync local commit" as separate span classes, and doc 27.3 run
//     set DB-01 needs the real database. Any run against this store is labelled
//     as such and must not be quoted as a durable-commit latency.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "llx/domain.hpp"
#include "llx/event.hpp"

namespace llx {

// --- outbox ----------------------------------------------------------------

enum class OutboxState : std::uint8_t {
    Ready = 0,       // durable and not yet claimed: safe to send the original
    Claimed = 1,     // a dispatcher holds it
    Sent = 2,        // the write completed and was acknowledged locally
    Ambiguous = 3,   // an attempt was made and its outcome is unknown (CW-22)
    Reconciled = 4,  // the LP outcome was established by evidence
};

std::string_view to_string(OutboxState s);

struct OutboxRecord {
    std::uint64_t id = 0;
    std::uint64_t journal_version = 0;
    std::uint32_t owner_epoch = 0;
    std::uint8_t kind = 0;  // 1 New, 2 Cancel, 3 Replace
    OrderId cl_ord_id = 0;
    OrderId orig_cl_ord_id = 0;
    SymbolId symbol = 0;
    Side side = Side::None;
    Qty qty = 0;
    Px px = 0;
    OutboxState state = OutboxState::Ready;
    // Durable attempt count. This is what makes an ambiguous send detectable
    // after a restart: attempts > 0 with no Sent marker means the LP may
    // already have the order, so LI-18 forbids issuing a fresh one.
    std::uint32_t attempts = 0;
};

// --- the port --------------------------------------------------------------

struct CommitRequest {
    DurableBusinessIntent intent;
    Proposal effect;  // full post-image, applied atomically or not at all
};

struct IAtomicBusinessStore {
    virtual CommitOutcome commit(const CommitRequest& req, CommitReceipt& out) = 0;

    // Resolves a COMMIT_UNKNOWN. Doc 13 and LI-17: the resolution is a query by
    // the idempotency key, never a reissued business command.
    virtual bool lookup_by_idempotency(std::uint64_t key, CommitReceipt& out) const = 0;

    virtual const BusinessState& state() const = 0;
    virtual std::uint64_t journal_version() const = 0;

    // Highest domain_seq committed for a risk domain, or 0. The owner needs it
    // to resume sequence assignment after a restart without re-deriving it
    // from the journal itself (doc 7, LI-13).
    virtual std::uint64_t committed_domain_seq(DomainId d) const = 0;

    // Fencing. A commit carrying an owner_epoch older than the store's current
    // epoch is refused, which is what stops a late callback from a displaced
    // owner from mutating state (LI-14, LI-47, doc 19.5).
    virtual std::uint32_t fencing_epoch() const = 0;
    virtual void fence_to_epoch(std::uint32_t epoch) = 0;

    // Outbox handoff. claim_next hands out only durable records, and only under
    // the current epoch.
    virtual bool claim_next(std::uint32_t owner_epoch, OutboxRecord& out) = 0;
    virtual bool mark_sent(std::uint64_t outbox_id) = 0;
    virtual bool mark_ambiguous(std::uint64_t outbox_id) = 0;
    virtual std::vector<OutboxRecord> outbox_snapshot() const = 0;

    virtual ~IAtomicBusinessStore() = default;
};

// --- journal-backed implementation ----------------------------------------

struct StoreStats {
    std::uint64_t commits_attempted = 0;
    std::uint64_t commits_committed = 0;
    std::uint64_t commits_rejected = 0;
    std::uint64_t commits_unknown = 0;
    std::uint64_t stale_epoch_refusals = 0;
    std::uint64_t out_of_order_refusals = 0;
    std::uint64_t inbox_duplicates = 0;
    std::uint64_t records_replayed = 0;
    std::uint64_t records_discarded = 0;  // torn or corrupt tail
    std::uint64_t syncs = 0;
};

// Threading contract.
//
// The journal is one durable resource and more than one thread reaches it:
// doc 7 gives the commit sequencing to T2 and the outbox handoff to T3, so the
// owner commits while the dispatcher claims. Both append to the same record
// log, and two threads appending to one std::vector is heap corruption -- an
// intermittent 0xC0000374 in the threaded runtime was exactly this.
//
// So every operation that touches the record log, the outbox or the file takes
// an internal mutex. A lock on the durable path costs nothing worth measuring:
// the commit it guards already performs an fsync, which is orders of magnitude
// more expensive, and doc 13 explicitly offers "a serialized worker" as the v1
// design.
//
// state() is deliberately NOT locked. Only commit() mutates the projection, and
// only the owner thread commits and reduces, so the reducer reading it is
// single-threaded by construction. No other role touches it.
class JournalStore final : public IAtomicBusinessStore {
public:
    struct Options {
        // Empty means memory only: the records are kept in a vector and a
        // "restart" is modelled by rebuilding from them. A path makes the
        // journal a real file that survives process death, which is what the
        // crash-window tests use.
        std::string journal_path;
        // flush after every commit. Off makes the store faster and strictly
        // less durable, and the manifest records which was used.
        bool sync_on_commit = true;
        // Refuse a commit whose domain_seq is not the next one for its domain.
        // Doc 7: an out-of-order commit must not be allowed to grant dispatch
        // eligibility ahead of its causal predecessor.
        bool enforce_domain_sequence = true;
        std::uint32_t initial_epoch = 1;
    };

    // Two constructors rather than a defaulted argument: `Options opts = {}`
    // would need Options' default member initializers inside the still
    // incomplete enclosing class, which the language does not allow.
    JournalStore();
    explicit JournalStore(Options opts);
    ~JournalStore() override;

    CommitOutcome commit(const CommitRequest& req, CommitReceipt& out) override;
    bool lookup_by_idempotency(std::uint64_t key, CommitReceipt& out) const override;
    const BusinessState& state() const override { return state_; }
    std::uint64_t journal_version() const override { return version_; }
    std::uint32_t fencing_epoch() const override { return epoch_; }
    void fence_to_epoch(std::uint32_t epoch) override;

    bool claim_next(std::uint32_t owner_epoch, OutboxRecord& out) override;
    bool mark_sent(std::uint64_t outbox_id) override;
    bool mark_ambiguous(std::uint64_t outbox_id) override;
    std::vector<OutboxRecord> outbox_snapshot() const override;

    // Discards every derived structure and rebuilds it from the journal. This
    // is the restart path: it is what LI-39 ("same committed log leads to the
    // same business state") is tested with, and what a crash-window test calls
    // after re-opening a journal a killed process left behind.
    void rebuild_from_journal();

    // Seeds a risk domain. Configuration, not a business event, so it is
    // journalled as its own record kind and survives a rebuild.
    void configure_risk_domain(DomainId domain, Qty limit_qty);

    const StoreStats& stats() const { return stats_; }
    std::string state_hash() const { return state_.state_hash(); }

    // Highest domain_seq committed per risk domain, used by the owner to
    // decide whether a commit result may release dispatch eligibility.
    std::uint64_t committed_domain_seq(DomainId d) const override;

    bool journal_is_file() const { return !opts_.journal_path.empty(); }
    std::size_t record_count() const { return records_.size(); }

private:
    // One durable record. Kinds beyond Business exist because an outbox attempt
    // and a send acknowledgement have to survive a restart too: without them a
    // restart could not tell "never sent" from "may have been sent", and the
    // difference is the whole of CW-20..CW-22.
    enum class RecordKind : std::uint8_t {
        Business = 1,
        OutboxAttempt = 2,
        OutboxSent = 3,
        OutboxAmbiguous = 4,
        EpochFence = 5,
        RiskConfig = 6,
    };

    struct Record {
        RecordKind kind = RecordKind::Business;
        std::uint64_t version = 0;
        // Business
        EventId event_id{};
        std::uint64_t domain_seq = 0;
        DomainId risk_domain = 0;
        std::uint32_t owner_epoch = 0;
        std::uint64_t idempotency_key = 0;
        Proposal effect{};
        bool write_inbox = false;
        // Outbox
        OutboxRecord outbox{};
        std::uint64_t outbox_id = 0;
        // Fence / risk config
        std::uint32_t epoch = 0;
        DomainId config_domain = 0;
        Qty config_limit = 0;
    };

    // Callable with mu_ already held, which commit() needs.
    std::uint64_t committed_domain_seq_locked(DomainId d) const;
    OutboxRecord* find_outbox_locked(std::uint64_t id);
    std::string encode(const Record& r) const;
    bool decode(const std::string& payload, Record& out) const;
    bool append(const Record& r);
    void apply_record(const Record& r);
    bool load_journal();

    Options opts_;
    BusinessState state_;
    std::vector<Record> records_;
    std::set<EventId> inbox_;
    std::set<ExecId> seen_exec_;
    std::map<std::uint64_t, CommitReceipt> idem_;
    std::map<DomainId, std::uint64_t> domain_seq_;
    std::vector<OutboxRecord> outbox_;
    // Index of the first row that might still be Ready, and a id -> position
    // map for the mark_* calls.
    //
    // Both exist because scanning outbox_ from the start was O(rows) per
    // claim, which made dispatch O(rows^2) over a run: the open-loop
    // benchmark fell from 20k/s offered to 2k/s completed with a p50 of 1.2
    // seconds, and the whole deficit was this scan. Rows are appended in id
    // order and only ever move Ready -> Claimed/Sent/Ambiguous, so the hint is
    // monotone and never skips a claimable row.
    std::size_t next_ready_hint_ = 0;
    std::map<std::uint64_t, std::size_t> outbox_pos_;
    std::uint64_t version_ = 0;
    std::uint64_t next_outbox_id_ = 1;
    std::uint32_t epoch_ = 1;
    void* file_ = nullptr;  // FILE* when the journal is file-backed
    StoreStats stats_{};
    // Guards records_, outbox_, inbox_, seen_exec_, idem_, domain_seq_,
    // version_, next_outbox_id_, epoch_, file_ and stats_.
    mutable std::mutex mu_;
};

}  // namespace llx
