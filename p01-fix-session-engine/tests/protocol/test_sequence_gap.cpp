// Group E of the what-if matrix: sequence ordering, duplicates and gap
// handling, plus playbook PB-05. Design doc 15.6, 8.2 and 16.
//
// The invariants on trial here are I-02 (sequences never regress), I-04
// (ordered, exactly-once application delivery) and I-09 (bounded buffers).
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

}  // namespace

SCENARIO("E-01", "P0", "an in-sequence message is committed and delivered exactly once") {
    tt::EngineFixture f("e01");
    f.connect_and_logon();
    const uint64_t expected = f.engine->next_in();

    f.feed(f.app(expected, "one"));
    CHECK_EQ(f.engine->next_in(), expected + 1);
    CHECK_EQ(f.sink.delivered.size(), size_t{1});
    CHECK_EQ(f.sink.delivered[0], expected);
    CHECK_EQ(f.metrics.counter(metric::kAppDelivered), uint64_t{1});
}

SCENARIO("E-02", "P0", "a missing sequence is requested and the later message is withheld") {
    tt::EngineFixture f("e02");
    f.connect_and_logon();
    f.out();
    const uint64_t missing = f.engine->next_in();

    f.feed(f.app(missing + 1, "ahead"));
    const auto rr = f.out_of_type("2");
    CHECK(rr.has_value());
    CHECK_EQ(peer::get(*rr, 7).value_or(""), std::to_string(missing));
    CHECK_EQ(peer::get(*rr, 16).value_or(""), std::to_string(missing));
    // The later message is held, not delivered out of order.
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->next_in(), missing);
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
    CHECK(!f.engine->ready());

    // Filling the hole releases both, in order.
    f.feed(f.app(missing, "the missing one"));
    CHECK_EQ(f.sink.delivered.size(), size_t{2});
    CHECK_EQ(f.sink.delivered[0], missing);
    CHECK_EQ(f.sink.delivered[1], missing + 1);
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("E-03", "P0", "a sequence a hundred thousand ahead costs one buffer slot") {
    tt::EngineFixture f("e03");
    f.connect_and_logon();
    const uint64_t expected = f.engine->next_in();

    f.feed(f.app(expected + 100000, "far ahead"));
    // One buffered frame, not one per missing sequence number.
    CHECK_EQ(f.engine->future_buffer_size(), size_t{1});
    CHECK(f.engine->future_buffer_bytes() < size_t{4096});
    const auto gap = f.engine->open_gap();
    CHECK(gap.has_value());
    CHECK_EQ(gap->first, expected);
    CHECK_EQ(gap->second, expected + 99999);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
}

SCENARIO("E-04", "P0", "a low sequence without PossDup ends the session") {
    tt::EngineFixture f("e04");
    f.connect_and_logon();
    for (int i = 0; i < 3; ++i) f.feed(f.app(f.engine->next_in(), "data"));
    const uint64_t in_before = f.engine->next_in();
    f.out();

    peer::Msg old = f.msg("U1", 2);
    old.extra.push_back({20001, "replayed as new"});
    f.feed(old.build());

    CHECK_EQ(count_type(f.transport.new_frames(), "5"), 1);
    CHECK_NE(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->next_in(), in_before);
    CHECK(f.audit.contains("too low"));
}

