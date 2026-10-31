#include "llx/affinity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "llx/failpoint.hpp"

#if defined(__linux__)
#define LLX_AFFINITY_LINUX 1
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#define LLX_AFFINITY_WIN32 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace llx {

const char* to_string(ThreadRole r) {
    switch (r) {
        case ThreadRole::FixReceive: return "T0_fix_receive";
        case ThreadRole::Owner: return "T1_owner";
        case ThreadRole::Persistence: return "T2_persistence";
        case ThreadRole::Dispatch: return "T3_dispatch";
        case ThreadRole::Metrics: return "T4_metrics";
        case ThreadRole::Maintenance: return "T5_maintenance";
        case ThreadRole::Count: return "count";
    }
    return "unknown";
}

const char* to_string(PinStatus s) {
    switch (s) {
        case PinStatus::Ok: return "OK";
        case PinStatus::NotAttempted: return "NOT_ATTEMPTED";
        case PinStatus::Unsupported: return "UNSUPPORTED";
        case PinStatus::PermissionDenied: return "PERMISSION_DENIED";
        case PinStatus::InvalidCpu: return "INVALID_CPU";
        case PinStatus::ReadBackMismatch: return "READ_BACK_MISMATCH";
        case PinStatus::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

const CpuInfo* Topology::find(int cpu_id) const {
    for (const CpuInfo& c : cpus)
        if (c.id == cpu_id) return &c;
    return nullptr;
}

const RolePlacement* PlacementPlan::for_role(ThreadRole r) const {
    for (const RolePlacement& p : roles)
        if (p.role == r) return &p;
    return nullptr;
}

// --- cpu list parsing ------------------------------------------------------

bool parse_cpu_list(const std::string& text, std::vector<int>& out) {
    out.clear();
    // Trim trailing newline and spaces; the /sys files end with one.
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' '))
        --end;
    std::size_t i = 0;
    while (i < end && text[i] == ' ') ++i;
    if (i >= end) return true;  // empty list is valid and means "no cpus"

    std::set<int> seen;
    while (i < end) {
        // Parse a number.
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
        long lo = 0;
        while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) {
            lo = lo * 10 + (text[i] - '0');
            if (lo > 1000000) return false;  // absurd cpu id, reject
            ++i;
        }
        long hi = lo;
        if (i < end && text[i] == '-') {
            ++i;
            if (i >= end || !std::isdigit(static_cast<unsigned char>(text[i]))) return false;
            hi = 0;
            while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) {
                hi = hi * 10 + (text[i] - '0');
                if (hi > 1000000) return false;
                ++i;
            }
            // A reversed range is a malformed list, not an empty one.
            if (hi < lo) return false;
        }
        for (long v = lo; v <= hi; ++v) seen.insert(static_cast<int>(v));
        if (i < end) {
            if (text[i] != ',') return false;
            ++i;
            // A trailing comma is malformed.
            if (i >= end) return false;
        }
    }
    out.assign(seen.begin(), seen.end());
    return true;
}

std::string format_cpu_list(const std::vector<int>& cpus) {
    if (cpus.empty()) return "";
    std::vector<int> s = cpus;
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    std::ostringstream o;
    std::size_t i = 0;
    bool first = true;
    while (i < s.size()) {
        std::size_t j = i;
        while (j + 1 < s.size() && s[j + 1] == s[j] + 1) ++j;
        if (!first) o << ",";
        first = false;
        if (j == i) o << s[i];
        else o << s[i] << "-" << s[j];
        i = j + 1;
    }
    return o.str();
}

// --- discovery -------------------------------------------------------------

namespace {

#if LLX_AFFINITY_LINUX
// Only Linux reads topology out of the filesystem; on other hosts the API is
// the only source, so this helper would be dead code there.
bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
#endif

#if LLX_AFFINITY_LINUX

// cgroup v2 effective cpuset. Doc 11 step 1 lists it explicitly, because a
// container's allowed set is narrower than the machine's online set and
// planning from the latter would place threads on CPUs the process may not use.
bool read_cgroup_cpuset(std::vector<int>& out) {
    std::string text;
    // Unified hierarchy first, then the v1 path.
    if (read_file("/sys/fs/cgroup/cpuset.cpus.effective", text) ||
        read_file("/sys/fs/cgroup/cpuset/cpuset.effective_cpus", text)) {
        return parse_cpu_list(text, out);
    }
    return false;
}

Topology discover_linux() {
    Topology t;
    t.capability.platform = "linux";
    t.capability.target_platform = true;

    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<int> allowed;
    if (::sched_getaffinity(0, sizeof(set), &set) == 0) {
        t.capability.allowed_mask = true;
        t.capability.read_back_affinity = true;
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) allowed.push_back(i);
    }
    t.capability.set_thread_affinity = true;

