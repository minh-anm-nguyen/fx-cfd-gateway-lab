// Group L of the what-if matrix: interoperability and the application
// boundary. Design doc 15.13 and 12.2.
//
// The rule under test is invariant I-11: FIX session recovery is not
// application level exactly-once, and the engine must never pretend otherwise.
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/wire.hpp"

using namespace fixse;

SCENARIO("L-02", "P0", "a peer asking for a resend after our restart gets a compliant reply") {
    tt::EngineFixture f("l02");
    f.connect_and_logon();
    for (int i = 0; i < 3; ++i)
        CHECK_EQ(f.engine->submit_app_message("U1", {{20001, "order"}}), SubmitResult::Accepted);
    const uint64_t last_original = f.engine->next_out() - 1;

    // We restart, reconnect, and the peer then asks for everything it missed.
    f.restart();
    f.reconnect();
    f.feed(f.logon_reply(f.engine->next_in()));
    f.out();

    peer::Msg rr = f.msg("2", f.engine->next_in());
    // From sequence 1, so the range covers our Logon as well as the three
    // application messages: the Logon has to be gap filled, not replayed.
    rr.extra.push_back({7, "1"});
    rr.extra.push_back({16, std::to_string(last_original)});
    f.feed(rr.build());

    int replays = 0;
    int gapfills = 0;
    for (const std::string& fr : f.transport.new_frames()) {
        const std::string type = peer::get(fr, 35).value_or("");
        if (type == "4") {
            ++gapfills;
            CHECK_EQ(peer::get(fr, 123).value_or(""), std::string("Y"));
            CHECK(peer::get(fr, 36).has_value());
            continue;
        }
        if (peer::get(fr, 43).value_or("") != "Y") continue;
        ++replays;
        // Every replay carries the original sequence, PossDupFlag and
        // OrigSendingTime, with a recomputed length and checksum.
        CHECK(peer::get(fr, 122).has_value());
        std::string why;
        CHECK_MSG(peer::verify_frame(fr, &why), why);
    }
    CHECK_EQ(replays, 3);
    CHECK(gapfills >= 1);  // the Logon we sent is covered by a GapFill
}

SCENARIO("L-04", "P1", "the negotiated open ended resend mode is honoured") {
    tt::EngineFixture f("l04", false);
    f.cfg.resend_mode = ResendMode::OpenEnded;
    f.start();
    f.connect_and_logon();
    f.out();

    f.feed(f.app(f.engine->next_in() + 2, "ahead"));
    const auto rr = f.out_of_type("2");
    CHECK(rr.has_value());
    // Open ended agreements use EndSeqNo=0 instead of a bounded range.
    CHECK_EQ(peer::get(*rr, 16).value_or(""), std::string("0"));

    tt::EngineFixture bounded("l04b");
    bounded.connect_and_logon();
    bounded.out();
    bounded.feed(bounded.app(bounded.engine->next_in() + 2, "ahead"));
    const auto rr2 = bounded.out_of_type("2");
    CHECK(rr2.has_value());
    CHECK_NE(peer::get(*rr2, 16).value_or(""), std::string("0"));
}

SCENARIO("L-06", "P0", "a lost delivery acknowledgement leads to at-least-once redelivery") {
    tt::EngineFixture f("l06");
    f.connect_and_logon();
    f.sink.refuse = true;  // the sink took it but the acknowledgement was lost

    const uint64_t seq = f.engine->next_in();
    f.feed(f.app(seq, "business event"));
    // Sequencing advanced because the message is durably committed, but the
    // event is still pending delivery.
    CHECK_EQ(f.engine->next_in(), seq + 1);
    CHECK_EQ(f.engine->pending_inbox(), size_t{1});
    CHECK_EQ(f.sink.delivered.size(), size_t{0});

    f.sink.refuse = false;
    f.tick();
    CHECK_EQ(f.sink.delivered.size(), size_t{1});
    CHECK_EQ(f.sink.delivered[0], seq);
    // The consumer saw it twice overall, which is why it has to be idempotent.
    CHECK(f.sink.attempts >= 2);
}

SCENARIO("L-07", "P0", "the application cannot send while the session is recovering") {
    tt::EngineFixture f("l07");
    f.connect_and_logon();
    CHECK_EQ(f.engine->submit_app_message("U1", {{20001, "fine"}}), SubmitResult::Accepted);

    f.feed(f.app(f.engine->next_in() + 1, "ahead"));
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
    CHECK_EQ(f.engine->submit_app_message("U1", {{20001, "too early"}}),
             SubmitResult::RejectedNotActive);

    f.feed(f.app(f.engine->next_in(), "fills the hole"));
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->submit_app_message("U1", {{20001, "fine again"}}), SubmitResult::Accepted);
}

