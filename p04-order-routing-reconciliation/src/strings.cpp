// Enumeration names. Kept in one translation unit so a failure message, an
// audit reason and a metric label always spell the same outcome the same way.
#include "router/ids.hpp"
#include "router/result.hpp"

namespace router {

std::string_view to_string(Side s) { return s == Side::Buy ? "Buy" : "Sell"; }

std::string_view to_string(OrdType t) {
    switch (t) {
        case OrdType::Limit: return "Limit";
        case OrdType::Market: return "Market";
        case OrdType::Stop: return "Stop";
        case OrdType::StopLimit: return "StopLimit";
    }
    return "Unknown";
}

std::string_view to_string(TimeInForce t) {
    switch (t) {
        case TimeInForce::Day: return "Day";
        case TimeInForce::GoodTillCancel: return "GoodTillCancel";
        case TimeInForce::ImmediateOrCancel: return "ImmediateOrCancel";
        case TimeInForce::FillOrKill: return "FillOrKill";
    }
    return "Unknown";
}

std::string_view to_string(ExecType t) {
    switch (t) {
        case ExecType::New: return "New";
        case ExecType::DoneForDay: return "DoneForDay";
        case ExecType::Canceled: return "Canceled";
        case ExecType::Replaced: return "Replaced";
        case ExecType::PendingCancel: return "PendingCancel";
        case ExecType::Stopped: return "Stopped";
        case ExecType::Rejected: return "Rejected";
        case ExecType::Suspended: return "Suspended";
        case ExecType::PendingNew: return "PendingNew";
        case ExecType::Calculated: return "Calculated";
        case ExecType::Expired: return "Expired";
        case ExecType::Restated: return "Restated";
        case ExecType::PendingReplace: return "PendingReplace";
        case ExecType::Trade: return "Trade";
        case ExecType::TradeCorrect: return "TradeCorrect";
        case ExecType::TradeCancel: return "TradeCancel";
        case ExecType::OrderStatus: return "OrderStatus";
    }
    return "Unknown";
}

std::string_view to_string(OrdStatus s) {
    switch (s) {
        case OrdStatus::New: return "New";
        case OrdStatus::PartiallyFilled: return "PartiallyFilled";
        case OrdStatus::Filled: return "Filled";
        case OrdStatus::DoneForDay: return "DoneForDay";
        case OrdStatus::Canceled: return "Canceled";
        case OrdStatus::PendingCancel: return "PendingCancel";
        case OrdStatus::Stopped: return "Stopped";
        case OrdStatus::Rejected: return "Rejected";
        case OrdStatus::Suspended: return "Suspended";
        case OrdStatus::PendingNew: return "PendingNew";
        case OrdStatus::Calculated: return "Calculated";
        case OrdStatus::Expired: return "Expired";
        case OrdStatus::AcceptedForBidding: return "AcceptedForBidding";
        case OrdStatus::PendingReplace: return "PendingReplace";
    }
    return "Unknown";
}

std::string_view to_string(CxlRejResponseTo r) {
    return r == CxlRejResponseTo::OrderCancelRequest ? "OrderCancelRequest"
                                                     : "OrderCancelReplaceRequest";
}

std::string_view to_string(RoutingError e) {
    switch (e) {
        case RoutingError::None: return "None";
        case RoutingError::ValidationError: return "VALIDATION_ERROR";
        case RoutingError::IdempotencyConflict: return "IDEMPOTENCY_CONFLICT";
        case RoutingError::AuthzDenied: return "AUTHZ_DENIED";
        case RoutingError::UnsupportedSymbol: return "UNSUPPORTED_SYMBOL";
        case RoutingError::UnsupportedOrdType: return "UNSUPPORTED_ORDTYPE";
        case RoutingError::UnsupportedTif: return "UNSUPPORTED_TIF";
        case RoutingError::AmbiguousSymbol: return "AMBIGUOUS_SYMBOL";
        case RoutingError::RouteUnavailable: return "ROUTE_UNAVAILABLE";
        case RoutingError::ConfigInvalid: return "CONFIG_INVALID";
        case RoutingError::RouteVersionConflict: return "ROUTE_VERSION_CONFLICT";
        case RoutingError::EntitlementRevoked: return "ENTITLEMENT_REVOKED";
        case RoutingError::RiskRejected: return "RISK_REJECTED";
        case RoutingError::RiskUnavailable: return "RISK_UNAVAILABLE";
        case RoutingError::RiskExpired: return "RISK_EXPIRED";
        case RoutingError::StaleVersion: return "STALE_VERSION";
        case RoutingError::UnknownChain: return "UNKNOWN_CHAIN";
        case RoutingError::PendingConflict: return "PENDING_CONFLICT";
        case RoutingError::TerminalState: return "TERMINAL_STATE";
        case RoutingError::UnknownOutcomeBlocked: return "UNKNOWN_OUTCOME_BLOCKED";
        case RoutingError::CapabilityUnsupported: return "CAPABILITY_UNSUPPORTED";
        case RoutingError::Overloaded: return "OVERLOADED";
        case RoutingError::StoreUnavailable: return "STORE_UNAVAILABLE";
        case RoutingError::SessionUnavailable: return "SESSION_UNAVAILABLE";
        case RoutingError::AuditUnavailable: return "AUDIT_UNAVAILABLE";
        case RoutingError::Halted: return "HALTED";
    }
    return "UNKNOWN";
}

std::string_view to_string(ValidationError e) {
    switch (e) {
        case ValidationError::None: return "None";
        case ValidationError::NotApplicationMessage: return "NotApplicationMessage";
        case ValidationError::UnsupportedMsgType: return "UnsupportedMsgType";
        case ValidationError::MissingRequiredTag: return "MissingRequiredTag";
        case ValidationError::DuplicateTag: return "DuplicateTag";
        case ValidationError::BadTagFormat: return "BadTagFormat";
        case ValidationError::BadDecimalFormat: return "BadDecimalFormat";
        case ValidationError::ValueOutOfRange: return "ValueOutOfRange";
        case ValidationError::FieldTooLong: return "FieldTooLong";
        case ValidationError::NonPrintableValue: return "NonPrintableValue";
        case ValidationError::UnknownExecType: return "UnknownExecType";
        case ValidationError::UnknownOrdStatus: return "UnknownOrdStatus";
        case ValidationError::MissingTradeFields: return "MissingTradeFields";
        case ValidationError::ZeroLastQty: return "ZeroLastQty";
        case ValidationError::MissingCancelRejectResponseTo:
            return "MissingCancelRejectResponseTo";
        case ValidationError::InconsistentQuantities: return "InconsistentQuantities";
        case ValidationError::ProhibitedCustomTag: return "ProhibitedCustomTag";
        case ValidationError::PayloadTooLarge: return "PayloadTooLarge";
    }
    return "Unknown";
}

std::string_view to_string(StoreError e) {
    switch (e) {
        case StoreError::None: return "None";
        case StoreError::Full: return "Full";
        case StoreError::IoError: return "IoError";
        case StoreError::Corrupt: return "Corrupt";
        case StoreError::Conflict: return "Conflict";
        case StoreError::NotOwner: return "NotOwner";
        case StoreError::SchemaMismatch: return "SchemaMismatch";
        case StoreError::Unavailable: return "Unavailable";
        case StoreError::CommitAckLost: return "CommitAckLost";
    }
    return "Unknown";
}

std::string_view to_string(HoldReason r) {
    switch (r) {
        case HoldReason::None: return "None";
        case HoldReason::AmbiguousSend: return "AmbiguousSend";
        case HoldReason::AckTimeout: return "AckTimeout";
        case HoldReason::ExecIdConflict: return "ExecIdConflict";
        case HoldReason::DuplicateExecIdChain: return "DuplicateExecIdChain";
        case HoldReason::OrphanReport: return "OrphanReport";
        case HoldReason::SecurityQuarantine: return "SecurityQuarantine";
        case HoldReason::IdentityConflict: return "IdentityConflict";
        case HoldReason::SemanticConflict: return "SemanticConflict";
        case HoldReason::SchemaViolation: return "SchemaViolation";
        case HoldReason::UnsupportedEvent: return "UnsupportedEvent";
        case HoldReason::SnapshotMismatch: return "SnapshotMismatch";
        case HoldReason::HistoryRequired: return "HistoryRequired";
        case HoldReason::CapabilityMissing: return "CapabilityMissing";
        case HoldReason::ReconBudgetExhausted: return "ReconBudgetExhausted";
        case HoldReason::StateRegression: return "StateRegression";
        case HoldReason::ReplaceAfterTerminal: return "ReplaceAfterTerminal";
        case HoldReason::PendingPolicy: return "PendingPolicy";
        case HoldReason::StoreFailClosed: return "StoreFailClosed";
        case HoldReason::OrphanQuotaExceeded: return "OrphanQuotaExceeded";
        case HoldReason::OperatorRequested: return "OperatorRequested";
    }
    return "Unknown";
}

}  // namespace router
