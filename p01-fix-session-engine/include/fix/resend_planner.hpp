// Resend planning.
//
// Design doc 8.3. Given a requested range and the outbound history, produce the
// exact sequence of actions that answers a ResendRequest:
//   * application messages (and Reject) are retransmitted with their ORIGINAL
//     MsgSeqNum, PossDupFlag=Y and OrigSendingTime
//   * administrative messages are never replayed; adjacent runs of them
//     collapse into one SequenceReset-GapFill
//   * a hole in the history is NOT papered over with a GapFill: the planner
//     fails closed so the caller can alert instead of lying about a successful
//     replay (scenario F-16)
//
// The planner is a pure function, which is what lets the F group and playbook
// PB-06 assert the plan directly without a socket or a peer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fix/result.hpp"
#include "fix/store.hpp"

namespace fixse {

struct ResendAction {
    enum class Kind { ReplayApp, GapFill };
    Kind kind = Kind::ReplayApp;
    // ReplayApp: the original sequence number being retransmitted.
    // GapFill: the MsgSeqNum the GapFill itself carries, which is the first
    // skipped sequence.
    uint64_t seq = 0;
    // GapFill only: NewSeqNo(36), one past the last skipped sequence.
    uint64_t new_seq_no = 0;
    // ReplayApp only: the stored original frame.
    std::string payload;
    // SendingTime of the original message, which becomes OrigSendingTime(122).
    std::string original_sending_time;
};

struct ResendPlan {
    bool ok = false;
    std::vector<ResendAction> actions;
    FaultReason fault = FaultReason::None;
    std::string detail;
};

// Validate a requested range against the engine state.
//   begin must be >= 1; end == 0 means "through the last original message"
//   and is resolved by the caller before calling this function.
struct ResendRange {
    bool valid = false;
    uint64_t begin = 0;
    uint64_t end = 0;
    std::string detail;
};

// Resolve a raw ResendRequest into a concrete range.
// last_original_seq is next_out - 1.
ResendRange resolve_range(uint64_t begin_seq_no, uint64_t end_seq_no, uint64_t last_original_seq);

// Build the plan. history must be the rows for [range.begin, range.end] in
// ascending sequence order; gaps in it are detected and reported.
ResendPlan plan_resend(const ResendRange& range, const std::vector<OutboundRecord>& history);

}  // namespace fixse
