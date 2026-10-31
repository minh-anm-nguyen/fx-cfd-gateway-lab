// In-memory ISessionStore for tests that need a seeded state or an injected
// store failure without going through SQLite.
//
// It keeps the same preconditions as the real store, because those
// preconditions are part of the contract the engine relies on: sequences only
// move forward, and a record must line up with the sequence being committed.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fix/audit.hpp"
#include "fix/config.hpp"
#include "fix/metrics.hpp"
#include "fix/session_actor.hpp"
#include "fix/store.hpp"
#include "engine_fixture.hpp"

namespace tt {

class FakeStore final : public fixse::ISessionStore {
public:
    fixse::PersistedState state;
    std::map<uint64_t, fixse::OutboundRecord> outbound;
    std::map<uint64_t, fixse::InboundRecord> inbox;
    std::vector<fixse::AuditRecord> audits;
    bool clean_shutdown = true;
    bool owns = true;

    // Injected failures. A non-None value fails the matching call, either
    // always or for the next n calls.
    fixse::StoreError fail_outbound = fixse::StoreError::None;
    int fail_outbound_times = -1;
    fixse::StoreError fail_inbound = fixse::StoreError::None;
    int fail_inbound_times = -1;
    fixse::StoreError fail_load_range = fixse::StoreError::None;
    bool report_inconsistent = false;
    std::string inconsistent_reason = "seeded inconsistency";

    int outbound_commits = 0;
    int inbound_commits = 0;
    int delivered_marks = 0;

    fixse::StoreError load_state(fixse::PersistedState* out) override {
        *out = state;
        return fixse::StoreError::None;
    }

    fixse::StoreError commit_outbound(const fixse::OutboundRecord& rec,
                                      uint64_t next_out) override {
        if (fail_outbound != fixse::StoreError::None &&
            (fail_outbound_times < 0 || fail_outbound_times-- > 0))
            return fail_outbound;
        if (next_out <= state.next_out || rec.seq != state.next_out)
            return fixse::StoreError::Inconsistent;
        outbound[rec.seq] = rec;
        state.next_out = next_out;
        ++state.version;
        ++outbound_commits;
        return fixse::StoreError::None;
    }

    fixse::StoreError commit_inbound(const fixse::InboundRecord* rec, uint64_t next_in) override {
        if (fail_inbound != fixse::StoreError::None &&
            (fail_inbound_times < 0 || fail_inbound_times-- > 0))
            return fail_inbound;
        if (next_in <= state.next_in) return fixse::StoreError::Inconsistent;
        if (rec != nullptr && rec->seq != state.next_in) return fixse::StoreError::Inconsistent;
        if (rec != nullptr) {
            if (inbox.count(rec->seq) != 0) return fixse::StoreError::Conflict;
            inbox[rec->seq] = *rec;
        }
        state.next_in = next_in;
        ++state.version;
        ++inbound_commits;
        return fixse::StoreError::None;
    }

    fixse::StoreError mark_delivered(uint64_t seq) override {
        auto it = inbox.find(seq);
        if (it != inbox.end()) it->second.delivery_state = fixse::DeliveryState::Delivered;
        ++delivered_marks;
        return fixse::StoreError::None;
    }

    fixse::StoreError load_outbound_range(uint64_t from_seq, uint64_t to_seq,
                                          std::vector<fixse::OutboundRecord>* out) override {
        out->clear();
        if (fail_load_range != fixse::StoreError::None) return fail_load_range;
        if (from_seq == 0 || to_seq < from_seq) return fixse::StoreError::Inconsistent;
        for (uint64_t s = from_seq; s <= to_seq; ++s) {
            auto it = outbound.find(s);
            if (it != outbound.end()) out->push_back(it->second);
        }
        return fixse::StoreError::None;
    }

    fixse::StoreError load_pending_inbox(size_t max_rows,
                                         std::vector<fixse::InboundRecord>* out) override {
        out->clear();
        for (const auto& kv : inbox) {
            if (kv.second.delivery_state != fixse::DeliveryState::Pending) continue;
            out->push_back(kv.second);
            if (out->size() >= max_rows) break;
        }
        return fixse::StoreError::None;
    }

    fixse::StoreError load_inbox_record(uint64_t seq, fixse::InboundRecord* out,
                                        bool* found) override {
        auto it = inbox.find(seq);
        *found = it != inbox.end();
        if (*found) *out = it->second;
        return fixse::StoreError::None;
    }

