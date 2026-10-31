// Group C of the what-if matrix: Logon, authentication and session identity.
// Design doc 15.4. The through-line is invariant I-10: a TCP connection is not
// a FIX session, and ACTIVE must not be reached without a validated handshake.
#include <string>

#include "engine_fixture.hpp"
#include "fix/wire.hpp"

using namespace fixse;

namespace {

std::string type_of(const std::string& frame) { return peer::get(frame, 35).value_or(""); }

bool any_of_type(std::vector<std::string> frames, const std::string& t) {
    for (const std::string& f : frames)
        if (type_of(f) == t) return true;
    return false;
}

}  // namespace

SCENARIO("C-01", "P0", "a valid handshake reaches ACTIVE and consumes one sequence each way") {
    tt::EngineFixture f("c01");
    CHECK_EQ(f.controller.connect_requests, 1);
    CHECK_EQ(f.engine->state(), SessionState::Connecting);

    f.connect();
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);
    const auto logon = f.out_of_type("A");
    CHECK(logon.has_value());
    std::string why;
    CHECK_MSG(peer::verify_frame(*logon, &why), why);
    CHECK_EQ(peer::get(*logon, 34).value_or(""), std::string("1"));
    CHECK_EQ(peer::get(*logon, 49).value_or(""), f.cfg.sender_comp_id);
    CHECK_EQ(peer::get(*logon, 56).value_or(""), f.cfg.target_comp_id);
    CHECK_EQ(peer::get(*logon, 98).value_or(""), std::string("0"));
    CHECK_EQ(peer::get(*logon, 108).value_or(""), std::string("30"));
    CHECK_EQ(f.engine->next_out(), uint64_t{2});

    f.feed(f.logon_reply(1));
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK(f.engine->ready());
    CHECK_EQ(f.engine->next_in(), uint64_t{2});
}

SCENARIO("C-02", "P0", "a Logon that is never answered times out and backs off") {
    tt::EngineFixture f("c02");
    f.connect();
    CHECK_EQ(f.engine->state(), SessionState::AwaitLogon);

    // One heartbeat interval is the handshake deadline.
    f.advance(30001);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK(!f.controller.disconnects.empty());
    CHECK(!f.engine->ready());
    // Sequence state is untouched by a failed handshake (invariant I-12).
    CHECK_EQ(f.engine->next_out(), uint64_t{2});
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
}

SCENARIO("C-03", "P0", "a Logout in place of a Logon acknowledgement never reaches ACTIVE") {
    tt::EngineFixture f("c03");
    f.connect();
    peer::Msg out = f.msg("5", 1);
    out.extra.push_back({58, "not authorised today"});
    f.feed(out.build());

    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK(!f.engine->ready());
    CHECK(f.audit.contains("peer Logout"));
}

SCENARIO("C-04", "P0", "a wrong SenderCompID is refused and audited") {
    tt::EngineFixture f("c04");
    f.connect();
    peer::Msg m = f.msg("A", 1);
    m.sender = "ATTACKER";
    m.extra.push_back({98, "0"});
    m.extra.push_back({108, "30"});
    f.feed(m.build());

    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.audit.contains("CompID mismatch"));
    // Nothing from an unauthorised peer advances inbound sequencing.
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
}

SCENARIO("C-05", "P0", "a wrong TargetCompID is refused and audited") {
    tt::EngineFixture f("c05");
    f.connect();
    peer::Msg m = f.msg("A", 1);
    m.target = "SOMEONE_ELSE";
    m.extra.push_back({98, "0"});
    m.extra.push_back({108, "30"});
    f.feed(m.build());

    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.audit.contains("CompID mismatch"));
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
}

SCENARIO("C-06", "P0", "repeated Logon rejection backs off instead of reconnecting in a loop") {
    tt::EngineFixture f("c06");
    uint64_t previous_delay = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        f.connect();
        peer::Msg out = f.msg("5", f.engine->next_in());
        out.extra.push_back({58, "bad credentials"});
        f.feed(out.build());
        CHECK_EQ(f.engine->state(), SessionState::Backoff);

        // No reconnect happens until the backoff has elapsed, and the delay
        // grows: a credential failure must not become a connect storm (A-12).
        const int before = f.controller.connect_requests;
        f.advance(1);
        CHECK_EQ(f.controller.connect_requests, before);

        uint64_t waited = 1;
        while (f.engine->state() == SessionState::Backoff && waited < 200000) {
            f.advance(500);
            waited += 500;
        }
        CHECK_EQ(f.engine->state(), SessionState::Connecting);
        CHECK(waited > previous_delay);
        previous_delay = waited > 1000 ? waited - 1000 : 0;
        ++f.generation;
    }
}

