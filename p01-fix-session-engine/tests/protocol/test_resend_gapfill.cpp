// Group F of the what-if matrix at the engine level, plus playbooks PB-07 and
// PB-11. Design doc 15.7, 8.3 and 16.
//
// Two rules carry most of these cases: a retransmission reuses the original
// sequence number and never consumes a new one (invariant I-05), and a replay
// the archive cannot support fails closed instead of being papered over with a
// GapFill (invariant I-08).
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
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

// Outbound history: a Logon at sequence 1 followed by n application messages.
void seed_history(tt::MiniEngine& m, int app_messages) {
    m.connect_and_logon();
    for (int i = 0; i < app_messages; ++i)
        m.engine->submit_app_message("U1", {{20001, "hist-" + std::to_string(i)}});
    m.out();
}

std::string resend_request(tt::MiniEngine& m, uint64_t begin, uint64_t end) {
    peer::Msg rr = m.msg("2", m.store.state.next_in);
    rr.extra.push_back({7, std::to_string(begin)});
    rr.extra.push_back({16, std::to_string(end)});
    return rr.build();
}

std::vector<uint64_t> replayed_sequences(const std::vector<std::string>& frames) {
    std::vector<uint64_t> v;
    for (const std::string& f : frames)
        if (peer::get(f, 43).value_or("") == "Y" && type_of(f) != "4")
            v.push_back(std::stoull(peer::get(f, 34).value()));
    return v;
}

}  // namespace

SCENARIO("F-01e", "P0", "a resend request is answered with replays of the original sequences") {
    tt::MiniEngine m(1, 1);
    seed_history(m, 3);  // sequence 1 = Logon, 2..4 = application
    const uint64_t out_after = m.store.state.next_out;

    m.feed(resend_request(m, 2, 4));
    const auto frames = m.transport.new_frames();
    const auto seqs = replayed_sequences(frames);
    CHECK_EQ(seqs.size(), size_t{3});
    CHECK_EQ(seqs[0], uint64_t{2});
    CHECK_EQ(seqs[2], uint64_t{4});
    for (const std::string& f : frames) {
        if (peer::get(f, 43).value_or("") != "Y") continue;
        std::string why;
        CHECK_MSG(peer::verify_frame(f, &why), why);
        CHECK(peer::get(f, 122).has_value());
    }
    // Retransmissions do not consume new sequence numbers (invariant I-05).
    CHECK_EQ(m.store.state.next_out, out_after);
    CHECK_EQ(m.store.audit_count_of(AuditEvent::ResendServed), 1);
}

SCENARIO("F-10", "P0", "every replayed frame carries a recomputed BodyLength and CheckSum") {
    tt::MiniEngine m(1, 1);
    seed_history(m, 4);
    m.feed(resend_request(m, 2, 5));

    int checked = 0;
    for (const std::string& f : m.transport.new_frames()) {
        if (peer::get(f, 43).value_or("") != "Y" || type_of(f) == "4") continue;
        ++checked;
        std::string why;
        CHECK_MSG(peer::verify_frame(f, &why), why + " frame=" + tt::wire_text(f));
        // SendingTime is fresh while OrigSendingTime preserves the original.
        CHECK(peer::get(f, 122).has_value());
    }
    CHECK_EQ(checked, 4);
}

SCENARIO("F-12", "P0", "a repeated resend request is served again without recursion") {
    tt::MiniEngine m(1, 1);
    seed_history(m, 2);

    for (int i = 0; i < 2; ++i) {
        m.feed(resend_request(m, 2, 3));
        const auto frames = m.transport.new_frames();
        // Each request is answered, and none of them triggers a resend request
        // of our own, which is what a recursive implementation would do.
        CHECK_EQ(count_type(frames, "2"), 0);
        CHECK_EQ(replayed_sequences(frames).size(), size_t{2});
    }
    CHECK_NE(m.engine->state(), SessionState::Faulted);
}

SCENARIO("F-13", "P1", "overlapping resend ranges stay consistent with the archive") {
    tt::MiniEngine m(1, 1);
    seed_history(m, 5);

    m.feed(resend_request(m, 2, 4));
    const auto a = replayed_sequences(m.transport.new_frames());
    m.feed(resend_request(m, 3, 6));
    const auto b = replayed_sequences(m.transport.new_frames());

    CHECK_EQ(a.size(), size_t{3});
    CHECK_EQ(b.size(), size_t{4});
    // The same sequence replayed twice keeps its sequence number: history is a
    // fact, not a cursor.
    CHECK_EQ(a[1], uint64_t{3});
    CHECK_EQ(b[0], uint64_t{3});
}

