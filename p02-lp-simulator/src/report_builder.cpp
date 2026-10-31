#include "lp/report_builder.hpp"

#include <cstdlib>

namespace lpsim {

std::string_view to_string(BuildError e) {
    switch (e) {
        case BuildError::Ok: return "Ok";
        case BuildError::MissingOrderId: return "MissingOrderId";
        case BuildError::MissingExecId: return "MissingExecId";
        case BuildError::MissingClOrdId: return "MissingClOrdId";
        case BuildError::MissingOrigClOrdId: return "MissingOrigClOrdId";
        case BuildError::TradeWithoutLast: return "TradeWithoutLast";
        case BuildError::NonTradeWithLast: return "NonTradeWithLast";
        case BuildError::LastQtyNotPositive: return "LastQtyNotPositive";
        case BuildError::TerminalLeavesNonZero: return "TerminalLeavesNonZero";
        case BuildError::CumExceedsOrderQty: return "CumExceedsOrderQty";
        case BuildError::LeavesMismatch: return "LeavesMismatch";
        case BuildError::MissingRejectReason: return "MissingRejectReason";
        case BuildError::MissingCxlRejResponseTo: return "MissingCxlRejResponseTo";
        case BuildError::BadFieldValue: return "BadFieldValue";
        case BuildError::WrongMessageType: return "WrongMessageType";
    }
    return "Unknown";
}

bool body_field(const CanonicalBody& body, int tag, std::string* out) {
    for (const auto& kv : body) {
        if (kv.first == tag) {
            if (out != nullptr) *out = kv.second;
            return true;
        }
    }
    return false;
}

namespace {

void put(CanonicalBody* b, int tag, std::string value) {
    b->emplace_back(tag, std::move(value));
}

std::string_view exec_type_for(ReportType t) {
    switch (t) {
        case ReportType::ExecNew: return exec_type::New;
        case ReportType::ExecTrade: return exec_type::Trade;
        case ReportType::ExecCanceled: return exec_type::Canceled;
        case ReportType::ExecReplaced: return exec_type::Replaced;
        case ReportType::ExecPendingCancel: return exec_type::PendingCancel;
        case ReportType::ExecPendingReplace: return exec_type::PendingReplace;
        case ReportType::ExecRejected: return exec_type::Rejected;
        case ReportType::ExecExpired: return exec_type::Canceled;  // overridden below
        default: return exec_type::New;
    }
}

bool is_terminal_status(std::string_view s) {
    return s == ord_status::Filled || s == ord_status::Canceled || s == ord_status::Expired ||
           s == ord_status::Rejected;
}

bool parse_i64(const std::string& s, long long* out) {
    if (s.empty()) return false;
    long long v = 0;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

// A decimal price string compares correctly as a number only after parsing, so
// the oracle checks presence and sign rather than relative value: price-versus
// limit is a matcher invariant (I-11), asserted there and in the ledger tests.
bool positive_decimal(const std::string& s) {
    bool any_nonzero = false;
    for (char c : s) {
        if (c == '.') continue;
        if (c < '0' || c > '9') return false;
        if (c != '0') any_nonzero = true;
    }
    return any_nonzero;
}

}  // namespace

BuildError build_report(const ReportRequest& req, CanonicalBody* out) {
    out->clear();
    CanonicalBody b;

    if (req.type == ReportType::BusinessReject) {
        // 35=j carries no order identity at all: it answers a message, not an
        // order (design doc 7.2).
        put(&b, tag::RefSeqNum, std::to_string(req.ref_seq_num));
        put(&b, tag::RefMsgType, req.ref_msg_type);
        if (!req.ref_id.empty()) put(&b, tag::BusinessRejectRefID, req.ref_id);
        put(&b, tag::BusinessRejectReason,
            std::to_string(static_cast<int>(BusinessRejectReason::UnsupportedMessageType)));
        put(&b, tag::Text, std::string(reject_text(req.reject)));
        const BuildError e = check_report_conformance(msg_type::BusinessMessageReject, b);
        if (e != BuildError::Ok) return e;
        *out = std::move(b);
        return BuildError::Ok;
    }

    if (req.cl_ord_id.empty()) return BuildError::MissingClOrdId;

    const OrderChain* o = req.chain;
    const std::string order_id =
        (o != nullptr && !o->order_id.empty()) ? o->order_id : std::string(kUnknownOrderId);
    if (order_id.empty()) return BuildError::MissingOrderId;

    // ---- OrderCancelReject(35=9) -----------------------------------------
    if (req.type == ReportType::CancelReject) {
        put(&b, tag::OrderID, order_id);
        put(&b, tag::ClOrdID, req.cl_ord_id);
        if (!req.orig_cl_ord_id.empty()) put(&b, tag::OrigClOrdID, req.orig_cl_ord_id);
        else if (!req.unknown_order) return BuildError::MissingOrigClOrdId;  // F-15
        // 39 must show the CURRENT state of the chain, not the state the
        // request assumed (F-17).
        put(&b, tag::OrdStatus,
            std::string(o != nullptr ? o->fix_ord_status() : ord_status::Rejected));
        put(&b, tag::CxlRejResponseTo,
            std::string(req.ref_msg_type == std::string(msg_type::OrderCancelReplaceRequest)
                            ? cxl_rej_response_to::CancelReplaceRequest
                            : cxl_rej_response_to::CancelRequest));
        put(&b, tag::CxlRejReason, std::to_string(cxl_rej_reason(req.reject)));
        put(&b, tag::Text, std::string(reject_text(req.reject)));

        const BuildError e = check_report_conformance(msg_type::OrderCancelReject, b);
        if (e != BuildError::Ok) return e;
        *out = std::move(b);
        return BuildError::Ok;
    }

    // ---- ExecutionReport(35=8) -------------------------------------------
    if (req.exec_id.empty()) return BuildError::MissingExecId;

    std::string_view et = exec_type_for(req.type);
    std::string_view status;
    Units order_qty = 0;
    Units cum_qty = 0;
    Units leaves = 0;
    Ticks avg_px = 0;
    std::string symbol;
    std::string_view side_v = side_code::Buy;

    if (req.type == ReportType::ExecRejected) {
        // A refused request is reported on its own terms, never on the terms of
        // some other chain that happens to share its ClOrdID. OrderID is the
        // placeholder because this request never became an order.
        status = ord_status::Rejected;
        et = exec_type::Rejected;
        symbol = req.symbol;
        side_v = fix_side(req.side);
        order_qty = req.order_qty;
        cum_qty = 0;
        leaves = 0;
        avg_px = 0;
    } else {
        if (o == nullptr) return BuildError::MissingOrderId;
        status = o->fix_ord_status();
        order_qty = o->effective_qty;
        cum_qty = o->cum_qty;
        leaves = o->leaves_qty;
        avg_px = o->avg_px_ticks;
        symbol = o->symbol;
        side_v = fix_side(o->side);
        if (req.type == ReportType::ExecExpired) {
            // FIX 4.4 has no ExecType for expiry separate from the status, so
            // the report is a Canceled-type execution with OrdStatus=C.
            et = exec_type::Canceled;
            status = ord_status::Expired;
        }
    }

    // A rejected request is not an order, so it carries the placeholder rather
    // than the identity of a chain it was refused against.
    const std::string reported_order_id =
        req.type == ReportType::ExecRejected ? std::string(kUnknownOrderId) : order_id;
    put(&b, tag::OrderID, reported_order_id);
    put(&b, tag::ClOrdID, req.cl_ord_id);
    if (!req.orig_cl_ord_id.empty()) put(&b, tag::OrigClOrdID, req.orig_cl_ord_id);
    put(&b, tag::ExecID, req.exec_id);
    put(&b, tag::ExecType, std::string(et));
    put(&b, tag::OrdStatus, std::string(status));
    if (!symbol.empty()) put(&b, tag::Symbol, symbol);
    put(&b, tag::Side, std::string(side_v));
    put(&b, tag::OrderQty, format_qty(order_qty));
    if (req.type == ReportType::ExecTrade) {
        if (req.last_qty <= 0) return BuildError::LastQtyNotPositive;
        if (req.last_px <= 0) return BuildError::TradeWithoutLast;
        put(&b, tag::LastQty, format_qty(req.last_qty));
        put(&b, tag::LastPx, format_price(req.last_px, req.price.scale));
    }
    put(&b, tag::CumQty, format_qty(cum_qty));
    put(&b, tag::LeavesQty, format_qty(leaves));
    put(&b, tag::AvgPx, format_price(avg_px, req.price.scale));
    if (req.type == ReportType::ExecRejected) {
        put(&b, tag::OrdRejReason, std::to_string(ord_rej_reason(req.reject)));
        put(&b, tag::Text, std::string(reject_text(req.reject)));
    }

    const BuildError e = check_report_conformance(msg_type::ExecutionReport, b);
    if (e != BuildError::Ok) return e;
    *out = std::move(b);
    return BuildError::Ok;
}

BuildError check_report_conformance(std::string_view mt, const CanonicalBody& body) {
    std::string v;

    if (mt == msg_type::BusinessMessageReject) {
        if (!body_field(body, tag::RefMsgType, nullptr)) return BuildError::BadFieldValue;
        if (!body_field(body, tag::BusinessRejectReason, nullptr))
            return BuildError::MissingRejectReason;
        return BuildError::Ok;
    }

    if (mt == msg_type::OrderCancelReject) {
        if (!body_field(body, tag::OrderID, nullptr)) return BuildError::MissingOrderId;
        if (!body_field(body, tag::ClOrdID, nullptr)) return BuildError::MissingClOrdId;
        if (!body_field(body, tag::OrdStatus, nullptr)) return BuildError::BadFieldValue;
        if (!body_field(body, tag::CxlRejResponseTo, &v))
            return BuildError::MissingCxlRejResponseTo;
        if (v != cxl_rej_response_to::CancelRequest &&
            v != cxl_rej_response_to::CancelReplaceRequest)
            return BuildError::BadFieldValue;
        if (!body_field(body, tag::CxlRejReason, nullptr)) return BuildError::MissingRejectReason;
        // A cancel refusal must never be an ExecutionReport field set (F-16).
        if (body_field(body, tag::ExecType, nullptr)) return BuildError::WrongMessageType;
        return BuildError::Ok;
    }

    if (mt != msg_type::ExecutionReport) return BuildError::WrongMessageType;

    if (!body_field(body, tag::OrderID, nullptr)) return BuildError::MissingOrderId;
    if (!body_field(body, tag::ClOrdID, nullptr)) return BuildError::MissingClOrdId;
    if (!body_field(body, tag::ExecID, nullptr)) return BuildError::MissingExecId;

    std::string exec_t;
    if (!body_field(body, tag::ExecType, &exec_t)) return BuildError::BadFieldValue;
    // FIX 4.4: 150=1 and 150=2 are the FIX 4.2 fill codes and are not valid
    // ExecType values here. Catching them is the whole point of PB-17.
    if (exec_t != exec_type::New && exec_t != exec_type::Trade &&
        exec_t != exec_type::Canceled && exec_t != exec_type::Replaced &&
        exec_t != exec_type::PendingCancel && exec_t != exec_type::PendingReplace &&
        exec_t != exec_type::Rejected)
        return BuildError::BadFieldValue;

    std::string status;
    if (!body_field(body, tag::OrdStatus, &status)) return BuildError::BadFieldValue;
    if (status != ord_status::New && status != ord_status::PartiallyFilled &&
        status != ord_status::Filled && status != ord_status::Canceled &&
        status != ord_status::PendingCancel && status != ord_status::Rejected &&
        status != ord_status::Expired && status != ord_status::PendingReplace)
        return BuildError::BadFieldValue;

    if (!body_field(body, tag::Side, &v)) return BuildError::BadFieldValue;
    if (v != side_code::Buy && v != side_code::Sell) return BuildError::BadFieldValue;

    std::string qty_s, cum_s, leaves_s;
    if (!body_field(body, tag::OrderQty, &qty_s)) return BuildError::BadFieldValue;
    if (!body_field(body, tag::CumQty, &cum_s)) return BuildError::BadFieldValue;
    if (!body_field(body, tag::LeavesQty, &leaves_s)) return BuildError::BadFieldValue;
    if (!body_field(body, tag::AvgPx, nullptr)) return BuildError::BadFieldValue;

    long long order_qty = 0, cum = 0, leaves = 0;
    if (!parse_i64(qty_s, &order_qty) || !parse_i64(cum_s, &cum) ||
        !parse_i64(leaves_s, &leaves))
        return BuildError::BadFieldValue;
    if (order_qty < 0 || cum < 0 || leaves < 0) return BuildError::BadFieldValue;

    const bool has_last_qty = body_field(body, tag::LastQty, &v);
    const bool has_last_px = body_field(body, tag::LastPx, nullptr);

    if (exec_t == exec_type::Trade) {
        // A trade must say how much traded and at what price (E-16).
        if (!has_last_qty || !has_last_px) return BuildError::TradeWithoutLast;
        long long last_qty = 0;
        if (!parse_i64(v, &last_qty)) return BuildError::BadFieldValue;
        if (last_qty <= 0) return BuildError::LastQtyNotPositive;
        std::string last_px;
        body_field(body, tag::LastPx, &last_px);
        if (!positive_decimal(last_px)) return BuildError::BadFieldValue;
    } else {
        // A non-trade report that carries trade fields would be read as a fill
        // by a conforming client (E-18).
        if (has_last_qty || has_last_px) return BuildError::NonTradeWithLast;
    }

    if (exec_t == exec_type::Rejected && !body_field(body, tag::OrdRejReason, nullptr))
        return BuildError::MissingRejectReason;
    if (exec_t == exec_type::Replaced && !body_field(body, tag::OrigClOrdID, nullptr))
        return BuildError::MissingOrigClOrdId;

    if (cum > order_qty && status != ord_status::Rejected) return BuildError::CumExceedsOrderQty;

    if (is_terminal_status(status)) {
        // Including a cancel after a partial fill: LeavesQty is exactly zero,
        // which is invariant I-04 (E-17).
        if (leaves != 0) return BuildError::TerminalLeavesNonZero;
    } else {
        // While the order is live, LeavesQty must agree with OrderQty - CumQty.
        if (leaves != order_qty - cum) return BuildError::LeavesMismatch;
    }

    return BuildError::Ok;
}

}  // namespace lpsim
