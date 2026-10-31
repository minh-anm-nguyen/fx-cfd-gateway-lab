#include "llx/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "llx/hash.hpp"

namespace llx {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "true" || v == "yes" || v == "on" || v == "1") { out = true; return true; }
    if (v == "false" || v == "no" || v == "off" || v == "0") { out = false; return true; }
    return false;
}

bool parse_size(const std::string& v, std::size_t& out) {
    if (v.empty()) return false;
    std::size_t acc = 0;
    for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        acc = acc * 10 + static_cast<std::size_t>(c - '0');
        if (acc > (std::size_t{1} << 40)) return false;
    }
    out = acc;
    return true;
}

bool parse_int(const std::string& v, int& out) {
    std::size_t s = 0;
    if (!parse_size(v, s)) return false;
    out = static_cast<int>(s);
    return true;
}

// Splits "a: 1, b: 2" from an inline map into key/value pairs.
std::vector<std::pair<std::string, std::string>> split_inline_map(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t comma = body.find(',', at);
        const std::string item =
            trim(body.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        if (!item.empty()) {
            const std::size_t colon = item.find(':');
            if (colon == std::string::npos) out.emplace_back(item, "");
            else out.emplace_back(trim(item.substr(0, colon)), trim(item.substr(colon + 1)));
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

bool is_power_of_two(std::size_t v) { return v >= 2 && (v & (v - 1)) == 0; }

}  // namespace

bool validate_profile(const ProfileConfig& c, std::vector<ConfigError>& errors) {
    const std::size_t before = errors.size();

    if (c.fix_mode != "simulator_only")
        errors.push_back({"fix_mode",
                          "only 'simulator_only' is accepted. Doc 22 requires a simulator "
                          "environment whenever the benchmark paths are enabled, so a "
                          "profile cannot opt into anything else here"});

    if (!is_power_of_two(c.spsc_capacity))
        errors.push_back({"spsc_capacity", "must be a power of two of at least 2 (LI-06)"});
    else {
        const std::vector<std::size_t> ok = supported_capacities();
        if (std::find(ok.begin(), ok.end(), c.spsc_capacity) == ok.end()) {
            std::ostringstream o;
            o << "capacity " << c.spsc_capacity << " is not compiled in; available: ";
            bool first = true;
            for (std::size_t v : ok) {
                if (!first) o << ", ";
                first = false;
                o << v;
            }
            o << ". Doc 20 rejects a capacity before allocation rather than rounding it";
            errors.push_back({"spsc_capacity", o.str()});
        }
    }

    // Doc 20: M >= max_queue_references + outstanding async handles + margin.
    const std::size_t needed = c.spsc_capacity + c.persistence_inflight_budget + 64;
    if (c.pool_slots < needed) {
        std::ostringstream o;
        o << "pool_slots " << c.pool_slots << " is below the queue capacity plus in-flight "
          << "budget plus margin (" << needed
          << "); a full queue would exhaust the pool first and the refusal would name the "
             "wrong resource";
        errors.push_back({"pool_slots", o.str()});
    }

    if (!c.watermarks.valid())
        errors.push_back({"watermarks",
                          "require 0 < watch_pct < shed_new_pct < stop_pct <= 100 and a "
                          "hysteresis margin smaller than watch_pct, otherwise the "
                          "controller cannot have a stable release threshold (doc 12)"});

    if (c.owner_shards == 0)
        errors.push_back({"owner_shards", "must be at least 1"});
    if (c.owner_shards > 1)
        errors.push_back({"owner_shards",
                          "sharding beyond 1 is refused in v1. Doc 30 open question 3: a "
                          "shared firm risk limit must be serialized by one authority, and "
                          "multi-shard is only admissible after the P05/P06 shared-limit "
                          "proof exists"});

    if (c.allocation_policy != "preallocated_hot_path" && c.allocation_policy != "malloc_baseline")
        errors.push_back({"allocation_policy",
                          "must be 'preallocated_hot_path' or 'malloc_baseline' (the A/B "
                          "pair of doc 10's experiment matrix)"});

    if (c.durability != "p06_atomic_store_required")
        errors.push_back({"durability",
                          "only 'p06_atomic_store_required' is accepted: LI-15 forbids any "
                          "configuration that removes the atomic commit boundary"});
    if (c.outbox_send_requires != "p06_committed_epoch")
        errors.push_back({"outbox_send_requires",
                          "only 'p06_committed_epoch' is accepted: LI-16 forbids sending an "
                          "original before the commit that authorises it"});
    if (c.failure_mode != "recon_only_on_unknown_commit")
        errors.push_back({"failure_mode",
                          "only 'recon_only_on_unknown_commit' is accepted: LI-17/18 forbid "
                          "inferring an outcome from an unknown commit"});

    if (c.metrics_clock != "monotonic_raw" && c.metrics_clock != "steady")
        errors.push_back({"metrics.clock",
                          "must be 'monotonic_raw' or 'steady'. A wall clock is never "
                          "admissible for a duration (LI-25)"});

    if (c.telemetry_buffer_per_thread == 0 || !is_power_of_two(c.telemetry_buffer_per_thread))
        errors.push_back({"telemetry_buffer_per_thread",
                          "must be a power of two: it sizes an SPSC ring"});

    if (c.persistence_inflight_budget == 0)
        errors.push_back({"persistence_inflight_budget", "must be at least 1"});

    return errors.size() == before;
}

bool parse_profile(const std::string& text, ProfileConfig& out,
                   std::vector<ConfigError>& errors) {
    const std::size_t before = errors.size();
    std::istringstream in(text);
    std::string line;
    int line_no = 0;

    auto bad = [&](const std::string& key, const std::string& msg) {
        errors.push_back({key, "line " + std::to_string(line_no) + ": " + msg});
    };

    while (std::getline(in, line)) {
        ++line_no;
        // Strip a comment, then whitespace.
        if (const std::size_t hash_at = line.find('#'); hash_at != std::string::npos)
            line = line.substr(0, hash_at);
        line = trim(line);
        if (line.empty()) continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            bad(line, "expected 'key: value'");
            continue;
        }
        const std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));

        // Inline map.
        if (!value.empty() && value.front() == '{') {
            if (value.back() != '}') {
                bad(key, "inline map is not closed");
                continue;
            }
            const auto items = split_inline_map(value.substr(1, value.size() - 2));
            if (key == "watermarks") {
                for (const auto& [k, v] : items) {
                    int iv = 0;
                    if (!parse_int(v, iv)) {
                        bad("watermarks." + k, "expected an integer");
                        continue;
                    }
                    if (k == "watch_pct") out.watermarks.watch_pct = iv;
                    else if (k == "shed_new_pct") out.watermarks.shed_new_pct = iv;
                    else if (k == "stop_pct") out.watermarks.stop_pct = iv;
                    else if (k == "release_hysteresis_pct")
                        out.watermarks.release_hysteresis_pct = iv;
                    else bad("watermarks." + k, "unknown key");
                }
            } else if (key == "metrics") {
                for (const auto& [k, v] : items) {
                    if (k == "clock") out.metrics_clock = v;
                    else if (k == "full_benchmark_samples") {
                        bool b = true;
                        if (!parse_bool(v, b)) bad("metrics.full_benchmark_samples",
                                                   "expected a boolean");
                        out.full_benchmark_samples = b;
                    } else bad("metrics." + k, "unknown key");
                }
            } else {
                bad(key, "unknown inline-map key");
            }
            continue;
        }

        // Scalars.
        if (key == "profile") out.profile = value;
        else if (key == "fix_mode") out.fix_mode = value;
        else if (key == "owner_cpu") out.owner_cpu = value;
        else if (key == "allocation_policy") out.allocation_policy = value;
        else if (key == "durability") out.durability = value;
        else if (key == "outbox_send_requires") out.outbox_send_requires = value;
        else if (key == "failure_mode") out.failure_mode = value;
        else if (key == "journal_path") out.journal_path = value;
        else if (key == "owner_shards") {
            std::size_t v = 0;
            if (!parse_size(value, v)) bad(key, "expected a non-negative integer");
            else out.owner_shards = static_cast<std::uint32_t>(v);
        } else if (key == "spsc_capacity") {
            if (!parse_size(value, out.spsc_capacity)) bad(key, "expected an integer");
        } else if (key == "pool_slots") {
            if (!parse_size(value, out.pool_slots)) bad(key, "expected an integer");
        } else if (key == "telemetry_buffer_per_thread") {
            if (!parse_size(value, out.telemetry_buffer_per_thread))
                bad(key, "expected an integer");
        } else if (key == "persistence_inflight_budget") {
            if (!parse_size(value, out.persistence_inflight_budget))
                bad(key, "expected an integer");
        } else if (key == "sync_on_commit") {
            if (!parse_bool(value, out.sync_on_commit)) bad(key, "expected a boolean");
        } else if (key == "attempt_pinning") {
            if (!parse_bool(value, out.attempt_pinning)) bad(key, "expected a boolean");
        } else if (key == "commit_mode") {
            if (value == "synchronous") out.commit_mode = CommitMode::Synchronous;
            else if (value == "async_worker") out.commit_mode = CommitMode::AsyncWorker;
            else bad(key, "expected 'synchronous' or 'async_worker'");
        } else {
            // Unknown keys are errors, not defaults. A typo that silently falls
            // back would make a run measure something other than its profile.
            bad(key, "unknown key");
        }
    }

    validate_profile(out, errors);
    return errors.size() == before;
}

