// ExecutionReport and OrderCancelReject construction, and the independent
// conformance oracle.
//
// Matrix coverage: E-01, E-13, E-15 to E-18, F-15, F-16, F-17 and playbook
// PB-17. This is where the FIX 4.4 report rules are nailed down, including the
// one the design doc flags as a release blocker: 150=1 and 150=2 are FIX 4.2
// fill codes and must never appear as an ExecType here.
//
// The suite runs the oracle BOTH ways round. First it checks that the builder
// cannot emit a malformed report, and then it hands hand-corrupted bodies to
// the oracle directly, which is what proves the oracle would catch a future
// builder regression rather than agreeing with it by construction (M-11).

#include <string>

#include "lp/domain.hpp"
#include "lp/report_builder.hpp"
#include "../support/tiny_test.hpp"

using namespace lpsim;

namespace {

const PriceFormat kFx5{5, 1};

OrderChain chain(Units qty, Units cum, BaseState base, Pending pending = Pending::None) {
    OrderChain o;
    o.order_id = "SIM-1001";
    o.firm = "TRADER01";
    o.business_day = "20261030";
    o.original_cl_ord_id = "O1";
    o.current_cl_ord_id = "O1";
    o.symbol = "EURUSD";
    o.side = Side::Buy;
    o.ord_type = OrdType::Limit;
    o.tif = Tif::Day;
    o.original_qty = qty;
    o.effective_qty = qty;
    o.limit_ticks = 108500;
    o.cum_qty = cum;
    o.base = base;
    o.pending = pending;
    o.leaves_qty = o.expected_leaves();
    o.avg_px_ticks = cum > 0 ? 108490 : 0;
    return o;
}

ReportRequest request(const OrderChain& c) {
    ReportRequest r;
    r.chain = &c;
    r.price = kFx5;
    r.cl_ord_id = c.current_cl_ord_id;
    r.exec_id = "EX-000001";
    return r;
}

std::string get(const CanonicalBody& b, int tag_num) {
    std::string v;
    body_field(b, tag_num, &v);
    return v;
}

bool has(const CanonicalBody& b, int tag_num) { return body_field(b, tag_num, nullptr); }

// Replace a tag's value in a finished body, to corrupt it for the oracle.
CanonicalBody with(const CanonicalBody& b, int tag_num, const std::string& value) {
    CanonicalBody out = b;
    for (auto& kv : out) {
        if (kv.first == tag_num) {
            kv.second = value;
            return out;
        }
    }
    out.emplace_back(tag_num, value);
    return out;
}

CanonicalBody without(const CanonicalBody& b, int tag_num) {
    CanonicalBody out;
    for (const auto& kv : b) {
        if (kv.first != tag_num) out.push_back(kv);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Well-formed reports
// ---------------------------------------------------------------------------

SCENARIO("E-01", "P0", "a New acknowledgement carries 150=0, 39=0, 14=0 and 151=qty") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    CanonicalBody b;
    CHECK_EQ(build_report(request(c), &b), BuildError::Ok);

    CHECK_EQ(get(b, tag::ExecType), std::string("0"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("0"));
    CHECK_EQ(get(b, tag::CumQty), std::string("0"));
    CHECK_EQ(get(b, tag::LeavesQty), std::string("100000"));
    CHECK_EQ(get(b, tag::AvgPx), std::string("0.00000"));
    CHECK_EQ(get(b, tag::OrderID), std::string("SIM-1001"));
    CHECK_EQ(get(b, tag::ClOrdID), std::string("O1"));
    CHECK_EQ(get(b, tag::Side), std::string("1"));
    CHECK_EQ(get(b, tag::Symbol), std::string("EURUSD"));
    // A no-fill report must not carry trade fields (E-18).
    CHECK(!has(b, tag::LastQty));
    CHECK(!has(b, tag::LastPx));
}

SCENARIO("E-02", "P0", "a partial fill reports 150=F with 39=1 and the traded amounts") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    // FIX 4.4: a trade is 150=F. 150=1 is the FIX 4.2 code and is wrong here.
    CHECK_EQ(get(b, tag::ExecType), std::string("F"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("1"));
    CHECK_EQ(get(b, tag::CumQty), std::string("40000"));
    CHECK_EQ(get(b, tag::LeavesQty), std::string("60000"));
    CHECK_EQ(get(b, tag::LastQty), std::string("40000"));
    CHECK_EQ(get(b, tag::LastPx), std::string("1.08490"));
}

SCENARIO("E-03", "P0", "a final fill reports 39=2 with LeavesQty zero") {
    const OrderChain c = chain(100000, 100000, BaseState::Filled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 60000;
    r.last_px = 108500;

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::ExecType), std::string("F"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("2"));
    CHECK_EQ(get(b, tag::CumQty), std::string("100000"));
    CHECK_EQ(get(b, tag::LeavesQty), std::string("0"));
}

SCENARIO("F-02", "P0", "a cancel after a partial fill keeps CumQty and zeroes LeavesQty") {
    // Invariant I-04: OrderQty = CumQty + LeavesQty does NOT hold here, and
    // that is correct. 40000 traded, 60000 never will.
    const OrderChain c = chain(100000, 40000, BaseState::Canceled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecCanceled;
    r.cl_ord_id = "C1";
    r.orig_cl_ord_id = "O1";

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::ExecType), std::string("4"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("4"));
    CHECK_EQ(get(b, tag::CumQty), std::string("40000"));
    CHECK_EQ(get(b, tag::LeavesQty), std::string("0"));
    CHECK_EQ(get(b, tag::OrderQty), std::string("100000"));
    CHECK_EQ(get(b, tag::ClOrdID), std::string("C1"));
    CHECK_EQ(get(b, tag::OrigClOrdID), std::string("O1"));
}

SCENARIO("F-09r", "P0", "a trade while a cancel is pending reports the pending status") {
    // FIX status precedence: a pending request outranks the underlying state
    // while the order is still live (design doc 6.3, 9.2).
    const OrderChain c = chain(100000, 60000, BaseState::PartiallyFilled, Pending::Cancel);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 20000;
    r.last_px = 108495;

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::ExecType), std::string("F"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("6"));  // PendingCancel
    CHECK_EQ(get(b, tag::CumQty), std::string("60000"));
}

SCENARIO("F-10r", "P0", "once terminal, the terminal status outranks a pending request") {
    // The order filled while the cancel was in flight. Reporting 39=6 now would
    // tell the client the order is still live.
    const OrderChain c = chain(100000, 100000, BaseState::Filled, Pending::Cancel);
    CHECK_EQ(std::string(ord_status_for(c.base, c.pending)), std::string("2"));

    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108500;
    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::OrdStatus), std::string("2"));
    CHECK_EQ(get(b, tag::LeavesQty), std::string("0"));
}

