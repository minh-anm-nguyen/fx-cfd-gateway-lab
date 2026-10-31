#include "jrnl/retention.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "jrnl/replay.hpp"
#include "jrnl/schema.hpp"

namespace jrnl {
namespace {

std::string escape_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out.append("\\\\");
        else if (c == '\t') out.append("\\t");
        else if (c == '\n') out.append("\\n");
        else out.push_back(c);
    }
    return out;
}

std::string unescape_field(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            if (s[i] == 't') out.push_back('\t');
            else if (s[i] == 'n') out.push_back('\n');
            else out.push_back(s[i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Tables that are authoritative rather than derived. The journal alone
// cannot reconstruct them: an execution dedup key carries a counterparty
// namespace and a fingerprint that never appear in a domain event, and
// restoring without them would leave the instance unable to recognise a
// re-reported trade. Orders, risk domains and reservations are deliberately
// absent: those ARE derived, and the restore rebuilds them by replay.
struct TableSpec {
    const char* name;
    const char* cols;
    int ncols;
};

const TableSpec kStateTables[] = {
    {"command_intents",
     "tenant_id,account_id,command_id,order_chain_id,canonical_hash,risk_reservation_id,"
     "decision,result,created_tx,created_at",
     10},
    {"command_outbox",
     "outbox_id,tenant_id,account_id,command_id,session_key,original_payload_hash,"
     "original_payload,state,p01_original_seq,claim_owner,claim_token,claim_expires_at,"
     "attempts,send_attempted,last_error,created_tx,created_at",
     17},
    {"inbox_receipts",
     "tenant_id,session_key,session_epoch,receipt_id,payload_hash,apply_state,tx_id,received_at",
     8},
    {"business_exec_keys",
     "tenant_id,lp_namespace,semantic_exec_id,fingerprint,order_chain_id,applied_tx,created_at",
     7},
    {"fills",
     "tenant_id,lp_namespace,semantic_exec_id,order_chain_id,last_qty,last_px,applied_tx,"
     "created_at",
     8},
    {"recovery_cases",
     "tenant_id,case_id,order_chain_id,reason_code,evidence_hash,status,opened_at,resolved_at,"
     "approvals",
     9},
};

// Distinguishable from an empty string, which several of these columns use.
constexpr const char* kNullMarker = "\\N";

std::string placeholders(int n) {
    std::string out;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) out.push_back(',');
        out += "?" + std::to_string(i);
    }
    return out;
}

std::string serialize_event(const TenantId& tenant, const JournalRecord& r) {
    std::ostringstream os;
    os << escape_field(tenant) << '\t' << escape_field(r.stream) << '\t' << r.version << '\t'
       << escape_field(r.event_id) << '\t' << escape_field(r.tx_id) << '\t' << r.ordinal << '\t'
       << escape_field(r.event_type) << '\t' << r.schema_version << '\t'
       << escape_field(r.reducer_version) << '\t' << r.policy_version << '\t'
       << escape_field(r.payload_text) << '\t' << to_hex(r.canonical_hash) << '\t'
       << to_hex(r.prev_state_hash) << '\t' << to_hex(r.state_hash) << '\t' << r.created_at;
    return os.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

RetentionResult prune(SqliteAtomicStore& store, const TenantId& tenant,
                      const RetentionPolicy& policy) {
    RetentionResult res;
    Db& db = store.db();

    if (policy.allow_journal_deletion) {
        // v1 does not delete journal, and the append-only triggers would
        // refuse anyway. Saying so here makes the refusal legible instead of
        // surfacing as an opaque SQL error.
        res.refused = true;
        res.refusal_reason =
            "journal deletion is not supported in v1; archive first and change the recovery "
            "contract explicitly";
        return res;
    }

    // JI-14, one guard per kind of evidence. Each one refuses the whole run.
    {
        Stmt s;
        if (db.prepare("SELECT COUNT(*) FROM orders WHERE tenant_id=?1 AND leaves_qty>0;", &s) ==
            StoreError::None) {
            s.bind_text(1, tenant);
            bool row = false;
            if (s.step(&row) == StoreError::None && row && s.col_i64(0) > 0) {
                res.refused = true;
                res.refusal_reason = std::to_string(s.col_i64(0)) +
                                     " orders are still open; their dedup evidence is needed";
                return res;
            }
        }
    }
    {
        Stmt s;
        if (db.prepare("SELECT COUNT(*) FROM command_outbox WHERE tenant_id=?1 AND state IN "
                       "('READY','CLAIMED','BOUND','UNKNOWN','HELD');",
                       &s) == StoreError::None) {
            s.bind_text(1, tenant);
            bool row = false;
            if (s.step(&row) == StoreError::None && row && s.col_i64(0) > 0) {
                res.refused = true;
                res.refusal_reason = std::to_string(s.col_i64(0)) +
                                     " outbound effects are unresolved";
                return res;
            }
        }
    }
    {
        Stmt s;
        if (db.prepare("SELECT COUNT(*) FROM recovery_cases WHERE tenant_id=?1 AND "
                       "status<>'RESOLVED';",
                       &s) == StoreError::None) {
            s.bind_text(1, tenant);
            bool row = false;
            if (s.step(&row) == StoreError::None && row && s.col_i64(0) > 0) {
                res.refused = true;
                res.refusal_reason = std::to_string(s.col_i64(0)) +
                                     " reconciliation cases are open";
                return res;
            }
        }
    }

    const std::int64_t now = store.clock().now_ms();
    const std::int64_t receipt_cutoff = now - policy.horizon_ms;
    const std::int64_t resend_cutoff = now - policy.resend_window_ms;
    const std::int64_t cutoff = receipt_cutoff < resend_cutoff ? receipt_cutoff : resend_cutoff;

    TxGuard tx(db);
    if (tx.begin() != StoreError::None) {
        res.refused = true;
        res.refusal_reason = "cannot begin retention transaction";
        return res;
    }

    {
        Stmt c;
        if (db.prepare("SELECT COUNT(*) FROM inbox_receipts WHERE tenant_id=?1 AND "
                       "received_at<?2;",
                       &c) == StoreError::None) {
            c.bind_text(1, tenant);
            c.bind_i64(2, cutoff);
            bool row = false;
            if (c.step(&row) == StoreError::None && row) {
                res.receipts_pruned = static_cast<std::size_t>(c.col_i64(0));
            }
        }
        Stmt s;
        if (db.prepare("DELETE FROM inbox_receipts WHERE tenant_id=?1 AND received_at<?2;", &s) ==
            StoreError::None) {
            s.bind_text(1, tenant);
            s.bind_i64(2, cutoff);
            s.run();
        }
    }

    // Execution dedup keys are NOT pruned in v1. A counterparty can re-report
    // an old trade long after the transport receipt is gone, and the key is
    // the only thing that stops it becoming a second fill (JI-06). Removing
    // one while its fill row is still on the books would additionally leave
    // the ledger with an orphan that the integrity verifier is right to
    // reject, so the key and the fill have to be archived together or kept
    // together. command_key_horizon_ms therefore has no effect here yet and
    // is reserved for the archival policy section 13 defers.
    res.exec_keys_pruned = 0;

    // Superseded snapshots. The newest verified one per stream is always
    // kept, because deleting it would raise recovery time for no benefit.
    {
        Stmt s;
        if (db.prepare(
                "DELETE FROM snapshots WHERE tenant_id=?1 AND (stream_id,stream_version) NOT IN "
                "(SELECT stream_id, MAX(stream_version) FROM snapshots WHERE tenant_id=?1 AND "
                "verified=1 GROUP BY stream_id);",
                &s) == StoreError::None) {
            s.bind_text(1, tenant);
            s.run();
        }
    }

    // Command intents are never pruned here. Section 18: an expired
    // idempotency key that a client later reuses places the order twice, so
    // the tombstone stays until an explicit, separately-reviewed policy says
    // otherwise.
    res.kept_because.push_back(
        "command_intents retained: an expired idempotency key can be replayed by a client");
    res.kept_because.push_back("journal_events retained: v1 deletes no journal");
    res.kept_because.push_back(
        "business_exec_keys retained: the key is the only defence against a re-reported "
        "trade, and its fill row would be orphaned without it");

    if (tx.commit() != StoreError::None) {
        res.refused = true;
        res.refusal_reason = "retention transaction did not commit";
    }
    return res;
}

// ---------------------------------------------------------------------------
// Archive
// ---------------------------------------------------------------------------

StoreError Archive::export_all(const SqliteAtomicStore& store, const TenantId& tenant,
                               std::size_t events_per_segment, ArchiveManifest* out,
                               std::string* why) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        if (why != nullptr) *why = "cannot create archive directory: " + ec.message();
        return StoreError::IoError;
    }
    if (events_per_segment == 0) events_per_segment = 1000;

    ArchiveManifest man;
    man.tenant = tenant;
    man.created_at = store.clock().now_iso8601();
    // Lineage is derived from the first event of the first stream: any two
    // databases that share it share a history prefix.
    man.lineage_id = "empty";

    Sha256 overall;
    overall.update(tenant);

    for (const StreamId& stream : store.list_streams(tenant)) {
        std::vector<JournalRecord> all;
        const StoreError e = store.read_stream(tenant, stream, 1, 0, &all);
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot read stream " + stream;
            return e;
        }
        if (all.empty()) continue;
        if (man.lineage_id == "empty") man.lineage_id = to_hex(all.front().state_hash);

        for (std::size_t i = 0; i < all.size(); i += events_per_segment) {
            const std::size_t n = std::min(events_per_segment, all.size() - i);
            ArchiveSegment seg;
            seg.stream = stream;
            seg.from = all[i].version;
            seg.to = all[i + n - 1].version;
            seg.events = n;
            seg.path = "seg-" + to_hex(sha256(stream)).substr(0, 12) + "-" +
                       std::to_string(seg.from) + "-" + std::to_string(seg.to) + ".seg";

            std::string body;
            for (std::size_t k = i; k < i + n; ++k) {
                body += serialize_event(tenant, all[k]);
                body.push_back('\n');
            }
            seg.content_hash = sha256(body);

            std::ofstream f(std::filesystem::path(dir_) / seg.path,
                            std::ios::binary | std::ios::trunc);
            if (!f) {
                if (why != nullptr) *why = "cannot write segment " + seg.path;
                return StoreError::IoError;
            }
            f << body;
            f.close();

            overall.update(seg.path);
            overall.update(seg.content_hash);
            man.segments.push_back(std::move(seg));
        }
    }

    // The authoritative non-derived tables travel alongside the journal.
    {
        std::string body;
        Db& db = const_cast<Db&>(store.db());
        for (const TableSpec& t : kStateTables) {
            Stmt s;
            if (db.prepare(std::string("SELECT ") + t.cols + " FROM " + t.name +
                               " WHERE tenant_id=?1;",
                           &s) != StoreError::None) {
                if (why != nullptr) *why = std::string("cannot read ") + t.name;
                return StoreError::IoError;
            }
            s.bind_text(1, tenant);
            while (true) {
                bool row = false;
                if (s.step(&row) != StoreError::None) {
                    if (why != nullptr) *why = std::string("cannot read ") + t.name;
                    return StoreError::IoError;
                }
                if (!row) break;
                body += t.name;
                for (int i = 0; i < t.ncols; ++i) {
                    body.push_back('\t');
                    body += s.col_is_null(i) ? kNullMarker : escape_field(s.col_text(i));
                }
                body.push_back('\n');
            }
        }
        std::ofstream f(std::filesystem::path(dir_) / "state.seg",
                        std::ios::binary | std::ios::trunc);
        if (!f) {
            if (why != nullptr) *why = "cannot write the state segment";
            return StoreError::IoError;
        }
        f << body;
        f.close();
        man.state_hash = sha256(body);
        overall.update("state.seg");
        overall.update(man.state_hash);
    }

    man.manifest_hash = overall.finish();

    std::ofstream mf(std::filesystem::path(dir_) / "manifest.txt",
                     std::ios::binary | std::ios::trunc);
    if (!mf) {
        if (why != nullptr) *why = "cannot write manifest";
        return StoreError::IoError;
    }
    mf << "TENANT\t" << escape_field(man.tenant) << "\n";
    mf << "CREATED\t" << escape_field(man.created_at) << "\n";
    mf << "LINEAGE\t" << man.lineage_id << "\n";
    mf << "STATE\t" << to_hex(man.state_hash) << "\n";
    for (const ArchiveSegment& s : man.segments) {
        mf << "SEGMENT\t" << escape_field(s.stream) << "\t" << s.from << "\t" << s.to << "\t"
           << s.events << "\t" << to_hex(s.content_hash) << "\t" << s.path << "\n";
    }
    mf << "MANIFEST\t" << to_hex(man.manifest_hash) << "\n";
    mf.close();

    if (out != nullptr) *out = man;
    return StoreError::None;
}

