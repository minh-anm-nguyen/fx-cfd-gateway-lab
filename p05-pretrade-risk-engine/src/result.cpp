#include "risk/result.hpp"

namespace risk {

std::string_view reason_name(Reason r) noexcept {
  switch (r) {
    case Reason::Ok: return "OK";

    case Reason::Unauthenticated: return "UNAUTHENTICATED";
    case Reason::Unauthorized: return "UNAUTHORIZED";
    case Reason::TenantMismatch: return "TENANT_MISMATCH";
    case Reason::AccountNotFound: return "ACCOUNT_NOT_FOUND";
    case Reason::MalformedIdentifier: return "MALFORMED_IDENTIFIER";
    case Reason::IdentifierTooLong: return "IDENTIFIER_TOO_LONG";
    case Reason::NonCanonicalPayload: return "NON_CANONICAL_PAYLOAD";
    case Reason::CommandIdConflict: return "COMMAND_ID_CONFLICT";
    case Reason::DuplicateCommand: return "DUPLICATE_COMMAND";
    case Reason::ChainAlreadyExists: return "CHAIN_ALREADY_EXISTS";
    case Reason::AccountDisabled: return "ACCOUNT_DISABLED";

    case Reason::UnknownInstrument: return "UNKNOWN_INSTRUMENT";
    case Reason::InstrumentDisabled: return "INSTRUMENT_DISABLED";
    case Reason::QtyBelowMin: return "QTY_BELOW_MIN";
    case Reason::QtyAboveMax: return "QTY_ABOVE_MAX";
    case Reason::QtyStepViolation: return "QTY_STEP_VIOLATION";
    case Reason::PriceTickViolation: return "PRICE_TICK_VIOLATION";
    case Reason::PriceNonPositive: return "PRICE_NON_POSITIVE";
    case Reason::UnsupportedOrderType: return "UNSUPPORTED_ORDER_TYPE";
    case Reason::UnsupportedTimeInForce: return "UNSUPPORTED_TIME_IN_FORCE";
    case Reason::UnsupportedProductMode: return "UNSUPPORTED_PRODUCT_MODE";

    case Reason::QuoteMissing: return "QUOTE_MISSING";
    case Reason::QuoteStale: return "QUOTE_STALE";
    case Reason::QuoteCrossed: return "QUOTE_CROSSED";
    case Reason::QuoteSequenceRegression: return "QUOTE_SEQUENCE_REGRESSION";
    case Reason::QuoteSourceUntrusted: return "QUOTE_SOURCE_UNTRUSTED";
    case Reason::FxMissing: return "FX_MISSING";
    case Reason::FxStale: return "FX_STALE";
    case Reason::FxPathAmbiguous: return "FX_PATH_AMBIGUOUS";
    case Reason::FxPathMissing: return "FX_PATH_MISSING";
    case Reason::FxNonPositive: return "FX_NON_POSITIVE";
    case Reason::EquityMissing: return "EQUITY_MISSING";
    case Reason::EquityStale: return "EQUITY_STALE";
    case Reason::EquityCurrencyMismatch: return "EQUITY_CURRENCY_MISMATCH";
    case Reason::ClockRollback: return "CLOCK_ROLLBACK";

    case Reason::ArithmeticOverflow: return "ARITHMETIC_OVERFLOW";
    case Reason::DivisionByZero: return "DIVISION_BY_ZERO";
    case Reason::NegativeRiskCharge: return "NEGATIVE_RISK_CHARGE";
    case Reason::ScaleConversionUnsupported: return "SCALE_CONVERSION_UNSUPPORTED";

    case Reason::LimitOrderNotional: return "LIMIT_ORDER_NOTIONAL";
    case Reason::LimitOrderQty: return "LIMIT_ORDER_QTY";
    case Reason::LimitPriceCollar: return "LIMIT_PRICE_COLLAR";
    case Reason::LimitChainQty: return "LIMIT_CHAIN_QTY";
    case Reason::LimitAccountSymbolNet: return "LIMIT_ACCOUNT_SYMBOL_NET";
    case Reason::LimitAccountSymbolGross: return "LIMIT_ACCOUNT_SYMBOL_GROSS";
    case Reason::LimitAccountWorkingCount: return "LIMIT_ACCOUNT_WORKING_COUNT";
    case Reason::LimitAccountTotalGross: return "LIMIT_ACCOUNT_TOTAL_GROSS";
    case Reason::LimitFirmGross: return "LIMIT_FIRM_GROSS";
    case Reason::LimitMarginBuffer: return "LIMIT_MARGIN_BUFFER";
    case Reason::LimitRate: return "LIMIT_RATE";
    case Reason::LimitBreached: return "LIMIT_BREACHED";

    case Reason::KillSwitchNoNew: return "KILL_SWITCH_NO_NEW";
    case Reason::KillSwitchCancelOnly: return "KILL_SWITCH_CANCEL_ONLY";
    case Reason::KillSwitchReconOnly: return "KILL_SWITCH_RECON_ONLY";
    case Reason::PersistenceCommitFailed: return "PERSISTENCE_COMMIT_FAILED";
    case Reason::PersistenceCommitUnknown: return "PERSISTENCE_COMMIT_UNKNOWN";
    case Reason::PersistenceUnavailable: return "PERSISTENCE_UNAVAILABLE";
    case Reason::FencingTokenStale: return "FENCING_TOKEN_STALE";
    case Reason::RevisionCasFailed: return "REVISION_CAS_FAILED";
    case Reason::OutboxFull: return "OUTBOX_FULL";
    case Reason::AuditUnavailable: return "AUDIT_UNAVAILABLE";

    case Reason::UnknownChain: return "UNKNOWN_CHAIN";
    case Reason::ReservationNotFound: return "RESERVATION_NOT_FOUND";
    case Reason::ReservationTerminal: return "RESERVATION_TERMINAL";
    case Reason::DuplicateEvent: return "DUPLICATE_EVENT";
    case Reason::EventHashMismatch: return "EVENT_HASH_MISMATCH";
    case Reason::FillExceedsAccepted: return "FILL_EXCEEDS_ACCEPTED";
    case Reason::FillSideMismatch: return "FILL_SIDE_MISMATCH";
    case Reason::FillSymbolMismatch: return "FILL_SYMBOL_MISMATCH";
    case Reason::PendingModifyBusy: return "PENDING_MODIFY_BUSY";
    case Reason::ReplaceBelowCumulative: return "REPLACE_BELOW_CUMULATIVE";
    case Reason::ReplaceNoAcceptedVersion: return "REPLACE_NO_ACCEPTED_VERSION";
    case Reason::SyntheticFillRefused: return "SYNTHETIC_FILL_REFUSED";
    case Reason::TradeCorrectUnsupported: return "TRADE_CORRECT_UNSUPPORTED";
    case Reason::CancelNotTerminal: return "CANCEL_NOT_TERMINAL";
    case Reason::CumulativeMismatch: return "CUMULATIVE_MISMATCH";
    case Reason::ReplaceGrantExceedsReservation:
      return "REPLACE_GRANT_EXCEEDS_RESERVATION";

    case Reason::SerializationConflict: return "SERIALIZATION_CONFLICT";
    case Reason::WriterNotOwner: return "WRITER_NOT_OWNER";
    case Reason::RecoveryIncomplete: return "RECOVERY_INCOMPLETE";
    case Reason::JournalCorrupt: return "JOURNAL_CORRUPT";
    case Reason::SnapshotMismatch: return "SNAPSHOT_MISMATCH";
    case Reason::ReconciliationRequired: return "RECONCILIATION_REQUIRED";
    case Reason::Quarantined: return "QUARANTINED";
    case Reason::EvidenceInsufficient: return "EVIDENCE_INSUFFICIENT";

    case Reason::PolicyInvalid: return "POLICY_INVALID";
    case Reason::PolicyVersionRegression: return "POLICY_VERSION_REGRESSION";
    case Reason::ApprovalRequired: return "APPROVAL_REQUIRED";
    case Reason::QueueFull: return "QUEUE_FULL";
    case Reason::BacklogFull: return "BACKLOG_FULL";
    case Reason::ResourceCapExceeded: return "RESOURCE_CAP_EXCEEDED";
    case Reason::Unsupported: return "UNSUPPORTED";
    case Reason::InternalInvariant: return "INTERNAL_INVARIANT";
  }
  return "UNKNOWN_REASON";
}

bool reason_is_retryable(Reason r) noexcept {
  switch (r) {
    // "We could not prove the outcome." These must surface as HOLD, never as a
    // definitive denial, because the command may already be at the LP.
    case Reason::PersistenceCommitUnknown:
    case Reason::PersistenceUnavailable:
    case Reason::PersistenceCommitFailed:
    case Reason::SerializationConflict:
    case Reason::RevisionCasFailed:
    case Reason::AuditUnavailable:
    case Reason::QueueFull:
    case Reason::OutboxFull:
    case Reason::RecoveryIncomplete:
    case Reason::ReconciliationRequired:
    // A fenced writer is not the owner any more, so it cannot speak
    // definitively about this command: another owner may already have decided
    // it. Nothing was written here, but the answer still has to be a hold
    // rather than a business denial (§18.3 SPLIT_BRAIN).
    case Reason::FencingTokenStale:
    case Reason::WriterNotOwner:
      return true;
    default:
      return false;
  }
}

}  // namespace risk
