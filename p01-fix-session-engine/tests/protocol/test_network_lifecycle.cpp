// Group A of the what-if matrix: TCP and connection lifecycle.
// Design doc 15.2. These run the real TcpConnector against a scripted socket
// layer, so the partial-write loop, the generation guard and the backoff ladder
// are the production code paths.
#include <string>
#include <vector>

#include "fake_socket.hpp"
#include "fake_store.hpp"
#include "fix/tcp_connector.hpp"
#include "tiny_test.hpp"

using namespace fixse;

namespace {

// An engine driven through the real connector over a scripted socket.
struct NetFixture {
    VirtualClock clock;
    SessionConfig cfg;
    tt::FakeStore store;
    tt::FakeSocketOps sock;
    tt::RecordingSink sink;
    Metrics metrics;
    AuditLog audit;
    std::unique_ptr<TcpConnector> connector;
    std::unique_ptr<SessionEngine> engine;

    NetFixture() {
        cfg.sender_comp_id = "TRADER01";
        cfg.target_comp_id = "SIMLP01";
        cfg.qualifier = "NET";
        cfg.heartbeat_seconds = 30;
        cfg.reconnect_initial_ms = 1000;
        cfg.reconnect_max_ms = 16000;
        cfg.reconnect_jitter_percent = 0;  // deterministic ladder for assertions

        connector = std::make_unique<TcpConnector>(cfg, clock, sock);
        EngineDeps d;
        d.cfg = &cfg;
        d.store = &store;
        d.clock = &clock;
        d.transport = connector.get();
        d.controller = connector.get();
        d.sink = &sink;
        d.metrics = &metrics;
        d.audit = &audit;
        engine = std::make_unique<SessionEngine>(d);
        connector->bind_engine(engine.get());
        engine->start();
    }

    // Run one driver step: service a pending connect, read, run timers.
    void pump() { connector->poll_once(0); }
    void advance(long long ms) {
        clock.advance(Millis(ms));
        pump();
    }

    peer::Msg msg(std::string type, uint64_t seq) const {
        peer::Msg m;
        m.msg_type = std::move(type);
        m.seq = seq;
        m.sender = cfg.target_comp_id;
        m.target = cfg.sender_comp_id;
        m.sending_time = format_utc_timestamp(clock.wall_now());
        return m;
    }
    std::string logon_reply(uint64_t seq) const {
        peer::Msg m = msg("A", seq);
        m.extra.push_back({98, "0"});
        m.extra.push_back({108, std::to_string(cfg.heartbeat_seconds)});
        return m.build();
    }
    std::string app(uint64_t seq, const std::string& payload) const {
        peer::Msg m = msg("U1", seq);
        m.extra.push_back({20001, payload});
        return m.build();
    }

    // Connect and complete the handshake.
    void bring_up() {
        pump();  // performs the connect, engine writes its Logon
        sock.deliver(logon_reply(store.state.next_in));
        pump();
    }
};

}  // namespace

SCENARIO("A-04", "P0", "a refused connect backs off instead of spinning") {
    NetFixture f;
    f.sock.default_connect = tt::FakeSocketOps::ConnectOutcome::Refused;

    f.pump();
    CHECK_EQ(f.sock.connect_attempts, 1);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);

    // Pumping without advancing the clock must not retry: no busy loop.
    for (int i = 0; i < 50; ++i) f.pump();
    CHECK_EQ(f.sock.connect_attempts, 1);

    // After the backoff the engine tries again, and the delay grows.
    f.advance(1200);
    f.pump();
    CHECK_EQ(f.sock.connect_attempts, 2);
    f.advance(1200);
    f.pump();
    CHECK_EQ(f.sock.connect_attempts, 2);  // the second delay is longer
    f.advance(2500);
    f.pump();
    CHECK_EQ(f.sock.connect_attempts, 3);
    CHECK_EQ(f.sock.open_handles, 0);
}

SCENARIO("A-05", "P0", "a connect that never completes does not hang the engine") {
    NetFixture f;
    f.sock.default_connect = tt::FakeSocketOps::ConnectOutcome::Timeout;
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK(!f.sock.connect_errors.empty());
    CHECK_EQ(f.sock.connect_errors.back(), std::string("connect timed out"));
    CHECK_EQ(f.sock.open_handles, 0);
}

