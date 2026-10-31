// P04's admission port, wired to the real P05 engine.
//
// Section 2 of the design doc gives P05 sole ownership of "Risk budget,
// exposure/reservations", so the lab fake P04 ships in `deps/p05_risk.hpp` --
// which its own header says "khong duoc trinh bay la risk engine thuc" -- comes
// off the admission path and `risk::RiskCoordinator` takes its place.
//
// The two were designed apart and disagree in ways a cast would hide. Each one
// is handled explicitly below, because every one of them is a wrong number in
// an account rather than a compile error:
//
//   * router::Side is {Buy=0, Sell=1} and risk::Side is {Buy=1, Sell=2}. A
//     static_cast turns a Sell into a Buy and a Buy into neither.
//   * P05 has no release(). Budget falls only to a verified terminal event or
//     to an audited reconciliation, which is EI-25: not on a disconnect, not
//     on a timeout, not on an unknown status.
//   * P04's verdict expiry is wall-clock microseconds; P05's dispatch deadline
//     is steady-clock nanoseconds. The remaining duration crosses between
//     them; the instant does not.
//   * P05 models limit orders at one price scale. An order it cannot represent
//     is refused rather than approximated (EI-15).
//   * P04's symbol is what goes on the wire ("EUR/USD"); P05's is its own
//     instrument key ("EURUSD"). Neither derives from the other.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "deps/p05_risk.hpp"
#include "gw/contract.hpp"
#include "risk/clock.hpp"
#include "risk/coordinator.hpp"
#include "risk/store.hpp"
#include "router/clock.hpp"

namespace gw {

// Maps P04's side onto P05's. False when `in` has no representation, which the
// caller must treat as a refusal: there is no safe default, since the wrong
// answer is an order on the opposite side of the market.
[[nodiscard]] bool to_risk_side(router::Side in, risk::Side& out);

// Carries P05's dispatch deadline into P04's clock domain.
//
// The two read different clocks, so the instant is meaningless across the
// boundary: subtracting a steady_clock reading from a system_clock reading
// yields a number decades wide. What does cross is the remaining duration, so
// that is what this computes -- how long the token has left according to P05,
// expressed as a wall-clock instant according to P04.
[[nodiscard]] router::WallMicros deadline_to_router_wall(risk::MonoNanos deadline_mono,
                                                         risk::MonoNanos risk_mono_now,
                                                         router::WallMicros router_wall_now);

class RiskAdmissionAdapter final : public router::p05::IRiskAdmission {
public:
    struct Options {
        // Whose authority the admission is made on. P05 checks that the
        // principal owns the account, so an account outside this set is a
        // definitive reject rather than a lookup failure.
        risk::Principal principal;
        // The only price scale P05 is configured for. A request carrying a
        // different one is refused: EI-15 wants a scale mismatch rejected, and
        // rescaling here would be exactly the guess it forbids.
        int price_digits = kNumericContract.price_digits;
        // Wire symbol to P05 instrument key, e.g. "EUR/USD" -> "EURUSD".
        //
        // Stripping the separator would work for these two and fail the moment
        // a venue quotes something shaped differently, so the mapping is
        // configuration rather than a rule. A symbol that is not in here is
        // refused: §2 pins "account/symbol policy", and an instrument P05 has
        // no limits for is not one this lab may trade.
        std::map<std::string, risk::SymbolId> wire_to_instrument;
    };

    RiskAdmissionAdapter(risk::RiskCoordinator& coordinator, const risk::RiskStore& store,
                         const risk::Clock& risk_clock, const router::IClock& router_clock,
                         Options options);

    router::p05::RiskDecision reserve(const router::p05::RiskRequest& req) override;
    void release(const std::string& reservation_id) override;
    [[nodiscard]] bool held(const std::string& reservation_id) const override;
    void note_cancel_requested(const router::TenantId& tenant, const router::AccountId& account,
                               const router::OrderChainId& chain,
                               const router::CommandId& command_id) override;

    // --- what the adapter could not honour -------------------------------
    //
    // These are not statistics. Each list is a place where the assembled system
    // is weaker than either component alone, and a scenario that cares about
    // exposure has to assert on them rather than on the verdict alone.

    // Reservations P04 asked to hand back after a definitively failed commit.
    //
    // P04 is careful here: it releases only when the commit refused outright,
    // and holds on when the acknowledgement was merely lost, because that
    // reservation may belong to a live order. But P05 has no release, and the
    // transition it would need -- COMMITTED -> FINAL -- is refused by name
    // ("a local timeout is not a terminal proof"). Reaching FINAL through
    // IN_FLIGHT would assert a dispatch that never happened.
    //
    // So the budget stays held and the id lands here. Over-reserving costs
    // availability; under-reserving costs money, and section 3 ranks business
    // safety above bounded availability. The real fix is EI-22: once the
    // reservation and the command intent commit in one P06 transaction, a
    // failed commit leaves no reservation to hand back and this list cannot
    // fill.
    [[nodiscard]] const std::vector<std::string>& stranded_reservations() const {
        return stranded_;
    }

    // Requests refused because the symbol's scale is not the one P05 runs at.
    [[nodiscard]] std::size_t scale_mismatches() const { return scale_mismatches_; }
    // Requests refused because the side had no representation in P05.
    [[nodiscard]] std::size_t unmappable_sides() const { return unmappable_sides_; }
    // Requests refused because the wire symbol has no configured instrument.
    [[nodiscard]] std::size_t unmapped_symbols() const { return unmapped_symbols_; }
    // Pending-cancel markers P05 would not accept. EI-24 headroom is not being
    // held for these, so a cancel scenario must come out empty here.
    [[nodiscard]] std::size_t rejected_cancel_markers() const { return rejected_cancel_markers_; }

private:
    router::p05::RiskDecision refuse(router::p05::Verdict v, std::string reason) const;

    risk::RiskCoordinator& coordinator_;
    const risk::RiskStore& store_;
    const risk::Clock& risk_clock_;
    const router::IClock& router_clock_;
    Options options_;

    std::vector<std::string> stranded_;
    std::size_t scale_mismatches_ = 0;
    std::size_t unmappable_sides_ = 0;
    std::size_t unmapped_symbols_ = 0;
    std::size_t rejected_cancel_markers_ = 0;
};

}  // namespace gw
