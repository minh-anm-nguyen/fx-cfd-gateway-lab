// Group K of the what-if matrix: concurrency, lifecycle and operability.
// Design doc 15.12, 4.2 and 14.3.
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/wire.hpp"

using namespace fixse;

SCENARIO("K-01", "P0", "a timer and a disconnect in the same tick produce one transition") {
    tt::EngineFixture f("k01");
    f.connect_and_logon();
    f.clock.advance(Millis(30000));

    // Both events reach the single owner in the same virtual instant.
    f.engine->on_disconnect(f.generation, "link lost");
    f.engine->on_timer(f.clock.steady_now());

    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    // One disconnect, one backoff: the timer did not tear anything else down.
    CHECK_EQ(f.metrics.counter(metric::kReconnects), uint64_t{1});
}

SCENARIO("K-02", "P0", "a ResendRequest arriving while logging out produces no ghost send") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    for (int i = 0; i < 3; ++i) m.engine->submit_app_message("U1", {{20001, "h"}});
    m.engine->request_graceful_stop();
    CHECK_EQ(m.engine->state(), SessionState::LoggingOut);
    m.out();

    peer::Msg rr = m.msg("2", m.store.state.next_in);
    rr.extra.push_back({7, "2"});
    rr.extra.push_back({16, "3"});
    m.feed(rr.build());
    const auto during = m.transport.new_frames();

    m.engine->on_disconnect(m.generation, "closed");
    CHECK_EQ(m.engine->state(), SessionState::Stopped);
    // Nothing is emitted once the session has stopped.
    const size_t written_at_stop = m.transport.written.size();
    m.feed(rr.build());
    m.tick();
    CHECK_EQ(m.transport.written.size(), written_at_stop);
    CHECK(during.size() <= size_t{4});
}

SCENARIO("K-03", "P0", "a stop request while the store is failing still reaches a terminal state") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    m.store.fail_outbound = StoreError::IoError;

    // The Logout cannot be committed, so it is not sent either; the engine must
    // still settle rather than hang.
    m.engine->request_graceful_stop();
    CHECK(m.engine->state() == SessionState::Faulted ||
          m.engine->state() == SessionState::Stopped ||
          m.engine->state() == SessionState::LoggingOut);
    m.store.fail_outbound = StoreError::None;
    m.engine->on_disconnect(m.generation, "closed");
    CHECK(m.engine->state() == SessionState::Stopped ||
          m.engine->state() == SessionState::Faulted);
}

SCENARIO("K-06", "P0", "a socket callback after a coordinated reset cannot mutate state") {
    tt::EngineFixture f("k06");
    f.connect_and_logon();
    const uint64_t old_generation = f.generation;
    for (int i = 0; i < 2; ++i) f.feed(f.app(f.engine->next_in(), "pre-reset"));

    CHECK(f.engine->operator_coordinated_reset("alice", "bob", "agreed reset"));
    CHECK_EQ(f.engine->epoch(), uint64_t{2});
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
    f.reconnect();
    const uint64_t in_after = f.engine->next_in();

    // A read completion from before the reset arrives late.
    f.engine->on_bytes(f.app(1, "from the old epoch"), old_generation);
    CHECK_EQ(f.engine->next_in(), in_after);
    CHECK_EQ(f.engine->epoch(), uint64_t{2});
}

SCENARIO("K-07", "P0", "changing a CompID is a different session identity, not a silent reuse") {
    SessionConfig a;
    a.sender_comp_id = "TRADER01";
    a.target_comp_id = "SIMLP01";
    a.qualifier = "PROD";
    SessionConfig b = a;
    b.target_comp_id = "OTHER_LP";

    // The session identity is also the ownership and state key, so a
    // reconfigured engine cannot inherit sequence state negotiated with a
    // different counterparty.
    CHECK_NE(a.session_id(), b.session_id());
    CHECK(a.session_id().find("SIMLP01") != std::string::npos);
    CHECK(b.session_id().find("OTHER_LP") != std::string::npos);
}

