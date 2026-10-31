#include "router/fix44.hpp"

#include <algorithm>
#include <set>

#include "router/numeric.hpp"

namespace router::fix {
namespace {

constexpr char kSoh = '\x01';

using MsgResult = Result<Message, ValidationError>;
using CheckResult = Result<bool, ValidationError>;

bool printable(std::string_view v) {
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) return false;
    }
    return true;
}

}  // namespace

bool Message::has(int tag) const {
    return std::any_of(fields_.begin(), fields_.end(), [tag](const Field& f) { return f.tag == tag; });
}

std::size_t Message::count(int tag) const {
    return static_cast<std::size_t>(
        std::count_if(fields_.begin(), fields_.end(), [tag](const Field& f) { return f.tag == tag; }));
}

std::optional<std::string> Message::get(int tag) const {
    for (const Field& f : fields_)
        if (f.tag == tag) return f.value;
    return std::nullopt;
}

std::string Message::get_or(int tag, std::string fallback) const {
    auto v = get(tag);
    return v ? *v : fallback;
}

char Message::msg_type() const {
    auto v = get(tag::kMsgType);
    if (!v || v->empty()) return '\0';
    return (*v)[0];
}

std::string Message::serialize() const {
    std::string out;
    for (const Field& f : fields_) {
        out += std::to_string(f.tag);
        out.push_back('=');
        out += f.value;
        out.push_back(kSoh);
    }
    return out;
}

Hash Message::digest() const {
    Sha256 s;
    for (const Field& f : fields_) {
        s.update(static_cast<std::uint64_t>(f.tag));
        s.field(f.value);
    }
    return s.finish();
}

// ---------------------------------------------------------------------------
// Enumeration mapping.

std::optional<ExecType> exec_type_from_wire(std::string_view v) {
    if (v.size() != 1) return std::nullopt;
    switch (v[0]) {
        case '0': return ExecType::New;
        case '3': return ExecType::DoneForDay;
        case '4': return ExecType::Canceled;
        case '5': return ExecType::Replaced;
        case '6': return ExecType::PendingCancel;
        case '7': return ExecType::Stopped;
        case '8': return ExecType::Rejected;
        case '9': return ExecType::Suspended;
        case 'A': return ExecType::PendingNew;
        case 'B': return ExecType::Calculated;
        case 'C': return ExecType::Expired;
        case 'D': return ExecType::Restated;
        case 'E': return ExecType::PendingReplace;
        case 'F': return ExecType::Trade;
        case 'G': return ExecType::TradeCorrect;
        case 'H': return ExecType::TradeCancel;
        case 'I': return ExecType::OrderStatus;
        default: return std::nullopt;  // includes the FIX 4.2 codes 1 and 2
    }
}

std::string_view exec_type_to_wire(ExecType t) {
    switch (t) {
        case ExecType::New: return "0";
        case ExecType::DoneForDay: return "3";
        case ExecType::Canceled: return "4";
        case ExecType::Replaced: return "5";
        case ExecType::PendingCancel: return "6";
        case ExecType::Stopped: return "7";
        case ExecType::Rejected: return "8";
        case ExecType::Suspended: return "9";
        case ExecType::PendingNew: return "A";
        case ExecType::Calculated: return "B";
        case ExecType::Expired: return "C";
        case ExecType::Restated: return "D";
        case ExecType::PendingReplace: return "E";
        case ExecType::Trade: return "F";
        case ExecType::TradeCorrect: return "G";
        case ExecType::TradeCancel: return "H";
        case ExecType::OrderStatus: return "I";
    }
    return "?";
}

std::optional<OrdStatus> ord_status_from_wire(std::string_view v) {
    if (v.size() != 1) return std::nullopt;
    switch (v[0]) {
        case '0': return OrdStatus::New;
        case '1': return OrdStatus::PartiallyFilled;
        case '2': return OrdStatus::Filled;
        case '3': return OrdStatus::DoneForDay;
        case '4': return OrdStatus::Canceled;
        case '6': return OrdStatus::PendingCancel;
        case '7': return OrdStatus::Stopped;
        case '8': return OrdStatus::Rejected;
        case '9': return OrdStatus::Suspended;
        case 'A': return OrdStatus::PendingNew;
        case 'B': return OrdStatus::Calculated;
        case 'C': return OrdStatus::Expired;
        case 'D': return OrdStatus::AcceptedForBidding;
        case 'E': return OrdStatus::PendingReplace;
        default: return std::nullopt;
    }
}