SCENARIO("A-06", "P0", "a silent peer after connect times out at the handshake") {
    NetFixture f;
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);
    // The Logon went out, nothing comes back.
    CHECK(!f.sock.sent_frames().empty());
    f.advance(30001);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.sock.open_handles, 0);
}

SCENARIO("A-07", "P0", "a FIN before the Logon acknowledgement does not reach ACTIVE") {
    NetFixture f;
    f.pump();
    f.sock.deliver_fin();
    f.pump();
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.sock.open_handles, 0);
}

SCENARIO("A-08", "P0", "a FIN while ACTIVE keeps sequence state intact") {
    NetFixture f;
    f.bring_up();
    CHECK_EQ(f.engine->state(), SessionState::Active);
    f.sock.deliver(f.app(f.store.state.next_in, "one"));
    f.pump();
    const uint64_t in_before = f.store.state.next_in;
    const uint64_t out_before = f.store.state.next_out;

    f.sock.deliver_fin();
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.store.state.next_in, in_before);
    CHECK_EQ(f.store.state.next_out, out_before);
    CHECK_EQ(f.store.state.epoch, uint64_t{1});
}

SCENARIO("A-09", "P0", "a reset in the middle of a frame discards the partial frame") {
    NetFixture f;
    f.bring_up();
    const uint64_t expected = f.store.state.next_in;
    const std::string frame = f.app(expected, "half delivered");

    // Half the frame arrives, then the connection is reset.
    f.sock.deliver(frame.substr(0, frame.size() / 2));
    f.pump();
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    f.sock.deliver_reset();
    f.pump();

    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.store.state.next_in, expected);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
}

SCENARIO("A-10", "P0", "a partition with a live socket is detected by the TestRequest") {
    NetFixture f;
    f.bring_up();
    // Packets stop flowing in both directions but the socket stays open.
    f.advance(30000);
    CHECK(f.engine->test_request_pending());
    f.advance(30001);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.metrics.counter(metric::kTestRequestTimeouts), uint64_t{1});
}

SCENARIO("A-11", "P0", "a one-way outage is detected rather than reported as ready") {
    NetFixture f;
    f.bring_up();
    // Our writes still succeed; nothing arrives. Readiness must go false.
    for (int i = 0; i < 4; ++i) f.advance(20000);
    CHECK(!f.engine->ready());
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
}

SCENARIO("A-12", "P1", "a thousand connection flaps leak no handles and keep one owner") {
    NetFixture f;
    for (int i = 0; i < 1000; ++i) {
        f.pump();  // connect
        f.sock.deliver_fin();
        f.pump();  // disconnect
        // Walk the clock past the backoff, which is capped by reconnect_max_ms.
        f.advance(20000);
    }
    // No leak: at most one socket is live at any moment, and every socket that
    // was opened has been closed again.
    CHECK(f.sock.open_handles <= 1);
    CHECK_EQ(f.sock.peak_open_handles, 1);
    CHECK(f.sock.connect_attempts >= 900);
    CHECK(f.sock.closes >= f.sock.connect_attempts - 1);
    // Sequence state survived a thousand reconnects without a reset.
    CHECK_EQ(f.store.state.epoch, uint64_t{1});
    CHECK(f.store.state.next_out > uint64_t{900});
}

SCENARIO("A-13", "P0", "a socket accepting a few bytes at a time preserves frame order") {
    NetFixture f;
    f.sock.max_bytes_per_send = 3;  // three bytes per syscall
    f.pump();
    // The connector keeps being driven; the partial-write loop resumes at the
    // exact offset each time.
    for (int i = 0; i < 200; ++i) f.pump();

    const auto frames = f.sock.sent_frames();
    CHECK(!frames.empty());
    std::string why;
    CHECK_MSG(peer::verify_frame(frames[0], &why), why);
    CHECK_EQ(peer::get(frames[0], 35).value_or(""), std::string("A"));
    CHECK(f.metrics.counter(metric::kPartialWrites) >= uint64_t{1});
    // No byte was duplicated: the frame appears exactly once.
    CHECK_EQ(frames.size(), size_t{1});
}

