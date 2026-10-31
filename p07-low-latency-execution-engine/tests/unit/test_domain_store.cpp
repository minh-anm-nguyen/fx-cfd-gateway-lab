// Catalog groups 18, 20 and 21: the P03 state machine and fill ledger, the P05
// risk reservation under shared limits, and the P06 atomic journal and durable
// outbox.
//
// Oracle codes from design doc 23: C is order, fill and event causality equal
// to the reference; R is exposure and admission equal to the reference; D is
// journal, projection and outbox atomic and replay-equal.
//
// A word on what these cases are for. P07 does not own these semantics -- LI-01
// forbids it from changing them -- so these cases are not a specification of
// P03 or P05. They pin the *reference model* that the differential and
// crash-window suites compare against, so that a later difference can be
// attributed to the engine rather than to a drifting oracle.

#include <string>
#include <vector>

#include "llx/domain.hpp"
#include "llx/store.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;

InputEnvelope new_order(OrderId id, Side side, Qty qty, Px px) {
    InputEnvelope e;
    e.event_id = EventId{1, id};
    e.type = EventType::NewOrder;
    e.lane = Lane::NewOrder;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.side = side;
    e.qty = qty;
    e.px = px;
    e.order_id = id;
    e.owner_epoch = 1;
    return e;
}

InputEnvelope exec(OrderId id, ExecId xid, ExecType t, Qty last_qty = 0, Px last_px = 0) {
    InputEnvelope e;
    e.event_id = EventId{2, xid};
    e.type = EventType::ExecutionReport;
    e.lane = Lane::AcceptedReceipt;
    e.accepted_evidence = true;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.order_id = id;
    e.exec_id = xid;
    e.exec_type = t;
    e.last_qty = last_qty;
    e.last_px = last_px;
    e.owner_epoch = 1;
    return e;
}

InputEnvelope cancel(OrderId new_id, OrderId orig) {
    InputEnvelope e;
    e.event_id = EventId{3, new_id};
    e.type = EventType::CancelRequest;
    e.lane = Lane::CancelReplace;
    e.risk_domain = kDomain;
    e.order_id = new_id;
    e.orig_order_id = orig;
    e.owner_epoch = 1;
    return e;
}

InputEnvelope replace(OrderId new_id, OrderId orig, Qty qty, Px px) {
    InputEnvelope e;
    e.event_id = EventId{4, new_id};
    e.type = EventType::ReplaceRequest;
    e.lane = Lane::CancelReplace;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.order_id = new_id;
    e.orig_order_id = orig;
    e.qty = qty;
    e.px = px;
    e.owner_epoch = 1;
    return e;
}

// Drives a sequence of events straight through reduce+commit, which is what the
// reference pipeline and the owner both do. Returns the outcomes so a case can
// assert on each step.
struct Harness {
    JournalStore store;
    std::uint64_t seq = 0;

    explicit Harness(Qty limit = 1000 * kQtyScale) {
        store.configure_risk_domain(kDomain, limit);
    }

    struct Step {
        Proposal proposal;
        CommitOutcome outcome = CommitOutcome::Rejected;
        CommitReceipt receipt;
    };

    Step apply_event(const InputEnvelope& ev, bool exec_seen = false) {
        Step s;
        s.proposal = reduce(store.state(), ev, exec_seen);
        if (!s.proposal.accepted || s.proposal.no_effect) return s;
        InputEnvelope e = ev;
        e.domain_seq = ++seq;
        CommitRequest req;
        req.intent = to_intent(s.proposal, e);
        req.effect = s.proposal;
        s.outcome = store.commit(req, s.receipt);
        if (s.outcome == CommitOutcome::Rejected) --seq;
        return s;
    }

    const OrderState* order(OrderId id) const {
        const auto it = store.state().orders.find(id);
        return it == store.state().orders.end() ? nullptr : &it->second;
    }
    const DomainRisk* risk() const {
        const auto it = store.state().risk.find(kDomain);
        return it == store.state().risk.end() ? nullptr : &it->second;
    }
};

}  // namespace

// --- group 18: order state machine and fill ledger -------------------------

