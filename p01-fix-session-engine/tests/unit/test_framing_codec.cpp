// Group B of the what-if matrix: framing, codec and input validation.
// Design doc 15.3. Every case here is a trust-boundary case: the bytes come
// from an untrusted peer and must not reach session state until proven well
// formed (invariant I-03).
#include <string>
#include <vector>

#include "fix/codec.hpp"
#include "fix/framer.hpp"
#include "peer_wire.hpp"
#include "tiny_test.hpp"

using namespace fixse;

namespace {

constexpr size_t kMaxFrame = 65536;
constexpr size_t kMaxBuffer = 131072;

FixFramer make_framer() { return FixFramer(kMaxFrame, kMaxBuffer); }

// Feed a whole byte string and collect every frame the framer yields.
struct ScanOutcome {
    std::vector<std::string> frames;
    WireError last_error = WireError::None;
};

ScanOutcome scan_all(std::string_view bytes, size_t chunk = 0) {
    FixFramer fr = make_framer();
    ScanOutcome out;
    size_t pos = 0;
    while (pos < bytes.size()) {
        const size_t n = chunk == 0 ? bytes.size() : std::min(chunk, bytes.size() - pos);
        const WireError ae = fr.append(bytes.substr(pos, n));
        if (ae != WireError::None) {
            out.last_error = ae;
            return out;
        }
        pos += n;
        std::string_view frame;
        WireError err = WireError::None;
        while (fr.next(&frame, &err)) out.frames.push_back(std::string(frame));
        out.last_error = err;
        if (is_malformed(err)) return out;
    }
    return out;
}

DecodeOptions default_opts() {
    DecodeOptions o;
    o.max_fields = 2048;
    return o;
}

WireError decode_of(std::string_view frame, const DecodeOptions& o) {
    DecodedMessage m;
    return decode(frame, o, &m);
}

std::string app_frame(uint64_t seq, std::string_view payload = "hello") {
    peer::Msg m;
    m.msg_type = "U1";  // custom test dictionary type, not standard FIX 4.4
    m.seq = seq;
    m.extra.push_back({20001, std::string(payload)});
    return m.build();
}

}  // namespace

SCENARIO("B-01", "P0", "one frame delivered one byte per recv is reassembled exactly once") {
    const std::string frame = app_frame(1);
    const ScanOutcome o = scan_all(frame, 1);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(o.frames[0], frame);
    CHECK_EQ(o.last_error, WireError::Incomplete);
}

SCENARIO("B-02", "P0", "one hundred frames inside a single recv are all parsed in order") {
    std::string stream;
    for (uint64_t i = 1; i <= 100; ++i) stream += app_frame(i);
    const ScanOutcome o = scan_all(stream);
    CHECK_EQ(o.frames.size(), size_t{100});
    for (uint64_t i = 0; i < 100; ++i) {
        DecodedMessage m;
        CHECK_EQ(decode(o.frames[i], default_opts(), &m), WireError::None);
        CHECK_EQ(m.seq_num, i + 1);
    }
}

SCENARIO("B-03", "P0", "a split at every byte offset yields the identical single frame") {
    const std::string frame = app_frame(7);
    for (size_t cut = 1; cut < frame.size(); ++cut) {
        FixFramer fr = make_framer();
        CHECK_EQ(fr.append(std::string_view(frame).substr(0, cut)), WireError::None);
        std::string_view got;
        WireError err = WireError::None;
        CHECK_MSG(!fr.next(&got, &err), std::string("premature frame at cut ") +
                                            std::to_string(cut));
        CHECK_EQ(err, WireError::Incomplete);
        CHECK_EQ(fr.append(std::string_view(frame).substr(cut)), WireError::None);
        CHECK(fr.next(&got, &err));
        CHECK_EQ(std::string(got), frame);
        CHECK(!fr.next(&got, &err));
    }
}

SCENARIO("B-04", "P0", "BodyLength one byte short is rejected, not resynchronised") {
    const std::string good = app_frame(1);
    const std::string declared = peer::get(good, 9).value();
    const std::string bad =
        peer::corrupt_field(good, 9, std::to_string(std::stoul(declared) - 1));
    const ScanOutcome o = scan_all(bad);
    CHECK_EQ(o.frames.size(), size_t{0});
    CHECK_EQ(o.last_error, WireError::BodyLengthMismatch);
}