std::string_view ord_status_to_wire(OrdStatus s) {
    switch (s) {
        case OrdStatus::New: return "0";
        case OrdStatus::PartiallyFilled: return "1";
        case OrdStatus::Filled: return "2";
        case OrdStatus::DoneForDay: return "3";
        case OrdStatus::Canceled: return "4";
        case OrdStatus::PendingCancel: return "6";
        case OrdStatus::Stopped: return "7";
        case OrdStatus::Rejected: return "8";
        case OrdStatus::Suspended: return "9";
        case OrdStatus::PendingNew: return "A";
        case OrdStatus::Calculated: return "B";
        case OrdStatus::Expired: return "C";
        case OrdStatus::AcceptedForBidding: return "D";
        case OrdStatus::PendingReplace: return "E";
    }
    return "?";
}

std::optional<Side> side_from_wire(std::string_view v) {
    if (v == "1") return Side::Buy;
    if (v == "2") return Side::Sell;
    return std::nullopt;
}
std::string_view side_to_wire(Side s) { return s == Side::Buy ? "1" : "2"; }

std::optional<OrdType> ord_type_from_wire(std::string_view v) {
    if (v == "1") return OrdType::Market;
    if (v == "2") return OrdType::Limit;
    if (v == "3") return OrdType::Stop;
    if (v == "4") return OrdType::StopLimit;
    return std::nullopt;
}
std::string_view ord_type_to_wire(OrdType t) {
    switch (t) {
        case OrdType::Market: return "1";
        case OrdType::Limit: return "2";
        case OrdType::Stop: return "3";
        case OrdType::StopLimit: return "4";
    }
    return "?";
}

std::optional<TimeInForce> tif_from_wire(std::string_view v) {
    if (v == "0") return TimeInForce::Day;
    if (v == "1") return TimeInForce::GoodTillCancel;
    if (v == "3") return TimeInForce::ImmediateOrCancel;
    if (v == "4") return TimeInForce::FillOrKill;
    return std::nullopt;
}
std::string_view tif_to_wire(TimeInForce t) {
    switch (t) {
        case TimeInForce::Day: return "0";
        case TimeInForce::GoodTillCancel: return "1";
        case TimeInForce::ImmediateOrCancel: return "3";
        case TimeInForce::FillOrKill: return "4";
    }
    return "?";
}

std::optional<CxlRejResponseTo> cxl_rej_response_to_from_wire(std::string_view v) {
    if (v == "1") return CxlRejResponseTo::OrderCancelRequest;
    if (v == "2") return CxlRejResponseTo::OrderCancelReplaceRequest;
    return std::nullopt;
}
std::string_view cxl_rej_response_to_wire(CxlRejResponseTo r) {
    return r == CxlRejResponseTo::OrderCancelRequest ? "1" : "2";
}

bool is_unsupported_in_v1(ExecType t) {
    return t == ExecType::Restated || t == ExecType::TradeCorrect || t == ExecType::TradeCancel;
}

// ---------------------------------------------------------------------------
// Encoding.

namespace {

// Shared validation for anything that goes out. The caller is trusted to have
// resolved the profile, but not trusted to have kept the record intact.
CheckResult check_outbound_common(const ClientOrderId& cl, const std::string& symbol,
                                  const std::string& transact_time) {
    if (cl.empty() || cl.str().size() > 64) return CheckResult::err(ValidationError::FieldTooLong);
    if (!printable(cl.str())) return CheckResult::err(ValidationError::NonPrintableValue);
    if (symbol.empty() || !printable(symbol))
        return CheckResult::err(ValidationError::NonPrintableValue);
    if (transact_time.size() != 21) return CheckResult::err(ValidationError::BadTagFormat);
    return CheckResult::ok(true);
}

CheckResult check_qty_price(std::int64_t qty, std::int64_t price_ticks, OrdType ord_type) {
    if (qty <= 0) return CheckResult::err(ValidationError::ValueOutOfRange);
    if (ord_type == OrdType::Limit && price_ticks <= 0)
        return CheckResult::err(ValidationError::ValueOutOfRange);
    return CheckResult::ok(true);
}

}  // namespace

