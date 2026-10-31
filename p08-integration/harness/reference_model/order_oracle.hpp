// Oracle A: the independent order reducer.
//
// §11.1 is blunt about why this file exists: "khong dung cung mot
// implementation de tu kiem chinh no". The oracle has to be "a small reference
// model ... khong share code paths voi P03/P05/P06 reducers". An oracle derived
// from the thing it checks proves only that the code agrees with itself.
//
// Writing a second order reducer from scratch would be the obvious way to get
// one, and it is not needed: P04 already has it. `router::p03::OrderStateMachine`
// is a 477-line reducer, written by whoever built the router, against the same
// FIX 4.4 rules and sharing no code with `order_core`. P04's own header is
// explicit that it exists because "P04 chi cung cap normalized event" -- it was
// built as a stand-in, and a stand-in written independently is exactly what an
// oracle is.
//
// So this file does not implement a reducer. It makes the two existing ones
// answerable to the same question:
//
//   P04 NormalizedReport ---> router::p03::OrderStateMachine::apply --> ChainState
//          |                                                              |
//          +--> rendered to FIX text --> order_core::reduce ----> OrderAggregate
//                                                                         |
//                                       both projected and compared <------+
//
// The rendering step is not incidental. P04 holds a report as typed enums and
// int64 ticks; P03 parses FIX text and does its own decimal conversion. Going
// through text means the two reach their numbers by independent parsers at the
// pinned scale, which is the EI-15 surface -- a scale mismatch has to show up
// here rather than in an account.
//
// A disagreement is a finding, not a failure of this file. When they differ,
// one of them is wrong about FIX 4.4, and §2 gives P03 the final word on order
// status and the fill ledger.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "deps/p03_osm.hpp"
#include "order_core/aggregate.hpp"
#include "order_core/events.hpp"
#include "order_core/policy.hpp"
#include "router/ids.hpp"
#include "router/report.hpp"

namespace gw::oracle {

// What both reducers can be asked about one order chain. Deliberately small:
// these are the quantities that decide what a client is told and what the firm
// is owed, and anything else the two track is their own business.
struct Projection {
    bool terminal = false;
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t avg_px_ticks = 0;
    // Absent when P03 is in a state P04 has no OrdStatus for. That is P03
    // knowing more rather than the two disagreeing, so it is not a divergence.
    std::optional<router::OrdStatus> status;
};

// Where the two answered differently about the same event stream.
//
// Named for reducer-versus-reducer to keep it apart from P03's own
// `RecoveryStatus::HashDivergence`, which is about a replay disagreeing with
// the journal -- a different question with a different remedy.
struct ReducerDisagreement {
    std::size_t step = 0;        // which event in the sequence
    std::string_view quantity;   // "terminal", "cum_qty", "leaves_qty", "avg_px", "status"
    std::string router;          // what P04's independent reducer said
    std::string order_core;      // what P03, the owner, said
};

// P03's Lifecycle is richer than FIX's OrdStatus: LocalPreparing,
// NewSentUnknown, ReconciliationRequired and Quarantined have no tag 39 value,
// because they are statements about what the gateway knows rather than about
// what the order is. Those return nullopt rather than being flattened onto a
// neighbouring status, since reporting Quarantined as New would be the exact
// kind of guess EI-19 forbids.
[[nodiscard]] std::optional<router::OrdStatus> lifecycle_to_ord_status(order_core::Lifecycle l);

// P04's typed report, rendered back into the FIX text P03 parses.
//
// `price_digits` is the pinned scale from the contract gate. It is a parameter
// rather than a constant so a test can prove that feeding the wrong one is
// caught instead of silently rescaling the price.
//
// `instrument` is P03's symbol key, which is not P04's. P04 carries the wire
// form, "EUR/USD"; P03 and P05 both key on "EURUSD". The divergence is
// systemic rather than one component's quirk, so the symbol is supplied at
// every boundary instead of passed through -- P03 answers "no pinned scale"
// and rejects the chain for a name it does not hold, which is the correct
// refusal and an unhelpful way to discover a naming mismatch.
[[nodiscard]] order_core::PeerReport to_peer_report(const router::NormalizedReport& r,
                                                    int price_digits,
                                                    const std::string& instrument);

[[nodiscard]] Projection project(const router::p03::ChainState& s);
[[nodiscard]] Projection project(const order_core::OrderAggregate& a);

// Compares the two projections for one step. Empty means they agree on every
// quantity either of them can answer.
[[nodiscard]] std::vector<ReducerDisagreement> compare(const Projection& router_side,
                                                       const Projection& order_core_side,
                                                       std::size_t step);

}  // namespace gw::oracle
