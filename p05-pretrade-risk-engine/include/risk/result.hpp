// P05 — FX/CFD Pre-trade Risk & Exposure Reservation Engine
// Typed reason codes and a minimal Result<T>. Every denial/hold in the engine
// must carry one of these codes so audit records stay machine-readable
// (RI-30: versioned, reasoned decisions).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace risk {

// Reason codes. The numeric values are part of the audit contract: they are
// serialized into the journal, so append new codes at the end of a group and
// never renumber an existing one.
enum class Reason : int {
  Ok = 0,

  // --- G01 identity / authz / canonicalization -----------------------------
  Unauthenticated = 100,
  Unauthorized = 101,
  TenantMismatch = 102,
  AccountNotFound = 103,
  MalformedIdentifier = 104,
  IdentifierTooLong = 105,
  NonCanonicalPayload = 106,
  CommandIdConflict = 107,
  DuplicateCommand = 108,
  ChainAlreadyExists = 109,
  AccountDisabled = 110,

  // --- G02 instrument metadata --------------------------------------------
  UnknownInstrument = 200,
  InstrumentDisabled = 201,
  QtyBelowMin = 202,
  QtyAboveMax = 203,
  QtyStepViolation = 204,
  PriceTickViolation = 205,
  PriceNonPositive = 206,
  UnsupportedOrderType = 207,
  UnsupportedTimeInForce = 208,
  UnsupportedProductMode = 209,

  // --- G03/G04 market data -------------------------------------------------
  QuoteMissing = 300,
  QuoteStale = 301,
  QuoteCrossed = 302,
  QuoteSequenceRegression = 303,
  QuoteSourceUntrusted = 304,
  FxMissing = 310,
  FxStale = 311,
  FxPathAmbiguous = 312,
  FxPathMissing = 313,
  FxNonPositive = 314,
  EquityMissing = 320,
  EquityStale = 321,
  EquityCurrencyMismatch = 322,
  ClockRollback = 330,

  // --- G05 arithmetic ------------------------------------------------------
  ArithmeticOverflow = 400,
  DivisionByZero = 401,
  NegativeRiskCharge = 402,
  ScaleConversionUnsupported = 403,

  // --- G06 limits ----------------------------------------------------------
  LimitOrderNotional = 500,
  LimitOrderQty = 501,
  LimitPriceCollar = 502,
  LimitChainQty = 503,
  LimitAccountSymbolNet = 504,
  LimitAccountSymbolGross = 505,
  LimitAccountWorkingCount = 506,
  LimitAccountTotalGross = 507,
  LimitFirmGross = 508,
  LimitMarginBuffer = 509,
  LimitRate = 510,
  LimitBreached = 511,

  // --- G07/G08 admission / durability -------------------------------------
  KillSwitchNoNew = 600,
  KillSwitchCancelOnly = 601,
  KillSwitchReconOnly = 602,
  PersistenceCommitFailed = 610,
  PersistenceCommitUnknown = 611,
  PersistenceUnavailable = 612,
  FencingTokenStale = 613,
  RevisionCasFailed = 614,
  OutboxFull = 615,
  AuditUnavailable = 616,

  // --- G09..G11 lifecycle --------------------------------------------------
  UnknownChain = 700,
  ReservationNotFound = 701,
  ReservationTerminal = 702,
  DuplicateEvent = 703,
  EventHashMismatch = 704,
  FillExceedsAccepted = 705,
  FillSideMismatch = 706,
  FillSymbolMismatch = 707,
  PendingModifyBusy = 708,
  ReplaceBelowCumulative = 709,
  ReplaceNoAcceptedVersion = 710,
  SyntheticFillRefused = 711,
  TradeCorrectUnsupported = 712,
  CancelNotTerminal = 713,
  CumulativeMismatch = 714,
  ReplaceGrantExceedsReservation = 715,

  // --- G12..G14 concurrency / recovery ------------------------------------
  SerializationConflict = 800,
  WriterNotOwner = 801,
  RecoveryIncomplete = 802,
  JournalCorrupt = 803,
  SnapshotMismatch = 804,
  ReconciliationRequired = 805,
  Quarantined = 806,
  EvidenceInsufficient = 807,

  // --- G16..G20 ops --------------------------------------------------------
  PolicyInvalid = 900,
  PolicyVersionRegression = 901,
  ApprovalRequired = 902,
  QueueFull = 903,
  BacklogFull = 904,
  ResourceCapExceeded = 905,
  Unsupported = 906,
  InternalInvariant = 999,
};

std::string_view reason_name(Reason r) noexcept;

// True when the reason denotes "we could not prove the outcome" rather than
// "we proved this must be refused". Holds must never be downgraded to denials
// (RI-02 note in the spec, §2.3).
bool reason_is_retryable(Reason r) noexcept;

// Minimal expected-like container. Deliberately not std::expected: the engine
// targets C++20 and must build on the lab toolchain without libstdc++ extras.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), reason_(Reason::Ok) {}
  static Result fail(Reason r, std::string detail = {}) {
    Result out{};
    out.reason_ = r;
    out.detail_ = std::move(detail);
    return out;
  }

  bool ok() const noexcept { return reason_ == Reason::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  Reason reason() const noexcept { return reason_; }
  const std::string& detail() const noexcept { return detail_; }

  const T& value() const noexcept { return *value_; }
  T& value() noexcept { return *value_; }
  T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

 private:
  Result() = default;
  std::optional<T> value_;
  Reason reason_ = Reason::Ok;
  std::string detail_;
};

// Void-flavoured status for operations with no payload.
class Status {
 public:
  Status() = default;
  static Status success() { return Status{}; }
  static Status fail(Reason r, std::string detail = {}) {
    Status s;
    s.reason_ = r;
    s.detail_ = std::move(detail);
    return s;
  }

  bool ok() const noexcept { return reason_ == Reason::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  Reason reason() const noexcept { return reason_; }
  const std::string& detail() const noexcept { return detail_; }

 private:
  Reason reason_ = Reason::Ok;
  std::string detail_;
};

}  // namespace risk
