#include "gw/order_reducer.hpp"

#include <utility>

#include "order_core/reducer.hpp"
#include "order_oracle.hpp"
#include "router/fix44.hpp"
#include "router/numeric.hpp"

namespace gw {
namespace {

using Outcome = router::p03::ReduceOutcome;
using Hold = router::HoldReason;

router::p03::ReduceResult held(Hold reason, std::string note) {
    router::p03::ReduceResult r;
    r.outcome = Outcome::Hold;
    r.reason = reason;
    r.note = std::move(note);
    return r;
}

// order_core decides in six values; P04 reads five. Two of the six collapse,
// and the collapse is the part worth being explicit about.
Outcome to_outcome(order_core::Decision d) {
    switch (d) {
        case order_core::Decision::Applied:
            return Outcome::Applied;
        case order_core::Decision::DuplicateNoop:
            return Outcome::Noop;
        case order_core::Decision::HeldForCausality:
            return Outcome::Hold;
        case order_core::Decision::RetryCommit:
            // A commit-layer answer, not a business one. Hold is the only safe
            // reading: the event has not been applied and must not look as
            // though it was.
            return Outcome::Hold;
        case order_core::Decision::Quarantined:
        case order_core::Decision::RejectLocal:
            // Both arrive as Quarantine because P04 has no other word for
            // "refused, keep the evidence". The two are told apart by
            // OrderCoreReducer::refusals() and by the note.
            return Outcome::Quarantine;
    }
    return Outcome::Quarantine;
}

Hold to_hold(order_core::Decision d, order_core::Reason reason) {
    if (d == order_core::Decision::HeldForCausality) return Hold::HistoryRequired;
    if (d == order_core::Decision::RetryCommit) return Hold::StoreFailClosed;
    // order_core's Reason vocabulary is its own and wider than P04's; rather
    // than guess at a mapping per value, a refusal lands on the general
    // conflict and carries order_core's own word for it in the note.
    (void)reason;
    return Hold::SemanticConflict;
}

const order_core::OrderAggregate* raw_aggregate(const router::p03::ChainState& s) {
    return static_cast<const order_core::OrderAggregate*>(s.reducer_state.get());
}

}  // namespace

const order_core::OrderAggregate* aggregate_of(const router::p03::ChainState& s) {
    return raw_aggregate(s);
}

void project_into(const order_core::OrderAggregate& a, router::p03::ChainState& s) {
    // --- the accepted version -------------------------------------------
    if (a.has_accepted()) {
        s.accepted_cl_ord_id = router::ClientOrderId(a.accepted_cl_ord_id());
        s.accepted_qty = a.accepted_qty_units();
        s.accepted_price = a.accepted_price_ticks();
    }

    // --- the ledger ------------------------------------------------------
    s.cum_qty = a.cum_qty_units;
    s.leaves_qty = a.leaves_qty_units();
    s.fills.clear();
    for (const order_core::FillLedgerEntry& e : a.ledger) {
        router::p03::Fill f;
        f.exec_id = router::ExecutionId(e.exec_id);
        f.qty = e.qty_units;
        f.px = e.price_ticks;
        f.applied_seq = e.accepted_version_seen;
        s.fills.push_back(std::move(f));
    }

    // P04 holds the notional in an int64. order_core holds it in an Int128
    // precisely because the product can exceed that, so a value that does not
    // fit is left at zero: P04 derives AvgPx from this field, and a truncated
    // notional is a wrong price presented as a right one.
    const order_core::Int128 notional = a.ledger_notional();
    const order_core::Int128 kMax = static_cast<order_core::Int128>(INT64_MAX);
    const order_core::Int128 kMin = static_cast<order_core::Int128>(INT64_MIN);
    s.px_qty_weighted =
        (notional <= kMax && notional >= kMin) ? static_cast<std::int64_t>(notional) : 0;

    // --- status ----------------------------------------------------------
    const order_core::Lifecycle life = a.lifecycle();
    if (const std::optional<router::OrdStatus> status = oracle::lifecycle_to_ord_status(life)) {
        s.status = *status;
    }
    // When there is no tag 39 for the lifecycle, `status` keeps whatever it last
    // was and the hold carries the real answer. Saying New for a quarantined
    // chain would be the guess EI-19 forbids.
    if (life == order_core::Lifecycle::Quarantined) {
        s.hold = Hold::SecurityQuarantine;
    } else if (life == order_core::Lifecycle::ReconciliationRequired) {
        s.hold = Hold::IdentityConflict;
    }

    // --- pending requests -------------------------------------------------
    // P03 tracks one pending request with a phase; P04 tracks a pending cancel
    // and a pending replace separately. `peer_pending_*` is the confirmed form,
    // which is what P04's flags mean: a request the peer has acknowledged.
    s.pending_cancel.reset();
    s.pending_replace.reset();
    if (a.pending.has_value()) {
        const order_core::PendingRequest& p = *a.pending;
        if (p.kind == order_core::CommandKind::Cancel) {
            s.pending_cancel = router::ClientOrderId(p.cl_ord_id);
        } else if (p.kind == order_core::CommandKind::Replace) {
            s.pending_replace = router::ClientOrderId(p.cl_ord_id);
            if (p.desired_qty_present) s.pending_replace_qty = p.desired_qty_units;
            if (p.desired_price_present) s.pending_replace_price = p.desired_price_ticks;
        }
    }

    // --- what the peer has told us ---------------------------------------
    s.lp_acked = !a.seen_reports.empty();
    s.local_only = a.seen_reports.empty();
    s.cancel_proven = a.done_reason == order_core::DoneReason::Canceled;
    if (!a.order_id_aliases.empty()) {
        // "peer|OrderID"; P04 wants the OrderID alone, and the peer is already
        // its own field on the chain.
        const std::string& alias = a.order_id_aliases.front();
        const std::size_t bar = alias.find('|');
        s.lp_order_id =
            router::OrderId(bar == std::string::npos ? alias : alias.substr(bar + 1));
    }

    s.version = a.revision;
}

OrderCoreReducer::OrderCoreReducer(Options options) : options_(std::move(options)) {}

router::p03::ReduceResult OrderCoreReducer::step(router::p03::ChainState& state,
                                                 const order_core::OrderEvent& event,
                                                 const router::NormalizedReport* source) {
    const order_core::OrderAggregate* previous = raw_aggregate(state);
    const order_core::OrderAggregate before = previous == nullptr
                                                  ? order_core::OrderAggregate{}
                                                  : *previous;

    const order_core::Reduction reduction =
        order_core::reduce(before, event, options_.policy, ++event_index_);

    router::p03::ReduceResult out;
    out.outcome = to_outcome(reduction.decision);
    out.note = std::string(order_core::to_string(reduction.decision));
    if (!reduction.detail.empty()) out.note += ": " + reduction.detail;

    if (reduction.decision == order_core::Decision::Quarantined ||
        reduction.decision == order_core::Decision::RejectLocal) {
        ++refusals_;
    }
    if (out.outcome == Outcome::Hold || out.outcome == Outcome::Quarantine) {
        out.reason = to_hold(reduction.decision, reduction.reason);
    }

    if (!reduction.mutates()) return out;

    // A report that was valid and recorded but moved no quantity is Evidence
    // rather than Applied -- P04's own definition, and the distinction matters
    // because a status snapshot must never look like a fill.
    const bool ledger_moved = reduction.next.cum_qty_units != before.cum_qty_units ||
                              reduction.next.ledger.size() != before.ledger.size();
    if (out.outcome == Outcome::Applied && !ledger_moved && source != nullptr &&
        source->is_status_snapshot()) {
        out.outcome = Outcome::Evidence;
    }

    state.reducer_state = std::make_shared<const order_core::OrderAggregate>(reduction.next);
    project_into(reduction.next, state);

    // The fingerprint P04 stores on a fill comes from the report, not from the
    // reducer: it is the digest of the bytes that justified the fill.
    if (source != nullptr) {
        for (router::p03::Fill& f : state.fills) {
            if (source->exec_id.has_value() && f.exec_id == *source->exec_id) {
                f.fingerprint = source->fingerprint;
            }
        }
    }
    return out;
}

router::p03::ChainState OrderCoreReducer::create(
    const router::OrderChainId& chain, const router::TenantId& tenant,
    const router::AccountId& account, const router::LpId& lp, const std::string& wire_symbol,
    router::Side side, int price_scale, const router::ClientOrderId& cl_ord_id,
    std::int64_t qty, std::int64_t price) {
    // The identity P03 does not carry. A chain belongs to a tenant and a route,
    // and order_core models neither: those are the router's business.
    router::p03::ChainState state;
    state.chain_id = chain;
    state.tenant = tenant;
    state.account = account;
    state.lp = lp;
    state.wire_symbol = wire_symbol;
    state.side = side;
    state.price_scale = price_scale;
    state.accepted_cl_ord_id = cl_ord_id;
    state.accepted_qty = qty;
    state.accepted_price = price;
    state.leaves_qty = qty;

    if (price_scale != options_.price_digits) {
        ++scale_mismatches_;
        state.hold = Hold::SchemaViolation;
        return state;
    }
    const auto it = options_.wire_to_instrument.find(wire_symbol);
    if (it == options_.wire_to_instrument.end()) {
        ++unmapped_symbols_;
        state.hold = Hold::SemanticConflict;
        return state;
    }

    order_core::LocalCommand c;
    c.kind = order_core::CommandKind::New;
    c.firm = options_.firm;
    c.business_day = options_.business_day;
    c.account = account.str();
    c.symbol = it->second;
    c.side_raw = std::string(router::fix::side_to_wire(side));
    c.cl_ord_id = cl_ord_id.str();
    c.qty_text = router::format_decimal_ticks(qty, 0);
    c.price_text = router::format_decimal_ticks(price, price_scale);
    c.command_seq = ++event_index_;

    const router::p03::ReduceResult r = step(state, order_core::OrderEvent{c}, nullptr);
    if (r.outcome == Outcome::Hold || r.outcome == Outcome::Quarantine) {
        // The chain exists locally either way -- the router has a command to
        // account for -- but it is marked so nothing dispatches on it.
        state.hold = r.reason;
    }
    return state;
}

router::p03::ReduceResult OrderCoreReducer::apply(router::p03::ChainState& state,
                                                  const router::NormalizedReport& r) {
    const auto it = options_.wire_to_instrument.find(state.wire_symbol);
    if (it == options_.wire_to_instrument.end()) {
        ++unmapped_symbols_;
        return held(Hold::SemanticConflict,
                    "wire symbol " + state.wire_symbol + " maps to no P03 instrument");
    }

    order_core::PeerReport peer = oracle::to_peer_report(r, state.price_scale, it->second);
    return step(state, order_core::OrderEvent{peer}, &r);
}

void OrderCoreReducer::note_cancel_requested(router::p03::ChainState& state,
                                             const router::ClientOrderId& cancel_id) {
    // P04 calls this at dispatch time, so what P03 is being told is that a
    // cancel command has been issued -- a LocalCommand, not a peer report. The
    // result is deliberately dropped: P04's signature has nowhere to put it, and
    // the chain's own state carries whatever P03 made of it.
    order_core::LocalCommand c;
    c.kind = order_core::CommandKind::Cancel;
    c.firm = options_.firm;
    c.business_day = options_.business_day;
    c.account = state.account.str();
    c.cl_ord_id = cancel_id.str();
    c.orig_cl_ord_id = state.accepted_cl_ord_id.str();
    c.command_seq = ++event_index_;
    step(state, order_core::OrderEvent{c}, nullptr);
}

void OrderCoreReducer::note_replace_requested(router::p03::ChainState& state,
                                              const router::ClientOrderId& replace_id,
                                              std::int64_t qty, std::int64_t price) {
    order_core::LocalCommand c;
    c.kind = order_core::CommandKind::Replace;
    c.firm = options_.firm;
    c.business_day = options_.business_day;
    c.account = state.account.str();
    c.cl_ord_id = replace_id.str();
    c.orig_cl_ord_id = state.accepted_cl_ord_id.str();
    c.qty_text = router::format_decimal_ticks(qty, 0);
    c.price_text = router::format_decimal_ticks(price, state.price_scale);
    c.command_seq = ++event_index_;
    step(state, order_core::OrderEvent{c}, nullptr);
}

}  // namespace gw