SCENARIO("G-01", "P0", "a replace reports 150=5 and echoes the previous version") {
    OrderChain c = chain(150000, 40000, BaseState::PartiallyFilled);
    c.current_cl_ord_id = "R1";
    ReportRequest r = request(c);
    r.type = ReportType::ExecReplaced;
    r.cl_ord_id = "R1";
    r.orig_cl_ord_id = "O1";

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::ExecType), std::string("5"));
    CHECK_EQ(get(b, tag::OrderID), std::string("SIM-1001"));  // stable across replace
    CHECK_EQ(get(b, tag::ClOrdID), std::string("R1"));
    CHECK_EQ(get(b, tag::OrigClOrdID), std::string("O1"));
    CHECK_EQ(get(b, tag::OrderQty), std::string("150000"));
    CHECK_EQ(get(b, tag::CumQty), std::string("40000"));   // not reset by a replace
    CHECK_EQ(get(b, tag::LeavesQty), std::string("110000"));
}

SCENARIO("F-03", "P0", "a cancel for an unknown order is 35=9 with OrderID NONE") {
    ReportRequest r;
    r.type = ReportType::CancelReject;
    r.price = kFx5;
    r.cl_ord_id = "C1";
    r.reject = BizReject::UnknownOrder;
    r.unknown_order = true;
    r.ref_msg_type = std::string(msg_type::OrderCancelRequest);

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    // OrderID is a required field, so it carries the profile placeholder rather
    // than being dropped.
    CHECK_EQ(get(b, tag::OrderID), std::string("NONE"));
    CHECK_EQ(get(b, tag::CxlRejResponseTo), std::string("1"));
    CHECK_EQ(get(b, tag::CxlRejReason), std::string("1"));  // Unknown order
    CHECK(!has(b, tag::ExecType));  // a 35=9 is not an ExecutionReport
}

