#include "fix/codec.hpp"

#include <algorithm>
#include <cstring>

namespace fixse {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool parse_bounded_u64(std::string_view s, size_t max_digits, uint64_t* out) {
    if (s.empty() || s.size() > max_digits) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    *out = v;
    return true;
}

bool printable_value(std::string_view v) {
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return false;
    }
    return true;
}

// A boolean FIX field must be exactly Y or N; anything else is an error rather
// than a falsy default (B-19).
bool parse_bool(std::string_view v, bool* out) {
    if (v == "Y") {
        *out = true;
        return true;
    }
    if (v == "N") {
        *out = false;
        return true;
    }
    return false;
}

constexpr int kUniqueHeaderTags[] = {
    tag::BeginString,  tag::BodyLength,      tag::MsgType,     tag::MsgSeqNum,
    tag::SenderCompID, tag::TargetCompID,    tag::SendingTime, tag::CheckSum,
    tag::PossDupFlag,  tag::OrigSendingTime, tag::PossResend};

}  // namespace

std::optional<std::string_view> DecodedMessage::find(int t) const {
    for (const Field& f : fields)
        if (f.tag == t) return f.value;
    return std::nullopt;
}

int DecodedMessage::count(int t) const {
    int n = 0;
    for (const Field& f : fields)
        if (f.tag == t) ++n;
    return n;
}

bool DecodedMessage::find_u64(int t, uint64_t* out) const {
    auto v = find(t);
    if (!v) return false;
    return parse_bounded_u64(*v, 19, out);
}

bool DecodedMessage::find_int(int t, int* out) const {
    uint64_t v = 0;
    if (!find_u64(t, &v)) return false;
    if (v > 0x7FFFFFFFull) return false;
    *out = static_cast<int>(v);
    return true;
}

