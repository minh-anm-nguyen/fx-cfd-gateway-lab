// Reservation record and lifecycle state machine (§11).
//
// A reservation is the durable claim on risk budget for one order chain. It
// carries two quantity branches: the `accepted` version (what the LP has
// acknowledged) and an optional `proposed` version (an in-flight Replace). The
// risk-effective outstanding quantity is the *maximum* of the two remainders,
// never the proposed one alone, because either branch may be the one that fills
// (RI-06, §14.1).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "risk/policy.hpp"
#include "risk/result.hpp"
#include "risk/types.hpp"
#include "risk/valuation.hpp"

namespace risk {

enum class ReservationState : std::uint8_t {
  Requested = 0,
  Denied = 1,
  Committed = 2,
  InFlight = 3,
  Working = 4,
  Unknown = 5,
  Final = 6,
  Quarantine = 7,
};

std::string_view reservation_state_name(ReservationState s) noexcept;
bool parse_reservation_state(std::string_view text, ReservationState& out) noexcept;

// True when the reservation still consumes risk budget. Note that Unknown and
// Quarantine are both live: not knowing the outcome is not evidence that the
// order is gone (RI-26).
bool state_holds_budget(ReservationState s) noexcept;

// Only the transitions drawn in the §11 diagram are permitted. In particular
// there is no Committed -> Final edge driven by a local timeout.
Status validate_transition(ReservationState from, ReservationState to);

// Same rules, but allows one implied intermediate hop. An LP acknowledgement
// arriving for a reservation still marked COMMITTED is itself evidence that
// the dispatch happened, so COMMITTED -> WORKING is legal *via* IN_FLIGHT.
//
// This relaxation is about bookkeeping, not about what may release a hold:
// reaching FINAL still requires a verified terminal event, which the event
// layer enforces. A local timeout produces no event and therefore no
// transition (RI-26). FINAL and DENIED remain sinks on both functions.
Status validate_transition_path(ReservationState from, ReservationState to);

struct Reservation {
  ReservationId reservation_id;
  TenantId tenant_id;
  DomainId domain_id;
  AccountId account_id;
  OrderChainId order_chain_id;
  CommandId command_id;  // the New command that created this chain

  SymbolId symbol;
  Side side = Side::Buy;

  // Accepted branch: the quantity the LP is known to be working, in base units.
  std::int64_t accepted_total_units = 0;
  // Verified cumulative executed quantity, from the P03 fill ledger.
  std::int64_t cum_units = 0;

  // The *raw* limit price of each branch, before any conservative buffer.
  // Revaluation reads these rather than the stored risk price: re-buffering an
  // already-buffered price would ratchet the bound upward on every pass, which
  // is conservative but makes two equivalent states value differently.
  std::int64_t accepted_limit_price_ticks = 0;
  std::optional<std::int64_t> proposed_limit_price_ticks;

  // Proposed branch of an in-flight Replace, with the command that proposed it.
  std::optional<std::int64_t> proposed_total_units;
  std::optional<CommandId> pending_modify_command_id;
  std::optional<Valuation> proposed_valuation;

  // A cancel request that has been sent but not confirmed. Recorded for
  // observability; it deliberately has no effect on the held quantity (RI-04).
  bool pending_cancel = false;
  std::optional<CommandId> pending_cancel_command_id;

  ReservationState state = ReservationState::Requested;
  PolicyVersion policy_version = 0;
  std::string policy_hash;
  Valuation accepted_valuation;

  Revision revision = 0;
  Reason terminal_reason = Reason::Ok;
  std::string note;

  // max(0, accepted_total - cum)
  std::int64_t accepted_remaining() const noexcept;
  // max(0, proposed_total - cum); 0 when there is no proposal.
  std::int64_t proposed_remaining() const noexcept;
  // The quantity that actually consumes B/S: max of both branches, or 0 once
  // the reservation is terminal.
  std::int64_t risk_effective_remaining() const noexcept;

  bool holds_budget() const noexcept { return state_holds_budget(state); }
  bool has_pending_modify() const noexcept {
    return proposed_total_units.has_value();
  }

  // The conservative valuation to use for this reservation: the branch with the
  // higher risk price, so a pending replace at a worse price is charged at that
  // worse price before it is accepted (§9.2).
  const Valuation& effective_valuation() const noexcept;

  // The higher raw limit price across both branches, used as the revaluation
  // input for this symbol.
  std::int64_t effective_limit_price_ticks() const noexcept;

  std::string digest() const;
};

}  // namespace risk
