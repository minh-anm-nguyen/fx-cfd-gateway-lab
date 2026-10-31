#include "gw/evidence.hpp"

namespace gw {

std::string_view to_string(Evidence e) {
    switch (e) {
        case Evidence::IntentRecorded: return "INTENT_RECORDED";
        case Evidence::FixQueued: return "FIX_QUEUED";
        case Evidence::SocketWritten: return "SOCKET_WRITTEN";
        case Evidence::LpAccepted: return "LP_ACCEPTED";
        case Evidence::Executed: return "EXECUTED";
        case Evidence::UnknownOutcome: return "UNKNOWN_OUTCOME";
    }
    // Unreachable for any value the enum can legally hold. Naming it rather
    // than returning an empty string keeps a corrupted value visible in a log
    // instead of looking like a field nobody filled in.
    return "EVIDENCE_INVALID";
}

bool advances_to(Evidence from, Evidence to) {
    // Losing the evidence is always possible: a socket dies mid-write, a commit
    // returns neither success nor failure, the LP never answers. P07 already
    // names this outcome on its own send path, llx::SendResult::Unknown.
    if (to == Evidence::UnknownOutcome) return from != Evidence::UnknownOutcome;

    // Leaving UNKNOWN_OUTCOME requires evidence, and only reconciliation can
    // produce it. EI-20: a timeout is not evidence, so no amount of waiting
    // moves this state. In particular it can never fall back to
    // IntentRecorded, which is the one rung that would re-permit an original
    // send.
    if (from == Evidence::UnknownOutcome) {
        return to == Evidence::LpAccepted || to == Evidence::Executed;
    }

    // Forward along the ladder, skipping allowed. An ExecutionReport can carry
    // acceptance and a fill together, so FixQueued -> Executed is legal. What
    // is never legal is going back, because a rung already reached is durable
    // evidence and cannot be un-observed.
    return static_cast<std::uint8_t>(to) > static_cast<std::uint8_t>(from);
}

bool may_release_reservation(Evidence e) {
    // EI-25: not on a disconnect, not on a timeout, not on an unknown status.
    // Of the six rungs only EXECUTED is a reconciled terminal outcome, and
    // everything weaker may still fill.
    return e == Evidence::Executed;
}

bool may_send_original(Evidence e) {
    // EI-09 caps one accepted command id at one original business intent, and
    // EI-34 forbids resending an outbox row sitting in UNKNOWN. Both reduce to
    // the same test: nothing has left the building yet.
    return e == Evidence::IntentRecorded;
}

}  // namespace gw