SCENARIO("LL-341", "P0", "C,R,D", "a new order reserves exposure and stages one original") {
    Harness h;
    const Qty q = 100 * kQtyScale;
    const Harness::Step s = h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    CHECK(s.proposal.accepted);
    CHECK_EQ(s.outcome, CommitOutcome::Committed);

    const OrderState* o = h.order(1);
    CHECK(o != nullptr);
    CHECK_EQ(o->status, OrderStatus::PendingNew);
    CHECK_EQ(o->leaves_qty, q);
    CHECK_EQ(o->cum_qty, Qty{0});

    // Exposure is held for the whole order from the moment it is admitted, not
    // when it fills: the conservative envelope of doc 10.
    const DomainRisk* r = h.risk();
    CHECK(r != nullptr);
    CHECK_EQ(r->reserved_buy, q);
    CHECK_EQ(r->position, Qty{0});
    CHECK_EQ(r->worst_long(), q);

    // Exactly one outbox row, and it is a New.
    const auto ob = h.store.outbox_snapshot();
    CHECK_EQ(ob.size(), std::size_t{1});
    CHECK_EQ(ob[0].kind, std::uint8_t{1});
    CHECK_EQ(ob[0].cl_ord_id, OrderId{1});
    CHECK_EQ(ob[0].state, OutboxState::Ready);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-342", "P0", "C,R,D",
         "a partial fill moves quantity from the reservation into the position") {
    Harness h;
    const Qty q = 100 * kQtyScale, f = 30 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    const Harness::Step s = h.apply_event(exec(1, 11, ExecType::PartialFill, f, 1 * kPxScale));
    CHECK_EQ(s.outcome, CommitOutcome::Committed);

    const OrderState* o = h.order(1);
    CHECK_EQ(o->status, OrderStatus::PartiallyFilled);
    CHECK_EQ(o->cum_qty, f);
    CHECK_EQ(o->leaves_qty, q - f);

    // The conservation law of doc 24 for a BUY fill: the position gains f and
    // the buy reservation loses f, so the worst-case long is unchanged.
    const DomainRisk* r = h.risk();
    CHECK_EQ(r->position, f);
    CHECK_EQ(r->reserved_buy, q - f);
    CHECK_EQ(r->worst_long(), q);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-343", "P0", "C,R,D", "the fill ledger sums to cum_qty across many fills") {
    Harness h;
    const Qty q = 100 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    Qty total = 0;
    for (int i = 0; i < 9; ++i) {
        const Qty f = 10 * kQtyScale;
        const Harness::Step s =
            h.apply_event(exec(1, static_cast<ExecId>(20 + i), ExecType::PartialFill, f,
                               (1 + i) * kPxScale));
        CHECK_EQ(s.outcome, CommitOutcome::Committed);
        total += f;
        CHECK_EQ(h.order(1)->cum_qty, total);
    }
    // The last 10 closes the order exactly.
    const Harness::Step last =
        h.apply_event(exec(1, 40, ExecType::Fill, 10 * kQtyScale, 10 * kPxScale));
    CHECK_EQ(last.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->status, OrderStatus::Filled);
    CHECK_EQ(h.order(1)->leaves_qty, Qty{0});
    CHECK_EQ(h.order(1)->cum_qty, q);
    // Reservation fully converted, nothing left held.
    CHECK_EQ(h.risk()->reserved_buy, Qty{0});
    CHECK_EQ(h.risk()->position, q);
    CHECK_EQ(h.store.state().fills.size(), std::size_t{10});
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-344", "P0", "C,R,D",
         "a fill that would exceed the order quantity quarantines instead of absorbing") {
    Harness h;
    const Qty q = 100 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::PartialFill, 80 * kQtyScale, 1 * kPxScale));

    // 80 filled, 30 more would be 110 against an order of 100. Our view and the
    // counterparty's disagree, and doc 4 says quarantine, not absorb and not
    // drop.
    const Harness::Step s =
        h.apply_event(exec(1, 12, ExecType::PartialFill, 30 * kQtyScale, 1 * kPxScale));
    CHECK(s.proposal.accepted);  // the quarantine itself must be durable
    CHECK(s.proposal.quarantine);
    CHECK_EQ(s.proposal.reason, RejectReason::Contradiction);
    CHECK_EQ(s.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->status, OrderStatus::Quarantined);
    // The impossible fill was not applied.
    CHECK_EQ(h.order(1)->cum_qty, 80 * kQtyScale);
    CHECK_EQ(h.store.state().fills.size(), std::size_t{1});
}