SCENARIO("B-05", "P0", "BodyLength too large followed by EOF stays incomplete and bounded") {
    const std::string good = app_frame(1);
    const std::string declared = peer::get(good, 9).value();
    const std::string bad =
        peer::corrupt_field(good, 9, std::to_string(std::stoul(declared) + 100));
    FixFramer fr = make_framer();
    CHECK_EQ(fr.append(bad), WireError::None);
    std::string_view got;
    WireError err = WireError::None;
    CHECK(!fr.next(&got, &err));
    CHECK_EQ(err, WireError::Incomplete);
    // No allocation is driven by the declared length: only the bytes actually
    // received are held.
    CHECK_EQ(fr.buffered(), bad.size());
}

SCENARIO("B-06", "P0", "empty, signed and alphabetic BodyLength are rejected before allocation") {
    const std::string good = app_frame(1);
    for (const char* v : {"", "-1", "abc", "+12", "1 2"}) {
        const ScanOutcome o = scan_all(peer::corrupt_field(good, 9, v));
        CHECK_EQ(o.frames.size(), size_t{0});
        CHECK_MSG(o.last_error == WireError::BadBodyLength ||
                      o.last_error == WireError::Incomplete,
                  std::string("value=") + v + " error=" + std::string(to_string(o.last_error)));
    }
}

SCENARIO("B-07", "P0", "a twenty digit BodyLength cannot overflow the length arithmetic") {
    const std::string good = app_frame(1);
    const ScanOutcome o = scan_all(peer::corrupt_field(good, 9, "99999999999999999999"));
    CHECK_EQ(o.frames.size(), size_t{0});
    CHECK_EQ(o.last_error, WireError::BodyLengthOverflow);
}

SCENARIO("B-08", "P0", "BodyLength beyond max_frame_bytes is rejected immediately") {
    const std::string good = app_frame(1);
    const ScanOutcome o = scan_all(peer::corrupt_field(good, 9, "100000000"));
    CHECK_EQ(o.frames.size(), size_t{0});
    CHECK_EQ(o.last_error, WireError::BodyLengthTooLarge);
}

SCENARIO("B-09", "P0", "a single flipped payload byte is caught by the checksum") {
    std::string frame = app_frame(1);
    const size_t mid = frame.size() / 2;
    // Flip a byte inside the body without touching the declared length.
    frame[mid] = frame[mid] == 'A' ? 'B' : 'A';
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{0});
    CHECK_MSG(o.last_error == WireError::CheckSumMismatch ||
                  o.last_error == WireError::BodyLengthMismatch,
              std::string(to_string(o.last_error)));
}

SCENARIO("B-10", "P1", "a CheckSum that is not exactly three digits is malformed") {
    const std::string good = app_frame(1);
    for (const char* v : {"2", "1234", "ab1"}) {
        const ScanOutcome o = scan_all(peer::corrupt_field(good, 10, v));
        CHECK_EQ(o.frames.size(), size_t{0});
        CHECK_MSG(o.last_error == WireError::BadCheckSumFormat ||
                      o.last_error == WireError::Incomplete,
                  std::string("value=") + v + " error=" + std::string(to_string(o.last_error)));
    }
}

SCENARIO("B-11", "P0", "the bytes 10= inside a field value do not terminate the frame") {
    peer::Msg m;
    m.msg_type = "U1";
    m.seq = 3;
    // A payload that contains a complete-looking trailer.
    m.extra.push_back({20001, std::string("x10=123") + peer::SOH + "y"});
    // Build by hand: the embedded SOH makes this a data-like field, which the
    // strict decoder rejects, but the FRAMER must still cut the frame at the
    // boundary computed from BodyLength.
    const std::string frame = m.build();
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(o.frames[0], frame);

    // Same idea without an embedded SOH, which must decode cleanly and keep the
    // value intact.
    peer::Msg m2;
    m2.msg_type = "U1";
    m2.seq = 4;
    m2.extra.push_back({20001, "price10=99"});
    const std::string f2 = m2.build();
    const ScanOutcome o2 = scan_all(f2);
    CHECK_EQ(o2.frames.size(), size_t{1});
    DecodedMessage dm;
    CHECK_EQ(decode(o2.frames[0], default_opts(), &dm), WireError::None);
    CHECK_EQ(std::string(dm.find(20001).value()), "price10=99");
}

