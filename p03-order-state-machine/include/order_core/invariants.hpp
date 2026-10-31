// The 24 safety invariants from design doc 2.2, as executable checks.
//
// They run after every accepted event and after recovery. A violation is not a
// log line: the reducer refuses to commit the proposed state and quarantines
// the chain instead (invariant I24, fail closed on contradictions).
//
// Some invariants are about the engine rather than one aggregate (who is
// allowed to write, whether an atomic commit exists, whether indices are
// bounded). Those are supplied through InvariantContext so they are still
// asserted explicitly rather than assumed.
#pragma once

#include <string>
#include <vector>

#include "order_core/aggregate.hpp"
#include "order_core/policy.hpp"

namespace order_core {

struct InvariantViolation {
    int id = 0;  // 1..24, matching I01..I24
    std::string name;
    std::string detail;
};

std::string invariant_label(int id);

// Facts the aggregate cannot know about itself.
struct InvariantContext {
    bool single_writer = true;           // I01
    bool unique_chain = true;            // I02
    bool identity_isolated = true;       // I03
    bool exec_scope_correct = true;      // I10
    bool synthetic_fill_created = false; // I19
    bool atomic_commit_available = true; // I21
    bool publishes_confirmed = true;     // I21
    bool replay_hash_matches = true;     // I22
    bool indices_bounded = true;         // I23
    size_t dedup_index_size = 0;         // I23
    size_t chain_count = 0;              // I23
};

// Returns every violation found, not just the first: an incident report that
// names one problem when there are four is a worse starting point for an
// operator.
std::vector<InvariantViolation> check_invariants(const OrderAggregate& a, const Policy& p,
                                                 const InvariantContext& ctx);

// Convenience for call sites that only have an aggregate.
std::vector<InvariantViolation> check_invariants(const OrderAggregate& a, const Policy& p);

std::string describe(const std::vector<InvariantViolation>& v);

}  // namespace order_core
