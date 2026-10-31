#include "lp/failpoint.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace lpsim::fp {

namespace {

struct Armed {
    Action action = Action::None;
    int param = 0;
    int occurrence = 1;  // 1-based hit on which to fire
    int hits = 0;
};

struct State {
    std::mutex mu;
    std::map<std::string, Armed, std::less<>> points;
    std::string marker_path;
    std::string last_fired;
};

State& state() {
    static State s;
    return s;
}

// Write the cutpoint marker and leave immediately. No unwinding, no flush:
// anything the process had buffered is lost, which is the point.
[[noreturn]] void abrupt_exit(std::string_view name, const std::string& marker_path) {
    if (!marker_path.empty()) {
        // Open, write and close with the C API: an fstream destructor would run
        // during a path that is meant to model a process that never cleans up.
        std::FILE* f = std::fopen(marker_path.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(name.data(), 1, name.size(), f);
            std::fputc('\n', f);
            std::fflush(f);
            std::fclose(f);
        }
    }
    std::_Exit(137);  // 128 + SIGKILL, the conventional shell encoding
}

}  // namespace

void arm(std::string name, Action action, int param, int occurrence) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    Armed a;
    a.action = action;
    a.param = param;
    a.occurrence = occurrence < 1 ? 1 : occurrence;
    s.points[std::move(name)] = a;
}

void disarm_all() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    s.points.clear();
    s.last_fired.clear();
}

void set_marker_path(std::string path) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    s.marker_path = std::move(path);
}

std::string_view last_fired() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    return s.last_fired;
}

void arm_from_env() {
    const char* spec = std::getenv("LPSIM_FAILPOINT");
    if (spec == nullptr || *spec == '\0') return;

    const char* marker = std::getenv("LPSIM_FAILPOINT_MARKER");
    if (marker != nullptr && *marker != '\0') set_marker_path(marker);

    // name=action[:param][@occurrence], comma-separated.
    std::string rest(spec);
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t comma = rest.find(',', start);
        const std::string item =
            rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            const std::size_t eq = item.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "LPSIM_FAILPOINT: missing '=' in %s\n", item.c_str());
                std::abort();
            }
            const std::string name = item.substr(0, eq);
            std::string verb = item.substr(eq + 1);

            int occurrence = 1;
            const std::size_t at = verb.find('@');
            if (at != std::string::npos) {
                occurrence = std::atoi(verb.c_str() + at + 1);
                if (occurrence < 1) {
                    std::fprintf(stderr, "LPSIM_FAILPOINT: bad occurrence in %s\n", item.c_str());
                    std::abort();
                }
                verb = verb.substr(0, at);
            }

            int param = 0;
            const std::size_t colon = verb.find(':');
            if (colon != std::string::npos) {
                param = std::atoi(verb.c_str() + colon + 1);
                verb = verb.substr(0, colon);
            }

            Action action = Action::None;
            if (verb == "error") action = Action::ReturnError;
            else if (verb == "exit") action = Action::AbruptExit;
            else if (verb == "delay") action = Action::Delay;
            else {
                std::fprintf(stderr, "LPSIM_FAILPOINT: unknown action '%s'\n", verb.c_str());
                std::abort();
            }
            arm(name, action, param, occurrence);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

Action fire(std::string_view name) {
    State& s = state();
    std::string marker;
    Action act = Action::None;
    int param = 0;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        const auto it = s.points.find(name);
        if (it == s.points.end()) return Action::None;
        Armed& a = it->second;
        ++a.hits;
        if (a.hits != a.occurrence) return Action::None;
        act = a.action;
        param = a.param;
        marker = s.marker_path;
        s.last_fired.assign(name);
    }

    switch (act) {
        case Action::AbruptExit:
            abrupt_exit(name, marker);
        case Action::Delay:
            std::this_thread::sleep_for(std::chrono::milliseconds(param));
            return Action::Delay;
        case Action::ReturnError:
            return Action::ReturnError;
        case Action::None:
            return Action::None;
    }
    return Action::None;
}

}  // namespace lpsim::fp