SCENARIO("B-12", "P0", "a missing BeginString or BodyLength is rejected") {
    const std::string good = app_frame(1);
    const size_t soh1 = good.find(peer::SOH);
    // Drop field 8 entirely.
    const ScanOutcome o1 = scan_all(good.substr(soh1 + 1));
    CHECK_EQ(o1.frames.size(), size_t{0});
    CHECK_EQ(o1.last_error, WireError::MissingBeginString);

    // Keep 8 but drop 9.
    const size_t soh2 = good.find(peer::SOH, soh1 + 1);
    const std::string no_len = good.substr(0, soh1 + 1) + good.substr(soh2 + 1);
    const ScanOutcome o2 = scan_all(no_len);
    CHECK_EQ(o2.frames.size(), size_t{0});
    CHECK_EQ(o2.last_error, WireError::MissingBodyLength);
}

SCENARIO("B-13", "P1", "MsgType outside the third position is rejected by the strict profile") {
    // Swap the order of 35 and 49 while keeping length and checksum consistent.
    std::vector<peer::Field> f{{49, "SIMLP01"}, {35, "U1"}, {56, "TRADER01"},
                              {34, "1"},       {52, "20261030-05:00:00.000"}};
    const std::string frame = peer::build_from_fields(f);
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::MissingMsgType);
}

SCENARIO("B-14", "P0", "dropping any mandatory header tag prevents decoding") {
    for (int drop : {34, 49, 56, 52}) {
        std::vector<peer::Field> f{{35, "U1"},
                                   {49, "SIMLP01"},
                                   {56, "TRADER01"},
                                   {34, "1"},
                                   {52, "20261030-05:00:00.000"}};
        std::vector<peer::Field> kept;
        for (const auto& x : f)
            if (x.tag != drop) kept.push_back(x);
        const std::string frame = peer::build_from_fields(kept);
        const ScanOutcome o = scan_all(frame);
        CHECK_EQ(o.frames.size(), size_t{1});
        const WireError e = decode_of(o.frames[0], default_opts());
        CHECK_MSG(e == WireError::MissingRequiredHeader, std::string("dropped tag ") +
                                                             std::to_string(drop) + " gave " +
                                                             std::string(to_string(e)));
    }
}

SCENARIO("B-15", "P0", "a duplicated mandatory tag is ambiguous and therefore rejected") {
    for (int dup : {34, 49, 56, 52, 10}) {
        std::vector<peer::Field> f{{35, "U1"},
                                   {49, "SIMLP01"},
                                   {56, "TRADER01"},
                                   {34, "1"},
                                   {52, "20261030-05:00:00.000"}};
        if (dup == 10) f.push_back({10, "000"});
        else f.push_back({dup, "9"});
        const std::string frame = peer::build_from_fields(f);
        const ScanOutcome o = scan_all(frame);
        CHECK_EQ(o.frames.size(), size_t{1});
        const WireError e = decode_of(o.frames[0], default_opts());
        CHECK_MSG(e == WireError::DuplicateHeaderTag, std::string("dup tag ") +
                                                          std::to_string(dup) + " gave " +
                                                          std::string(to_string(e)));
    }
}

SCENARIO("B-16", "P0", "a BeginString other than FIX.4.4 is rejected") {
    // Build a fully well formed FIX.4.2 frame: correct BodyLength, correct
    // CheckSum for its own bytes. Only the version is wrong, so the rejection
    // has to come from the decoder rather than from an accidental length or
    // checksum error.
    std::string body;
    body += std::string("35=U1") + peer::SOH;
    body += std::string("49=SIMLP01") + peer::SOH;
    body += std::string("56=TRADER01") + peer::SOH;
    body += std::string("34=1") + peer::SOH;
    body += std::string("52=20261030-05:00:00.000") + peer::SOH;
    std::string frame = std::string("8=FIX.4.2") + peer::SOH + "9=" +
                        std::to_string(body.size()) + peer::SOH + body;
    frame += "10=" + peer::three_digits(peer::independent_checksum(frame)) + peer::SOH;

    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::BadBeginString);
}

SCENARIO("B-17", "P1", "a vertical bar delimiter is not accepted as SOH") {
    std::string frame = app_frame(1);
    for (char& c : frame)
        if (c == peer::SOH) c = '|';
    FixFramer fr = make_framer();
    CHECK_EQ(fr.append(frame), WireError::None);
    std::string_view got;
    WireError err = WireError::None;
    CHECK(!fr.next(&got, &err));
    CHECK_EQ(err, WireError::Incomplete);
}

