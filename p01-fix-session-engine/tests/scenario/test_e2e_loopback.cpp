// End to end over a real TCP connection on the loopback interface.
//
// This is the demonstration release gate E asks for: connect, Logon, heartbeat,
// a peer TestRequest answered with the matching TestReqID, then a graceful
// Logout, with the durable store inspected afterwards. Everything here is the
// production path: RealSocketOps, TcpConnector, SqliteSessionStore and the real
// system clock.
//
// The peer is built from tests/support/peer_wire.hpp, a wire implementation
// written independently of src/codec.cpp. That is cross-implementation
// evidence, not certification: the design doc is explicit that a peer you wrote
// yourself cannot prove your own compliance (17.3 layer 6).
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "fix/session_actor.hpp"
#include "fix/sqlite_store.hpp"
#include "fix/tcp_connector.hpp"
#include "loopback_peer.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace fixse;

namespace {

class CountingSink final : public IApplicationSink {
public:
    bool deliver(uint64_t seq, std::string_view, std::string_view) override {
        delivered.push_back(seq);
        return true;
    }
    std::vector<uint64_t> delivered;
};

struct E2E {
    tt::TempDir dir;
    SystemClock clock;
    SessionConfig cfg;
    std::unique_ptr<SqliteSessionStore> store;
    RealSocketOps ops;
    std::unique_ptr<TcpConnector> connector;
    CountingSink sink;
    Metrics metrics;
    AuditLog audit;
    std::unique_ptr<SessionEngine> engine;

    E2E(const std::string& label, uint16_t port, const std::string& reuse_db = std::string())
        : dir(label) {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "E2E";
        cfg.heartbeat_seconds = 1;  // keeps the demonstration quick
        cfg.test_request_timeout_ms = 2000;
        cfg.host = "127.0.0.1";
        cfg.port = port;
        cfg.connect_timeout_ms = 2000;
        cfg.database_path = reuse_db.empty() ? dir.file("session.db") : reuse_db;

        SqliteSessionStore::Options o;
        o.path = cfg.database_path;
        o.key.sender_comp_id = cfg.sender_comp_id;
        o.key.target_comp_id = cfg.target_comp_id;
        o.key.qualifier = cfg.qualifier;
        o.create_if_missing = reuse_db.empty();
        o.clock = &clock;
        std::string why;
        if (SqliteSessionStore::open(o, &store, &why) != StoreError::None)
            throw tt::AssertionFailure("store open failed: " + why);

        connector = std::make_unique<TcpConnector>(cfg, clock, ops);
        EngineDeps d;
        d.cfg = &cfg;
        d.store = store.get();
        d.clock = &clock;
        d.transport = connector.get();
        d.controller = connector.get();
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<SessionEngine>(d);
        connector->bind_engine(engine.get());
        if (engine->start() != StoreError::None)
            throw tt::AssertionFailure("engine start failed");
    }

    // Drive the connector until the predicate holds or the deadline passes.
    template <typename Predicate>
    bool pump_until(Predicate done, int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            connector->poll_once(20);
            if (done()) return true;
        }
        return done();
    }
};

int count_type(const std::vector<std::string>& frames, const std::string& type) {
    int n = 0;
    for (const std::string& f : frames)
        if (peer::get(f, 35).value_or("") == type) ++n;
    return n;
}

}  // namespace

