#include "llx/domain.hpp"

#include <set>
#include <sstream>

#include "llx/hash.hpp"

namespace llx {

std::string_view to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::None: return "NONE";
        case OrderStatus::PendingNew: return "PENDING_NEW";
        case OrderStatus::Working: return "WORKING";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::PendingCancel: return "PENDING_CANCEL";
        case OrderStatus::Canceled: return "CANCELED";
        case OrderStatus::PendingReplace: return "PENDING_REPLACE";
        case OrderStatus::Rejected: return "REJECTED";
        case OrderStatus::Expired: return "EXPIRED";
        case OrderStatus::Quarantined: return "QUARANTINED";
    }
    return "UNKNOWN";
}

bool is_terminal(OrderStatus s) {
    return s == OrderStatus::Filled || s == OrderStatus::Canceled ||
           s == OrderStatus::Rejected || s == OrderStatus::Expired;
}

bool is_live(OrderStatus s) {
    return s == OrderStatus::PendingNew || s == OrderStatus::Working ||
           s == OrderStatus::PartiallyFilled || s == OrderStatus::PendingCancel ||
           s == OrderStatus::PendingReplace;
}

std::string_view to_string(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "NONE";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
        case RejectReason::DuplicateOrder: return "DUPLICATE_ORDER";
        case RejectReason::DuplicateExec: return "DUPLICATE_EXEC";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::InvalidSymbol: return "INVALID_SYMBOL";
        case RejectReason::OrderTerminal: return "ORDER_TERMINAL";
        case RejectReason::RiskLimitExceeded: return "RISK_LIMIT_EXCEEDED";
        case RejectReason::RiskDomainUnknown: return "RISK_DOMAIN_UNKNOWN";
        case RejectReason::Contradiction: return "CONTRADICTION";
        case RejectReason::CausalOutOfOrder: return "CAUSAL_OUT_OF_ORDER";
        case RejectReason::NotLive: return "NOT_LIVE";
    }
    return "UNKNOWN";
}

