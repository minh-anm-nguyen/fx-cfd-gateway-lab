// Reference order state machine and risk model.
//
// Project 07 does not own these semantics -- P03 owns the reducer and P05 owns
// the risk policy, and LI-01 forbids this project from changing either. What
// P07 needs, and what this header is, is a *reference model* of both: design
// doc 23 layer 2 defines the oracle as "a plain deque plus deterministic
// P03/P05/P06 effects", and layer 4 compares the baseline and the optimized
// pipeline by replaying the same event stream through it.
//
// So the rules here are deliberately simple and deliberately strict. They are
// not a reimplementation of P03; they are the invariants P07 must not break:
//
//   * one event produces at most one effect, and a duplicate ExecID produces
//     none (doc 19.3);
//   * a fill that contradicts the ledger quarantines the order instead of being
//     absorbed (LL-008, doc 4 "Business");
//   * a cancel cannot overtake a fill that causally precedes it (LI-36,
//     doc 19.4);
//   * risk is released only against evidence, never against a timeout
//     (LI-18), and the exposure envelope is conservative while an outcome is
//     unknown.
//
// The reducer is a pure function. It reads the current projection and produces
// a Proposal; it never mutates anything. That is what makes staging safe: doc
// 13 requires that nothing derived from a staged decision is observable before
// P06 commits, and a reducer that cannot mutate cannot accidentally make it so.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "llx/event.hpp"

namespace llx {

// --- order projection ------------------------------------------------------

enum class OrderStatus : std::uint8_t {
    None = 0,
    PendingNew = 1,      // command staged/sent, no LP acknowledgement yet
    Working = 2,         // acknowledged, nothing filled
    PartiallyFilled = 3,
    Filled = 4,          // terminal
    PendingCancel = 5,
    Canceled = 6,        // terminal
    PendingReplace = 7,
    Rejected = 8,        // terminal
    Expired = 9,         // terminal
    Quarantined = 10,    // a contradiction was observed; no further automation
};

std::string_view to_string(OrderStatus s);
bool is_terminal(OrderStatus s);
bool is_live(OrderStatus s);

struct OrderState {
    OrderId order_id = 0;
    OrderId orig_order_id = 0;   // previous link in the replace chain
    SymbolId symbol = 0;
    Side side = Side::None;
    Qty qty = 0;                 // currently effective order quantity
    Px px = 0;
    OrderStatus status = OrderStatus::None;
    Qty cum_qty = 0;
    Qty leaves_qty = 0;
    // Scaled so the average is exact in fixed point: notional is kept as
    // qty*px in the combined scale and divided only for display. Doc 10
    // forbids floating point in the domain arithmetic.
    Px avg_px = 0;
    std::uint64_t notional = 0;  // sum(last_qty * last_px), combined scale
    std::uint32_t version = 0;   // order-version chain, bumped by a replace
    // Set while a replace is outstanding, so a fill arriving in between is
    // applied against the quantity that is actually effective (doc 19.4).
    Qty pending_qty = 0;
    Px pending_px = 0;
    std::uint64_t last_order_seq = 0;  // causal position of the last applied event

    bool exists() const { return status != OrderStatus::None; }
};

struct FillRecord {
    ExecId exec_id = 0;
    OrderId order_id = 0;
    Qty last_qty = 0;
    Px last_px = 0;
    std::uint64_t journal_version = 0;
};

// --- risk model ------------------------------------------------------------

// Per risk domain. Reserved quantity is tracked per side because that is what
// makes the conservation law checkable: for a netting account, a BUY fill of f
// moves f from reserved_buy into the position, and a SELL fill moves f out of
// reserved_sell and out of the position. Those two identities are asserted
// after every event in the property tests.
struct DomainRisk {
    DomainId domain = 0;
    Qty limit_qty = 0;      // worst-case gross exposure allowed
    Qty reserved_buy = 0;   // outstanding BUY quantity held against the limit
    Qty reserved_sell = 0;
    Qty position = 0;       // net, positive long

