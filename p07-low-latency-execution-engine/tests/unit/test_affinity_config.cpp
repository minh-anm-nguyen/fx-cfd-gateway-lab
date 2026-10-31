// Catalog groups 08 and 24 (LL-141..160, LL-461..480): affinity, cpuset, SMT
// and CPU hotplug; configuration, build and ABI changes.
//
// Oracle code O: the operational, configuration, affinity and security
// behaviour is correct and audited.
//
// The cpu-list parser and the placement planner are pure functions of their
// inputs, so they are tested exhaustively on any host. The pinning itself is
// host-dependent and says so: a case that cannot verify a pin on this platform
// reports INCONCLUSIVE rather than claiming a placement that was never proven
// (LI-31).

#include <algorithm>
#include <string>
#include <vector>

#include "llx/affinity.hpp"
#include "llx/config.hpp"
#include "tiny_test.hpp"

using namespace llx;

namespace {

// Builds a synthetic topology so the planner can be tested against shapes this
// host does not have: SMT pairs, several NUMA nodes, a narrow cpuset.
Topology fake_topology(int cpus, bool smt, const std::vector<int>& allowed) {
    Topology t;
    t.capability.platform = "synthetic";
    t.capability.allowed_mask = true;
    t.capability.online_cpus = true;
    t.capability.smt_siblings = smt;
    t.capability.set_thread_affinity = true;
    t.capability.read_back_affinity = true;
    for (int i = 0; i < cpus; ++i) {
        CpuInfo c;
        c.id = i;
        c.online = true;
        c.allowed = std::find(allowed.begin(), allowed.end(), i) != allowed.end();
        // Two logical CPUs per physical core when SMT is on.
        c.core_id = smt ? i / 2 : i;
        c.socket_id = 0;
        c.numa_node = 0;
        if (smt) {
            const int sib = (i % 2 == 0) ? i + 1 : i - 1;
            if (sib >= 0 && sib < cpus) c.siblings.push_back(sib);
        }
        t.cpus.push_back(std::move(c));
    }
    t.smt_present = smt;
    t.allowed = allowed;
    std::sort(t.allowed.begin(), t.allowed.end());
    return t;
}

std::string fixtures_dir() {
#ifdef LLX_FIXTURES_DIR
    return std::string(LLX_FIXTURES_DIR);
#else
    return "fixtures";
#endif
}

std::string configs_dir() {
    // The configs live beside the fixtures directory in the source tree.
    const std::string f = fixtures_dir();
    const std::size_t at = f.rfind("fixtures");
    return (at == std::string::npos) ? "configs" : f.substr(0, at) + "configs";
}

}  // namespace

// --- cpu list parsing -----------------------------------------------------

SCENARIO("LL-141", "P0", "O", "the cpuset list format parses exactly") {
    std::vector<int> out;
    CHECK(parse_cpu_list("0-3", out));
    CHECK_EQ(out.size(), std::size_t{4});
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[3], 3);

    CHECK(parse_cpu_list("0-3,8,10-11", out));
    CHECK_EQ(out.size(), std::size_t{7});
    CHECK_EQ(format_cpu_list(out), std::string("0-3,8,10-11"));

    CHECK(parse_cpu_list("5", out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0], 5);

    // The /sys files end with a newline.
    CHECK(parse_cpu_list("0-1\n", out));
    CHECK_EQ(out.size(), std::size_t{2});

    // Empty means no CPUs, which is valid and different from malformed.
    CHECK(parse_cpu_list("", out));
    CHECK(out.empty());
    CHECK(parse_cpu_list("\n", out));
    CHECK(out.empty());
}