SCENARIO("E-05", "P0", "an already processed PossDup is ignored without a second delivery") {
    tt::EngineFixture f("e05");
    f.connect_and_logon();
    const uint64_t seq = f.engine->next_in();
    const std::string original = f.app(seq, "only once");
    f.feed(original);
    CHECK_EQ(f.sink.delivered.size(), size_t{1});

    // The same business message arrives again as a FIX retransmission.
    peer::Msg dup = f.msg("U1", seq);
    dup.poss_dup = true;
    dup.orig_sending_time = peer::get(original, 52).value();
    dup.extra.push_back({20001, "only once"});
    f.feed(dup.build());

    CHECK_EQ(f.sink.delivered.size(), size_t{1});
    CHECK_EQ(f.engine->next_in(), seq + 1);
    CHECK_EQ(f.metrics.counter(metric::kDuplicateInbound), uint64_t{1});
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("E-06", "P0", "a PossDup without OrigSendingTime is not trusted") {
    tt::EngineFixture f("e06");
    f.connect_and_logon();
    f.out();
    const uint64_t seq = f.engine->next_in();

    // PossDupFlag=Y with no tag 122: FIX 4.4 requires it, and without it the
    // provenance of the duplicate cannot be checked.
    std::vector<peer::Field> fields{{35, "U1"},
                                    {49, f.cfg.target_comp_id},
                                    {56, f.cfg.sender_comp_id},
                                    {34, std::to_string(seq)},
                                    {52, format_utc_timestamp(f.clock.wall_now())},
                                    {43, "Y"},
                                    {20001, "no provenance"}};
    f.feed(peer::build_from_fields(fields));

    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(count_type(f.transport.new_frames(), "3"), 1);
    // Sequencing still advances because the message was rejected, not lost.
    CHECK_EQ(f.engine->next_in(), seq + 1);
}

SCENARIO("E-07", "P0", "a high sequence carrying PossDup still triggers recovery") {
    tt::EngineFixture f("e07");
    f.connect_and_logon();
    f.out();
    const uint64_t expected = f.engine->next_in();

    peer::Msg ahead = f.msg("U1", expected + 2);
    ahead.poss_dup = true;
    ahead.orig_sending_time = format_utc_timestamp(f.clock.wall_now());
    ahead.extra.push_back({20001, "retransmission out of order"});
    f.feed(ahead.build());

    CHECK(f.out_of_type("2").has_value());
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->next_in(), expected);
}

SCENARIO("E-08", "P0", "messages arriving out of order are delivered in sequence order") {
    tt::EngineFixture f("e08");
    f.connect_and_logon();
    const uint64_t base = f.engine->next_in();

    f.feed(f.app(base + 2, "third"));
    f.feed(f.app(base, "first"));
    f.feed(f.app(base + 1, "second"));

    CHECK_EQ(f.sink.delivered.size(), size_t{3});
    CHECK_EQ(f.sink.delivered[0], base);
    CHECK_EQ(f.sink.delivered[1], base + 1);
    CHECK_EQ(f.sink.delivered[2], base + 2);
    CHECK_EQ(f.engine->next_in(), base + 3);
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("PB-05", "P0", "the playbook gap sequence is delivered as 100 to 104") {
    // Playbook PB-05: NextNumIn starts at 100; the peer sends 102 then 104,
    // the engine requests the gap, and 100, 101 then 103 arrive.
    tt::MiniEngine m(100, 50);
    m.connect_and_logon();  // the Logon itself occupies sequence 100
    const uint64_t base = m.store.state.next_in;
    CHECK_EQ(base, uint64_t{101});

    m.feed(m.app(base + 1, "102"));  // 102
    m.feed(m.app(base + 3, "104"));  // 104
    CHECK_EQ(m.sink.delivered.size(), size_t{0});
    CHECK(m.engine->open_gap().has_value());

    m.feed(m.app(base, "101"));      // fills the first hole
    m.feed(m.app(base + 2, "103"));  // fills the second

    CHECK_EQ(m.sink.delivered.size(), size_t{4});
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(m.sink.delivered[i], base + i);
    CHECK_EQ(m.engine->state(), SessionState::Active);
    CHECK_EQ(m.store.state.next_in, base + 4);
}

SCENARIO("E-09", "P1", "a duplicate heartbeat has no repeated side effect") {
    tt::EngineFixture f("e09");
    f.connect_and_logon();
    const uint64_t seq = f.engine->next_in();
    const std::string hb = f.msg("0", seq).build();
    f.feed(hb);
    CHECK_EQ(f.engine->next_in(), seq + 1);

    peer::Msg dup = f.msg("0", seq);
    dup.poss_dup = true;
    dup.orig_sending_time = peer::get(hb, 52).value();
    f.feed(dup.build());
    CHECK_EQ(f.engine->next_in(), seq + 1);
    CHECK_EQ(f.engine->state(), SessionState::Active);
}

SCENARIO("E-10", "P0", "a duplicated administrative message is not acted on twice") {
    tt::EngineFixture f("e10");
    f.connect_and_logon();
    f.out();

    const uint64_t seq = f.engine->next_in();
    peer::Msg tr = f.msg("1", seq);
    tr.extra.push_back({112, "probe-1"});
    const std::string original = tr.build();
    f.feed(original);
    CHECK_EQ(count_type(f.transport.new_frames(), "0"), 1);

    // The same TestRequest arrives again as a retransmission. Answering it a
    // second time would replay an administrative side effect.
    peer::Msg dup = f.msg("1", seq);
    dup.poss_dup = true;
    dup.orig_sending_time = peer::get(original, 52).value();
    dup.extra.push_back({112, "probe-1"});
    f.feed(dup.build());
    CHECK_EQ(count_type(f.transport.new_frames(), "0"), 0);
}

SCENARIO("E-11", "P0", "a gap discovered after a reconnect is recovered before ACTIVE") {
    tt::EngineFixture f("e11");
    f.connect_and_logon();
    for (int i = 0; i < 2; ++i) f.feed(f.app(f.engine->next_in(), "pre-drop"));
    const uint64_t in_before = f.engine->next_in();

    f.engine->on_disconnect(f.generation, "link lost");
    CHECK_EQ(f.engine->state(), SessionState::Backoff);
    f.reconnect();
    f.out();

    // The peer logs on with a sequence three ahead: it believes it already sent
    // two application messages plus the Logon we never saw.
    f.feed(f.logon_reply(in_before + 2));
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
    CHECK(!f.engine->ready());
    const auto rr = f.out_of_type("2");
    CHECK(rr.has_value());
    CHECK_EQ(peer::get(*rr, 7).value_or(""), std::to_string(in_before));

    // The peer answers the way a real peer does: it retransmits the two
    // application messages, then covers its own administrative sequence (the
    // Logon) with a SequenceReset-GapFill.
    for (uint64_t s = in_before; s <= in_before + 1; ++s) f.feed(f.app(s, "recovered"));
    // The requested range is now complete, so the engine is synchronised. The
    // peer Logon that sat at the next sequence was never committed, because a
    // Logon with a high sequence establishes the session without advancing
    // NextNumIn.
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->next_in(), in_before + 2);

    peer::Msg gf = f.msg("4", in_before + 2);
    gf.poss_dup = true;
    gf.orig_sending_time = format_utc_timestamp(f.clock.wall_now());
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, std::to_string(in_before + 3)});
    f.feed(gf.build());

    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->next_in(), in_before + 3);
    CHECK_EQ(f.sink.delivered.size(), size_t{4});
    // The skipped administrative sequence is applied so recovery can finish,
    // but it is recorded as never recovered rather than assumed delivered.
    CHECK_EQ(f.engine->skipped_unrecovered(), uint64_t{1});
    CHECK_EQ(f.metrics.gauge(metric::kUnresolvedGaps), int64_t{1});
    CHECK(f.audit.contains("never recovered"));
}

