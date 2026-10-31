#include "llx/failpoint.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace llx::fp {
namespace {

struct Point {
    Action action = Action::None;
    std::int64_t param = 0;
    int occurrence = 1;
    std::uint64_t hits = 0;
    std::uint64_t fired = 0;
    bool released = false;  // a Pause that has been released stays released
};

struct Registry {
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, Point, std::less<>> points;
};

Registry& reg() {
    static Registry r;
    return r;
}

// How many points currently have an action armed.
//
// fire() checks this with one relaxed load and returns immediately when it is
// zero, without touching the mutex. That fast path is not an optimization, it
// is what makes the lab build usable: every ring push and pop crosses two
// failpoints, so taking a global mutex unconditionally put four contended lock
// round-trips on the hot path and the concurrency stress suite stopped making
// progress. LI-24 forbids unbounded work on the critical path, and a global
// mutex per queue operation is exactly that.
//
// The consequence, stated because it bounds what hits() means: the hit counter
// is only maintained while something is armed. Every playbook arms its point
// before asserting on fired(), so the evidence doc 25 requires is unaffected.
std::atomic<int> g_armed{0};

// Recomputes the armed count. Called under the lock after any change.
void refresh_armed_locked() {
    int n = 0;
    for (const auto& [name, p] : reg().points) {
        (void)name;
        if (p.action != Action::None) ++n;
    }
    g_armed.store(n, std::memory_order_release);
}

[[noreturn]] void bad_spec(const std::string& spec) {
    std::fprintf(stderr, "llx: malformed failpoint specification: %s\n", spec.c_str());
    // Aborting is deliberate: continuing would run an uninstrumented binary
    // that a playbook would then score as a pass (design doc 25).
    std::exit(2);
}

}  // namespace

void arm(std::string name, Action action, std::int64_t param, int occurrence) {
    std::lock_guard<std::mutex> lk(reg().mu);
    Point& p = reg().points[std::move(name)];
    p.action = action;
    p.param = param;
    p.occurrence = occurrence < 1 ? 1 : occurrence;
    p.released = false;
    refresh_armed_locked();
}

void disarm(std::string_view name) {
    std::lock_guard<std::mutex> lk(reg().mu);
    auto it = reg().points.find(name);
    if (it != reg().points.end()) {
        it->second.action = Action::None;
        it->second.released = true;  // never leave a thread parked
    }
    refresh_armed_locked();
    reg().cv.notify_all();
}

void disarm_all() {
    {
        std::lock_guard<std::mutex> lk(reg().mu);
        for (auto& [name, p] : reg().points) {
            (void)name;
            p.action = Action::None;
            p.released = true;
        }
        refresh_armed_locked();
    }
    reg().cv.notify_all();
}

void release(std::string_view name) {
    {
        std::lock_guard<std::mutex> lk(reg().mu);
        reg().points[std::string(name)].released = true;
    }
    reg().cv.notify_all();
}

std::uint64_t hits(std::string_view name) {
    std::lock_guard<std::mutex> lk(reg().mu);
    auto it = reg().points.find(name);
    return it == reg().points.end() ? 0 : it->second.hits;
}

std::uint64_t fired(std::string_view name) {
    std::lock_guard<std::mutex> lk(reg().mu);
    auto it = reg().points.find(name);
    return it == reg().points.end() ? 0 : it->second.fired;
}

void reset_counters() {
    std::lock_guard<std::mutex> lk(reg().mu);
    for (auto& [name, p] : reg().points) {
        (void)name;
        p.hits = 0;
        p.fired = 0;
    }
}

std::int64_t corrupt_param(std::string_view name) {
    std::lock_guard<std::mutex> lk(reg().mu);
    auto it = reg().points.find(name);
    return it == reg().points.end() ? 0 : it->second.param;
}

void arm_from_spec(const std::string& spec) {
    // One or more comma-separated entries: name=action[:param][@occurrence]
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::string entry =
            spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!entry.empty()) {
            const std::size_t eq = entry.find('=');
            if (eq == std::string::npos) bad_spec(spec);
            const std::string name = entry.substr(0, eq);
            std::string rest = entry.substr(eq + 1);
            int occurrence = 1;
            if (const std::size_t at = rest.find('@'); at != std::string::npos) {
                occurrence = std::atoi(rest.c_str() + at + 1);
                if (occurrence < 1) bad_spec(spec);
                rest = rest.substr(0, at);
            }
            std::int64_t param = 0;
            if (const std::size_t colon = rest.find(':'); colon != std::string::npos) {
                param = std::atoll(rest.c_str() + colon + 1);
                rest = rest.substr(0, colon);
            }
            Action a = Action::None;
            if (rest == "error") a = Action::ReturnError;
            else if (rest == "exit") a = Action::AbruptExit;
            else if (rest == "delay") a = Action::Delay;
            else if (rest == "pause") a = Action::Pause;
            else if (rest == "corrupt") a = Action::Corrupt;
            else bad_spec(spec);
            if (name.empty()) bad_spec(spec);
            arm(name, a, param, occurrence);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

void arm_from_env() {
    if (const char* s = std::getenv("LLX_FAILPOINT"); s != nullptr && *s != '\0')
        arm_from_spec(s);
}

Action fire(std::string_view name) {
    // The fast path: nothing is armed anywhere, so there is nothing this call
    // could do. One acquire load and a branch, no lock. See the comment on
    // g_armed for why this matters.
    if (g_armed.load(std::memory_order_acquire) == 0) return Action::None;

    std::unique_lock<std::mutex> lk(reg().mu);
    auto it = reg().points.find(name);
    if (it == reg().points.end()) {
        // Record the crossing even for a point nobody armed, so a playbook can
        // confirm the boundary is reachable at all before arming it.
        reg().points[std::string(name)].hits = 1;
        return Action::None;
    }
    Point& p = it->second;
    ++p.hits;
    if (p.action == Action::None) return Action::None;
    if (p.hits < static_cast<std::uint64_t>(p.occurrence)) return Action::None;
    ++p.fired;
    const Action a = p.action;
    const std::int64_t param = p.param;

    switch (a) {
        case Action::AbruptExit:
            lk.unlock();
            // No unwinding, no flush: the closest stand-in for SIGKILL that is
            // portable. _Exit skips atexit handlers and stream flushing, which
            // is what makes the crash-window oracle meaningful.
            std::_Exit(137);
        case Action::Delay:
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(param));
            return a;
        case Action::Pause: {
            // Park until released. Bounded by a generous ceiling so a mistake
            // in a test hangs the case rather than the whole suite forever.
            const bool ok = reg().cv.wait_for(lk, std::chrono::seconds(30),
                                              [&] { return it->second.released; });
            if (!ok) std::fprintf(stderr, "llx: failpoint pause timed out: %.*s\n",
                                  static_cast<int>(name.size()), name.data());
            return a;
        }
        default:
            return a;
    }
}

}  // namespace llx::fp