SCENARIO("A-14", "P0", "a would-block write is retried without duplicating the frame") {
    NetFixture f;
    f.sock.would_block_sends = 5;
    f.pump();
    CHECK_EQ(f.sock.sent.size(), size_t{0});
    for (int i = 0; i < 10; ++i) f.pump();

    const auto frames = f.sock.sent_frames();
    CHECK_EQ(frames.size(), size_t{1});
    std::string why;
    CHECK_MSG(peer::verify_frame(frames[0], &why), why);
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);
}

SCENARIO("A-15", "P0", "a stale callback from a replaced socket is ignored") {
    NetFixture f;
    f.bring_up();
    const uint64_t live_generation = f.connector->generation();
    const uint64_t in_before = f.store.state.next_in;

    // A read completion from the previous socket arrives late.
    f.engine->on_bytes(f.app(in_before, "from an old socket"), live_generation - 1);
    CHECK_EQ(f.store.state.next_in, in_before);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});

    // A disconnect for the old generation must not tear down the live session.
    f.engine->on_disconnect(live_generation - 1, "stale disconnect");
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK(f.audit.contains("stale"));

    // The live generation still works.
    f.sock.deliver(f.app(in_before, "live"));
    f.pump();
    CHECK_EQ(f.sink.delivered.size(), size_t{1});
}

SCENARIO("A-16", "P1", "a stop request during connect leaves consistent state") {
    NetFixture f;
    CHECK_EQ(f.engine->state(), SessionState::Connecting);
    f.engine->request_graceful_stop();
    CHECK_EQ(f.engine->state(), SessionState::Stopped);
    const uint64_t out_before = f.store.state.next_out;
    // A stopped engine neither connects nor writes.
    for (int i = 0; i < 5; ++i) f.pump();
    CHECK_EQ(f.store.state.next_out, out_before);
    CHECK_EQ(f.sock.open_handles, 0);
}

SCENARIO("A-17", "P1", "a run of connect failures stays bounded and keeps backing off") {
    NetFixture f;
    f.sock.default_connect = tt::FakeSocketOps::ConnectOutcome::Refused;
    for (int i = 0; i < 40; ++i) f.advance(20000);
    // The ladder is capped, so attempts are regular but never a storm.
    CHECK(f.sock.connect_attempts <= 45);
    CHECK(f.sock.connect_attempts >= 30);
    CHECK_EQ(f.sock.open_handles, 0);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
}

SCENARIO("A-18", "P2", "a first dead endpoint is followed by a successful one") {
    NetFixture f;
    f.sock.connect_script.push_back(tt::FakeSocketOps::ConnectOutcome::Refused);
    f.sock.connect_script.push_back(tt::FakeSocketOps::ConnectOutcome::Success);
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    f.advance(1500);
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);
    f.sock.deliver(f.logon_reply(f.store.state.next_in));
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO_BLOCKED("A-19", "P0", "a failed TLS handshake must not fall back to plaintext",
                 "TLS is out of v1 scope; startup refuses tls_enabled instead of downgrading, "
                 "which scenario C-19 asserts. A real handshake test needs a TLS stack.")

SCENARIO("A-01", "P1", "a resolve failure is treated as a transient transport error") {
    NetFixture f;
    f.sock.connect_script.push_back(tt::FakeSocketOps::ConnectOutcome::Refused);
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    // No sequence number is consumed by a connect that never happened.
    CHECK_EQ(f.store.state.next_out, uint64_t{1});
    CHECK_EQ(f.sock.sent.size(), size_t{0});
}

SCENARIO("A-02", "P1", "repeated resolve failures end in one successful session") {
    NetFixture f;
    for (int i = 0; i < 3; ++i)
        f.sock.connect_script.push_back(tt::FakeSocketOps::ConnectOutcome::Timeout);
    f.sock.connect_script.push_back(tt::FakeSocketOps::ConnectOutcome::Success);
    for (int i = 0; i < 8 && f.engine->state() != SessionState::AwaitLogon; ++i) f.advance(20000);
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);
    f.sock.deliver(f.logon_reply(f.store.state.next_in));
    f.pump();
    CHECK_EQ(f.engine->state(), SessionState::Active);
    // Exactly one live connection, and exactly one Logon reached the wire.
    CHECK_EQ(f.sock.peak_open_handles, 1);
    int logons = 0;
    for (const std::string& fr : f.sock.sent_frames())
        if (peer::get(fr, 35).value_or("") == "A") ++logons;
    CHECK_EQ(logons, 1);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "network_lifecycle"); }
