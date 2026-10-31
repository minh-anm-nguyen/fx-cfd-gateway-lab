// CPU topology discovery, placement planning and verified pinning.
//
// Design doc 11 sets out a preflight sequence and this header is that sequence,
// in order:
//
//   1. discover online CPUs, the allowed set after cgroups/cpuset, SMT sibling
//      pairs and NUMA nodes -- never assume CPUs are 0..7 (doc 11 step 2);
//   2. build a role -> CPU plan from the *allowed intersection*, keeping at
//      least one housekeeping CPU so session and OS work cannot be starved
//      (LI-32, doc 11 step 2);
//   3. pin, then read the mask back from the OS and compare. A pin that
//      silently did not take is worse than no pin at all, because the
//      benchmark would be attributed to a placement that never happened
//      (LI-31, doc 11 step 3);
//   4. on failure, record it, fall back, and mark the benchmark invalid rather
//      than continuing as though placement succeeded (LI-40, doc 21).
//
// Two things this module deliberately will not do. It never sets SCHED_FIFO or
// SCHED_RR: doc 11 says those can starve OS work and must not be enabled
// automatically (ADR-07). And it never claims pinning removes jitter -- doc 1.3
// lists that as a non-goal, so the plan carries a note saying what was and was
// not verified.
//
// Platform. The design targets Linux, where the sources are sched_getaffinity,
// /sys/devices/system/cpu and the cgroup v2 cpuset files. On a non-Linux host
// the Win32 affinity API provides a reduced equivalent: process affinity mask
// and thread affinity, no cpuset, no reliable SMT map, no NUMA policy. The
// reduced capability is reported through TopologyCapability rather than
// silently faked, so a run on such a host cannot be presented as a Linux
// placement result.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llx {

// Thread roles of doc 7. These are roles, not a requirement for six dedicated
// cores: the plan maps several roles onto one CPU when that is all the allowed
// set offers.
enum class ThreadRole : std::uint8_t {
    FixReceive = 0,   // T0
    Owner = 1,        // T1
    Persistence = 2,  // T2
    Dispatch = 3,     // T3
    Metrics = 4,      // T4
    Maintenance = 5,  // T5
    Count = 6,
};

const char* to_string(ThreadRole r);

// What the host actually let us observe. Every false here narrows what a
// benchmark report may claim.
struct TopologyCapability {
    bool allowed_mask = false;      // process affinity / cpuset readable
    bool online_cpus = false;
    bool smt_siblings = false;      // sibling map available
    bool numa_nodes = false;
    bool cpuset_cgroup = false;     // cgroup v2 cpuset.cpus.effective readable
    bool set_thread_affinity = false;
    bool read_back_affinity = false;  // can verify a pin actually took
    bool target_platform = false;     // true only on Linux
    std::string platform;
    std::string note;
};

struct CpuInfo {
    int id = -1;
    int core_id = -1;     // physical core, -1 when unknown
    int socket_id = -1;
    int numa_node = -1;
    bool online = false;
    bool allowed = false;  // in the process's allowed set
    // The other logical CPUs sharing this physical core. Non-empty means SMT,
    // and doc 10/11 require an A/B with the sibling idle before a pinning
    // result is believed.
    std::vector<int> siblings;
};

struct Topology {
    std::vector<CpuInfo> cpus;
    std::vector<int> allowed;        // sorted ids of allowed, online CPUs
    std::vector<int> numa_nodes;
    bool smt_present = false;
    TopologyCapability capability;

    const CpuInfo* find(int cpu_id) const;
    std::size_t allowed_count() const { return allowed.size(); }
};

// Reads the real topology of the running process.
Topology discover_topology();

// --- cpuset list parsing ---------------------------------------------------
//
// The format Linux uses in cpuset.cpus, isolcpus and /sys sibling lists:
// "0-3,8,10-11". Parsed strictly: an unparsable list is rejected rather than
// partially applied, because a half-read mask would place threads somewhere
// nobody intended (doc 11 step 7, "never ignore EPERM/EINVAL" in spirit).
bool parse_cpu_list(const std::string& text, std::vector<int>& out);
std::string format_cpu_list(const std::vector<int>& cpus);

// --- placement planning ----------------------------------------------------