    std::vector<int> cgroup_cpus;
    if (read_cgroup_cpuset(cgroup_cpus)) {
        t.capability.cpuset_cgroup = true;
        // The intersection is what the process may actually run on. Doc 11
        // step 2 says to build the plan from the allowed intersection, not
        // from either source alone.
        if (!allowed.empty() && !cgroup_cpus.empty()) {
            std::vector<int> inter;
            std::set_intersection(allowed.begin(), allowed.end(), cgroup_cpus.begin(),
                                  cgroup_cpus.end(), std::back_inserter(inter));
            allowed = inter;
        } else if (allowed.empty()) {
            allowed = cgroup_cpus;
        }
    }

    std::vector<int> online;
    std::string text;
    if (read_file("/sys/devices/system/cpu/online", text) && parse_cpu_list(text, online))
        t.capability.online_cpus = true;
    if (online.empty()) {
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < n; ++i) online.push_back(static_cast<int>(i));
    }

    std::set<int> nodes;
    for (int id : online) {
        CpuInfo c;
        c.id = id;
        c.online = true;
        c.allowed = std::find(allowed.begin(), allowed.end(), id) != allowed.end();
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(id);
        if (read_file(base + "/topology/core_id", text)) c.core_id = std::atoi(text.c_str());
        if (read_file(base + "/topology/physical_package_id", text))
            c.socket_id = std::atoi(text.c_str());
        std::vector<int> sib;
        if (read_file(base + "/topology/thread_siblings_list", text) &&
            parse_cpu_list(text, sib)) {
            t.capability.smt_siblings = true;
            for (int s : sib)
                if (s != id) c.siblings.push_back(s);
            if (!c.siblings.empty()) t.smt_present = true;
        }
        // NUMA node: the cpu directory carries a node<N> symlink.
        for (int n = 0; n < 64; ++n) {
            std::string probe;
            if (read_file("/sys/devices/system/node/node" + std::to_string(n) + "/cpulist",
                          probe)) {
                std::vector<int> ncpus;
                if (parse_cpu_list(probe, ncpus) &&
                    std::find(ncpus.begin(), ncpus.end(), id) != ncpus.end()) {
                    c.numa_node = n;
                    nodes.insert(n);
                    t.capability.numa_nodes = true;
                    break;
                }
            }
        }
        t.cpus.push_back(std::move(c));
    }
    t.numa_nodes.assign(nodes.begin(), nodes.end());
    for (const CpuInfo& c : t.cpus)
        if (c.allowed) t.allowed.push_back(c.id);
    std::sort(t.allowed.begin(), t.allowed.end());
    t.capability.note =
        "Linux: sched_getaffinity intersected with cgroup cpuset where readable; "
        "sibling and NUMA maps from /sys";
    return t;
}

#elif LLX_AFFINITY_WIN32

Topology discover_win32() {
    Topology t;
    t.capability.platform = "windows";
    t.capability.target_platform = false;
    t.capability.note =
        "non-target platform: the design specifies Linux. Win32 gives a process "
        "affinity mask and per-thread affinity, so placement can be planned, applied "
        "and read back, but there is no cpuset, no cgroup and the SMT/NUMA maps here "
        "are derived from GetLogicalProcessorInformationEx. A placement result from "
        "this host must not be presented as a Linux result (LI-28, LI-29)";

    DWORD_PTR process_mask = 0, system_mask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask)) {
        t.capability.allowed_mask = true;
        t.capability.online_cpus = true;
        t.capability.read_back_affinity = true;
    }
    t.capability.set_thread_affinity = true;

    const int bits = static_cast<int>(sizeof(DWORD_PTR) * 8);
    for (int i = 0; i < bits; ++i) {
        const DWORD_PTR bit = (static_cast<DWORD_PTR>(1) << i);
        if ((system_mask & bit) == 0) continue;
        CpuInfo c;
        c.id = i;
        c.online = true;
        c.allowed = (process_mask & bit) != 0;
        t.cpus.push_back(std::move(c));
    }

    // Core relationships, so SMT siblings are known rather than guessed.
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<unsigned char> buf(len);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (::GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
            t.capability.smt_siblings = true;
            unsigned char* p = buf.data();
            unsigned char* endp = buf.data() + len;
            int core_index = 0;
            while (p < endp) {
                auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (rec->Relationship == RelationProcessorCore) {
                    std::vector<int> members;
                    for (WORD g = 0; g < rec->Processor.GroupCount; ++g) {
                        const KAFFINITY mask = rec->Processor.GroupMask[g].Mask;
                        for (int b = 0; b < bits; ++b)
                            if (mask & (static_cast<KAFFINITY>(1) << b))
                                members.push_back(b);
                    }
                    for (int m : members) {
                        for (CpuInfo& c : t.cpus) {
                            if (c.id != m) continue;
                            c.core_id = core_index;
                            for (int other : members)
                                if (other != m) c.siblings.push_back(other);
                            if (!c.siblings.empty()) t.smt_present = true;
                        }
                    }
                    ++core_index;
                }
                p += rec->Size;
            }
        }
    }

    for (const CpuInfo& c : t.cpus)
        if (c.allowed) t.allowed.push_back(c.id);
    std::sort(t.allowed.begin(), t.allowed.end());
    return t;
}

