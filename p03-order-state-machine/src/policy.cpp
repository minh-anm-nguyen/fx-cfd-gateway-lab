#include "order_core/policy.hpp"

#include <set>

#include "order_core/math.hpp"

namespace order_core {

const char* to_string(ConfigErrorCode c) {
    switch (c) {
        case ConfigErrorCode::None: return "None";
        case ConfigErrorCode::DuplicateSymbol: return "DuplicateSymbol";
        case ConfigErrorCode::ZeroQtyStep: return "ZeroQtyStep";
        case ConfigErrorCode::BadTick: return "BadTick";
        case ConfigErrorCode::TickOverflow: return "TickOverflow";
        case ConfigErrorCode::DuplicateScaleId: return "DuplicateScaleId";
        case ConfigErrorCode::EmptyNamespace: return "EmptyNamespace";
        case ConfigErrorCode::ZeroLimit: return "ZeroLimit";
        case ConfigErrorCode::RetentionBelowResendHorizon:
            return "RetentionBelowResendHorizon";
        case ConfigErrorCode::AuditRetentionBelowDedup: return "AuditRetentionBelowDedup";
        case ConfigErrorCode::CorrectionsEnabledWithoutMigration:
            return "CorrectionsEnabledWithoutMigration";
    }
    return "Unknown";
}

ConfigError Policy::validate() const {
    if (business_namespace.empty()) {
        return {ConfigErrorCode::EmptyNamespace, "business_namespace must be set"};
    }

    std::set<uint32_t> scale_ids;
    for (const auto& [symbol, spec] : symbols) {
        if (spec.qty_step <= 0) {
            return {ConfigErrorCode::ZeroQtyStep, symbol + ": qty_step must be > 0"};
        }
        if (spec.tick_numerator <= 0 || spec.tick_denominator <= 0) {
            return {ConfigErrorCode::BadTick,
                    symbol + ": tick numerator and denominator must be > 0"};
        }
        // A tick finer than a trillionth of a price unit cannot produce a tick
        // count that fits in int64 for any realistic price, so every order on
        // the symbol would be rejected at parse time. Refuse the configuration
        // instead of discovering that per order (what-if P-14).
        constexpr int64_t kMaxTickDenominator = 1000000000000LL;
        if (spec.tick_denominator > kMaxTickDenominator) {
            return {ConfigErrorCode::TickOverflow,
                    symbol + ": tick_denominator " + std::to_string(spec.tick_denominator) +
                        " exceeds " + std::to_string(kMaxTickDenominator)};
        }
        Int128 probe = 0;
        if (!checked_mul_wide(spec.tick_denominator, 1000000000000000LL, probe)) {
            return {ConfigErrorCode::TickOverflow, symbol + ": tick_denominator too large"};
        }
        if (spec.max_qty < 0 || spec.min_qty < 0) {
            return {ConfigErrorCode::ZeroLimit, symbol + ": negative qty bound"};
        }
        if (spec.max_qty != 0 && spec.max_qty < spec.min_qty) {
            return {ConfigErrorCode::ZeroLimit, symbol + ": max_qty below min_qty"};
        }
        if (!scale_ids.insert(spec.scale_id).second) {
            // Two symbols sharing a scale_id would let a price accepted under
            // one be reinterpreted under the other.
            return {ConfigErrorCode::DuplicateScaleId,
                    symbol + ": scale_id already used by another symbol"};
        }
    }

    if (limits.max_chain_count == 0 || limits.max_queue_events == 0 ||
        limits.max_pending_per_chain == 0 || limits.max_exec_id_bytes == 0 ||
        limits.max_cl_ord_id_bytes == 0 || limits.max_event_payload_bytes == 0 ||
        limits.max_accepted_versions == 0 || limits.max_held_events_per_chain == 0) {
        return {ConfigErrorCode::ZeroLimit, "a resource limit is zero"};
    }
    if (limits.max_pending_mutations_per_chain == 0) {
        return {ConfigErrorCode::ZeroLimit, "max_pending_mutations_per_chain is zero"};
    }

    // Dedup identities must outlive every window in which a peer may resend,
    // otherwise eviction reopens the door to a double fill (what-if F-17, P-16).
    if (dedup_retention_units < fix_resend_horizon_units) {
        return {ConfigErrorCode::RetentionBelowResendHorizon,
                "dedup_retention_units below fix_resend_horizon_units"};
    }
    // An incident cannot be investigated if the audit trail is shorter than the
    // dedup window that suppressed the event (what-if Q-14).
    if (audit_retention_units < dedup_retention_units) {
        return {ConfigErrorCode::AuditRetentionBelowDedup,
                "audit_retention_units below dedup_retention_units"};
    }
    // Turning on the v2 correction engine without a certified schema migration
    // would reinterpret v1 snapshots (what-if P-03).
    if (corrections_engine_v2_enabled && !corrections_schema_migrated) {
        return {ConfigErrorCode::CorrectionsEnabledWithoutMigration,
                "corrections engine v2 requires a certified schema migration"};
    }

    return {};
}

const SymbolSpec* Policy::find_symbol(const std::string& symbol) const {
    const auto it = symbols.find(symbol);
    return it == symbols.end() ? nullptr : &it->second;
}

Policy default_lab_policy() {
    Policy p;
    SymbolSpec eurusd;
    eurusd.tick_numerator = 1;
    eurusd.tick_denominator = 100000;  // 0.00001
    eurusd.qty_step = 1000;
    eurusd.min_qty = 1000;
    eurusd.max_qty = 100000000;
    eurusd.scale_id = 1;
    p.symbols["EURUSD"] = eurusd;

    SymbolSpec usdjpy;
    usdjpy.tick_numerator = 1;
    usdjpy.tick_denominator = 1000;  // 0.001, a deliberately different scale
    usdjpy.qty_step = 1000;
    usdjpy.min_qty = 1000;
    usdjpy.max_qty = 100000000;
    usdjpy.scale_id = 2;
    p.symbols["USDJPY"] = usdjpy;

    // A symbol with single-unit granularity, used by the bounded model checker
    // where quantities are 1..3.
    SymbolSpec unitfx;
    unitfx.tick_numerator = 1;
    unitfx.tick_denominator = 100;
    unitfx.qty_step = 1;
    unitfx.min_qty = 1;
    unitfx.max_qty = 1000000;
    unitfx.scale_id = 3;
    p.symbols["UNITFX"] = unitfx;

    return p;
}

}  // namespace order_core
