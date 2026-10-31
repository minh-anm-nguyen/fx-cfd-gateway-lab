// Engine configuration: parsing, validation and the config hash.
//
// Design doc 20. The keys are the ones in that section's example profile, and
// the rules it states about them are enforced rather than documented:
//
//   * capacities are powers of two, and a capacity outside the compiled set is
//     rejected before anything is allocated;
//   * the pool must be at least as large as the queue plus the in-flight
//     budget plus a margin, so a full queue produces a queue refusal and not a
//     confusing pool exhaustion;
//   * "no forced CPU 3 on machines without that CPU": a cpu list in the config
//     is intersected with the allowed set, never assumed;
//   * every run writes a config hash, because LI-43 requires a cacheline or
//     capacity change to be versioned and validated rather than to drift.
//
// An unknown key is an error. A profile with a typo that silently falls back to
// a default is how a benchmark ends up measuring something other than what it
// says it measured.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llx/backpressure.hpp"
#include "llx/engine.hpp"

namespace llx {

struct ProfileConfig {
    std::string profile = "lab-p07-v1";
    // simulator_only is the only accepted value. Doc 22: when the benchmark
    // flags are on, the environment must be a simulator, and the config is the
    // place that refuses anything else.
    std::string fix_mode = "simulator_only";
    std::uint32_t owner_shards = 1;
    std::size_t spsc_capacity = 8192;
    std::size_t pool_slots = 32768;
    std::string owner_cpu = "auto_from_allowed_cpuset";
    std::size_t telemetry_buffer_per_thread = 8192;
    std::string allocation_policy = "preallocated_hot_path";
    Watermarks watermarks{};
    std::string durability = "p06_atomic_store_required";
    std::string outbox_send_requires = "p06_committed_epoch";
    std::string failure_mode = "recon_only_on_unknown_commit";
    std::string metrics_clock = "monotonic_raw";
    bool full_benchmark_samples = true;
    bool attempt_pinning = false;
    std::string journal_path;
    bool sync_on_commit = true;
    CommitMode commit_mode = CommitMode::Synchronous;
    std::size_t persistence_inflight_budget = 1;

    // SHA-256 over the canonical rendering below. Goes into the manifest so a
    // report names the configuration it came from (doc 20, LI-48).
    std::string hash() const;
    std::string to_canonical_text() const;

    // Turns the profile into the engine's own configuration, intersecting any
    // CPU request with what the host actually allows.
    Engine::Config to_engine_config() const;
};

struct ConfigError {
    std::string key;
    std::string message;
};

// Parses the YAML subset doc 20 uses: `key: value`, inline maps
// `key: {a: 1, b: 2}`, `#` comments. Returns false and fills `errors` on any
// unknown key, malformed value or failed validation rule.
bool parse_profile(const std::string& text, ProfileConfig& out,
                   std::vector<ConfigError>& errors);
bool load_profile_file(const std::string& path, ProfileConfig& out,
                       std::vector<ConfigError>& errors);

// The validation rules, separated so a test can call them on a hand-built
// profile without going through the parser.
bool validate_profile(const ProfileConfig& c, std::vector<ConfigError>& errors);

}  // namespace llx
