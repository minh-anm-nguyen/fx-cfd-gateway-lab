#include "order_oracle.hpp"

#include <string>

#include "router/fix44.hpp"
#include "router/numeric.hpp"

namespace gw::oracle {
namespace {

std::string ticks_text(std::int64_t ticks, int scale) {
    return router::format_decimal_ticks(ticks, scale);
}

// Quantities are counts of base units, so they render at scale 0. Passing the
// price scale here would multiply every quantity by 10^5, which is the kind of
// mistake that produces a plausible-looking number.
std::string units_text(std::int64_t units) { return router::format_decimal_ticks(units, 0); }

std::string status_text(const std::optional<router::OrdStatus>& s) {
    if (!s.has_value()) return "(no FIX status)";
    return std::string(router::to_string(*s));
}

void note(std::vector<ReducerDisagreement>& out, std::size_t step, std::string_view quantity,
          std::string router_said, std::string order_core_said) {
    out.push_back(ReducerDisagreement{step, quantity, std::move(router_said),
                                      std::move(order_core_said)});
}

}  // namespace

std::optional<router::OrdStatus> lifecycle_to_ord_status(order_core::Lifecycle l) {
    switch (l) {
        case order_core::Lifecycle::Working:
            return router::OrdStatus::New;
        case order_core::Lifecycle::PartFilled:
            return router::OrdStatus::PartiallyFilled;
        case order_core::Lifecycle::PendingCancel:
            return router::OrdStatus::PendingCancel;
        case order_core::Lifecycle::PendingReplace:
            return router::OrdStatus::PendingReplace;
        case order_core::Lifecycle::Filled:
            return router::OrdStatus::Filled;
        case order_core::Lifecycle::Canceled:
            return router::OrdStatus::Canceled;
        case order_core::Lifecycle::Expired:
            return router::OrdStatus::Expired;
        case order_core::Lifecycle::Rejected:
            return router::OrdStatus::Rejected;
        case order_core::Lifecycle::DoneForDay:
            return router::OrdStatus::DoneForDay;

        // No tag 39 value exists for these, and inventing one would be a lie
        // about the order rather than about the gateway. LocalPreparing and
        // NewSentUnknown are "we have not been told"; ReconciliationRequired
        // and Quarantined are "we have been told two incompatible things".
        // Reporting any of them as New is what EI-19 rules out.
        case order_core::Lifecycle::LocalPreparing:
        case order_core::Lifecycle::NewSentUnknown:
        case order_core::Lifecycle::ReconciliationRequired:
        case order_core::Lifecycle::Quarantined:
            return std::nullopt;
    }
    return std::nullopt;
}

order_core::PeerReport to_peer_report(const router::NormalizedReport& r, int price_digits,
                                      const std::string& instrument) {
    order_core::PeerReport out;

    // '9' is an OrderCancelReject, which carries no execution and no quantity.
    out.msg_type = r.kind == router::ReportKind::CancelReject ? '9' : '8';

    // P04's own dictionary does the enum-to-tag mapping. Writing a second table
    // here would make the oracle agree with its own idea of FIX rather than
    // with the one the router actually encodes.
    out.exec_type = std::string(router::fix::exec_type_to_wire(r.exec_type));
    out.ord_status = std::string(router::fix::ord_status_to_wire(r.ord_status));
    out.side_raw = std::string(router::fix::side_to_wire(r.side));

    out.exec_id = r.exec_id.has_value() ? r.exec_id->str() : std::string{};
    out.order_id = r.order_id.has_value() ? r.order_id->str() : std::string{};
    out.cl_ord_id = r.cl_ord_id.str();
    out.orig_cl_ord_id = r.orig_cl_ord_id.has_value() ? r.orig_cl_ord_id->str() : std::string{};
    out.exec_ref_id = r.exec_ref_id.has_value() ? r.exec_ref_id->str() : std::string{};

    out.account = r.account.str();
    out.symbol = instrument;

    // An absent optional stays absent. P03's header is explicit that a missing
    // tag is not the same as a tag present with value zero, and collapsing the
    // two would hide exactly the reports those cases are about.
    if (r.last_qty.has_value()) out.last_qty = units_text(*r.last_qty);
    if (r.last_px.has_value()) out.last_px = ticks_text(*r.last_px, price_digits);
    if (r.order_qty.has_value()) out.order_qty = units_text(*r.order_qty);

    // These three are not optional in P04: the normalizer resolves an absent
    // tag to zero before the report gets this far. So they are always rendered
    // as present, and the absent-tag cases P03 distinguishes cannot be reached
    // through this path -- they would have to be driven at P03 directly. Worth
    // knowing when reading a run: the oracle covers the arithmetic, not the
    // missing-tag semantics.
    out.cum_qty = units_text(r.cum_qty);
    out.leaves_qty = units_text(r.leaves_qty);
    out.avg_px = ticks_text(r.avg_px, price_digits);

    if (r.cxl_rej_response_to.has_value()) {
        out.cxl_rej_response_to =
            std::string(router::fix::cxl_rej_response_to_wire(*r.cxl_rej_response_to));
    }
    if (r.cxl_rej_reason >= 0) out.cxl_rej_reason = std::to_string(r.cxl_rej_reason);

    out.peer_id = r.lp.str();
    out.session_id = r.session_key;
    out.session_epoch = r.epoch;
    out.source_seq = r.source_receipt;
    out.has_source_cursor = r.source_receipt != 0;
    return out;
}

Projection project(const router::p03::ChainState& s) {
    Projection p;
    p.terminal = s.terminal();
    p.cum_qty = s.cum_qty;
    p.leaves_qty = s.leaves_qty;
    p.avg_px_ticks = s.avg_px();
    p.status = s.status;
    return p;
}

Projection project(const order_core::OrderAggregate& a) {
    Projection p;
    p.terminal = a.terminal();
    p.cum_qty = a.cum_qty_units;
    p.leaves_qty = a.leaves_qty_units();
    std::int64_t avg = 0;
    // False when there are no fills, in which case zero is the answer both
    // sides give.
    p.avg_px_ticks = a.avg_px_ticks_rounded(avg) ? avg : 0;
    p.status = lifecycle_to_ord_status(a.lifecycle());
    return p;
}

std::vector<ReducerDisagreement> compare(const Projection& router_side,
                                         const Projection& order_core_side, std::size_t step) {
    std::vector<ReducerDisagreement> out;

    if (router_side.terminal != order_core_side.terminal) {
        note(out, step, "terminal", router_side.terminal ? "true" : "false",
             order_core_side.terminal ? "true" : "false");
    }
    if (router_side.cum_qty != order_core_side.cum_qty) {
        note(out, step, "cum_qty", units_text(router_side.cum_qty),
             units_text(order_core_side.cum_qty));
    }
    if (router_side.leaves_qty != order_core_side.leaves_qty) {
        note(out, step, "leaves_qty", units_text(router_side.leaves_qty),
             units_text(order_core_side.leaves_qty));
    }
    if (router_side.avg_px_ticks != order_core_side.avg_px_ticks) {
        note(out, step, "avg_px", std::to_string(router_side.avg_px_ticks),
             std::to_string(order_core_side.avg_px_ticks));
    }

    // Only when both have a FIX status to give. P03 holding a state P04 cannot
    // name is not a disagreement: see lifecycle_to_ord_status().
    if (router_side.status.has_value() && order_core_side.status.has_value() &&
        *router_side.status != *order_core_side.status) {
        note(out, step, "status", status_text(router_side.status),
             status_text(order_core_side.status));
    }

    return out;
}

}  // namespace gw::oracle
