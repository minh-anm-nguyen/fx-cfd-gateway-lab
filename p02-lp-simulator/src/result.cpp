#include "lp/result.hpp"

namespace lpsim {

std::string_view to_string(WireError e) {
    switch (e) {
        case WireError::None: return "None";
        case WireError::Incomplete: return "Incomplete";
        case WireError::MissingBeginString: return "MissingBeginString";
        case WireError::BadBeginString: return "BadBeginString";
        case WireError::MissingBodyLength: return "MissingBodyLength";
        case WireError::BadBodyLength: return "BadBodyLength";
        case WireError::BodyLengthOverflow: return "BodyLengthOverflow";
        case WireError::BodyLengthTooLarge: return "BodyLengthTooLarge";
        case WireError::BodyLengthMismatch: return "BodyLengthMismatch";
        case WireError::BadCheckSumFormat: return "BadCheckSumFormat";
        case WireError::CheckSumMismatch: return "CheckSumMismatch";
        case WireError::FrameTooLarge: return "FrameTooLarge";
        case WireError::EmptyTagValue: return "EmptyTagValue";
        case WireError::BadTagNumber: return "BadTagNumber";
        case WireError::TooManyFields: return "TooManyFields";
        case WireError::MissingMsgType: return "MissingMsgType";
        case WireError::MissingRequiredHeader: return "MissingRequiredHeader";
        case WireError::DuplicateHeaderTag: return "DuplicateHeaderTag";
        case WireError::BadSeqNum: return "BadSeqNum";
        case WireError::BadTimestamp: return "BadTimestamp";
        case WireError::NonPrintableValue: return "NonPrintableValue";
        case WireError::ValueTooLong: return "ValueTooLong";
    }
    return "Unknown";
}

bool is_malformed(WireError e) {
    return e != WireError::None && e != WireError::Incomplete;
}

std::string_view to_string(StoreError e) {
    switch (e) {
        case StoreError::None: return "None";
        case StoreError::Busy: return "Busy";
        case StoreError::Full: return "Full";
        case StoreError::IoError: return "IoError";
        case StoreError::Corrupt: return "Corrupt";
        case StoreError::Conflict: return "Conflict";
        case StoreError::NotOwner: return "NotOwner";
        case StoreError::SchemaMismatch: return "SchemaMismatch";
        case StoreError::Inconsistent: return "Inconsistent";
        case StoreError::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(BizReject r) {
    switch (r) {
        case BizReject::None: return "None";
        case BizReject::MissingRequiredField: return "MissingRequiredField";
        case BizReject::AmbiguousDuplicateTag: return "AmbiguousDuplicateTag";
        case BizReject::IdTooLong: return "IdTooLong";
        case BizReject::UnknownSymbol: return "UnknownSymbol";
        case BizReject::DisabledSymbol: return "DisabledSymbol";
        case BizReject::BadQuantity: return "BadQuantity";
        case BizReject::BadPrice: return "BadPrice";
        case BizReject::OffTick: return "OffTick";
        case BizReject::OffLotStep: return "OffLotStep";
        case BizReject::UnsupportedOrdType: return "UnsupportedOrdType";
        case BizReject::UnsupportedTimeInForce: return "UnsupportedTimeInForce";
        case BizReject::UnsupportedMsgType: return "UnsupportedMsgType";
        case BizReject::DuplicateRequest: return "DuplicateRequest";
        case BizReject::ConflictingRequest: return "ConflictingRequest";
        case BizReject::UnknownOrder: return "UnknownOrder";
        case BizReject::StaleOrigClOrdId: return "StaleOrigClOrdId";
        case BizReject::TooLate: return "TooLate";
        case BizReject::AlreadyPending: return "AlreadyPending";
        case BizReject::QtyBelowCumQty: return "QtyBelowCumQty";
        case BizReject::ImmutableAttribute: return "ImmutableAttribute";
        case BizReject::StaleTransactTime: return "StaleTransactTime";
        case BizReject::FutureTransactTime: return "FutureTransactTime";
        case BizReject::UnknownAccount: return "UnknownAccount";
        case BizReject::OrderCapReached: return "OrderCapReached";
        case BizReject::AdmissionStopped: return "AdmissionStopped";
        case BizReject::NotActive: return "NotActive";
        case BizReject::CrossFirm: return "CrossFirm";
    }
    return "Unknown";
}

// FIX 4.4 OrdRejReason(103). Values with no faithful standard counterpart map
// to 99 (Other) with tag 58 carrying the specific reason, rather than being
// forced onto an unrelated enumeration.
int ord_rej_reason(BizReject r) {
    switch (r) {
        case BizReject::UnknownSymbol:
        case BizReject::DisabledSymbol: return 1;   // Unknown symbol
        case BizReject::BadQuantity:
        case BizReject::OffLotStep: return 13;      // Incorrect quantity
        case BizReject::OffTick: return 18;         // Invalid price increment
        case BizReject::UnsupportedOrdType:
        case BizReject::UnsupportedTimeInForce: return 11;  // Unsupported characteristic
        case BizReject::DuplicateRequest:
        case BizReject::ConflictingRequest: return 6;       // Duplicate Order
        case BizReject::UnknownOrder:
        case BizReject::StaleOrigClOrdId: return 5;         // Unknown order
        case BizReject::TooLate: return 4;                  // Too late to enter
        case BizReject::StaleTransactTime: return 8;        // Stale order
        case BizReject::UnknownAccount: return 15;          // Unknown account
        case BizReject::OrderCapReached: return 3;          // Order exceeds limit
        default: return 99;                                 // Other
    }
}

// FIX 4.4 CxlRejReason(102).
int cxl_rej_reason(BizReject r) {
    switch (r) {
        case BizReject::TooLate: return 0;            // Too late to cancel
        case BizReject::UnknownOrder:
        case BizReject::StaleOrigClOrdId: return 1;   // Unknown order
        case BizReject::AlreadyPending: return 3;     // Already pending cancel or replace
        case BizReject::DuplicateRequest:
        case BizReject::ConflictingRequest: return 6; // Duplicate ClOrdID received
        default: return 99;                           // Other
    }
}

std::string_view reject_text(BizReject r) {
    switch (r) {
        case BizReject::None: return "";
        case BizReject::MissingRequiredField: return "required field missing";
        case BizReject::AmbiguousDuplicateTag: return "mandatory tag sent more than once";
        case BizReject::IdTooLong: return "identifier exceeds agreed length";
        case BizReject::UnknownSymbol: return "unknown symbol";
        case BizReject::DisabledSymbol: return "symbol disabled";
        case BizReject::BadQuantity: return "invalid quantity";
        case BizReject::BadPrice: return "invalid price";
        case BizReject::OffTick: return "price not on tick";
        case BizReject::OffLotStep: return "quantity not on lot step";
        case BizReject::UnsupportedOrdType: return "unsupported OrdType";
        case BizReject::UnsupportedTimeInForce: return "unsupported TimeInForce";
        case BizReject::UnsupportedMsgType: return "unsupported MsgType";
        case BizReject::DuplicateRequest: return "duplicate request";
        case BizReject::ConflictingRequest: return "ClOrdID reused with different semantics";
        case BizReject::UnknownOrder: return "unknown order";
        case BizReject::StaleOrigClOrdId: return "OrigClOrdID is not the last accepted version";
        case BizReject::TooLate: return "too late to process";
        case BizReject::AlreadyPending: return "order already pending cancel or replace";
        case BizReject::QtyBelowCumQty: return "OrderQty below CumQty";
        case BizReject::ImmutableAttribute: return "attribute cannot be modified";
        case BizReject::StaleTransactTime: return "TransactTime too old";
        case BizReject::FutureTransactTime: return "TransactTime in the future";
        case BizReject::UnknownAccount: return "unknown account";
        case BizReject::OrderCapReached: return "order capacity reached";
        case BizReject::AdmissionStopped: return "admission stopped, fail closed";
        case BizReject::NotActive: return "session not active";
        case BizReject::CrossFirm: return "order belongs to another firm";
    }
    return "unknown";
}

std::string_view to_string(HaltReason r) {
    switch (r) {
        case HaltReason::None: return "None";
        case HaltReason::StoreCommitFailed: return "StoreCommitFailed";
        case HaltReason::StoreCorrupt: return "StoreCorrupt";
        case HaltReason::LedgerMismatch: return "LedgerMismatch";
        case HaltReason::DuplicateExecId: return "DuplicateExecId";
        case HaltReason::OutboxConflict: return "OutboxConflict";
        case HaltReason::SequenceConflict: return "SequenceConflict";
        case HaltReason::MissingOutboundHistory: return "MissingOutboundHistory";
        case HaltReason::OwnershipLost: return "OwnershipLost";
        case HaltReason::SchemaMismatch: return "SchemaMismatch";
        case HaltReason::IdentityMismatch: return "IdentityMismatch";
        case HaltReason::AllocatorRollback: return "AllocatorRollback";
        case HaltReason::StaleSnapshot: return "StaleSnapshot";
        case HaltReason::InvariantViolated: return "InvariantViolated";
        case HaltReason::UnsafeConfiguration: return "UnsafeConfiguration";
        case HaltReason::DiskCritical: return "DiskCritical";
    }
    return "Unknown";
}

}  // namespace lpsim
