// The evidence ladder.
//
// Section 2 of the design doc states the cross-project ambiguity contract as
// six named states, and the whole point of the list is that each one is weaker
// evidence than the next:
//
//   INTENT_RECORDED  command + reservation + outbox committed; nothing sent
//   FIX_QUEUED       P01 accepted the message for dispatch
//   SOCKET_WRITTEN   the kernel took the bytes; the peer may never see them
//   LP_ACCEPTED      a correlated, persisted acknowledgment from the LP
//   EXECUTED         durable unique fill evidence, reconciled to the LP
//   UNKNOWN_OUTCOME  the LP may or may not have acted; no evidence either way
//
// Four of the fifty-six invariants are about not mistaking one rung for
// another: EI-04 (a TCP write is not a business acknowledgment), EI-09 (one
// accepted command id yields one original intent), EI-20 (an unknown outcome
// must not produce a fresh original New just because a timer fired) and EI-34
// (an outbox row in UNKNOWN must not be blind-resent).
//
// The seven components each track progress in their own vocabulary and none can
// see the whole ladder: P03 has order_core::Reason::UnknownOutcome, P04 has
// router::RoutingError::UnknownOutcomeBlocked, P05 has its own, and P01, P05
// and P07 disagree on what a DeliveryState or a CommitOutcome is. This header
// is the one place where the rungs are named for the assembled system, so an
// adapter translating between two components has to say which rung it claims.
#pragma once

#include <cstdint>
#include <string_view>

namespace gw {

// Ordered deliberately: the integer value is the strength of the evidence, and
// advances_to() relies on it. UnknownOutcome sits apart at the top because it
// is not a rung at all -- it is the admission that the ladder has been lost --
// and the gap keeps it out of any ordinary comparison reached by accident.
enum class Evidence : std::uint8_t {
    IntentRecorded = 1,
    FixQueued = 2,
    SocketWritten = 3,
    LpAccepted = 4,
    Executed = 5,
    UnknownOutcome = 255,
};

std::string_view to_string(Evidence e);

// True when `to` is a legal next state for `from`.
//
// The rule is not merely "the number went up". A rung may be skipped forward --
// an ExecutionReport can carry acceptance and a fill at once, so FixQueued ->
// Executed is legal -- but never retraced, because a component that has seen a
// fill cannot later decide the order was only queued. Any rung may fall to
// UnknownOutcome, since evidence can be lost at any point. Nothing leaves
// UnknownOutcome except LpAccepted or Executed, and only reconciliation can
// supply those: EI-20 exists because a timeout is not evidence, so a timer must
// never be allowed to resolve this state.
[[nodiscard]] bool advances_to(Evidence from, Evidence to);

// The subset of the ladder on which a risk reservation may be released. EI-25
// is explicit that a disconnect, a timeout or an unknown status is not among
// them, which is why this is a function and not a comparison against Executed:
// the answer for UnknownOutcome must be false, and the ordering alone would get
// that wrong.
[[nodiscard]] bool may_release_reservation(Evidence e);

// True when the evidence is weak enough that sending an original New for this
// business intent is still correct. Only IntentRecorded qualifies: every rung
// above it means something may already be in flight, and EI-09 caps a command
// id at one original intent.
[[nodiscard]] bool may_send_original(Evidence e);

}  // namespace gw