bool Archive::read_manifest(ArchiveManifest* out, std::string* why) const {
    std::ifstream f(std::filesystem::path(dir_) / "manifest.txt", std::ios::binary);
    if (!f) {
        if (why != nullptr) *why = "manifest is missing";
        return false;
    }
    ArchiveManifest man;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> cols = split_tabs(line);
        if (cols.empty()) continue;
        if (cols[0] == "TENANT" && cols.size() >= 2) man.tenant = unescape_field(cols[1]);
        else if (cols[0] == "CREATED" && cols.size() >= 2) man.created_at = unescape_field(cols[1]);
        else if (cols[0] == "LINEAGE" && cols.size() >= 2) man.lineage_id = cols[1];
        else if (cols[0] == "STATE" && cols.size() >= 2) from_hex(cols[1], &man.state_hash);
        else if (cols[0] == "MANIFEST" && cols.size() >= 2) from_hex(cols[1], &man.manifest_hash);
        else if (cols[0] == "SEGMENT" && cols.size() >= 7) {
            ArchiveSegment s;
            s.stream = unescape_field(cols[1]);
            s.from = std::strtoull(cols[2].c_str(), nullptr, 10);
            s.to = std::strtoull(cols[3].c_str(), nullptr, 10);
            s.events = static_cast<std::size_t>(std::strtoull(cols[4].c_str(), nullptr, 10));
            if (!from_hex(cols[5], &s.content_hash)) {
                if (why != nullptr) *why = "segment checksum is malformed";
                return false;
            }
            s.path = cols[6];
            man.segments.push_back(std::move(s));
        }
    }
    if (out != nullptr) *out = man;
    return true;
}

