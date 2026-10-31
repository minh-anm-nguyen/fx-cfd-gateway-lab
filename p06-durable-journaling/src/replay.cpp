#include "jrnl/replay.hpp"

#include <algorithm>

#include "jrnl/failpoint.hpp"
#include "jrnl/snapshot.hpp"

namespace jrnl {
namespace {

// Mirrors the two helpers in sqlite_store.cpp. They are duplicated rather
// than shared because replay must be able to recompute the hashes from the
// stored row alone: if it reused a function that took the live event object
// it would be checking the store against itself.
Digest event_chain_hash(const Digest& prev, const Digest& canonical_hash,
                        const Digest& state_dig, const StreamId& stream, Version version,
                        const std::string& reducer_version) {
    const std::string bind = to_hex(canonical_hash) + to_hex(state_dig);
    return chain_step(prev, bind, stream, version, reducer_version);
}

CanonValue envelope_from_row(const TenantId& tenant, const JournalRecord& r,
                             const CanonValue& payload) {
    CanonMap m;
    m["tenant"] = CanonValue::str(tenant);
    m["stream"] = CanonValue::str(r.stream);
    m["version"] = CanonValue::integer(static_cast<std::int64_t>(r.version));
    m["event_id"] = CanonValue::str(r.event_id);
    m["event_type"] = CanonValue::str(r.event_type);
    m["schema_version"] = CanonValue::integer(r.schema_version);
    m["reducer_version"] = CanonValue::str(r.reducer_version);
    m["policy_version"] = CanonValue::integer(static_cast<std::int64_t>(r.policy_version));
    m["payload"] = payload;
    return CanonValue::map(std::move(m));
}

}  // namespace

bool ReplayResult::has(Incident i) const {
    return std::find(incidents.begin(), incidents.end(), i) != incidents.end();
}

bool VerifyReport::has(Incident i) const {
    return std::find(incidents.begin(), incidents.end(), i) != incidents.end();
}

void VerifyReport::fail(Incident i, std::string detail, const StreamId& stream) {
    incidents.push_back(i);
    details.push_back(stream.empty() ? detail : stream + ": " + detail);
    if (is_risk_stream(stream)) frozen_domains.insert(stream_suffix(stream));
}

ReplayResult replay_stream(const SqliteAtomicStore& store, const TenantId& tenant,
                           const StreamId& stream, const SnapshotRecord* from) {
    ReplayResult out;
    out.stream = stream;

    const bool is_order = is_order_stream(stream);
    const bool is_risk = is_risk_stream(stream);
    if (!is_order && !is_risk) {
        out.incidents.push_back(Incident::SchemaIncompatible);
        out.detail = "unknown stream kind";
        return out;
    }

    const StreamHead head = store.head(tenant, stream);
    const ReducerRegistry& reg = store.reducers();

    OrderState ostate;
    RiskDomainState rstate;
    Digest running = zero_digest();
    Version expected = 1;
    std::string pinned_reducer;

    if (from != nullptr) {
        if (!from->found || !from->verified) {
            out.incidents.push_back(Incident::SnapshotUnverified);
            out.detail = "snapshot is not marked verified";
            return out;
        }
        if (from->version > head.version) {
            // JI-13: a snapshot cannot be ahead of the committed head, and a
            // snapshot whose tail is missing is not usable as a substitute.
            out.incidents.push_back(Incident::SnapshotAheadOfJournal);
            out.detail = "snapshot version " + std::to_string(from->version) +
                         " exceeds stream head " + std::to_string(head.version);
            return out;
        }
        if (!(sha256(from->serialized_state) == from->content_hash)) {
            out.incidents.push_back(Incident::SnapshotChecksumFailure);
            out.detail = "snapshot content hash mismatch";
            return out;
        }
        CanonValue sv;
        if (!canonical_decode(from->serialized_state, &sv, nullptr)) {
            out.incidents.push_back(Incident::SnapshotChecksumFailure);
            out.detail = "snapshot state is not canonical";
            return out;
        }
        if (is_order) {
            const IOrderReducer* r = reg.order(from->reducer_version);
            if (r == nullptr || !r->from_canon(sv, &ostate)) {
                out.incidents.push_back(Incident::ReducerUnavailable);
                out.detail = "cannot decode snapshot with reducer " + from->reducer_version;
                return out;
            }
        } else {
            const IRiskReducer* r = reg.risk(from->reducer_version);
            if (r == nullptr || !r->from_canon(sv, &rstate)) {
                out.incidents.push_back(Incident::ReducerUnavailable);
                out.detail = "cannot decode snapshot with reducer " + from->reducer_version;
                return out;
            }
        }
        running = from->state_hash;
        expected = from->version + 1;
        out.started_from = from->version;
        pinned_reducer = from->reducer_version;
    }

    std::vector<JournalRecord> page;
    const StoreError e = store.read_stream(tenant, stream, expected, 0, &page);
    if (e != StoreError::None) {
        out.incidents.push_back(e == StoreError::Corrupt ? Incident::HashChainBroken
                                                          : Incident::SchemaIncompatible);
        out.detail = "cannot read stream: " + std::string(to_string(e));
        return out;
    }

    for (const JournalRecord& r : page) {
        JRNL_FP(fp::point::kReplayMidStream);

        if (r.version != expected) {
            // Section 12: a gap or a duplicate is a quarantine, never a skip.
            out.incidents.push_back(r.version > expected ? Incident::StreamVersionGap
                                                          : Incident::StreamVersionDuplicate);
            out.detail = "expected version " + std::to_string(expected) + " but read " +
                         std::to_string(r.version);
            return out;
        }
        if (!(r.prev_state_hash == running)) {
            out.incidents.push_back(Incident::HashChainBroken);
            out.detail = "prev_state_hash at version " + std::to_string(r.version) +
                         " does not match the running chain";
            return out;
        }
        if (!pinned_reducer.empty() && r.reducer_version != pinned_reducer) {
            out.incidents.push_back(Incident::SchemaIncompatible);
            out.detail = "reducer version changes mid-stream at version " +
                         std::to_string(r.version);
            return out;
        }
        pinned_reducer = r.reducer_version;

        CanonValue payload;
        std::string why;
        if (!canonical_decode(r.payload_text, &payload, &why)) {
            out.incidents.push_back(Incident::HashChainBroken);
            out.detail = "payload at version " + std::to_string(r.version) +
                         " is not canonical: " + why;
            return out;
        }

        // Recompute the event hash from the stored columns. This is what
        // catches a hand-edited event_type or policy_version that left the
        // payload alone.
        const Digest recomputed =
            sha256(canonical_encode(envelope_from_row(tenant, r, payload)));
        if (!(recomputed == r.canonical_hash)) {
            out.incidents.push_back(Incident::HashChainBroken);
            out.detail = "canonical hash mismatch at version " + std::to_string(r.version);
            return out;
        }

        ReduceError rerr;
        CanonValue new_canon;
        if (is_order) {
            // JI-37: the reducer named on the event, not the newest one.
            const IOrderReducer* red = reg.order(r.reducer_version);
            if (red == nullptr) {
                out.incidents.push_back(Incident::ReducerUnavailable);
                out.detail = "no reducer registered for " + r.reducer_version;
                return out;
            }
            if (!red->apply(&ostate, r.event_type, payload, &rerr)) {
                out.incidents.push_back(rerr.incident);
                out.detail = "reducer refused a committed event at version " +
                             std::to_string(r.version) + ": " + rerr.detail;
                return out;
            }
            new_canon = red->to_canon(ostate);
        } else {
            const IRiskReducer* red = reg.risk(r.reducer_version);
            if (red == nullptr) {
                out.incidents.push_back(Incident::ReducerUnavailable);
                out.detail = "no reducer registered for " + r.reducer_version;
                return out;
            }
            if (!red->apply(&rstate, r.event_type, payload, &rerr)) {
                out.incidents.push_back(rerr.incident);
                out.detail = "reducer refused a committed event at version " +
                             std::to_string(r.version) + ": " + rerr.detail;
                return out;
            }
            new_canon = red->to_canon(rstate);
        }

        const Digest chain = event_chain_hash(running, r.canonical_hash, state_digest(new_canon),
                                              stream, r.version, r.reducer_version);
        if (!(chain == r.state_hash)) {
            // Either the stored state hash was altered, or the reducer no
            // longer produces what it produced when the event was written.
            // Both are incidents; neither is repaired here.
            out.incidents.push_back(Incident::StateHashMismatch);
            out.detail = "state hash mismatch at version " + std::to_string(r.version);
            return out;
        }

        running = chain;
        out.state = new_canon;
        ++expected;
        ++out.events_applied;
    }

    out.version = expected - 1;
    out.state_hash = running;
    if (out.events_applied == 0 && from == nullptr) {
        // An empty stream replays to the empty state, which is a legitimate
        // result and not an error.
        out.state = is_order ? reg.order(reg.current_order_version())->to_canon(OrderState{})
                             : reg.risk(reg.current_risk_version())->to_canon(RiskDomainState{});
    }

    if (head.exists) {
        if (out.version != head.version) {
            out.incidents.push_back(Incident::HeadMismatch);
            out.detail = "replay reached version " + std::to_string(out.version) +
                         " but the head says " + std::to_string(head.version);
            return out;
        }
        if (!(out.state_hash == head.state_hash)) {
            out.incidents.push_back(Incident::HeadMismatch);
            out.detail = "replay chain value does not match the stored head";
            return out;
        }
    } else if (out.events_applied > 0) {
        out.incidents.push_back(Incident::HeadMismatch);
        out.detail = "stream has events but no head row";
        return out;
    }

    out.ok = true;
    return out;
}

ReplayResult replay_from_empty(const SqliteAtomicStore& store, const TenantId& tenant,
                               const StreamId& stream) {
    return replay_stream(store, tenant, stream, nullptr);
}

VerifyReport verify_stream(const SqliteAtomicStore& store, const TenantId& tenant,
                           const StreamId& stream) {
    VerifyReport rep;
    rep.streams_checked = 1;
    const ReplayResult r = replay_from_empty(store, tenant, stream);
    rep.events_checked = r.events_applied;
    if (!r.ok) {
        rep.journal_valid = false;
        for (Incident i : r.incidents) rep.fail(i, r.detail, stream);
        return rep;
    }

    // Projection equivalence: the canonical state the journal produces must
    // be byte-identical to what the projection row holds. This is integrity
    // level 3 and the reason JI-12 can be enforced rather than hoped for.
    if (is_order_stream(stream)) {
        OrderState projected;
        if (!store.load_order(tenant, stream_suffix(stream), &projected)) {
            rep.projections_valid = false;
            rep.fail(Incident::ProjectionReplayMismatch, "order projection missing", stream);
            return rep;
        }
        const IOrderReducer* red =
            store.reducers().order(store.head(tenant, stream).reducer_version);
        if (red == nullptr) {
            rep.projections_valid = false;
            rep.fail(Incident::ReducerUnavailable, "projection reducer unavailable", stream);
            return rep;
        }
        if (canonical_encode(red->to_canon(projected)) != canonical_encode(r.state)) {
            rep.projections_valid = false;
            rep.fail(Incident::ProjectionReplayMismatch,
                     "order projection differs from replayed state", stream);
        }
    } else if (is_risk_stream(stream)) {
        RiskDomainState projected;
        if (!store.load_risk(tenant, stream_suffix(stream), &projected)) {
            rep.projections_valid = false;
            rep.fail(Incident::ProjectionReplayMismatch, "risk projection missing", stream);
            return rep;
        }
        const IRiskReducer* red =
            store.reducers().risk(store.reducers().current_risk_version());
        if (canonical_encode(red->to_canon(projected)) != canonical_encode(r.state)) {
            rep.projections_valid = false;
            rep.fail(Incident::RiskLedgerDiff, "risk projection differs from replayed state",
                     stream);
        }
    }
    return rep;
}

VerifyReport verify_all(const SqliteAtomicStore& store, const TenantId& tenant) {
    VerifyReport rep;
    Db& db = const_cast<Db&>(store.db());

    // Level 0: the file itself.
    {
        std::string why;
        if (db.integrity_check(&why) != StoreError::None) {
            rep.storage_valid = false;
            rep.fail(Incident::HashChainBroken, "database integrity check failed: " + why);
            return rep;
        }
        const Db::DurabilityProfile p = db.durability();
        if (!p.safe) {
            // JI-30. Running on a weaker profile does not corrupt anything,
            // but it makes ACCEPTED_DURABLE a claim the engine cannot back,
            // so the verifier reports it rather than letting it pass.
            rep.storage_valid = false;
            rep.fail(Incident::DurabilityProfileUnsafe,
                     "durability profile is journal_mode=" + p.journal_mode +
                         " synchronous=" + p.synchronous);
        }
    }

    // Levels 2 and 3: per-stream chain and projection equivalence.
    for (const StreamId& s : store.list_streams(tenant)) {
        const VerifyReport one = verify_stream(store, tenant, s);
        rep.streams_checked += one.streams_checked;
        rep.events_checked += one.events_checked;
        if (!one.journal_valid) rep.journal_valid = false;
        if (!one.projections_valid) rep.projections_valid = false;
        for (std::size_t i = 0; i < one.incidents.size(); ++i) {
            rep.incidents.push_back(one.incidents[i]);
            rep.details.push_back(one.details[i]);
        }
        for (const RiskDomainId& d : one.frozen_domains) rep.frozen_domains.insert(d);
        // An order stream that fails freezes the risk domain it draws on,
        // because that is the scope where admission must stop (JI-39).
        if ((!one.journal_valid || !one.projections_valid) && is_order_stream(s)) {
            OrderState o;
            if (store.load_order(tenant, stream_suffix(s), &o) && !o.risk_domain.empty()) {
                rep.frozen_domains.insert(o.risk_domain);
            }
        }
    }

    // Level 4: cross-stream transaction manifests.
    {
        Stmt s;
        if (db.prepare("SELECT tx_id,expected_event_count,members_digest FROM "
                       "journal_transactions WHERE tenant_id=?1;",
                       &s) == StoreError::None) {
            s.bind_text(1, tenant);
            while (true) {
                bool row = false;
                if (s.step(&row) != StoreError::None || !row) break;
                const std::string tx = s.col_text(0);
                const std::int64_t expected = s.col_i64(1);
                Digest declared{};
                if (!s.col_hex(2, &declared)) {
                    rep.manifests_valid = false;
                    rep.fail(Incident::TxManifestMismatch, "manifest " + tx + " has a bad digest");
                    continue;
                }
                Stmt m;
                if (db.prepare("SELECT payload FROM journal_events WHERE tenant_id=?1 AND "
                               "tx_id=?2 ORDER BY event_ordinal;",
                               &m) != StoreError::None) {
                    continue;
                }
                m.bind_text(1, tenant);
                m.bind_text(2, tx);
                std::vector<Digest> members;
                while (true) {
                    bool mrow = false;
                    if (m.step(&mrow) != StoreError::None || !mrow) break;
                    members.push_back(sha256(m.col_text(0)));
                }
                if (static_cast<std::int64_t>(members.size()) != expected) {
                    rep.manifests_valid = false;
                    rep.fail(Incident::TxManifestMismatch,
                             "manifest " + tx + " declares " + std::to_string(expected) +
                                 " members but " + std::to_string(members.size()) +
                                 " are present");
                    continue;
                }
                if (!(members_digest(members) == declared)) {
                    rep.manifests_valid = false;
                    rep.fail(Incident::TxManifestMismatch,
                             "manifest " + tx + " digest does not match its members");
                }
            }
        }
    }

    // Dedup and intent orphans. The foreign keys already prevent most of
    // these; the verifier checks anyway, because section 26.1 is explicit
    // that an existence check in SQL is not by itself a release gate, and a
    // constraint dropped by a bad migration would otherwise be silent.
    {
        Stmt s;
        if (db.prepare("SELECT o.outbox_id FROM command_outbox o LEFT JOIN command_intents c "
                       "ON c.tenant_id=o.tenant_id AND c.account_id=o.account_id AND "
                       "c.command_id=o.command_id WHERE o.tenant_id=?1 AND c.command_id IS NULL;",
                       &s) == StoreError::None) {
            s.bind_text(1, tenant);
            while (true) {
                bool row = false;
                if (s.step(&row) != StoreError::None || !row) break;
                rep.dedup_valid = false;
                rep.fail(Incident::OrphanOutbox,
                         "outbox row " + s.col_text(0) + " has no command intent");
            }
        }
        Stmt f;
        if (db.prepare("SELECT f.semantic_exec_id FROM fills f LEFT JOIN business_exec_keys k "
                       "ON k.tenant_id=f.tenant_id AND k.lp_namespace=f.lp_namespace AND "
                       "k.semantic_exec_id=f.semantic_exec_id WHERE f.tenant_id=?1 AND "
                       "k.semantic_exec_id IS NULL;",
                       &f) == StoreError::None) {
            f.bind_text(1, tenant);
            while (true) {
                bool row = false;
                if (f.step(&row) != StoreError::None || !row) break;
                rep.dedup_valid = false;
                rep.fail(Incident::OrphanDedupKey,
                         "fill " + f.col_text(0) + " has no execution dedup key");
            }
        }
    }

    // Risk ledger conservation: the sum of held reservations must equal the
    // domain's reserved figure. A projection that is internally consistent
    // but arithmetically wrong is still a release blocker (section 16).
    {
        Stmt s;
        if (db.prepare(
                "SELECT d.domain_id, d.reserved, COALESCE(SUM(CASE WHEN r.state='Held' THEN "
                "r.amount ELSE 0 END),0) FROM risk_domains d LEFT JOIN risk_reservations r "
                "ON r.tenant_id=d.tenant_id AND r.domain_id=d.domain_id WHERE d.tenant_id=?1 "
                "GROUP BY d.domain_id, d.reserved;",
                &s) == StoreError::None) {
            s.bind_text(1, tenant);
            while (true) {
                bool row = false;
                if (s.step(&row) != StoreError::None || !row) break;
                const std::string domain = s.col_text(0);
                const std::int64_t reserved = s.col_i64(1);
                const std::int64_t held_sum = s.col_i64(2);
                if (reserved != held_sum) {
                    rep.projections_valid = false;
                    rep.fail(Incident::RiskLedgerDiff,
                             "domain " + domain + " reports reserved " + std::to_string(reserved) +
                                 " but holds " + std::to_string(held_sum));
                    rep.frozen_domains.insert(domain);
                }
            }
        }
    }

    // Level 5 in part: the audit chain inside this database. The offsite
    // immutable anchor that section 18 asks for is not implemented here and
    // is reported as a gap rather than assumed.
    {
        std::string why;
        if (!store.audit_chain_intact(tenant, &why)) {
            rep.audit_valid = false;
            rep.fail(Incident::HashChainBroken, "audit chain: " + why);
        }
    }

    return rep;
}

}  // namespace jrnl
