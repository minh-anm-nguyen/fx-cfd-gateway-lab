// Group J of the what-if matrix: security and an adversarial peer.
// Design doc 15.11 and 11.2.
#include <fstream>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/wire.hpp"

using namespace fixse;

SCENARIO("J-02", "P0", "the engine never claims that a CompID match is authentication") {
    tt::EngineFixture f("j02");
    // Recorded once at every start of a plain TCP session, so an operator
    // cannot mistake CompID validation for authentication.
    CHECK(f.audit.contains("TransportNotAuthenticated"));
    CHECK(f.audit.contains("not authentication"));
    CHECK(!f.cfg.tls_enabled);
}

SCENARIO("J-03", "P0", "a secret never reaches a log line, a frame dump or free text") {
    tt::EngineFixture f("j03", false);
    f.cfg.username = "trader01";
    f.cfg.password = "pa55w0rd-do-not-log";
    f.start();
    f.connect();
    const auto logon = f.out_of_type("A");
    CHECK(logon.has_value());

    CHECK(!f.audit.contains(f.cfg.password));
    CHECK(f.audit.sanitize_frame(*logon).find(f.cfg.password) == std::string::npos);
    // Even when it arrives through an unexpected field, such as a peer echoing
    // it back in tag 58.
    const std::string echoed = f.audit.sanitize_value("peer said " + f.cfg.password);
    CHECK(echoed.find(f.cfg.password) == std::string::npos);
    CHECK(echoed.find("[REDACTED]") != std::string::npos);
}

SCENARIO("J-04", "P0", "an oversized declared length is rejected before any allocation") {
    tt::EngineFixture f("j04", false);
    f.cfg.max_frame_bytes = 4096;
    f.cfg.max_read_buffer_bytes = 8192;
    f.start();
    f.connect_and_logon();

    // A header claiming a gigabyte of body.
    std::string attack = std::string("8=FIX.4.4") + kSOH + "9=1000000000" + kSOH + "35=U1" + kSOH;
    f.feed_bytes(attack);
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK(f.metrics.counter(metric::kMalformedFrames) >= uint64_t{1});
    CHECK(f.metrics.gauge(metric::kRxBufferBytes) <=
          static_cast<int64_t>(f.cfg.max_read_buffer_bytes));
}

SCENARIO("J-05", "P0", "a frame dripped one byte at a time hits the incomplete frame deadline") {
    tt::EngineFixture f("j05", false);
    f.cfg.partial_frame_deadline_ms = 5000;
    f.start();
    f.connect_and_logon();

    const std::string frame = f.app(f.engine->next_in(), "slow");
    for (size_t i = 0; i < frame.size() / 2; ++i) {
        f.feed_bytes(frame.substr(i, 1));
        f.advance(10);
    }
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    f.advance(6000);
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    CHECK(f.audit.contains("incomplete frame deadline"));
}

SCENARIO("J-06", "P0", "resend amplification is capped") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    for (int i = 0; i < 50; ++i) m.engine->submit_app_message("U1", {{20001, "payload"}});
    m.out();

    const int limit = m.cfg.max_resend_cycles;
    for (int i = 0; i < limit + 5 && m.engine->state() != SessionState::Faulted; ++i) {
        peer::Msg rr = m.msg("2", m.store.state.next_in);
        rr.extra.push_back({7, "1"});
        rr.extra.push_back({16, "0"});
        m.feed(rr.build());
    }
    CHECK_EQ(m.engine->state(), SessionState::Faulted);
    CHECK_EQ(m.engine->fault_reason(), FaultReason::ResendStorm);
    // Bytes emitted stay proportional to the cap, not to the attacker effort.
    CHECK(m.metrics.counter(metric::kResendMessages) <= static_cast<uint64_t>(50 * (limit + 1)));
}

