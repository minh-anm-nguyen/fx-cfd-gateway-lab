// P03 -- the order state machine, the single owner of order status and the fill
// ledger (design doc 5.1 and 6.3).
//
// The important line in the ownership table is the one about P04: "Khong; P04
// chi cung cap normalized event". So this reducer is the only code in the
// project that is allowed to move an OrdStatus or append a fill, and the router
// reaches it through apply() and nothing else. Every rule below is a FIX 4.4
// business rule, not a routing decision:
//
//   * a Trade needs LastQty and LastPx; a CumQty delta is not a fill (R-I09)
//   * Pending Cancel and Pending Replace are not terminal (R-I11, R-I12)
//   * a fill while a replace is pending belongs to the accepted version
//   * an impossible quantity is held, never clamped (R-I10)
//   * a late New after a fill does not regress the state (OR-G-07)
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "router/hash.hpp"
#include "router/ids.hpp"
#include "router/report.hpp"
#include "router/result.hpp"

namespace router::p03 {

struct Fill {
    ExecutionId exec_id;
    std::int64_t qty = 0;
    std::int64_t px = 0;
    Hash fingerprint;
    std::uint64_t applied_seq = 0;  // order of application, for replay equality
};

// The projection of one order chain. `chain_id` is stable across replace, which
// is what lets a cancel after two replaces still be audited as one order.
struct ChainState {
    OrderChainId chain_id;
    TenantId tenant;
    AccountId account;
    LpId lp;
    std::string wire_symbol;
    Side side = Side::Buy;
    int price_scale = 5;

    // The accepted version: the ClOrdID the LP is working, and its parameters.
    ClientOrderId accepted_cl_ord_id;
    std::int64_t accepted_qty = 0;
    std::int64_t accepted_price = 0;

    // Outstanding requests. A pending replace does NOT change the accepted
    // version until an ExecType=5 arrives (R-I12, OR-C-04).
    std::optional<ClientOrderId> pending_cancel;
    std::optional<ClientOrderId> pending_replace;
    std::int64_t pending_replace_qty = 0;
    std::int64_t pending_replace_price = 0;

    OrdStatus status = OrdStatus::New;
    OrdStatus status_before_pending = OrdStatus::New;
    bool lp_acked = false;        // has any valid business report arrived?
    bool local_only = true;       // true until the first LP report
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t px_qty_weighted = 0;  // sum of qty*px, for AvgPx without floats
    std::vector<Fill> fills;
    std::optional<OrderId> lp_order_id;
    // Set only when an ExecType=4 Canceled report was actually applied. R-I11
    // reads this instead of re-scanning the journal, which keeps the per-step
    // oracle linear.
    bool cancel_proven = false;

    StateVersion version = 0;
    std::optional<HoldReason> hold;

    // Whatever authoritative state the installed reducer keeps, opaque here.
    //
    // The reducer contract below is that it is stateless and that all state
    // travels in this struct, so the router can run a reduction inside a
    // transaction and throw the result away when the commit fails. A reducer
    // richer than this projection -- `order_core`, which owns a fill ledger,
    // accepted-version chain and seen-report set the router has no field for --
    // would otherwise have to keep that state beside the transaction, where a
    // rolled-back commit would leave it ahead of the durable record.
    //
    // So it rides along instead: copied with the ChainState, discarded with it,
    // and never read by P04. `const` because every holder shares one immutable
    // value; a reduction produces a new one rather than mutating in place.
    //
    // It is a pointer, which means it does not survive a process restart. For
    // this lab that is correct rather than a shortcut: StoreImage is in-process
    // memory, and a reducer's state is rebuilt by replaying the journal through
    // `replay_chains_from_journal`, which is the same path the live write takes.
    // A real database behind StoreImage would need this serialised.
    std::shared_ptr<const void> reducer_state;