namespace {

Proposal reject(RejectReason r) {
    Proposal p;
    p.accepted = false;
    p.reason = r;
    return p;
}

// A contradiction is not a rejection. The command was well formed and the
// order exists, but applying the event would break the ledger, which means
// this engine's view and the counterparty's view disagree. Doc 4 and LL-008
// require the order to be quarantined for human reconciliation rather than
// either absorbed or dropped.
Proposal contradict(const OrderState& current) {
    Proposal p;
    p.accepted = true;  // the effect is the quarantine, and it must be durable
    p.reason = RejectReason::Contradiction;
    p.quarantine = true;
    p.write_projection = true;
    p.order = current;
    p.order.status = OrderStatus::Quarantined;
    return p;
}

const OrderState* find_order(const BusinessState& s, OrderId id) {
    const auto it = s.orders.find(id);
    return it == s.orders.end() ? nullptr : &it->second;
}

const DomainRisk* find_risk(const BusinessState& s, DomainId d) {
    const auto it = s.risk.find(d);
    return it == s.risk.end() ? nullptr : &it->second;
}

// Admission against the conservative envelope. The candidate reservation is
// added to whichever side the order is on and the worse of the two directions
// is tested, so an approval is still correct if every outstanding order on that
// side fills and nothing on the other side does.
bool risk_admits(const DomainRisk& r, Side side, Qty add) {
    DomainRisk probe = r;
    if (side == Side::Buy) probe.reserved_buy += add;
    else probe.reserved_sell += add;
    return probe.within_limit();
}

Proposal reduce_new_order(const BusinessState& state, const InputEnvelope& ev) {
    if (ev.qty <= 0) return reject(RejectReason::InvalidQuantity);
    if (ev.px <= 0) return reject(RejectReason::InvalidPrice);
    if (ev.symbol == 0) return reject(RejectReason::InvalidSymbol);
    if (find_order(state, ev.order_id) != nullptr) return reject(RejectReason::DuplicateOrder);

    const DomainRisk* r = find_risk(state, ev.risk_domain);
    if (r == nullptr) return reject(RejectReason::RiskDomainUnknown);
    if (!risk_admits(*r, ev.side, ev.qty)) return reject(RejectReason::RiskLimitExceeded);

    Proposal p;
    p.accepted = true;
    p.write_projection = true;
    p.order.order_id = ev.order_id;
    p.order.symbol = ev.symbol;
    p.order.side = ev.side;
    p.order.qty = ev.qty;
    p.order.px = ev.px;
    p.order.status = OrderStatus::PendingNew;
    p.order.cum_qty = 0;
    p.order.leaves_qty = ev.qty;
    p.order.version = 1;
    p.order.last_order_seq = ev.order_seq;

    p.write_risk = true;
    p.risk_domain = ev.risk_domain;
    if (ev.side == Side::Buy) p.reserved_buy_delta = ev.qty;
    else p.reserved_sell_delta = ev.qty;

    // The original 35=D. It becomes eligible to send only when this
    // transaction commits (LI-16).
    p.outbox_kind = 1;
    p.outbox_cl_ord_id = ev.order_id;
    return p;
}

Proposal reduce_cancel_request(const BusinessState& state, const InputEnvelope& ev) {
    const OrderState* cur = find_order(state, ev.orig_order_id);
    // LL-014: a cancel with no admitted New has no lineage, and marking it high
    // priority does not create one.
    if (cur == nullptr) return reject(RejectReason::UnknownOrder);
    if (is_terminal(cur->status)) return reject(RejectReason::OrderTerminal);
    if (cur->status == OrderStatus::Quarantined) return reject(RejectReason::NotLive);
    if (!is_live(cur->status)) return reject(RejectReason::NotLive);

    Proposal p;
    p.accepted = true;
    p.write_projection = true;
    p.order = *cur;
    p.order.status = OrderStatus::PendingCancel;
    p.order.last_order_seq = ev.order_seq;
    // No risk change yet. The reservation is released when the LP confirms the
    // cancel, not when it is requested: releasing on request would free budget
    // an order that is still live at the LP could consume (LI-18).
    p.outbox_kind = 2;
    p.outbox_cl_ord_id = ev.order_id;
    p.outbox_orig_cl_ord_id = ev.orig_order_id;
    return p;
}

Proposal reduce_replace_request(const BusinessState& state, const InputEnvelope& ev) {
    const OrderState* cur = find_order(state, ev.orig_order_id);
    if (cur == nullptr) return reject(RejectReason::UnknownOrder);
    if (is_terminal(cur->status)) return reject(RejectReason::OrderTerminal);
    if (cur->status == OrderStatus::Quarantined) return reject(RejectReason::NotLive);
    if (!is_live(cur->status)) return reject(RejectReason::NotLive);
    if (ev.qty <= 0) return reject(RejectReason::InvalidQuantity);
    if (ev.px <= 0) return reject(RejectReason::InvalidPrice);
    // A replace cannot take the order below what is already filled: the fills
    // happened and cannot be unwound.
    if (ev.qty < cur->cum_qty) return reject(RejectReason::InvalidQuantity);

    Proposal p;
    p.accepted = true;
    p.write_projection = true;
    p.order = *cur;
    p.order.status = OrderStatus::PendingReplace;
    p.order.pending_qty = ev.qty;
    p.order.pending_px = ev.px;
    p.order.last_order_seq = ev.order_seq;

    // An increase needs budget now, because the larger order could fill as soon
    // as the LP accepts it. A decrease releases nothing yet: until the LP
    // confirms, the original larger quantity is still live there, and releasing
    // early is exactly the double-spend LI-18 and doc 14 of the P05 design
    // forbid.
    const Qty delta = ev.qty - cur->qty;
    if (delta > 0) {
        const DomainRisk* r = find_risk(state, ev.risk_domain);
        if (r == nullptr) return reject(RejectReason::RiskDomainUnknown);
        if (!risk_admits(*r, cur->side, delta)) return reject(RejectReason::RiskLimitExceeded);
        p.write_risk = true;
        p.risk_domain = ev.risk_domain;
        if (cur->side == Side::Buy) p.reserved_buy_delta = delta;
        else p.reserved_sell_delta = delta;
    }

    p.outbox_kind = 3;
    p.outbox_cl_ord_id = ev.order_id;
    p.outbox_orig_cl_ord_id = ev.orig_order_id;
    return p;
}

Proposal reduce_execution_report(const BusinessState& state, const InputEnvelope& ev,
                                 bool exec_already_seen) {
    const OrderState* cur = find_order(state, ev.order_id);
    if (cur == nullptr) return reject(RejectReason::UnknownOrder);
    if (cur->status == OrderStatus::Quarantined) return reject(RejectReason::NotLive);

    // Exactly one effect per ExecID, whatever the session layer did with
    // PossDup (doc 19.3). A duplicate is a recognised no-effect, not a reject:
    // the event was legitimate and has already been applied once.
    if (exec_already_seen && ev.exec_id != 0) {
        Proposal p;
        p.accepted = true;
        p.no_effect = true;
        p.reason = RejectReason::DuplicateExec;
        return p;
    }

    switch (ev.exec_type) {
        case ExecType::New: {
            if (cur->status != OrderStatus::PendingNew) {
                // An acknowledgement for an order that is not pending is a
                // disagreement about state, not a no-op.
                if (cur->status == OrderStatus::Working) {
                    Proposal p;
                    p.accepted = true;
                    p.no_effect = true;
                    return p;
                }
                return contradict(*cur);
            }
            Proposal p;
            p.accepted = true;
            p.write_projection = true;
            p.order = *cur;
            p.order.status = OrderStatus::Working;
            p.order.last_order_seq = ev.order_seq;
            return p;
        }

        case ExecType::PartialFill:
        case ExecType::Fill: {
            if (is_terminal(cur->status)) return contradict(*cur);
            if (ev.last_qty <= 0) return contradict(*cur);
            Qty new_cum = 0;
            if (!checked_add(cur->cum_qty, ev.last_qty, new_cum)) return contradict(*cur);
            // The effective quantity is the current one; a pending replace has
            // not been confirmed, so the LP is still working the old size.
            if (new_cum > cur->qty) return contradict(*cur);
            if (ev.exec_type == ExecType::Fill && new_cum != cur->qty) return contradict(*cur);

            Proposal p;
            p.accepted = true;
            p.write_projection = true;
            p.order = *cur;
            p.order.cum_qty = new_cum;
            p.order.leaves_qty = cur->qty - new_cum;
            p.order.notional = cur->notional + static_cast<std::uint64_t>(ev.last_qty) *
                                                   static_cast<std::uint64_t>(ev.last_px);
            // Average price in the price scale, computed from the integer
            // notional so the result is exact and reproducible.
            p.order.avg_px = new_cum == 0
                                 ? 0
                                 : static_cast<Px>(p.order.notional /
                                                   static_cast<std::uint64_t>(new_cum));
            p.order.status = (p.order.leaves_qty == 0) ? OrderStatus::Filled
                                                       : OrderStatus::PartiallyFilled;
            // A fill while a cancel or replace is outstanding keeps that
            // pending marker, because the request is still outstanding at the
            // LP (doc 19.4).
            if (p.order.leaves_qty != 0 && (cur->status == OrderStatus::PendingCancel ||
                                            cur->status == OrderStatus::PendingReplace))
                p.order.status = cur->status;
            p.order.last_order_seq = ev.order_seq;

            p.append_fill = true;
            p.fill.exec_id = ev.exec_id;
            p.fill.order_id = ev.order_id;
            p.fill.last_qty = ev.last_qty;
            p.fill.last_px = ev.last_px;

            // The conservation law: a BUY fill of f moves f out of reserved_buy
            // and into the position; a SELL fill moves f out of reserved_sell
            // and out of the position.
            p.write_risk = true;
            p.risk_domain = ev.risk_domain;
            if (cur->side == Side::Buy) {
                p.reserved_buy_delta = -ev.last_qty;
                p.position_delta = ev.last_qty;
            } else {
                p.reserved_sell_delta = -ev.last_qty;
                p.position_delta = -ev.last_qty;
            }
            return p;
        }

        case ExecType::Canceled: {
            if (is_terminal(cur->status)) return contradict(*cur);
            Proposal p;
            p.accepted = true;
            p.write_projection = true;
            p.order = *cur;
            p.order.status = OrderStatus::Canceled;
            p.order.leaves_qty = 0;
            p.order.last_order_seq = ev.order_seq;
            // Now, and only now, the unfilled remainder is released.
            p.write_risk = true;
            p.risk_domain = ev.risk_domain;
            if (cur->side == Side::Buy) p.reserved_buy_delta = -cur->leaves_qty;
            else p.reserved_sell_delta = -cur->leaves_qty;
            return p;
        }

        case ExecType::Replaced: {
            if (cur->status != OrderStatus::PendingReplace) return contradict(*cur);
            const Qty new_qty = cur->pending_qty;
            if (new_qty < cur->cum_qty) return contradict(*cur);
            Proposal p;
            p.accepted = true;
            p.write_projection = true;
            p.order = *cur;
            p.order.qty = new_qty;
            p.order.px = cur->pending_px;
            p.order.pending_qty = 0;
            p.order.pending_px = 0;
            p.order.leaves_qty = new_qty - cur->cum_qty;
            p.order.version = cur->version + 1;
            p.order.status = (p.order.leaves_qty == 0)
                                 ? OrderStatus::Filled
                                 : (cur->cum_qty > 0 ? OrderStatus::PartiallyFilled
                                                     : OrderStatus::Working);
            p.order.last_order_seq = ev.order_seq;
            // A decrease releases its delta only here, against the LP's
            // confirmation.
            const Qty delta = new_qty - cur->qty;
            if (delta < 0) {
                p.write_risk = true;
                p.risk_domain = ev.risk_domain;
                if (cur->side == Side::Buy) p.reserved_buy_delta = delta;
                else p.reserved_sell_delta = delta;
            }
            return p;
        }

        case ExecType::Rejected:
        case ExecType::Expired: {
            if (is_terminal(cur->status)) return contradict(*cur);
            Proposal p;
            p.accepted = true;
            p.write_projection = true;
            p.order = *cur;
            p.order.status = (ev.exec_type == ExecType::Rejected) ? OrderStatus::Rejected
                                                                  : OrderStatus::Expired;
            p.order.leaves_qty = 0;
            p.order.last_order_seq = ev.order_seq;
            p.write_risk = true;
            p.risk_domain = ev.risk_domain;
            if (cur->side == Side::Buy) p.reserved_buy_delta = -cur->leaves_qty;
            else p.reserved_sell_delta = -cur->leaves_qty;
            return p;
        }

        case ExecType::None:
        default:
            return reject(RejectReason::Contradiction);
    }
}

}  // namespace

