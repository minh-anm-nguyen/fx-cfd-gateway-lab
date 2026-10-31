// FIX 4.4 wire-level constants for the LP acceptor.
//
// Reference: LP_Simulator_System_Design.md sections 7.1 (supported messages),
// 7.2 (semantic tags) and 7.3 (ExecutionReport builder rules).
//
// This is an INDEPENDENT implementation from the Project 01 initiator codec on
// purpose: ADR-01 and scenario M-01 require that a bug shared by both sides can
// still be caught, which a single shared codec would hide.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lpsim {

// The real field delimiter is SOH (0x01). A vertical bar appears ONLY in
// documentation and test output (design doc 7.4 warns against sending it).
constexpr char kSOH = '\x01';
constexpr std::string_view kBeginStringFix44 = "FIX.4.4";

namespace tag {
// Standard header and trailer.
constexpr int BeginString = 8;
constexpr int BodyLength = 9;
constexpr int MsgType = 35;
constexpr int MsgSeqNum = 34;
constexpr int SenderCompID = 49;
constexpr int TargetCompID = 56;
constexpr int SendingTime = 52;
constexpr int CheckSum = 10;
constexpr int PossDupFlag = 43;
constexpr int PossResend = 97;
constexpr int OrigSendingTime = 122;

// Session layer.
constexpr int EncryptMethod = 98;
constexpr int HeartBtInt = 108;
constexpr int TestReqID = 112;
constexpr int BeginSeqNo = 7;
constexpr int EndSeqNo = 16;
constexpr int NewSeqNo = 36;
constexpr int GapFillFlag = 123;
constexpr int ResetSeqNumFlag = 141;
constexpr int Text = 58;
constexpr int RefSeqNum = 45;
constexpr int RefTagID = 371;
constexpr int RefMsgType = 372;
constexpr int SessionRejectReason = 373;
constexpr int Username = 553;
constexpr int Password = 554;

// Application layer: order requests and reports.
constexpr int Account = 1;
constexpr int AvgPx = 6;
constexpr int ClOrdID = 11;
constexpr int CumQty = 14;
constexpr int ExecID = 17;
constexpr int LastPx = 31;
constexpr int LastQty = 32;
constexpr int OrderID = 37;
constexpr int OrderQty = 38;
constexpr int OrdStatus = 39;
constexpr int OrdType = 40;
constexpr int OrigClOrdID = 41;
constexpr int Price = 44;
constexpr int Side = 54;
constexpr int Symbol = 55;
constexpr int TimeInForce = 59;
constexpr int TransactTime = 60;
constexpr int CxlRejReason = 102;
constexpr int OrdRejReason = 103;
constexpr int ExecType = 150;
constexpr int LeavesQty = 151;
constexpr int CxlRejResponseTo = 434;

// BusinessMessageReject (35=j).
constexpr int BusinessRejectRefID = 379;
constexpr int BusinessRejectReason = 380;
}  // namespace tag

namespace msg_type {
// Session layer.
constexpr std::string_view Logon = "A";
constexpr std::string_view Heartbeat = "0";
constexpr std::string_view TestRequest = "1";
constexpr std::string_view ResendRequest = "2";
constexpr std::string_view Reject = "3";
constexpr std::string_view SequenceReset = "4";
constexpr std::string_view Logout = "5";
// Application layer.
constexpr std::string_view NewOrderSingle = "D";
constexpr std::string_view OrderCancelRequest = "F";
constexpr std::string_view OrderCancelReplaceRequest = "G";
constexpr std::string_view ExecutionReport = "8";
constexpr std::string_view OrderCancelReject = "9";
constexpr std::string_view BusinessMessageReject = "j";
constexpr std::string_view OrderStatusRequest = "H";  // phase 2, not supported in v1
}  // namespace msg_type

// FIX 4.4 ExecType(150). Design doc 7.2 is explicit that 150=1/2 are the FIX
// 4.2 partial/full fill codes and must NOT be used to report a trade here.
namespace exec_type {
constexpr std::string_view New = "0";
constexpr std::string_view Canceled = "4";
constexpr std::string_view Replaced = "5";
constexpr std::string_view PendingCancel = "6";
constexpr std::string_view Rejected = "8";
constexpr std::string_view PendingReplace = "E";
constexpr std::string_view Trade = "F";
}  // namespace exec_type