Result<Message, ValidationError> encode_new_order(const NewOrderFields& f) {
    auto c = check_outbound_common(f.cl_ord_id, f.wire_symbol, f.transact_time);
    if (c.is_err()) return MsgResult::err(c.error());
    auto q = check_qty_price(f.qty, f.price_ticks, f.ord_type);
    if (q.is_err()) return MsgResult::err(q.error());

    Message m;
    m.push(tag::kMsgType, "D");
    m.push(tag::kClOrdID, f.cl_ord_id.str());
    m.push(tag::kAccount, f.account.str());
    m.push(tag::kSymbol, f.wire_symbol);
    m.push(tag::kSide, std::string(side_to_wire(f.side)));
    m.push(tag::kTransactTime, f.transact_time);
    m.push(tag::kOrderQty, format_decimal_ticks(f.qty, 0));
    m.push(tag::kOrdType, std::string(ord_type_to_wire(f.ord_type)));
    if (f.ord_type == OrdType::Limit)
        m.push(tag::kPrice, format_decimal_ticks(f.price_ticks, f.price_scale));
    m.push(tag::kTimeInForce, std::string(tif_to_wire(f.tif)));
    return MsgResult::ok(std::move(m));
}

Result<Message, ValidationError> encode_cancel(const CancelFields& f) {
    auto c = check_outbound_common(f.cl_ord_id, f.wire_symbol, f.transact_time);
    if (c.is_err()) return MsgResult::err(c.error());
    if (f.orig_cl_ord_id.empty()) return MsgResult::err(ValidationError::MissingRequiredTag);
    if (f.cl_ord_id == f.orig_cl_ord_id)
        // R-I04: a cancel request owns a fresh ClOrdID, never the order's own.
        return MsgResult::err(ValidationError::BadTagFormat);

    Message m;
    m.push(tag::kMsgType, "F");
    m.push(tag::kOrigClOrdID, f.orig_cl_ord_id.str());
    m.push(tag::kClOrdID, f.cl_ord_id.str());
    m.push(tag::kAccount, f.account.str());
    m.push(tag::kSymbol, f.wire_symbol);
    m.push(tag::kSide, std::string(side_to_wire(f.side)));
    m.push(tag::kTransactTime, f.transact_time);
    if (f.order_id) m.push(tag::kOrderID, f.order_id->str());
    return MsgResult::ok(std::move(m));
}

Result<Message, ValidationError> encode_replace(const ReplaceFields& f) {
    auto c = check_outbound_common(f.cl_ord_id, f.wire_symbol, f.transact_time);
    if (c.is_err()) return MsgResult::err(c.error());
    if (f.orig_cl_ord_id.empty()) return MsgResult::err(ValidationError::MissingRequiredTag);
    if (f.cl_ord_id == f.orig_cl_ord_id) return MsgResult::err(ValidationError::BadTagFormat);
    // OR-I-17: a replace that omits the quantity is not encodable. 38 on a G is
    // the whole intended order size, so "unset" has no safe interpretation.
    auto q = check_qty_price(f.total_qty, f.price_ticks, f.ord_type);
    if (q.is_err()) return MsgResult::err(q.error());

    Message m;
    m.push(tag::kMsgType, "G");
    m.push(tag::kOrigClOrdID, f.orig_cl_ord_id.str());
    m.push(tag::kClOrdID, f.cl_ord_id.str());
    m.push(tag::kAccount, f.account.str());
    m.push(tag::kSymbol, f.wire_symbol);
    m.push(tag::kSide, std::string(side_to_wire(f.side)));
    m.push(tag::kTransactTime, f.transact_time);
    m.push(tag::kOrderQty, format_decimal_ticks(f.total_qty, 0));
    m.push(tag::kOrdType, std::string(ord_type_to_wire(f.ord_type)));
    if (f.ord_type == OrdType::Limit)
        m.push(tag::kPrice, format_decimal_ticks(f.price_ticks, f.price_scale));
    m.push(tag::kTimeInForce, std::string(tif_to_wire(f.tif)));
    if (f.order_id) m.push(tag::kOrderID, f.order_id->str());
    return MsgResult::ok(std::move(m));
}