#endif

}  // namespace

Topology discover_topology() {
#if LLX_AFFINITY_LINUX
    return discover_linux();
#elif LLX_AFFINITY_WIN32
    return discover_win32();
#else
    Topology t;
    t.capability.platform = "unknown";
    t.capability.note =
        "no topology source on this platform: the engine runs unpinned and any "
        "placement claim is unavailable";
    return t;
#endif
}

// --- planning --------------------------------------------------------------

PlacementPlan plan_placement(const Topology& topo, const PlacementPolicy& policy) {
    PlacementPlan plan;
    plan.allowed_at_plan_time = topo.allowed;

    const int allowed_n = static_cast<int>(topo.allowed.size());
    if (allowed_n == 0) {
        plan.accepted = false;
        plan.reject_reason =
            "no allowed CPU could be determined; running unpinned and the benchmark "
            "carries no placement claim";
        return plan;
    }
    if (allowed_n < policy.min_allowed_cpus) {
        // Doc 11 step 2: reject the latency profile rather than plan something
        // that cannot leave a housekeeping CPU.
        plan.accepted = false;
        plan.reject_reason = "allowed CPU count " + std::to_string(allowed_n) +
                             " is below the configured minimum " +
                             std::to_string(policy.min_allowed_cpus) +
                             "; pinning refused so housekeeping is not starved (LI-32)";
        return plan;
    }
    if (allowed_n <= policy.housekeeping_cpus) {
        plan.accepted = false;
        plan.reject_reason =
            "every allowed CPU would be needed for housekeeping; pinning refused";
        return plan;
    }

    // Reserve housekeeping from the end of the allowed list, leaving the low
    // ids for the pipeline. Which end is arbitrary; what matters is that the
    // reservation is explicit and recorded.
    std::vector<int> pool = topo.allowed;
    for (int i = 0; i < policy.housekeeping_cpus && pool.size() > 1; ++i) {
        plan.housekeeping.push_back(pool.back());
        pool.pop_back();
    }

    // Assign in priority order. Roles beyond the available CPUs are left
    // unpinned (cpu = -1) rather than stacked onto a critical CPU: doc 7 says
    // the roles may share, but an unpinned metrics thread is strictly safer
    // than one competing with the owner.
    std::set<int> used_cores;
    std::size_t next = 0;
    for (ThreadRole role : policy.priority) {
        RolePlacement rp;
        rp.role = role;
        if (next < pool.size()) {
            int cpu = pool[next];
            // Skip a CPU whose physical core is already taken by a critical
            // role, when the topology lets us tell and the policy asks.
            if (policy.avoid_smt_siblings && topo.capability.smt_siblings) {
                std::size_t probe = next;
                while (probe < pool.size()) {
                    const CpuInfo* ci = topo.find(pool[probe]);
                    const int core = (ci != nullptr) ? ci->core_id : -1;
                    if (core < 0 || used_cores.find(core) == used_cores.end()) {
                        cpu = pool[probe];
                        // Move the chosen cpu into position so the next role
                        // starts after it.
                        std::swap(pool[next], pool[probe]);
                        break;
                    }
                    ++probe;
                }
                if (probe >= pool.size()) {
                    // Only sibling CPUs remain: take the next one and record
                    // that this role shares a physical core.
                    cpu = pool[next];
                    rp.shared_with_sibling = true;
                    plan.smt_shared = true;
                }
            }
            rp.cpu = cpu;
            rp.exclusive = true;
            if (const CpuInfo* ci = topo.find(cpu); ci != nullptr && ci->core_id >= 0)
                used_cores.insert(ci->core_id);
            ++next;
        }
        plan.roles.push_back(rp);
    }

    plan.accepted = true;
    plan.note =
        "plan built from the allowed intersection at this instant. Pinning reduces "
        "migration; it does not remove scheduler, IRQ or thermal jitter, and doc 1.3 "
        "lists hard real-time as a non-goal. Every pin is read back before the "
        "placement is treated as real (LI-31)";
    if (!topo.capability.target_platform)
        plan.note += ". NOTE: " + topo.capability.note;
    return plan;
}

