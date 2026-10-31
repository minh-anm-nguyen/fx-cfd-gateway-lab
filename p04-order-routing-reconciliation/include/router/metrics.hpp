// Counters and latency histograms with a hard cardinality bound.
//
// Design doc 17.2 names the metrics this router must expose and then adds the
// rule that matters: labels are drawn from small enumerations, never from a
// ClOrdID or an account. OR-Q-11 pushes a million unique order IDs through and
// expects the label space to stay bounded, so the registry refuses an unknown
// label value instead of trusting the caller.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace router {

class Metrics {
public:
    // Registers the set of values a label may take. A value outside the set is
    // folded into "other" and counted separately, which keeps a hostile peer or
    // a buggy call site from growing the series set without bound.
    void allow_label(const std::string& metric, const std::vector<std::string>& values);

    void inc(const std::string& metric, std::uint64_t by = 1);
    void inc(const std::string& metric, const std::string& label, std::uint64_t by = 1);

    void set_gauge(const std::string& metric, std::int64_t value);
    void observe(const std::string& metric, std::uint64_t micros);

    std::uint64_t counter(const std::string& metric) const;
    std::uint64_t counter(const std::string& metric, const std::string& label) const;
    std::int64_t gauge(const std::string& metric) const;

    std::size_t series_count() const;
    std::uint64_t rejected_labels() const { return rejected_labels_; }

    struct Percentiles {
        std::uint64_t count = 0;
        std::uint64_t p50 = 0, p95 = 0, p99 = 0, max = 0;
    };
    Percentiles percentiles(const std::string& metric) const;

    // Stable text rendering used by the evidence artifacts.
    std::string render() const;

    const std::map<std::string, std::uint64_t>& all_counters() const { return counters_; }

private:
    static std::string key(const std::string& metric, const std::string& label);

    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, std::int64_t> gauges_;
    std::map<std::string, std::vector<std::uint64_t>> histograms_;
    std::map<std::string, std::vector<std::string>> allowed_;
    std::uint64_t rejected_labels_ = 0;
};

// Metric names, kept in one place so a typo cannot create a second series.
namespace metric {
inline constexpr const char* kCommandReceived = "router_command_received_total";
inline constexpr const char* kCommandRejected = "router_command_rejected_total";
inline constexpr const char* kCommandDuplicate = "router_command_duplicate_total";
inline constexpr const char* kOutboxDepth = "router_outbox_depth";
inline constexpr const char* kOutboxOldestAge = "router_outbox_oldest_age_seconds";
inline constexpr const char* kOriginalSendTotal = "router_original_send_total";
inline constexpr const char* kUnknownOutcomeOpen = "router_unknown_outcome_open";
inline constexpr const char* kReconOpen = "router_reconciliation_open";
inline constexpr const char* kOrphanReports = "router_orphan_reports_total";
inline constexpr const char* kDuplicateExec = "router_duplicate_exec_total";
inline constexpr const char* kExecIdConflicts = "router_execid_conflicts_total";
inline constexpr const char* kBusinessReject = "router_fix_business_reject_total";
inline constexpr const char* kInboundCommitLatency = "router_inbound_commit_latency_us";
inline constexpr const char* kCommandToIntentLatency = "router_command_to_send_intent_us";
inline constexpr const char* kQuarantineDepth = "router_quarantine_depth";
inline constexpr const char* kQuarantineAge = "router_quarantine_age_seconds";
inline constexpr const char* kStatusQueriesInFlight = "router_status_queries_in_flight";
inline constexpr const char* kStatusQueriesTotal = "router_status_queries_total";
inline constexpr const char* kReplayLag = "router_replay_lag";
inline constexpr const char* kHoldTotal = "router_hold_total";
inline constexpr const char* kLogSuppressed = "router_log_suppressed_total";
}  // namespace metric

}  // namespace router