SCENARIO("C-08", "P1", "a second Logon on an established session is refused without a reset") {
    tt::EngineFixture f("c08");
    f.connect_and_logon();
    CHECK_EQ(f.engine->state(), SessionState::Active);
    const uint64_t in_before = f.engine->next_in();
    const uint64_t out_before = f.engine->next_out();
    const uint64_t epoch_before = f.engine->epoch();

    f.feed(f.logon_reply(in_before));
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.audit.contains("duplicate Logon"));
    // Refusing a duplicate Logon must never reset sequence state.
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK(f.engine->next_out() >= out_before);
}

SCENARIO("C-09", "P0", "application data before a Logon is never delivered") {
    tt::EngineFixture f("c09");
    f.connect();
    f.feed(f.app(1, "early-order"));

    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
    CHECK(f.audit.contains("received before Logon"));
}

SCENARIO("C-10", "P1", "an administrative message before a Logon does not authenticate") {
    tt::EngineFixture f("c10");
    f.connect();
    peer::Msg tr = f.msg("1", 1);
    tr.extra.push_back({112, "probe"});
    f.feed(tr.build());

    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(!f.engine->ready());
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
}

SCENARIO("C-11", "P0", "a Logon with a higher sequence establishes the session and asks for the gap") {
    tt::EngineFixture f("c11");
    f.connect();
    f.out();  // discard the outbound Logon

    // The peer believes it has already sent five messages we never saw.
    f.feed(f.logon_reply(6));

    // The handshake completes, but ACTIVE waits for the gap to close (I-10).
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
    CHECK(!f.engine->ready());
    const auto rr = f.out_of_type("2");
    CHECK(rr.has_value());
    CHECK_EQ(peer::get(*rr, 7).value_or(""), std::string("1"));
    CHECK_EQ(peer::get(*rr, 16).value_or(""), std::string("5"));
    // The Logon itself did not advance NextNumIn: sequence 1 is still missing.
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
    const auto gap = f.engine->open_gap();
    CHECK(gap.has_value());
    CHECK_EQ(gap->first, uint64_t{1});
    CHECK_EQ(gap->second, uint64_t{5});
}

SCENARIO("C-12", "P0", "a Logon with a lower sequence and no PossDup ends the session") {
    tt::EngineFixture f("c12");
    f.connect_and_logon();
    CHECK_EQ(f.engine->state(), SessionState::Active);
    f.out();

    // next_in is 2 by now; a Logon claiming sequence 1 again without
    // PossDupFlag means the peer rewound its own history.
    f.feed(f.logon_reply(1));
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(any_of_type(f.transport.frames(), "5"));
    CHECK(f.audit.contains("too low"));
    CHECK_EQ(f.engine->next_in(), uint64_t{2});
}

SCENARIO("C-13", "P0", "the engine does not send ResetSeqNumFlag unless configured") {
    tt::EngineFixture f("c13");
    f.connect();
    const auto logon = f.out_of_type("A");
    CHECK(logon.has_value());
    // Defaulting ResetOnLogon to Y is how sequence gaps get hidden; the frame
    // must not carry tag 141 at all.
    CHECK_EQ(peer::count_tag(*logon, 141), 0);
    CHECK(!f.cfg.allow_reset_on_logon);
}

SCENARIO("C-14", "P0", "an unsolicited peer reset request faults instead of losing history") {
    tt::EngineFixture f("c14");
    f.connect_and_logon();
    const uint64_t in_before = f.engine->next_in();
    const uint64_t out_before = f.engine->next_out();
    const uint64_t epoch_before = f.engine->epoch();
    f.out();

    // A peer asking for a reset mid-session, outside the agreement.
    f.feed(f.logon_reply(in_before, std::string("Y")));

    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK_EQ(f.engine->fault_reason(), FaultReason::UnauthorizedSequenceReset);
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK_EQ(f.engine->next_out(), out_before);
    // A faulted engine stops transmitting.
    CHECK_EQ(f.engine->pending_tx_bytes(), size_t{0});
}

