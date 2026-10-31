// Typed error taxonomy for the LP simulator.
//
// The simulator never uses exceptions to make protocol or business decisions:
// every refusal is classified so that the policy tables in
// LP_Simulator_System_Design.md sections 7.2, 10.5 and 11.1 can be applied
// deterministically and asserted by the what-if matrix (section 16).
#pragma once

#include <string>
#include <string_view>

namespace lpsim {

// ---------------------------------------------------------------------------
// Wire layer: framing and decoding. Every one of these is detected BEFORE any
// session or business state is touched (invariant I-01).
// ---------------------------------------------------------------------------
enum class WireError {
    None = 0,
    Incomplete,             // need more bytes; not an error by itself
    MissingBeginString,     // first field is not tag 8
    BadBeginString,         // tag 8 present but not FIX.4.4            A-05
    MissingBodyLength,      // second field is not tag 9
    BadBodyLength,          // empty / non-numeric / negative
    BodyLengthOverflow,     // more digits than any sane length
    BodyLengthTooLarge,     // exceeds max_frame_bytes                  L-08
    BodyLengthMismatch,     // trailer absent at the computed boundary  B-15
    BadCheckSumFormat,      // tag 10 is not exactly three digits
    CheckSumMismatch,       // computed differs from declared           B-16
    FrameTooLarge,          // buffer cap reached without a frame       L-08
    EmptyTagValue,          // tag present with no value
    BadTagNumber,           // non-numeric or overflowing tag id
    TooManyFields,          // field-count cap exceeded
    MissingMsgType,         // tag 35 absent or not the third field
    MissingRequiredHeader,  // 34 / 49 / 56 / 52 absent
    DuplicateHeaderTag,     // a mandatory header tag appears twice     B-18
    BadSeqNum,              // 34 non-numeric, zero or overflowing
    BadTimestamp,           // 52 or 122 is not a valid UTCTimestamp
    NonPrintableValue,      // control or non-ASCII byte in a value     B-19
    ValueTooLong,           // a single field value exceeds the cap     B-19
};

std::string_view to_string(WireError e);

// True for everything except None and Incomplete.
bool is_malformed(WireError e);

// ---------------------------------------------------------------------------
// Durability layer. A report is never written to a socket when the commit that
// authorises it did not succeed (invariant I-14).
// ---------------------------------------------------------------------------
enum class StoreError {
    None = 0,
    Busy,            // lock contention or timeout
    Full,            // ENOSPC / SQLITE_FULL                            J-11
    IoError,         // EIO / permission revoked
    Corrupt,         // integrity check failed                          J-13
    Conflict,        // uniqueness violated: duplicate ExecID / event   E-14 I-13
    NotOwner,        // ownership lease not held                        J-12
    SchemaMismatch,  // incompatible schema version                     J-20
    Inconsistent,    // ledger and snapshot disagree                    E-13 J-17
    Unknown,
};

std::string_view to_string(StoreError e);

// ---------------------------------------------------------------------------
// Why the simulator refused an application request. These map onto FIX 4.4
// OrdRejReason(103) for 35=8 rejects and CxlRejReason(102) for 35=9 rejects;
// the mapping is explicit rather than implied (section 7.2).
// ---------------------------------------------------------------------------
enum class BizReject {
    None = 0,
    MissingRequiredField,     // B-01..B-05, B-13, B-14
    AmbiguousDuplicateTag,    // B-18: a mandatory business tag sent twice
    IdTooLong,                // B-19: ClOrdID beyond the agreed bound
    UnknownSymbol,            // B-10
    DisabledSymbol,           // B-10
    BadQuantity,              // B-06, B-09
    BadPrice,                 // B-07
    OffTick,                  // B-08
    OffLotStep,               // B-06 boundary
    UnsupportedOrdType,       // B-11
    UnsupportedTimeInForce,   // B-12
    UnsupportedMsgType,       // B-17
    DuplicateRequest,         // C-02, C-03: same key, same semantics
    ConflictingRequest,       // C-04, C-05, C-06: same key, other semantics
    UnknownOrder,             // F-03, G-06
    StaleOrigClOrdId,         // F-08, G-11
    TooLate,                  // F-04, F-10, G-05, G-14
    AlreadyPending,           // F-12, G-12, G-16, G-17
    QtyBelowCumQty,           // G-04
    ImmutableAttribute,       // G-19: symbol or side change
    StaleTransactTime,        // C-17
    FutureTransactTime,       // C-18
    UnknownAccount,           // C-16
    OrderCapReached,          // L-01
    AdmissionStopped,         // L-03: outbox backlog, fail closed
    NotActive,                // A-01, A-15, H-19: session not ACTIVE yet
    CrossFirm,                // F-20
};

std::string_view to_string(BizReject r);

// FIX 4.4 OrdRejReason(103) for ExecutionReport rejects.
int ord_rej_reason(BizReject r);
// FIX 4.4 CxlRejReason(102) for OrderCancelReject.
int cxl_rej_reason(BizReject r);
// Short human text for tag 58, already free of secrets.
std::string_view reject_text(BizReject r);

// ---------------------------------------------------------------------------
// Fail-closed reasons. Reaching any of these moves the simulator to HALTED:
// no new fills, no new original reports, operator recovery required (I-19).
// ---------------------------------------------------------------------------
enum class HaltReason {
    None = 0,
    StoreCommitFailed,        // J-11
    StoreCorrupt,             // J-13
    LedgerMismatch,           // E-13, J-17: recomputed CumQty != snapshot
    DuplicateExecId,          // E-14
    OutboxConflict,           // I-13: same event_id, different payload
    SequenceConflict,         // I-11: same original seq, different payload
    MissingOutboundHistory,   // I-08
    OwnershipLost,            // J-12
    SchemaMismatch,           // J-19, J-20
    IdentityMismatch,         // A-18, J-20: snapshot belongs to another profile
    AllocatorRollback,        // J-15, J-16
    StaleSnapshot,            // J-14
    InvariantViolated,        // any I-01..I-20 check that fires
    UnsafeConfiguration,      // L-05, L-06
    DiskCritical,             // L-04
};

std::string_view to_string(HaltReason r);

}  // namespace lpsim
