// Groups D and H of the what-if matrix: heartbeats, TestRequest liveness,
// clocks and session schedule. Design doc 15.5, 15.9 and section 10.
//
// The rule being tested throughout: liveness is measured on a monotonic clock,
// and only a VALID inbound frame proves the peer is processing FIX. A live TCP
// socket proves nothing (playbook PB-08).
#include <string>

#include "engine_fixture.hpp"
#include "fix/wire.hpp"

using namespace fixse;

namespace {

std::string type_of(const std::string& f) { return peer::get(f, 35).value_or(""); }

int count_type(const std::vector<std::string>& frames, const std::string& t) {
    int n = 0;
    for (const std::string& f : frames)
        if (type_of(f) == t) ++n;
    return n;
}

}  // namespace

SCENARIO("D-01", "P0", "an idle outbound session emits a heartbeat with a fresh sequence") {
    tt::EngineFixture f("d01");
    f.connect_and_logon();
    const uint64_t out_before = f.engine->next_out();
    f.out();

    f.advance(30000);
    const auto frames = f.transport.new_frames();
    CHECK(count_type(frames, "0") >= 1);
    for (const std::string& fr : frames) {
        if (type_of(fr) != "0") continue;
        CHECK_EQ(peer::get(fr, 34).value_or(""), std::to_string(out_before));
        std::string why;
        CHECK_MSG(peer::verify_frame(fr, &why), why);
        break;
    }
    CHECK(f.engine->next_out() > out_before);
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("D-02", "P1", "a busy session does not emit redundant heartbeats") {
    tt::EngineFixture f("d02");
    f.connect_and_logon();
    f.out();
    const uint64_t hb_before = f.metrics.counter(metric::kHeartbeatsSent);

    // Traffic in both directions well inside the interval.
    for (uint64_t i = 0; i < 6; ++i) {
        f.advance(10000);
        CHECK_EQ(f.engine->submit_app_message("U1", {{20001, "tick"}}), SubmitResult::Accepted);
        f.feed(f.app(f.engine->next_in(), "pong"));
    }
    CHECK_EQ(f.metrics.counter(metric::kHeartbeatsSent), hb_before);
    CHECK_EQ(f.metrics.counter(metric::kTestRequests), uint64_t{0});
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("D-03", "P0", "an idle inbound session sends a TestRequest with a unique id") {
    tt::EngineFixture f("d03");
    f.connect_and_logon();
    f.out();

    f.advance(30000);
    const auto tr = f.out_of_type("1");
    CHECK(tr.has_value());
    const std::string id = peer::get(*tr, 112).value_or("");
    CHECK(!id.empty());
    CHECK(f.engine->test_request_pending());
    CHECK_EQ(f.engine->pending_test_req_id(), id);
}

SCENARIO("D-04", "P0", "a heartbeat echoing the pending TestReqID clears it") {
    tt::EngineFixture f("d04");
    f.connect_and_logon();
    f.advance(30000);
    const std::string id = f.engine->pending_test_req_id();
    CHECK(!id.empty());

    peer::Msg hb = f.msg("0", f.engine->next_in());
    hb.extra.push_back({112, id});
    f.feed(hb.build());

    CHECK(!f.engine->test_request_pending());
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("D-05", "P0", "a heartbeat with a different TestReqID does not clear the request") {
    tt::EngineFixture f("d05");
    f.connect_and_logon();
    f.advance(30000);
    CHECK(f.engine->test_request_pending());

    peer::Msg hb = f.msg("0", f.engine->next_in());
    hb.extra.push_back({112, "some-other-id"});
    f.feed(hb.build());

    CHECK(f.engine->test_request_pending());
    CHECK(f.audit.contains("did not match the pending TestReqID"));
}

SCENARIO("D-06", "P0", "a bare heartbeat does not answer an outstanding TestRequest") {
    tt::EngineFixture f("d06");
    f.connect_and_logon();
    f.advance(30000);
    CHECK(f.engine->test_request_pending());

    f.feed(f.msg("0", f.engine->next_in()).build());
    CHECK(f.engine->test_request_pending());
}

SCENARIO("D-07", "P0", "a heartbeat after the deadline cannot resurrect the session") {
    tt::EngineFixture f("d07");
    f.connect_and_logon();
    f.advance(30000);
    const std::string id = f.engine->pending_test_req_id();
    CHECK(!id.empty());

    // The deadline is max(heartbeat interval, test timeout) = 30s.
    f.advance(30001);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.metrics.counter(metric::kTestRequestTimeouts), uint64_t{1});
    const uint64_t in_at_timeout = f.engine->next_in();

    // The answer finally arrives on the socket that has already been dropped.
    peer::Msg late = f.msg("0", in_at_timeout);
    late.extra.push_back({112, id});
    f.feed(late.build());
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.engine->next_in(), in_at_timeout);
}

SCENARIO("D-08", "P0", "a TestRequest without a TestReqID is rejected, not answered") {
    tt::EngineFixture f("d08");
    f.connect_and_logon();
    f.out();

    f.feed(f.msg("1", f.engine->next_in()).build());
    const auto frames = f.transport.new_frames();
    CHECK_EQ(count_type(frames, "0"), 0);
    CHECK_EQ(count_type(frames, "3"), 1);
    for (const std::string& fr : frames) {
        if (type_of(fr) != "3") continue;
        CHECK_EQ(peer::get(fr, 373).value_or(""), std::string("1"));  // RequiredTagMissing
    }
}

SCENARIO("D-09", "P0", "a valid TestRequest is answered with the same TestReqID") {
    tt::EngineFixture f("d09");
    f.connect_and_logon();
    f.out();

    peer::Msg tr = f.msg("1", f.engine->next_in());
    tr.extra.push_back({112, "peer-probe-1"});
    f.feed(tr.build());

    const auto hb = f.out_of_type("0");
    CHECK(hb.has_value());
    CHECK_EQ(peer::get(*hb, 112).value_or(""), std::string("peer-probe-1"));
}

SCENARIO("D-10", "P1", "two TestRequests are answered separately with their own ids") {
    tt::EngineFixture f("d10");
    f.connect_and_logon();
    f.out();

    for (const char* id : {"probe-a", "probe-b"}) {
        peer::Msg tr = f.msg("1", f.engine->next_in());
        tr.extra.push_back({112, id});
        f.feed(tr.build());
    }
    const auto frames = f.transport.new_frames();
    CHECK_EQ(count_type(frames, "0"), 2);
    std::vector<std::string> ids;
    for (const std::string& fr : frames)
        if (type_of(fr) == "0") ids.push_back(peer::get(fr, 112).value_or(""));
    CHECK_EQ(ids.size(), size_t{2});
    CHECK_EQ(ids[0], std::string("probe-a"));
    CHECK_EQ(ids[1], std::string("probe-b"));
}

SCENARIO("D-11", "P1", "a late timer tick is handled without a stuck state") {
    tt::EngineFixture f("d11");
    f.connect_and_logon();
    f.out();

    // Monotonic time leaps forward without the wall clock moving, which is what
    // CPU starvation or a suspended process looks like.
    f.clock.advance_steady(Millis(300000));
    f.tick();
    // The engine reacts once, consistently: it does not emit 300 seconds worth
    // of heartbeats.
    CHECK(f.metrics.counter(metric::kHeartbeatsSent) <= uint64_t{2});
    CHECK(f.engine->state() == SessionState::Active ||
          f.engine->state() == SessionState::Backoff);

    f.clock.advance_steady(Millis(300000));
    f.tick();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK_EQ(f.metrics.counter(metric::kTestRequestTimeouts), uint64_t{1});
}

SCENARIO("D-12", "P0", "a wall clock step backwards does not disturb liveness") {
    tt::EngineFixture f("d12");
    f.connect_and_logon();
    f.out();
    const uint64_t epoch_before = f.engine->epoch();
    const uint64_t in_before = f.engine->next_in();

    // NTP steps the wall clock back an hour. Monotonic time is untouched.
    f.clock.jump_wall(std::chrono::seconds(-3600));
    f.tick();
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK_EQ(f.engine->next_in(), in_before);

    // Heartbeat scheduling still works off monotonic time.
    f.advance(30000);
    CHECK(f.metrics.counter(metric::kHeartbeatsSent) >= uint64_t{1});
    CHECK(f.engine->state() == SessionState::Active);
}

SCENARIO("D-13", "P1", "valid inbound traffic refreshes the inbound idle timer") {
    tt::EngineFixture f("d13");
    f.connect_and_logon();
    f.out();

    // Inbound activity every 20 seconds keeps the engine from probing.
    for (int i = 0; i < 5; ++i) {
        f.advance(20000);
        f.feed(f.app(f.engine->next_in(), "data"));
    }
    CHECK_EQ(f.metrics.counter(metric::kTestRequests), uint64_t{0});
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.sink.delivered.size(), size_t{5});
}

