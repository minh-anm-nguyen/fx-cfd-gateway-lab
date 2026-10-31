// The safety invariant oracle: R-I01 to R-I26 from design doc 2.2.
//
// Design doc 19.4 is specific about what PASS means: "toan bo R-I invariants van
// phai dung sau moi step", not just at the end of a scenario. So this checker is
// written to run after every single step of every test, and it reports the first
// failing invariant by name so a counterexample is readable.
//
// It checks the persisted state, not the code's intentions. Everything it reads
// comes from the durable image, the session image and the configuration -- the
// same material an auditor would have.
#pragma once

#include <string>
#include <vector>

#include "deps/p01_session.hpp"
#include "router/config.hpp"
#include "router/store.hpp"

namespace router {

struct Violation {
    std::string invariant;  // "R-I07"
    std::string detail;
    std::string subject;
};

// Everything the oracle needs. Passed by reference because it is called after
// every step and must not copy the world.
struct InvariantInputs {
    const StoreImage& store;
    const p01::SessionImage& session;
    const Config& cfg;
    std::size_t status_probes_in_flight = 0;
    // Prefixes a previous call already verified. Journal and audit records are
    // append only and immutable, so re-checking a verified prefix on every
    // model step is pure cost. A full check passes zero for both, which is what
    // every scenario outside the random walk does.
    std::size_t journal_from = 0;
    std::size_t audit_from = 0;
    std::size_t quarantine_from = 0;
    // When true, the invariants that scan the whole command table are skipped.
    // Those tables only grow, so a full scan after every step of a long random
    // walk is quadratic; the walk runs the cheap chain-level check every step
    // and the complete check on a fixed cadence and at the end. Every scenario
    // outside the walk runs the complete check.
    bool skip_command_tables = false;
};

// Returns every violation found, in invariant order. Empty means the state is
// consistent with all twenty-six invariants.
std::vector<Violation> check_invariants(const InvariantInputs& in);

// Convenience for assertions: renders the violations as one line.
std::string render_violations(const std::vector<Violation>& v);

}  // namespace router
