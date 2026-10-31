// Resend planning: group F of the what-if matrix at the pure-function level,
// plus playbook PB-06. Design doc 8.3 and 16.
#include <algorithm>
#include <string>
#include <vector>

#include "fix/codec.hpp"
#include "fix/resend_planner.hpp"
#include "fix/wire.hpp"
#include "peer_wire.hpp"
#include "tiny_test.hpp"

using namespace fixse;

namespace {

OutboundRecord rec(uint64_t seq, const char* type) {
    OutboundRecord r;
    r.seq = seq;
    r.msg_type = type;
    r.payload = "frame-" + std::to_string(seq);
    r.original_sending_time = "20261030-05:00:0" + std::to_string(seq % 10) + ".000";
    return r;
}

ResendRange range_of(uint64_t begin, uint64_t end) {
    ResendRange r;
    r.valid = true;
    r.begin = begin;
    r.end = end;
    return r;
}

std::string describe(const ResendPlan& p) {
    std::string s;
    for (const ResendAction& a : p.actions) {
        if (a.kind == ResendAction::Kind::ReplayApp) s += "app(" + std::to_string(a.seq) + ") ";
        else s += "gapfill(" + std::to_string(a.seq) + "->" + std::to_string(a.new_seq_no) + ") ";
    }
    return s;
}

}  // namespace

SCENARIO("F-01", "P0", "a fully available range is replayed in ascending order") {
    std::vector<OutboundRecord> history;
    for (uint64_t s = 10; s <= 20; ++s) history.push_back(rec(s, "U1"));
    const ResendPlan p = plan_resend(range_of(10, 20), history);
    CHECK(p.ok);
    CHECK_EQ(p.actions.size(), size_t{11});
    for (size_t i = 0; i < p.actions.size(); ++i) {
        CHECK_EQ(p.actions[i].kind, ResendAction::Kind::ReplayApp);
        CHECK_EQ(p.actions[i].seq, uint64_t{10} + i);
    }
}

SCENARIO("F-02", "P0", "EndSeqNo of zero resolves to the last original message") {
    const ResendRange r = resolve_range(10, 0, 42);
    CHECK(r.valid);
    CHECK_EQ(r.begin, uint64_t{10});
    CHECK_EQ(r.end, uint64_t{42});
}

SCENARIO("F-03", "P0", "BeginSeqNo of zero is rejected without scanning history") {
    const ResendRange r = resolve_range(0, 10, 42);
    CHECK(!r.valid);
    CHECK(r.detail.find("BeginSeqNo") != std::string::npos);
    const ResendPlan p = plan_resend(r, {});
    CHECK(!p.ok);
    CHECK_EQ(p.actions.size(), size_t{0});
}

SCENARIO("F-04", "P0", "a range that runs backwards is a session level error") {
    const ResendRange r = resolve_range(20, 10, 42);
    CHECK(!r.valid);
    CHECK(r.detail.find("lower than") != std::string::npos);
}

SCENARIO("F-05", "P1", "a range beyond the last original message is clamped") {
    const ResendRange r = resolve_range(10, 999999, 42);
    CHECK(r.valid);
    CHECK_EQ(r.end, uint64_t{42});

    // A range that starts beyond anything ever sent is refused outright.
    CHECK(!resolve_range(100, 0, 42).valid);
    // A session that has never sent an original message cannot serve one.
    CHECK(!resolve_range(1, 0, 0).valid);
}

SCENARIO("F-06", "P0", "administrative messages are gap filled, never replayed") {
    std::vector<OutboundRecord> history{rec(10, "A"), rec(11, "0"), rec(12, "1")};
    const ResendPlan p = plan_resend(range_of(10, 12), history);
    CHECK(p.ok);
    CHECK_EQ(p.actions.size(), size_t{1});
    CHECK_EQ(p.actions[0].kind, ResendAction::Kind::GapFill);
    CHECK_EQ(p.actions[0].seq, uint64_t{10});
    CHECK_EQ(p.actions[0].new_seq_no, uint64_t{13});
}

SCENARIO("F-07", "P0", "a run of adjacent administrative messages collapses into one GapFill") {
    std::vector<OutboundRecord> history{rec(1, "0"), rec(2, "0"), rec(3, "1"), rec(4, "0"),
                                        rec(5, "5")};
    const ResendPlan p = plan_resend(range_of(1, 5), history);
    CHECK(p.ok);
    CHECK_MSG(p.actions.size() == size_t{1}, describe(p));
    CHECK_EQ(p.actions[0].seq, uint64_t{1});
    CHECK_EQ(p.actions[0].new_seq_no, uint64_t{6});
}

SCENARIO("F-08", "P0", "an interleaved range replays applications and gap fills the rest") {
    std::vector<OutboundRecord> history{rec(1, "U1"), rec(2, "0"), rec(3, "1"),
                                        rec(4, "U1"), rec(5, "0"), rec(6, "U1")};
    const ResendPlan p = plan_resend(range_of(1, 6), history);
    CHECK(p.ok);
    CHECK_MSG(describe(p) == "app(1) gapfill(2->4) app(4) gapfill(5->6) app(6) ", describe(p));
}