Result<Message, ValidationError> encode_status(const StatusFields& f) {
    if (f.cl_ord_id.empty()) return MsgResult::err(ValidationError::MissingRequiredTag);
    if (f.wire_symbol.empty()) return MsgResult::err(ValidationError::MissingRequiredTag);

    Message m;
    m.push(tag::kMsgType, "H");
    m.push(tag::kClOrdID, f.cl_ord_id.str());
    m.push(tag::kAccount, f.account.str());
    m.push(tag::kSymbol, f.wire_symbol);
    m.push(tag::kSide, std::string(side_to_wire(f.side)));
    if (f.order_id) m.push(tag::kOrderID, f.order_id->str());
    // 790 only when the peer has agreed to echo it (OR-N-04, OR-N-05).
    if (f.ord_status_req_id) m.push(tag::kOrdStatusReqID, f.ord_status_req_id->str());
    return MsgResult::ok(std::move(m));
}

// ---------------------------------------------------------------------------
// Parsing.

Result<Message, ValidationError> parse_app_frame(std::string_view frame, const ParseLimits& lim) {
    if (frame.size() > lim.max_frame_bytes) return MsgResult::err(ValidationError::PayloadTooLarge);
    if (frame.empty()) return MsgResult::err(ValidationError::MissingRequiredTag);

    std::vector<Field> fields;
    std::set<int> seen;
    std::size_t i = 0;
    while (i < frame.size()) {
        const std::size_t soh = frame.find(kSoh, i);
        if (soh == std::string_view::npos) {
            // A trailing fragment with no SOH means the frame was cut. P01 owns
            // framing, so anything reaching here unterminated is malformed.
            return MsgResult::err(ValidationError::BadTagFormat);
        }
        std::string_view field = frame.substr(i, soh - i);
        i = soh + 1;
        if (field.empty()) return MsgResult::err(ValidationError::BadTagFormat);
        if (field.size() > lim.max_field_bytes) return MsgResult::err(ValidationError::FieldTooLong);
        if (fields.size() >= lim.max_fields) return MsgResult::err(ValidationError::PayloadTooLarge);

        const std::size_t eq = field.find('=');
        if (eq == std::string_view::npos || eq == 0)
            return MsgResult::err(ValidationError::BadTagFormat);
        int tag = 0;
        for (std::size_t k = 0; k < eq; ++k) {
            const char c = field[k];
            if (c < '0' || c > '9') return MsgResult::err(ValidationError::BadTagFormat);
            tag = tag * 10 + (c - '0');
            if (tag > 100000) return MsgResult::err(ValidationError::BadTagFormat);
        }
        std::string_view value = field.substr(eq + 1);
        if (value.empty()) return MsgResult::err(ValidationError::BadTagFormat);
        if (!printable(value)) return MsgResult::err(ValidationError::NonPrintableValue);

        // No repeating groups are in scope, so a repeated tag is ambiguous. Doc
        // 9.2 forbids last-write-wins (OR-D-20).
        if (!seen.insert(tag).second) return MsgResult::err(ValidationError::DuplicateTag);

        // Must-understand policy: a tag outside the dictionary and outside the
        // agreed tolerated list is refused rather than ignored (OR-Q-17).
        if (tag >= 5000 && std::find(lim.tolerated_custom_tags.begin(),
                                     lim.tolerated_custom_tags.end(),
                                     tag) == lim.tolerated_custom_tags.end())
            return MsgResult::err(ValidationError::ProhibitedCustomTag);

        fields.push_back(Field{tag, std::string(value)});
    }

    if (fields.empty() || fields.front().tag != tag::kMsgType)
        return MsgResult::err(ValidationError::MissingRequiredTag);
    const std::string& mt = fields.front().value;
    if (mt.size() != 1) return MsgResult::err(ValidationError::UnsupportedMsgType);
    // Session messages belong to P01. If one shows up here, P01 routed it
    // wrongly and it must raise an incident, not be normalized (OR-J-09).
    switch (mt[0]) {
        case '8':
        case '9':
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case 'A':
            return MsgResult::err(ValidationError::NotApplicationMessage);
        default:
            return MsgResult::err(ValidationError::UnsupportedMsgType);
    }
    return MsgResult::ok(Message(std::move(fields)));
}

