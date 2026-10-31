// Counters and gauges from section 22.
//
// Header-only and deliberately dull. The value of this file is that the
// names in the alert table exist as symbols, so a test can assert that an
// injected fault actually incremented the counter an on-call engineer would
// be paged on, rather than only that the engine behaved correctly in
// silence.
//
// Section 22 also forbids putting credentials or whole FIX payloads in
// metric labels, so nothing here takes a free-form label: a counter is a
// name and a number.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace jrnl {

class Metrics {
public:
    void inc(std::string_view name, std::int64_t by = 1) {
        std::lock_guard<std::mutex> g(mu_);
        counters_[std::string(name)] += by;
    }
    void set(std::string_view name, std::int64_t value) {
        std::lock_guard<std::mutex> g(mu_);
        gauges_[std::string(name)] = value;
    }
    std::int64_t get(std::string_view name) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto c = counters_.find(std::string(name));
        if (c != counters_.end()) return c->second;
        const auto gg = gauges_.find(std::string(name));
        return gg == gauges_.end() ? 0 : gg->second;
    }
    std::vector<std::pair<std::string, std::int64_t>> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::pair<std::string, std::int64_t>> out;
        for (const auto& e : counters_) out.push_back(e);
        for (const auto& e : gauges_) out.push_back(e);
        return out;
    }
    void reset() {
        std::lock_guard<std::mutex> g(mu_);
        counters_.clear();
        gauges_.clear();
    }

    static Metrics& instance() {
        static Metrics m;
        return m;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, std::int64_t> counters_;
    std::map<std::string, std::int64_t> gauges_;
};

// The names in the section 22 table. Spelled once here so an alert rule and
// the code that fires it cannot drift apart.
namespace metric {
constexpr std::string_view kJournalAppendFailures = "journal_append_failures_total";
constexpr std::string_view kDbCommitUnknown = "db_commit_unknown_total";
constexpr std::string_view kRecoveryIntegrityFail = "recovery_integrity_fail_total";
constexpr std::string_view kJournalVersionGap = "journal_version_gap_total";
constexpr std::string_view kProjectionReplayMismatch = "projection_replay_mismatch_total";
constexpr std::string_view kOutboxUnknownCount = "outbox_unknown_count";
constexpr std::string_view kOutboxOldestAge = "outbox_oldest_age";
constexpr std::string_view kReceiptRedelivery = "receipt_redelivery_total";
constexpr std::string_view kReceiptUnackedAge = "receipt_unacked_age";
constexpr std::string_view kOwnerFenceRejections = "owner_fence_rejections_total";
constexpr std::string_view kBackupRestoreDrillLastSuccess = "backup_restore_drill_last_success";
constexpr std::string_view kRiskReservationLedgerDiff = "risk_reservation_ledger_diff";
constexpr std::string_view kDeadlockTotal = "deadlock_total";
constexpr std::string_view kSerializationRetries = "serialization_retries";
constexpr std::string_view kSnapshotChecksumFailure = "snapshot_checksum_failure";
constexpr std::string_view kQuarantineTotal = "quarantine_total";
constexpr std::string_view kAdmissionBlocked = "admission_blocked_total";
constexpr std::string_view kBackpressureRejections = "backpressure_rejections_total";
}  // namespace metric

}  // namespace jrnl
