// Deterministic replay and the integrity verifier.
//
// Section 12 of the design doc gives the replay algorithm and section 18 the
// integrity levels. The two belong together: replay is how level 3
// (projection versus replay equivalence) is computed, and the verifier is
// what the recovery supervisor consults before it will let anything be
// dispatched.
//
// Replay is pure. It opens no socket, reads no price feed and writes nothing.
// A mismatch is never repaired here; it produces an incident and the affected
// risk domain is frozen (JI-12, JI-39).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "jrnl/store.hpp"

namespace jrnl {

struct SnapshotRecord;

struct ReplayResult {
    bool ok = false;
    StreamId stream;
    Version version = 0;        // version reached
    Digest state_hash{};        // chain value at that version
    CanonValue state;           // canonical reduced state
    std::size_t events_applied = 0;
    Version started_from = 0;   // 0 when replayed from empty state
    std::vector<Incident> incidents;
    std::string detail;

    bool has(Incident i) const;
};

// Replays one stream from the empty state, or from a verified snapshot when
// one is supplied. The snapshot is a shortcut, never a substitute: the tail
// after it must be present and must chain onto it (JI-13).
ReplayResult replay_stream(const SqliteAtomicStore& store, const TenantId& tenant,
                           const StreamId& stream, const SnapshotRecord* from = nullptr);

// Full-history replay with no snapshot, for the differential check in
// section 26.3 item 8.
ReplayResult replay_from_empty(const SqliteAtomicStore& store, const TenantId& tenant,
                               const StreamId& stream);

struct VerifyReport {
    bool storage_valid = true;
    bool journal_valid = true;
    bool projections_valid = true;
    bool manifests_valid = true;
    bool dedup_valid = true;
    bool audit_valid = true;

    std::vector<Incident> incidents;
    std::vector<std::string> details;
    // Risk domains that must not admit new business until a human closes the
    // incident (JI-39).
    std::set<RiskDomainId> frozen_domains;
    std::size_t streams_checked = 0;
    std::size_t events_checked = 0;

    bool ok() const {
        return storage_valid && journal_valid && projections_valid && manifests_valid &&
               dedup_valid && audit_valid;
    }
    bool has(Incident i) const;
    void fail(Incident i, std::string detail, const StreamId& stream = "");
};

// Runs every integrity level that can be checked from inside the database:
// constraints, per-stream chains, projection equivalence, cross-stream
// manifests, dedup orphans and the audit chain. The two levels it cannot
// reach from here are stated rather than implied: an offsite immutable
// anchor and the counterparty's own execution history.
VerifyReport verify_all(const SqliteAtomicStore& store, const TenantId& tenant);

// Just the chain and version checks for one stream, for targeted tests.
VerifyReport verify_stream(const SqliteAtomicStore& store, const TenantId& tenant,
                           const StreamId& stream);

}  // namespace jrnl