struct PlacementPolicy {
    // Roles that get a CPU of their own if the allowed set is large enough, in
    // priority order. Owner first: it is the thread whose tail latency the
    // whole exercise is about.
    std::vector<ThreadRole> priority{ThreadRole::Owner, ThreadRole::FixReceive,
                                     ThreadRole::Persistence, ThreadRole::Dispatch,
                                     ThreadRole::Metrics, ThreadRole::Maintenance};
    // CPUs reserved for the OS, interrupt handling and anything not in the
    // plan. LI-32: pinning must not starve housekeeping.
    int housekeeping_cpus = 1;
    // Avoid placing two critical roles on SMT siblings of one physical core;
    // doc 10 lists SMT contention as a measured cost, not an assumption.
    bool avoid_smt_siblings = true;
    // Refuse to plan at all when the allowed set is too small to honour the
    // housekeeping reservation. Doc 11 step 2: reject the latency profile
    // rather than produce a plan that cannot be safe.
    int min_allowed_cpus = 2;
};

struct RolePlacement {
    ThreadRole role = ThreadRole::Owner;
    int cpu = -1;        // -1 means "not pinned, run wherever the OS decides"
    bool exclusive = false;
    bool shared_with_sibling = false;
};

struct PlacementPlan {
    bool accepted = false;      // false: run unpinned, benchmark marked reduced
    std::string reject_reason;
    std::vector<RolePlacement> roles;
    std::vector<int> housekeeping;
    std::vector<int> allowed_at_plan_time;
    bool smt_shared = false;    // at least one critical role shares a core
    std::string note;

    const RolePlacement* for_role(ThreadRole r) const;
};

// Pure function of the topology and the policy, so the whole planner is
// testable without touching the host's scheduler.
PlacementPlan plan_placement(const Topology& topo, const PlacementPolicy& policy);

// --- pinning ---------------------------------------------------------------

enum class PinStatus : std::uint8_t {
    Ok,                 // set and read back as requested
    NotAttempted,       // plan said unpinned
    Unsupported,        // host has no thread affinity API
    PermissionDenied,   // EPERM
    InvalidCpu,         // EINVAL: cpu not in the allowed set
    ReadBackMismatch,   // the call succeeded but the mask is not what we asked
    Failed,
};

const char* to_string(PinStatus s);

struct PinResult {
    PinStatus status = PinStatus::NotAttempted;
    int requested_cpu = -1;
    std::vector<int> effective_mask;  // what the OS reports after the call
    int os_error = 0;
    std::string detail;

    bool verified() const { return status == PinStatus::Ok; }
    // Doc 21: an affinity failure makes the benchmark invalid; it does not by
    // itself stop the engine, because business processing is still correct on
    // whatever CPU the OS chose.
    bool invalidates_benchmark() const {
        return status != PinStatus::Ok && status != PinStatus::NotAttempted;
    }
};

// Pins the calling thread and reads the mask back. The read-back is the point:
// LI-31 requires the effective placement to come from the OS, not from the fact
// that a syscall returned zero.
PinResult pin_current_thread(int cpu_id);

// Removes any pin, restoring the process-wide allowed set. Used by the rollback
// path of doc 11 step 7 and LI-40.
PinResult unpin_current_thread(const Topology& topo);

// Re-reads the allowed set and reports whether it still matches what the plan
// was built from. A cpuset change or a CPU hotplug event invalidates the plan
// (doc 11 step 7, doc 21 "CPU hotplug after pin"), and the engine must degrade
// rather than keep quoting a placement that no longer exists.
bool plan_still_valid(const PlacementPlan& plan, std::string& drift_detail);

// Migration and context-switch counters for the calling thread, when the host
// exposes them. Doc 11 step 3 wants them recorded alongside a pinning result.
struct SchedCounters {
    bool available = false;
    std::uint64_t voluntary_switches = 0;
    std::uint64_t involuntary_switches = 0;
    std::uint64_t migrations = 0;
};
SchedCounters read_sched_counters();

// Which CPU the caller is on right now, or -1 when the host cannot say.
int current_cpu();

std::string topology_to_json(const Topology& t);
std::string plan_to_json(const PlacementPlan& p);

}  // namespace llx
