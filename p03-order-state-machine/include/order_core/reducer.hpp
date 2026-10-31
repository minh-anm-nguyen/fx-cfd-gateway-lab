// The pure reducer.
//
// reduce() is a function of (previous state, event, pinned policy) and nothing
// else. No clock, no random source, no socket, no storage, no logging side
// effect. Everything it wants the outside world to do leaves as an
// EffectIntent, and the caller only acts on those intents after the atomic
// commit succeeds (design doc 5.2). That is what makes replay and the crash
// windows testable at all.
#pragma once

#include <string>
#include <vector>

#include "order_core/aggregate.hpp"
#include "order_core/events.hpp"
#include "order_core/invariants.hpp"
#include "order_core/policy.hpp"

namespace order_core {

// Design doc 10.2. These six are the only outcomes; there is no "ok, probably".
enum class Decision {
    Applied,
    DuplicateNoop,
    HeldForCausality,
    Quarantined,
    RetryCommit,
    RejectLocal,
};
const char* to_string(Decision d);

enum class Reason {
    None,
    // local command validation
    InvalidQty,
    InvalidPrice,
    InvalidSide,
    MissingField,
    UnknownSymbol,
    QtyStepMismatch,
    QtyOutOfRange,
    IdTooLong,
    IdCollision,
    LineageMismatch,
    PendingMutation,
    TerminalState,
    UnsafeDependentCommand,
    QtyBelowCum,
    ZeroWorkingLeaves,
    ResourceExhausted,
    NotAuthorized,
    // peer report handling
    DuplicateSamePayload,
    ConflictingDuplicate,
    Overfill,
    InvalidLastQty,
    CumRegression,
    MissingFillEvidence,
    CumMismatch,
    LeavesInconsistent,
    AccountMismatch,
    PeerMismatch,
    SymbolMismatch,
    SideMismatch,
    IdentityInsufficient,
    AliasCollision,
    UnknownRequest,
    UnexpectedPending,
    ObsoleteOrdStatus,
    WrongFixVersionSemantics,
    UnsupportedEvent,
    UnknownExecType,
    MissingCorrectionReference,
    UnknownCorrectionReference,
    PriceOutsideLimit,
    ScaleMismatch,
    PostTerminalEvent,
    LateReportNoRegression,
    ContradictoryResponse,
    StatusInconsistent,
    MissingSourceCursor,
    StaleGeneration,
    AwaitingCorrelation,
    AwaitingFillEvidence,
    UnknownOutcome,
    InvariantViolated,
    MalformedNumber,
    ProtocolMismatch,
};
const char* to_string(Reason r);

// Something the orchestration must do, expressed as data. The idempotency key
// is stable across replay so a redelivered effect is recognisable as the same
// one (what-if F-20, L-11).
struct EffectIntent {
    enum class Kind {
        RouteNew,
        RouteCancel,
        RouteReplace,
        RiskReserve,
        RiskAdjust,
        RiskRelease,
        QueryOrderStatus,
        ReconciliationRequired,
        Alert,
        Audit,
    };
    Kind kind = Kind::Audit;
    std::string idempotency_key;
    std::string detail;
};
const char* to_string(EffectIntent::Kind k);

struct Reduction {
    OrderAggregate next;
    std::vector<EffectIntent> intents;
    std::vector<InvariantViolation> violations;
    Decision decision = Decision::RejectLocal;
    Reason reason = Reason::None;
    std::string detail;
    // The semantic key this event was applied under, recorded in the same
    // transaction as the state so a post-crash redelivery is a no-op.
    AppliedKey applied_key;

    [[nodiscard]] bool mutates() const { return decision == Decision::Applied; }
};

// `event_index` is the per-chain ordinal the caller assigns; it appears in
// audit records and in accepted-version provenance. It is passed in rather
// than read from a counter inside the reducer so the function stays pure.
Reduction reduce(const OrderAggregate& previous, const OrderEvent& event, const Policy& policy,
                 uint64_t event_index = 0);

}  // namespace order_core