std::vector<Incident> Archive::verify(std::string* why) const {
    std::vector<Incident> out;
    ArchiveManifest man;
    if (!read_manifest(&man, why)) {
        out.push_back(Incident::MissingArchiveRange);
        return out;
    }

    Sha256 overall;
    overall.update(man.tenant);
    std::map<StreamId, Version> expected_next;

    for (const ArchiveSegment& s : man.segments) {
        const std::filesystem::path p = std::filesystem::path(dir_) / s.path;
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            // Section 17.2: a missing WAL segment stops recovery. The
            // equivalent here is a missing journal segment, and the answer
            // is the same — stop, do not reconstruct by assuming the events
            // never existed.
            out.push_back(Incident::MissingArchiveRange);
            if (why != nullptr) *why = "segment " + s.path + " is missing";
            continue;
        }
        std::ostringstream body;
        body << f.rdbuf();
        const std::string content = body.str();
        if (!(sha256(content) == s.content_hash)) {
            out.push_back(Incident::HashChainBroken);
            if (why != nullptr) *why = "segment " + s.path + " fails its checksum";
            continue;
        }
        overall.update(s.path);
        overall.update(s.content_hash);

        auto it = expected_next.find(s.stream);
        const Version want = it == expected_next.end() ? 1 : it->second;
        if (s.from != want) {
            out.push_back(Incident::MissingArchiveRange);
            if (why != nullptr) {
                *why = "stream " + s.stream + " jumps from version " + std::to_string(want) +
                       " to " + std::to_string(s.from);
            }
        }
        expected_next[s.stream] = s.to + 1;
    }

    {
        std::ifstream f(std::filesystem::path(dir_) / "state.seg", std::ios::binary);
        if (!f) {
            out.push_back(Incident::MissingArchiveRange);
            if (why != nullptr) *why = "the state segment is missing";
        } else {
            std::ostringstream body;
            body << f.rdbuf();
            if (!(sha256(body.str()) == man.state_hash)) {
                out.push_back(Incident::HashChainBroken);
                if (why != nullptr) *why = "the state segment fails its checksum";
            } else {
                overall.update("state.seg");
                overall.update(man.state_hash);
            }
        }
    }

    if (out.empty() && !(overall.finish() == man.manifest_hash)) {
        out.push_back(Incident::HashChainBroken);
        if (why != nullptr) *why = "manifest hash does not cover the segments present";
    }
    return out;
}

