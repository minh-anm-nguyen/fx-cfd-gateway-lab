// FIX 4.4 field decoder and message encoder for the LP acceptor.
//
// Reference: design doc 7.2 (semantic tags) and 7.3 (report builder rules).
//
// The decoder is strict by construction: anything ambiguous is an error rather
// than a guess, because a guess here becomes a wrong sequence number or a wrong
// business decision later (invariants I-01 and I-07). In particular a duplicated
// mandatory tag is rejected rather than resolved by first-wins or last-wins
// (B-18): choosing silently would let a crafted message aim at a different
// order than the one the operator would see in the log.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lp/clock.hpp"
#include "lp/result.hpp"
#include "lp/wire.hpp"

namespace lpsim {

struct Field {
    int tag = 0;
    std::string_view value;
};

struct DecodeOptions {
    int max_fields = 2048;
    // A single field value longer than this is refused without being copied
    // anywhere, which is what keeps a 10 MB ClOrdID out of the logs (B-19).
    std::size_t max_value_bytes = 256;
    std::string_view begin_string = kBeginStringFix44;
};

// A decoded message. Every string_view points into the caller-owned raw frame,
// so a DecodedMessage must not outlive the buffer it was decoded from.
struct DecodedMessage {
    std::string_view raw;
    std::vector<Field> fields;

    std::string_view msg_type;
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    std::uint64_t seq_num = 0;
    WallTime sending_time{};
    std::string_view sending_time_raw;

    bool poss_dup = false;
    bool poss_resend = false;
    std::optional<WallTime> orig_sending_time;
    std::string_view orig_sending_time_raw;

    bool is_app() const { return is_application_message(msg_type); }

    // First occurrence of a tag, or nullopt. Mandatory header tags are already
    // guaranteed unique by the decoder.
    std::optional<std::string_view> find(int tag) const;
    bool has(int tag) const { return find(tag).has_value(); }
    int count(int tag) const;

    bool find_u64(int tag, std::uint64_t* out) const;
    bool find_int(int tag, int* out) const;
};

// Decode a frame already validated by FixFramer: BodyLength and CheckSum are
// assumed verified. Returns WireError::None on success.
WireError decode(std::string_view raw, const DecodeOptions& opt, DecodedMessage* out);

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

// A message to be serialised. The encoder owns BodyLength(9) and CheckSum(10);
// callers never supply them. That is what makes the round-trip property test
// meaningful: the serialiser cannot emit a frame whose declared length or
// checksum disagrees with its own bytes.
struct OutboundFields {
    std::string msg_type;
    std::uint64_t seq_num = 0;
    std::string sender_comp_id;
    std::string target_comp_id;
    WallTime sending_time{};
    bool poss_dup = false;
    std::string orig_sending_time;  // tag 122, required when poss_dup is set
    // Body fields in the order they should follow the standard header.
    std::vector<std::pair<int, std::string>> body;
};

std::string encode(const OutboundFields& f);

// Rebuild a stored original frame as a retransmission: keep the original
// MsgSeqNum, set PossDupFlag(43)=Y, set OrigSendingTime(122) to the original
// SendingTime, stamp a fresh SendingTime(52) and recompute BodyLength and
// CheckSum.
//
// This is the only sanctioned way to put a previously issued report back on the
// wire. It never allocates a new sequence number and never changes the business
// payload, which is what invariant I-15 and scenario I-12 require. Returns
// false when the stored frame cannot be parsed, in which case the caller must
// fail closed rather than send anything (I-08).
bool rebuild_as_retransmission(std::string_view original_raw, WallTime new_sending_time,
                               std::string* out);

}  // namespace lpsim
