// FIX 4.4 field decoder and message encoder.
// Reference: design doc 5.2 (frame/field rules) and 15.3 (group B).
//
// The decoder is strict by construction: anything ambiguous is an error rather
// than a guess, because a guess at this layer becomes a wrong sequence number
// or a wrong session identity later (invariant I-03).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fix/clock.hpp"
#include "fix/result.hpp"
#include "fix/wire.hpp"

namespace fixse {

struct Field {
    int tag = 0;
    std::string_view value;
};

struct DecodeOptions {
    int max_fields = 2048;
    // Accepted BeginString. A mismatch is rejected rather than tolerated (B-16).
    std::string_view begin_string = kBeginStringFix44;
    // When non-empty, an application MsgType outside this list is rejected
    // with UnsupportedMsgType (B-20).
    const std::vector<std::string>* allowed_app_msg_types = nullptr;
};

// A decoded message. All string_views point into the caller-owned raw frame, so
// a DecodedMessage never outlives the buffer it was decoded from.
struct DecodedMessage {
    std::string_view raw;
    std::vector<Field> fields;

    std::string_view msg_type;
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    uint64_t seq_num = 0;
    WallTime sending_time{};
    std::string_view sending_time_raw;

    bool poss_dup = false;
    bool poss_resend = false;
    std::optional<WallTime> orig_sending_time;
    std::string_view orig_sending_time_raw;

    bool is_app() const { return is_application_message(msg_type); }

    // First occurrence of a tag, or nullopt. Mandatory header tags are already
    // guaranteed unique by the decoder (B-15).
    std::optional<std::string_view> find(int tag) const;
    bool has(int tag) const { return find(tag).has_value(); }
    int count(int tag) const;

    // Parse a tag as an unsigned integer with overflow checking.
    bool find_u64(int tag, uint64_t* out) const;
    bool find_int(int tag, int* out) const;
};

// Decode a frame previously validated by FixFramer (BodyLength and CheckSum are
// assumed verified). Returns WireError::None on success.
WireError decode(std::string_view raw, const DecodeOptions& opt, DecodedMessage* out);

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

// A message to be serialised. The encoder owns BodyLength(9) and CheckSum(10):
// callers never supply them, which is what makes the B-24 property test
// meaningful (the serialiser cannot emit a frame whose length or checksum
// disagrees with its own bytes).
struct OutboundFields {
    std::string msg_type;
    uint64_t seq_num = 0;
    std::string sender_comp_id;
    std::string target_comp_id;
    WallTime sending_time{};
    bool poss_dup = false;
    std::string orig_sending_time;  // tag 122, required when poss_dup is set
    // Body fields in the order they should appear after the standard header.
    std::vector<std::pair<int, std::string>> body;
};

// Serialise to raw wire bytes with a real SOH delimiter.
std::string encode(const OutboundFields& f);

// Rebuild a stored original frame as a retransmission: keep the original
// MsgSeqNum, set PossDupFlag=Y, set OrigSendingTime(122) to the original
// SendingTime, stamp a fresh SendingTime(52) and recompute BodyLength and
// CheckSum (design doc 8.3, invariant I-05, scenarios F-09 and F-10).
// Returns false when the stored frame cannot be parsed, in which case the
// caller must fail closed instead of sending anything (F-17).
bool rebuild_as_retransmission(std::string_view original_raw, WallTime new_sending_time,
                               std::string* out);

}  // namespace fixse