SCENARIO("PB-06", "P0", "the playbook replay plan is produced exactly as specified") {
    // Playbook PB-06: history 10=app, 11=Heartbeat, 12=TestRequest, 13=app,
    // 14=Logout, 15=app; the peer requested [10..15].
    std::vector<OutboundRecord> history{rec(10, "U1"), rec(11, "0"), rec(12, "1"),
                                        rec(13, "U1"), rec(14, "5"), rec(15, "U1")};
    const ResendPlan p = plan_resend(range_of(10, 15), history);
    CHECK(p.ok);
    CHECK_EQ(p.actions.size(), size_t{5});
    CHECK_MSG(describe(p) == "app(10) gapfill(11->13) app(13) gapfill(14->15) app(15) ",
              describe(p));
    // The GapFill carries the OrigSendingTime of the first message it covers.
    CHECK_EQ(p.actions[1].original_sending_time, rec(11, "0").original_sending_time);
}

SCENARIO("F-16", "P0", "a hole in the archive fails closed instead of being gap filled") {
    // Sequence 12 is missing, which is what a premature retention job or a lost
    // row looks like (also scenario G-20).
    std::vector<OutboundRecord> history{rec(10, "U1"), rec(11, "U1"), rec(13, "U1")};
    const ResendPlan p = plan_resend(range_of(10, 13), history);
    CHECK(!p.ok);
    CHECK_EQ(p.fault, FaultReason::MissingOutboundHistory);
    CHECK_EQ(p.actions.size(), size_t{0});
    CHECK(p.detail.find("12") != std::string::npos);
}

SCENARIO("F-06b", "P1", "a session level Reject is retransmitted rather than gap filled") {
    // FIX 4.4 allows Reject to be resent, unlike Logon or Heartbeat
    // (design doc 8.3).
    std::vector<OutboundRecord> history{rec(1, "0"), rec(2, "3"), rec(3, "0")};
    const ResendPlan p = plan_resend(range_of(1, 3), history);
    CHECK(p.ok);
    CHECK_MSG(describe(p) == "gapfill(1->2) app(2) gapfill(3->4) ", describe(p));
}

SCENARIO("F-11b", "P0", "chunked planning neither drops nor duplicates a sequence") {
    std::vector<OutboundRecord> all;
    for (uint64_t s = 1; s <= 25; ++s) all.push_back(rec(s, s % 5 == 0 ? "0" : "U1"));

    std::vector<uint64_t> replayed;
    std::vector<std::pair<uint64_t, uint64_t>> filled;
    const uint64_t chunk = 7;
    for (uint64_t pos = 1; pos <= 25; pos += chunk) {
        const uint64_t end = std::min<uint64_t>(25, pos + chunk - 1);
        std::vector<OutboundRecord> slice;
        for (const OutboundRecord& r : all)
            if (r.seq >= pos && r.seq <= end) slice.push_back(r);
        const ResendPlan p = plan_resend(range_of(pos, end), slice);
        CHECK(p.ok);
        for (const ResendAction& a : p.actions) {
            if (a.kind == ResendAction::Kind::ReplayApp) replayed.push_back(a.seq);
            else filled.emplace_back(a.seq, a.new_seq_no);
        }
    }

    // Every sequence is covered exactly once, by a replay or by a gap fill.
    std::vector<bool> covered(26, false);
    for (uint64_t s : replayed) {
        CHECK(!covered[s]);
        covered[s] = true;
    }
    for (const auto& g : filled) {
        for (uint64_t s = g.first; s < g.second; ++s) {
            CHECK(!covered[s]);
            covered[s] = true;
        }
    }
    for (uint64_t s = 1; s <= 25; ++s) CHECK_MSG(covered[s], "sequence " + std::to_string(s));
}

SCENARIO("F-09", "P0", "a retransmission keeps the original sequence and marks PossDup") {
    // rebuild_as_retransmission is what the engine uses for every replay.
    OutboundFields f;
    f.msg_type = "U1";
    f.seq_num = 77;
    f.sender_comp_id = "TRADER01";
    f.target_comp_id = "SIMLP01";
    f.sending_time = WallTime{} + std::chrono::seconds(1793336400);
    f.body.emplace_back(20001, "original-payload");
    const std::string original = encode(f);

    std::string replay;
    CHECK(rebuild_as_retransmission(original, f.sending_time + std::chrono::seconds(90), &replay));

    DecodeOptions opt;
    DecodedMessage m;
    CHECK_EQ(decode(replay, opt, &m), WireError::None);
    CHECK_EQ(m.seq_num, uint64_t{77});
    CHECK(m.poss_dup);
    CHECK_EQ(std::string(m.orig_sending_time_raw), std::string("20261030-05:00:00.000"));
    CHECK_EQ(std::string(m.sending_time_raw), std::string("20261030-05:01:30.000"));
    CHECK_EQ(std::string(m.find(20001).value()), std::string("original-payload"));
    // The body length and checksum are recomputed for the new bytes (F-10).
    std::string why;
    CHECK_MSG(peer::verify_frame(replay, &why), why);
}

SCENARIO("F-17b", "P0", "a corrupt stored frame cannot be turned into a retransmission") {
    std::string out;
    CHECK(!rebuild_as_retransmission("not a fix frame at all", WallTime{}, &out));
    CHECK(!rebuild_as_retransmission("", WallTime{}, &out));
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "resend_planner"); }
