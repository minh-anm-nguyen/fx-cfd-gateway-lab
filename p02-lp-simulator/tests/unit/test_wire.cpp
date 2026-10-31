// Wire layer: framing, decoding, encoding and fixed-point arithmetic.
//
// Matrix coverage: B-06 to B-09 and B-15 to B-19 (the frame and numeric half of
// group B), D-19, E-04, E-19 and the K-06/K-07 chunking properties.
//
// Everything here runs without a store or a socket, which is test layer 1 of
// design doc 18.2: price and quantity arithmetic, the business-field parser and
// the frame boundary rules.

#include <string>
#include <vector>

#include "lp/clock.hpp"
#include "lp/codec.hpp"
#include "lp/fixed_point.hpp"
#include "lp/framer.hpp"
#include "../support/tiny_test.hpp"

using namespace lpsim;

namespace {

const PriceFormat kFx5{5, 1};

std::string soh(std::string s) {
    for (char& c : s) {
        if (c == '|') c = kSOH;
    }
    return s;
}

// Build a well-formed frame from a body so the tests are readable.
std::string frame(const std::string& body_with_bars) {
    const std::string body = soh(body_with_bars);
    std::string head = "8=FIX.4.4";
    head.push_back(kSOH);
    head += "9=" + std::to_string(body.size());
    head.push_back(kSOH);
    head += body;
    const int sum = checksum_of(head);
    head += "10=" + checksum_string(sum);
    head.push_back(kSOH);
    return head;
}

const char* kLogonBody =
    "35=A|49=TRADER01|56=SIMLP01|34=1|52=20261030-05:00:00.000|98=0|108=30|";

std::string field_or_empty(const DecodedMessage& m, int tag_num) {
    const auto v = m.find(tag_num);
    return v ? std::string(*v) : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

SCENARIO("K-07", "P0", "a frame split into single bytes is reassembled exactly once") {
    const std::string f = frame(kLogonBody);
    FixFramer fr(65536, 131072);
    for (std::size_t i = 0; i < f.size(); ++i) {
        CHECK_EQ(fr.append(f.substr(i, 1)), WireError::None);
        std::string_view out;
        WireError e = WireError::None;
        const bool got = fr.next(&out, &e);
        if (i + 1 < f.size()) {
            // No premature delivery: a partial frame yields Incomplete, never a
            // guess at where the message ends.
            CHECK(!got);
            CHECK_EQ(e, WireError::Incomplete);
        } else {
            CHECK(got);
            CHECK_EQ(std::string(out), f);
        }
    }
    CHECK_EQ(fr.frames_parsed(), static_cast<std::size_t>(1));
}

SCENARIO("K-06", "P0", "a hundred coalesced frames are delivered individually and in order") {
    std::string blob;
    const int kCount = 100;
    for (int i = 1; i <= kCount; ++i) {
        blob += frame("35=0|49=TRADER01|56=SIMLP01|34=" + std::to_string(i) +
                      "|52=20261030-05:00:00.000|");
    }
    FixFramer fr(65536, 1 << 20);
    CHECK_EQ(fr.append(blob), WireError::None);

    int seen = 0;
    while (true) {
        std::string_view out;
        WireError e = WireError::None;
        if (!fr.next(&out, &e)) {
            CHECK_EQ(e, WireError::Incomplete);
            break;
        }
        ++seen;
        DecodedMessage m;
        CHECK_EQ(decode(out, DecodeOptions{}, &m), WireError::None);
        CHECK_EQ(m.seq_num, static_cast<std::uint64_t>(seen));
    }
    CHECK_EQ(seen, kCount);
}

SCENARIO("B-15", "P0", "a BodyLength that does not reach the trailer is refused") {
    std::string f = frame(kLogonBody);
    const std::size_t b = f.find("9=");
    const std::size_t e = f.find(kSOH, b);
    f.replace(b, e - b, "9=7");

    FixFramer fr(65536, 131072);
    CHECK_EQ(fr.append(f), WireError::None);
    std::string_view out;
    WireError err = WireError::None;
    CHECK(!fr.next(&out, &err));
    CHECK_EQ(err, WireError::BodyLengthMismatch);
}

SCENARIO("B-16", "P0", "a wrong CheckSum is refused and nothing is delivered") {
    std::string f = frame(kLogonBody);
    const std::size_t p = f.rfind("10=");
    f[p + 3] = f[p + 3] == '0' ? '1' : '0';

    FixFramer fr(65536, 131072);
    CHECK_EQ(fr.append(f), WireError::None);
    std::string_view out;
    WireError err = WireError::None;
    CHECK(!fr.next(&out, &err));
    CHECK_EQ(err, WireError::CheckSumMismatch);
    CHECK_EQ(fr.frames_parsed(), static_cast<std::size_t>(0));
}

SCENARIO("L-08", "P0", "an enormous declared BodyLength allocates nothing") {
    std::string f = "8=FIX.4.4";
    f.push_back(kSOH);
    f += "9=1000000000";
    f.push_back(kSOH);
    f += "35=A";
    f.push_back(kSOH);

    FixFramer fr(65536, 131072);
    CHECK_EQ(fr.append(f), WireError::None);
    std::string_view out;
    WireError err = WireError::None;
    CHECK(!fr.next(&out, &err));
    // Refused for being past the frame cap, before any buffer is reserved.
    CHECK_EQ(err, WireError::BodyLengthTooLarge);
    CHECK(fr.buffered() < 64);
}

SCENARIO("L-08b", "P0", "an unterminated frame cannot grow past the buffer cap") {
    FixFramer fr(1024, 2048);
    const std::string chunk(512, 'x');
    WireError e = WireError::None;
    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        e = fr.append(chunk);
        if (e != WireError::None) break;
        ++accepted;
    }
    CHECK_EQ(e, WireError::FrameTooLarge);
    CHECK(accepted <= 4);
}

SCENARIO("A-05", "P0", "a FIX.4.2 BeginString is refused at the frame boundary") {
    std::string f = frame(kLogonBody);
    const std::size_t p = f.find("8=FIX.4.4");
    f.replace(p, 9, "8=FIX.4.2");

    FixFramer fr(65536, 131072);
    CHECK_EQ(fr.append(f), WireError::None);
    std::string_view out;
    WireError err = WireError::None;
    CHECK(!fr.next(&out, &err));
    CHECK_EQ(err, WireError::BadBeginString);
}

SCENARIO("B-15b", "P0", "the bytes 10= inside a field value do not terminate a frame") {
    // Tag 58 carries a value containing "10=", which a scanner looking for the
    // trailer rather than honouring BodyLength would mistake for the checksum.
    const std::string f =
        frame("35=3|49=TRADER01|56=SIMLP01|34=4|52=20261030-05:00:00.000|58=x10=999x|");
    FixFramer fr(65536, 131072);
    CHECK_EQ(fr.append(f), WireError::None);
    std::string_view out;
    WireError err = WireError::None;
    CHECK(fr.next(&out, &err));
    CHECK_EQ(std::string(out), f);

    DecodedMessage m;
    CHECK_EQ(decode(out, DecodeOptions{}, &m), WireError::None);
    CHECK_EQ(field_or_empty(m, tag::Text), std::string("x10=999x"));
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

SCENARIO("B-18", "P0", "a repeated mandatory header tag is refused, not resolved") {
    const std::string f = frame(
        "35=A|49=TRADER01|56=SIMLP01|34=1|34=2|52=20261030-05:00:00.000|98=0|108=30|");
    DecodedMessage m;
    CHECK_EQ(decode(f, DecodeOptions{}, &m), WireError::DuplicateHeaderTag);
}

SCENARIO("B-19", "P0", "a control byte in a field value is refused") {
    std::string body = soh("35=A|49=TRADER01|56=SIMLP01|34=1|52=20261030-05:00:00.000|58=a");
    body.push_back('\x07');  // BEL
    body.push_back(kSOH);
    std::string head = "8=FIX.4.4";
    head.push_back(kSOH);
    head += "9=" + std::to_string(body.size());
    head.push_back(kSOH);
    head += body;
    head += "10=" + checksum_string(checksum_of(head));
    head.push_back(kSOH);

    DecodedMessage m;
    CHECK_EQ(decode(head, DecodeOptions{}, &m), WireError::NonPrintableValue);
}

SCENARIO("B-19b", "P0", "an oversized field value is refused without being copied") {
    const std::string huge(5000, 'A');
    const std::string f =
        frame("35=D|49=TRADER01|56=SIMLP01|34=1|52=20261030-05:00:00.000|11=" + huge + "|");
    DecodedMessage m;
    DecodeOptions opt;
    opt.max_value_bytes = 256;
    CHECK_EQ(decode(f, opt, &m), WireError::ValueTooLong);
}

SCENARIO("I-18", "P1", "PossDup without OrigSendingTime is refused") {
    const std::string f =
        frame("35=D|49=TRADER01|56=SIMLP01|34=5|52=20261030-05:00:00.000|43=Y|11=O1|");
    DecodedMessage m;
    CHECK_EQ(decode(f, DecodeOptions{}, &m), WireError::MissingRequiredHeader);
}

SCENARIO("B-23", "P0", "an impossible calendar date in SendingTime is refused") {
    const std::string f =
        frame("35=A|49=TRADER01|56=SIMLP01|34=1|52=20260230-05:00:00.000|98=0|108=30|");
    DecodedMessage m;
    CHECK_EQ(decode(f, DecodeOptions{}, &m), WireError::BadTimestamp);
}

SCENARIO("B-23b", "P0", "leap-year handling is calendar-correct in both directions") {
    WallTime t{};
    // 2026 is not a leap year, so 29 February does not exist in it.
    CHECK(!parse_utc_timestamp("20260229-23:59:59.123", &t));
    // 2024 is, and the value round-trips through format unchanged.
    CHECK(parse_utc_timestamp("20240229-12:00:00.000", &t));
    CHECK_EQ(format_utc_timestamp(t), std::string("20240229-12:00:00.000"));
    // 2100 is divisible by 100 but not 400, so it is not a leap year.
    CHECK(!parse_utc_timestamp("21000229-00:00:00.000", &t));
    // 2000 is divisible by 400, so it is.
    CHECK(parse_utc_timestamp("20000229-00:00:00.000", &t));
}

SCENARIO("B-23c", "P0", "fractional-second precision is accepted only in legal widths") {
    WallTime t{};
    CHECK(parse_utc_timestamp("20261030-05:00:00", &t));          // no fraction
    CHECK(parse_utc_timestamp("20261030-05:00:00.123", &t));      // milliseconds
    CHECK(parse_utc_timestamp("20261030-05:00:00.123456", &t));   // microseconds
    CHECK(parse_utc_timestamp("20261030-05:00:00.123456789", &t));
    CHECK(!parse_utc_timestamp("20261030-05:00:00.1", &t));       // two digits short
    CHECK(!parse_utc_timestamp("20261030-05:00:00.12345", &t));
    CHECK(!parse_utc_timestamp("20261030-25:00:00.000", &t));     // hour out of range
    CHECK(!parse_utc_timestamp("20261030-05:60:00.000", &t));     // minute out of range
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

SCENARIO("M-06", "P0", "an encoded frame always agrees with its own length and checksum") {
    // The encoder owns tags 9 and 10, so this is a property of every message it
    // can produce, not of one fixture.
    for (int i = 1; i <= 50; ++i) {
        OutboundFields f;
        f.msg_type = "8";
        f.seq_num = static_cast<std::uint64_t>(i);
        f.sender_comp_id = "SIMLP01";
        f.target_comp_id = "TRADER01";
        f.sending_time = WallTime{} + std::chrono::seconds(1793336400 + i);
        f.body.emplace_back(tag::OrderID, "SIM-" + std::to_string(1000 + i));
        f.body.emplace_back(tag::Text, std::string(static_cast<std::size_t>(i), 'z'));
        const std::string raw = encode(f);

        FixFramer fr(65536, 1 << 20);
        CHECK_EQ(fr.append(raw), WireError::None);
        std::string_view out;
        WireError e = WireError::None;
        CHECK_MSG(fr.next(&out, &e), to_string(e));
        CHECK_EQ(std::string(out), raw);
    }
}

SCENARIO("I-06e", "P0", "a retransmission keeps the original sequence and marks 43/122") {
    OutboundFields f;
    f.msg_type = "8";
    f.seq_num = 80;
    f.sender_comp_id = "SIMLP01";
    f.target_comp_id = "TRADER01";
    f.sending_time = WallTime{} + std::chrono::seconds(1793336400);
    f.body.emplace_back(tag::OrderID, "SIM-1001");
    f.body.emplace_back(tag::ExecID, "EX-000002");
    f.body.emplace_back(tag::ExecType, std::string(exec_type::Trade));
    const std::string original = encode(f);

    std::string replay;
    const WallTime later = WallTime{} + std::chrono::seconds(1793336460);
    CHECK(rebuild_as_retransmission(original, later, &replay));

    DecodedMessage m;
    DecodeOptions opt;
    opt.max_value_bytes = 4096;
    CHECK_EQ(decode(replay, opt, &m), WireError::None);
    // The original sequence number and the original ExecID both survive: a
    // replay is the same message again, not a new one (I-05, I-12).
    CHECK_EQ(m.seq_num, static_cast<std::uint64_t>(80));
    CHECK(m.poss_dup);
    CHECK_EQ(field_or_empty(m, tag::ExecID), std::string("EX-000002"));
    CHECK_EQ(field_or_empty(m, tag::OrigSendingTime),
             std::string("20261030-05:00:00.000"));
    CHECK_EQ(field_or_empty(m, tag::SendingTime), std::string("20261030-05:01:00.000"));
}

SCENARIO("I-06f", "P1", "replaying a replay still points at the first transmission") {
    OutboundFields f;
    f.msg_type = "8";
    f.seq_num = 80;
    f.sender_comp_id = "SIMLP01";
    f.target_comp_id = "TRADER01";
    f.sending_time = WallTime{} + std::chrono::seconds(1793336400);
    f.body.emplace_back(tag::OrderID, "SIM-1001");
    const std::string original = encode(f);

    std::string once, twice;
    CHECK(rebuild_as_retransmission(original, WallTime{} + std::chrono::seconds(1793336460),
                                    &once));
    CHECK(rebuild_as_retransmission(once, WallTime{} + std::chrono::seconds(1793336520),
                                    &twice));

    DecodedMessage m;
    DecodeOptions opt;
    opt.max_value_bytes = 4096;
    CHECK_EQ(decode(twice, opt, &m), WireError::None);
    CHECK_EQ(m.seq_num, static_cast<std::uint64_t>(80));
    CHECK_EQ(field_or_empty(m, tag::OrigSendingTime),
             std::string("20261030-05:00:00.000"));
}

// ---------------------------------------------------------------------------
// Fixed point
// ---------------------------------------------------------------------------

SCENARIO("B-07", "P0", "a non-numeric price is refused and never becomes zero") {
    Ticks t = -1;
    CHECK_EQ(parse_price_ticks("abc", kFx5, &t), NumParse::NotNumeric);
    CHECK_EQ(parse_price_ticks("NaN", kFx5, &t), NumParse::NotNumeric);
    CHECK_EQ(parse_price_ticks("1e999", kFx5, &t), NumParse::NotNumeric);
    CHECK_EQ(parse_price_ticks("1.0.5", kFx5, &t), NumParse::NotNumeric);
    CHECK_EQ(parse_price_ticks("", kFx5, &t), NumParse::Empty);
    CHECK_EQ(parse_price_ticks("-1.08500", kFx5, &t), NumParse::NotPositive);
    CHECK_EQ(parse_price_ticks("0.00000", kFx5, &t), NumParse::NotPositive);
    // Every refusal left the output untouched.
    CHECK_EQ(t, static_cast<Ticks>(-1));
}

SCENARIO("B-08", "P0", "more precision than the symbol carries is refused, not rounded") {
    Ticks t = 0;
    CHECK_EQ(parse_price_ticks("1.085005", kFx5, &t), NumParse::TooManyDecimals);
    CHECK_EQ(parse_price_ticks("1.08500", kFx5, &t), NumParse::Ok);
    CHECK_EQ(t, static_cast<Ticks>(108500));
    CHECK_EQ(parse_price_ticks("1.085", kFx5, &t), NumParse::Ok);
    CHECK_EQ(t, static_cast<Ticks>(108500));
}

SCENARIO("B-08b", "P0", "an off-tick price is detected separately from a malformed one") {
    const PriceFormat half_pip{5, 5};  // only multiples of 0.00005 are accepted
    Ticks t = 0;
    CHECK_EQ(parse_price_ticks("1.08502", half_pip, &t), NumParse::Ok);
    CHECK(!on_tick(t, half_pip));
    CHECK_EQ(parse_price_ticks("1.08505", half_pip, &t), NumParse::Ok);
    CHECK(on_tick(t, half_pip));
}

SCENARIO("B-06", "P0", "zero and negative quantities are refused") {
    Units q = -1;
    CHECK_EQ(parse_qty_units("0", &q), NumParse::NotPositive);
    CHECK_EQ(parse_qty_units("-1000", &q), NumParse::NotPositive);
    CHECK_EQ(q, static_cast<Units>(-1));
    CHECK_EQ(parse_qty_units("100000", &q), NumParse::Ok);
    CHECK_EQ(q, static_cast<Units>(100000));
    // A trailing all-zero fraction is accepted; a real fraction is not.
    CHECK_EQ(parse_qty_units("100000.00", &q), NumParse::Ok);
    CHECK_EQ(q, static_cast<Units>(100000));
    CHECK_EQ(parse_qty_units("100000.50", &q), NumParse::FractionalQty);
}

SCENARIO("B-09", "P0", "a quantity beyond the integer domain is refused before arithmetic") {
    Units q = 0;
    CHECK_EQ(parse_qty_units("9999999999999999999999", &q), NumParse::Overflow);
    // One past int64 max.
    CHECK_EQ(parse_qty_units("9223372036854775808", &q), NumParse::Overflow);
    CHECK_EQ(parse_qty_units("9223372036854775807", &q), NumParse::Ok);
}

SCENARIO("B-09b", "P0", "a price whose scaled value overflows is refused") {
    Ticks t = 0;
    CHECK_EQ(parse_price_ticks("99999999999999999", kFx5, &t), NumParse::Overflow);
}

SCENARIO("E-04", "P0", "AvgPx is the exact weighted average of the fills") {
    // The worked example from design doc 7.4: 40k at 1.08490 then 60k at
    // 1.08500 gives 1.08496 exactly.
    NotionalAccumulator acc;
    CHECK(acc.add(40000, 108490));
    CHECK(acc.add(60000, 108500));
    CHECK_EQ(acc.qty(), static_cast<Units>(100000));
    CHECK_EQ(acc.avg_px_ticks(), static_cast<Ticks>(108496));
    CHECK_EQ(format_price(acc.avg_px_ticks(), 5), std::string("1.08496"));
}

SCENARIO("E-19", "P1", "AvgPx rounding at a half-tick boundary is round-half-up") {
    // 1 at 100 and 1 at 101 averages to 100.5 ticks, which rounds to 101.
    NotionalAccumulator acc;
    CHECK(acc.add(1, 100));
    CHECK(acc.add(1, 101));
    CHECK_EQ(acc.avg_px_ticks(), static_cast<Ticks>(101));

    // 1 at 100 and 2 at 101 averages to 100.67, which rounds to 101.
    NotionalAccumulator b;
    CHECK(b.add(1, 100));
    CHECK(b.add(2, 101));
    CHECK_EQ(b.avg_px_ticks(), static_cast<Ticks>(101));

    // 2 at 100 and 1 at 101 averages to 100.33, which rounds to 100.
    NotionalAccumulator c;
    CHECK(c.add(2, 100));
    CHECK(c.add(1, 101));
    CHECK_EQ(c.avg_px_ticks(), static_cast<Ticks>(100));
}

SCENARIO("E-19b", "P1", "an empty accumulator reports AvgPx zero rather than undefined") {
    NotionalAccumulator acc;
    CHECK(acc.empty());
    CHECK_EQ(acc.avg_px_ticks(), static_cast<Ticks>(0));
    CHECK_EQ(format_price(0, 5), std::string("0.00000"));
}

SCENARIO("D-19", "P0", "an overflowing weighted notional is refused, not wrapped") {
    // One int64 quantity times one int64 price always fits in 128 bits: the
    // largest possible product is about 8.5e37 against a domain of 1.7e38. So
    // a single fill cannot overflow the notional, and asserting that it does
    // would be asserting a false premise. What CAN overflow is the running sum
    // across fills, and the quantity sum, and both are refused here.
    const Units huge_qty = 4000000000000000000LL;
    const Ticks huge_px = 4000000000000000000LL;

    NotionalAccumulator acc;
    CHECK(acc.add(huge_qty, huge_px));  // 1.6e37, accepted
    CHECK_EQ(acc.qty(), huge_qty);
    CHECK(acc.add(huge_qty, huge_px));  // 3.2e37, still inside the domain
    CHECK_EQ(acc.qty(), static_cast<Units>(8000000000000000000LL));
    CHECK_EQ(acc.avg_px_ticks(), huge_px);

    // The third add would take the QUANTITY sum past int64, and that is what
    // refuses it. The accumulator is left exactly as it was.
    //
    // Worth stating plainly, because the design doc's D-19 presumes the
    // notional sum is the binding bound and it is not: with int64 quantities
    // and prices, the sum of qty*px can never exceed about 8.5e37 against a
    // 128-bit domain of 1.7e38. The int128 guard in NotionalAccumulator is
    // defence in depth; the quantity sum is the constraint that actually
    // fires. The guard is kept because it is cheap and because it stops being
    // unreachable the moment the quantity type widens.
    CHECK(!acc.add(huge_qty, huge_px));
    CHECK_EQ(acc.qty(), static_cast<Units>(8000000000000000000LL));
    CHECK_EQ(acc.avg_px_ticks(), huge_px);

    // A notional that fits but whose quantity sum does not is also refused.
    NotionalAccumulator qty_only;
    CHECK(qty_only.add(9223372036854775807LL, 1));
    CHECK(!qty_only.add(1, 1));
    CHECK_EQ(qty_only.qty(), static_cast<Units>(9223372036854775807LL));

    // A zero or negative input is refused outright.
    NotionalAccumulator fresh;
    CHECK(!fresh.add(0, 100));
    CHECK(!fresh.add(100, 0));
    CHECK(!fresh.add(-1, 100));
    CHECK_EQ(fresh.qty(), static_cast<Units>(0));
}

SCENARIO("D-19b", "P0", "checked arithmetic reports overflow instead of wrapping") {
    std::int64_t out = 0;
    CHECK(checked_add(1, 2, &out));
    CHECK_EQ(out, static_cast<std::int64_t>(3));
    CHECK(!checked_add(9223372036854775807LL, 1, &out));
    CHECK(!checked_mul(4611686018427387904LL, 4, &out));
    CHECK(checked_sub(5, 7, &out));
    CHECK_EQ(out, static_cast<std::int64_t>(-2));
}

SCENARIO("E-02f", "P0", "prices and quantities format exactly as the wire needs them") {
    CHECK_EQ(format_price(108500, 5), std::string("1.08500"));
    CHECK_EQ(format_price(108490, 5), std::string("1.08490"));
    CHECK_EQ(format_price(1, 5), std::string("0.00001"));
    CHECK_EQ(format_qty(0), std::string("0"));
    CHECK_EQ(format_qty(100000), std::string("100000"));
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "wire"); }