SCENARIO("G-05", "P0", "a replace refusal uses CxlRejResponseTo=2") {
    const OrderChain c = chain(100000, 100000, BaseState::Filled);
    ReportRequest r = request(c);
    r.type = ReportType::CancelReject;
    r.cl_ord_id = "R1";
    r.orig_cl_ord_id = "O1";
    r.reject = BizReject::TooLate;
    r.ref_msg_type = std::string(msg_type::OrderCancelReplaceRequest);

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::CxlRejResponseTo), std::string("2"));
    CHECK_EQ(get(b, tag::CxlRejReason), std::string("0"));  // Too late to cancel
    // F-17: the reject shows the CURRENT state, which is Filled.
    CHECK_EQ(get(b, tag::OrdStatus), std::string("2"));
}

SCENARIO("B-06r", "P0", "a refused New is 35=8 with ExecType=8 and an OrdRejReason") {
    ReportRequest r;
    r.type = ReportType::ExecRejected;
    r.price = kFx5;
    r.cl_ord_id = "O1";
    r.exec_id = "EX-000001";
    r.reject = BizReject::BadQuantity;
    r.ref_id = "EURUSD";

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(get(b, tag::ExecType), std::string("8"));
    CHECK_EQ(get(b, tag::OrdStatus), std::string("8"));
    CHECK_EQ(get(b, tag::OrdRejReason), std::string("13"));  // Incorrect quantity
    CHECK_EQ(get(b, tag::OrderID), std::string("NONE"));
    CHECK(!has(b, tag::LastQty));
}

SCENARIO("B-17r", "P0", "an unsupported MsgType is answered with 35=j, not a session reject") {
    ReportRequest r;
    r.type = ReportType::BusinessReject;
    r.price = kFx5;
    r.ref_seq_num = 42;
    r.ref_msg_type = "H";
    r.ref_id = "O1";
    r.reject = BizReject::UnsupportedMsgType;

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);
    CHECK_EQ(std::string(msg_type_for(ReportType::BusinessReject)), std::string("j"));
    CHECK_EQ(get(b, tag::RefMsgType), std::string("H"));
    CHECK_EQ(get(b, tag::RefSeqNum), std::string("42"));
    CHECK_EQ(get(b, tag::BusinessRejectReason), std::string("3"));  // Unsupported
}

// ---------------------------------------------------------------------------
// The builder refuses to emit a malformed report
// ---------------------------------------------------------------------------

SCENARIO("E-16", "P0", "a trade without LastQty or LastPx is refused before commit") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 0;  // missing price

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::TradeWithoutLast);
    CHECK(b.empty());

    r.last_px = 108490;
    r.last_qty = 0;  // missing quantity
    CHECK_EQ(build_report(r, &b), BuildError::LastQtyNotPositive);
    CHECK(b.empty());
}

SCENARIO("E-15", "P0", "a report with no order identity is refused") {
    ReportRequest r;
    r.type = ReportType::ExecTrade;
    r.price = kFx5;
    r.cl_ord_id = "O1";
    r.exec_id = "EX-000001";
    r.chain = nullptr;  // no chain and not a New rejection
    r.last_qty = 1000;
    r.last_px = 108490;

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::MissingOrderId);
    CHECK(b.empty());
}

SCENARIO("E-15b", "P0", "an ExecutionReport without an ExecID is refused") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    ReportRequest r = request(c);
    r.exec_id.clear();
    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::MissingExecId);
}

SCENARIO("F-15", "P0", "a cancel reject for a KNOWN order must echo OrigClOrdID") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    ReportRequest r = request(c);
    r.type = ReportType::CancelReject;
    r.cl_ord_id = "C1";
    r.orig_cl_ord_id.clear();  // omitted
    r.unknown_order = false;
    r.ref_msg_type = std::string(msg_type::OrderCancelRequest);

    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::MissingOrigClOrdId);
    CHECK(b.empty());
}

SCENARIO("E-15c", "P0", "a report with no ClOrdID is refused") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    ReportRequest r = request(c);
    r.cl_ord_id.clear();
    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::MissingClOrdId);
}

// ---------------------------------------------------------------------------
// The oracle catches corruption the builder did not produce
// ---------------------------------------------------------------------------