WireError decode(std::string_view raw, const DecodeOptions& opt, DecodedMessage* out) {
    out->raw = raw;
    out->fields.clear();
    out->poss_dup = false;
    out->poss_resend = false;
    out->orig_sending_time.reset();
    out->orig_sending_time_raw = {};
    out->seq_num = 0;

    size_t pos = 0;
    int field_count = 0;
    while (pos < raw.size()) {
        const size_t eq = raw.find('=', pos);
        if (eq == std::string_view::npos) return WireError::BadTagNumber;
        const std::string_view tag_text = raw.substr(pos, eq - pos);
        uint64_t tag_num = 0;
        // A tag id longer than nine digits is rejected without arithmetic (B-21).
        if (!parse_bounded_u64(tag_text, 9, &tag_num) || tag_num == 0)
            return WireError::BadTagNumber;

        const size_t soh = raw.find(kSOH, eq + 1);
        if (soh == std::string_view::npos) return WireError::BodyLengthMismatch;
        const std::string_view value = raw.substr(eq + 1, soh - eq - 1);
        if (value.empty()) return WireError::EmptyTagValue;
        if (!printable_value(value)) return WireError::NonPrintableValue;

        if (++field_count > opt.max_fields) return WireError::TooManyFields;
        out->fields.push_back(Field{static_cast<int>(tag_num), value});
        pos = soh + 1;
    }

    // Structural requirements: 8, 9, 35 are the first three fields and 10 is
    // last (design doc 5.2). Strict ordering is deliberate: a profile that
    // tolerates a shuffled header cannot tell a broken peer from an attacker
    // (B-13).
    if (out->fields.size() < 5) return WireError::MissingRequiredHeader;
    if (out->fields[0].tag != tag::BeginString) return WireError::MissingBeginString;
    if (out->fields[0].value != opt.begin_string) return WireError::BadBeginString;
    if (out->fields[1].tag != tag::BodyLength) return WireError::MissingBodyLength;
    if (out->fields[2].tag != tag::MsgType) return WireError::MissingMsgType;
    if (out->fields.back().tag != tag::CheckSum) return WireError::BadCheckSumFormat;

    for (int t : kUniqueHeaderTags) {
        if (out->count(t) > 1) return WireError::DuplicateHeaderTag;
    }

    out->msg_type = out->fields[2].value;

    auto sender = out->find(tag::SenderCompID);
    auto target = out->find(tag::TargetCompID);
    auto seq = out->find(tag::MsgSeqNum);
    auto stime = out->find(tag::SendingTime);
    if (!sender || !target || !seq || !stime) return WireError::MissingRequiredHeader;
    out->sender_comp_id = *sender;
    out->target_comp_id = *target;

    uint64_t seq_num = 0;
    // MsgSeqNum is 1-based and must never wrap: at the uint64 boundary the
    // engine fails closed rather than rolling over (E-19).
    if (!parse_bounded_u64(*seq, 19, &seq_num) || seq_num == 0) return WireError::BadSeqNum;
    out->seq_num = seq_num;

    out->sending_time_raw = *stime;
    if (!parse_utc_timestamp(*stime, &out->sending_time)) return WireError::BadTimestamp;

    if (auto pd = out->find(tag::PossDupFlag)) {
        if (!parse_bool(*pd, &out->poss_dup)) return WireError::BadFieldValue;
    }
    if (auto pr = out->find(tag::PossResend)) {
        if (!parse_bool(*pr, &out->poss_resend)) return WireError::BadFieldValue;
    }
    if (auto ost = out->find(tag::OrigSendingTime)) {
        WallTime t{};
        if (!parse_utc_timestamp(*ost, &t)) return WireError::BadTimestamp;
        out->orig_sending_time = t;
        out->orig_sending_time_raw = *ost;
    } else if (out->poss_dup) {
        // FIX 4.4 requires OrigSendingTime whenever PossDupFlag is Y. Taking a
        // retransmission without it would mean trusting a duplicate whose
        // provenance cannot be checked (E-06).
        return WireError::MissingRequiredHeader;
    }

    if (opt.allowed_app_msg_types != nullptr && !opt.allowed_app_msg_types->empty() &&
        is_application_message(out->msg_type)) {
        const auto& allowed = *opt.allowed_app_msg_types;
        if (std::find(allowed.begin(), allowed.end(), std::string(out->msg_type)) == allowed.end())
            return WireError::UnsupportedMsgType;
    }

    return WireError::None;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

namespace {

void append_field(std::string* s, int t, std::string_view v) {
    s->append(std::to_string(t));
    s->push_back('=');
    s->append(v);
    s->push_back(kSOH);
}

std::string finish_frame(std::string_view begin_string, const std::string& body) {
    std::string frame;
    frame.reserve(body.size() + 32);
    append_field(&frame, tag::BeginString, begin_string);
    // BodyLength counts the bytes from just after its own delimiter up to and
    // including the delimiter before the trailer, which is exactly the body
    // assembled by the caller (design doc 5.2).
    append_field(&frame, tag::BodyLength, std::to_string(body.size()));
    frame.append(body);
    append_field(&frame, tag::CheckSum, checksum_string(checksum_of(frame)));
    return frame;
}

}  // namespace

std::string encode(const OutboundFields& f) {
    std::string body;
    append_field(&body, tag::MsgType, f.msg_type);
    append_field(&body, tag::SenderCompID, f.sender_comp_id);
    append_field(&body, tag::TargetCompID, f.target_comp_id);
    append_field(&body, tag::MsgSeqNum, std::to_string(f.seq_num));
    append_field(&body, tag::SendingTime, format_utc_timestamp(f.sending_time));
    if (f.poss_dup) {
        append_field(&body, tag::PossDupFlag, "Y");
        append_field(&body, tag::OrigSendingTime, f.orig_sending_time);
    }
    for (const auto& kv : f.body) append_field(&body, kv.first, kv.second);
    return finish_frame(kBeginStringFix44, body);
}

bool rebuild_as_retransmission(std::string_view original_raw, WallTime new_sending_time,
                               std::string* out) {
    DecodeOptions opt;
    DecodedMessage msg;
    // A stored frame that no longer decodes is corrupt history: the caller must
    // fail closed instead of putting unverified bytes on the wire (F-17).
    if (decode(original_raw, opt, &msg) != WireError::None) return false;

    const std::string orig_sending_time(msg.sending_time_raw);
    std::string body;
    append_field(&body, tag::MsgType, msg.msg_type);
    append_field(&body, tag::SenderCompID, msg.sender_comp_id);
    append_field(&body, tag::TargetCompID, msg.target_comp_id);
    // The retransmission keeps the ORIGINAL sequence number: a resend never
    // consumes a new one (invariant I-05).
    append_field(&body, tag::MsgSeqNum, std::to_string(msg.seq_num));
    append_field(&body, tag::SendingTime, format_utc_timestamp(new_sending_time));
    append_field(&body, tag::PossDupFlag, "Y");
    append_field(&body, tag::OrigSendingTime, orig_sending_time);

    for (const Field& fld : msg.fields) {
        switch (fld.tag) {
            case tag::BeginString:
            case tag::BodyLength:
            case tag::MsgType:
            case tag::MsgSeqNum:
            case tag::SenderCompID:
            case tag::TargetCompID:
            case tag::SendingTime:
            case tag::PossDupFlag:
            case tag::OrigSendingTime:
            case tag::CheckSum:
                continue;
            default:
                append_field(&body, fld.tag, fld.value);
        }
    }

    *out = finish_frame(kBeginStringFix44, body);
    return true;
}

}  // namespace fixse
