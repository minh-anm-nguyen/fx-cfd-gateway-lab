// Counters and a monotonic-bucket latency histogram (§18.2).
//
// Labels are deliberately low-cardinality: decision, reason and scope only.
// Per-order identifiers belong in the audit trail, which has retention and
// access control, not in a metric label where they would explode cardinality.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "risk/result.hpp"

namespace risk {

class Histogram {
 public:
  // Fixed exponential-ish buckets in microseconds, so two runs on the same
  // hardware produce comparable percentiles.
  Histogram();

  void observe_micros(std::int64_t micros);
  std::int64_t count() const;
  // Returns the upper bound of the bucket containing the requested quantile.
  std::int64_t quantile_micros(double q) const;
  std::vector<std::pair<std::int64_t, std::int64_t>> buckets() const;
  std::int64_t max_observed_micros() const;

 private:
  mutable std::mutex mu_;
  std::vector<std::int64_t> bounds_;
  std::vector<std::int64_t> counts_;  // one extra slot for the overflow bucket
  std::int64_t total_ = 0;
  std::int64_t max_ = 0;
};

class Metrics {
 public:
  void incr(const std::string& name, std::int64_t delta = 1);
  void incr_labeled(const std::string& name, const std::string& label,
                    std::int64_t delta = 1);
  void set_gauge(const std::string& name, std::int64_t value);

  std::int64_t counter(const std::string& name) const;
  std::int64_t labeled(const std::string& name, const std::string& label) const;
  std::int64_t gauge(const std::string& name) const;

  Histogram& admission_commit_seconds() { return admission_commit_; }
  const Histogram& admission_commit_seconds() const { return admission_commit_; }

  // Prometheus-ish text rendering for the CLI and for test artifacts.
  std::string render() const;

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::int64_t> counters_;
  std::map<std::string, std::int64_t> gauges_;
  Histogram admission_commit_;
};

// Metric names from §18.2, kept as constants so a typo is a compile error.
namespace metric {
inline constexpr const char* kAdmissionTotal = "risk_admission_total";
inline constexpr const char* kOutstandingUnits = "risk_outstanding_units";
inline constexpr const char* kUnknownOutcome = "risk_unknown_outcome_count";
inline constexpr const char* kReconciliationPending = "risk_reconciliation_pending_total";
inline constexpr const char* kLimitBreach = "risk_limit_breach_total";
inline constexpr const char* kQuoteStale = "risk_quote_stale_total";
inline constexpr const char* kFencingReject = "risk_fencing_reject_total";
inline constexpr const char* kQueueDepth = "risk_queue_depth";
inline constexpr const char* kEventDedup = "risk_event_dedup_total";
inline constexpr const char* kRestoreFailure = "risk_restore_failure_total";
inline constexpr const char* kRiskBudgetUsed = "risk_risk_budget_used_minor";
inline constexpr const char* kCommitOutcome = "risk_commit_outcome_total";
inline constexpr const char* kQuarantine = "risk_quarantine_total";
}  // namespace metric

}  // namespace risk