SCENARIO("L-08", "P0", "the session layer never invents a business retry") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    CHECK_EQ(m.engine->submit_app_message("U1", {{20001, "ORDER-1"}}), SubmitResult::Accepted);
    const uint64_t out_after_order = m.store.state.next_out;
    m.out();

    // No answer ever comes. The engine probes liveness and eventually gives up
    // on the connection, but it never re-sends the application message as a new
    // original: that decision belongs to the order layer (playbook PB-13).
    for (int i = 0; i < 10; ++i) m.advance(20000);

    int app_originals = 0;
    for (const std::string& f : m.transport.frames()) {
        if (peer::get(f, 35).value_or("") != "U1") continue;
        if (peer::get(f, 43).value_or("") == "Y") continue;
        ++app_originals;
    }
    // Exactly the one original the application asked for, and no second one
    // conjured by a timeout.
    CHECK_EQ(app_originals, 1);
    CHECK_EQ(m.store.outbound.count(out_after_order - 1), size_t{1});
    // Later sequence numbers were spent on heartbeats and test requests, never
    // on a second copy of the application message.
    for (const auto& kv : m.store.outbound) {
        if (kv.first < out_after_order) continue;
        CHECK_MSG(kv.second.msg_type != "U1",
                  "sequence " + std::to_string(kv.first) + " re-sent an application message");
    }
}

SCENARIO("L-10", "P1", "a domain level rejection by the sink does not stall sequencing") {
    tt::EngineFixture f("l10");
    f.connect_and_logon();
    f.sink.refuse = true;

    const uint64_t seq = f.engine->next_in();
    f.feed(f.app(seq, "rejected by the domain"));
    // The session advanced because the message was received and committed; the
    // application problem is handled on the application side.
    CHECK_EQ(f.engine->next_in(), seq + 1);
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->pending_inbox(), size_t{1});
}

SCENARIO("L-11", "P0", "an accepted submission is not a claim that the peer received it") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    // The socket refuses to take any bytes at all.
    m.transport.would_block = true;
    const size_t written_before = m.transport.written.size();

    const SubmitResult r = m.engine->submit_app_message("U1", {{20001, "ORDER-9"}});
    CHECK_EQ(r, SubmitResult::Accepted);
    // Accepted means durably committed and queued, nothing more: not one byte
    // reached the peer. There is deliberately no API that reports delivery.
    CHECK_EQ(m.transport.written.size(), written_before);
    CHECK(m.engine->pending_tx_bytes() > size_t{0});
    CHECK_EQ(m.store.outbound.count(m.store.state.next_out - 1), size_t{1});
}

SCENARIO("L-12", "P1", "golden fixtures keep byte for byte stability across builds") {
    // A frame built from fixed inputs must serialise identically for ever: this
    // is what catches an accidental change of field order, padding or
    // timestamp precision after a toolchain or library upgrade.
    OutboundFields f;
    f.msg_type = "A";
    f.seq_num = 1;
    f.sender_comp_id = "TRADER01";
    f.target_comp_id = "SIMLP01";
    f.sending_time = WallTime{} + std::chrono::seconds(1793336400);
    f.body.emplace_back(tag::EncryptMethod, "0");
    f.body.emplace_back(tag::HeartBtInt, "30");
    const std::string logon = encode(f);

    const std::string expected_printable =
        "8=FIX.4.4|9=70|35=A|49=TRADER01|56=SIMLP01|34=1|52=20261030-05:00:00.000|98=0|108=30|"
        "10=233|";
    CHECK_EQ(tt::wire_text(logon), expected_printable);
    // The real delimiter is SOH, not the vertical bar used for printing.
    CHECK(logon.find('|') == std::string::npos);
    CHECK_EQ(logon[9], kSOH);

    std::string why;
    CHECK_MSG(peer::verify_frame(logon, &why), why);
}

SCENARIO("L-14", "P0", "only a pre-agreed reset creates a new epoch across a restart") {
    tt::EngineFixture f("l14");
    f.connect_and_logon();
    for (int i = 0; i < 2; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t in_before = f.engine->next_in();

    // A restart at the virtual session cutover: no reset, so the epoch stands.
    f.clock.jump_wall(std::chrono::seconds(3600 * 20));
    f.restart();
    CHECK_EQ(f.engine->epoch(), uint64_t{1});
    CHECK_EQ(f.engine->next_in(), in_before);

    // The agreed reset, with two approvers, is the only thing that starts a new
    // epoch, and it survives a restart.
    CHECK(f.engine->operator_coordinated_reset("alice", "bob", "agreed weekly reset"));
    CHECK_EQ(f.engine->epoch(), uint64_t{2});
    f.restart();
    CHECK_EQ(f.engine->epoch(), uint64_t{2});
    CHECK_EQ(f.engine->next_in(), uint64_t{1});
    CHECK_EQ(f.engine->next_out(), uint64_t{1});
}

SCENARIO_BLOCKED("L-01", "P0", "a QuickFIX acceptor completes Logon, heartbeat and logout",
                 "QuickFIX/C++ is not available in this environment, and the design doc is "
                 "explicit that a peer you wrote yourself cannot prove your own compliance. "
                 "The loopback scenario e2e-loopback is the nearest available evidence: a real "
                 "TCP round trip against a separately written wire implementation.")

SCENARIO_BLOCKED("L-03", "P0", "a QuickFIX peer logging on with a higher sequence recovers",
                 "same dependency as L-01. The behaviour itself is covered against the scripted "
                 "peer by C-11 and E-11.")

SCENARIO_BLOCKED("L-05", "P0", "a peer without the requested TLS feature is not downgraded",
                 "TLS is out of v1 scope: startup refuses tls_enabled outright (C-19) instead of "
                 "negotiating down to plaintext.")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "interop_boundary"); }
