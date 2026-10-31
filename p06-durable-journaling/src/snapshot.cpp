#include "jrnl/snapshot.hpp"

#include "jrnl/failpoint.hpp"
#include "jrnl/replay.hpp"
#include "jrnl/schema.hpp"

namespace jrnl {

StoreError SnapshotStore::create(const TenantId& tenant, const StreamId& stream,
                                 Version* written) {
    const StreamHead head = store_.head(tenant, stream);
    if (!head.exists || head.version == 0) return StoreError::Rejected;

    // The state comes from the projection, which the store wrote from the
    // reducer inside the business transaction. Taking it here rather than
    // replaying keeps snapshotting off the critical path, and verify() is
    // what makes the shortcut safe.
    std::string serialized;
    if (is_order_stream(stream)) {
        OrderState s;
        if (!store_.load_order(tenant, stream_suffix(stream), &s)) return StoreError::Corrupt;
        const IOrderReducer* r = store_.reducers().order(head.reducer_version);
        if (r == nullptr) return StoreError::Rejected;
        serialized = canonical_encode(r->to_canon(s));
    } else if (is_risk_stream(stream)) {
        RiskDomainState s;
        if (!store_.load_risk(tenant, stream_suffix(stream), &s)) return StoreError::Corrupt;
        const IRiskReducer* r = store_.reducers().risk(head.reducer_version);
        if (r == nullptr) return StoreError::Rejected;
        serialized = canonical_encode(r->to_canon(s));
    } else {
        return StoreError::Rejected;
    }

    // The anchor ties the snapshot to one specific committed event, so a
    // snapshot cannot later be paired with a journal it never came from.
    std::vector<JournalRecord> anchor;
    const StoreError re =
        store_.read_stream(tenant, stream, head.version, 1, &anchor);
    if (re != StoreError::None) return re;
    if (anchor.empty()) return StoreError::Corrupt;

    Stmt st;
    StoreError e = store_.db().prepare(
        "INSERT INTO snapshots(tenant_id,stream_id,stream_version,schema_version,"
        "reducer_version,serialized_state,content_hash,state_hash,journal_anchor_hash,"
        "verified,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,0,?10) "
        "ON CONFLICT(tenant_id,stream_id,stream_version) DO UPDATE SET "
        "serialized_state=excluded.serialized_state,content_hash=excluded.content_hash,"
        "state_hash=excluded.state_hash,journal_anchor_hash=excluded.journal_anchor_hash,"
        "reducer_version=excluded.reducer_version,verified=0,verified_at=NULL;",
        &st);
    if (e != StoreError::None) return e;
    st.bind_text(1, tenant);
    st.bind_text(2, stream);
    st.bind_u64(3, head.version);
    st.bind_i64(4, kSchemaVersion);
    st.bind_text(5, head.reducer_version);
    st.bind_text(6, serialized);
    st.bind_hex(7, sha256(serialized));
    st.bind_hex(8, head.state_hash);
    st.bind_hex(9, anchor.front().state_hash);
    st.bind_i64(10, store_.clock().now_ms());
    e = st.run();
    if (e != StoreError::None) return e;

    // W22. Everything above is durable; the usable marker is not yet set. A
    // process killed here must leave a snapshot that recovery ignores.
    if (JRNL_FP(fp::point::kAfterSnapshotInsert) == fp::Action::ReturnError) {
        return StoreError::IoError;
    }

    if (written != nullptr) *written = head.version;
    return StoreError::None;
}

StoreError SnapshotStore::verify(const TenantId& tenant, const StreamId& stream, Version version,
                                 std::string* why) {
    SnapshotRecord rec;
    if (!load(tenant, stream, version, &rec)) {
        if (why != nullptr) *why = "snapshot row not found";
        return StoreError::Rejected;
    }

    // Read back and recompute, rather than trusting what was just written.
    if (!(sha256(rec.serialized_state) == rec.content_hash)) {
        if (why != nullptr) *why = "content hash does not match the stored state";
        return StoreError::Corrupt;
    }
    if (rec.schema_version != kSchemaVersion) {
        if (why != nullptr) *why = "snapshot was written by another schema version";
        return StoreError::SchemaMismatch;
    }

    const StreamHead head = store_.head(tenant, stream);
    if (!head.exists || version > head.version) {
        if (why != nullptr) *why = "snapshot version is ahead of the journal head";
        return StoreError::Corrupt;
    }

    std::vector<JournalRecord> anchor;
    const StoreError re = store_.read_stream(tenant, stream, version, 1, &anchor);
    if (re != StoreError::None) return re;
    if (anchor.empty() || anchor.front().version != version) {
        if (why != nullptr) *why = "no journal event at the snapshot version";
        return StoreError::Corrupt;
    }
    if (!(anchor.front().state_hash == rec.journal_anchor_hash)) {
        if (why != nullptr) *why = "journal anchor hash does not match the event at that version";
        return StoreError::Corrupt;
    }

    // The decisive check: replaying from the snapshot must reach the same
    // head the journal reaches on its own. A snapshot that merely looks
    // well-formed is not usable (section 13).
    SnapshotRecord provisional = rec;
    provisional.verified = true;
    const ReplayResult tail = replay_stream(store_, tenant, stream, &provisional);
    if (!tail.ok) {
        if (why != nullptr) *why = "replay from snapshot failed: " + tail.detail;
        return StoreError::Corrupt;
    }

    // The other half of W22: the verification passed but the process never
    // got to record it. The snapshot stays unusable, which is the safe way
    // round.
    if (JRNL_FP(fp::point::kBeforeSnapshotVerified) == fp::Action::ReturnError) {
        if (why != nullptr) *why = "crashed before the snapshot was marked usable";
        return StoreError::IoError;
    }

    Stmt st;
    StoreError e = store_.db().prepare(
        "UPDATE snapshots SET verified=1, verified_at=?1 WHERE tenant_id=?2 AND stream_id=?3 "
        "AND stream_version=?4;",
        &st);
    if (e != StoreError::None) return e;
    st.bind_i64(1, store_.clock().now_ms());
    st.bind_text(2, tenant);
    st.bind_text(3, stream);
    st.bind_u64(4, version);
    return st.run();
}

StoreError SnapshotStore::checkpoint(const TenantId& tenant, const StreamId& stream,
                                     Version* written, std::string* why) {
    Version v = 0;
    StoreError e = create(tenant, stream, &v);
    if (e != StoreError::None) return e;
    e = verify(tenant, stream, v, why);
    if (e != StoreError::None) return e;
    if (written != nullptr) *written = v;
    return StoreError::None;
}

bool SnapshotStore::load(const TenantId& tenant, const StreamId& stream, Version version,
                         SnapshotRecord* out) const {
    Stmt s;
    if (const_cast<Db&>(store_.db()).prepare(
            "SELECT schema_version,reducer_version,serialized_state,content_hash,state_hash,"
            "journal_anchor_hash,verified,created_at FROM snapshots WHERE tenant_id=?1 AND "
            "stream_id=?2 AND stream_version=?3;",
            &s) != StoreError::None) {
        return false;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, stream);
    s.bind_u64(3, version);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    out->found = true;
    out->stream = stream;
    out->version = version;
    out->schema_version = static_cast<SchemaVersion>(s.col_i64(0));
    out->reducer_version = s.col_text(1);
    out->serialized_state = s.col_text(2);
    if (!s.col_hex(3, &out->content_hash) || !s.col_hex(4, &out->state_hash) ||
        !s.col_hex(5, &out->journal_anchor_hash)) {
        return false;
    }
    out->verified = s.col_i64(6) != 0;
    out->created_at = s.col_i64(7);
    return true;
}

bool SnapshotStore::newest_usable(const TenantId& tenant, const StreamId& stream,
                                  SnapshotRecord* out) const {
    for (const SnapshotRecord& r : all(tenant, stream)) {
        if (!r.verified) continue;
        // Re-check rather than trusting the marker. A row can be marked
        // verified and then altered, and section 13 says a snapshot with a
        // hash mismatch is simply not used.
        if (!(sha256(r.serialized_state) == r.content_hash)) continue;
        const StreamHead head = store_.head(tenant, stream);
        if (!head.exists || r.version > head.version) continue;
        *out = r;
        return true;
    }
    return false;
}

std::vector<SnapshotRecord> SnapshotStore::all(const TenantId& tenant,
                                               const StreamId& stream) const {
    std::vector<SnapshotRecord> out;
    Stmt s;
    if (const_cast<Db&>(store_.db()).prepare(
            "SELECT stream_version FROM snapshots WHERE tenant_id=?1 AND stream_id=?2 "
            "ORDER BY stream_version DESC;",
            &s) != StoreError::None) {
        return out;
    }
    s.bind_text(1, tenant);
    s.bind_text(2, stream);
    std::vector<Version> versions;
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) break;
        versions.push_back(s.col_u64(0));
    }
    for (Version v : versions) {
        SnapshotRecord r;
        if (load(tenant, stream, v, &r)) out.push_back(std::move(r));
    }
    return out;
}

}  // namespace jrnl
