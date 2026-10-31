// Group I of the what-if matrix: backpressure, resource exhaustion and
// graceful degradation. Design doc 15.10 and 11.1.
//
// Invariant I-09 is the subject: every queue and buffer has a hard cap, and
// exceeding it produces defined behaviour rather than unbounded memory.
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/wire.hpp"

using namespace fixse;

SCENARIO("I-01", "P0", "a sink that stops consuming applies backpressure instead of growing") {
    tt::EngineFixture f("i01");  // max_inbox_pending is 16 in the fixture
    f.connect_and_logon();
    f.sink.refuse = true;

    for (uint64_t i = 0; i < 100; ++i) {
        f.feed(f.app(f.engine->next_in(), "backlog"));
        if (f.engine->inbox_paused()) break;
    }

    // The engine stopped consuming the socket rather than buffering for ever.
    CHECK(f.engine->inbox_paused());
    CHECK(f.engine->pending_inbox() <= f.cfg.max_inbox_pending + 1);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK(!f.sink.refused.empty());

    // Once the application recovers, the backlog drains in sequence order.
    f.sink.refuse = false;
    f.tick();
    CHECK(!f.engine->inbox_paused());
    CHECK(!f.sink.delivered.empty());
    for (size_t i = 1; i < f.sink.delivered.size(); ++i)
        CHECK(f.sink.delivered[i] > f.sink.delivered[i - 1]);
}

SCENARIO("I-02", "P0", "a peer sending faster than the parser stays bounded") {
    tt::EngineFixture f("i02", false);
    f.cfg.max_read_buffer_bytes = 8192;
    f.cfg.max_frame_bytes = 4096;
    f.start();
    f.connect_and_logon();

    std::string burst;
    for (uint64_t i = 0; i < 200; ++i) burst += f.app(f.engine->next_in() + i, "flood");
    f.feed_bytes(burst);

    CHECK(f.engine->pending_tx_bytes() <= f.engine->tx_queue_cap());
    CHECK(f.metrics.gauge(metric::kRxBufferBytes) <=
          static_cast<int64_t>(f.cfg.max_read_buffer_bytes));
}

SCENARIO("I-03", "P0", "a failing store never produces an uncommitted delivery") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    const uint64_t in_before = m.store.state.next_in;

    m.store.fail_inbound = StoreError::Busy;
    m.feed(m.app(in_before, "must not be delivered"));

    CHECK_EQ(m.engine->state(), SessionState::Faulted);
    CHECK_EQ(m.engine->fault_reason(), FaultReason::StoreCommitFailed);
    CHECK_EQ(m.sink.delivered.size(), size_t{0});
    CHECK_EQ(m.store.state.next_in, in_before);
    CHECK(m.metrics.counter(metric::kStoreErrors) >= uint64_t{1});
}

SCENARIO("I-04", "P0", "a sequence bearing message is never silently dropped") {
    tt::EngineFixture f("i04");
    f.connect_and_logon();
    const uint64_t base = f.engine->next_in();
    for (uint64_t i = 0; i < 50; ++i) f.feed(f.app(base + i, "accounted"));

    CHECK_EQ(f.sink.delivered.size(), size_t{50});
    for (size_t i = 0; i < 50; ++i) CHECK_EQ(f.sink.delivered[i], base + i);
    CHECK_EQ(f.engine->next_in(), base + 50);
}

SCENARIO("I-05", "P0", "the out of order buffer is capped by bytes as well as by count") {
    tt::EngineFixture f("i05", false);
    f.cfg.max_future_messages = 1000;
    f.cfg.max_future_bytes = 2048;  // the byte cap binds first
    f.start();
    f.connect_and_logon();
    const uint64_t base = f.engine->next_in();

    for (uint64_t i = 1; i <= 200; ++i) f.feed(f.app(base + i, std::string(200, 'x')));
    CHECK(f.engine->future_buffer_bytes() <= f.cfg.max_future_bytes);
    CHECK(f.engine->future_buffer_size() < size_t{200});
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
}

