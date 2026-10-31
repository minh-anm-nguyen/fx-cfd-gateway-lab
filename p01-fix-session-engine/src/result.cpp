#include "fix/result.hpp"

namespace fixse {

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
        case WireError::UnsupportedMsgType: return "UnsupportedMsgType";
        case WireError::BadFieldValue: return "BadFieldValue";
    }
    return "Unknown";
}

bool is_malformed(WireError e) { return e != WireError::None && e != WireError::Incomplete; }

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

std::string_view to_string(FaultReason r) {
    switch (r) {
        case FaultReason::None: return "None";
        case FaultReason::StoreCommitFailed: return "StoreCommitFailed";
        case FaultReason::StoreCorrupt: return "StoreCorrupt";
        case FaultReason::StoreInconsistent: return "StoreInconsistent";
        case FaultReason::OwnershipLost: return "OwnershipLost";
        case FaultReason::SequenceRollback: return "SequenceRollback";
        case FaultReason::UnauthorizedSequenceReset: return "UnauthorizedSequenceReset";
        case FaultReason::GapFillOverApplicationMessage: return "GapFillOverApplicationMessage";
        case FaultReason::MissingOutboundHistory: return "MissingOutboundHistory";
        case FaultReason::CorruptOutboundHistory: return "CorruptOutboundHistory";
        case FaultReason::ResendStorm: return "ResendStorm";
        case FaultReason::RecoveryTimeout: return "RecoveryTimeout";
        case FaultReason::ConfigRejected: return "ConfigRejected";
        case FaultReason::InboxOverflow: return "InboxOverflow";
        case FaultReason::IntegrityMismatch: return "IntegrityMismatch";
        case FaultReason::SchemaMismatch: return "SchemaMismatch";
    }
    return "Unknown";
}

}  // namespace fixse
