#include "risk/reservation.hpp"

#include "risk/checked.hpp"
#include "risk/hash.hpp"

namespace risk {

std::string_view reservation_state_name(ReservationState s) noexcept {
  switch (s) {
    case ReservationState::Requested: return "REQUESTED";
    case ReservationState::Denied: return "DENIED";
    case ReservationState::Committed: return "COMMITTED";
    case ReservationState::InFlight: return "IN_FLIGHT";
    case ReservationState::Working: return "WORKING";
    case ReservationState::Unknown: return "UNKNOWN";
    case ReservationState::Final: return "FINAL";
    case ReservationState::Quarantine: return "QUARANTINE";
  }
  return "UNKNOWN_STATE";
}

bool parse_reservation_state(std::string_view text, ReservationState& out) noexcept {
  const ReservationState all[] = {
      ReservationState::Requested, ReservationState::Denied,
      ReservationState::Committed, ReservationState::InFlight,
      ReservationState::Working,   ReservationState::Unknown,
      ReservationState::Final,     ReservationState::Quarantine};
  for (const ReservationState s : all) {
    if (reservation_state_name(s) == text) {
      out = s;
      return true;
    }
  }
  return false;
}

bool state_holds_budget(ReservationState s) noexcept {
  switch (s) {
    case ReservationState::Committed:
    case ReservationState::InFlight:
    case ReservationState::Working:
    case ReservationState::Unknown:
    case ReservationState::Quarantine:
      return true;
    case ReservationState::Requested:
    case ReservationState::Denied:
    case ReservationState::Final:
      return false;
  }
  return false;
}

Status validate_transition(ReservationState from, ReservationState to) {
  using S = ReservationState;
  const auto allow = [&](S a, S b) { return from == a && to == b; };

  if (from == to) return Status::success();  // idempotent re-application

  if (allow(S::Requested, S::Denied)) return Status::success();
  if (allow(S::Requested, S::Committed)) return Status::success();
  if (allow(S::Committed, S::InFlight)) return Status::success();
  if (allow(S::InFlight, S::Working)) return Status::success();
  if (allow(S::InFlight, S::Unknown)) return Status::success();
  if (allow(S::InFlight, S::Final)) return Status::success();      // definitive New reject
  if (allow(S::Working, S::Final)) return Status::success();
  if (allow(S::Working, S::Unknown)) return Status::success();      // session loss
  if (allow(S::Working, S::Quarantine)) return Status::success();
  if (allow(S::Unknown, S::Working)) return Status::success();
  if (allow(S::Unknown, S::Final)) return Status::success();
  if (allow(S::Unknown, S::Quarantine)) return Status::success();
  if (allow(S::Quarantine, S::Working)) return Status::success();
  if (allow(S::Quarantine, S::Final)) return Status::success();
  if (allow(S::Committed, S::Quarantine)) return Status::success();
  if (allow(S::Committed, S::Unknown)) return Status::success();

  // Everything else is refused, including:
  //   Committed -> Final (a local timeout is not a terminal proof, RI-26)
  //   Final -> anything   (terminal is terminal; a late report goes to recon)
  //   Denied -> anything
  return Status::fail(Reason::InternalInvariant,
                      std::string("illegal reservation transition ") +
                          std::string(reservation_state_name(from)) + " -> " +
                          std::string(reservation_state_name(to)));
}

Status validate_transition_path(ReservationState from, ReservationState to) {
  if (validate_transition(from, to).ok()) return Status::success();

  const ReservationState all[] = {
      ReservationState::Requested, ReservationState::Denied,
      ReservationState::Committed, ReservationState::InFlight,
      ReservationState::Working,   ReservationState::Unknown,
      ReservationState::Final,     ReservationState::Quarantine};
  for (const ReservationState mid : all) {
    if (mid == from || mid == to) continue;
    if (validate_transition(from, mid).ok() && validate_transition(mid, to).ok()) {
      return Status::success();
    }
  }
  return Status::fail(Reason::InternalInvariant,
                      std::string("no legal path ") +
                          std::string(reservation_state_name(from)) + " -> " +
                          std::string(reservation_state_name(to)));
}

std::int64_t Reservation::accepted_remaining() const noexcept {
  std::int64_t remaining = 0;
  if (!sub_i64(accepted_total_units, cum_units, remaining)) return 0;
  return remaining > 0 ? remaining : 0;
}

std::int64_t Reservation::proposed_remaining() const noexcept {
  if (!proposed_total_units.has_value()) return 0;
  std::int64_t remaining = 0;
  if (!sub_i64(*proposed_total_units, cum_units, remaining)) return 0;
  return remaining > 0 ? remaining : 0;
}

std::int64_t Reservation::risk_effective_remaining() const noexcept {
  if (!holds_budget()) return 0;
  const std::int64_t a = accepted_remaining();
  const std::int64_t p = proposed_remaining();
  return a > p ? a : p;
}

std::int64_t Reservation::effective_limit_price_ticks() const noexcept {
  const std::int64_t proposed = proposed_limit_price_ticks.value_or(0);
  return accepted_limit_price_ticks > proposed ? accepted_limit_price_ticks
                                               : proposed;
}

const Valuation& Reservation::effective_valuation() const noexcept {
  if (proposed_valuation.has_value() &&
      proposed_valuation->risk_price_ticks > accepted_valuation.risk_price_ticks) {
    return *proposed_valuation;
  }
  return accepted_valuation;
}

std::string Reservation::digest() const {
  Hasher h;
  h.named("reservation_id", reservation_id)
      .named("tenant", tenant_id)
      .named("domain", domain_id)
      .named("account", account_id)
      .named("chain", order_chain_id)
      .named("command", command_id)
      .named("symbol", symbol)
      .named("side", side_name(side))
      .named("accepted_total_units", accepted_total_units)
      .named("cum_units", cum_units)
      .named("accepted_limit_price_ticks", accepted_limit_price_ticks)
      .named("proposed_limit_price_ticks",
             proposed_limit_price_ticks.value_or(static_cast<std::int64_t>(-1)))
      .named("proposed_total_units",
             proposed_total_units.has_value() ? *proposed_total_units
                                              : static_cast<std::int64_t>(-1))
      .named("pending_modify",
             pending_modify_command_id.has_value() ? *pending_modify_command_id
                                                   : std::string("-"))
      .named("pending_cancel", pending_cancel ? std::string("1") : std::string("0"))
      .named("state", reservation_state_name(state))
      .named("policy_version", policy_version)
      .named("policy_hash", policy_hash)
      .named("accepted_valuation", accepted_valuation.digest())
      .named("proposed_valuation", proposed_valuation.has_value()
                                       ? proposed_valuation->digest()
                                       : std::string("-"))
      .named("terminal_reason", reason_name(terminal_reason));
  return h.hex();
}

}  // namespace risk