SCENARIO("J-07", "P0", "a forged ResetSeqNumFlag cannot reset the session") {
    tt::EngineFixture f("j07");
    f.connect_and_logon();
    for (int i = 0; i < 3; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t in_before = f.engine->next_in();
    const uint64_t out_before = f.engine->next_out();

    f.feed(f.logon_reply(in_before, std::string("Y")));
    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK_EQ(f.engine->fault_reason(), FaultReason::UnauthorizedSequenceReset);
    CHECK_EQ(f.engine->epoch(), uint64_t{1});
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK_EQ(f.engine->next_out(), out_before);
}

SCENARIO("J-08", "P0", "a captured Logon replayed later is refused on sequence and time") {
    tt::EngineFixture f("j08");
    f.connect_and_logon();
    const std::string captured = f.logon_reply(1);
    for (int i = 0; i < 2; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t in_before = f.engine->next_in();

    // The attacker replays the recorded Logon verbatim, much later.
    f.clock.advance(Millis(600000));
    f.feed(captured);
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->next_in(), in_before);
    // Either the stale SendingTime or the low sequence catches it, and both are
    // recorded.
    CHECK(f.audit.contains("SendingTime outside tolerance") || f.audit.contains("too low"));
}

SCENARIO("J-09", "P1", "control characters in free text cannot forge an audit line") {
    tt::EngineFixture f("j09");
    f.connect_and_logon();

    std::string nasty = "ok";
    nasty.push_back(10);
    nasty += "event=Faulted reason=injected";
    nasty.push_back(13);
    const std::string cleaned = f.audit.sanitize_value(nasty);
    CHECK(cleaned.find(static_cast<char>(10)) == std::string::npos);
    CHECK(cleaned.find(static_cast<char>(13)) == std::string::npos);
    // And the value is bounded, so a megabyte of text cannot flood the log.
    const std::string huge(100000, 'A');
    CHECK(f.audit.sanitize_value(huge).size() <= f.cfg.max_audit_value_bytes + 8);
}

SCENARIO("J-11", "P0", "sequence number edge values are bounds checked") {
    // Length and tag boundaries are covered by B-07, B-08 and B-21 at the codec
    // level; here the engine itself is fed the sequence boundary values.
    const char* seqs[] = {"0", "18446744073709551616", "99999999999999999999999"};
    for (int i = 0; i < 3; ++i) {
        tt::EngineFixture f("j11-" + std::to_string(i));
        f.connect_and_logon();
        const uint64_t in_before = f.engine->next_in();
        std::vector<peer::Field> fields{{35, "U1"},
                                        {49, f.cfg.target_comp_id},
                                        {56, f.cfg.sender_comp_id},
                                        {34, seqs[i]},
                                        {52, format_utc_timestamp(f.clock.wall_now())},
                                        {20001, "edge"}};
        f.feed(peer::build_from_fields(fields));
        CHECK_EQ(f.sink.delivered.size(), size_t{0});
        CHECK_EQ(f.engine->next_in(), in_before);
    }
}

SCENARIO("J-12", "P1", "a burst of many small messages keeps state consistent") {
    tt::EngineFixture f("j12");
    f.connect_and_logon();
    const uint64_t base = f.engine->next_in();

    std::string burst;
    for (uint64_t i = 0; i < 2000; ++i) burst += f.app(base + i, "x");
    // Delivered the way a socket delivers: in reads of at most a few kilobytes.
    // A single write larger than the configured receive buffer is refused by
    // design (scenario I-02), so the burst is chunked here.
    for (size_t pos = 0; pos < burst.size(); pos += 8192)
        f.feed_bytes(std::string_view(burst).substr(pos, 8192));

    CHECK_EQ(f.sink.delivered.size(), size_t{2000});
    CHECK_EQ(f.sink.delivered.front(), base);
    CHECK_EQ(f.sink.delivered.back(), base + 1999);
    CHECK_EQ(f.engine->next_in(), base + 2000);
    CHECK(f.metrics.gauge(metric::kRxBufferBytes) <=
          static_cast<int64_t>(f.cfg.max_read_buffer_bytes));
}

SCENARIO("J-14", "P1", "a password written into the config file is refused") {
    // The schema accepts only the NAME of an environment variable, so a secret
    // committed into the file is an unknown key rather than a working setting
    // (design doc 11.2).
    tt::TempDir d("j14");
    const std::string path = d.file("session.toml");
    {
        std::ofstream out(path, std::ios::binary);
        out << "[session]" << std::endl;
        out << "sender_comp_id = " << static_cast<char>(34) << "TRADER01" << static_cast<char>(34)
            << std::endl;
        out << "target_comp_id = " << static_cast<char>(34) << "SIMLP01" << static_cast<char>(34)
            << std::endl;
        out << "[security]" << std::endl;
        out << "password = " << static_cast<char>(34) << "secret-in-a-file"
            << static_cast<char>(34) << std::endl;
    }
    SessionConfig cfg;
    std::vector<ConfigError> errors;
    CHECK(!load_config_file(path, &cfg, &errors));
    bool saw = false;
    for (const ConfigError& e : errors)
        if (e.field == "security.password") saw = true;
    CHECK(saw);
    CHECK(cfg.password.empty());
}

SCENARIO("J-15", "P0", "a sequence reset without two approvers is refused and audited") {
    tt::EngineFixture f("j15");
    f.connect_and_logon();
    const uint64_t epoch_before = f.engine->epoch();

    CHECK(!f.engine->operator_coordinated_reset("", "", "no approval"));
    CHECK(!f.engine->operator_coordinated_reset("alice", "", "one approver"));
    CHECK(!f.engine->operator_coordinated_reset("alice", "alice", "same person twice"));
    CHECK_EQ(f.engine->epoch(), epoch_before);
    CHECK(f.audit.contains("SequenceResetRefused"));

    CHECK(f.engine->operator_coordinated_reset("alice", "bob", "agreed weekly reset"));
    CHECK_EQ(f.engine->epoch(), epoch_before + 1);
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
    CHECK_EQ(f.engine->next_out(), uint64_t{1});
    CHECK(f.audit.contains("CoordinatedResetApplied"));
}

SCENARIO_BLOCKED("J-01", "P0", "an impostor peer is rejected by TLS identity",
                 "TLS is out of v1 scope. Startup refuses tls_enabled rather than silently "
                 "running plaintext (C-19), and J-02 records that CompID is not authentication.")

SCENARIO_BLOCKED("J-13", "P0", "an expired certificate chain never falls back to plaintext",
                 "same reason as J-01: there is no TLS stack in v1 to present a bad chain to.")

SCENARIO_BLOCKED("J-10", "P0", "a million fuzz iterations find no crash or undefined behaviour",
                 "covered by the fuzz_parse_frame suite, which runs the corpus and the mutation "
                 "loop and reports there rather than duplicating the run here.")

#ifdef FIXSE_UBSAN_TRAP_BUILD
SCENARIO("SAN-01", "P0", "the whole suite passes with trapping undefined behaviour checks") {
    // Reaching this line means the binary was built with
    // -fsanitize=undefined -fsanitize-undefined-trap-on-error and
    // _GLIBCXX_ASSERTIONS, and that no check has trapped so far. A trap is an
    // illegal instruction, so it would abort the process rather than fail an
    // assertion.
    CHECK(true);
}
#else
SCENARIO_BLOCKED("SAN-01", "P0",
                 "the whole suite passes with trapping undefined behaviour checks",
                 "this build has no undefined behaviour checks. Run "
                 "scripts/run_sanitizers.sh --ubsan-trap, which configures build-ubtrap with "
                 "FIXSE_UBSAN_TRAP=ON and runs every suite under it.")
#endif

SCENARIO_BLOCKED("SAN-02", "P0", "AddressSanitizer reports nothing across the suite",
                 "this toolchain ships no libasan: mingw-w64 GCC has no AddressSanitizer "
                 "runtime, so -fsanitize=address does not link. The CMake option FIXSE_ASAN and "
                 "scripts/run_sanitizers.sh --asan are in place for the Linux target the design "
                 "doc names; SAN-01 is the undefined behaviour evidence available here.")

SCENARIO_BLOCKED("SAN-03", "P1", "ThreadSanitizer reports nothing across the suite",
                 "same missing runtime as SAN-02, and the engine is single threaded by "
                 "construction, so there is little for TSan to find in v1. The option is kept "
                 "for the Linux target and for the threaded work in Project 07.")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "security"); }