SCENARIO("E-12", "P0", "a corrupt frame inside a gap is not processed") {
    tt::EngineFixture f("e12");
    f.connect_and_logon();
    f.out();
    const uint64_t missing = f.engine->next_in();

    f.feed(f.app(missing + 1, "ahead"));
    CHECK(f.engine->open_gap().has_value());

    std::string broken = f.app(missing, "corrupt");
    broken[broken.size() - 4] = broken[broken.size() - 4] == '0' ? '1' : '0';
    f.feed(broken);

    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK(f.metrics.counter(metric::kMalformedFrames) >= uint64_t{1});
    CHECK_NE(f.engine->state(), SessionState::Active);
}

SCENARIO("E-13", "P1", "two disjoint gaps are recovered without looping") {
    tt::EngineFixture f("e13");
    f.connect_and_logon();
    f.out();
    const uint64_t base = f.engine->next_in();

    f.feed(f.app(base + 2, "a"));
    f.feed(f.app(base + 5, "b"));
    const auto gap = f.engine->open_gap();
    CHECK(gap.has_value());
    CHECK_EQ(gap->first, base);
    CHECK_EQ(gap->second, base + 4);
    CHECK_EQ(f.engine->future_buffer_size(), size_t{2});

    for (uint64_t s : {base, base + 1, base + 3, base + 4}) f.feed(f.app(s, "fill"));
    CHECK_EQ(f.sink.delivered.size(), size_t{6});
    for (size_t i = 0; i < 6; ++i) CHECK_EQ(f.sink.delivered[i], base + i);
    CHECK_EQ(f.engine->state(), SessionState::Active);
    CHECK_EQ(f.engine->future_buffer_size(), size_t{0});
}

