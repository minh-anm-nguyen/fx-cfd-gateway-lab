// Typed error taxonomy.
//
// Design doc section 20 asks for typed error mapping rather than exceptions
// carrying control flow, and section 9.3 makes the distinction that matters
// most: a serialization abort is a retry with the SAME idempotency key, an
// ambiguous commit is NOT, and a corruption is never either.
#pragma once

#include <string>
#include <string_view>

#include "jrnl/ids.hpp"

namespace jrnl {

// Storage-layer failures. The classification drives policy, so each value has
// exactly one meaning and the mapping from the SQL engine is in one place.
enum class StoreError {
    None = 0,
    Busy,             // lock contention; retry same key                 G13
    Serialization,    // 40001-class abort; retry same key               G13
    Deadlock,         // 40P01-class abort; retry same key               G13
    Conflict,         // unique violation: the same key with other content G01-07
    StaleVersion,     // the stream moved under us; rebuild and re-apply    G01-04
    Full,             // ENOSPC / SQLITE_FULL; fail closed               G15
    IoError,          // EIO / permission revoked; fail closed           G15
    Corrupt,          // integrity check failed; quarantine              G15
    SchemaMismatch,   // database written by another schema version      G18
    NotOwner,         // fencing token is stale                          G14
    CommitUnknown,    // connection lost at COMMIT; resolve by key       G13
    Rejected,         // permanent validation failure, no mutation
    Quarantined,      // contradictory evidence; frozen, not applied
    Unknown,
};

std::string_view to_string(StoreError e);

// True when the only safe response is to re-enter the transaction with the
// exact same canonical key and request hash (invariant JI-27).
bool is_retryable(StoreError e);

// True when the outcome of the write is genuinely unknown. JI-15 forbids
// reading this as either a rollback or an acceptance.
bool is_ambiguous(StoreError e);

// True when the engine must stop admitting work rather than continue on a
// best-effort basis (JI-29).
bool is_fail_closed(StoreError e);

// The verdict a business command receives, from section 1.2.
enum class CommitKind {
    Committed,   // ACCEPTED_DURABLE under the pinned durability profile
    Replayed,    // same key, same hash: the earlier durable result is returned
    Noop,        // duplicate with nothing left to do
    Conflict,    // same key, different request hash
    Quarantined, // integrity or identity contradiction
    Unknown,     // COMMIT_UNKNOWN
    Rejected,    // refused before any mutation
};

std::string_view to_string(CommitKind k);

struct CommitOutcome {
    CommitKind kind = CommitKind::Rejected;
    StoreError error = StoreError::None;
    TxId tx;              // manifest id of the committed transaction, when there is one
    std::string detail;   // human-readable reason, for audit and test failure messages

    bool durable() const { return kind == CommitKind::Committed || kind == CommitKind::Replayed; }
};

// Reasons a domain is frozen or the supervisor refuses to go ACTIVE.
enum class Incident {
    None = 0,
    StreamVersionGap,
    StreamVersionDuplicate,
    HashChainBroken,
    StateHashMismatch,
    HeadMismatch,
    TxManifestMismatch,
    ProjectionReplayMismatch,
    SnapshotChecksumFailure,
    SnapshotAheadOfJournal,
    SnapshotUnverified,
    OrphanOutbox,
    OrphanDedupKey,
    DuplicateExecFingerprint,
    RiskLedgerDiff,
    OverfillDetected,
    OverReleaseDetected,
    CrossTenantLeak,
    StaleFence,
    UnknownOutboundOutcome,
    MissingArchiveRange,
    RestoreLineageDiverged,
    SchemaIncompatible,
    ReducerUnavailable,
    RetentionWouldDestroyEvidence,
    DurabilityProfileUnsafe,
    BackpressureLimit,
};

std::string_view to_string(Incident i);

}  // namespace jrnl
