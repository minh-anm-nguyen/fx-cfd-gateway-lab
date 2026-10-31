// Inbound validation, correlation and normalization (design doc 11).
//
// The pipeline is the one in 11.1, in that order, and it stops at the first
// problem so that nothing half-validated reaches P03:
//
//   allowed session/account/LP  ->  pinned dictionary  ->  ExecType/OrdStatus
//   semantics  ->  request lineage, OrderID binding, ExecID scope  ->  immutable
//   NormalizedReport
//
// Correlation follows the precedence in 7.2 exactly, and in particular it never
// matches a report to a chain by symbol, quantity or price. OR-L-08 injects an
// unrelated report with identical instrument and size and expects it to stay
// unmatched; a heuristic here would silently credit someone else's fill.
#pragma once

#include <optional>
#include <string>

#include "deps/p01_session.hpp"
#include "router/config.hpp"
#include "router/fix44.hpp"
#include "router/report.hpp"
#include "router/store.hpp"

namespace router {

enum class NormalizeOutcome {
    Ok,          // correlated and valid
    Quarantine,  // valid-looking but contradictory or unsupported
    Orphan,      // nothing correlates; keep the evidence and review it
    Invalid,     // failed the dictionary
};

struct NormalizeResult {
    NormalizeOutcome outcome = NormalizeOutcome::Ok;
    ValidationError validation_error = ValidationError::None;
    HoldReason reason = HoldReason::None;
    std::optional<NormalizedReport> report;
    std::string note;
    // Set when the message parsed far enough to know which chain it claimed,
    // even if correlation then refused it. Used for the audit trail.
    std::optional<OrderChainId> chain_hint;
};

class Normalizer {
public:
    Normalizer(const Config& cfg, const Store& store) : cfg_(cfg), store_(store) {}

    // `route` is the route the session belongs to, so a report arriving on one
    // session can never be applied to a chain routed elsewhere (R-I18).
    NormalizeResult parse_and_correlate(const p01::DurableFixInbound& inbound,
                                        const Route& route) const;

private:
    NormalizeResult correlate(const fix::Message& m, const p01::DurableFixInbound& inbound,
                              const Route& route) const;

    const Config& cfg_;
    const Store& store_;
};

}  // namespace router
