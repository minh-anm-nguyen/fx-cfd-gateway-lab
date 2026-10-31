// Typed outcomes. Nothing in this router decides policy from an exception: every
// refusal is a classified value so the tables in design doc 8, 11.2 and 16.2 can
// be applied deterministically and asserted by tests.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace router {

// Why a command was refused at admission (design doc 8.1, 8.3).
enum class RoutingError {
    None = 0,
    ValidationError,       // schema / units / precision / bounds        OR-A-06..A-10
    IdempotencyConflict,   // same command_id, different payload         OR-A-03 A-04
    AuthzDenied,           // tenant or actor does not own the account   OR-A-05 A-12 Q-01
    UnsupportedSymbol,     // outside the configured allowlist           OR-A-11
    UnsupportedOrdType,    // outside the pinned ROE profile             OR-B-09
    UnsupportedTif,        //                                            OR-B-10
    AmbiguousSymbol,       // two mappings for one user alias            OR-B-11
    RouteUnavailable,      // no active route for the account            OR-B-02
    ConfigInvalid,         // ambiguous / unchecksummed / forbidden      OR-B-03 B-16 B-17
    RouteVersionConflict,  // config moved under the admission           OR-B-05
    EntitlementRevoked,    //                                            OR-B-15
    RiskRejected,          // P05 said no                                OR-A-15
    RiskUnavailable,       // P05 timeout or down, fail closed           OR-A-16
    RiskExpired,           // verdict outlived valid_until               OR-A-18
    StaleVersion,          // expected_version != P03 aggregate version  OR-A-14
    UnknownChain,          // cancel or replace for a chain we do not own
    PendingConflict,       // strict pending policy blocks this request  OR-C-08 C-09 H-10
    TerminalState,         // chain already Filled or Canceled           OR-H-13 H-14
    UnknownOutcomeBlocked, // chain is ambiguous, no business retry      R-I16
    CapabilityUnsupported, // ROE does not enable this, e.g. 35=H        OR-B-07 N-01
    Overloaded,            // bounded queue refused the work             OR-P-03 P-04
    StoreUnavailable,      // P06 cannot make the intent durable         OR-O-20 R-I20
    SessionUnavailable,    // P01 not ACTIVE and policy forbids queueing
    AuditUnavailable,      // audit could not be committed, fail closed  OR-Q-10
    Halted,                // router is in DEGRADED or HALT              design doc 15.2
};

std::string_view to_string(RoutingError e);

// Why an inbound application message could not become a business event
// (design doc 11.1 and 11.2).
enum class ValidationError {
    None = 0,
    NotApplicationMessage,  // 35 is a session message P01 should have handled  OR-J-09
    UnsupportedMsgType,
    MissingRequiredTag,
    DuplicateTag,
    BadTagFormat,
    BadDecimalFormat,     // exponent, locale comma, stray sign      OR-D-15 D-16
    ValueOutOfRange,      // negative qty, overflow                  OR-D-17 D-18
    FieldTooLong,         // bounded parser                          OR-D-13
    NonPrintableValue,    //                                         OR-D-14
    UnknownExecType,      //                                         OR-D-11
    UnknownOrdStatus,     //                                         OR-D-12
    MissingTradeFields,   // ExecType=F without 17, 31 or 32         OR-D-07 D-08 J-10
    ZeroLastQty,          //                                         OR-G-14
    MissingCancelRejectResponseTo,  // 35=9 without 434              OR-D-10 H-05
    InconsistentQuantities,         // 14, 151 and 38 impossible     OR-J-18
    ProhibitedCustomTag,            // must-understand violation     OR-Q-17
    PayloadTooLarge,
};

std::string_view to_string(ValidationError e);

// Durability failures from the P06 adapter (design doc 15.2).
enum class StoreError {
    None = 0,
    Full,          // ENOSPC
    IoError,
    Corrupt,       // checksum or audit chain mismatch
    Conflict,      // uniqueness violation, used for idempotent inserts
    NotOwner,      // fencing token lost          OR-E-18 R-12
    SchemaMismatch,
    Unavailable,
    CommitAckLost, // commit happened but the caller never learned  OR-O-19
};

std::string_view to_string(StoreError e);

// Why something is being held for a human instead of being decided by code.
// Design doc 14 and 11.2: a hold is a first class outcome, never a silent drop.
enum class HoldReason {
    None = 0,
    AmbiguousSend,          // R-I16  unknown whether the LP saw the request
    AckTimeout,
    ExecIdConflict,         // same ExecID, different content      OR-K-05 K-06
    DuplicateExecIdChain,   // same ExecID claimed by two chains   OR-K-16
    OrphanReport,           // nothing correlates                  OR-L-07 J-11
    SecurityQuarantine,     // wrong account, tenant or CompID     OR-C-18 J-15 Q-03
    IdentityConflict,       // OrderID or lineage contradiction    OR-C-12 C-14 L-16
    SemanticConflict,       // wrong symbol, side or qty relation  OR-G-11..G-13 J-16..J-18
    SchemaViolation,        // report failed the dictionary        OR-D-07..D-12
    UnsupportedEvent,       // Restated, TradeCorrect, TradeCancel OR-J-12..J-14
    SnapshotMismatch,       // status disagrees with the ledger    OR-K-13 N-08
    HistoryRequired,        // status ahead of the ledger          OR-K-12 N-10 N-11
    CapabilityMissing,      // cannot probe because H is off       OR-B-07 M-08 N-01
    ReconBudgetExhausted,   //                                     OR-M-13
    StateRegression,        // CumQty would go backwards           OR-N-11, section 17.3
    ReplaceAfterTerminal,   //                                     OR-I-10
    PendingPolicy,          // strict pessimistic chaining         OR-C-08 H-12 I-11
    StoreFailClosed,
    OrphanQuotaExceeded,    //                                     OR-P-19
    OperatorRequested,
};

std::string_view to_string(HoldReason r);

// ---------------------------------------------------------------------------
// A small Result. Deliberately not std::expected: this code must build on any
// C++20 compiler, and the explicit accessors read better at call sites.

template <typename T, typename E>
class Result {
public:
    static Result ok(T v) { return Result(std::move(v), true); }
    static Result err(E e) { return Result(std::move(e)); }

    bool is_ok() const noexcept { return ok_; }
    bool is_err() const noexcept { return !ok_; }
    explicit operator bool() const noexcept { return ok_; }

    const T& value() const { return std::get<0>(slot_); }
    T& value() { return std::get<0>(slot_); }
    const E& error() const { return std::get<1>(slot_); }

private:
    Result(T v, bool) : slot_(std::in_place_index<0>, std::move(v)), ok_(true) {}
    explicit Result(E e) : slot_(std::in_place_index<1>, std::move(e)), ok_(false) {}

    std::variant<T, E> slot_;
    bool ok_ = false;
};

}  // namespace router