SCENARIO("D-14", "P0", "a flood of malformed frames does not count as liveness") {
    tt::EngineFixture f("d14");
    f.connect_and_logon();
    f.out();

    // Bytes that can never form a valid frame. The engine drops the connection
    // rather than treating the noise as a healthy peer.
    f.feed_bytes("garbage-that-is-not-fix");
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.metrics.counter(metric::kMalformedFrames) >= uint64_t{1});
    CHECK(!f.controller.disconnects.empty());
}

SCENARIO("D-15", "P0", "a peer that reads TCP but ignores FIX is detected and dropped") {
    tt::EngineFixture f("d15");
    f.connect_and_logon();
    f.out();

    // The socket stays writable, nothing comes back. This is playbook PB-08.
    f.advance(30000);
    CHECK(f.engine->test_request_pending());
    CHECK_EQ(f.engine->state(), SessionState::Active);

    f.advance(30001);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK(!f.controller.disconnects.empty());
    CHECK_EQ(f.controller.disconnects.back().reason, std::string("test request timeout"));
    // Exactly one TestRequest per idle window, not an endless stream.
    CHECK(f.metrics.counter(metric::kTestRequests) <= uint64_t{2});
}

SCENARIO("H-01", "P0", "a stale SendingTime is refused while timers keep working") {
    tt::EngineFixture f("h01");
    f.connect_and_logon();
    f.out();

    // Build a message now, then step the wall clock forward an hour so the
    // message looks an hour old when it arrives.
    const std::string stale = f.app(f.engine->next_in(), "stale");
    f.clock.jump_wall(std::chrono::seconds(3600));
    const uint64_t in_before = f.engine->next_in();
    f.feed(stale);

    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.audit.contains("SendingTime outside tolerance"));
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->next_in(), in_before);
}