SCENARIO("PB-17", "P0", "the oracle rejects 150=1, the FIX 4.2 fill code") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, good), BuildError::Ok);

    // The mutation build from the playbook: ExecType 1 instead of F.
    const CanonicalBody mutated = with(good, tag::ExecType, "1");
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, mutated),
             BuildError::BadFieldValue);
    // And 2, the FIX 4.2 full-fill code.
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, with(good, tag::ExecType, "2")),
             BuildError::BadFieldValue);
}

SCENARIO("E-17", "P0", "the oracle rejects OrdStatus=Filled with a non-zero LeavesQty") {
    const OrderChain c = chain(100000, 100000, BaseState::Filled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 100000;
    r.last_px = 108500;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);

    // The second mutation from PB-17: 39=2 with leaves 60000.
    const CanonicalBody mutated = with(good, tag::LeavesQty, "60000");
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, mutated),
             BuildError::TerminalLeavesNonZero);
}

SCENARIO("E-18", "P1", "the oracle rejects a no-fill report carrying LastQty") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    CanonicalBody good;
    CHECK_EQ(build_report(request(c), &good), BuildError::Ok);

    CanonicalBody mutated = with(good, tag::LastQty, "40000");
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, mutated),
             BuildError::NonTradeWithLast);
    mutated = with(good, tag::LastPx, "1.08490");
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, mutated),
             BuildError::NonTradeWithLast);
}

SCENARIO("E-13", "P0", "the oracle rejects CumQty greater than OrderQty") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);

    CanonicalBody mutated = with(good, tag::CumQty, "140000");
    mutated = with(mutated, tag::LeavesQty, "0");
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, mutated),
             BuildError::CumExceedsOrderQty);
}

SCENARIO("E-17b", "P0", "the oracle rejects a live report whose LeavesQty does not add up") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);

    // 100000 - 40000 is 60000, not 50000.
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      with(good, tag::LeavesQty, "50000")),
             BuildError::LeavesMismatch);
}

SCENARIO("F-16", "P0", "the oracle rejects a cancel refusal dressed as an ExecutionReport") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    ReportRequest r = request(c);
    r.type = ReportType::CancelReject;
    r.cl_ord_id = "C1";
    r.orig_cl_ord_id = "O1";
    r.reject = BizReject::TooLate;
    r.ref_msg_type = std::string(msg_type::OrderCancelRequest);
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);
    CHECK_EQ(check_report_conformance(msg_type::OrderCancelReject, good), BuildError::Ok);

    // Adding an ExecType turns it into the wrong message type.
    CHECK_EQ(check_report_conformance(msg_type::OrderCancelReject,
                                      with(good, tag::ExecType, "8")),
             BuildError::WrongMessageType);
    // And dropping the required response-to tag is refused.
    CHECK_EQ(check_report_conformance(msg_type::OrderCancelReject,
                                      without(good, tag::CxlRejResponseTo)),
             BuildError::MissingCxlRejResponseTo);
    CHECK_EQ(check_report_conformance(msg_type::OrderCancelReject,
                                      without(good, tag::CxlRejReason)),
             BuildError::MissingRejectReason);
}

SCENARIO("E-15d", "P0", "the oracle rejects a report with a missing required field") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    CanonicalBody good;
    CHECK_EQ(build_report(request(c), &good), BuildError::Ok);

    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, without(good, tag::OrderID)),
             BuildError::MissingOrderId);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, without(good, tag::ClOrdID)),
             BuildError::MissingClOrdId);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, without(good, tag::ExecID)),
             BuildError::MissingExecId);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, without(good, tag::AvgPx)),
             BuildError::BadFieldValue);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, without(good, tag::CumQty)),
             BuildError::BadFieldValue);
}

SCENARIO("E-18b", "P1", "the oracle rejects an OrdStatus outside the FIX 4.4 set") {
    const OrderChain c = chain(100000, 0, BaseState::New);
    CanonicalBody good;
    CHECK_EQ(build_report(request(c), &good), BuildError::Ok);
    // 'Z' is not a FIX 4.4 OrdStatus, and 3 (Done for day) is outside this
    // profile's set.
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      with(good, tag::OrdStatus, "Z")),
             BuildError::BadFieldValue);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, with(good, tag::Side, "7")),
             BuildError::BadFieldValue);
}

SCENARIO("E-16b", "P0", "the oracle rejects a trade whose LastPx is zero on the wire") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);

    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      with(good, tag::LastPx, "0.00000")),
             BuildError::BadFieldValue);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      with(good, tag::LastQty, "0")),
             BuildError::LastQtyNotPositive);
}

