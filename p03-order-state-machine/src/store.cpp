#include "order_core/store.hpp"

namespace order_core {

const char* to_string(DurabilityMode m) {
    switch (m) {
        case DurabilityMode::InMemory: return "IN_MEMORY";
        case DurabilityMode::DurableJournal: return "DURABLE_JOURNAL";
    }
    return "UNKNOWN";
}

const char* to_string(CommitStatus s) {
    switch (s) {
        case CommitStatus::Committed: return "COMMITTED";
        case CommitStatus::RevisionConflict: return "REVISION_CONFLICT";
        case CommitStatus::DuplicateKey: return "DUPLICATE_KEY";
        case CommitStatus::Failed: return "FAILED";
        case CommitStatus::Ambiguous: return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

const char* to_string(CrashWindow w) {
    switch (w) {
        case CrashWindow::None: return "NONE";
        case CrashWindow::W00_BeforeParse: return "W00";
        case CrashWindow::W01_AfterParseBeforeCorrelation: return "W01";
        case CrashWindow::W02_AfterCorrelationBeforeDedup: return "W02";
        case CrashWindow::W03_AfterDedupBeforeReduce: return "W03";
        case CrashWindow::W04_AfterReduceBeforeTxn: return "W04";
        case CrashWindow::W05_InTxnBeforeJournalInsert: return "W05";
        case CrashWindow::W06_AfterDedupInsertBeforeState: return "W06";
        case CrashWindow::W07_AfterStateBeforeOutbox: return "W07";
        case CrashWindow::W08_AfterOutboxBeforeCommit: return "W08";
        case CrashWindow::W09_AfterCommitBeforeInboxAck: return "W09";
        case CrashWindow::W10_AfterCommitBeforeDispatch: return "W10";
        case CrashWindow::W11_AfterDispatchBeforeMarkSent: return "W11";
        case CrashWindow::W12_AmbiguousFlush: return "W12";
        case CrashWindow::W13_DuringSnapshotCompaction: return "W13";
        case CrashWindow::W14_DuringSchemaMigration: return "W14";
        case CrashWindow::W15_DuringRecoveryReplay: return "W15";
    }
    return "UNKNOWN";
}

namespace {
// How much consumed history the inbox keeps before pruning its prefix. Large
// enough that a test or an operator can still look back, small enough that a
// long soak does not accumulate every event forever.
constexpr size_t kInboxRetainedPrefix = 4096;
}  // namespace

uint64_t MemoryStore::append_inbox(const OrderEvent& e) {
    InboxRow row;
    row.seq = next_inbox_seq_++;
    row.event = e;
    inbox_.push_back(row);
    ++unconsumed_count_;
    return row.seq;
}

const InboxRow* MemoryStore::row(uint64_t seq) const {
    if (seq < inbox_base_seq_) return nullptr;  // pruned, therefore consumed
    const size_t index = static_cast<size_t>(seq - inbox_base_seq_);
    if (index >= inbox_.size()) return nullptr;
    return &inbox_[index];
}

bool MemoryStore::is_consumed(uint64_t seq) const {
    const InboxRow* r = row(seq);
    // A pruned or unknown sequence is not outstanding work.
    return r == nullptr ? true : r->consumed;
}

void MemoryStore::ack_inbox(uint64_t seq) {
    if (seq < inbox_base_seq_) return;
    const size_t index = static_cast<size_t>(seq - inbox_base_seq_);
    if (index >= inbox_.size() || inbox_[index].consumed) return;
    inbox_[index].consumed = true;
    --unconsumed_count_;

    while (inbox_cursor_ < inbox_.size() && inbox_[inbox_cursor_].consumed) ++inbox_cursor_;
    // Prune the consumed prefix, keeping a bounded tail of history.
    if (inbox_cursor_ > kInboxRetainedPrefix) {
        const size_t drop = inbox_cursor_ - kInboxRetainedPrefix;
        inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<long>(drop));
        inbox_base_seq_ += drop;
        inbox_cursor_ -= drop;
    }
}

uint64_t MemoryStore::first_unconsumed_seq() {
    while (inbox_cursor_ < inbox_.size() && inbox_[inbox_cursor_].consumed) ++inbox_cursor_;
    if (inbox_cursor_ >= inbox_.size()) return 0;
    return inbox_[inbox_cursor_].seq;
}

CommitResult MemoryStore::commit(OrderId id, uint64_t expected_revision,
                                 const Reduction& reduction, uint64_t event_index,
                                 const OrderEvent& event, const std::string& audit) {
    // The whole body below is one transaction. Nothing touches the durable
    // members until the staging area is complete, which is what makes the
    // rollback windows real: a crash before the final splice leaves no trace.
    if (fail_next_commit_) {
        fail_next_commit_ = false;
        return {CommitStatus::Failed, expected_revision, fail_detail_};
    }

    failpoint_.check(CrashWindow::W05_InTxnBeforeJournalInsert, "commit:before journal insert");

    // Optimistic concurrency on the aggregate revision. A mismatch means
    // another owner wrote; the caller must re-read rather than overwrite
    // (invariant I01, what-if N-02).
    const auto existing = aggregates_.find(id.value);
    const uint64_t current = existing == aggregates_.end() ? 0 : existing->second.revision;
    if (current != expected_revision) {
        return {CommitStatus::RevisionConflict, current,
                "expected revision " + std::to_string(expected_revision) + ", found " +
                    std::to_string(current)};
    }

    // Staging: dedup key first, mirroring the SQL sketch in design doc 13.1.
    const AppliedKey& key = reduction.applied_key;
    if (!key.empty()) {
        const auto it = applied_keys_.find(key.semantic_key);
        if (it != applied_keys_.end()) {
            // The unique constraint is what makes a post-crash redelivery a
            // no-op instead of a second effect (window W09).
            return {CommitStatus::DuplicateKey, current, "semantic key already applied"};
        }
    }

    failpoint_.check(CrashWindow::W06_AfterDedupInsertBeforeState,
                     "commit:after dedup insert, before state update");

    JournalRecord rec;
    rec.id = id;
    rec.event_index = event_index;
    rec.revision_after = expected_revision + 1;
    rec.event = event;
    rec.decision = reduction.decision;
    rec.state_hash_after = reduction.next.state_hash();
    rec.applied_key = key;
    rec.audit = audit;

    AggregateRow row;
    row.revision = expected_revision + 1;
    row.state = reduction.next;
    row.state.revision = row.revision;

    failpoint_.check(CrashWindow::W07_AfterStateBeforeOutbox,
                     "commit:after state update, before outbox insert");

    std::vector<OutboxRow> staged_outbox;
    for (const EffectIntent& intent : reduction.intents) {
        OutboxRow o;
        o.seq = next_outbox_seq_ + staged_outbox.size();
        o.id = id;
        o.kind = intent.kind;
        o.idempotency_key = intent.idempotency_key;
        o.detail = intent.detail;
        staged_outbox.push_back(o);
    }

    failpoint_.check(CrashWindow::W08_AfterOutboxBeforeCommit,
                     "commit:after outbox insert, before COMMIT");
    failpoint_.check(CrashWindow::W12_AmbiguousFlush, "commit:flush returned ambiguous");

    // COMMIT. From here on the change is durable and indivisible.
    journal_.push_back(std::move(rec));
    aggregates_[id.value] = std::move(row);
    if (!key.empty()) {
        applied_keys_[key.semantic_key] = {id.value, key.payload_hash, event_index};
    }
    for (OutboxRow& o : staged_outbox) outbox_.push_back(std::move(o));
    next_outbox_seq_ += reduction.intents.size();

    return {CommitStatus::Committed, expected_revision + 1, ""};
}

bool MemoryStore::corrupt_journal_hash(size_t index) {
    if (index >= journal_.size()) return false;
    journal_[index].state_hash_after ^= 0xDEADBEEFULL;
    return true;
}

void MemoryStore::write_snapshot(const std::map<uint64_t, AggregateRow>& live, uint64_t watermark,
                                 uint32_t policy_version) {
    Snapshot s;
    s.journal_watermark = watermark;
    s.schema_version = schema_version_;
    s.policy_version = policy_version;
    s.complete = false;
    // Published incomplete first, then filled, so a crash in the middle leaves
    // a snapshot that recovery can recognise as unusable (what-if L-18).
    snapshot_ = s;
    std::string digest_input;
    for (const auto& [id, row] : live) {
        failpoint_.check(CrashWindow::W13_DuringSnapshotCompaction, "snapshot:mid-write");
        snapshot_->aggregates[id] = row.state;
        digest_input += std::to_string(id) + ":" + row.state.canonical_text();
    }
    snapshot_->digest = fnv1a(digest_input);
    snapshot_->complete = true;
}

bool MemoryStore::evict_applied_keys_before(uint64_t watermark, bool safe_watermark_proven) {
    if (!safe_watermark_proven) {
        // Refusing is the point: an evicted ExecID lets the same fill apply
        // twice if the peer resends (what-if F-17, P-16).
        return false;
    }
    size_t removed = 0;
    for (auto it = applied_keys_.begin(); it != applied_keys_.end();) {
        if (it->second.event_index < watermark) {
            it = applied_keys_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed > 0;
}

}  // namespace order_core