SCENARIO("H-02", "P0", "a wall clock step backwards produces no negative idle time") {
    tt::EngineFixture f("h02");
    f.connect_and_logon();
    f.clock.jump_wall(std::chrono::seconds(-3600));
    // A message stamped with the stepped-back clock is inside tolerance again,
    // and nothing resets.
    f.feed(f.app(f.engine->next_in(), "after step"));
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.sink.delivered.size(), size_t{1});
    CHECK_EQ(f.engine->epoch(), uint64_t{1});
}

SCENARIO("H-03", "P0", "drift beyond tolerance ends the session with a reject and logout") {
    tt::EngineFixture f("h03");
    f.connect_and_logon();
    f.out();

    peer::Msg skewed = f.msg("U1", f.engine->next_in());
    skewed.sending_time = format_utc_timestamp(f.clock.wall_now() + std::chrono::seconds(300));
    skewed.extra.push_back({20001, "from the future"});
    f.feed(skewed.build());

    const auto frames = f.transport.new_frames();
    CHECK_EQ(count_type(frames, "3"), 1);
    CHECK_EQ(count_type(frames, "5"), 1);
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
}

SCENARIO("H-07", "P0", "crossing UTC midnight creates no epoch and no sequence reset") {
    tt::EngineFixture f("h07");
    f.connect_and_logon();
    const uint64_t epoch_before = f.engine->epoch();

    // Walk across a day boundary in heartbeat-sized steps, answering probes so
    // the session stays up.
    for (int i = 0; i < 48; ++i) {
        f.advance(20000);
        f.feed(f.app(f.engine->next_in(), "overnight"));
    }
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK(f.engine->next_in() > uint64_t{40});
    CHECK(!f.audit.contains("CoordinatedResetApplied"));
}

SCENARIO("H-09", "P0", "a reconnect at a schedule boundary does not create a second epoch") {
    tt::EngineFixture f("h09");
    f.connect_and_logon();
    const uint64_t in_before = f.engine->next_in();
    const uint64_t out_before = f.engine->next_out();

    // Drop the connection exactly while the virtual day rolls over.
    f.clock.jump_wall(std::chrono::seconds(3600 * 19));
    f.engine->on_disconnect(f.generation, "link lost at rollover");
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    f.reconnect();

    CHECK_EQ(f.engine->epoch(), uint64_t{1});
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK(f.engine->next_out() > out_before);  // a new Logon is a new original
}

SCENARIO("H-11", "P2", "a very large monotonic delta does not overflow the timers") {
    tt::EngineFixture f("h11");
    f.connect_and_logon();
    // Ten years of monotonic time in one step.
    f.clock.advance_steady(Millis(315360000000LL));
    f.tick();
    CHECK(f.engine->state() == SessionState::Active ||
          f.engine->state() == SessionState::Backoff);
    f.clock.advance_steady(Millis(315360000000LL));
    f.tick();
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "heartbeat_timers"); }
