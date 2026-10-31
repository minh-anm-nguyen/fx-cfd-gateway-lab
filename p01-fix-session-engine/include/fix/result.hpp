// Typed error taxonomy. The engine never relies on exceptions for protocol
// decisions; every failure is classified so the policy table in design doc 9.5
// can be applied deterministically.
#pragma once

#include <string>
#include <string_view>

namespace fixse {

// Framing/decoding failures, all detected BEFORE any session or application
// state is touched (invariant I-03). Scenario IDs refer to design doc 15.3.
enum class WireError {
    None = 0,
    Incomplete,             // need more bytes; not an error by itself
    MissingBeginString,     // first field is not tag 8
    BadBeginString,         // tag 8 present but not FIX.4.4           B-16
    MissingBodyLength,      // second field is not tag 9               B-12
    BadBodyLength,          // empty / non-numeric / negative          B-06
    BodyLengthOverflow,     // more digits than any sane length        B-07
    BodyLengthTooLarge,     // exceeds max_frame_bytes                 B-08
    BodyLengthMismatch,     // trailer absent at computed boundary     B-04 B-05 B-11
    BadCheckSumFormat,      // not exactly three digits                B-10
    CheckSumMismatch,       // computed differs from declared          B-09
    FrameTooLarge,          // buffer exceeded cap without a frame     J-04
    EmptyTagValue,          // tag with no value                       B-06
    BadTagNumber,           // non-numeric or overflowing tag id       B-21
    TooManyFields,          // field count cap exceeded                B-22
    MissingMsgType,         // tag 35 absent or not the third field    B-13
    MissingRequiredHeader,  // 34 / 49 / 56 / 52 absent                B-14
    DuplicateHeaderTag,     // a mandatory tag appears twice           B-15
    BadSeqNum,              // 34 non-numeric, zero or overflowing     E-19
    BadTimestamp,           // 52 or 122 not a valid UTCTimestamp      B-23 H-05
    NonPrintableValue,      // control or non-ASCII byte in a value    B-18 J-09
    UnsupportedMsgType,     // 35 outside the configured dictionary    B-20
    BadFieldValue,          // enumerated field carries an illegal value  B-19
};

std::string_view to_string(WireError e);

// True for everything except None and Incomplete.
bool is_malformed(WireError e);

// Durability failures surfaced by ISessionStore. The engine must never write a
// FIX frame to the socket when a commit did not succeed (invariant I-06).
enum class StoreError {
    None = 0,
    Busy,            // lock contention or timeout            G-13
    Full,            // ENOSPC / SQLITE_FULL                   G-10 G-11
    IoError,         // EIO / permission revoked               G-12
    Corrupt,         // integrity check failed                 G-14
    Conflict,        // uniqueness violated (duplicate key)
    NotOwner,        // ownership lease not held              I-01
    SchemaMismatch,  // incompatible schema version           G-15
    Inconsistent,    // state vs history disagreement         G-17 G-18
    Unknown,
};

std::string_view to_string(StoreError e);

// Why the engine refused to act, or why it entered FAULTED. Used for metrics,
// audit records and test assertions.
enum class FaultReason {
    None = 0,
    StoreCommitFailed,
    StoreCorrupt,
    StoreInconsistent,
    OwnershipLost,
    SequenceRollback,
    UnauthorizedSequenceReset,
    GapFillOverApplicationMessage,
    MissingOutboundHistory,
    CorruptOutboundHistory,
    ResendStorm,
    RecoveryTimeout,
    ConfigRejected,
    InboxOverflow,
    IntegrityMismatch,
    SchemaMismatch,
};

std::string_view to_string(FaultReason r);

}  // namespace fixse
