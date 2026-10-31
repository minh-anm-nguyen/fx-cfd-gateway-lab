#include "gw/fix_transport.hpp"

#include <charconv>

namespace gw {
namespace {

constexpr char kSoh = '\x01';
constexpr int kMsgType = 35;

// Splits on SOH into (tag, value). A field without '=' or with a non-numeric
// tag ends the parse: a frame this bridge cannot read is refused rather than
// partially understood.
bool split_fields(std::string_view raw, std::vector<std::pair<int, std::string>>& out) {
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t end = raw.find(kSoh, i);
        const std::size_t stop = end == std::string_view::npos ? raw.size() : end;
        const std::string_view field = raw.substr(i, stop - i);
        i = stop + 1;
        if (field.empty()) continue;

        const std::size_t eq = field.find('=');
        if (eq == std::string_view::npos || eq == 0) return false;
        int tag = 0;
        const char* begin = field.data();
        const auto [ptr, ec] = std::from_chars(begin, begin + eq, tag);
        if (ec != std::errc() || ptr != begin + eq) return false;
        out.emplace_back(tag, std::string(field.substr(eq + 1)));
    }
    return !out.empty();
}

std::string join_fields(const std::vector<std::pair<int, std::string>>& fields) {
    std::string out;
    for (const auto& [tag, value] : fields) {
        out += std::to_string(tag);
        out += '=';
        out += value;
        out += kSoh;
    }
    return out;
}

}  // namespace

bool is_session_tag(int tag) {
    switch (tag) {
        // Standard header, the part P01 writes and validates.
        case 8:    // BeginString
        case 9:    // BodyLength
        case 34:   // MsgSeqNum
        case 49:   // SenderCompID
        case 56:   // TargetCompID
        case 52:   // SendingTime
        case 50:   // SenderSubID
        case 57:   // TargetSubID
        case 115:  // OnBehalfOfCompID
        case 128:  // DeliverToCompID
        case 142:  // SenderLocationID
        case 143:  // TargetLocationID
        case 122:  // OrigSendingTime
        case 212:  // XmlDataLen
        case 213:  // XmlData
        case 347:  // MessageEncoding
        case 369:  // LastMsgSeqNumProcessed
        // Standard trailer.
        case 10:   // CheckSum
        case 89:   // Signature
        case 93:   // SignatureLength
            return true;
        default:
            // 43 (PossDupFlag) and 97 (PossResend) are not here on purpose.
            // They are session-level markers that P04 records on the normalized
            // report, and EI-03 depends on telling a retransmission apart from
            // a business retry.
            return false;
    }
}

std::optional<std::string> strip_session_envelope(std::string_view raw) {
    std::vector<std::pair<int, std::string>> fields;
    if (!split_fields(raw, fields)) return std::nullopt;

    std::string msg_type;
    std::vector<std::pair<int, std::string>> kept;
    kept.reserve(fields.size());
    for (auto& [tag, value] : fields) {
        if (tag == kMsgType) {
            msg_type = std::move(value);
            continue;
        }
        if (is_session_tag(tag)) continue;
        kept.emplace_back(tag, std::move(value));
    }
    // A frame with no 35 is not a message this bridge can describe, and
    // inventing one would put a guess in front of the router's dictionary.
    if (msg_type.empty()) return std::nullopt;

    std::vector<std::pair<int, std::string>> ordered;
    ordered.reserve(kept.size() + 1);
    // 35 first, which is what parse_app_frame requires; the rest keep the order
    // they arrived in, because P04 digests the raw field sequence.
    ordered.emplace_back(kMsgType, std::move(msg_type));
    for (auto& f : kept) ordered.emplace_back(f.first, std::move(f.second));
    return join_fields(ordered);
}

std::optional<SubmitBody> to_submit_body(std::string_view app_frame) {
    std::vector<std::pair<int, std::string>> fields;
    if (!split_fields(app_frame, fields)) return std::nullopt;

    SubmitBody body;
    for (auto& [tag, value] : fields) {
        if (tag == kMsgType) {
            body.msg_type = std::move(value);
            continue;
        }
        // A session tag here would be a defect on P04's side. P01 would reject
        // the whole message, and it would do so for a reason that surfaces in
        // P01's fault detail rather than anywhere the router can act on, so it
        // is dropped and the message still goes.
        if (is_session_tag(tag)) continue;
        body.fields.emplace_back(tag, std::move(value));
    }
    if (body.msg_type.empty()) return std::nullopt;
    return body;
}

bool FixSink::deliver(std::uint64_t seq, std::string_view msg_type, std::string_view payload) {
    (void)seq;
    (void)msg_type;

    const std::optional<std::string> app = strip_session_envelope(payload);
    if (!app.has_value()) {
        ++unconvertible_;
        // False leaves it in P01's durable inbox, unacknowledged. The session
        // stops making progress on this message, which is the right failure: a
        // frame nobody can read must not be acknowledged as handled.
        return false;
    }

    const router::DurableReceiptId id = adapter_.deliver_inbound(*app);
    if (id == 0) return false;  // the store refused; P01 will offer it again
    ++delivered_;
    return true;
}

bool send_to_session(fixse::SessionEngine& engine, const std::string& app_frame) {
    const std::optional<SubmitBody> body = to_submit_body(app_frame);
    if (!body.has_value()) return false;
    return engine.submit_app_message(body->msg_type, body->fields) ==
           fixse::SubmitResult::Accepted;
}

}  // namespace gw