Proposal reduce(const BusinessState& state, const InputEnvelope& ev,
                bool exec_already_seen) {
    switch (ev.type) {
        case EventType::NewOrder: return reduce_new_order(state, ev);
        case EventType::CancelRequest: return reduce_cancel_request(state, ev);
        case EventType::ReplaceRequest: return reduce_replace_request(state, ev);
        case EventType::ExecutionReport:
            return reduce_execution_report(state, ev, exec_already_seen);
        case EventType::SessionControl:
        case EventType::Tick: {
            // Carries no business effect by design: session liveness is P01's
            // and must not be able to change order or risk state here.
            Proposal p;
            p.accepted = true;
            p.no_effect = true;
            return p;
        }
        default: return reject(RejectReason::Contradiction);
    }
}

void apply(BusinessState& state, const Proposal& p, std::uint64_t journal_version) {
    if (!p.accepted || p.no_effect) return;
    if (p.write_projection) state.orders[p.order.order_id] = p.order;
    if (p.append_fill) {
        FillRecord f = p.fill;
        f.journal_version = journal_version;
        state.fills.push_back(f);
    }
    if (p.write_risk) {
        DomainRisk& r = state.risk[p.risk_domain];
        r.domain = p.risk_domain;
        r.reserved_buy += p.reserved_buy_delta;
        r.reserved_sell += p.reserved_sell_delta;
        r.position += p.position_delta;
    }
}

