// P03 on the write path.
//
// §2 gives P03 "order status and the fill ledger" outright. Until now P04's own
// 477-line reducer held them and `order_core` was only an oracle to compare
// against. This puts the real one in charge: `router::Store` takes an
// `IOrderReducer`, and the one installed here reduces through
// `order_core::reduce`.
//
// Two constraints shaped it, and both came from P04's own header rather than
// from the design doc:
//
//   * "The reducer. Stateless: all state is in the ChainState the caller passes
//     in, so the router can run it inside a transaction and discard the result
//     if the commit fails." order_core's aggregate is far richer than P04's
//     projection -- a fill ledger, an accepted-version chain, a seen-report set,
//     quarantine incidents -- so keeping it in a map beside the transaction
//     would leave it ahead of the durable record whenever a commit failed. It
//     rides in `ChainState::reducer_state` instead, immutable and copied with
//     the projection.
//   * Replay runs through the same interface. `replay_chains_from_journal`
//     re-applies journalled reports, so installing this reducer means a restart
//     rebuilds order_core aggregates by the same path the live write took. That
//     is what EI-32 asks for, and it is why no aggregate serialiser is needed:
//     P03 is a pure fold, so the events are the state.
//
// What P04 keeps: command ids, FIX mapping, correlation, routing, recon cases.
// What it stops deciding: whether a fill counts, and what the order's status is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "deps/p03_osm.hpp"
#include "gw/contract.hpp"
#include "order_core/aggregate.hpp"
#include "order_core/events.hpp"
#include "order_core/policy.hpp"
#include "router/hash.hpp"
#include "router/report.hpp"

namespace gw {

// The authoritative aggregate carried by a chain, or nullptr when the chain was
// built by a different reducer -- the honest answer for a projection that came
// out of P04's own implementation.
[[nodiscard]] const order_core::OrderAggregate* aggregate_of(const router::p03::ChainState& s);

// Fills P04's projection from P03's aggregate.
//
// Only the fields P04 actually reads are written. Two are worth naming because
// the obvious mapping is wrong:
//
//   * `status` is left alone when P03 holds a state FIX has no tag 39 value for
//     -- Quarantined, ReconciliationRequired, NewSentUnknown, LocalPreparing.
//     Those say what the gateway knows, not what the order is, and reporting one
//     of them as New is the guess EI-19 forbids. `hold` carries them instead.
//   * `px_qty_weighted` is an int64 in P04 and an Int128 in P03. A notional that
//     does not fit is left at zero rather than truncated, because a missing
//     average price is better than a wrong one.
void project_into(const order_core::OrderAggregate& a, router::p03::ChainState& s);

class OrderCoreReducer final : public router::p03::IOrderReducer {
public:
    struct Options {
        // P03 refuses a chain whose symbol it holds no pinned scale for, so the
        // policy has to carry every instrument the router may route.
        order_core::Policy policy = order_core::default_lab_policy();
        int price_digits = kNumericContract.price_digits;
        // P04's wire symbol to P03's instrument key: "EUR/USD" -> "EURUSD".
        // The same divergence the risk adapter handles, for the same reason:
        // neither name derives from the other.
        std::map<std::string, std::string> wire_to_instrument;
        // The ClOrdID uniqueness scope. P03 keys a request on
        // (firm, business_day, ClOrdID), and the account deliberately does not
        // widen it, so these two decide what counts as the same request.
        std::string firm = "FIRM";
        std::string business_day = "2026-10-30";
    };

    explicit OrderCoreReducer(Options options);

    router::p03::ChainState create(const router::OrderChainId& chain,
                                   const router::TenantId& tenant,
                                   const router::AccountId& account, const router::LpId& lp,
                                   const std::string& wire_symbol, router::Side side,
                                   int price_scale, const router::ClientOrderId& cl_ord_id,
                                   std::int64_t qty, std::int64_t price) override;

    router::p03::ReduceResult apply(router::p03::ChainState& state,
                                    const router::NormalizedReport& r) override;

    void note_cancel_requested(router::p03::ChainState& state,
                               const router::ClientOrderId& cancel_id) override;
    void note_replace_requested(router::p03::ChainState& state,
                                const router::ClientOrderId& replace_id, std::int64_t qty,
                                std::int64_t price) override;

    // --- what order_core would not do, and P04's vocabulary cannot say -------

    // Reductions order_core refused outright. A scenario expecting a clean chain
    // asserts this is zero; one driving a contradiction asserts it is not.
    // Either way it is the count P04's ReduceOutcome cannot distinguish, since
    // RejectLocal and Quarantined both arrive there as Quarantine.
    [[nodiscard]] std::size_t refusals() const { return refusals_; }
    // Chains refused before order_core saw them, because the wire symbol had no
    // configured instrument. Fails closed: §2 pins the symbol policy.
    [[nodiscard]] std::size_t unmapped_symbols() const { return unmapped_symbols_; }
    // Requests whose price scale was not the one the policy pins (EI-15).
    [[nodiscard]] std::size_t scale_mismatches() const { return scale_mismatches_; }

private:
    // Runs one event and writes the result back into `state`, including the new
    // aggregate. Returns what P04 should make of it.
    router::p03::ReduceResult step(router::p03::ChainState& state,
                                   const order_core::OrderEvent& event,
                                   const router::NormalizedReport* source);

    Options options_;
    std::size_t refusals_ = 0;
    std::size_t unmapped_symbols_ = 0;
    std::size_t scale_mismatches_ = 0;
    std::uint64_t event_index_ = 0;
};

}  // namespace gw