SCENARIO("G-20r", "P0", "the oracle requires OrigClOrdID on a Replaced report") {
    OrderChain c = chain(150000, 40000, BaseState::PartiallyFilled);
    c.current_cl_ord_id = "R1";
    ReportRequest r = request(c);
    r.type = ReportType::ExecReplaced;
    r.cl_ord_id = "R1";
    r.orig_cl_ord_id = "O1";
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);

    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      without(good, tag::OrigClOrdID)),
             BuildError::MissingOrigClOrdId);
}

SCENARIO("B-06s", "P0", "the oracle requires OrdRejReason on a rejection") {
    ReportRequest r;
    r.type = ReportType::ExecRejected;
    r.price = kFx5;
    r.cl_ord_id = "O1";
    r.exec_id = "EX-000001";
    r.reject = BizReject::UnknownSymbol;
    CanonicalBody good;
    CHECK_EQ(build_report(r, &good), BuildError::Ok);
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport,
                                      without(good, tag::OrdRejReason)),
             BuildError::MissingRejectReason);
}

// ---------------------------------------------------------------------------
// Status precedence, exhaustively over the small state space
// ---------------------------------------------------------------------------

SCENARIO("H-01p", "P0", "status precedence is total over base state and pending request") {
    struct Row {
        BaseState base;
        Pending pending;
        const char* expect;
    };
    // Every combination, stated rather than derived, so a change to the
    // precedence rule has to be made deliberately.
    const Row rows[] = {
        {BaseState::New, Pending::None, "0"},
        {BaseState::New, Pending::Cancel, "6"},
        {BaseState::New, Pending::Replace, "E"},
        {BaseState::PartiallyFilled, Pending::None, "1"},
        {BaseState::PartiallyFilled, Pending::Cancel, "6"},
        {BaseState::PartiallyFilled, Pending::Replace, "E"},
        // A terminal state outranks any in-flight request.
        {BaseState::Filled, Pending::None, "2"},
        {BaseState::Filled, Pending::Cancel, "2"},
        {BaseState::Filled, Pending::Replace, "2"},
        {BaseState::Canceled, Pending::None, "4"},
        {BaseState::Canceled, Pending::Cancel, "4"},
        {BaseState::Expired, Pending::None, "C"},
        {BaseState::Rejected, Pending::None, "8"},
    };
    for (const Row& r : rows) {
        CHECK_EQ(std::string(ord_status_for(r.base, r.pending)), std::string(r.expect));
    }
}

SCENARIO("E-20", "P0", "LeavesQty is zero in every terminal state and derived otherwise") {
    // Invariant I-04 across the whole state space, including a cancel after a
    // partial fill where the usual arithmetic does not hold.
    const BaseState terminal[] = {BaseState::Filled, BaseState::Canceled, BaseState::Expired,
                                  BaseState::Rejected};
    for (BaseState s : terminal) {
        const OrderChain c = chain(100000, 40000, s);
        CHECK_EQ(c.expected_leaves(), static_cast<Units>(0));
    }
    const OrderChain live = chain(100000, 40000, BaseState::PartiallyFilled);
    CHECK_EQ(live.expected_leaves(), static_cast<Units>(60000));
    const OrderChain fresh = chain(100000, 0, BaseState::New);
    CHECK_EQ(fresh.expected_leaves(), static_cast<Units>(100000));
}

SCENARIO("M-11", "P0", "a canonical body round-trips through serialisation unchanged") {
    const OrderChain c = chain(100000, 40000, BaseState::PartiallyFilled);
    ReportRequest r = request(c);
    r.type = ReportType::ExecTrade;
    r.last_qty = 40000;
    r.last_px = 108490;
    CanonicalBody b;
    CHECK_EQ(build_report(r, &b), BuildError::Ok);

    // The store keeps the body as bytes and replays it verbatim, so a lossy
    // round trip would silently change a report across a restart.
    const std::string packed = serialize_body(b);
    CanonicalBody back;
    CHECK(deserialize_body(packed, &back));
    CHECK_EQ(back.size(), b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        CHECK_EQ(back[i].first, b[i].first);
        CHECK_EQ(back[i].second, b[i].second);
    }
    CHECK_EQ(check_report_conformance(msg_type::ExecutionReport, back), BuildError::Ok);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "reports"); }