// --- pinning ---------------------------------------------------------------

PinResult pin_current_thread(int cpu_id) {
    PinResult r;
    r.requested_cpu = cpu_id;
    if (cpu_id < 0) {
        r.status = PinStatus::NotAttempted;
        r.detail = "plan left this role unpinned";
        return r;
    }
    // Lets FI-31 exercise the failure path on a host where pinning would
    // otherwise succeed.
    if (LLX_FP(fp::point::kAffinityPinFail) == fp::Action::ReturnError) {
        r.status = PinStatus::Failed;
        r.detail = "failpoint affinity.pin_fail";
        return r;
    }

#if LLX_AFFINITY_LINUX
    cpu_set_t want;
    CPU_ZERO(&want);
    CPU_SET(cpu_id, &want);
    if (::sched_setaffinity(0, sizeof(want), &want) != 0) {
        r.os_error = errno;
        // Doc 11 step 7: never ignore EPERM/EINVAL. They mean different things
        // and the operator needs to know which happened.
        r.status = (errno == EPERM) ? PinStatus::PermissionDenied
                                    : (errno == EINVAL ? PinStatus::InvalidCpu
                                                       : PinStatus::Failed);
        r.detail = std::string("sched_setaffinity failed: ") + std::strerror(errno);
        return r;
    }
    cpu_set_t got;
    CPU_ZERO(&got);
    if (::sched_getaffinity(0, sizeof(got), &got) != 0) {
        r.os_error = errno;
        r.status = PinStatus::ReadBackMismatch;
        r.detail = "pin applied but the mask could not be read back for verification";
        return r;
    }
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &got)) r.effective_mask.push_back(i);
    if (r.effective_mask.size() != 1 || r.effective_mask[0] != cpu_id) {
        r.status = PinStatus::ReadBackMismatch;
        r.detail = "effective mask is " + format_cpu_list(r.effective_mask) +
                   ", not the requested cpu " + std::to_string(cpu_id);
        return r;
    }
    r.status = PinStatus::Ok;
    return r;

#elif LLX_AFFINITY_WIN32
    if (cpu_id >= static_cast<int>(sizeof(DWORD_PTR) * 8)) {
        r.status = PinStatus::InvalidCpu;
        r.detail = "cpu id outside the single-group affinity mask";
        return r;
    }
    const DWORD_PTR want = (static_cast<DWORD_PTR>(1) << cpu_id);
    const DWORD_PTR prev = ::SetThreadAffinityMask(::GetCurrentThread(), want);
    if (prev == 0) {
        r.os_error = static_cast<int>(::GetLastError());
        r.status = (r.os_error == ERROR_INVALID_PARAMETER) ? PinStatus::InvalidCpu
                                                           : PinStatus::Failed;
        r.detail = "SetThreadAffinityMask failed with error " + std::to_string(r.os_error);
        return r;
    }
    // Read-back: Win32 has no GetThreadAffinityMask, so the mask is verified by
    // setting it again and observing that the previous value is the one we just
    // asked for. This is the documented idiom and it does prove the mask took
    // effect, but it is a weaker check than sched_getaffinity, which is one
    // reason this host is not the target platform.
    const DWORD_PTR again = ::SetThreadAffinityMask(::GetCurrentThread(), want);
    if (again != want) {
        r.status = PinStatus::ReadBackMismatch;
        r.detail = "affinity read-back returned a different mask than requested";
        return r;
    }
    r.effective_mask.push_back(cpu_id);
    r.status = PinStatus::Ok;
    return r;
#else
    r.status = PinStatus::Unsupported;
    r.detail = "no thread affinity API on this platform";
    return r;
#endif
}

PinResult unpin_current_thread(const Topology& topo) {
    PinResult r;
    r.requested_cpu = -1;
#if LLX_AFFINITY_LINUX
    cpu_set_t want;
    CPU_ZERO(&want);
    for (int id : topo.allowed) CPU_SET(id, &want);
    if (topo.allowed.empty()) {
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < n; ++i) CPU_SET(static_cast<int>(i), &want);
    }
    if (::sched_setaffinity(0, sizeof(want), &want) != 0) {
        r.os_error = errno;
        r.status = PinStatus::Failed;
        r.detail = std::string("rollback failed: ") + std::strerror(errno);
        return r;
    }
    r.effective_mask = topo.allowed;
    r.status = PinStatus::Ok;
    return r;
