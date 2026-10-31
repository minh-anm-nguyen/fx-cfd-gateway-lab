// Normalized events entering the state machine.
//
// Three families, kept separate because conflating them is the bug the design
// doc warns about in section 5.1: a local command changes local intent, a peer
// report changes what the counterparty has confirmed, and a system event
// records uncertainty. None of the three may be mistaken for another.
//
// Numeric fields on a peer report stay as text. They arrive from a wire and
// group O requires that "abc", "nan", "1e50" and a 23-digit integer are
// rejected rather than coerced, which is impossible once something has already
// parsed them into an int64.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "order_core/ids.hpp"

namespace order_core {

enum class Side { Unspecified, Buy, Sell };

const char* to_string(Side s);
// FIX tag 54: '1' buy, '2' sell. Anything else is Unspecified, never a
// defaulted Buy (what-if O-07).
Side side_from_fix(const std::string& raw);

enum class CommandKind { New, Cancel, Replace };
const char* to_string(CommandKind k);

// A client intent produced by P04. Quantity and price are text for the same
// reason peer reports are: the validator, not the caller, decides whether they
// are admissible under the pinned symbol scale.
struct LocalCommand {
    CommandKind kind = CommandKind::New;
    std::string firm = "FIRM";
    std::string business_day = "2026-10-30";
    std::string account;
    std::string symbol;
    std::string side_raw;  // FIX tag 54 text
    std::string cl_ord_id;
    std::string orig_cl_ord_id;  // cancel/replace only
    std::string qty_text;
    std::string price_text;
    std::string tif = "DAY";
    // Identifies the producer's attempt so a resubmitted identical intent is a
    // duplicate rather than a second order (what-if A-02, F-09, F-10).
    uint64_t command_seq = 0;

    [[nodiscard]] RequestKey request_key() const { return {firm, business_day, cl_ord_id}; }
};

// FIX 4.4 ExecType(150) and OrdStatus(39) codes this profile recognises.
// Everything else is an unknown code, handled by policy rather than mapped.
namespace exec_type {
inline constexpr const char* kNew = "0";
inline constexpr const char* kDoneForDay = "3";
inline constexpr const char* kCanceled = "4";
inline constexpr const char* kReplaced = "5";
inline constexpr const char* kPendingCancel = "6";
inline constexpr const char* kStopped = "7";
inline constexpr const char* kRejected = "8";
inline constexpr const char* kSuspended = "9";
inline constexpr const char* kPendingNew = "A";
inline constexpr const char* kCalculated = "B";
inline constexpr const char* kExpired = "C";
inline constexpr const char* kRestated = "D";
inline constexpr const char* kPendingReplace = "E";
inline constexpr const char* kTrade = "F";
inline constexpr const char* kTradeCorrect = "G";
inline constexpr const char* kTradeCancel = "H";
inline constexpr const char* kOrderStatus = "I";
}  // namespace exec_type

namespace ord_status {
inline constexpr const char* kNew = "0";
inline constexpr const char* kPartiallyFilled = "1";
inline constexpr const char* kFilled = "2";
inline constexpr const char* kDoneForDay = "3";
inline constexpr const char* kCanceled = "4";
inline constexpr const char* kReplacedObsolete = "5";
inline constexpr const char* kPendingCancel = "6";
inline constexpr const char* kStopped = "7";
inline constexpr const char* kRejected = "8";
inline constexpr const char* kSuspended = "9";
inline constexpr const char* kPendingNew = "A";
inline constexpr const char* kCalculated = "B";
inline constexpr const char* kExpired = "C";
inline constexpr const char* kAcceptedForBidding = "D";
inline constexpr const char* kPendingReplace = "E";
}  // namespace ord_status

// Precedence of OrdStatus(39) per the FIX Order State Changes document,
// reproduced in design doc 3.2. Higher wins. This is the precedence of a *FIX
// field*, not of the local lifecycle: it is used to decide which reported
// status to believe, never to decide that an order is canceled.
int ord_status_precedence(const std::string& status);

// A validated ExecutionReport (35=8) or OrderCancelReject (35=9) from P04.
struct PeerReport {
    char msg_type = '8';  // '8' or '9'
    std::string exec_type;
    std::string ord_status;

    std::string exec_id;
    std::string order_id;
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string exec_ref_id;  // tag 19, required by corrections

    std::string account;
    std::string symbol;
    std::string side_raw;

    // Absent optional means the tag was not present, which is different from a
    // tag present with value zero (what-if O-01, C-04).
    std::optional<std::string> last_qty;
    std::optional<std::string> last_px;
    std::optional<std::string> cum_qty;
    std::optional<std::string> leaves_qty;
    std::optional<std::string> order_qty;
    std::optional<std::string> price;
    std::optional<std::string> avg_px;

    std::string cxl_rej_response_to;  // tag 434: '1' cancel, '2' replace
    std::string cxl_rej_reason;       // tag 102
    std::string text;                 // tag 58, size-bounded by P04

    // Provenance. Transport fields are recorded but excluded from the business
    // fingerprint so a FIX resend with a new MsgSeqNum still dedups (F-05).
    std::string peer_id = "LP1";
    std::string session_id = "S1";
    uint64_t session_epoch = 0;
    uint64_t source_seq = 0;
    bool has_source_cursor = true;
    bool poss_dup = false;
    bool poss_resend = false;
    std::string sending_time;
    uint64_t reconciliation_generation = 0;

    [[nodiscard]] ExecKey exec_key(const std::string& business_namespace) const {
        return {peer_id, business_namespace, exec_id};
    }
};

enum class SystemKind {
    // The outbox intent for a request became durable. This is what moves a
    // request from "prepared locally" to "sent, outcome unknown"; it is not an
    // acknowledgement and must not reach Working (what-if A-10).
    Dispatched,
    AckTimeout,
    TransportDisconnected,
    ReconcileStarted,
    ReconcileResolved,
    StorageFailed,
    ManualOverride,
};
const char* to_string(SystemKind k);

struct SystemEvent {
    SystemKind kind = SystemKind::AckTimeout;
    // Which dispatched request the fact concerns. A timeout is always about a
    // specific request, never about the order in general.
    std::string target_cl_ord_id;
    std::string detail;
    // A manual resolution needs evidence and dual control; without both it is
    // denied (what-if P-17, Q-13).
    bool evidence_provided = false;
    bool dual_control_signed = false;
    // The resolution an operator or reconciliation reached, when applicable.
    std::string resolved_status;
    uint64_t reconciliation_generation = 0;
};

using OrderEvent = std::variant<LocalCommand, PeerReport, SystemEvent>;

// Coarse classification used in audit records and test assertions.
enum class EventClass { LocalCommand, PeerReport, SystemEvent };
EventClass classify(const OrderEvent& e);

// Canonical business fingerprint. Two deliveries of the same logical event
// produce the same value even when MsgSeqNum, SendingTime, PossDup,
// PossResend, session epoch or source cursor differ; any business field
// differing produces a different value, which is what turns a redelivery into
// a conflict instead of a silent no-op (what-if F-01 through F-05).
uint64_t business_fingerprint(const PeerReport& r);
uint64_t business_fingerprint(const LocalCommand& c);

// The economic content of a New intent with the identifiers removed: same
// firm, account, symbol, side, quantity, price and time in force. Two New
// commands with different ClOrdIDs but the same intent fingerprint are the
// same order asked for twice, which is how a blind retry of an unacknowledged
// order is recognised and refused (what-if F-08).
uint64_t intent_fingerprint(const LocalCommand& c);

}  // namespace order_core