SCENARIO("LL-345", "P0", "C,R,D", "a duplicate ExecID produces exactly one effect") {
    // Doc 19.3: a PossDup session replay must not become a second business
    // effect. Identity, not arrival order, is what dedups.
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 100 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    const auto fill = exec(1, 11, ExecType::PartialFill, 40 * kQtyScale, 1 * kPxScale);
    CHECK_EQ(h.apply_event(fill, false).outcome, CommitOutcome::Committed);
    const Qty cum_after_first = h.order(1)->cum_qty;

    // Same ExecID again, this time with the inbox reporting it as seen.
    const Harness::Step dup = h.apply_event(fill, true);
    CHECK(dup.proposal.accepted);
    CHECK(dup.proposal.no_effect);
    CHECK_EQ(dup.proposal.reason, RejectReason::DuplicateExec);
    CHECK_EQ(h.order(1)->cum_qty, cum_after_first);
    CHECK_EQ(h.store.state().fills.size(), std::size_t{1});
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-346", "P0", "C,R,D",
         "a cancel request holds the reservation until the LP confirms") {
    Harness h;
    const Qty q = 100 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::PartialFill, 40 * kQtyScale, 1 * kPxScale));

    // The request alone changes no exposure. Releasing on request would free
    // budget an order still live at the LP could consume.
    const Qty reserved_before = h.risk()->reserved_buy;
    const Harness::Step req = h.apply_event(cancel(2, 1));
    CHECK_EQ(req.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->status, OrderStatus::PendingCancel);
    CHECK_EQ(h.risk()->reserved_buy, reserved_before);

    // The confirmation releases exactly the unfilled remainder.
    const Harness::Step ack = h.apply_event(exec(1, 12, ExecType::Canceled));
    CHECK_EQ(ack.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->status, OrderStatus::Canceled);
    CHECK_EQ(h.order(1)->leaves_qty, Qty{0});
    CHECK_EQ(h.risk()->reserved_buy, Qty{0});
    CHECK_EQ(h.risk()->position, 40 * kQtyScale);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-347", "P0", "C,R,D",
         "a fill arriving while a cancel is pending is applied and keeps the pending mark") {
    // Doc 19.4: the fill causally precedes the cancel's confirmation, so it
    // applies, and the cancel request is still outstanding at the LP.
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 100 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(cancel(2, 1));
    CHECK_EQ(h.order(1)->status, OrderStatus::PendingCancel);

    const Harness::Step s =
        h.apply_event(exec(1, 11, ExecType::PartialFill, 25 * kQtyScale, 1 * kPxScale));
    CHECK_EQ(s.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->cum_qty, 25 * kQtyScale);
    CHECK_EQ(h.order(1)->status, OrderStatus::PendingCancel);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-348", "P0", "C,R,D",
         "a cancel for an order that was never admitted has no lineage and is refused") {
    // LL-014: marking a cancel high priority does not create the New it refers
    // to.
    Harness h;
    const Harness::Step s = h.apply_event(cancel(2, 999));
    CHECK(!s.proposal.accepted);
    CHECK_EQ(s.proposal.reason, RejectReason::UnknownOrder);
    // No outbox row: nothing may be sent for an order with no lineage.
    CHECK_EQ(h.store.outbox_snapshot().size(), std::size_t{0});
}