SCENARIO("C-15", "P0", "a HeartBtInt outside the agreement is rejected") {
    for (const char* hb : {"0", "99999", "abc"}) {
        tt::EngineFixture f(std::string("c15-") + hb);
        f.connect();
        f.feed(f.logon_reply(1, std::nullopt, std::string(hb)));
        CHECK_MSG(f.engine->state() != SessionState::Active, std::string("hb=") + hb);
        CHECK_MSG(f.audit.contains("HeartBtInt mismatch"), std::string("hb=") + hb);
    }
    // The agreed value is accepted.
    tt::EngineFixture ok("c15-ok");
    ok.connect();
    ok.feed(ok.logon_reply(1, std::nullopt, std::string("30")));
    CHECK_EQ(ok.engine->state(), SessionState::Active);
}

SCENARIO("C-16", "P0", "Logon credentials never appear in audit output") {
    tt::EngineFixture f("c16", false);
    f.cfg.username = "trader01";
    f.cfg.password = "sup3r-s3cret-passphrase";
    f.start();
    f.connect();

    const auto logon = f.out_of_type("A");
    CHECK(logon.has_value());
    // The password is on the wire, because the counterparty requires it there.
    CHECK(logon->find(f.cfg.password) != std::string::npos);

    // It must not be anywhere in the audit trail, neither through a field nor
    // through a frame dump.
    CHECK(!f.audit.contains(f.cfg.password));
    const std::string dumped = f.audit.sanitize_frame(*logon);
    CHECK(dumped.find(f.cfg.password) == std::string::npos);
    CHECK(dumped.find("554=[REDACTED]") != std::string::npos);
    // A value that leaks through an unexpected tag is caught too.
    CHECK(f.audit.sanitize_value("text containing " + f.cfg.password)
              .find(f.cfg.password) == std::string::npos);
}

SCENARIO("C-17", "P0", "a restart during the handshake does not reuse the Logon sequence") {
    tt::EngineFixture f("c17");
    f.connect();
    const auto first = f.out_of_type("A");
    CHECK(first.has_value());
    CHECK_EQ(peer::get(*first, 34).value_or(""), std::string("1"));
    CHECK_EQ(f.engine->next_out(), uint64_t{2});

    // The process goes away before the peer ever answered.
    f.restart(false);
    CHECK_EQ(f.engine->next_out(), uint64_t{2});
    f.reconnect();

    const auto second = f.out_of_type("A");
    CHECK(second.has_value());
    // The second Logon is a NEW original message and takes the next sequence:
    // the engine never issues sequence 1 twice (invariants I-02, I-05).
    CHECK_EQ(peer::get(*second, 34).value_or(""), std::string("2"));
    CHECK(f.audit.contains("UncleanShutdownDetected") ||
          f.metrics.counter("fix_faults_total") == 0);
}

SCENARIO("C-18", "P0", "an engine cannot start when another owns the session store") {
    tt::EngineFixture first("c18");
    CHECK(first.store->owns_session());

    // A second engine on the same store path is refused ownership, so it never
    // reaches the point of sending a Logon (invariant I-01, playbook PB-09).
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

SCENARIO("C-19", "P1", "a configuration that claims unsupported behaviour fails validation") {
    SessionConfig c;
    c.sender_comp_id = "TRADER01";
    c.target_comp_id = "SIMLP01";
    // Asking the engine to carry a session MsgType as an application type, or
    // to use a transport it does not implement, must fail loudly rather than
    // pretend support (design doc 13, scenarios A-19, L-05).
    c.app_msg_types = {"U1", "A"};
    c.tls_enabled = true;
    const auto errors = validate(c);
    bool saw_session_type = false;
    bool saw_tls = false;
    for (const ConfigError& e : errors) {
        if (e.field == "app.app_msg_types") saw_session_type = true;
        if (e.field == "security.tls_enabled") saw_tls = true;
    }
    CHECK(saw_session_type);
    CHECK(saw_tls);

    SessionConfig good;
    good.sender_comp_id = "TRADER01";
    good.target_comp_id = "SIMLP01";
    CHECK_EQ(validate(good).size(), size_t{0});
}

SCENARIO("C-07", "P0", "an unauthorised CompID pair never reaches ACTIVE even if well formed") {
    tt::EngineFixture f("c07");
    f.connect();
    // Both ids are syntactically fine but neither matches the configuration.
    peer::Msg m = f.msg("A", 1);
    m.sender = "OTHER_LP";
    m.target = "OTHER_CLIENT";
    m.extra.push_back({98, "0"});
    m.extra.push_back({108, "30"});
    f.feed(m.build());
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(!f.engine->ready());
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "logon_identity"); }
