// Configuration, and the refusals that go with it.
//
// Section 21 gives the recovery.yaml shape and section 23's G23 group asks
// what happens when configuration drifts between environments. The answer
// this file implements: a configuration that would make a safety claim
// unprovable is refused at startup, not accepted with a warning.
//
// The parser accepts the two-level subset of YAML that the doc's example
// uses. It is deliberately strict — an unknown key is an error, because a
// typo in a durability setting that silently keeps the default is exactly
// the drift G23 is about.
#pragma once

#include <string>
#include <vector>

#include "jrnl/result.hpp"

namespace jrnl {

struct RecoveryConfig {
    struct Storage {
        int pg_version_major = 17;
        bool require_fsync = true;
        bool require_full_page_writes = true;
        std::string require_synchronous_commit = "on";
        int max_transaction_retries = 5;
        int statement_timeout_ms = 2000;
        int lock_timeout_ms = 500;
    } storage;

    struct Recovery {
        bool dispatch_default_enabled = false;
        bool verify_journal_hashes = true;
        bool verify_projections = true;
        bool verify_outbox_receipts = true;
        int max_replay_batch_events = 1000;
        std::string on_unknown_lp_outcome = "hold_and_reconcile";
        std::string on_data_mismatch = "quarantine";
    } recovery;

    struct Outbox {
        int claim_limit = 100;
        bool safe_retry_requires_p01_stable_send = true;
        std::string default_ambiguous_action = "unknown";
    } outbox;

    struct Backup {
        bool require_restore_drill = true;
        bool block_unsafe_wal_pruning = true;
    } backup;
};

struct ConfigProblem {
    std::string key;
    std::string why;
};

// Parses the file. Returns false with the reason on any syntax error or
// unknown key.
bool load_config(const std::string& path, RecoveryConfig* out, std::string* why);
bool parse_config(const std::string& text, RecoveryConfig* out, std::string* why);

// The refusals. Each entry is a setting that would let the engine claim a
// durability or safety property it cannot demonstrate.
std::vector<ConfigProblem> validate_config(const RecoveryConfig& c);

}  // namespace jrnl