SCENARIO("F-18", "P0", "a GapFill that does not move forward is rejected") {
    tt::EngineFixture f("f18");
    f.connect_and_logon();
    f.out();
    const uint64_t expected = f.engine->next_in();

    peer::Msg gf = f.msg("4", expected);
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, std::to_string(expected)});  // NewSeqNo == expected
    f.feed(gf.build());

    CHECK_EQ(f.engine->next_in(), expected);
    CHECK_EQ(count_type(f.transport.new_frames(), "3"), 1);
    CHECK(f.audit.contains("not ahead of NextNumIn"));
    CHECK_NE(f.engine->state(), SessionState::Faulted);
}

SCENARIO("F-19", "P0", "a GapFill pointing backwards never rolls the sequence back") {
    tt::EngineFixture f("f19");
    f.connect_and_logon();
    for (int i = 0; i < 3; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t expected = f.engine->next_in();
    f.out();

    peer::Msg gf = f.msg("4", expected);
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, "2"});
    f.feed(gf.build());

    CHECK_EQ(f.engine->next_in(), expected);
    CHECK_EQ(count_type(f.transport.new_frames(), "3"), 1);
}

SCENARIO("PB-07", "P0", "under a fail-closed agreement a skipping GapFill is quarantined") {
    tt::EngineFixture f("pb07", false);
    // The playbook configures the agreement that forbids skipping.
    f.cfg.gapfill_policy = GapFillPolicy::FailClosed;
    f.start();
    f.connect_and_logon();
    const uint64_t required = f.engine->next_in();
    f.out();

    // The peer claims sequences [required .. required+9] can simply be skipped.
    peer::Msg gf = f.msg("4", required);
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, std::to_string(required + 10)});
    f.feed(gf.build());

    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK_EQ(f.engine->fault_reason(), FaultReason::GapFillOverApplicationMessage);
    // The engine never claims those sequences were delivered.
    CHECK_EQ(f.engine->next_in(), required);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->skipped_unrecovered(), uint64_t{10});
    CHECK(f.metrics.gauge(metric::kUnresolvedGaps) >= int64_t{10});
    CHECK(f.audit.contains("GapFillQuarantined"));
}

SCENARIO("F-20", "P0", "under the default policy a skipping GapFill is applied but flagged") {
    tt::EngineFixture f("f20");
    CHECK_EQ(f.cfg.gapfill_policy, GapFillPolicy::AcceptAndFlag);
    f.connect_and_logon();
    const uint64_t from = f.engine->next_in();

    peer::Msg gf = f.msg("4", from);
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, std::to_string(from + 4)});
    f.feed(gf.build());

    // Recovery proceeds, but the four skipped sequences are recorded as never
    // recovered and stay in the metric for an operator to reconcile.
    CHECK_EQ(f.engine->next_in(), from + 4);
    CHECK_EQ(f.engine->skipped_unrecovered(), uint64_t{4});
    CHECK_EQ(f.metrics.gauge(metric::kUnresolvedGaps), int64_t{4});
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK(f.audit.contains("never recovered"));
}

