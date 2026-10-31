// FIX 4.4 wire-level constants and helpers.
// Reference: FIX_Session_Engine_System_Design.md section 5 (FIX 4.4 wire format).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fixse {

// The real field delimiter is SOH (0x01). The vertical bar is used ONLY when
// printing examples in documentation or test output (design doc 5.2).
constexpr char kSOH = '\x01';
constexpr std::string_view kBeginStringFix44 = "FIX.4.4";

namespace tag {
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
}  // namespace tag

namespace msg_type {
constexpr std::string_view Logon = "A";
constexpr std::string_view Heartbeat = "0";
constexpr std::string_view TestRequest = "1";
constexpr std::string_view ResendRequest = "2";
constexpr std::string_view Reject = "3";
constexpr std::string_view SequenceReset = "4";
constexpr std::string_view Logout = "5";
}  // namespace msg_type

// Session-level message types defined by FIX 4.4. Everything else is an
// application message for this engine (design doc 5.1): the engine never
// interprets application business semantics.
inline bool is_session_message(std::string_view mt) {
    return mt == msg_type::Logon || mt == msg_type::Heartbeat ||
           mt == msg_type::TestRequest || mt == msg_type::ResendRequest ||
           mt == msg_type::Reject || mt == msg_type::SequenceReset ||
           mt == msg_type::Logout;
}

inline bool is_application_message(std::string_view mt) { return !is_session_message(mt); }

// Messages that must never be retransmitted verbatim in answer to a
// ResendRequest; they are covered by SequenceReset-GapFill instead.
// Reject (35=3) IS retransmittable per design doc 8.3.
inline bool is_gapfill_only(std::string_view mt) {
    return mt == msg_type::Logon || mt == msg_type::Logout ||
           mt == msg_type::ResendRequest || mt == msg_type::Heartbeat ||
           mt == msg_type::TestRequest || mt == msg_type::SequenceReset;
}

// FIX 4.4 SessionRejectReason (tag 373) values used by this engine.
enum class RejectReason : int {
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

// Render a frame for logs by replacing SOH with a vertical bar. Never used on
// the wire; secret redaction is applied separately by the audit layer.
std::string printable(std::string_view raw);

// FIX CheckSum (tag 10): sum of all bytes before the trailer, modulo 256.
int checksum_of(std::string_view bytes_before_trailer);

// Render a checksum as exactly three decimal digits.
std::string checksum_string(int sum_mod_256);

}  // namespace fixse
