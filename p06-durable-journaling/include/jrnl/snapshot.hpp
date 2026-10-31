// Snapshots and checkpoints.
//
// Section 13: a snapshot is a shortcut for replay, not a backup and not a
// substitute for the journal. It becomes usable only after it has been read
// back and checked, and the lifecycle below is the one the doc draws:
//
//   read verified head -> serialise -> digest -> INSERT unverified
//     -> read back -> verify checksum, schema, version, anchor -> mark usable
//
// The gap between INSERT and the usable marker is crash window W22, so it is
// a real gap with a failpoint in it, not an implementation detail.
#pragma once

#include <string>
#include <vector>

#include "jrnl/store.hpp"

namespace jrnl {

struct SnapshotRecord {
    bool found = false;
    StreamId stream;
    Version version = 0;
    SchemaVersion schema_version = 0;
    std::string reducer_version;
    std::string serialized_state;  // canonical state text
    Digest content_hash{};         // over serialized_state
    Digest state_hash{};           // the stream chain value at this version
    Digest journal_anchor_hash{};  // chain value of the event at this version
    bool verified = false;
    std::int64_t created_at = 0;
};

class SnapshotStore {
public:
    explicit SnapshotStore(SqliteAtomicStore& store) : store_(store) {}

    // Writes an unverified snapshot of the stream's current head. Returns
    // Rejected when the stream has no history to snapshot.
    StoreError create(const TenantId& tenant, const StreamId& stream, Version* written);

    // Reads the row back, recomputes the digests and marks it usable. A
    // snapshot that fails here is left in place and unusable, with the reason
    // reported, rather than deleted: the failure is evidence.
    StoreError verify(const TenantId& tenant, const StreamId& stream, Version version,
                      std::string* why);

    // create() followed by verify(). The common path.
    StoreError checkpoint(const TenantId& tenant, const StreamId& stream, Version* written,
                          std::string* why);

    bool load(const TenantId& tenant, const StreamId& stream, Version version,
              SnapshotRecord* out) const;

    // Newest snapshot that is marked verified AND still checks out against
    // the journal. An unverified or stale one is skipped rather than used
    // with a warning (JI-13).
    bool newest_usable(const TenantId& tenant, const StreamId& stream,
                       SnapshotRecord* out) const;

    std::vector<SnapshotRecord> all(const TenantId& tenant, const StreamId& stream) const;

private:
    SqliteAtomicStore& store_;
};

}  // namespace jrnl