SCENARIO("B-18", "P1", "a non-ASCII or control byte in a value is rejected") {
    peer::Msg m;
    m.msg_type = "U1";
    m.extra.push_back({20001, std::string("ok") + static_cast<char>(0xC3) + "x"});
    const std::string frame = m.build();
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::NonPrintableValue);
}

SCENARIO("B-19", "P1", "an unknown but well formed optional field is carried without harm") {
    peer::Msg m;
    m.msg_type = "U1";
    m.extra.push_back({29999, "custom-value"});
    const std::string frame = m.build();
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    DecodedMessage dm;
    CHECK_EQ(decode(o.frames[0], default_opts(), &dm), WireError::None);
    CHECK_EQ(std::string(dm.find(29999).value()), "custom-value");
}

SCENARIO("B-20", "P1", "an application MsgType outside the configured dictionary is rejected") {
    const std::vector<std::string> allowed{"U1"};
    DecodeOptions o = default_opts();
    o.allowed_app_msg_types = &allowed;

    peer::Msg bad;
    bad.msg_type = "ZZ";
    CHECK_EQ(decode_of(bad.build(), o), WireError::UnsupportedMsgType);

    peer::Msg good;
    good.msg_type = "U1";
    CHECK_EQ(decode_of(good.build(), o), WireError::None);

    // A session message is always in scope regardless of the app dictionary.
    peer::Msg hb;
    hb.msg_type = "0";
    CHECK_EQ(decode_of(hb.build(), o), WireError::None);
}

SCENARIO("B-21", "P0", "a three hundred digit tag number neither overflows nor spins") {
    const std::string huge_tag(300, '9');
    std::string body;
    body += std::string("35=U1") + peer::SOH;
    body += std::string("49=SIMLP01") + peer::SOH;
    body += std::string("56=TRADER01") + peer::SOH;
    body += std::string("34=1") + peer::SOH;
    body += std::string("52=20261030-05:00:00.000") + peer::SOH;
    body += huge_tag + "=1" + peer::SOH;
    std::string frame = std::string("8=FIX.4.4") + peer::SOH + "9=" +
                        std::to_string(body.size()) + peer::SOH + body;
    frame += "10=" + peer::three_digits(peer::independent_checksum(frame)) + peer::SOH;

    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});
    CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::BadTagNumber);
}

SCENARIO("B-22", "P1", "a frame with more fields than the cap is rejected") {
    std::vector<peer::Field> f{{35, "U1"},
                               {49, "SIMLP01"},
                               {56, "TRADER01"},
                               {34, "1"},
                               {52, "20261030-05:00:00.000"}};
    for (int i = 0; i < 400; ++i) f.push_back({30000 + i, "1"});
    const std::string frame = peer::build_from_fields(f);
    const ScanOutcome o = scan_all(frame);
    CHECK_EQ(o.frames.size(), size_t{1});

    DecodeOptions tight = default_opts();
    tight.max_fields = 50;
    CHECK_EQ(decode_of(o.frames[0], tight), WireError::TooManyFields);
    // The same frame is fine under the configured production cap.
    CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::None);
}

SCENARIO("B-23", "P0", "a malformed SendingTime is rejected and cannot move sequence state") {
    const char* bad_times[] = {
        "20261030",
        "2026-10-30-05:00:00",
        "20261332-05:00:00.000",
        "20270229-05:00:00.000",
        "20261030-25:00:00.000",
        "20261030-05:61:00.000",
        "20261030-05:00:00.1",
        "20261030T05:00:00.000",
    };
    for (const char* t : bad_times) {
        peer::Msg m;
        m.msg_type = "U1";
        m.sending_time = t;
        const ScanOutcome o = scan_all(m.build());
        CHECK_EQ(o.frames.size(), size_t{1});
        const WireError e = decode_of(o.frames[0], default_opts());
        CHECK_MSG(e == WireError::BadTimestamp,
                  std::string("time=") + t + " gave " + std::string(to_string(e)));
    }
    // A real leap day is accepted; 2027-02-29 above was not.
    peer::Msg leap;
    leap.msg_type = "U1";
    leap.sending_time = "20280229-05:00:00.000";
    CHECK_EQ(decode_of(leap.build(), default_opts()), WireError::None);
    for (const char* t : {"20261030-05:00:00", "20261030-05:00:00.123",
                          "20261030-05:00:00.123456", "20261030-05:00:00.123456789"}) {
        peer::Msg p;
        p.msg_type = "U1";
        p.sending_time = t;
        CHECK_MSG(decode_of(p.build(), default_opts()) == WireError::None, t);
    }
}