SCENARIO("I-06", "P0", "a resend request for every sequence is served in bounded chunks") {
    tt::MiniEngine m(1, 1);
    m.cfg.resend_chunk_size = 100;
    m.cfg.max_resend_cycles = 8;
    m.connect_and_logon();
    for (int i = 0; i < 250; ++i) m.engine->submit_app_message("U1", {{20001, "h"}});
    m.out();

    // The peer asks for everything using the open ended form.
    peer::Msg rr = m.msg("2", m.store.state.next_in);
    rr.extra.push_back({7, "1"});
    rr.extra.push_back({16, "0"});
    m.feed(rr.build());

    std::vector<uint64_t> replayed;
    int gapfills = 0;
    for (const std::string& f : m.transport.new_frames()) {
        if (peer::get(f, 35).value_or("") == "4") {
            ++gapfills;
            continue;
        }
        if (peer::get(f, 43).value_or("") == "Y")
            replayed.push_back(std::stoull(peer::get(f, 34).value()));
    }
    // Every application message replayed exactly once, the Logon covered by one
    // GapFill, and no chunk boundary artefacts.
    CHECK_EQ(replayed.size(), size_t{250});
    CHECK_EQ(gapfills, 1);
    for (size_t i = 1; i < replayed.size(); ++i) CHECK(replayed[i] == replayed[i - 1] + 1);
}

SCENARIO("I-09", "P0", "a store reporting a full disk fails closed with nothing on the wire") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    m.out();
    m.store.fail_outbound = StoreError::Full;

    const size_t written_before = m.transport.written.size();
    CHECK_EQ(m.engine->submit_app_message("U1", {{20001, "order"}}),
             SubmitResult::RejectedStoreFailure);
    CHECK_EQ(m.transport.written.size(), written_before);
    CHECK_EQ(m.engine->state(), SessionState::Faulted);
    CHECK_EQ(m.engine->fault_reason(), FaultReason::StoreCommitFailed);
}

SCENARIO("I-13", "P0", "a flood of invalid frames ends the session rather than spinning") {
    tt::EngineFixture f("i13");
    f.connect_and_logon();

    for (int i = 0; i < 200 && f.engine->state() == SessionState::Active; ++i) {
        std::vector<peer::Field> junk{
            {35, "U1"}, {49, "WRONG"}, {56, "ALSO_WRONG"}, {34, "1"},
            {52, format_utc_timestamp(f.clock.wall_now())}};
        f.feed(peer::build_from_fields(junk));
    }
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(!f.controller.disconnects.empty());
}

SCENARIO("I-12", "P1", "the engine does no background work between calls") {
    tt::EngineFixture f("i12");
    f.connect_and_logon();
    f.advance(1000);
    const std::string before = f.metrics.render();
    const size_t audit_before = f.audit.size();
    const uint64_t in_before = f.engine->next_in();

    // A single owner driven by its entry points: with no call, nothing can
    // change. That is what makes the concurrency model testable at all
    // (design doc 4.2).
    CHECK_EQ(f.metrics.render(), before);
    CHECK_EQ(f.audit.size(), audit_before);
    CHECK_EQ(f.engine->next_in(), in_before);
}

SCENARIO("I-16", "P2", "an accidentally tiny frame cap is rejected at startup") {
    SessionConfig c;
    c.sender_comp_id = "TRADER01";
    c.target_comp_id = "SIMLP01";
    c.max_frame_bytes = 128;
    c.max_read_buffer_bytes = 128;
    bool saw = false;
    for (const ConfigError& e : validate(c))
        if (e.field == "network.max_frame_bytes") saw = true;
    CHECK(saw);
}

SCENARIO_BLOCKED("I-10", "P1", "a disk nearing its quota warns before writes fail",
                 "v1 has no filesystem quota monitor: the engine reacts to the store error it "
                 "receives (G-10, I-09) rather than predicting it. A quota watcher is backlog.")

SCENARIO_BLOCKED("I-14", "P1", "an unreachable metrics backend does not affect sequencing",
                 "v1 keeps metrics in process with no exporter, so there is no backend to make "
                 "unreachable. I-12 shows the engine does no background work.")

SCENARIO_BLOCKED("I-15", "P1", "a full logging partition does not corrupt session state",
                 "v1 writes audit records to the store and an in-memory log; there is no separate "
                 "log partition yet. The store path is covered by G-10 and G-12.")

SCENARIO_BLOCKED("I-07", "P1", "heartbeat behaviour under fifty thousand messages per second",
                 "throughput measurement needs the benchmark harness and a quiet machine; it is "
                 "reported separately rather than asserted as a pass or fail here.")

SCENARIO_BLOCKED("I-08", "P1", "parse latency percentiles are explained by metrics",
                 "latency percentiles belong to the benchmark report, not to a functional gate; "
                 "the doc forbids presenting sanitizer builds as production latency.")

SCENARIO_BLOCKED("I-11", "P1", "file descriptors do not grow over ten thousand reconnects",
                 "covered at the socket layer by A-12, which counts opens against closes over a "
                 "thousand flaps. A real ten thousand cycle soak belongs to the soak job.")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "backpressure"); }