SCENARIO("LL-142", "P0", "O", "a malformed cpu list is rejected, not partially applied") {
    // Doc 11: a half-read mask would place threads somewhere nobody intended,
    // so the parser refuses the whole list.
    std::vector<int> out;
    CHECK(!parse_cpu_list("0-", out));
    CHECK(!parse_cpu_list("-3", out));
    CHECK(!parse_cpu_list("0,,2", out));
    CHECK(!parse_cpu_list("0,", out));
    CHECK(!parse_cpu_list("abc", out));
    CHECK(!parse_cpu_list("0-3x", out));
    CHECK(!parse_cpu_list("3-1", out));   // reversed range
    CHECK(!parse_cpu_list("99999999999", out));  // absurd id
}

SCENARIO("LL-143", "P0", "O", "formatting is the inverse of parsing") {
    for (const char* s : {"0", "0-7", "0-3,8", "1,3,5,7", "0-1,4-5,8-9", ""}) {
        std::vector<int> v;
        CHECK(parse_cpu_list(s, v));
        CHECK_EQ(format_cpu_list(v), std::string(s));
    }
    // Duplicates and disorder normalise rather than duplicating.
    std::vector<int> v;
    CHECK(parse_cpu_list("3,1,1,2", v));
    CHECK_EQ(format_cpu_list(v), std::string("1-3"));
}

// --- placement planning ---------------------------------------------------

SCENARIO("LL-144", "P0", "O", "the plan is built from the allowed set, never from 0..N") {
    // Doc 11 step 2, verbatim: never assume CPUs are 0..7. Here the process is
    // allowed only 4, 5, 6 and 7.
    const Topology t = fake_topology(8, false, {4, 5, 6, 7});
    PlacementPolicy p;
    const PlacementPlan plan = plan_placement(t, p);
    CHECK(plan.accepted);
    for (const RolePlacement& r : plan.roles) {
        if (r.cpu < 0) continue;
        CHECK(r.cpu >= 4);
        CHECK(r.cpu <= 7);
    }
    // And a housekeeping CPU is reserved out of the allowed set.
    CHECK_EQ(plan.housekeeping.size(), std::size_t{1});
    CHECK(plan.housekeeping[0] >= 4);
}

SCENARIO("LL-145", "P0", "O", "at least one housekeeping CPU is always reserved") {
    // LI-32: pinning must not starve the OS and the session work.
    for (int n = 2; n <= 16; ++n) {
        std::vector<int> allowed;
        for (int i = 0; i < n; ++i) allowed.push_back(i);
        const Topology t = fake_topology(n, false, allowed);
        const PlacementPlan plan = plan_placement(t, PlacementPolicy{});
        CHECK(plan.accepted);
        CHECK(!plan.housekeeping.empty());
        // No role may be placed on a housekeeping CPU.
        for (const RolePlacement& r : plan.roles) {
            if (r.cpu < 0) continue;
            CHECK(std::find(plan.housekeeping.begin(), plan.housekeeping.end(), r.cpu) ==
                  plan.housekeeping.end());
        }
    }
}

SCENARIO("LL-146", "P0", "O", "too few allowed CPUs rejects the latency profile") {
    // Doc 11 step 2: reject rather than produce a plan that cannot be safe.
    const Topology one = fake_topology(4, false, {2});
    const PlacementPlan plan = plan_placement(one, PlacementPolicy{});
    CHECK(!plan.accepted);
    CHECK(!plan.reject_reason.empty());
    CHECK(plan.reject_reason.find("minimum") != std::string::npos);

    const Topology none = fake_topology(4, false, {});
    const PlacementPlan plan2 = plan_placement(none, PlacementPolicy{});
    CHECK(!plan2.accepted);
    CHECK(plan2.reject_reason.find("no allowed CPU") != std::string::npos);
}