SCENARIO("LL-349", "P0", "C,R,D", "a replace upward reserves the delta before the ack") {
    Harness h;
    const Qty q = 100 * kQtyScale, up = 150 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));

    const Harness::Step req = h.apply_event(replace(2, 1, up, 2 * kPxScale));
    CHECK_EQ(req.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->status, OrderStatus::PendingReplace);
    // The larger size could fill as soon as the LP accepts it, so the extra
    // budget is held now.
    CHECK_EQ(h.risk()->reserved_buy, up);
    // The effective quantity has not changed yet.
    CHECK_EQ(h.order(1)->qty, q);
    CHECK_EQ(h.order(1)->pending_qty, up);

    const Harness::Step ack = h.apply_event(exec(1, 11, ExecType::Replaced));
    CHECK_EQ(ack.outcome, CommitOutcome::Committed);
    CHECK_EQ(h.order(1)->qty, up);
    CHECK_EQ(h.order(1)->px, 2 * kPxScale);
    CHECK_EQ(h.order(1)->leaves_qty, up);
    CHECK_EQ(h.order(1)->version, std::uint32_t{2});
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-350", "P0", "C,R,D",
         "a replace downward releases nothing until the LP confirms") {
    Harness h;
    const Qty q = 100 * kQtyScale, down = 40 * kQtyScale;
    h.apply_event(new_order(1, Side::Buy, q, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));

    h.apply_event(replace(2, 1, down, 1 * kPxScale));
    // Still holding the original, larger reservation: the big order is still
    // live at the LP and could fill in full. Releasing early is the double
    // spend LI-18 forbids.
    CHECK_EQ(h.risk()->reserved_buy, q);

    h.apply_event(exec(1, 11, ExecType::Replaced));
    CHECK_EQ(h.risk()->reserved_buy, down);
    CHECK_EQ(h.order(1)->qty, down);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-351", "P0", "C,R,D",
         "a replace below the filled quantity is refused, not clamped") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 100 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::PartialFill, 60 * kQtyScale, 1 * kPxScale));

    // 40 is below the 60 already filled. The fills happened and cannot be
    // unwound, so the request is refused rather than silently adjusted.
    const Harness::Step s = h.apply_event(replace(2, 1, 40 * kQtyScale, 1 * kPxScale));
    CHECK(!s.proposal.accepted);
    CHECK_EQ(s.proposal.reason, RejectReason::InvalidQuantity);
    CHECK_EQ(h.order(1)->status, OrderStatus::PartiallyFilled);
}

SCENARIO("LL-352", "P0", "C,R,D", "an event after a terminal status quarantines the order") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::Fill, 10 * kQtyScale, 1 * kPxScale));
    CHECK_EQ(h.order(1)->status, OrderStatus::Filled);

    // A late fill on a filled order is a disagreement, not a no-op.
    const Harness::Step s =
        h.apply_event(exec(1, 12, ExecType::PartialFill, 1 * kQtyScale, 1 * kPxScale));
    CHECK(s.proposal.quarantine);
    CHECK_EQ(h.order(1)->status, OrderStatus::Quarantined);
}

SCENARIO("LL-353", "P0", "C,R,D", "the average price is exact fixed point, never floating") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 3 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    // Three fills of 1 at prices 1, 2 and 3: the notional is exactly 6 and the
    // average exactly 2, with no rounding anywhere.
    h.apply_event(exec(1, 11, ExecType::PartialFill, 1 * kQtyScale, 1 * kPxScale));
    h.apply_event(exec(1, 12, ExecType::PartialFill, 1 * kQtyScale, 2 * kPxScale));
    h.apply_event(exec(1, 13, ExecType::Fill, 1 * kQtyScale, 3 * kPxScale));
    CHECK_EQ(h.order(1)->status, OrderStatus::Filled);
    CHECK_EQ(h.order(1)->avg_px, 2 * kPxScale);
}

// --- group 20: risk reservation and shared limits -------------------------

SCENARIO("LL-381", "P0", "R,D", "an order beyond the limit is refused before any outbox") {
    Harness h(100 * kQtyScale);
    const Harness::Step s = h.apply_event(new_order(1, Side::Buy, 101 * kQtyScale, kPxScale));
    CHECK(!s.proposal.accepted);
    CHECK_EQ(s.proposal.reason, RejectReason::RiskLimitExceeded);
    // Nothing reserved, nothing to send. The refusal is upstream of durable
    // admission, which is LI-21.
    CHECK_EQ(h.store.outbox_snapshot().size(), std::size_t{0});
    CHECK(h.store.state().risk.at(kDomain).reserved_buy == 0);
}