namespace {

// Strict unsigned integer field, used for quantities. OR-D-17 and OR-D-18 rely
// on the negative sign and the digit cap being refused here.
CheckResult qty_field(const Message& m, int tag, std::int64_t& out) {
    auto v = m.get(tag);
    if (!v) return CheckResult::err(ValidationError::MissingRequiredTag);
    auto r = parse_decimal_ticks(*v, 0, 18);
    if (r.is_err()) {
        switch (r.error()) {
            case NumError::Negative: return CheckResult::err(ValidationError::ValueOutOfRange);
            case NumError::TooManyDigits:
            case NumError::Overflow: return CheckResult::err(ValidationError::ValueOutOfRange);
            case NumError::Separator: return CheckResult::err(ValidationError::BadDecimalFormat);
            case NumError::TooPrecise: return CheckResult::err(ValidationError::BadDecimalFormat);
            default: return CheckResult::err(ValidationError::BadDecimalFormat);
        }
    }
    out = r.value();
    return CheckResult::ok(true);
}

CheckResult price_field(const Message& m, int tag, int scale, std::int64_t& out) {
    auto v = m.get(tag);
    if (!v) return CheckResult::err(ValidationError::MissingRequiredTag);
    auto r = parse_decimal_ticks(*v, scale, 18);
    if (r.is_err()) {
        switch (r.error()) {
            case NumError::NotDigit: return CheckResult::err(ValidationError::BadDecimalFormat);
            case NumError::Separator: return CheckResult::err(ValidationError::BadDecimalFormat);
            case NumError::TooPrecise: return CheckResult::err(ValidationError::BadDecimalFormat);
            case NumError::Negative: return CheckResult::err(ValidationError::ValueOutOfRange);
            case NumError::TooManyDigits:
            case NumError::Overflow: return CheckResult::err(ValidationError::ValueOutOfRange);
            default: return CheckResult::err(ValidationError::BadDecimalFormat);
        }
    }
    out = r.value();
    return CheckResult::ok(true);
}

}  // namespace