// FIX 4.4 OrdStatus(39).
namespace ord_status {
constexpr std::string_view New = "0";
constexpr std::string_view PartiallyFilled = "1";
constexpr std::string_view Filled = "2";
constexpr std::string_view Canceled = "4";
constexpr std::string_view PendingCancel = "6";
constexpr std::string_view Rejected = "8";
constexpr std::string_view Expired = "C";
constexpr std::string_view PendingReplace = "E";
}  // namespace ord_status

// Side(54): only Buy and Sell are in the v1 profile (design doc 7.2).
namespace side_code {
constexpr std::string_view Buy = "1";
constexpr std::string_view Sell = "2";
}  // namespace side_code

// OrdType(40): v1 accepts Limit only; the others exist so that an unsupported
// value is rejected for a named reason instead of falling through (B-11).
namespace ord_type {
constexpr std::string_view Market = "1";
constexpr std::string_view Limit = "2";
constexpr std::string_view Stop = "3";
constexpr std::string_view StopLimit = "4";
}  // namespace ord_type

// TimeInForce(59): v1 accepts Day (or an absent tag, which defaults to Day per
// the FIX 4.4 dictionary).
namespace tif_code {
constexpr std::string_view Day = "0";
constexpr std::string_view GoodTillCancel = "1";
constexpr std::string_view AtTheOpening = "2";
constexpr std::string_view ImmediateOrCancel = "3";
constexpr std::string_view FillOrKill = "4";
}  // namespace tif_code

// CxlRejResponseTo(434).
namespace cxl_rej_response_to {
constexpr std::string_view CancelRequest = "1";
constexpr std::string_view CancelReplaceRequest = "2";
}  // namespace cxl_rej_response_to

// Session-level message types defined by FIX 4.4. Everything else is an
// application message, which the session layer forwards only once the session
// is ACTIVE and the inbound frame is durably committed (invariant I-01).
inline bool is_session_message(std::string_view mt) {
    return mt == msg_type::Logon || mt == msg_type::Heartbeat ||
           mt == msg_type::TestRequest || mt == msg_type::ResendRequest ||
           mt == msg_type::Reject || mt == msg_type::SequenceReset ||
           mt == msg_type::Logout;
}

inline bool is_application_message(std::string_view mt) { return !is_session_message(mt); }

// Application message types this profile implements. 35=H is deliberately
// absent: design doc 7.1 forbids pretending to support it (B-17, M-19).
inline bool is_supported_app_request(std::string_view mt) {
    return mt == msg_type::NewOrderSingle || mt == msg_type::OrderCancelRequest ||
           mt == msg_type::OrderCancelReplaceRequest;
}

// Messages that must never be retransmitted verbatim in answer to a
// ResendRequest; they are covered by SequenceReset-GapFill instead
// (design doc, I-07: never fabricate an ExecutionReport in a gap fill).
inline bool is_gapfill_only(std::string_view mt) {
    return mt == msg_type::Logon || mt == msg_type::Logout ||
           mt == msg_type::ResendRequest || mt == msg_type::Heartbeat ||
           mt == msg_type::TestRequest || mt == msg_type::SequenceReset;
}

// FIX 4.4 SessionRejectReason (tag 373) values used by this acceptor.
enum class SessionRejectReason : int {
    InvalidTagNumber = 0,
    RequiredTagMissing = 1,
    TagNotDefinedForMsgType = 2,
    UndefinedTag = 3,
    TagSpecifiedWithoutValue = 4,
    ValueIsIncorrect = 5,
    IncorrectDataFormat = 6,
    CompIDProblem = 9,
    SendingTimeAccuracyProblem = 10,
    InvalidMsgType = 11,
    TagAppearsMoreThanOnce = 13,
    TagSpecifiedOutOfRequiredOrder = 14,
    Other = 99,
};

// FIX 4.4 BusinessRejectReason (tag 380) values used for 35=j.
enum class BusinessRejectReason : int {
    Other = 0,
    UnknownID = 1,
    UnknownSecurity = 2,
    UnsupportedMessageType = 3,
    ApplicationNotAvailable = 4,
    ConditionallyRequiredFieldMissing = 5,
};

// Render a frame for logs by replacing SOH with a vertical bar. Never used on
// the wire; secret redaction is a separate step in the audit layer (L-07).
std::string printable(std::string_view raw);

// FIX CheckSum (tag 10): sum of all bytes before the trailer, modulo 256.
int checksum_of(std::string_view bytes_before_trailer);

// Render a checksum as exactly three decimal digits.
std::string checksum_string(int sum_mod_256);

}  // namespace lpsim