DurableBusinessIntent to_intent(const Proposal& p, const InputEnvelope& ev) {
    DurableBusinessIntent in;
    in.event_id = ev.event_id;
    in.domain_seq = ev.domain_seq;
    in.risk_domain = ev.risk_domain;
    in.owner_epoch = ev.owner_epoch;
    in.order_id = p.write_projection ? p.order.order_id : ev.order_id;
    in.source_type = ev.type;
    // Derived from the business identity, never from a pointer or a ring index
    // (LI-12). This is the key a COMMIT_UNKNOWN is resolved by (CW-17).
    in.idempotency_key = fnv1a_mix(fnv1a_mix(0x9e3779b97f4a7c15ULL, ev.event_id.hi),
                                   ev.event_id.lo);

    in.write_inbox = (ev.type == EventType::ExecutionReport);
    in.write_journal = true;
    in.write_projection = p.write_projection;
    in.write_fill = p.append_fill;
    in.write_risk = p.write_risk;
    in.write_outbox = (p.outbox_kind != 0);

    if (p.write_projection) {
        in.projection_order = p.order.order_id;
        in.projection_status = static_cast<std::uint8_t>(p.order.status);
        in.projection_cum_qty = p.order.cum_qty;
        in.projection_leaves_qty = p.order.leaves_qty;
        in.projection_avg_px = p.order.avg_px;
    }
    if (p.append_fill) {
        in.fill_exec_id = p.fill.exec_id;
        in.fill_last_qty = p.fill.last_qty;
        in.fill_last_px = p.fill.last_px;
    }
    if (p.write_risk) {
        in.risk_domain_key = p.risk_domain;
        // Carried as the pair of side deltas the store applies; packed into the
        // two fields the durable record defines.
        in.risk_reserved_delta = p.reserved_buy_delta + p.reserved_sell_delta;
        in.risk_position_delta = p.position_delta;
    }
    in.outbox_kind = p.outbox_kind;
    in.outbox_cl_ord_id = p.outbox_cl_ord_id;
    in.outbox_orig_cl_ord_id = p.outbox_orig_cl_ord_id;
    in.outbox_symbol = ev.symbol;
    in.outbox_side = ev.side;
    in.outbox_qty = p.write_projection ? p.order.qty : ev.qty;
    in.outbox_px = p.write_projection ? p.order.px : ev.px;
    return in;
}

