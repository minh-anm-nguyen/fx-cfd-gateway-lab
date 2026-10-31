// The P04 <-> P01 frame bridge.
//
// The assertion that matters is the last one in each direction: a raw frame off
// P01's socket, once stripped, is accepted by `router::fix::parse_app_frame` --
// the router's own parser, not a reimplementation of it. Anything short of that
// is a bridge that compiles and then rejects every message on the wire.
//
// Both parsers are strict in ways that do not agree, which is the whole reason
// this file exists. P04's rules are "35 first, no unknown must-understand
// tags"; a raw frame begins 8=FIX.4.4, 9=..., 35=... And P01 owns 8, 9, 34, 49,
// 52, 56 and 10, refusing a caller that supplies them.
#include <cstdio>
#include <optional>
#include <string>

#include "gw/fix_transport.hpp"
#include "router/fix44.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* expr, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL line %d  %s%s%s\n", line, expr, detail.empty() ? "" : " -- ",
                detail.c_str());
}

#define CHECK(expr) check((expr), #expr, __LINE__, {})
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

constexpr char kSoh = '\x01';

std::string soh(const std::string& pipes) {
    std::string out = pipes;
    for (char& c : out) {
        if (c == '|') c = kSoh;
    }
    return out;
}

// An ExecutionReport as it arrives off the socket: full standard header, the
// business tags, standard trailer.
std::string raw_execution_report() {
    return soh(
        "8=FIX.4.4|9=176|35=8|34=7|49=LPSIM01|56=ROUTER04|52=20261031-09:15:00.000|"
        "37=LP-ORD-1|11=O-1|17=E-1|150=F|39=1|1=ACC1|55=EUR/USD|54=1|38=10000|"
        "32=5000|31=1.10000|14=5000|151=5000|6=1.10000|60=20261031-09:15:00.000|10=093|");
}

// What P04 hands out for a New: 35 plus the business tags, no session tags.
std::string p04_new_order() {
    return soh("35=D|11=O-1|1=ACC1|55=EUR/USD|54=1|60=20261031-09:15:00.000|38=10000|40=2|"
               "44=1.10000|59=0|");
}

// ---------------------------------------------------------------------------

void the_session_envelope_is_the_part_p01_owns() {
    std::printf("the_session_envelope_is_the_part_p01_owns\n");

    for (const int tag : {8, 9, 34, 49, 56, 52, 10, 122, 89, 93}) {
        CHECK_MSG(gw::is_session_tag(tag), "tag " + std::to_string(tag));
    }
    // Business tags stay.
    for (const int tag : {11, 17, 37, 39, 150, 1, 55, 54, 38, 32, 31, 14, 151, 6, 44, 59, 40}) {
        CHECK_MSG(!gw::is_session_tag(tag), "tag " + std::to_string(tag));
    }
    // And the two that look like session fields but are not, for this bridge's
    // purposes: P04 records both on the normalized report, and EI-03 turns on
    // telling a retransmission apart from a business retry.
    CHECK(!gw::is_session_tag(43));  // PossDupFlag
    CHECK(!gw::is_session_tag(97));  // PossResend
}

void a_stripped_inbound_frame_is_accepted_by_p04s_own_parser() {
    std::printf("a_stripped_inbound_frame_is_accepted_by_p04s_own_parser\n");

    const std::string raw = raw_execution_report();
    const router::fix::ParseLimits limits;

    // The router refuses the raw frame, which is the problem being solved
    // rather than an incidental detail.
    CHECK(router::fix::parse_app_frame(raw, limits).is_err());

    const std::optional<std::string> app = gw::strip_session_envelope(raw);
    CHECK(app.has_value());
    if (!app.has_value()) return;

    // 35 moved to the front, which is what parse_app_frame requires.
    CHECK(app->rfind("35=8", 0) == 0);

    const auto parsed = router::fix::parse_app_frame(*app, limits);
    CHECK_MSG(parsed.is_ok(), "the router still refuses the stripped frame");
    if (parsed.is_err()) return;

    const router::fix::Message& m = parsed.value();
    CHECK(m.msg_type() == '8');

    // Every business tag survived, with its value.
    CHECK(m.get_or(17) == "E-1");
    CHECK(m.get_or(11) == "O-1");
    CHECK(m.get_or(37) == "LP-ORD-1");
    CHECK(m.get_or(150) == "F");
    CHECK(m.get_or(39) == "1");
    CHECK(m.get_or(32) == "5000");
    CHECK(m.get_or(31) == "1.10000");
    CHECK(m.get_or(14) == "5000");
    CHECK(m.get_or(151) == "5000");
    CHECK(m.get_or(55) == "EUR/USD");

    // And no session tag reached the router.
    for (const int tag : {8, 9, 34, 49, 56, 52, 10}) {
        CHECK_MSG(!m.has(tag), "session tag " + std::to_string(tag) + " reached P04");
    }
}

