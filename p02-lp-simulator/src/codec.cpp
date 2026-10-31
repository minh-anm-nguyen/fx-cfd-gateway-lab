#include "lp/codec.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace lpsim {

std::optional<std::string_view> DecodedMessage::find(int tag) const {
    for (const Field& f : fields) {
        if (f.tag == tag) return f.value;
    }
    return std::nullopt;
}

int DecodedMessage::count(int tag) const {
    int n = 0;
    for (const Field& f : fields) {
        if (f.tag == tag) ++n;
    }
    return n;
}

bool DecodedMessage::find_u64(int tag, std::uint64_t* out) const {
    const auto v = find(tag);
    if (!v || v->empty()) return false;
    std::uint64_t acc = 0;
    for (char c : *v) {
        if (c < '0' || c > '9') return false;
        if (acc > (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(c - '0')) / 10)
            return false;
        acc = acc * 10 + static_cast<std::uint64_t>(c - '0');
    }
    *out = acc;
    return true;
}

bool DecodedMessage::find_int(int tag, int* out) const {
    std::uint64_t v = 0;
    if (!find_u64(tag, &v)) return false;
    if (v > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return false;
    *out = static_cast<int>(v);
    return true;
}

namespace {

bool printable_value(std::string_view v) {
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return false;
    }
    return true;
}

// A mandatory header tag may appear exactly once. Repeating any of these is
// rejected outright (B-18) rather than resolved by position.
bool is_unique_header_tag(int t) {
    switch (t) {
        case tag::BeginString:
        case tag::BodyLength:
        case tag::MsgType:
        case tag::MsgSeqNum:
        case tag::SenderCompID:
        case tag::TargetCompID:
        case tag::SendingTime:
        case tag::PossDupFlag:
        case tag::PossResend:
        case tag::OrigSendingTime:
        case tag::CheckSum:
            return true;
        default:
            return false;
    }
}

}  // namespace

WireError decode(std::string_view raw, const DecodeOptions& opt, DecodedMessage* out) {
    out->raw = raw;
    out->fields.clear();
    out->msg_type = {};
    out->sender_comp_id = {};
    out->target_comp_id = {};
    out->seq_num = 0;
    out->sending_time = WallTime{};
    out->sending_time_raw = {};
    out->poss_dup = false;
    out->poss_resend = false;
    out->orig_sending_time.reset();
    out->orig_sending_time_raw = {};

    std::size_t pos = 0;
    int field_count = 0;
    while (pos < raw.size()) {
        // tag
        const std::size_t eq = raw.find('=', pos);
        if (eq == std::string_view::npos) return WireError::BadTagNumber;
        if (eq == pos) return WireError::BadTagNumber;

        long long tag_num = 0;
        for (std::size_t i = pos; i < eq; ++i) {
            const char c = raw[i];
            if (c < '0' || c > '9') return WireError::BadTagNumber;
            tag_num = tag_num * 10 + (c - '0');
            if (tag_num > 999999) return WireError::BadTagNumber;
        }
        if (tag_num == 0) return WireError::BadTagNumber;

        // value
        const std::size_t soh = raw.find(kSOH, eq + 1);
        if (soh == std::string_view::npos) return WireError::BodyLengthMismatch;
        const std::string_view value = raw.substr(eq + 1, soh - eq - 1);
        if (value.empty()) return WireError::EmptyTagValue;
        if (value.size() > opt.max_value_bytes) return WireError::ValueTooLong;
        if (!printable_value(value)) return WireError::NonPrintableValue;

        if (++field_count > opt.max_fields) return WireError::TooManyFields;

        const int t = static_cast<int>(tag_num);
        if (is_unique_header_tag(t)) {
            for (const Field& f : out->fields) {
                if (f.tag == t) return WireError::DuplicateHeaderTag;
            }
        }
        out->fields.push_back(Field{t, value});
        pos = soh + 1;
    }

    // --- Standard header ordering ------------------------------------------
    if (out->fields.size() < 4) return WireError::MissingRequiredHeader;
    if (out->fields[0].tag != tag::BeginString) return WireError::MissingBeginString;
    if (out->fields[0].value != opt.begin_string) return WireError::BadBeginString;
    if (out->fields[1].tag != tag::BodyLength) return WireError::MissingBodyLength;
    // FIX 4.4 requires MsgType to be the third field of the standard header.
    if (out->fields[2].tag != tag::MsgType) return WireError::MissingMsgType;
    out->msg_type = out->fields[2].value;

    const auto sender = out->find(tag::SenderCompID);
    const auto target = out->find(tag::TargetCompID);
    const auto sending = out->find(tag::SendingTime);
    if (!sender || !target || !sending) return WireError::MissingRequiredHeader;
    out->sender_comp_id = *sender;
    out->target_comp_id = *target;
    out->sending_time_raw = *sending;

    if (!out->find_u64(tag::MsgSeqNum, &out->seq_num)) return WireError::BadSeqNum;
    if (out->seq_num == 0) return WireError::BadSeqNum;

    if (!parse_utc_timestamp(out->sending_time_raw, &out->sending_time))
        return WireError::BadTimestamp;

    // --- PossDup / PossResend ---------------------------------------------
    if (const auto pd = out->find(tag::PossDupFlag)) {
        if (*pd == "Y") out->poss_dup = true;
        else if (*pd == "N") out->poss_dup = false;
        else return WireError::NonPrintableValue;  // illegal enumerated value
    }
    if (const auto pr = out->find(tag::PossResend)) {
        if (*pr == "Y") out->poss_resend = true;
        else if (*pr == "N") out->poss_resend = false;
        else return WireError::NonPrintableValue;
    }
    if (const auto ost = out->find(tag::OrigSendingTime)) {
        out->orig_sending_time_raw = *ost;
        WallTime parsed{};
        if (!parse_utc_timestamp(*ost, &parsed)) return WireError::BadTimestamp;
        out->orig_sending_time = parsed;
    }
    // A retransmission without OrigSendingTime(122) is rejected rather than
    // accepted with a fabricated original timestamp (I-18).
    if (out->poss_dup && !out->orig_sending_time.has_value())
        return WireError::MissingRequiredHeader;

    return WireError::None;
}