// --- state hash and invariant check ---------------------------------------

std::string BusinessState::state_hash() const {
    // Canonical and order-independent by construction: the containers are
    // ordered, and every field is length-prefixed so two different field
    // layouts cannot hash alike.
    Sha256 h;
    h.field("orders");
    h.field_num(static_cast<std::uint64_t>(orders.size()));
    for (const auto& [id, o] : orders) {
        h.field_num(id);
        h.field_num(o.symbol);
        h.field_num(o.side);
        h.field_num(o.qty);
        h.field_num(o.px);
        h.field_num(o.status);
        h.field_num(o.cum_qty);
        h.field_num(o.leaves_qty);
        h.field_num(o.avg_px);
        h.field_num(o.notional);
        h.field_num(o.version);
        h.field_num(o.pending_qty);
        h.field_num(o.pending_px);
    }
    h.field("fills");
    h.field_num(static_cast<std::uint64_t>(fills.size()));
    for (const FillRecord& f : fills) {
        h.field_num(f.exec_id);
        h.field_num(f.order_id);
        h.field_num(f.last_qty);
        h.field_num(f.last_px);
    }
    h.field("risk");
    h.field_num(static_cast<std::uint64_t>(risk.size()));
    for (const auto& [d, r] : risk) {
        h.field_num(d);
        h.field_num(r.limit_qty);
        h.field_num(r.reserved_buy);
        h.field_num(r.reserved_sell);
        h.field_num(r.position);
    }
    return h.hex();
}

BusinessState::Check BusinessState::check() const {
    Check c;
    std::ostringstream detail;

    // Per-order ledger: cum + leaves must equal the effective quantity for any
    // order that is still live, and the fills must sum to cum_qty.
    std::map<OrderId, Qty> summed;
    std::set<ExecId> seen;
    for (const FillRecord& f : fills) {
        if (f.exec_id != 0 && !seen.insert(f.exec_id).second) {
            c.no_duplicate_exec = false;
            detail << "duplicate exec_id " << f.exec_id << "; ";
        }
        summed[f.order_id] += f.last_qty;
    }
    for (const auto& [id, o] : orders) {
        if (o.status == OrderStatus::Quarantined) continue;
        if (is_live(o.status) && o.cum_qty + o.leaves_qty != o.qty) {
            c.ledger_consistent = false;
            detail << "order " << id << ": cum " << o.cum_qty << " + leaves " << o.leaves_qty
                   << " != qty " << o.qty << "; ";
        }
        if (o.cum_qty < 0 || o.leaves_qty < 0) {
            c.ledger_consistent = false;
            detail << "order " << id << ": negative ledger quantity; ";
        }
        const auto it = summed.find(id);
        const Qty from_fills = (it == summed.end()) ? 0 : it->second;
        if (from_fills != o.cum_qty) {
            c.fills_match_ledger = false;
            detail << "order " << id << ": fills sum " << from_fills << " != cum_qty "
                   << o.cum_qty << "; ";
        }
    }
    for (const auto& [d, r] : risk) {
        if (!r.non_negative()) {
            c.risk_non_negative = false;
            detail << "domain " << d << ": negative reservation (buy " << r.reserved_buy
                   << ", sell " << r.reserved_sell << "); ";
        }
        if (!r.within_limit()) {
            c.risk_within_limit = false;
            detail << "domain " << d << ": envelope " << r.envelope() << " exceeds limit "
                   << r.limit_qty << "; ";
        }
    }
    c.detail = detail.str();
    return c;
}

}  // namespace llx
