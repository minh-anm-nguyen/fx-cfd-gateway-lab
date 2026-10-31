// Deterministic driver for the session engine.
//
// The engine performs no I/O of its own, so a whole scenario is a sequence of
// on_connected / on_bytes / on_timer calls against fakes. Time is virtual, so a
// thirty second heartbeat is one function call rather than a thirty second wait
// (design doc 17.1).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fix/audit.hpp"
#include "fix/clock.hpp"
#include "fix/config.hpp"
#include "fix/metrics.hpp"
#include "fix/ports.hpp"
#include "fix/session_actor.hpp"
#include "fix/sqlite_store.hpp"
#include "peer_wire.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

namespace tt {

// Transport that records what the engine wrote and can be made to accept only
// part of a write, or to fail outright.
class RecordingTransport final : public fixse::ITransport {
public:
    long write(uint64_t generation, std::string_view bytes) override {
        ++write_calls;
        if (generation != accept_generation) return -1;
        if (fail_next) {
            fail_next = false;
            return -1;
        }
        if (would_block) return 0;
        const size_t n = std::min(bytes.size(), max_bytes_per_write);
        written.append(bytes.substr(0, n));
        return static_cast<long>(n);
    }

    std::vector<std::string> frames() const { return peer::split_frames(written); }
    // Frames written since the last call, so a step can assert only its own
    // output.
    std::vector<std::string> new_frames() {
        auto all = frames();
        std::vector<std::string> fresh;
        for (size_t i = consumed; i < all.size(); ++i) fresh.push_back(all[i]);
        consumed = all.size();
        return fresh;
    }
    void forget() { consumed = frames().size(); }

    uint64_t accept_generation = 1;
    size_t max_bytes_per_write = static_cast<size_t>(-1);
    bool would_block = false;
    bool fail_next = false;
    std::string written;
    size_t consumed = 0;
    int write_calls = 0;
};

class FakeController final : public fixse::IConnectionController {
public:
    void request_connect() override { ++connect_requests; }
    void request_disconnect(uint64_t generation, std::string_view reason) override {
        disconnects.push_back({generation, std::string(reason)});
    }
    struct Disconnect {
        uint64_t generation;
        std::string reason;
    };
    int connect_requests = 0;
    std::vector<Disconnect> disconnects;
};

class RecordingSink final : public fixse::IApplicationSink {
public:
    bool deliver(uint64_t seq, std::string_view msg_type, std::string_view payload) override {
        ++attempts;
        if (refuse) {
            refused.push_back(seq);
            return false;
        }
        delivered.push_back(seq);
        types.push_back(std::string(msg_type));
        payloads.push_back(std::string(payload));
        return true;
    }
    std::vector<uint64_t> delivered;
    std::vector<uint64_t> refused;
    std::vector<std::string> types;
    std::vector<std::string> payloads;
    int attempts = 0;
    bool refuse = false;
};


// A started engine with a durable store, wired to the fakes above.
class EngineFixture {
public:
    explicit EngineFixture(const std::string& label, bool auto_start = true) : dir_(label) {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "TEST";
        cfg.heartbeat_seconds = 30;
        cfg.database_path = dir_.file("session.db");
        cfg.max_future_messages = 8;
        cfg.max_future_bytes = 64 * 1024;
        cfg.max_inbox_pending = 16;
        cfg.max_resend_cycles = 4;
        cfg.max_recovery_duration_ms = 40000;
        if (auto_start) start();
    }

    void start() {
        fixse::SqliteSessionStore::Options o;
        o.path = cfg.database_path;
        o.key.sender_comp_id = cfg.sender_comp_id;
        o.key.target_comp_id = cfg.target_comp_id;
        o.key.qualifier = cfg.qualifier;
        o.create_if_missing = true;
        o.clock = &clock;
        std::string why;
        const fixse::StoreError e = fixse::SqliteSessionStore::open(o, &store, &why);
        if (e != fixse::StoreError::None)
            throw AssertionFailure("store open failed: " + why);

        fixse::EngineDeps d;
        d.cfg = &cfg;
        d.store = store.get();
        d.clock = &clock;
        d.transport = &transport;
        d.controller = &controller;
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<fixse::SessionEngine>(d);
        engine->start();
    }

    // Close the engine and store without a clean shutdown marker, then reopen:
    // the shape of a process restart after a crash.
    void restart(bool clean = true) {
        if (clean && store != nullptr) store->mark_clean_shutdown();
        engine.reset();
        store.reset();
        transport.written.clear();
        transport.consumed = 0;
        controller.connect_requests = 0;
        controller.disconnects.clear();
        start();
    }

    // Bring the session to ACTIVE: connect, then answer the Logon.
    void connect_and_logon(uint64_t peer_logon_seq = 1) {
        connect();
        feed(logon_reply(peer_logon_seq));
    }

    void connect() {
        // The transport must already accept this generation before the engine
        // writes its Logon, exactly as a real connector would.
        transport.accept_generation = generation;
        engine->on_connected(generation);
    }

    // Simulate a reconnect with a fresh generation.
    void reconnect() {
        ++generation;
        connect();
    }

    void feed(const std::string& frame) { engine->on_bytes(frame, generation); }
    void feed_bytes(std::string_view bytes) { engine->on_bytes(bytes, generation); }

    void advance(long long ms) {
        clock.advance(fixse::Millis(ms));
        engine->on_timer(clock.steady_now());
    }
    void tick() { engine->on_timer(clock.steady_now()); }

    // A peer message with the comp ids and timestamp already correct.
    peer::Msg msg(std::string type, uint64_t seq) const {
        peer::Msg m;
        m.msg_type = std::move(type);
        m.seq = seq;
        m.sender = cfg.target_comp_id;
        m.target = cfg.sender_comp_id;
        m.sending_time = fixse::format_utc_timestamp(clock.wall_now());
        return m;
    }

    std::string logon_reply(uint64_t seq, std::optional<std::string> reset_flag = std::nullopt,
                            std::optional<std::string> heartbeat = std::nullopt) const {
        peer::Msg m = msg("A", seq);
        m.extra.push_back({98, "0"});
        m.extra.push_back({108, heartbeat.value_or(std::to_string(cfg.heartbeat_seconds))});
        if (reset_flag.has_value()) m.extra.push_back({141, *reset_flag});
        return m.build();
    }

    std::string app(uint64_t seq, const std::string& payload = "payload") const {
        peer::Msg m = msg("U1", seq);
        m.extra.push_back({20001, payload});
        return m.build();
    }

    // The engine output frames since the previous inspection.
    std::vector<std::string> out() { return transport.new_frames(); }

    // First output frame of a given MsgType since the previous inspection.
    std::optional<std::string> out_of_type(const std::string& type) {
        for (const std::string& f : transport.new_frames())
            if (peer::get(f, 35).value_or("") == type) return f;
        return std::nullopt;
    }

    std::string db_path() const { return cfg.database_path; }

    fixse::VirtualClock clock;
    fixse::SessionConfig cfg;
    std::unique_ptr<fixse::SqliteSessionStore> store;
    RecordingTransport transport;
    FakeController controller;
    RecordingSink sink;
    fixse::Metrics metrics;
    fixse::AuditLog audit;
    std::unique_ptr<fixse::SessionEngine> engine;
    uint64_t generation = 1;

private:
    TempDir dir_;
};

}  // namespace tt
