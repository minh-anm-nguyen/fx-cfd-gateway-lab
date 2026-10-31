#include "jrnl/result.hpp"

namespace jrnl {

std::string_view to_string(StoreError e) {
    switch (e) {
        case StoreError::None: return "None";
        case StoreError::Busy: return "Busy";
        case StoreError::Serialization: return "Serialization";
        case StoreError::Deadlock: return "Deadlock";
        case StoreError::Conflict: return "Conflict";
        case StoreError::StaleVersion: return "StaleVersion";
        case StoreError::Full: return "Full";
        case StoreError::IoError: return "IoError";
        case StoreError::Corrupt: return "Corrupt";
        case StoreError::SchemaMismatch: return "SchemaMismatch";
        case StoreError::NotOwner: return "NotOwner";
        case StoreError::CommitUnknown: return "CommitUnknown";
        case StoreError::Rejected: return "Rejected";
        case StoreError::Quarantined: return "Quarantined";
        case StoreError::Unknown: return "Unknown";
    }
    return "Unknown";
}

bool is_retryable(StoreError e) {
    // Deliberately excludes CommitUnknown. Section 9.3 separates the two: an
    // abort left nothing behind, an unknown commit may have left everything
    // behind, and retrying the second as if it were the first is how a command
    // happens twice.
    return e == StoreError::Busy || e == StoreError::Serialization ||
           e == StoreError::Deadlock;
}

bool is_ambiguous(StoreError e) { return e == StoreError::CommitUnknown; }

bool is_fail_closed(StoreError e) {
    return e == StoreError::Full || e == StoreError::IoError || e == StoreError::Corrupt ||
           e == StoreError::SchemaMismatch;
}

std::string_view to_string(CommitKind k) {
    switch (k) {
        case CommitKind::Committed: return "ACCEPTED_DURABLE";
        case CommitKind::Replayed: return "REPLAYED";
        case CommitKind::Noop: return "NOOP";
        case CommitKind::Conflict: return "CONFLICT";
        case CommitKind::Quarantined: return "QUARANTINED";
        case CommitKind::Unknown: return "COMMIT_UNKNOWN";
        case CommitKind::Rejected: return "REJECTED";
    }
    return "REJECTED";
}

std::string_view to_string(Incident i) {
    switch (i) {
        case Incident::None: return "None";
        case Incident::StreamVersionGap: return "StreamVersionGap";
        case Incident::StreamVersionDuplicate: return "StreamVersionDuplicate";
        case Incident::HashChainBroken: return "HashChainBroken";
        case Incident::StateHashMismatch: return "StateHashMismatch";
        case Incident::HeadMismatch: return "HeadMismatch";
        case Incident::TxManifestMismatch: return "TxManifestMismatch";
        case Incident::ProjectionReplayMismatch: return "ProjectionReplayMismatch";
        case Incident::SnapshotChecksumFailure: return "SnapshotChecksumFailure";
        case Incident::SnapshotAheadOfJournal: return "SnapshotAheadOfJournal";
        case Incident::SnapshotUnverified: return "SnapshotUnverified";
        case Incident::OrphanOutbox: return "OrphanOutbox";
        case Incident::OrphanDedupKey: return "OrphanDedupKey";
        case Incident::DuplicateExecFingerprint: return "DuplicateExecFingerprint";
        case Incident::RiskLedgerDiff: return "RiskLedgerDiff";
        case Incident::OverfillDetected: return "OverfillDetected";
        case Incident::OverReleaseDetected: return "OverReleaseDetected";
        case Incident::CrossTenantLeak: return "CrossTenantLeak";
        case Incident::StaleFence: return "StaleFence";
        case Incident::UnknownOutboundOutcome: return "UnknownOutboundOutcome";
        case Incident::MissingArchiveRange: return "MissingArchiveRange";
        case Incident::RestoreLineageDiverged: return "RestoreLineageDiverged";
        case Incident::SchemaIncompatible: return "SchemaIncompatible";
        case Incident::ReducerUnavailable: return "ReducerUnavailable";
        case Incident::RetentionWouldDestroyEvidence: return "RetentionWouldDestroyEvidence";
        case Incident::DurabilityProfileUnsafe: return "DurabilityProfileUnsafe";
        case Incident::BackpressureLimit: return "BackpressureLimit";
    }
    return "None";
}

}  // namespace jrnl