SCENARIO("LL-147", "P0", "O", "critical roles avoid sharing a physical core when they can") {
    // Doc 10 lists SMT contention as a measured cost. With 8 logical CPUs on 4
    // physical cores, the owner and the receive thread should not land on the
    // same core.
    const Topology t = fake_topology(8, true, {0, 1, 2, 3, 4, 5, 6, 7});
    PlacementPolicy p;
    p.avoid_smt_siblings = true;
    const PlacementPlan plan = plan_placement(t, p);
    CHECK(plan.accepted);

    const RolePlacement* owner = plan.for_role(ThreadRole::Owner);
    const RolePlacement* recv = plan.for_role(ThreadRole::FixReceive);
    CHECK(owner != nullptr);
    CHECK(recv != nullptr);
    CHECK(owner->cpu >= 0);
    CHECK(recv->cpu >= 0);
    const CpuInfo* oc = t.find(owner->cpu);
    const CpuInfo* rc = t.find(recv->cpu);
    CHECK(oc != nullptr);
    CHECK(rc != nullptr);
    CHECK_NE(oc->core_id, rc->core_id);
}

SCENARIO("LL-148", "P0", "O",
         "when only siblings remain the sharing is recorded, not hidden") {
    // Four logical CPUs on two physical cores, minus housekeeping. Some roles
    // must share, and the plan says so, because a benchmark attributed to an
    // unshared core that was actually shared is a false result.
    const Topology t = fake_topology(4, true, {0, 1, 2, 3});
    PlacementPolicy p;
    p.avoid_smt_siblings = true;
    const PlacementPlan plan = plan_placement(t, p);
    CHECK(plan.accepted);
    bool any_shared = false;
    for (const RolePlacement& r : plan.roles)
        if (r.shared_with_sibling) any_shared = true;
    CHECK_EQ(any_shared, plan.smt_shared);
    CHECK(!plan.note.empty());
}

SCENARIO("LL-149", "P0", "O",
         "the plan note never claims pinning removes jitter") {
    // Doc 1.3 lists hard real-time and jitter elimination as non-goals, so the
    // note has to be honest about what was and was not achieved.
    const Topology t = fake_topology(8, false, {0, 1, 2, 3, 4, 5, 6, 7});
    const PlacementPlan plan = plan_placement(t, PlacementPolicy{});
    CHECK(plan.accepted);
    CHECK(plan.note.find("does not remove") != std::string::npos);
    CHECK(plan.note.find("read back") != std::string::npos);
}

SCENARIO("LL-150", "P0", "O", "roles beyond the available CPUs are left unpinned") {
    // Three allowed CPUs, six roles. The surplus roles get cpu = -1 rather than
    // being stacked onto a critical CPU: an unpinned metrics thread is strictly
    // safer than one competing with the owner.
    const Topology t = fake_topology(8, false, {0, 1, 2});
    const PlacementPlan plan = plan_placement(t, PlacementPolicy{});
    CHECK(plan.accepted);
    int pinned = 0, unpinned = 0;
    for (const RolePlacement& r : plan.roles) {
        if (r.cpu >= 0) ++pinned;
        else ++unpinned;
    }
    CHECK_EQ(pinned + unpinned, 6);
    CHECK_EQ(pinned, 2);  // 3 allowed minus 1 housekeeping
    CHECK_EQ(unpinned, 4);
    // The owner, being first in priority, is one of the pinned ones.
    CHECK(plan.for_role(ThreadRole::Owner)->cpu >= 0);
}

SCENARIO("LL-151", "P0", "O",
         "a plan built from a stale allowed set is detected as drifted") {
    // Doc 21 "CPU hotplug after pin": the plan no longer describes this
    // process, so the run degrades and the benchmark is invalidated.
    PlacementPlan plan;
    plan.accepted = true;
    plan.allowed_at_plan_time = {999, 998};  // a set this host cannot have
    std::string drift;
    CHECK(!plan_still_valid(plan, drift));
    CHECK(!drift.empty());
    CHECK(drift.find("no longer describes") != std::string::npos);

    // A plan built from the real topology right now is valid.
    const Topology now = discover_topology();
    PlacementPlan fresh = plan_placement(now, PlacementPolicy{});
    fresh.allowed_at_plan_time = now.allowed;
    std::string no_drift;
    CHECK(plan_still_valid(fresh, no_drift));
    CHECK(no_drift.empty());
}

