// The immutable business event that crosses the P04 to P03 boundary.
//
// Contract NormalizedReportV1 in design doc 6.6. Three properties matter.
//
// R-I05: ExecType(150) and OrdStatus(39) are kept as separate fields and never
// collapsed into one "state" value, because they answer different questions.
//
// R-I14: every event carries its provenance -- which session and epoch it came
// from, which durable receipt, the digest of the raw frame, and the parser and
// profile versions that read it. Without those an event cannot be audited or
// replayed.
//
// The struct has no methods that change it: once the normalizer has produced
// one, it is evidence, not working state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "router/clock.hpp"
#include "router/hash.hpp"
#include "router/ids.hpp"

namespace router {

enum class ReportKind { ExecutionReport, CancelReject };

struct NormalizedReport {
    // --- provenance (R-I14) ------------------------------------------------
    std::string session_key;      // SenderCompID:TargetCompID:qualifier
    SessionEpoch epoch = 0;
    DurableReceiptId source_receipt = 0;
    MsgSeqNum msg_seq_num = 0;    // session ordering only, never an event identity
    Hash raw_frame_hash;
    std::string parser_version = "fix44_app_v1";
    std::string profile_version;
    WallMicros source_wall = 0;

    // --- identity ----------------------------------------------------------
    LpId lp;
    OrderChainId chain_id;
    AccountId account;
    ClientOrderId cl_ord_id;
    std::optional<ClientOrderId> orig_cl_ord_id;
    std::optional<OrderId> order_id;
    std::optional<ExecutionId> exec_id;
    std::optional<ExecutionId> exec_ref_id;
    std::optional<QueryId> ord_status_req_id;

    // --- business content --------------------------------------------------
    ReportKind kind = ReportKind::ExecutionReport;
    ExecType exec_type = ExecType::New;
    OrdStatus ord_status = OrdStatus::New;
    std::optional<CxlRejResponseTo> cxl_rej_response_to;
    int cxl_rej_reason = -1;

    std::string wire_symbol;
    Side side = Side::Buy;
    std::optional<std::int64_t> last_qty;
    std::optional<std::int64_t> last_px;
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t avg_px = 0;
    std::optional<std::int64_t> order_qty;

    bool poss_dup = false;     // FIX 43, a session level replay marker
    bool poss_resend = false;  // FIX 97, a different situation entirely

    // Content digest over the business-meaningful fields. Two reports with the
    // same ExecID must have the same fingerprint or they contradict each other
    // (OR-K-05, OR-K-06, OR-K-17).
    Hash fingerprint;

    // True when this report carries a business execution identity that may be
    // used as a dedup key. A status snapshot with ExecID=0 does not (R-I08).
    bool has_business_exec_identity() const {
        if (!exec_id) return false;
        if (exec_id->str().empty() || exec_id->str() == "0") return false;
        if (exec_type == ExecType::OrderStatus) return false;
        return true;
    }

    bool is_trade() const { return exec_type == ExecType::Trade; }
    bool is_status_snapshot() const { return exec_type == ExecType::OrderStatus; }
};

// Computes the fingerprint. Kept as a free function so both the normalizer and
// the test oracle can derive it the same way without sharing state.
Hash report_fingerprint(const NormalizedReport& r);

}  // namespace router