StoreError Archive::restore_into(const std::string& target_db_path, std::string* why) const {
    ArchiveManifest man;
    if (!read_manifest(&man, why)) return StoreError::IoError;

    const std::vector<Incident> problems = verify(why);
    if (!problems.empty()) {
        // JI-29: a restore from an archive that does not verify is refused.
        // "Best effort" here would mean silently losing committed history.
        return StoreError::Corrupt;
    }

    std::error_code ec;
    std::filesystem::remove(target_db_path, ec);

    Db::Options dopt;
    dopt.path = target_db_path;
    dopt.create_if_missing = true;
    std::unique_ptr<Db> db;
    StoreError e = Db::open(dopt, &db, why);
    if (e != StoreError::None) return e;
    e = apply_schema(*db, why);
    if (e != StoreError::None) return e;

    TxGuard tx(*db);
    e = tx.begin();
    if (e != StoreError::None) return e;

    std::map<std::string, int> tx_counts;
    std::vector<std::vector<std::string>> rows;

    for (const ArchiveSegment& s : man.segments) {
        std::ifstream f(std::filesystem::path(dir_) / s.path, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> c = split_tabs(line);
            if (c.size() < 15) {
                if (why != nullptr) *why = "malformed row in segment " + s.path;
                return StoreError::Corrupt;
            }
            ++tx_counts[unescape_field(c[4])];
            rows.push_back(std::move(c));
        }
    }

    // Manifests first, because the events carry a foreign key to them.
    for (const auto& entry : tx_counts) {
        Stmt st;
        e = db->prepare(
            "INSERT INTO journal_transactions(tenant_id,tx_id,expected_event_count,"
            "members_digest,created_at) VALUES(?1,?2,?3,?4,0) "
            "ON CONFLICT(tenant_id,tx_id) DO NOTHING;",
            &st);
        if (e != StoreError::None) return e;
        st.bind_text(1, man.tenant);
        st.bind_text(2, entry.first);
        st.bind_i64(3, entry.second);
        // The members digest is recomputed below once every member is in.
        st.bind_hex(4, zero_digest());
        e = st.run();
        if (e != StoreError::None) return e;
    }

    std::map<std::string, std::vector<Digest>> members;
    std::map<StreamId, std::pair<Version, Digest>> heads;
    std::map<StreamId, std::string> head_reducer;

    for (const std::vector<std::string>& c : rows) {
        const std::string tenant = unescape_field(c[0]);
        const StreamId stream = unescape_field(c[1]);
        const Version version = std::strtoull(c[2].c_str(), nullptr, 10);
        const std::string payload = unescape_field(c[10]);

        Stmt st;
        e = db->prepare(
            "INSERT INTO journal_events(tenant_id,stream_id,stream_version,event_id,tx_id,"
            "event_ordinal,event_type,schema_version,reducer_version,policy_version,payload,"
            "canonical_hash,prev_state_hash,state_hash,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);",
            &st);
        if (e != StoreError::None) return e;
        st.bind_text(1, tenant);
        st.bind_text(2, stream);
        st.bind_u64(3, version);
        st.bind_text(4, unescape_field(c[3]));
        st.bind_text(5, unescape_field(c[4]));
        st.bind_i64(6, std::strtoll(c[5].c_str(), nullptr, 10));
        st.bind_text(7, unescape_field(c[6]));
        st.bind_i64(8, std::strtoll(c[7].c_str(), nullptr, 10));
        st.bind_text(9, unescape_field(c[8]));
        st.bind_i64(10, std::strtoll(c[9].c_str(), nullptr, 10));
        st.bind_text(11, payload);
        st.bind_text(12, c[11]);
        st.bind_text(13, c[12]);
        st.bind_text(14, c[13]);
        st.bind_i64(15, std::strtoll(c[14].c_str(), nullptr, 10));
        e = st.run();
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot restore event: " + db->last_error();
            return e;
        }

        members[unescape_field(c[4])].push_back(sha256(payload));
        Digest sh{};
        from_hex(c[13], &sh);
        auto it = heads.find(stream);
        if (it == heads.end() || version > it->second.first) {
            heads[stream] = {version, sh};
            head_reducer[stream] = unescape_field(c[8]);
        }
    }

    for (const auto& entry : members) {
        Stmt st;
        e = db->prepare(
            "UPDATE journal_transactions SET members_digest=?1 WHERE tenant_id=?2 AND tx_id=?3;",
            &st);
        if (e != StoreError::None) return e;
        st.bind_hex(1, members_digest(entry.second));
        st.bind_text(2, man.tenant);
        st.bind_text(3, entry.first);
        e = st.run();
        if (e != StoreError::None) return e;
    }

    for (const auto& entry : heads) {
        Stmt st;
        e = db->prepare(
            "INSERT INTO stream_heads(tenant_id,stream_id,current_version,state_hash,"
            "reducer_version,fencing_token) VALUES(?1,?2,?3,?4,?5,0);",
            &st);
        if (e != StoreError::None) return e;
        st.bind_text(1, man.tenant);
        st.bind_text(2, entry.first);
        st.bind_u64(3, entry.second.first);
        st.bind_hex(4, entry.second.second);
        st.bind_text(5, head_reducer[entry.first]);
        e = st.run();
        if (e != StoreError::None) return e;
    }

    // The authoritative tables. Order matters: command_outbox carries a
    // foreign key to command_intents, and kStateTables is listed so that
    // parents come first.
    {
        std::ifstream f(std::filesystem::path(dir_) / "state.seg", std::ios::binary);
        if (!f) {
            if (why != nullptr) *why = "the state segment is missing";
            return StoreError::Corrupt;
        }
        std::map<std::string, std::vector<std::vector<std::string>>> by_table;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> c = split_tabs(line);
            if (c.empty()) continue;
            const std::string table = c.front();
            c.erase(c.begin());
            by_table[table].push_back(std::move(c));
        }
        for (const TableSpec& t : kStateTables) {
            const auto it = by_table.find(t.name);
            if (it == by_table.end()) continue;
            for (const std::vector<std::string>& row : it->second) {
                if (static_cast<int>(row.size()) != t.ncols) {
                    if (why != nullptr) {
                        *why = std::string("malformed state row for ") + t.name;
                    }
                    return StoreError::Corrupt;
                }
                Stmt st;
                e = db->prepare(std::string("INSERT INTO ") + t.name + "(" + t.cols +
                                    ") VALUES(" + placeholders(t.ncols) + ");",
                                &st);
                if (e != StoreError::None) return e;
                for (int i = 0; i < t.ncols; ++i) {
                    if (row[static_cast<std::size_t>(i)] == kNullMarker) st.bind_null(i + 1);
                    else st.bind_text(i + 1, unescape_field(row[static_cast<std::size_t>(i)]));
                }
                e = st.run();
                if (e != StoreError::None) {
                    if (why != nullptr) {
                        *why = std::string("cannot restore ") + t.name + ": " +
                               db->last_error();
                    }
                    return e;
                }
            }
        }
    }

    e = tx.commit();
    if (e != StoreError::None) return e;

    // Projections are derived, so they are rebuilt from the journal rather
    // than copied. This is also the cheapest possible proof that what came
    // back replays: if it did not, the rebuild fails here.
    SqliteAtomicStore::Options sopt;
    sopt.path = target_db_path;
    sopt.create_if_missing = false;
    std::unique_ptr<SqliteAtomicStore> store;
    e = SqliteAtomicStore::open(sopt, &store, why);
    if (e != StoreError::None) return e;
    return store->rebuild_projections(man.tenant, why);
}