    bool terminal() const;
    std::int64_t avg_px() const;  // weighted average, truncated, 0 when no fills
    // Deterministic digest of the business state. Used for the replay
    // equivalence proof in OR-R-18 and PB-28.
    Hash state_hash() const;
    bool has_fill(const ExecutionId& id) const;
};

enum class ReduceOutcome {
    Applied,      // state moved
    Evidence,     // valid and recorded, but the ledger did not change (status snapshot)
    Noop,         // already applied, nothing to do
    Hold,         // contradiction a human has to look at
    Quarantine,   // not supported in v1, keep the raw evidence
};

std::string_view to_string(ReduceOutcome o);

// What the reducer wants the router to do next. The reducer itself performs no
// side effect: that keeps it pure and replayable.
enum class EffectKind {
    None,
    ReleaseRiskReservation,  // terminal state reached
    NotifyClient,
    OpenRecon,
    CloseCancelRequest,
    CloseReplaceRequest,
    ActivateReplaceVersion,
};

struct Effect {
    EffectKind kind = EffectKind::None;
    std::string subject;  // ClOrdID or chain, depending on the kind
    HoldReason reason = HoldReason::None;
};

struct ReduceResult {
    ReduceOutcome outcome = ReduceOutcome::Applied;
    HoldReason reason = HoldReason::None;
    std::vector<Effect> effects;
    std::string note;  // human-readable, already sanitised
};

// The reducer. Stateless: all state is in the ChainState the caller passes in,
// so the router can run it inside a transaction and discard the result if the
// commit fails.
// The reducer the router applies reports through.
//
// Section 2 of the Project 08 design doc gives P03 "order status and the fill
// ledger" outright, and the class below is P04's own stand-in for it. These
// entry points are virtual so the real `order_core` can take over the write
// path without the router learning a second vocabulary: the projection it reads
// afterwards is a ChainState either way.
//
// Replay goes through the same interface. Store::replay() rebuilds chains by
// re-applying journalled reports, so whichever reducer is installed is also the
// one that reconstructs state after a restart -- which is the only thing that
// makes the equivalence proof in OR-R-18 mean anything.
class IOrderReducer {
public:
    virtual ~IOrderReducer() = default;

    // Creates the local projection for a brand new chain. Nothing here is
    // visible to the LP yet: `local_only` stays true until a report arrives.
    virtual ChainState create(const OrderChainId& chain, const TenantId& tenant,
                              const AccountId& account, const LpId& lp,
                              const std::string& wire_symbol, Side side, int price_scale,
                              const ClientOrderId& cl_ord_id, std::int64_t qty,
                              std::int64_t price) = 0;

    // Applies one normalized report. `state` is mutated only when the result is
    // Applied or Evidence.
    virtual ReduceResult apply(ChainState& state, const NormalizedReport& r) = 0;

    // Records that a cancel or replace request has been sent, so the chain knows
    // a request is outstanding. Called by the router at dispatch time, not from
    // a peer message.
    virtual void note_cancel_requested(ChainState& state, const ClientOrderId& cancel_id) = 0;
    virtual void note_replace_requested(ChainState& state, const ClientOrderId& replace_id,
                                        std::int64_t qty, std::int64_t price) = 0;
};

// P04's own reducer. Stateless, so one instance serves a whole process and
// `default_order_reducer()` hands out the one the store falls back to.
class OrderStateMachine final : public IOrderReducer {
public:
    ChainState create(const OrderChainId& chain, const TenantId& tenant,
                      const AccountId& account, const LpId& lp, const std::string& wire_symbol,
                      Side side, int price_scale, const ClientOrderId& cl_ord_id,
                      std::int64_t qty, std::int64_t price) override;
    ReduceResult apply(ChainState& state, const NormalizedReport& r) override;
    void note_cancel_requested(ChainState& state, const ClientOrderId& cancel_id) override;
    void note_replace_requested(ChainState& state, const ClientOrderId& replace_id,
                                std::int64_t qty, std::int64_t price) override;
};

// The instance used when nothing else is installed.
IOrderReducer& default_order_reducer();

}  // namespace router::p03
