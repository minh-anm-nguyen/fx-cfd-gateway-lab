// Retention, archive and restore drills.
//
// Two sections of the design doc meet here.
//
// Section 13 on compaction: v1 deletes no journal. Retention may only touch
// records that are provably no longer needed, and JI-14 lists what "needed"
// means — a live order, an open reconciliation case, an unresolved outbound
// effect, or a resend window that has not closed. The pruner's job is mostly
// to refuse.
//
// Section 17 on backup and PITR: what can be tested here is P06's DECISION
// logic, not PostgreSQL's WAL machinery. There is no PostgreSQL in this lab,
// so what the archive below models is the part the engine owns: segments
// with checksums and a manifest, detection of a missing range or an altered
// segment, and the rule that a restore never resumes dispatch on its own
// (JI-31). The PostgreSQL-specific scenarios are reported BLOCKED in the
// test report rather than claimed.
#pragma once

#include <string>
#include <vector>

#include "jrnl/store.hpp"

namespace jrnl {

struct RetentionPolicy {
    // Nothing older than this is even considered. Expressed in wall-clock
    // milliseconds because it is a legal and operational horizon, not a
    // replay ordering (section 8 note 8).
    std::int64_t horizon_ms = 0;
    // Section 18: an idempotency key cannot simply be forgotten, because a
    // client replaying an old command id after the key is gone would place
    // the order a second time. Keys are kept, and a tombstone horizon is
    // deliberately much longer than the receipt one.
    std::int64_t command_key_horizon_ms = 0;
    // How long a FIX counterparty may still ask for a resend.
    std::int64_t resend_window_ms = 0;
    bool allow_journal_deletion = false;  // v1: always false
};

struct RetentionResult {
    bool refused = false;
    std::string refusal_reason;
    std::size_t receipts_pruned = 0;
    std::size_t exec_keys_pruned = 0;
    std::size_t snapshots_pruned = 0;
    std::vector<std::string> kept_because;
};

// Applies the policy. Any condition that would destroy evidence refuses the
// whole run rather than pruning the parts that happen to be safe: a partial
// prune is harder to reason about than none.
RetentionResult prune(SqliteAtomicStore& store, const TenantId& tenant,
                      const RetentionPolicy& policy);

// --- Archive ---------------------------------------------------------------

struct ArchiveSegment {
    StreamId stream;
    Version from = 0;
    Version to = 0;
    std::string path;
    Digest content_hash{};
    std::size_t events = 0;
};

struct ArchiveManifest {
    TenantId tenant;
    std::string created_at;
    std::vector<ArchiveSegment> segments;
    // Digest over the state segment: the authoritative tables the journal
    // cannot reconstruct (dedup keys, intents, outbox, receipts, cases).
    Digest state_hash{};
    Digest manifest_hash{};
    // Identifies the database this archive came from. A restore that lands
    // in a different lineage is a diverged timeline, which section 17.2 says
    // must stop recovery rather than be merged.
    std::string lineage_id;
};

class Archive {
public:
    explicit Archive(std::string directory) : dir_(std::move(directory)) {}

    // Writes every stream as one or more segments plus a manifest.
    StoreError export_all(const SqliteAtomicStore& store, const TenantId& tenant,
                          std::size_t events_per_segment, ArchiveManifest* out,
                          std::string* why);

    bool read_manifest(ArchiveManifest* out, std::string* why) const;

    // Checks every segment against the manifest: present, readable, correct
    // checksum, and contiguous within its stream. Returns the incidents
    // found rather than a bare boolean, because "missing range" and
    // "corrupted segment" call for different operator actions.
    std::vector<Incident> verify(std::string* why) const;

    // Rebuilds a database from the archive. Even a clean restore comes back
    // with dispatch disabled: JI-31 forbids resuming outbound traffic before
    // the counterparty has been reconciled with.
    StoreError restore_into(const std::string& target_db_path, std::string* why) const;

    const std::string& directory() const { return dir_; }

private:
    std::string dir_;
};

struct RestoreDrillResult {
    bool restored = false;
    bool verified = false;
    bool dispatch_enabled = false;  // always false; stated so a test can assert it
    std::size_t events_restored = 0;
    std::int64_t duration_ms = 0;
    std::vector<Incident> incidents;
    std::string detail;
};

// Export, verify, restore to a scratch database and replay it. Section 17.2
// asks for drills rather than a green backup job, so this is the thing a
// release gate runs.
RestoreDrillResult restore_drill(SqliteAtomicStore& store, const TenantId& tenant,
                                 const std::string& archive_dir,
                                 const std::string& scratch_db_path);

}  // namespace jrnl
