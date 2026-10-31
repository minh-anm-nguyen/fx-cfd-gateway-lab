// A deterministic random walk over the whole system, with the invariant oracle
// checked after every single step.
//
// Design doc 19.1 asks for "property-based state exploration + deterministic
// scheduler + fault injection", and 19.4 asks a failing run to record the seed,
// the model step and the first failing invariant. That is exactly what this
// does: one PRNG, one virtual clock, no sleeps, and a failure message that
// names the seed and step so the run can be replayed byte for byte.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lab.hpp"

namespace lab {

class Model {
public:
    struct Options {
        std::uint64_t seed = 42;
        int max_chains = 12;
        bool inject_id_collisions = false;    // OR-R-14
        bool inject_extreme_numbers = false;  // OR-R-15
        bool inject_faults = true;            // crashes, flaps, blackholes
        bool enable_status_queries = false;
        // How often the complete twenty-six invariant check runs. 1 means after
        // every step, which is what design doc 19.4 asks for and what every
        // short walk uses. A long walk raises it: the command table only grows,
        // so scanning all of it after each of a hundred thousand steps is
        // quadratic. The chain, ledger and dedup invariants are still checked
        // every single step; only the command-table ones move to this cadence,
        // and the complete check also runs at the end of the walk.
        int full_check_every = 1;
    };

    explicit Model(Options opt) : opt_(opt), state_(opt.seed * 2654435761u + 1) {
        if (opt_.enable_status_queries) lab.enable_status_queries();
        // The rate and queue bounds are real behaviour, but a random walk that
        // keeps tripping them explores less, so they are widened here and
        // exercised deliberately in section P instead.
        lab.cfg.limits.admission_rate_per_account_per_sec = 1000000;
        lab.cfg.limits.orphan_quota = 1000000;
        lab.cfg.finalize();
    }

    // Runs `steps` actions. Throws tt::AssertionFailure naming the seed, the
    // step and the invariant the moment anything breaks.
    void run(int steps);

    // Counters a scenario can assert on afterwards.
    std::size_t applied_fills = 0;
    std::size_t duplicate_reports = 0;
    std::size_t crashes = 0;
    std::size_t steps_run = 0;

    Lab lab;

private:
    std::uint64_t next() {
        // xorshift64star: small, deterministic, and good enough to explore.
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }
    std::uint64_t pick(std::uint64_t n) { return n == 0 ? 0 : next() % n; }

    void check(int step, const char* action);
    void full_check(int step, const char* action);

    struct Live {
        AcceptedIntent order;
        std::string pending_cancel;
        std::string pending_replace;
        std::int64_t sent_qty = 0;
    };

    Options opt_;
    std::uint64_t state_;
    std::size_t journal_checked_ = 0;
    std::size_t audit_checked_ = 0;
    std::size_t quarantine_checked_ = 0;
    std::vector<Live> live_;
    int next_command_ = 0;
    int next_exec_ = 0;
};

}  // namespace lab