SCENARIO("E-14", "P0", "a full out of order buffer drops future frames instead of growing") {
    tt::EngineFixture f("e14");
    f.connect_and_logon();
    const uint64_t base = f.engine->next_in();

    for (uint64_t i = 1; i <= 40; ++i) f.feed(f.app(base + i, "future"));
    CHECK(f.engine->future_buffer_size() <= f.cfg.max_future_messages);
    CHECK(f.engine->future_buffer_bytes() <= f.cfg.max_future_bytes);
    CHECK_EQ(f.sink.delivered.size(), size_t{0});
    CHECK_EQ(f.engine->state(), SessionState::Synchronizing);
}

SCENARIO("E-16", "P0", "a Logout with a sequence gap is recovered before closing") {
    tt::EngineFixture f("e16");
    f.connect_and_logon();
    f.out();
    const uint64_t expected = f.engine->next_in();

    peer::Msg bye = f.msg("5", expected + 2);
    bye.extra.push_back({58, "closing"});
    f.feed(bye.build());

    // The gap is requested, and the Logout is not treated as having filled it.
    CHECK(f.out_of_type("2").has_value());
    CHECK_EQ(f.engine->next_in(), expected);
    CHECK_NE(f.engine->state(), SessionState::Active);
}

SCENARIO("E-17", "P0", "a ResendRequest with a high sequence is served and the gap requested") {
    tt::MiniEngine m(1, 1);
    m.connect_and_logon();
    for (int i = 0; i < 3; ++i)
        CHECK_EQ(m.engine->submit_app_message("U1", {{20001, "hist"}}), SubmitResult::Accepted);
    m.out();
    const uint64_t expected = m.store.state.next_in;

    peer::Msg rr = m.msg("2", expected + 3);
    rr.extra.push_back({7, "2"});
    rr.extra.push_back({16, "3"});
    m.feed(rr.build());

    const auto frames = m.transport.new_frames();
    bool saw_replay = false;
    for (const std::string& fr : frames)
        if (peer::get(fr, 43).value_or("") == "Y") saw_replay = true;
    // Both halves happen, so neither side waits for the other (F-14).
    CHECK(saw_replay);
    CHECK(count_type(frames, "2") >= 1);
    CHECK_EQ(m.store.state.next_in, expected);
}

SCENARIO("E-18", "P0", "a SequenceReset-GapFill ahead of the expected sequence is not applied") {
    tt::EngineFixture f("e18");
    f.connect_and_logon();
    f.out();
    const uint64_t expected = f.engine->next_in();

    peer::Msg gf = f.msg("4", expected + 3);
    gf.extra.push_back({123, "Y"});
    gf.extra.push_back({36, std::to_string(expected + 9)});
    f.feed(gf.build());

    CHECK_EQ(f.engine->next_in(), expected);
    CHECK(f.out_of_type("2").has_value());
    CHECK(f.audit.contains("GapFill arrived ahead"));
}

SCENARIO("E-19", "P1", "a sequence at the 64 bit boundary fails closed instead of wrapping") {
    tt::MiniEngine m(UINT64_MAX, UINT64_MAX);
    // The engine refuses to issue an original message at the boundary, so no
    // Logon is ever written.
    m.connect();
    CHECK_EQ(m.engine->state(), SessionState::Faulted);
    CHECK_EQ(m.engine->fault_reason(), FaultReason::SequenceRollback);
    CHECK_EQ(m.transport.written.size(), size_t{0});
}

SCENARIO("E-20", "P0", "the same sequence with different content is an integrity failure") {
    tt::EngineFixture f("e20");
    f.connect_and_logon();
    const uint64_t seq = f.engine->next_in();
    const std::string original = f.app(seq, "the real payload");
    f.feed(original);
    CHECK_EQ(f.sink.delivered.size(), size_t{1});

    // A retransmission claiming the same sequence but carrying something else.
    peer::Msg tampered = f.msg("U1", seq);
    tampered.poss_dup = true;
    tampered.orig_sending_time = peer::get(original, 52).value();
    tampered.extra.push_back({20001, "a different payload"});
    f.feed(tampered.build());

    CHECK_EQ(f.engine->state(), SessionState::Faulted);
    CHECK_EQ(f.engine->fault_reason(), FaultReason::IntegrityMismatch);
    CHECK_EQ(f.sink.delivered.size(), size_t{1});
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "sequence_gap"); }
