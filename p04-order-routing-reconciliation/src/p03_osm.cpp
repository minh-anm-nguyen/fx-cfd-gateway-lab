#include "deps/p03_osm.hpp"

#include <algorithm>

#include "router/numeric.hpp"

namespace router::p03 {

std::string_view to_string(ReduceOutcome o) {
    switch (o) {
        case ReduceOutcome::Applied: return "Applied";
        case ReduceOutcome::Evidence: return "Evidence";
        case ReduceOutcome::Noop: return "Noop";
        case ReduceOutcome::Hold: return "Hold";
        case ReduceOutcome::Quarantine: return "Quarantine";
    }
    return "Unknown";
}

bool ChainState::terminal() const {
    return status == OrdStatus::Filled || status == OrdStatus::Canceled ||
           status == OrdStatus::Rejected || status == OrdStatus::Expired ||
           status == OrdStatus::DoneForDay;
}

std::int64_t ChainState::avg_px() const {
    if (cum_qty == 0) return 0;
    return px_qty_weighted / cum_qty;
}

bool ChainState::has_fill(const ExecutionId& id) const {
    return std::any_of(fills.begin(), fills.end(),
                       [&id](const Fill& f) { return f.exec_id == id; });
}

Hash ChainState::state_hash() const {
    Sha256 s;
    s.field(chain_id.str());
    s.field(account.str());
    s.field(wire_symbol);
    s.update(static_cast<std::uint64_t>(side == Side::Buy ? 1 : 2));
    s.field(accepted_cl_ord_id.str());
    s.update(static_cast<std::uint64_t>(accepted_qty));
    s.update(static_cast<std::uint64_t>(accepted_price));
    s.field(pending_cancel ? pending_cancel->str() : "");
    s.field(pending_replace ? pending_replace->str() : "");
    s.field(to_string(status));
    s.update(static_cast<std::uint64_t>(cum_qty));
    s.update(static_cast<std::uint64_t>(leaves_qty));
    s.update(static_cast<std::uint64_t>(px_qty_weighted));
    s.field(lp_order_id ? lp_order_id->str() : "");
    s.update(static_cast<std::uint64_t>(cancel_proven ? 1 : 0));
    s.update(static_cast<std::uint64_t>(fills.size()));
    for (const Fill& f : fills) {
        s.field(f.exec_id.str());
        s.update(static_cast<std::uint64_t>(f.qty));
        s.update(static_cast<std::uint64_t>(f.px));
    }
    s.field(hold ? to_string(*hold) : "");
    return s.finish();
}

ChainState OrderStateMachine::create(const OrderChainId& chain, const TenantId& tenant,
                                     const AccountId& account, const LpId& lp,
                                     const std::string& wire_symbol, Side side, int price_scale,
                                     const ClientOrderId& cl_ord_id, std::int64_t qty,
                                     std::int64_t price) {
    ChainState s;
    s.chain_id = chain;
    s.tenant = tenant;
    s.account = account;
    s.lp = lp;
    s.wire_symbol = wire_symbol;
    s.side = side;
    s.price_scale = price_scale;
    s.accepted_cl_ord_id = cl_ord_id;
    s.accepted_qty = qty;
    s.accepted_price = price;
    s.status = OrdStatus::PendingNew;  // locally created, the LP has said nothing
    s.status_before_pending = OrdStatus::PendingNew;
    s.leaves_qty = qty;
    s.version = 1;
    return s;
}

void OrderStateMachine::note_cancel_requested(ChainState& state, const ClientOrderId& cancel_id) {
    state.pending_cancel = cancel_id;
    ++state.version;
}

void OrderStateMachine::note_replace_requested(ChainState& state, const ClientOrderId& replace_id,
                                               std::int64_t qty, std::int64_t price) {
    state.pending_replace = replace_id;
    state.pending_replace_qty = qty;
    state.pending_replace_price = price;
    ++state.version;
}

namespace {

ReduceResult hold(HoldReason reason, std::string note) {
    ReduceResult r;
    r.outcome = ReduceOutcome::Hold;
    r.reason = reason;
    r.note = std::move(note);
    return r;
}

ReduceResult quarantine(HoldReason reason, std::string note) {
    ReduceResult r;
    r.outcome = ReduceOutcome::Quarantine;
    r.reason = reason;
    r.note = std::move(note);
    return r;
}

ReduceResult applied(std::string note = {}) {
    ReduceResult r;
    r.outcome = ReduceOutcome::Applied;
    r.note = std::move(note);
    return r;
}

ReduceResult noop() {
    ReduceResult r;
    r.outcome = ReduceOutcome::Noop;
    return r;
}

ReduceResult evidence(std::string note = {}) {
    ReduceResult r;
    r.outcome = ReduceOutcome::Evidence;
    r.note = std::move(note);
    return r;
}

// The three ExecTypes v1 recognises only in order to refuse them. Duplicated
// here rather than taken from fix44.hpp on purpose: P03 must not depend on the
// wire dictionary, so that an independent oracle can share this reducer's rules
// without sharing the parser (design doc 22.1, critical independence).
bool unsupported_in_v1(ExecType t) {
    return t == ExecType::Restated || t == ExecType::TradeCorrect || t == ExecType::TradeCancel;
}

// A report whose instrument or direction does not match the chain is not a
// surprising fill, it is a contradiction. OR-G-11, OR-G-12, OR-J-16, OR-J-17.
bool instrument_matches(const ChainState& s, const NormalizedReport& r) {
    return r.wire_symbol == s.wire_symbol && r.side == s.side;
}

// Terminal statuses leave the order alone; a Pending status does not overwrite
// a terminal one either. This is the only place status precedence lives.
void set_status(ChainState& s, OrdStatus next) {
    if (next == OrdStatus::PendingCancel || next == OrdStatus::PendingReplace) {
        if (!s.terminal()) {
            s.status_before_pending = s.status;
            s.status = next;
        }
        return;
    }
    s.status = next;
}

}  // namespace

ReduceResult OrderStateMachine::apply(ChainState& s, const NormalizedReport& r) {
    // A chain already on hold does not absorb more events silently; the router
    // keeps the raw evidence and a human decides (R-I24).
    if (s.hold.has_value() && r.exec_type != ExecType::OrderStatus)
        return hold(*s.hold, "chain already held");

    // v1 does not model trade corrections, trade busts or restatements. Doing
    // the arithmetic ourselves is exactly the "self-designed correction math"
    // design doc 11.2 forbids (OR-J-12, OR-J-13, OR-J-14, PB-24).
    if (r.kind == ReportKind::ExecutionReport && unsupported_in_v1(r.exec_type))
        return quarantine(HoldReason::UnsupportedEvent,
                          std::string("ExecType ") + std::string(to_string(r.exec_type)) +
                              " not supported in v1");

    if (r.kind == ReportKind::CancelReject) {
        if (!r.cxl_rej_response_to)
            return quarantine(HoldReason::SchemaViolation, "cancel reject without 434");
        ReduceResult out = applied("cancel/replace request rejected");
        // The peer answered about this chain, so the projection is no longer
        // local only. It is still not an acknowledgement of the order itself.
        s.local_only = false;
        if (*r.cxl_rej_response_to == CxlRejResponseTo::OrderCancelRequest) {
            // OR-H-03, OR-H-04: the *request* is finished, the order is not.
            // R-I11 forbids reading this as a cancellation.
            if (!s.pending_cancel)
                return hold(HoldReason::IdentityConflict,
                            "cancel reject with no pending cancel request");
            out.effects.push_back(Effect{EffectKind::CloseCancelRequest,
                                         s.pending_cancel->str(), HoldReason::None});
            s.pending_cancel.reset();
        } else {
            // OR-I-07: a rejected replace never becomes the accepted version.
            if (!s.pending_replace)
                return hold(HoldReason::IdentityConflict,
                            "replace reject with no pending replace request");
            out.effects.push_back(Effect{EffectKind::CloseReplaceRequest,
                                         s.pending_replace->str(), HoldReason::None});
            s.pending_replace.reset();
            s.pending_replace_qty = 0;
            s.pending_replace_price = 0;
        }
        if (s.status == OrdStatus::PendingCancel || s.status == OrdStatus::PendingReplace)
            s.status = s.status_before_pending;
        ++s.version;
        return out;
    }

    if (!instrument_matches(s, r))
        return hold(HoldReason::SemanticConflict,
                    "report instrument/side does not match the chain");

    // Binding the LP OrderID. Once bound it is immutable for the chain under the
    // pinned ROE; a different value later is an identity conflict, never a
    // silent remap (OR-C-10, OR-L-16).
    if (r.order_id && !r.order_id->str().empty() && r.order_id->str() != "NONE") {
        if (!s.lp_order_id)
            s.lp_order_id = r.order_id;
        else if (*s.lp_order_id != *r.order_id)
            return hold(HoldReason::IdentityConflict, "OrderID changed for this chain");
    }

    switch (r.exec_type) {
        case ExecType::New: {
            s.lp_acked = true;
            s.local_only = false;
            // A New arriving after a fill or a terminal state is late, not a
            // regression. OR-G-07 and OR-L-03 both depend on this.
            if (s.cum_qty > 0 || s.terminal()) {
                ++s.version;
                return evidence("late NewAck after progress, no regression");
            }
            set_status(s, OrdStatus::New);
            if (r.order_qty) s.accepted_qty = *r.order_qty;
            s.leaves_qty = s.accepted_qty - s.cum_qty;
            ++s.version;
            return applied("order acknowledged");
        }

        case ExecType::PendingNew: {
            s.lp_acked = true;
            s.local_only = false;
            ++s.version;
            return evidence("pending new");
        }

        case ExecType::Trade: {
            // A fill after the LP already reported the order finished
            // contradicts that report. Applying it would leave a cancelled
            // order with a remainder, or overfill a filled one, so it is held
            // with the evidence kept (R-I10, R-I11).
            if (s.terminal())
                return hold(HoldReason::SemanticConflict,
                            "fill arrived after the order was already terminal");
            if (!r.last_qty || !r.last_px)
                return quarantine(HoldReason::SchemaViolation, "trade without LastQty/LastPx");
            if (!r.exec_id || r.exec_id->str().empty())
                return quarantine(HoldReason::SchemaViolation, "trade without ExecID");
            if (*r.last_qty <= 0)
                return quarantine(HoldReason::SchemaViolation, "trade with non-positive LastQty");
            // A zero or negative execution price is not a price. R-I09 forbids a
            // fill that could only have been invented, and a 0 LastPx would put
            // one in the ledger.
            if (*r.last_px <= 0)
                return quarantine(HoldReason::SchemaViolation, "trade with non-positive LastPx");

            // Dedup by execution identity is the router's job, but the ledger
            // double-checks: a second fill with a known ExecID is a no-op here
            // too, so a replay can never add quantity (R-I07).
            if (s.has_fill(*r.exec_id)) return noop();

            std::int64_t new_cum = 0;
            if (!checked_add(s.cum_qty, *r.last_qty, new_cum))
                return hold(HoldReason::SemanticConflict, "CumQty overflow");

            // The peer's own CumQty has to agree with ours. OR-G-13 sends
            // LastQty=60000 with CumQty=10000 and expects a hold, not a guess.
            if (r.cum_qty != 0 && r.cum_qty != new_cum)
                return hold(HoldReason::SemanticConflict,
                            "report CumQty disagrees with ledger plus LastQty");

            // Overfill. R-I10: the router does not clamp and does not rewrite
            // the ledger; it stops and escalates.
            if (new_cum > s.accepted_qty && !s.pending_replace.has_value())
                return hold(HoldReason::SemanticConflict, "fill would overfill the order");

            std::int64_t weighted = 0;
            if (!checked_mul(*r.last_qty, *r.last_px, weighted))
                return hold(HoldReason::SemanticConflict, "price times quantity overflow");
            std::int64_t total = 0;
            if (!checked_add(s.px_qty_weighted, weighted, total))
                return hold(HoldReason::SemanticConflict, "weighted price overflow");

            Fill f;
            f.exec_id = *r.exec_id;
            f.qty = *r.last_qty;
            f.px = *r.last_px;
            f.fingerprint = r.fingerprint;
            f.applied_seq = s.fills.size() + 1;
            s.fills.push_back(f);
            s.cum_qty = new_cum;
            s.px_qty_weighted = total;
            s.leaves_qty = s.accepted_qty > s.cum_qty ? s.accepted_qty - s.cum_qty : 0;
            s.lp_acked = true;
            s.local_only = false;

            ReduceResult out = applied("fill applied");
            if (s.leaves_qty == 0) {
                // A completed order is terminal even if a cancel is pending; the
                // cancel request is resolved by its own report (OR-H-07).
                s.status = OrdStatus::Filled;
                out.effects.push_back(
                    Effect{EffectKind::ReleaseRiskReservation, s.chain_id.str(), HoldReason::None});
            } else if (!s.terminal()) {
                if (s.status != OrdStatus::PendingCancel && s.status != OrdStatus::PendingReplace)
                    s.status = OrdStatus::PartiallyFilled;
                else
                    s.status_before_pending = OrdStatus::PartiallyFilled;
            }
            ++s.version;
            out.effects.push_back(Effect{EffectKind::NotifyClient, s.chain_id.str(), HoldReason::None});
            return out;
        }

        case ExecType::PendingCancel: {
            if (s.terminal()) {
                ++s.version;
                return evidence("pending cancel after terminal, recorded only");
            }
            set_status(s, OrdStatus::PendingCancel);
            s.lp_acked = true;
            s.local_only = false;
            ++s.version;
            return applied("cancel pending, order still working");
        }

        case ExecType::PendingReplace: {
            if (s.terminal()) {
                ++s.version;
                return evidence("pending replace after terminal, recorded only");
            }
            // R-I12: the proposed price and quantity are NOT active yet.
            set_status(s, OrdStatus::PendingReplace);
            s.lp_acked = true;
            s.local_only = false;
            ++s.version;
            return applied("replace pending, accepted version unchanged");
        }

        case ExecType::Canceled: {
            if (s.status == OrdStatus::Filled)
                return hold(HoldReason::SemanticConflict, "cancel confirmation after full fill");
            if (s.status == OrdStatus::Canceled) return noop();
            // Fills already applied are kept; a cancel only ends the remainder.
            s.status = OrdStatus::Canceled;
            s.leaves_qty = 0;
            s.cancel_proven = true;
            ReduceResult out = applied("order canceled, prior fills retained");
            if (s.pending_cancel) {
                out.effects.push_back(Effect{EffectKind::CloseCancelRequest,
                                             s.pending_cancel->str(), HoldReason::None});
                s.pending_cancel.reset();
            }
            out.effects.push_back(
                Effect{EffectKind::ReleaseRiskReservation, s.chain_id.str(), HoldReason::None});
            ++s.version;
            return out;
        }

        case ExecType::Replaced: {
            // OR-I-10: a replace acceptance for an order that already finished
            // cannot reopen it. The ROE has to be checked by a human first.
            if (s.terminal())
                return hold(HoldReason::ReplaceAfterTerminal,
                            "replace accepted after the order was terminal");
            if (!s.pending_replace) {
                // Either a duplicate of an acceptance we already processed, or
                // an acceptance we never asked for.
                if (r.cl_ord_id == s.accepted_cl_ord_id) return noop();
                return hold(HoldReason::IdentityConflict,
                            "replace accepted with no pending replace request");
            }
            if (r.cl_ord_id != *s.pending_replace)
                return hold(HoldReason::IdentityConflict,
                            "replace acceptance names a different ClOrdID");

            const std::int64_t new_total =
                r.order_qty.has_value() ? *r.order_qty : s.pending_replace_qty;
            // OR-I-14 should have been refused before the wire, but if the peer
            // accepts a size below what is already done, the ledger would need a
            // negative remainder. Hold rather than invent one.
            if (new_total < s.cum_qty)
                return hold(HoldReason::StateRegression,
                            "accepted replace quantity is below CumQty");

            s.accepted_cl_ord_id = *s.pending_replace;
            s.accepted_qty = new_total;
            s.accepted_price = s.pending_replace_price;
            s.pending_replace.reset();
            s.pending_replace_qty = 0;
            s.pending_replace_price = 0;
            s.leaves_qty = s.accepted_qty - s.cum_qty;
            s.status = s.cum_qty > 0 ? OrdStatus::PartiallyFilled : OrdStatus::New;
            if (s.leaves_qty == 0) s.status = OrdStatus::Filled;
            s.lp_acked = true;
            s.local_only = false;
            ++s.version;
            ReduceResult out = applied("replace accepted, version activated");
            out.effects.push_back(
                Effect{EffectKind::ActivateReplaceVersion, s.accepted_cl_ord_id.str(),
                       HoldReason::None});
            return out;
        }

        case ExecType::Rejected: {
            // A rejection that arrives after fills contradicts the fills. Doc
            // 11.2 says classify correctly and do not discard, so this is a hold
            // with the evidence kept (OR-G-18).
            if (s.cum_qty > 0)
                return hold(HoldReason::SemanticConflict,
                            "order rejected after quantity was already filled");
            s.status = OrdStatus::Rejected;
            s.leaves_qty = 0;
            s.lp_acked = true;
            s.local_only = false;
            ++s.version;
            ReduceResult out = applied("order rejected by the LP");
            out.effects.push_back(
                Effect{EffectKind::ReleaseRiskReservation, s.chain_id.str(), HoldReason::None});
            return out;
        }

        case ExecType::Expired:
        case ExecType::DoneForDay: {
            if (s.status == OrdStatus::Filled)
                return hold(HoldReason::SemanticConflict, "expiry after full fill");
            s.status = r.exec_type == ExecType::Expired ? OrdStatus::Expired : OrdStatus::DoneForDay;
            s.leaves_qty = 0;
            ++s.version;
            ReduceResult out = applied("order ended by the LP");
            out.effects.push_back(
                Effect{EffectKind::ReleaseRiskReservation, s.chain_id.str(), HoldReason::None});
            return out;
        }

        case ExecType::OrderStatus: {
            // A snapshot is evidence about the peer's view, never a ledger
            // change. R-I17: the summary does not prove the individual fills.
            // It does prove the peer knows this chain, so the projection stops
            // being local-only -- but it is not an acknowledgement of the New,
            // so lp_acked stays as it was (design doc 10.3 ack hierarchy).
            s.local_only = false;
            if (r.cum_qty > s.cum_qty)
                return hold(HoldReason::HistoryRequired,
                            "status CumQty exceeds the local fill ledger");
            if (r.cum_qty < s.cum_qty)
                return hold(HoldReason::SnapshotMismatch,
                            "status CumQty is below the local fill ledger");
            return evidence("status snapshot agrees with the ledger");
        }

        case ExecType::Stopped:
        case ExecType::Suspended:
        case ExecType::Calculated:
        case ExecType::Restated:
        case ExecType::TradeCorrect:
        case ExecType::TradeCancel:
            return quarantine(HoldReason::UnsupportedEvent, "ExecType outside the v1 profile");
    }
    return quarantine(HoldReason::UnsupportedEvent, "unreachable ExecType");
}

IOrderReducer& default_order_reducer() {
    // Stateless, so one instance is enough and its lifetime is the process.
    static OrderStateMachine instance;
    return instance;
}

}  // namespace router::p03