SCENARIO("LL-152", "P0", "O", "the real topology is discovered and its limits declared") {
    const Topology t = discover_topology();
    CHECK(!t.capability.platform.empty());
    CHECK(!t.capability.note.empty());
    // Whatever the host, the allowed set must be a subset of the known CPUs.
    for (int id : t.allowed) CHECK(t.find(id) != nullptr);
    tt::note_state_hash("platform=" + t.capability.platform + ";allowed=" +
                        format_cpu_list(t.allowed));
    if (!t.capability.target_platform) {
        // The design targets Linux. A non-Linux host is usable but its
        // placement results are not Linux results (LI-28/29).
        CHECK(t.capability.note.find("not be presented as a Linux result") !=
              std::string::npos);
    }
    if (t.capability.allowed_mask) CHECK(!t.allowed.empty());
}

SCENARIO("LL-153", "P0", "O", "a pin is verified by reading the mask back from the OS") {
    // LI-31: the effective placement comes from the OS, not from a syscall
    // returning zero.
    const Topology t = discover_topology();
    if (!t.capability.set_thread_affinity || t.allowed.empty())
        TT_INCONCLUSIVE("this host exposes no thread affinity API, so no pin can be "
                        "verified and no placement claim is available");

    const int cpu = t.allowed.front();
    const PinResult r = pin_current_thread(cpu);
    if (r.status == PinStatus::PermissionDenied || r.status == PinStatus::Unsupported)
        TT_INCONCLUSIVE(std::string("pinning is unavailable to this process: ") +
                        to_string(r.status) + " -- " + r.detail);
    CHECK_EQ(r.status, PinStatus::Ok);
    CHECK(r.verified());
    CHECK(!r.invalidates_benchmark());
    CHECK_EQ(r.requested_cpu, cpu);
    CHECK(!r.effective_mask.empty());

    // Rollback restores the process-wide allowed set (doc 11 step 7, LI-40).
    const PinResult back = unpin_current_thread(t);
    CHECK_EQ(back.status, PinStatus::Ok);
}

SCENARIO("LL-154", "P0", "O", "an impossible cpu is refused and invalidates the benchmark") {
    const PinResult r = pin_current_thread(100000);
    CHECK_NE(r.status, PinStatus::Ok);
    // Doc 21: an affinity failure makes the benchmark invalid; it does not by
    // itself stop the engine, because business processing is still correct on
    // whatever CPU the OS chose.
    CHECK(r.invalidates_benchmark());
    CHECK(!r.detail.empty());

    // A negative cpu means "the plan left this role unpinned", which is not a
    // failure at all.
    const PinResult none = pin_current_thread(-1);
    CHECK_EQ(none.status, PinStatus::NotAttempted);
    CHECK(!none.invalidates_benchmark());
}

SCENARIO("LL-155", "P0", "O", "a pin failure injected by failpoint is reported, not ignored") {
#ifndef LLX_ENABLE_FAILPOINTS
    TT_INCONCLUSIVE("failpoints are not compiled into this build variant");
#else
    fp::disarm_all();
    fp::reset_counters();
    fp::arm(std::string(fp::point::kAffinityPinFail), fp::Action::ReturnError, 0, 1);
    const PinResult r = pin_current_thread(0);
    CHECK_EQ(r.status, PinStatus::Failed);
    CHECK(r.invalidates_benchmark());
    CHECK(fp::fired(fp::point::kAffinityPinFail) > 0);
    fp::disarm_all();
#endif
}

// --- configuration --------------------------------------------------------

