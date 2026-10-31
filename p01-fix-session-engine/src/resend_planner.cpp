#include "fix/resend_planner.hpp"

#include "fix/wire.hpp"

namespace fixse {

ResendRange resolve_range(uint64_t begin_seq_no, uint64_t end_seq_no,
                          uint64_t last_original_seq) {
    ResendRange r;
    // BeginSeqNo is 1-based; zero or a range that runs backwards is a
    // session-level error, not something to be guessed at (F-03, F-04).
    if (begin_seq_no == 0) {
        r.detail = "BeginSeqNo must be at least 1";
        return r;
    }
    if (end_seq_no != 0 && end_seq_no < begin_seq_no) {
        r.detail = "EndSeqNo is lower than BeginSeqNo";
        return r;
    }

    // EndSeqNo=0 means "everything from BeginSeqNo onwards" (F-02).
    uint64_t end = end_seq_no == 0 ? last_original_seq : end_seq_no;

    if (last_original_seq == 0 || begin_seq_no > last_original_seq) {
        r.detail = "requested range starts beyond the last original message";
        return r;
    }
    // A request past the high-water mark is clamped rather than scanned out of
    // bounds (F-05).
    if (end > last_original_seq) end = last_original_seq;

    r.valid = true;
    r.begin = begin_seq_no;
    r.end = end;
    return r;
}

ResendPlan plan_resend(const ResendRange& range, const std::vector<OutboundRecord>& history) {
    ResendPlan plan;
    if (!range.valid) {
        plan.detail = range.detail;
        return plan;
    }

    // Index the history by sequence for a single pass over the range.
    size_t idx = 0;
    uint64_t gap_begin = 0;
    uint64_t gap_end = 0;  // last skipped sequence
    std::string gap_orig_time;

    const auto flush_gap = [&]() {
        if (gap_begin == 0) return;
        ResendAction a;
        a.kind = ResendAction::Kind::GapFill;
        a.seq = gap_begin;
        a.new_seq_no = gap_end + 1;
        a.original_sending_time = gap_orig_time;
        plan.actions.push_back(std::move(a));
        gap_begin = 0;
        gap_end = 0;
        gap_orig_time.clear();
    };

    for (uint64_t seq = range.begin; seq <= range.end; ++seq) {
        while (idx < history.size() && history[idx].seq < seq) ++idx;
        if (idx >= history.size() || history[idx].seq != seq) {
            // The archive cannot answer this range. Replacing the missing
            // message with a GapFill would tell the peer the gap is resolved
            // when the message is simply gone (F-16, F-20).
            plan.fault = FaultReason::MissingOutboundHistory;
            plan.detail = "outbound history has no row for sequence " + std::to_string(seq);
            plan.actions.clear();
            return plan;
        }
        const OutboundRecord& rec = history[idx];

        if (is_gapfill_only(rec.msg_type)) {
            if (gap_begin == 0) {
                gap_begin = seq;
                gap_orig_time = rec.original_sending_time;
            }
            gap_end = seq;
            continue;
        }

        // An application message, or a Reject, which FIX 4.4 does allow to be
        // retransmitted (design doc 8.3).
        flush_gap();
        ResendAction a;
        a.kind = ResendAction::Kind::ReplayApp;
        a.seq = seq;
        a.payload = rec.payload;
        a.original_sending_time = rec.original_sending_time;
        plan.actions.push_back(std::move(a));
    }
    flush_gap();

    plan.ok = true;
    return plan;
}

}  // namespace fixse