bool load_profile_file(const std::string& path, ProfileConfig& out,
                       std::vector<ConfigError>& errors) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        errors.push_back({path, "cannot be opened"});
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_profile(ss.str(), out, errors);
}

std::string ProfileConfig::to_canonical_text() const {
    // Fixed key order so the hash is a function of the values, not of the order
    // they happened to appear in the file.
    std::ostringstream o;
    o << "profile=" << profile << "\nfix_mode=" << fix_mode << "\nowner_shards="
      << owner_shards << "\nspsc_capacity=" << spsc_capacity << "\npool_slots=" << pool_slots
      << "\nowner_cpu=" << owner_cpu << "\ntelemetry_buffer_per_thread="
      << telemetry_buffer_per_thread << "\nallocation_policy=" << allocation_policy
      << "\nwatermarks=" << watermarks.watch_pct << "/" << watermarks.shed_new_pct << "/"
      << watermarks.stop_pct << "/" << watermarks.release_hysteresis_pct << "/"
      << watermarks.dwell_ns << "\ndurability=" << durability << "\noutbox_send_requires="
      << outbox_send_requires << "\nfailure_mode=" << failure_mode << "\nmetrics_clock="
      << metrics_clock << "\nfull_benchmark_samples=" << (full_benchmark_samples ? 1 : 0)
      << "\nattempt_pinning=" << (attempt_pinning ? 1 : 0) << "\nsync_on_commit="
      << (sync_on_commit ? 1 : 0) << "\ncommit_mode="
      << (commit_mode == CommitMode::Synchronous ? "synchronous" : "async_worker")
      << "\npersistence_inflight_budget=" << persistence_inflight_budget
      // The compiled cache-line size is part of the configuration, because
      // LI-43 treats a cacheline change as a versioned change.
      << "\ncache_line_bytes=" << kCacheLineBytes << "\n";
    return o.str();
}