    // Worst case if every outstanding BUY fills and no SELL does, and the
    // mirror image. The admission test is against the worse of the two, which
    // is what makes the hold conservative while outcomes are unknown.
    Qty worst_long() const { return position + reserved_buy; }
    Qty worst_short() const { return position - reserved_sell; }
    Qty envelope() const {
        const Qty a = worst_long() < 0 ? -worst_long() : worst_long();
        const Qty b = worst_short() < 0 ? -worst_short() : worst_short();
        return a > b ? a : b;
    }
    bool within_limit() const { return envelope() <= limit_qty; }
    // Reserved quantity can never go negative: that would mean more was
    // released than was ever held, which is the double-release bug LI-10 and
    // the P05 contract exist to prevent.
    bool non_negative() const { return reserved_buy >= 0 && reserved_sell >= 0; }
};

// --- reducer output --------------------------------------------------------

enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownOrder,         // cancel/replace with no lineage (LL-014)
    DuplicateOrder,
    DuplicateExec,        // same ExecID seen before: exactly one effect
    InvalidQuantity,
    InvalidPrice,
    InvalidSymbol,
    OrderTerminal,        // event arrived after a terminal status
    RiskLimitExceeded,
    RiskDomainUnknown,
    Contradiction,        // ledger cannot absorb the event; quarantine
    CausalOutOfOrder,
    NotLive,
};

std::string_view to_string(RejectReason r);

// What the reducer proposes. Applied only by the store, and only atomically.
struct Proposal {
    bool accepted = false;
    RejectReason reason = RejectReason::None;
    bool quarantine = false;

    bool write_projection = false;
    OrderState order{};          // full proposed value, not a delta

    bool append_fill = false;
    FillRecord fill{};

    bool write_risk = false;
    DomainId risk_domain = 0;
    Qty reserved_buy_delta = 0;
    Qty reserved_sell_delta = 0;
    Qty position_delta = 0;

    // The original FIX command the dispatcher may send once this commits.
    std::uint8_t outbox_kind = 0;  // 0 none, 1 New, 2 Cancel, 3 Replace
    OrderId outbox_cl_ord_id = 0;
    OrderId outbox_orig_cl_ord_id = 0;

    bool no_effect = false;  // duplicate that is correctly ignored
};

// --- the projection the store owns ----------------------------------------

// Ordered containers on purpose. A state hash taken over an unordered_map would
// depend on bucket order, which differs between the baseline and the optimized
// pipeline even when the business content is identical, and doc 23 layer 4
// compares exactly that hash.
struct BusinessState {
    std::map<OrderId, OrderState> orders;
    std::vector<FillRecord> fills;
    std::map<DomainId, DomainRisk> risk;

    // Canonical, order-independent digest of the committed business state. This
    // is the oracle doc 23 requires in every artifact.
    std::string state_hash() const;

    // Every invariant the reference model guarantees, checked together. Any
    // false here is an LI violation and a hard release block (doc 28 Gate 1).
    struct Check {
        bool ledger_consistent = true;   // cum + leaves == qty for live orders
        bool fills_match_ledger = true;  // sum of fills == cum_qty per order
        bool risk_non_negative = true;
        bool risk_within_limit = true;
        bool no_duplicate_exec = true;
        std::string detail;
        bool ok() const {
            return ledger_consistent && fills_match_ledger && risk_non_negative &&
                   risk_within_limit && no_duplicate_exec;
        }
    };
    Check check() const;
};

// --- the reducer -----------------------------------------------------------

// Pure: reads state, writes nothing. `seen_exec` is the inbox dedup set the
// store owns, passed in so the reducer can refuse a duplicate without being
// able to record one.
Proposal reduce(const BusinessState& state, const InputEnvelope& ev,
                bool exec_already_seen);

// Applies an accepted proposal. Only the store calls this, and only inside the
// atomic commit, which is what keeps staged state non-authoritative (doc 13).
void apply(BusinessState& state, const Proposal& p, std::uint64_t journal_version);

// Builds the durable intent the store commits from a proposal. Kept separate so
// the owner can stage without touching the store, and so a test can inspect
// exactly what would have been written.
DurableBusinessIntent to_intent(const Proposal& p, const InputEnvelope& ev);

}  // namespace llx