SCENARIO("LL-461", "P0", "O", "the example profile of doc 20 parses and validates") {
    ProfileConfig c;
    std::vector<ConfigError> errs;
    const bool ok = load_profile_file(configs_dir() + "/lab_8c.yaml", c, errs);
    if (!ok) {
        std::string detail;
        for (const ConfigError& e : errs) detail += e.key + ": " + e.message + "; ";
        TT_FAIL("lab_8c.yaml should be valid: " + detail);
    }
    CHECK_EQ(c.spsc_capacity, std::size_t{8192});
    CHECK_EQ(c.pool_slots, std::size_t{32768});
    CHECK_EQ(c.watermarks.watch_pct, 60);
    CHECK_EQ(c.watermarks.shed_new_pct, 85);
    CHECK_EQ(c.watermarks.stop_pct, 95);
    CHECK_EQ(c.fix_mode, std::string("simulator_only"));
    CHECK(!c.attempt_pinning);
    // The hash is stable and non-empty, so a report can name its configuration.
    CHECK_EQ(c.hash().size(), std::size_t{64});
    tt::note_state_hash(c.hash());
}

SCENARIO("LL-462", "P0", "O", "every shipped profile is valid") {
    for (const char* name : {"lab_1c.yaml", "lab_8c.yaml", "container.yaml",
                             "failclosed.yaml"}) {
        ProfileConfig c;
        std::vector<ConfigError> errs;
        const bool ok = load_profile_file(configs_dir() + "/" + name, c, errs);
        if (!ok) {
            std::string detail;
            for (const ConfigError& e : errs) detail += e.key + ": " + e.message + "; ";
            TT_FAIL(std::string(name) + " is invalid: " + detail);
        }
        CHECK(c.watermarks.valid());
    }
}

SCENARIO("LL-463", "P0", "O", "the invalid profile is rejected with one error per rule") {
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!load_profile_file(configs_dir() + "/invalid_rejected.yaml", c, errs));
    CHECK(!errs.empty());

    // Each broken rule has to be named. A single "config invalid" would make
    // the profile impossible to fix.
    auto has = [&](const std::string& key) {
        for (const ConfigError& e : errs)
            if (e.key.find(key) != std::string::npos) return true;
        return false;
    };
    CHECK(has("fix_mode"));
    CHECK(has("owner_shards"));
    CHECK(has("spsc_capacity"));
    CHECK(has("pool_slots"));
    CHECK(has("allocation_policy"));
    CHECK(has("watermarks"));
    CHECK(has("durability"));
    CHECK(has("outbox_send_requires"));
    CHECK(has("failure_mode"));
    CHECK(has("metrics.clock"));
    CHECK(has("unknown_key_that_should_fail"));
}

SCENARIO("LL-464", "P0", "O", "an unknown key is an error, never a silent default") {
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!parse_profile("spsc_capcity: 8192\n", c, errs));  // typo
    bool named = false;
    for (const ConfigError& e : errs)
        if (e.key == "spsc_capcity") named = true;
    CHECK(named);
    // And the real value was not set by the typo.
    CHECK_EQ(c.spsc_capacity, std::size_t{8192});  // still the default
}

SCENARIO("LL-465", "P0", "O", "a capacity that is not compiled in is rejected") {
    // Doc 20: rejected before allocation, not rounded.
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!parse_profile("spsc_capacity: 4096\npool_slots: 65536\n", c, errs));
    bool named = false;
    for (const ConfigError& e : errs)
        if (e.key == "spsc_capacity" && e.message.find("not compiled in") != std::string::npos)
            named = true;
    CHECK(named);

    // And every capacity the factory claims to support really is supported.
    for (std::size_t cap : supported_capacities()) {
        auto ch = make_channel(cap);
        CHECK(ch != nullptr);
        CHECK_EQ(ch->capacity(), cap);
    }
    CHECK(make_channel(4096) == nullptr);
    CHECK(make_channel(0) == nullptr);
}

SCENARIO("LL-466", "P0", "O",
         "a pool smaller than the queue plus in-flight budget is rejected") {
    // Doc 20's sizing rule. Without it a full queue exhausts the pool first and
    // the refusal names the wrong resource, which sends an operator looking in
    // the wrong place.
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!parse_profile("spsc_capacity: 8192\npool_slots: 8192\n", c, errs));
    bool named = false;
    for (const ConfigError& e : errs)
        if (e.key == "pool_slots") named = true;
    CHECK(named);
}

