// ExecutionReport(35=8), OrderCancelReject(35=9) and BusinessMessageReject(35=j)
// construction, plus an independent conformance oracle.
//
// Reference: design doc 7.3, which lists the builder rules, and playbook PB-17,
// which requires that a mutated builder be caught BEFORE the outbound commit
// rather than after a client complains.
//
// The oracle is deliberately separate from the builder and reads only the
// finished body. That separation is what scenarios M-10 and M-11 are about: a
// test that checks only the wire, or only the database, passes for the wrong
// reason. check_report_conformance() can be pointed at a hand-corrupted body in
// a test, and build_report() runs it on its own output so a builder bug fails
// closed instead of reaching the socket.
#pragma once

#include <string>
#include <string_view>

#include "lp/domain.hpp"

namespace lpsim {

enum class BuildError {
    Ok = 0,
    MissingOrderId,           // 37 absent                             E-15
    MissingExecId,            // 17 absent on an ExecutionReport
    MissingClOrdId,           // 11 absent
    MissingOrigClOrdId,       // 41 absent where required              F-15
    TradeWithoutLast,         // 150=F without 32 or 31                E-16
    NonTradeWithLast,         // 32 or 31 on a non-trade report        E-18
    LastQtyNotPositive,       // 150=F with 32 <= 0
    TerminalLeavesNonZero,    // terminal status with 151 != 0         E-17
    CumExceedsOrderQty,       // 14 > 38                               E-13
    LeavesMismatch,           // 151 disagrees with 38 - 14 while active
    MissingRejectReason,      // 150=8 without 103, or 35=9 without 102
    MissingCxlRejResponseTo,  // 35=9 without 434                      F-16
    BadFieldValue,            // an enumerated field outside FIX 4.4
    WrongMessageType,         // a cancel refusal built as 35=8        F-16
};

std::string_view to_string(BuildError e);

// What the business actor asks for. The builder owns every derived field, so a
// caller cannot hand it an inconsistent CumQty and have it serialised.
struct ReportRequest {
    ReportType type = ReportType::ExecNew;
    const OrderChain* chain = nullptr;  // required for every type except BusinessReject
    PriceFormat price;

    std::string exec_id;
    // The ClOrdID this report answers. For a trade or an unsolicited report
    // that is the current accepted version; for a pending or rejected request
    // it is the request's own ClOrdID.
    std::string cl_ord_id;
    std::string orig_cl_ord_id;

    // Describe the REFUSED REQUEST, for ExecRejected only.
    //
    // A rejected New never borrows an existing chain's quantities, even when
    // the refusal is because its ClOrdID already belongs to one (C-04..C-06).
    // OrdStatus=8 is a statement about this request, so it reports CumQty 0 and
    // LeavesQty 0; copying a live chain's LeavesQty would produce a report
    // claiming an order is both rejected and still working, which the
    // conformance oracle refuses outright.
    std::string symbol;
    Side side = Side::Buy;
    Units order_qty = 0;

    // Trade only.
    Units last_qty = 0;
    Ticks last_px = 0;

    // Rejects only.
    BizReject reject = BizReject::None;

    // BusinessMessageReject only.
    std::uint64_t ref_seq_num = 0;
    std::string ref_msg_type;
    std::string ref_id;

    // Set when the report refers to an order the LP never accepted, in which
    // case tag 37 carries the profile placeholder rather than being omitted:
    // OrderID is a required field and dropping it is not an option (F-03).
    bool unknown_order = false;
};

// Placeholder OrderID used when no chain exists. Design doc 7.3 pins this so
// that a client can tell "no order" from "a real order whose id is missing".
constexpr std::string_view kUnknownOrderId = "NONE";

// Build the canonical business body. On any BuildError the output is left empty
// and nothing may be committed to the outbox.
BuildError build_report(const ReportRequest& req, CanonicalBody* out);

// Independent oracle: validate a finished body against the FIX 4.4 rules for
// its message type. Pass the MsgType the body is destined for.
BuildError check_report_conformance(std::string_view msg_type, const CanonicalBody& body);

// Convenience accessor used by tests and the audit layer.
bool body_field(const CanonicalBody& body, int tag, std::string* out);

}  // namespace lpsim