#elif LLX_AFFINITY_WIN32
    DWORD_PTR mask = 0;
    for (int id : topo.allowed)
        if (id < static_cast<int>(sizeof(DWORD_PTR) * 8))
            mask |= (static_cast<DWORD_PTR>(1) << id);
    if (mask == 0) mask = ~static_cast<DWORD_PTR>(0);
    if (::SetThreadAffinityMask(::GetCurrentThread(), mask) == 0) {
        r.os_error = static_cast<int>(::GetLastError());
        r.status = PinStatus::Failed;
        r.detail = "rollback SetThreadAffinityMask failed";
        return r;
    }
    r.effective_mask = topo.allowed;
    r.status = PinStatus::Ok;
    return r;
#else
    (void)topo;
    r.status = PinStatus::Unsupported;
    return r;
#endif
}

bool plan_still_valid(const PlacementPlan& plan, std::string& drift_detail) {
    drift_detail.clear();
    const Topology now = discover_topology();
    if (now.allowed != plan.allowed_at_plan_time) {
        drift_detail = "allowed CPU set changed from [" +
                       format_cpu_list(plan.allowed_at_plan_time) + "] to [" +
                       format_cpu_list(now.allowed) +
                       "]: the placement plan no longer describes this process, so the "
                       "run degrades and the benchmark is marked invalid (doc 21)";
        return false;
    }
    return true;
}

SchedCounters read_sched_counters() {
    SchedCounters c;
#if LLX_AFFINITY_LINUX
    std::string text;
    if (read_file("/proc/self/status", text)) {
        c.available = true;
        auto grab = [&](const char* key, std::uint64_t& out) {
            const std::size_t at = text.find(key);
            if (at == std::string::npos) return;
            const std::size_t colon = text.find(':', at);
            if (colon == std::string::npos) return;
            out = static_cast<std::uint64_t>(std::atoll(text.c_str() + colon + 1));
        };
        grab("voluntary_ctxt_switches", c.voluntary_switches);
        grab("nonvoluntary_ctxt_switches", c.involuntary_switches);
    }
    // Migration count needs perf or /proc/self/sched; left unavailable rather
    // than estimated, because doc 14 forbids presenting a derived number as a
    // measured counter.
#endif
    return c;
}

int current_cpu() {
#if LLX_AFFINITY_LINUX
    return ::sched_getcpu();
#elif LLX_AFFINITY_WIN32
    return static_cast<int>(::GetCurrentProcessorNumber());
#else
    return -1;
#endif
}

// --- reporting -------------------------------------------------------------

std::string topology_to_json(const Topology& t) {
    std::ostringstream o;
    o << "{\"platform\":\"" << t.capability.platform << "\",\"target_platform\":"
      << (t.capability.target_platform ? "true" : "false") << ",\"cpu_count\":"
      << t.cpus.size() << ",\"allowed\":\"" << format_cpu_list(t.allowed)
      << "\",\"smt_present\":" << (t.smt_present ? "true" : "false")
      << ",\"numa_nodes\":" << t.numa_nodes.size() << ",\"capability\":{\"allowed_mask\":"
      << (t.capability.allowed_mask ? "true" : "false") << ",\"online_cpus\":"
      << (t.capability.online_cpus ? "true" : "false") << ",\"smt_siblings\":"
      << (t.capability.smt_siblings ? "true" : "false") << ",\"numa_nodes\":"
      << (t.capability.numa_nodes ? "true" : "false") << ",\"cpuset_cgroup\":"
      << (t.capability.cpuset_cgroup ? "true" : "false") << ",\"read_back_affinity\":"
      << (t.capability.read_back_affinity ? "true" : "false") << "},\"note\":\""
      << t.capability.note << "\"}";
    return o.str();
}

std::string plan_to_json(const PlacementPlan& p) {
    std::ostringstream o;
    o << "{\"accepted\":" << (p.accepted ? "true" : "false") << ",\"reject_reason\":\""
      << p.reject_reason << "\",\"housekeeping\":\"" << format_cpu_list(p.housekeeping)
      << "\",\"smt_shared\":" << (p.smt_shared ? "true" : "false") << ",\"roles\":[";
    bool first = true;
    for (const RolePlacement& r : p.roles) {
        if (!first) o << ",";
        first = false;
        o << "{\"role\":\"" << to_string(r.role) << "\",\"cpu\":" << r.cpu
          << ",\"shared_with_sibling\":" << (r.shared_with_sibling ? "true" : "false")
          << "}";
    }
    o << "],\"note\":\"" << p.note << "\"}";
    return o.str();
}

}  // namespace llx