SCENARIO("LL-382", "P0", "R,D",
         "concurrent orders on one shared limit cannot overspend it") {
    // Doc 15: a shared limit is serialized by one authority. Here the store is
    // that authority, and the budget admits exactly as many orders as fit.
    Harness h(100 * kQtyScale);
    int admitted = 0, refused = 0;
    for (OrderId i = 1; i <= 20; ++i) {
        const Harness::Step s = h.apply_event(new_order(i, Side::Buy, 30 * kQtyScale, kPxScale));
        if (s.proposal.accepted) ++admitted;
        else ++refused;
    }
    // Three of thirty fit inside a hundred; the rest are refused.
    CHECK_EQ(admitted, 3);
    CHECK_EQ(refused, 17);
    const DomainRisk& r = h.store.state().risk.at(kDomain);
    CHECK_EQ(r.reserved_buy, 90 * kQtyScale);
    CHECK(r.within_limit());
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-383", "P0", "R,D",
         "opposing sides are measured by the worse direction, not netted away") {
    // The envelope is conservative: an outstanding BUY and an outstanding SELL
    // do not cancel out, because only one of them may fill.
    Harness h(100 * kQtyScale);
    CHECK(h.apply_event(new_order(1, Side::Buy, 80 * kQtyScale, kPxScale)).proposal.accepted);
    CHECK(h.apply_event(new_order(2, Side::Sell, 80 * kQtyScale, kPxScale)).proposal.accepted);
    const DomainRisk& r = h.store.state().risk.at(kDomain);
    CHECK_EQ(r.reserved_buy, 80 * kQtyScale);
    CHECK_EQ(r.reserved_sell, 80 * kQtyScale);
    CHECK_EQ(r.worst_long(), 80 * kQtyScale);
    CHECK_EQ(r.worst_short(), -80 * kQtyScale);
    CHECK_EQ(r.envelope(), 80 * kQtyScale);
    CHECK(r.within_limit());

    // A third order on either side would breach the worse direction.
    CHECK(!h.apply_event(new_order(3, Side::Buy, 30 * kQtyScale, kPxScale)).proposal.accepted);
}

SCENARIO("LL-384", "P0", "R,D", "a reservation can never go negative") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 50 * kQtyScale, kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::Fill, 50 * kQtyScale, kPxScale));
    CHECK_EQ(h.risk()->reserved_buy, Qty{0});
    // A cancel confirmation after a full fill would try to release a remainder
    // that no longer exists. It is caught as a contradiction, so the
    // reservation stays non-negative.
    const Harness::Step s = h.apply_event(exec(1, 12, ExecType::Canceled));
    CHECK(s.proposal.quarantine);
    CHECK(h.risk()->non_negative());
    CHECK(h.store.state().check().risk_non_negative);
}

SCENARIO("LL-385", "P0", "R,D", "an unknown risk domain is refused, not created") {
    Harness h;
    InputEnvelope e = new_order(1, Side::Buy, 10 * kQtyScale, kPxScale);
    e.risk_domain = 999;
    const Proposal p = reduce(h.store.state(), e, false);
    CHECK(!p.accepted);
    CHECK_EQ(p.reason, RejectReason::RiskDomainUnknown);
}

// --- group 21: atomic journal, outbox and replay --------------------------

SCENARIO("LL-401", "P0", "D", "a rejected commit leaves no trace at all") {
    Harness h;
    const std::string before = h.store.state_hash();
    const std::size_t records_before = h.store.record_count();

    // An out-of-order domain_seq is refused by the store's causal check.
    const Proposal p = reduce(h.store.state(), new_order(1, Side::Buy, kQtyScale, kPxScale),
                              false);
    CHECK(p.accepted);
    InputEnvelope e = new_order(1, Side::Buy, kQtyScale, kPxScale);
    e.domain_seq = 99;  // not the next one
    CommitRequest req;
    req.intent = to_intent(p, e);
    req.effect = p;
    CommitReceipt r;
    CHECK_EQ(h.store.commit(req, r), CommitOutcome::Rejected);

    // All or nothing: the state hash and the record count are untouched.
    CHECK_EQ(h.store.state_hash(), before);
    CHECK_EQ(h.store.record_count(), records_before);
    CHECK_EQ(h.store.stats().out_of_order_refusals, std::uint64_t{1});
}