RestoreDrillResult restore_drill(SqliteAtomicStore& store, const TenantId& tenant,
                                 const std::string& archive_dir,
                                 const std::string& scratch_db_path) {
    RestoreDrillResult out;
    const std::int64_t t0 = store.clock().steady_ms();

    Archive ar(archive_dir);
    ArchiveManifest man;
    std::string why;
    if (ar.export_all(store, tenant, 100, &man, &why) != StoreError::None) {
        out.detail = "export failed: " + why;
        out.duration_ms = store.clock().steady_ms() - t0;
        return out;
    }

    out.incidents = ar.verify(&why);
    if (!out.incidents.empty()) {
        out.detail = "archive verification failed: " + why;
        out.duration_ms = store.clock().steady_ms() - t0;
        return out;
    }

    if (ar.restore_into(scratch_db_path, &why) != StoreError::None) {
        out.detail = "restore failed: " + why;
        out.duration_ms = store.clock().steady_ms() - t0;
        return out;
    }
    out.restored = true;

    // A restore is only proven by replaying what came back. Section 17.2 is
    // explicit that a successful backup job is not evidence.
    SqliteAtomicStore::Options sopt;
    sopt.path = scratch_db_path;
    sopt.create_if_missing = false;
    sopt.clock = &store.clock();
    std::unique_ptr<SqliteAtomicStore> restored;
    if (SqliteAtomicStore::open(sopt, &restored, &why) != StoreError::None) {
        out.detail = "cannot open the restored database: " + why;
        out.duration_ms = store.clock().steady_ms() - t0;
        return out;
    }

    for (const StreamId& s : restored->list_streams(tenant)) {
        const ReplayResult r = replay_from_empty(*restored, tenant, s);
        out.events_restored += r.events_applied;
        if (!r.ok) {
            out.incidents.insert(out.incidents.end(), r.incidents.begin(), r.incidents.end());
            out.detail = "replay of restored stream " + s + " failed: " + r.detail;
            out.duration_ms = store.clock().steady_ms() - t0;
            return out;
        }
    }
    out.verified = true;
    // Stays false by construction. JI-31: a restored instance does not
    // resume outbound traffic until the counterparty has been reconciled.
    out.dispatch_enabled = false;
    out.duration_ms = store.clock().steady_ms() - t0;
    return out;
}

}  // namespace jrnl