SCENARIO("E2E-01", "P0", "a real TCP session logs on, heartbeats, answers a probe and logs out") {
    tt::LoopbackPeer peer;
    peer.heartbeat_seconds = 1;
    uint16_t port = 0;
    CHECK(peer.start(&port));
    CHECK(port != 0);

    E2E e("e2e01", port);

    // 1. Connect and complete the handshake over a real socket.
    CHECK(e.pump_until([&] { return e.engine->state() == SessionState::Active; }, 5000));
    CHECK_EQ(e.engine->state(), SessionState::Active);
    CHECK(e.engine->ready());

    const auto received = peer.received();
    CHECK(!received.empty());
    CHECK_EQ(count_type(received, "A"), 1);
    std::string why;
    CHECK_MSG(peer::verify_frame(received[0], &why), why);
    CHECK_EQ(peer::get(received[0], 34).value_or(""), std::string("1"));
    CHECK_EQ(peer::get(received[0], 108).value_or(""), std::string("1"));

    // 2. An idle second produces a heartbeat.
    CHECK(e.pump_until([&] { return count_type(peer.received(), "0") >= 1; }, 5000));
    CHECK(e.metrics.counter(metric::kHeartbeatsSent) >= uint64_t{1});

    // 3. The peer probes us; we answer with the same TestReqID.
    peer.queue_test_request("e2e-probe-1");
    CHECK(e.pump_until(
        [&] {
            for (const std::string& f : peer.received())
                if (peer::get(f, 35).value_or("") == "0" &&
                    peer::get(f, 112).value_or("") == "e2e-probe-1")
                    return true;
            return false;
        },
        5000));

    // 4. A graceful logout, acknowledged by the peer.
    e.engine->request_graceful_stop();
    CHECK(e.pump_until([&] { return e.engine->state() == SessionState::Stopped; }, 5000));
    CHECK_EQ(e.engine->state(), SessionState::Stopped);
    CHECK(count_type(peer.received(), "5") >= 1);

    // 5. The durable store agrees with what went over the wire.
    const uint64_t out_final = e.engine->next_out();
    CHECK(out_final > uint64_t{3});  // Logon, at least one heartbeat, Logout
    const ConsistencyReport rep = e.store->verify_consistency();
    CHECK_MSG(rep.ok, rep.problem);

    std::vector<OutboundRecord> history;
    CHECK_EQ(e.store->load_outbound_range(1, out_final - 1, &history), StoreError::None);
    CHECK_EQ(history.size(), static_cast<size_t>(out_final - 1));
    for (const OutboundRecord& r : history) {
        std::string problem;
        CHECK_MSG(peer::verify_frame(r.payload, &problem),
                  "sequence " + std::to_string(r.seq) + ": " + problem);
        CHECK_EQ(peer::get(r.payload, 34).value_or(""), std::to_string(r.seq));
    }
    e.store->mark_clean_shutdown();
    peer.stop();
}

SCENARIO("E2E-02", "P0", "a real session survives an unclean restart and resumes its sequence") {
    uint64_t out_before = 0;
    std::string db;
    {
        tt::LoopbackPeer peer;
        peer.heartbeat_seconds = 1;
        uint16_t port = 0;
        CHECK(peer.start(&port));

        E2E first("e2e02", port);
        db = first.cfg.database_path;
        CHECK(
            first.pump_until([&] { return first.engine->state() == SessionState::Active; }, 5000));
        out_before = first.engine->next_out();
        CHECK(out_before >= uint64_t{2});
        // No clean shutdown marker: this models a process that died.
        peer.stop();
    }

    tt::LoopbackPeer peer2;
    peer2.heartbeat_seconds = 1;
    uint16_t port2 = 0;
    CHECK(peer2.start(&port2));

    // The same store, a new engine, a new connection.
    E2E second("e2e02b", port2, db);
    CHECK(second.audit.contains("UncleanShutdownDetected"));
    CHECK_EQ(second.engine->next_out(), out_before);
    // The counterparty resumes its own sequence too, as a real one would.
    peer2.set_next_out(second.engine->next_in());

    CHECK(second.pump_until([&] { return second.engine->state() == SessionState::Active; }, 5000));
    const auto received = peer2.received();
    CHECK(!received.empty());
    // The new Logon used the next sequence, not sequence one.
    CHECK_EQ(peer::get(received[0], 34).value_or(""), std::to_string(out_before));
    CHECK_EQ(second.engine->epoch(), uint64_t{1});
    second.store->mark_clean_shutdown();
    peer2.stop();
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "e2e_loopback"); }