Result<bool, ValidationError> validate_execution_report(const Message& m, const Config& cfg) {
    if (m.msg_type() != '8') return CheckResult::err(ValidationError::UnsupportedMsgType);

    auto et_raw = m.get(tag::kExecType);
    if (!et_raw) return CheckResult::err(ValidationError::MissingRequiredTag);
    auto et = exec_type_from_wire(*et_raw);
    if (!et) return CheckResult::err(ValidationError::UnknownExecType);

    auto os_raw = m.get(tag::kOrdStatus);
    if (!os_raw) return CheckResult::err(ValidationError::MissingRequiredTag);
    if (!ord_status_from_wire(*os_raw)) return CheckResult::err(ValidationError::UnknownOrdStatus);

    // Always required on a 4.4 ExecutionReport in this profile (OR-D-09).
    for (int t : {tag::kOrderID, tag::kExecID, tag::kClOrdID, tag::kSymbol, tag::kSide,
                  tag::kCumQty, tag::kLeavesQty, tag::kAvgPx})
        if (!m.has(t)) return CheckResult::err(ValidationError::MissingRequiredTag);

    auto side = side_from_wire(m.get_or(tag::kSide));
    if (!side) return CheckResult::err(ValidationError::BadTagFormat);

    const std::string sym = m.get_or(tag::kSymbol);
    const SymbolProfile* prof = cfg.symbol(sym);
    // An unknown symbol is not a parse error: correlation decides whether this
    // is a cross-symbol contradiction. Use the default scale for the numeric
    // checks so the report can still be rejected on its own terms.
    const int scale = prof ? prof->price_scale : 5;

    std::int64_t cum = 0, leaves = 0;
    auto c1 = qty_field(m, tag::kCumQty, cum);
    if (c1.is_err()) return c1;
    auto c2 = qty_field(m, tag::kLeavesQty, leaves);
    if (c2.is_err()) return c2;

    std::int64_t avg = 0;
    auto c3 = price_field(m, tag::kAvgPx, scale, avg);
    if (c3.is_err()) return c3;

    if (m.has(tag::kOrderQty)) {
        std::int64_t ordqty = 0;
        auto c4 = qty_field(m, tag::kOrderQty, ordqty);
        if (c4.is_err()) return c4;
        // OR-J-18: a working order whose CumQty plus LeavesQty does not equal
        // OrderQty is impossible. Terminal states legitimately have leaves 0
        // with cum below OrderQty (a cancel), so the relation is only enforced
        // where FIX makes it hold.
        const auto os = *ord_status_from_wire(*os_raw);
        const bool working = os == OrdStatus::New || os == OrdStatus::PartiallyFilled ||
                             os == OrdStatus::PendingCancel || os == OrdStatus::PendingReplace;
        std::int64_t sum = 0;
        if (!checked_add(cum, leaves, sum)) return CheckResult::err(ValidationError::ValueOutOfRange);
        if (working && sum != ordqty)
            return CheckResult::err(ValidationError::InconsistentQuantities);
        if (sum > ordqty) return CheckResult::err(ValidationError::InconsistentQuantities);
    }

    if (*et == ExecType::Trade) {
        // A Trade without the three fields that define it is a schema violation.
        // Inferring LastQty from a CumQty delta is exactly what R-I09 forbids.
        if (!m.has(tag::kLastQty) || !m.has(tag::kLastPx))
            return CheckResult::err(ValidationError::MissingTradeFields);
        if (m.get_or(tag::kExecID).empty())
            return CheckResult::err(ValidationError::MissingTradeFields);
        std::int64_t last_qty = 0;
        auto c5 = qty_field(m, tag::kLastQty, last_qty);
        if (c5.is_err()) return c5;
        if (last_qty == 0) return CheckResult::err(ValidationError::ZeroLastQty);
        std::int64_t last_px = 0;
        auto c6 = price_field(m, tag::kLastPx, scale, last_px);
        if (c6.is_err()) return c6;
        // An execution at price zero is not a valid FX trade; accepting it would
        // put a fill in the ledger that nothing justifies (R-I09).
        if (last_px <= 0) return CheckResult::err(ValidationError::ValueOutOfRange);
        if (prof && last_px > prof->max_price_ticks)
            return CheckResult::err(ValidationError::ValueOutOfRange);
        if (last_qty > cum) return CheckResult::err(ValidationError::InconsistentQuantities);
    }

    if (*et == ExecType::OrderStatus) {
        // A status snapshot must not carry trade fields: that is how a snapshot
        // would smuggle in a fill (R-I09, OR-D-06).
        if (m.has(tag::kLastQty)) {
            std::int64_t last_qty = 0;
            auto c7 = qty_field(m, tag::kLastQty, last_qty);
            if (c7.is_err()) return c7;
            if (last_qty != 0) return CheckResult::err(ValidationError::InconsistentQuantities);
        }
    }

    return CheckResult::ok(true);
}

Result<bool, ValidationError> validate_cancel_reject(const Message& m) {
    if (m.msg_type() != '9') return CheckResult::err(ValidationError::UnsupportedMsgType);
    // 434 is what tells us whether an F or a G was rejected. Without it the
    // message cannot be applied to either (OR-D-10, OR-H-05).
    auto rt = m.get(tag::kCxlRejResponseTo);
    if (!rt) return CheckResult::err(ValidationError::MissingCancelRejectResponseTo);
    if (!cxl_rej_response_to_from_wire(*rt))
        return CheckResult::err(ValidationError::MissingCancelRejectResponseTo);
    for (int t : {tag::kClOrdID, tag::kOrigClOrdID, tag::kOrdStatus, tag::kOrderID})
        if (!m.has(t)) return CheckResult::err(ValidationError::MissingRequiredTag);
    if (!ord_status_from_wire(m.get_or(tag::kOrdStatus)))
        return CheckResult::err(ValidationError::UnknownOrdStatus);
    return CheckResult::ok(true);
}

}  // namespace router::fix
