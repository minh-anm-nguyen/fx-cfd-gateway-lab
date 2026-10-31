// Metric registry. Names follow design doc 14.1 so dashboards and alerts in the
// doc map one to one onto what the engine emits.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace fixse {

class Metrics {
public:
    void inc(std::string_view name, uint64_t by = 1) { counters_[std::string(name)] += by; }
    void set(std::string_view name, int64_t v) { gauges_[std::string(name)] = v; }
    void max_of(std::string_view name, int64_t v) {
        auto& g = gauges_[std::string(name)];
        if (v > g) g = v;
    }

    uint64_t counter(std::string_view name) const {
        const auto it = counters_.find(std::string(name));
        return it == counters_.end() ? 0 : it->second;
    }
    int64_t gauge(std::string_view name) const {
        const auto it = gauges_.find(std::string(name));
        return it == gauges_.end() ? 0 : it->second;
    }

    // Prometheus style text, for the admin endpoint and test artifacts.
    std::string render() const;

    const std::map<std::string, uint64_t>& counters() const { return counters_; }
    const std::map<std::string, int64_t>& gauges() const { return gauges_; }

private:
    std::map<std::string, uint64_t> counters_;
    std::map<std::string, int64_t> gauges_;
};

namespace metric {
constexpr std::string_view kState = "fix_session_state";
constexpr std::string_view kConnected = "fix_connected";
constexpr std::string_view kLogonAttempts = "fix_logon_attempts_total";
constexpr std::string_view kLogonRejects = "fix_logon_rejects_total";
constexpr std::string_view kReconnects = "fix_reconnects_total";
constexpr std::string_view kNextIn = "fix_next_in";
constexpr std::string_view kNextOut = "fix_next_out";
constexpr std::string_view kGapDetected = "fix_gap_detected_total";
constexpr std::string_view kResendRequestsSent = "fix_resend_requests_sent_total";
constexpr std::string_view kResendMessages = "fix_resend_messages_total";
constexpr std::string_view kGapFillRanges = "fix_gapfill_ranges_total";
constexpr std::string_view kDuplicateInbound = "fix_duplicate_inbound_total";
constexpr std::string_view kUnresolvedGaps = "fix_unresolved_gaps";
constexpr std::string_view kHeartbeatsSent = "fix_heartbeat_sent_total";
constexpr std::string_view kTestRequests = "fix_test_requests_total";
constexpr std::string_view kTestRequestTimeouts = "fix_test_request_timeouts_total";
constexpr std::string_view kStoreErrors = "fix_store_errors_total";
constexpr std::string_view kInboxPending = "fix_inbox_pending_messages";
constexpr std::string_view kRxBufferBytes = "fix_rx_buffer_bytes";
constexpr std::string_view kRecoveryBufferMessages = "fix_recovery_buffer_messages";
constexpr std::string_view kMalformedFrames = "fix_malformed_frames_total";
constexpr std::string_view kSessionRejectsSent = "fix_session_rejects_sent_total";
constexpr std::string_view kAppDelivered = "fix_app_delivered_total";
constexpr std::string_view kAppSubmitRejected = "fix_app_submit_rejected_total";
constexpr std::string_view kFaults = "fix_faults_total";
constexpr std::string_view kBytesWritten = "fix_bytes_written_total";
constexpr std::string_view kPartialWrites = "fix_partial_writes_total";
}  // namespace metric

}  // namespace fixse