// ---------------------------------------------------------------------------

std::string encode(const OutboundFields& f) {
    // Body: everything between BodyLength and CheckSum, which for FIX includes
    // MsgType and the rest of the standard header.
    std::string body;
    body.reserve(256);

    auto put = [&body](int tag_num, std::string_view value) {
        char num[12];
        const int n = std::snprintf(num, sizeof(num), "%d", tag_num);
        body.append(num, static_cast<std::size_t>(n));
        body.push_back('=');
        body.append(value);
        body.push_back(kSOH);
    };

    put(tag::MsgType, f.msg_type);
    put(tag::SenderCompID, f.sender_comp_id);
    put(tag::TargetCompID, f.target_comp_id);
    {
        char num[24];
        const int n = std::snprintf(num, sizeof(num), "%llu",
                                    static_cast<unsigned long long>(f.seq_num));
        put(tag::MsgSeqNum, std::string_view(num, static_cast<std::size_t>(n)));
    }
    if (f.poss_dup) put(tag::PossDupFlag, "Y");
    put(tag::SendingTime, format_utc_timestamp(f.sending_time));
    if (f.poss_dup && !f.orig_sending_time.empty())
        put(tag::OrigSendingTime, f.orig_sending_time);

    for (const auto& kv : f.body) put(kv.first, kv.second);

    std::string head;
    head.reserve(body.size() + 32);
    head.append("8=").append(kBeginStringFix44);
    head.push_back(kSOH);
    {
        char num[24];
        const int n = std::snprintf(num, sizeof(num), "%zu", body.size());
        head.append("9=").append(num, static_cast<std::size_t>(n));
        head.push_back(kSOH);
    }
    head.append(body);

    const int sum = checksum_of(head);
    head.append("10=").append(checksum_string(sum));
    head.push_back(kSOH);
    return head;
}

bool rebuild_as_retransmission(std::string_view original_raw, WallTime new_sending_time,
                               std::string* out) {
    DecodedMessage m;
    DecodeOptions opt;
    // A stored frame may legitimately carry a longer value than a fresh inbound
    // one; the cap is raised rather than letting history become unreplayable.
    opt.max_value_bytes = 4096;
    if (decode(original_raw, opt, &m) != WireError::None) return false;

    OutboundFields f;
    f.msg_type = std::string(m.msg_type);
    f.seq_num = m.seq_num;  // the original sequence number, never a new one
    f.sender_comp_id = std::string(m.sender_comp_id);
    f.target_comp_id = std::string(m.target_comp_id);
    f.sending_time = new_sending_time;
    f.poss_dup = true;
    // 122 carries the SendingTime of the original transmission. When the stored
    // frame was itself a retransmission, its own 122 is preserved so the chain
    // still points at the first transmission.
    f.orig_sending_time = m.orig_sending_time_raw.empty()
                              ? std::string(m.sending_time_raw)
                              : std::string(m.orig_sending_time_raw);

    for (const Field& fl : m.fields) {
        switch (fl.tag) {
            case tag::BeginString:
            case tag::BodyLength:
            case tag::MsgType:
            case tag::MsgSeqNum:
            case tag::SenderCompID:
            case tag::TargetCompID:
            case tag::SendingTime:
            case tag::OrigSendingTime:
            case tag::PossDupFlag:
            case tag::CheckSum:
                break;  // owned by the header writer above
            default:
                f.body.emplace_back(fl.tag, std::string(fl.value));
                break;
        }
    }

    *out = encode(f);
    return true;
}

}  // namespace lpsim