SCENARIO("LL-467", "P0", "O", "the config hash changes when any value changes") {
    // LI-43: a cacheline or capacity change is versioned. If the hash did not
    // move, two different runs could claim the same configuration.
    ProfileConfig a;
    const std::string h0 = a.hash();
    ProfileConfig b = a;
    b.spsc_capacity = 1024;
    CHECK_NE(b.hash(), h0);
    ProfileConfig d = a;
    d.watermarks.watch_pct = 61;
    CHECK_NE(d.hash(), h0);
    ProfileConfig e = a;
    e.sync_on_commit = false;
    CHECK_NE(e.hash(), h0);
    // And it is stable for an unchanged value.
    ProfileConfig f = a;
    CHECK_EQ(f.hash(), h0);
    // The compiled cache-line size is part of the hash, so a rebuild with a
    // different padding is a different configuration.
    CHECK(a.to_canonical_text().find("cache_line_bytes=") != std::string::npos);
}

SCENARIO("LL-468", "P0", "O",
         "a profile that weakens the durability contract is refused") {
    // LI-15, LI-16, LI-17: these are not tunables. No profile may switch them
    // off to get a better number.
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!parse_profile("durability: best_effort\n", c, errs));
    errs.clear();
    CHECK(!parse_profile("outbox_send_requires: nothing\n", c, errs));
    errs.clear();
    CHECK(!parse_profile("failure_mode: retry_on_unknown\n", c, errs));
    errs.clear();
    CHECK(!parse_profile("fix_mode: live_lp\n", c, errs));
}

SCENARIO("LL-469", "P0", "O",
         "multi-shard ownership is refused until the shared-limit proof exists") {
    // Doc 30 open question 3. A shared firm limit must be serialized by one
    // authority, so sharding is not available in v1 even though the code could
    // express it.
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(!parse_profile("owner_shards: 4\n", c, errs));
    bool named = false;
    for (const ConfigError& e : errs)
        if (e.key == "owner_shards" && e.message.find("shared-limit") != std::string::npos)
            named = true;
    CHECK(named);
}

SCENARIO("LL-470", "P0", "O", "the engine configuration derived from a profile is usable") {
    ProfileConfig c;
    std::vector<ConfigError> errs;
    CHECK(load_profile_file(configs_dir() + "/lab_8c.yaml", c, errs));
    const Engine::Config ec = c.to_engine_config();
    CHECK_EQ(ec.spsc_capacity, c.spsc_capacity);
    // The pool is laid out as slabs and must still total at least the request.
    CHECK(static_cast<std::size_t>(ec.pool_slabs) * ec.pool_slots_per_slab >= c.pool_slots);
    CHECK_EQ(ec.watermarks.watch_pct, c.watermarks.watch_pct);
    CHECK(!ec.attempt_pinning);
}

SCENARIO_BLOCKED("LL-471", "P0", "O",
                 "NUMA first-touch placement is compared local against remote",
                 "this host reports no NUMA nodes, so there is no remote node to place "
                 "memory on and the A/B of doc 10's experiment matrix cannot be run. "
                 "mbind/set_mempolicy are Linux-only and the design doc keeps them "
                 "optional for exactly this reason (doc 11 step 4)");

SCENARIO_BLOCKED("LL-472", "P0", "O",
                 "isolcpus and nohz_full reduce measured scheduler jitter",
                 "requires kernel boot parameters on a disposable host. Doc 11 step 5 "
                 "allows it only there, and doc 22 requires an operator change plan with "
                 "rollback. Not runnable in this environment");

SCENARIO_BLOCKED("LL-473", "P0", "O",
                 "SCHED_FIFO placement is evaluated with a watchdog",
                 "ADR-07 and doc 11 forbid setting a real-time policy automatically, and "
                 "this project never does. Evaluating it needs host isolation, a watchdog "
                 "and an operator-approved rollback plan");

int main(int argc, char** argv) { return tt::run_all(argc, argv, "affinity_config"); }