SCENARIO("LL-402", "P0", "D", "a commit from a superseded epoch is refused") {
    // LI-14 and LI-47: a displaced owner's work cannot mutate current state.
    Harness h;
    h.store.fence_to_epoch(5);
    const std::string before = h.store.state_hash();

    InputEnvelope e = new_order(1, Side::Buy, kQtyScale, kPxScale);
    e.owner_epoch = 4;  // stale
    e.domain_seq = 1;
    const Proposal p = reduce(h.store.state(), e, false);
    CommitRequest req;
    req.intent = to_intent(p, e);
    req.effect = p;
    CommitReceipt r;
    CHECK_EQ(h.store.commit(req, r), CommitOutcome::Rejected);
    CHECK_EQ(h.store.state_hash(), before);
    CHECK_EQ(h.store.stats().stale_epoch_refusals, std::uint64_t{1});
}

SCENARIO("LL-403", "P0", "D",
         "a COMMIT_UNKNOWN is durable and resolvable by idempotency key") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // CW-17: the acknowledgement is lost after the record is durable. LI-17
    // forbids reading that as a failure, and the resolution is a key lookup,
    // never a reissued command.
    fp::disarm_all();
    fp::reset_counters();
    Harness h;
    fp::arm(std::string(fp::point::kStoreCommitLostAck), fp::Action::ReturnError, 0, 1);

    const InputEnvelope e = new_order(1, Side::Buy, 10 * kQtyScale, kPxScale);
    const Harness::Step s = h.apply_event(e);
    CHECK_EQ(s.outcome, CommitOutcome::Unknown);
    CHECK(fp::fired(fp::point::kStoreCommitLostAck) > 0);

    // The transaction did commit: the effect is present and the key resolves.
    CHECK(h.order(1) != nullptr);
    CHECK_EQ(h.order(1)->status, OrderStatus::PendingNew);
    CommitReceipt found;
    CHECK(h.store.lookup_by_idempotency(s.receipt.idempotency_key, found));
    CHECK_EQ(found.outcome, CommitOutcome::Committed);
    CHECK_EQ(found.journal_version, s.receipt.journal_version);
    fp::disarm_all();
#endif
}

SCENARIO("LL-404", "P0", "D", "an inbound duplicate is deduped by the inbox, idempotently") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 100 * kQtyScale, kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    const auto fill = exec(1, 11, ExecType::PartialFill, 20 * kQtyScale, kPxScale);
    h.apply_event(fill);
    const std::string after_first = h.store.state_hash();

    // Offer the identical event to the store again, bypassing the reducer's own
    // dedup, so what is exercised is the inbox.
    const Proposal p = reduce(h.store.state(), fill, false);
    InputEnvelope e = fill;
    e.domain_seq = h.seq + 1;
    CommitRequest req;
    req.intent = to_intent(p, e);
    req.effect = p;
    CommitReceipt r;
    const CommitOutcome o = h.store.commit(req, r);
    CHECK_EQ(o, CommitOutcome::Committed);  // idempotent success, not an error
    CHECK_EQ(h.store.stats().inbox_duplicates, std::uint64_t{1});
    // And no second effect.
    CHECK_EQ(h.store.state_hash(), after_first);
}

SCENARIO("LL-405", "P0", "D", "replaying the journal reproduces the state exactly") {
    // LI-39: the same committed log yields the same business state, whatever
    // the runtime scheduling was.
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 100 * kQtyScale, kPxScale));
    h.apply_event(new_order(2, Side::Sell, 50 * kQtyScale, 2 * kPxScale));
    h.apply_event(exec(1, 10, ExecType::New));
    h.apply_event(exec(1, 11, ExecType::PartialFill, 40 * kQtyScale, kPxScale));
    h.apply_event(cancel(3, 2));
    h.apply_event(exec(2, 12, ExecType::Canceled));

    const std::string before = h.store.state_hash();
    const std::size_t orders_before = h.store.state().orders.size();
    const std::size_t fills_before = h.store.state().fills.size();
    tt::note_state_hash(before);

    h.store.rebuild_from_journal();

    CHECK_EQ(h.store.state_hash(), before);
    CHECK_EQ(h.store.state().orders.size(), orders_before);
    CHECK_EQ(h.store.state().fills.size(), fills_before);
    CHECK(h.store.state().check().ok());
}