    fixse::StoreError append_audit(const fixse::AuditRecord& rec) override {
        audits.push_back(rec);
        return fixse::StoreError::None;
    }

    fixse::StoreError record_connection_generation(uint64_t generation) override {
        state.last_good_connection_generation = generation;
        return fixse::StoreError::None;
    }

    fixse::StoreError coordinated_reset(const std::string& a, const std::string& b,
                                        const std::string&) override {
        if (a.empty() || b.empty() || a == b) return fixse::StoreError::NotOwner;
        ++state.epoch;
        state.next_in = 1;
        state.next_out = 1;
        ++state.version;
        return fixse::StoreError::None;
    }

    fixse::ConsistencyReport verify_consistency() override {
        fixse::ConsistencyReport r;
        r.clean_shutdown = clean_shutdown;
        r.integrity_checksum_valid = true;
        r.ok = !report_inconsistent;
        if (report_inconsistent) r.problem = inconsistent_reason;
        for (const auto& kv : inbox)
            if (kv.second.delivery_state == fixse::DeliveryState::Pending) ++r.pending_inbox_rows;
        return r;
    }

    bool owns_session() const override { return owns; }
    void mark_clean_shutdown() override { clean_shutdown = true; }
    size_t audit_count() const override { return audits.size(); }

    int audit_count_of(fixse::AuditEvent e) const {
        int n = 0;
        for (const fixse::AuditRecord& r : audits)
            if (r.event == e) ++n;
        return n;
    }
};

}  // namespace tt

namespace tt {

// An engine wired to the in-memory store. Used where a scenario needs a seeded
// sequence state, or needs the store itself to fail in a specific way.
class MiniEngine {
public:
    explicit MiniEngine(uint64_t next_in = 1, uint64_t next_out = 1, uint64_t epoch = 1) {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "MINI";
        cfg.heartbeat_seconds = 30;
        cfg.max_future_messages = 8;
        cfg.max_future_bytes = 64 * 1024;
        cfg.max_inbox_pending = 4;
        cfg.max_resend_cycles = 4;
        cfg.max_recovery_duration_ms = 40000;
        store.state.next_in = next_in;
        store.state.next_out = next_out;
        store.state.epoch = epoch;

        fixse::EngineDeps d;
        d.cfg = &cfg;
        d.store = &store;
        d.clock = &clock;
        d.transport = &transport;
        d.controller = &controller;
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<fixse::SessionEngine>(d);
        engine->start();
    }

    void connect() {
        transport.accept_generation = generation;
        engine->on_connected(generation);
    }
    void connect_and_logon() {
        connect();
        feed(logon_reply(store.state.next_in));
    }
    void feed(const std::string& frame) { engine->on_bytes(frame, generation); }
    void advance(long long ms) {
        clock.advance(fixse::Millis(ms));
        engine->on_timer(clock.steady_now());
    }
    void tick() { engine->on_timer(clock.steady_now()); }

    peer::Msg msg(std::string type, uint64_t seq) const {
        peer::Msg m;
        m.msg_type = std::move(type);
        m.seq = seq;
        m.sender = cfg.target_comp_id;
        m.target = cfg.sender_comp_id;
        m.sending_time = fixse::format_utc_timestamp(clock.wall_now());
        return m;
    }
    std::string logon_reply(uint64_t seq) const {
        peer::Msg m = msg("A", seq);
        m.extra.push_back({98, "0"});
        m.extra.push_back({108, std::to_string(cfg.heartbeat_seconds)});
        return m.build();
    }
    std::string app(uint64_t seq, const std::string& payload = "payload") const {
        peer::Msg m = msg("U1", seq);
        m.extra.push_back({20001, payload});
        return m.build();
    }
    std::vector<std::string> out() { return transport.new_frames(); }
    std::optional<std::string> out_of_type(const std::string& type) {
        for (const std::string& f : transport.new_frames())
            if (peer::get(f, 35).value_or("") == type) return f;
        return std::nullopt;
    }

    fixse::VirtualClock clock;
    fixse::SessionConfig cfg;
    FakeStore store;
    RecordingTransport transport;
    FakeController controller;
    RecordingSink sink;
    fixse::Metrics metrics;
    fixse::AuditLog audit;
    std::unique_ptr<fixse::SessionEngine> engine;
    uint64_t generation = 1;
};

}  // namespace tt
