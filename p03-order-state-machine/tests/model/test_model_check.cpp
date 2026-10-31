// Bounded exhaustive exploration as a gate (design doc 20.2, gate G1).
#include <cstdio>

#include "../support/lab.hpp"
#include "model_check.hpp"

using namespace lab;

SCENARIO("MODEL-01", "P0", "every reachable state within the bound satisfies I01 to I24") {
    model::Bounds b;
    b.order_qty = 3;
    b.max_fill = 2;
    b.replace_qty = 3;
    b.max_depth = 7;

    const model::Report r = model::explore(b);
    CHECK_MSG(r.ok(), "trace: " + r.failing_trace + " -- " + describe(r.violations));
    CHECK(!r.hit_state_limit);
    // The search really ran: a bound that explored nothing would pass vacuously.
    CHECK(r.states_visited > 100);
    CHECK(r.transitions > 500);
    CHECK(r.committed > 100);
    CHECK_EQ(r.deepest, b.max_depth);

    std::printf("          [model] %zu states, %zu transitions, depth %zu "
                "(committed %zu, quarantined %zu, duplicate %zu, rejected %zu, held %zu)\n",
                r.states_visited, r.transitions, r.deepest, r.committed, r.quarantined,
                r.duplicates, r.rejected, r.held);
}

SCENARIO("MODEL-02", "P0", "the search covers every decision outcome, not only the happy path") {
    model::Bounds b;
    b.max_depth = 6;
    const model::Report r = model::explore(b);
    CHECK(r.ok());
    // If a category were empty the alphabet would be too narrow to be evidence
    // of anything.
    CHECK(r.committed > 0);
    CHECK(r.quarantined > 0);
    CHECK(r.duplicates > 0);
    CHECK(r.rejected > 0);
}

SCENARIO("MODEL-03", "P0", "the exploration is deterministic") {
    model::Bounds b;
    b.max_depth = 5;
    const model::Report a = model::explore(b);
    const model::Report c = model::explore(b);
    CHECK_EQ(a.states_visited, c.states_visited);
    CHECK_EQ(a.transitions, c.transitions);
    CHECK_EQ(a.committed, c.committed);
    CHECK_EQ(a.quarantined, c.quarantined);
    CHECK(a.ok() && c.ok());
}

SCENARIO("MODEL-04", "P0", "a wider quantity bound is still clean") {
    model::Bounds b;
    b.order_qty = 4;
    b.max_fill = 3;
    b.replace_qty = 5;
    b.max_depth = 6;
    const model::Report r = model::explore(b);
    CHECK_MSG(r.ok(), "trace: " + r.failing_trace + " -- " + describe(r.violations));
    CHECK(r.states_visited > 100);
}
