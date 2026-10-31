// Metrics and audit records.
//
// Reference: design doc 15.1 (metrics) and 15.2 (structured audit fields).
//
// Two constraints from the matrix shape this:
//
//   L-11  collecting metrics must not be able to block the business actor, so
//         every counter here is a plain in-process value and nothing in this
//         header performs I/O or takes a lock that the actor waits on;
//   L-13  labels must be bounded. A counter keyed by ClOrdID would grow one
//         series per order, so the only label dimensions allowed are the small
//         closed sets below (reject reason, symbol, report type).
//
// L-07 governs the audit record: a password or a raw frame that might contain
// one never reaches a log line. redact() is the single place that rule lives.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "lp/domain.hpp"
#include "lp/result.hpp"

namespace lpsim {

struct Metrics {
    // Session.
    std::uint64_t sessions_active = 0;
    std::uint64_t logon_rejected_total = 0;
    std::uint64_t resend_requests_total = 0;
    std::uint64_t duplicate_fix_frames_total = 0;
    std::uint64_t sequence_divergence_total = 0;

    // Business.
    std::uint64_t orders_accepted_total = 0;
    std::uint64_t fills_total = 0;
    Units filled_qty_units_total = 0;
    std::uint64_t duplicate_clordid_total = 0;
    std::map<std::string, std::uint64_t> orders_rejected_total;  // by reason name
    std::map<std::string, std::uint64_t> cancel_reject_total;
    std::map<std::string, std::uint64_t> replace_reject_total;

    // State and integrity.
    std::uint64_t live_orders = 0;
    Units fills_ledger_qty_total = 0;
    std::uint64_t invariant_violations_total = 0;
    std::map<std::string, Units> quote_inventory_units;  // by symbol
    std::uint64_t state_recovery_failures_total = 0;

    // Durability.
    std::uint64_t db_errors_total = 0;
    std::uint64_t outbox_pending = 0;
    std::uint64_t send_once_conflicts_total = 0;

    // Resources.
    std::uint64_t actor_queue_depth = 0;
    std::uint64_t scheduled_event_queue_depth = 0;
    std::int64_t scenario_elapsed_virtual_ms = 0;

    void count_order_reject(BizReject r) { ++orders_rejected_total[std::string(to_string(r))]; }
    void count_cancel_reject(BizReject r) { ++cancel_reject_total[std::string(to_string(r))]; }
    void count_replace_reject(BizReject r) {
        ++replace_reject_total[std::string(to_string(r))];
    }

    // Prometheus-style text, for a lab endpoint or a test artifact.
    std::string render() const;
};

// Structured audit record. Every field is one the design doc names in 15.2, so
// a trace can be joined across the session and business layers.
struct AuditRecord {
    std::string timestamp_utc;
    std::int64_t virtual_time_us = 0;
    std::string run_id;
    std::uint64_t seed = 0;
    std::string session_id;
    std::uint64_t epoch = 0;
    std::uint64_t connection_generation = 0;
    std::uint64_t inbound_seq = 0;
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string order_id;
    std::string exec_id;
    std::string event_id;
    std::uint64_t quote_version = 0;
    std::uint64_t order_version = 0;
    Units cum_qty = 0;
    Units leaves_qty = 0;
    std::uint64_t outbox_seq = 0;
    std::string fault_id;
    std::string decision_reason;

    std::string render() const;
};

// Remove anything that could carry a secret from a string destined for a log,
// a wire capture or a test artifact.
//
// The rule is deliberately crude: redact the VALUE of any sensitive tag rather
// than trying to recognise a password by shape. Tag 554 is the one FIX field
// that definitely holds one, and 553 is redacted alongside it because a
// username is still an identity. Scenario L-07 greps artifacts for the fixture
// password, so a near miss here is a visible failure, not a silent one.
std::string redact(std::string_view text);

// Sensitive tags whose values never appear in an artifact.
bool is_sensitive_tag(int tag_num);

}  // namespace lpsim