SCENARIO("F-21", "P0", "an unsolicited SequenceReset-Reset is refused and audited") {
    tt::EngineFixture f("f21");
    f.connect_and_logon();
    for (int i = 0; i < 2; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t in_before = f.engine->next_in();
    f.out();

    peer::Msg rst = f.msg("4", 999);  // the Reset form ignores MsgSeqNum
    rst.extra.push_back({123, "N"});
    rst.extra.push_back({36, "1"});
    f.feed(rst.build());

    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK_EQ(f.engine->fault_reason(), FaultReason::UnauthorizedSequenceReset);
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK(f.audit.contains("not permitted by configuration"));
}

SCENARIO("F-21b", "P1", "a SequenceReset-Reset is applied when the agreement allows it") {
    tt::EngineFixture f("f21b", false);
    f.cfg.allow_unsolicited_seq_reset = true;
    f.start();
    f.connect_and_logon();
    const uint64_t in_before = f.engine->next_in();

    peer::Msg rst = f.msg("4", 777);
    rst.extra.push_back({123, "N"});
    rst.extra.push_back({36, std::to_string(in_before + 50)});
    f.feed(rst.build());

    CHECK_EQ(f.engine->next_in(), in_before + 50);
    CHECK_NE(f.engine->state(), SessionState::Faulted);
    CHECK(f.audit.contains("explicit configuration allowance"));

    // Even when allowed, it has to move forward.
    peer::Msg back = f.msg("4", 778);
    back.extra.push_back({123, "N"});
    back.extra.push_back({36, "2"});
    f.feed(back.build());
    CHECK_EQ(f.engine->next_in(), in_before + 50);
}

SCENARIO("F-24", "P0", "a gap the peer never fills ends in an escalated fault") {
    tt::EngineFixture f("f24");
    f.connect_and_logon();
    f.out();
    const uint64_t missing = f.engine->next_in();
    f.feed(f.app(missing + 1, "ahead"));
    CHECK(f.engine->open_gap().has_value());

    // The peer stays silent. The engine re-requests a bounded number of times
    // and then escalates rather than retrying for ever.
    for (int i = 0; i < 60 && f.engine->state() != SessionState::Faulted; ++i) f.advance(1000);

    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK(f.engine->fault_reason() == FaultReason::RecoveryTimeout ||
          f.engine->fault_reason() == FaultReason::ResendStorm);
    CHECK(f.engine->resend_requests_sent() <=
          static_cast<uint64_t>(f.cfg.max_resend_cycles) + 1);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
}

SCENARIO("PB-11", "P0", "a resend storm from the peer is bounded and then quarantined") {
    tt::MiniEngine m(1, 1);
    seed_history(m, 3);
    const int limit = m.cfg.max_resend_cycles;

    for (int i = 0; i < limit + 3 && m.engine->state() != SessionState::Faulted; ++i)
        m.feed(resend_request(m, 2, 4));

    CHECK_EQ(m.engine->state(), SessionState::Faulted);
    CHECK_EQ(m.engine->fault_reason(), FaultReason::ResendStorm);
    CHECK_EQ(m.store.audit_count_of(AuditEvent::ResendStormBlocked), 1);
    // Bounded work: the engine stopped serving rather than amplifying.
    CHECK(m.metrics.counter(metric::kResendMessages) <=
          static_cast<uint64_t>(3 * (limit + 1)));
}

SCENARIO("F-25", "P1", "a replay whose OrigSendingTime is in the future is rejected") {
    tt::EngineFixture f("f25");
    f.connect_and_logon();
    f.out();
    const uint64_t seq = f.engine->next_in();

    peer::Msg bad = f.msg("U1", seq);
    bad.poss_dup = true;
    bad.orig_sending_time = format_utc_timestamp(f.clock.wall_now() + std::chrono::seconds(60));
    bad.extra.push_back({20001, "impossible replay"});
    f.feed(bad.build());

    CHECK_EQ(count_type(f.transport.new_frames(), "3"), 1);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->next_in(), seq);
    CHECK(f.audit.contains("OrigSendingTime is later"));
}

SCENARIO("F-26", "P1", "session replay and application resend are different things") {
    tt::EngineFixture f("f26");
    f.connect_and_logon();
    const uint64_t seq = f.engine->next_in();

    // PossResend(97)=Y is an application level statement. The session layer
    // passes it through untouched: deduplicating it belongs to the order layer
    // (invariant I-11, playbook PB-13).
    peer::Msg resent = f.msg("U1", seq);
    resent.extra.push_back({97, "Y"});
    resent.extra.push_back({20001, "ORDER-123"});
    f.feed(resent.build());

    CHECK_EQ(f.sink.delivered.size(), size_t{1});
    CHECK(f.sink.payloads[0].find("97=Y") != std::string::npos);

    // A session level duplicate of the same sequence is suppressed here.
    peer::Msg dup = f.msg("U1", seq);
    dup.poss_dup = true;
    dup.orig_sending_time = format_utc_timestamp(f.clock.wall_now());
    dup.extra.push_back({97, "Y"});
    dup.extra.push_back({20001, "ORDER-123"});
    f.feed(dup.build());
    CHECK_EQ(f.sink.delivered.size(), size_t{1});

    // The same business id at a NEW sequence is a different session message and
    // is delivered: the engine must not pretend to solve business duplication.
    f.feed(f.app(f.engine->next_in(), "ORDER-123"));
    CHECK_EQ(f.sink.delivered.size(), size_t{2});
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "resend_gapfill"); }