std::string ProfileConfig::hash() const { return Sha256::of(to_canonical_text()); }

Engine::Config ProfileConfig::to_engine_config() const {
    Engine::Config ec;
    ec.spsc_capacity = spsc_capacity;
    // Lay the pool out as slabs of at most 8192 slots, so a future NUMA-aware
    // build can bind a slab per node without changing the handle layout.
    const std::uint32_t per_slab = 8192;
    const std::size_t slabs = (pool_slots + per_slab - 1) / per_slab;
    ec.pool_slabs = static_cast<std::uint32_t>(slabs == 0 ? 1 : slabs);
    ec.pool_slots_per_slab = per_slab;
    ec.persistence_inflight_budget = persistence_inflight_budget;
    ec.commit_mode = commit_mode;
    ec.watermarks = watermarks;
    ec.journal_path = journal_path;
    ec.sync_on_commit = sync_on_commit;
    ec.attempt_pinning = attempt_pinning;
    ec.measure_spans = true;

    // "No forced CPU 3 on machines without that CPU" (doc 20). An explicit cpu
    // list is intersected with what the host allows; anything left over is
    // dropped and the placement planner works from the intersection.
    if (owner_cpu != "auto_from_allowed_cpuset" && !owner_cpu.empty()) {
        std::vector<int> want;
        if (parse_cpu_list(owner_cpu, want)) {
            const Topology t = discover_topology();
            std::vector<int> usable;
            std::set_intersection(want.begin(), want.end(), t.allowed.begin(),
                                  t.allowed.end(), std::back_inserter(usable));
            // The planner takes a policy, not a list, so the intersection is
            // expressed as the housekeeping reservation it implies.
            if (!usable.empty())
                ec.placement.housekeeping_cpus =
                    std::max(1, static_cast<int>(t.allowed.size()) -
                                    static_cast<int>(usable.size()));
        }
    }
    return ec;
}

}  // namespace llx