SCENARIO("LL-406", "P0", "D",
         "an outbox row is durable before it is claimable and carries an attempt count") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    auto ob = h.store.outbox_snapshot();
    CHECK_EQ(ob.size(), std::size_t{1});
    CHECK_EQ(ob[0].state, OutboxState::Ready);
    CHECK_EQ(ob[0].attempts, std::uint32_t{0});

    OutboxRecord claimed;
    CHECK(h.store.claim_next(1, claimed));
    // The attempt is recorded before the record is handed out, so a crash
    // between the claim and the write cannot look like "never attempted".
    CHECK_EQ(claimed.attempts, std::uint32_t{1});
    ob = h.store.outbox_snapshot();
    CHECK_EQ(ob[0].state, OutboxState::Claimed);

    // Nothing else is claimable: one original per row.
    OutboxRecord again;
    CHECK(!h.store.claim_next(1, again));

    CHECK(h.store.mark_sent(claimed.id));
    CHECK_EQ(h.store.outbox_snapshot()[0].state, OutboxState::Sent);
}

SCENARIO("LL-407", "P0", "D", "a fenced-out dispatcher cannot claim an outbox row") {
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    h.store.fence_to_epoch(9);
    OutboxRecord ob;
    CHECK(!h.store.claim_next(8, ob));   // stale epoch
    CHECK(h.store.claim_next(9, ob));    // current epoch
}

SCENARIO("LL-408", "P0", "D",
         "an attempted row replays as ambiguous, never back to ready") {
    // CW-19..CW-22: after a restart, attempts > 0 with no Sent marker means the
    // LP may already have the order. It comes back as Ambiguous so the
    // reconciliation path owns it and no second original is issued.
    Harness h;
    h.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    OutboxRecord claimed;
    CHECK(h.store.claim_next(1, claimed));

    h.store.rebuild_from_journal();
    const auto ob = h.store.outbox_snapshot();
    CHECK_EQ(ob.size(), std::size_t{1});
    CHECK_EQ(ob[0].state, OutboxState::Ambiguous);
    CHECK_EQ(ob[0].attempts, std::uint32_t{1});
    // And it is not claimable, so nothing can send it again as an original.
    OutboxRecord again;
    CHECK(!h.store.claim_next(1, again));
}

SCENARIO("LL-409", "P0", "D",
         "the state hash is canonical: same content, same hash, regardless of path") {
    // The differential suite compares this hash, so it has to be a function of
    // business content only.
    Harness a, b;
    // Same events, offered in a different interleaving across two orders.
    a.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    a.apply_event(new_order(2, Side::Sell, 20 * kQtyScale, 2 * kPxScale));
    a.apply_event(exec(1, 10, ExecType::New));
    a.apply_event(exec(2, 11, ExecType::New));

    b.apply_event(new_order(2, Side::Sell, 20 * kQtyScale, 2 * kPxScale));
    b.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    b.apply_event(exec(2, 11, ExecType::New));
    b.apply_event(exec(1, 10, ExecType::New));

    // The committed content is identical, so the hashes must agree even though
    // the journal order differs.
    CHECK_EQ(a.store.state_hash(), b.store.state_hash());
    tt::note_state_hash(a.store.state_hash());

    // And a single changed quantity must change it.
    Harness c;
    c.apply_event(new_order(1, Side::Buy, 11 * kQtyScale, kPxScale));
    CHECK_NE(c.store.state_hash(), a.store.state_hash());
}

SCENARIO("LL-410", "P0", "D",
         "a store sub-step failpoint before the write leaves nothing applied") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    // CW-11..CW-16 in-process form: the transaction is refused part-way and no
    // half effect is visible. The process-death form is in the crash suite.
    fp::disarm_all();
    fp::reset_counters();
    Harness h;
    const std::string before = h.store.state_hash();
    const std::size_t records_before = h.store.record_count();

    fp::arm(std::string(fp::point::kStoreCommitStall), fp::Action::ReturnError, 0, 1);
    const Harness::Step s = h.apply_event(new_order(1, Side::Buy, 10 * kQtyScale, kPxScale));
    CHECK_EQ(s.outcome, CommitOutcome::Rejected);
    CHECK(fp::fired(fp::point::kStoreCommitStall) > 0);

    CHECK_EQ(h.store.state_hash(), before);
    CHECK_EQ(h.store.record_count(), records_before);
    CHECK_EQ(h.store.outbox_snapshot().size(), std::size_t{0});
    fp::disarm_all();
#endif
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "domain_store"); }