void a_retransmission_marker_survives_the_strip() {
    std::printf("a_retransmission_marker_survives_the_strip\n");

    const std::string raw = soh(
        "8=FIX.4.4|9=100|35=8|34=7|49=LPSIM01|56=ROUTER04|52=20261031-09:15:00.000|"
        "43=Y|97=Y|122=20261031-09:14:00.000|37=LP-ORD-1|11=O-1|17=E-1|150=F|39=1|"
        "1=ACC1|55=EUR/USD|54=1|38=10000|32=5000|31=1.10000|14=5000|151=5000|10=093|");

    const std::optional<std::string> app = gw::strip_session_envelope(raw);
    CHECK(app.has_value());
    if (!app.has_value()) return;

    const router::fix::ParseLimits limits;
    const auto parsed = router::fix::parse_app_frame(*app, limits);
    CHECK_MSG(parsed.is_ok(), "the stripped resend was refused");
    if (parsed.is_err()) return;

    // The markers the router needs to tell a session resend from a business
    // retry are still there; OrigSendingTime, which is P01's business, is not.
    CHECK(parsed.value().get_or(43) == "Y");
    CHECK(parsed.value().get_or(97) == "Y");
    CHECK(!parsed.value().has(122));
}

void an_outbound_frame_becomes_a_submit_body() {
    std::printf("an_outbound_frame_becomes_a_submit_body\n");

    const std::optional<gw::SubmitBody> body = gw::to_submit_body(p04_new_order());
    CHECK(body.has_value());
    if (!body.has_value()) return;

    // 35 is lifted out, because P01 takes it as a separate argument and writes
    // it into the header itself.
    CHECK(body->msg_type == "D");
    for (const auto& [tag, value] : body->fields) {
        CHECK_MSG(tag != 35, "35 was left in the body");
        CHECK_MSG(!gw::is_session_tag(tag),
                  "session tag " + std::to_string(tag) + " in the body");
    }

    // The business content is intact and in order: P04 digests the field
    // sequence, so reordering it would change the fingerprint of the frame it
    // believes it sent.
    CHECK(body->fields.size() == 9);
    CHECK(body->fields.front().first == 11);
    CHECK(body->fields.front().second == "O-1");
    CHECK(body->fields.back().first == 59);
}

void a_frame_without_a_message_type_is_refused() {
    std::printf("a_frame_without_a_message_type_is_refused\n");

    // No 35. There is nothing to infer, and inventing one would put a guess in
    // front of the router's dictionary, so both directions refuse.
    const std::string headerless = soh("8=FIX.4.4|9=20|34=7|49=LPSIM01|56=ROUTER04|10=093|");
    CHECK(!gw::strip_session_envelope(headerless).has_value());
    CHECK(!gw::to_submit_body(soh("11=O-1|38=10000|")).has_value());

    // Malformed input is refused rather than half-read.
    CHECK(!gw::strip_session_envelope(soh("notatag|35=8|")).has_value());
    CHECK(!gw::strip_session_envelope(soh("=5|35=8|")).has_value());
    CHECK(!gw::strip_session_envelope("").has_value());
}

void a_new_order_is_outbound_only() {
    std::printf("a_new_order_is_outbound_only\n");

    // Worth pinning, because the obvious round-trip test is wrong. P04's
    // parse_app_frame refuses 35=D with UnsupportedMsgType -- not over a tag,
    // but because a New is something P04 sends and never receives. Its inbound
    // dictionary covers 35=8 and 35=9. So the two directions carry different
    // message types and there is no symmetric round trip to assert.
    const router::fix::ParseLimits limits;
    const auto refused = router::fix::parse_app_frame(p04_new_order(), limits);
    CHECK(refused.is_err());
    if (refused.is_err()) {
        CHECK(refused.error() == router::ValidationError::UnsupportedMsgType);
    }

    // The outbound direction is still the one that matters for a New, and it
    // does not go through that parser at all: the body goes to P01.
    CHECK(gw::to_submit_body(p04_new_order()).has_value());
}

void the_inbound_direction_composes() {
    std::printf("the_inbound_direction_composes\n");

    // The real composition: an ExecutionReport body, wrapped in the envelope
    // P01 writes, strips back to something the router accepts. The envelope
    // here stands in for P01's, since building it is P01's job.
    const std::string body = soh("37=LP-ORD-1|11=O-1|17=E-2|150=F|39=2|1=ACC1|55=EUR/USD|"
                                 "54=1|38=10000|32=5000|31=1.10000|14=10000|151=0|");
    std::string wrapped = soh("8=FIX.4.4|9=0|35=8|34=9|49=LPSIM01|56=ROUTER04|"
                              "52=20261031-09:16:00.000|") + body + soh("10=011|");

    const std::optional<std::string> back = gw::strip_session_envelope(wrapped);
    CHECK(back.has_value());
    if (!back.has_value()) return;

    const router::fix::ParseLimits limits;
    const auto parsed = router::fix::parse_app_frame(*back, limits);
    CHECK_MSG(parsed.is_ok(), "the wrapped report strips to a frame P04 refuses");
    if (parsed.is_err()) return;
    CHECK(parsed.value().msg_type() == '8');
    CHECK(parsed.value().get_or(17) == "E-2");
    CHECK(parsed.value().get_or(14) == "10000");
    CHECK(parsed.value().get_or(151) == "0");
}

}  // namespace

int main() {
    std::printf("P08 FIX transport: P04's frames through P01's session\n\n");

    the_session_envelope_is_the_part_p01_owns();
    a_stripped_inbound_frame_is_accepted_by_p04s_own_parser();
    a_retransmission_marker_survives_the_strip();
    an_outbound_frame_becomes_a_submit_body();
    a_frame_without_a_message_type_is_refused();
    a_new_order_is_outbound_only();
    the_inbound_direction_composes();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