SCENARIO("B-24", "P0",
         "every frame the encoder produces is self-consistent under an independent checker") {
    // The serialiser owns BodyLength and CheckSum, so no caller can produce a
    // frame whose length or checksum disagrees with its own bytes.
    uint64_t seed = 20261030;
    const auto rnd = [&seed](uint64_t n) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<size_t>((seed >> 33) % n);
    };
    for (int iter = 0; iter < 2000; ++iter) {
        OutboundFields f;
        f.msg_type = (iter % 7 == 0) ? "A" : (iter % 3 == 0 ? "0" : "U1");
        f.seq_num = 1 + rnd(1000000);
        f.sender_comp_id = "TRADER01";
        f.target_comp_id = "SIMLP01";
        f.sending_time = WallTime{} + std::chrono::seconds(1793336400 + rnd(100000));
        f.poss_dup = rnd(4) == 0;
        if (f.poss_dup) f.orig_sending_time = "20261030-05:00:00.000";
        const size_t extras = rnd(6);
        for (size_t i = 0; i < extras; ++i) {
            const std::string v(1 + rnd(20), static_cast<char>('A' + rnd(26)));
            f.body.emplace_back(static_cast<int>(20000 + rnd(500)), v);
        }
        const std::string frame = encode(f);

        std::string why;
        CHECK_MSG(peer::verify_frame(frame, &why), why + " frame=" + tt::wire_text(frame));

        const ScanOutcome o = scan_all(frame);
        CHECK_EQ(o.frames.size(), size_t{1});
        DecodedMessage dm;
        CHECK_EQ(decode(o.frames[0], default_opts(), &dm), WireError::None);
        CHECK_EQ(dm.seq_num, f.seq_num);
        CHECK_EQ(std::string(dm.msg_type), f.msg_type);
        CHECK_EQ(dm.poss_dup, f.poss_dup);
    }
}

SCENARIO("B-25", "P1", "checksum rendering is correct at the 0 and 255 boundaries") {
    CHECK_EQ(checksum_string(0), std::string("000"));
    CHECK_EQ(checksum_string(255), std::string("255"));
    CHECK_EQ(checksum_string(256), std::string("000"));
    CHECK_EQ(checksum_string(7), std::string("007"));

    // Build real frames whose checksum lands on each boundary by padding a
    // filler field until the running sum reaches the wanted residue.
    for (int target : {0, 255}) {
        bool built = false;
        for (int pad = 1; pad <= 40 && !built; ++pad) {
            for (int ch = 'A'; ch <= 'Z' && !built; ++ch) {
                peer::Msg m;
                m.msg_type = "U1";
                m.seq = 1;
                m.extra.push_back(
                    {20001, std::string(static_cast<size_t>(pad), static_cast<char>(ch))});
                const std::string frame = m.build();
                if (peer::get(frame, 10).value() != peer::three_digits(target)) continue;
                const ScanOutcome o = scan_all(frame);
                CHECK_EQ(o.frames.size(), size_t{1});
                CHECK_EQ(decode_of(o.frames[0], default_opts()), WireError::None);
                built = true;
            }
        }
        CHECK_MSG(built, std::string("no frame found with checksum ") + std::to_string(target));
    }
}

SCENARIO("B-26", "P1", "garbage before a valid frame is not silently skipped") {
    const std::string stream = std::string("JUNKJUNK") + app_frame(1);
    const ScanOutcome o = scan_all(stream);
    CHECK_EQ(o.frames.size(), size_t{0});
    CHECK_EQ(o.last_error, WireError::MissingBeginString);
}

SCENARIO("B-28", "P0", "the receive buffer cap is enforced instead of growing without bound") {
    // Slowloris shape: a frame header that never completes. The framer refuses
    // further bytes once the cap is reached rather than allocating (J-05).
    FixFramer fr(1024, 2048);
    const std::string prefix = std::string("8=FIX.4.4") + peer::SOH + "9=900" + peer::SOH;
    CHECK_EQ(fr.append(prefix), WireError::None);
    WireError last = WireError::None;
    size_t appended = prefix.size();
    while (last == WireError::None) {
        last = fr.append(std::string(64, 'x'));
        if (last == WireError::None) appended += 64;
    }
    CHECK_EQ(last, WireError::FrameTooLarge);
    CHECK(appended <= size_t{2048});
    CHECK(fr.buffered() <= size_t{2048});
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "framing_codec"); }
