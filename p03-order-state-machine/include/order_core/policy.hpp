// Pinned policy: symbol scales, Rules of Engagement switches and resource
// ceilings.
//
// A policy is immutable once an order version references it. Reloading config
// while an order is working must not retroactively change how that order's
// price was interpreted (what-if P-01, P-02, D-11), so every accepted version
// records the policy_version and scale_id it was admitted under.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace order_core {

// What to do with an event class the v1 profile does not implement. The design
// doc is explicit that "unsupported" must fail closed, never degrade to a
// silent no-op (ADR-06).
enum class UnsupportedPolicy { Quarantine, Reject };

struct SymbolSpec {
    // One tick equals tick_numerator / tick_denominator of a price unit.
    int64_t tick_numerator = 1;
    int64_t tick_denominator = 100000;
    // Admissible quantity granularity in base units.
    int64_t qty_step = 1;
    int64_t min_qty = 0;
    int64_t max_qty = 0;  // 0 means "no explicit ceiling beyond int64 checks"
    // Identifies the scale an accepted version was pinned to. Two configs with
    // different ticks must not share a scale_id, otherwise a reloaded config
    // could be mistaken for the one an order was accepted under.
    uint32_t scale_id = 0;
    bool price_must_be_positive = true;
};

struct Limits {
    size_t max_chain_count = 100000;
    size_t max_queue_events = 65536;
    size_t max_pending_per_chain = 64;
    size_t max_pending_mutations_per_chain = 1;
    size_t max_exec_id_bytes = 128;
    size_t max_cl_ord_id_bytes = 64;
    size_t max_event_payload_bytes = 64 * 1024;
    size_t max_text_bytes = 256;
    size_t max_idempotency_entries = 1000000;
    size_t max_accepted_versions = 256;
    size_t max_reconcile_attempts = 5;
    size_t max_quarantine_incidents = 10000;
    size_t max_held_events_per_chain = 16;
};

enum class ConfigErrorCode {
    None,
    DuplicateSymbol,
    ZeroQtyStep,
    BadTick,
    TickOverflow,
    DuplicateScaleId,
    EmptyNamespace,
    ZeroLimit,
    RetentionBelowResendHorizon,
    AuditRetentionBelowDedup,
    CorrectionsEnabledWithoutMigration,
};

const char* to_string(ConfigErrorCode c);

struct ConfigError {
    ConfigErrorCode code = ConfigErrorCode::None;
    std::string detail;
    [[nodiscard]] bool ok() const { return code == ConfigErrorCode::None; }
};

struct Policy {
    // Identity scope. ClOrdID uniqueness is (firm, business_day, ClOrdID); the
    // account deliberately does not widen it in the strict v1 profile
    // (what-if E-02, M-03).
    std::string business_namespace = "FIRM-DEMO:2026-10-30";
    std::string peer_id = "LP1";
    uint32_t policy_version = 1;
    uint32_t event_schema_version = 1;

    // Rules of Engagement switches.
    bool reject_qty_below_cum_on_replace = true;
    bool reject_replace_qty_equal_cum = true;   // zero working leaves, what-if I-05
    bool allow_optimistic_chaining = false;     // replace on an unaccepted version
    bool allow_cross_version_alias = false;
    bool normalize_cl_ord_id_case = false;      // what-if E-19: no silent casefold
    bool enforce_limit_price_on_fill = true;    // what-if D-14, D-15
    UnsupportedPolicy trade_corrections = UnsupportedPolicy::Quarantine;
    UnsupportedPolicy unknown_exec_type = UnsupportedPolicy::Quarantine;
    bool status_snapshot_may_create_fill = false;  // never true in v1 (ADR-07)
    bool corrections_engine_v2_enabled = false;
    bool corrections_schema_migrated = false;
    bool terminal_leaves_is_zero = true;        // invariant I07 profile choice

    // Retention, in whatever unit the operator counts resend horizons.
    uint64_t dedup_retention_units = 100;
    uint64_t fix_resend_horizon_units = 50;
    uint64_t audit_retention_units = 200;

    Limits limits{};
    std::map<std::string, SymbolSpec> symbols;

    // Rejects a configuration that would make trading unsafe before any order
    // is accepted, instead of discovering it mid-session (group P).
    [[nodiscard]] ConfigError validate() const;

    [[nodiscard]] const SymbolSpec* find_symbol(const std::string& symbol) const;
};

// The lab default used by tests and the CLI: EURUSD and USDJPY at distinct
// scales so cross-symbol scale reuse would be visible (what-if D-12).
Policy default_lab_policy();

}  // namespace order_core