SCENARIO("K-09", "P0", "readiness is false while a gap is being recovered") {
    tt::EngineFixture f("k09");
    f.connect_and_logon();
    CHECK(f.engine->ready());

    f.feed(f.app(f.engine->next_in() + 1, "ahead"));
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
    // A health check must not report ready here: the application would be told
    // the session is usable while messages are still missing.
    CHECK(!f.engine->ready());

    f.feed(f.app(f.engine->next_in(), "fills the hole"));
    CHECK(f.engine->ready());
}

SCENARIO("K-10", "P0", "a hundred restarts neither reset nor lose sequence state") {
    tt::EngineFixture f("k10");
    f.connect_and_logon();
    for (int i = 0; i < 3; ++i) f.feed(f.app(f.engine->next_in(), "data"));

    uint64_t last_in = f.engine->next_in();
    uint64_t last_out = f.engine->next_out();
    for (int cycle = 0; cycle < 100; ++cycle) {
        const std::string at = "cycle " + std::to_string(cycle);
        f.restart();
        CHECK_MSG(f.engine->epoch() == uint64_t{1}, at);
        CHECK_MSG(f.engine->next_in() == last_in, at);
        CHECK_MSG(f.engine->next_out() >= last_out, at);
        f.reconnect();
        f.feed(f.logon_reply(f.engine->next_in()));
        CHECK_MSG(f.engine->state() == SessionState::Active, at);
        f.feed(f.app(f.engine->next_in(), "after restart"));
        last_in = f.engine->next_in();
        last_out = f.engine->next_out();
    }
    CHECK(last_in > uint64_t{200});
}

SCENARIO("K-11", "P0", "an operator reset outside the agreement is refused") {
    tt::EngineFixture f("k11");
    f.connect_and_logon();
    const uint64_t epoch_before = f.engine->epoch();
    CHECK(!f.engine->operator_coordinated_reset("solo-operator", "", "I am in a hurry"));
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK(f.audit.contains("SequenceResetRefused"));
}

SCENARIO("K-12", "P2", "metric counters near their maximum stay well defined") {
    Metrics m;
    m.inc("c", UINT64_MAX - 2);
    m.inc("c", 1);
    CHECK_EQ(m.counter("c"), UINT64_MAX - 1);
    m.set("g", INT64_MAX);
    CHECK_EQ(m.gauge("g"), INT64_MAX);
    m.max_of("g", INT64_MAX - 5);
    CHECK_EQ(m.gauge("g"), INT64_MAX);
    CHECK(!m.render().empty());
}

SCENARIO("K-14", "P0", "a duplicate deployment cannot both own the session") {
    tt::EngineFixture first("k14");
    SqliteSessionStore::Options o;
    o.path = first.db_path();
    o.key.sender_comp_id = first.cfg.sender_comp_id;
    o.key.target_comp_id = first.cfg.target_comp_id;
    o.key.qualifier = first.cfg.qualifier;
    o.create_if_missing = false;
    o.clock = &first.clock;
    std::unique_ptr<SqliteSessionStore> second;
    std::string why;
    CHECK_EQ(SqliteSessionStore::open(o, &second, &why), StoreError::NotOwner);
    CHECK(second == nullptr);
}

SCENARIO_BLOCKED("K-04", "P0", "a kill during a durability commit recovers safely",
                 "runs in the crash suite, which spawns a child and terminates it at an exact "
                 "store failpoint; an in-process test cannot model an abrupt kill.")

SCENARIO_BLOCKED("K-05", "P0", "two threads cannot reserve the same sequence number",
                 "the engine is single owner by construction (design doc 4.2), so a "
                 "multi-threaded writer is not a supported configuration and there is no "
                 "behaviour to assert. Ownership across processes is enforced and tested by "
                 "C-18, G-21 and K-14.")

SCENARIO_BLOCKED("K-08", "P1", "a hot reload of the heartbeat interval is applied safely",
                 "v1 has no hot reload: configuration is read once at startup and a changed "
                 "HeartBtInt is rejected at the next Logon (C-15).")

SCENARIO_BLOCKED("K-13", "P0", "an out of memory kill is followed by a safe recovery",
                 "the recovery path is the same as any abrupt termination and is exercised by "
                 "the crash suite; forcing a real cgroup kill needs a container, not a unit test.")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "concurrency_lifecycle"); }
