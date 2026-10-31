// FIX 4.4 application layer: encoder for D/F/G/H and a strict parser for 8/9.
//
// Two boundaries are deliberate here.
//
// First, this encoder emits the *application* fields only. Design doc 9.1 ends
// with "P01 generates 8/9/10/34/49/52/56, checksum and session resend flags;
// P04 does not attach 34". Attaching a sequence number from here would break
// R-I06, so the type system does not even offer the tag.
//
// Second, the parser is pinned to the dictionary subset in 3.2 and 11.2 and
// refuses everything else. FIX 4.4 uses ExecType(150)=F for a Trade; the old
// 150=1/2 partial-fill codes are not accepted (OR-D-05, OR-D-11), ExecType=I is
// a snapshot that can never become a fill (R-I09), and 35=9 without 434 is
// malformed rather than "probably a cancel reject" (OR-D-10).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "router/config.hpp"
#include "router/hash.hpp"
#include "router/ids.hpp"
#include "router/result.hpp"

namespace router::fix {

// FIX tag numbers used in this project.
namespace tag {
inline constexpr int kAccount = 1;
inline constexpr int kAvgPx = 6;
inline constexpr int kClOrdID = 11;
inline constexpr int kCumQty = 14;
inline constexpr int kExecID = 17;
inline constexpr int kLastPx = 31;
inline constexpr int kLastQty = 32;
inline constexpr int kMsgType = 35;
inline constexpr int kOrderID = 37;
inline constexpr int kOrderQty = 38;
inline constexpr int kOrdStatus = 39;
inline constexpr int kOrdType = 40;
inline constexpr int kOrigClOrdID = 41;
inline constexpr int kPossDupFlag = 43;
inline constexpr int kPrice = 44;
inline constexpr int kSide = 54;
inline constexpr int kSymbol = 55;
inline constexpr int kText = 58;
inline constexpr int kTimeInForce = 59;
inline constexpr int kTransactTime = 60;
inline constexpr int kPossResend = 97;
inline constexpr int kCxlRejReason = 102;
inline constexpr int kExecType = 150;
inline constexpr int kLeavesQty = 151;
inline constexpr int kCxlRejResponseTo = 434;
inline constexpr int kExecRefID = 19;
inline constexpr int kOrdStatusReqID = 790;
}  // namespace tag

struct Field {
    int tag = 0;
    std::string value;
};

// An ordered, immutable field list. Order is preserved because the raw digest
// in R-I14 must be reproducible from what the peer actually sent.
class Message {
public:
    Message() = default;
    explicit Message(std::vector<Field> f) : fields_(std::move(f)) {}

    const std::vector<Field>& fields() const { return fields_; }
    bool has(int tag) const;
    std::size_t count(int tag) const;
    // First occurrence. The parser rejects duplicates, so first is also only.
    std::optional<std::string> get(int tag) const;
    std::string get_or(int tag, std::string fallback = {}) const;
    char msg_type() const;

    std::string serialize() const;  // SOH separated, in the stored order
    Hash digest() const;

    void push(int tag, std::string value) { fields_.push_back(Field{tag, std::move(value)}); }

private:
    std::vector<Field> fields_;
};

// ---------------------------------------------------------------------------
// Outbound encoding (design doc 9.1).

// Everything the encoder needs for a NewOrderSingle. The caller has already
// validated units against the symbol profile; the encoder re-checks bounds so a
// corrupted record cannot reach the wire (OR-E-11).
struct NewOrderFields {
    ClientOrderId cl_ord_id;
    AccountId account;
    std::string wire_symbol;
    Side side = Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price_ticks = 0;
    int price_scale = 5;
    OrdType ord_type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;
    std::string transact_time;  // FIX UTCTimestamp, caller supplies
};

struct CancelFields {
    ClientOrderId cl_ord_id;
    ClientOrderId orig_cl_ord_id;
    AccountId account;
    std::string wire_symbol;
    Side side = Side::Buy;
    std::optional<OrderId> order_id;  // only when the peer requires it
    std::string transact_time;
};

struct ReplaceFields {
    ClientOrderId cl_ord_id;
    ClientOrderId orig_cl_ord_id;
    AccountId account;
    std::string wire_symbol;
    Side side = Side::Buy;
    std::int64_t total_qty = 0;  // the whole intended order size, never LeavesQty
    std::int64_t price_ticks = 0;
    int price_scale = 5;
    OrdType ord_type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;
    std::optional<OrderId> order_id;
    std::string transact_time;
};

struct StatusFields {
    ClientOrderId cl_ord_id;
    AccountId account;
    std::string wire_symbol;
    Side side = Side::Buy;
    std::optional<OrderId> order_id;
    std::optional<QueryId> ord_status_req_id;  // 790, only when the peer echoes it
};

Result<Message, ValidationError> encode_new_order(const NewOrderFields& f);
Result<Message, ValidationError> encode_cancel(const CancelFields& f);
Result<Message, ValidationError> encode_replace(const ReplaceFields& f);
Result<Message, ValidationError> encode_status(const StatusFields& f);

// ---------------------------------------------------------------------------
// Inbound parsing.

struct ParseLimits {
    std::size_t max_frame_bytes = 1024 * 1024;
    std::size_t max_field_bytes = 256;
    std::size_t max_fields = 128;
    std::vector<int> tolerated_custom_tags{};
};

// Splits an application frame into fields and applies every structural rule:
// numeric tags, non-empty printable values, bounded sizes, no duplicates, 35
// first, no unknown must-understand tags. No business meaning is assigned here.
Result<Message, ValidationError> parse_app_frame(std::string_view frame, const ParseLimits& lim);

// Dictionary level checks for 35=8 and 35=9: required tags for the specific
// ExecType, strict decimal formats against the symbol scale, value ranges and
// the quantity relation. Returns the first violation, so a report never reaches
// the reducer half-validated.
Result<bool, ValidationError> validate_execution_report(const Message& m, const Config& cfg);
Result<bool, ValidationError> validate_cancel_reject(const Message& m);

// Enumeration mapping. Unknown codes are an error, never a default.
std::optional<ExecType> exec_type_from_wire(std::string_view v);
std::optional<OrdStatus> ord_status_from_wire(std::string_view v);
std::optional<Side> side_from_wire(std::string_view v);
std::optional<OrdType> ord_type_from_wire(std::string_view v);
std::optional<TimeInForce> tif_from_wire(std::string_view v);
std::optional<CxlRejResponseTo> cxl_rej_response_to_from_wire(std::string_view v);

std::string_view exec_type_to_wire(ExecType t);
std::string_view ord_status_to_wire(OrdStatus s);
std::string_view side_to_wire(Side s);
std::string_view ord_type_to_wire(OrdType t);
std::string_view tif_to_wire(TimeInForce t);
std::string_view cxl_rej_response_to_wire(CxlRejResponseTo r);

// True for the ExecTypes that v1 recognises but refuses to reduce: Restated,
// TradeCorrect, TradeCancel (design doc 11.2, OR-J-12..J-14).
bool is_unsupported_in_v1(ExecType t);

}  // namespace router::fix
