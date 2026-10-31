#include "fix/failpoint.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fixse::fp {
namespace {

struct Armed {
    Action action = Action::None;
    int param = 0;
    int occurrence = 1;
    int hits = 0;
};

std::mutex& mu() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, Armed>& table() {
    static std::unordered_map<std::string, Armed> t;
    return t;
}

[[noreturn]] void abrupt_exit(std::string_view name) {
    // Deliberately bypass destructors, atexit handlers and stdio flushing: the
    // point of the test is that only what the store already made durable
    // survives.
    std::fprintf(stderr, "FAILPOINT ABRUPT EXIT: %.*s\n", static_cast<int>(name.size()),
                 name.data());
    std::fflush(stderr);
    std::_Exit(99);
}

}  // namespace

void arm(std::string name, Action action, int param, int occurrence) {
    std::lock_guard<std::mutex> g(mu());
    Armed a;
    a.action = action;
    a.param = param;
    a.occurrence = occurrence < 1 ? 1 : occurrence;
    table()[std::move(name)] = a;
}

void disarm_all() {
    std::lock_guard<std::mutex> g(mu());
    table().clear();
}

void arm_from_env() {
    const char* spec = std::getenv("FIXSE_FAILPOINT");
    if (spec == nullptr || *spec == 0) return;
    arm_from_spec(spec);
}

void arm_from_spec(const std::string& s) {
    if (s.empty()) return;
    const size_t eq = s.find('=');
    if (eq == std::string::npos) {
        std::fprintf(stderr, "FIXSE_FAILPOINT: expected name=action, got %s\n", s.c_str());
        std::exit(2);
    }
    std::string name = s.substr(0, eq);
    std::string rest = s.substr(eq + 1);

    int occurrence = 1;
    const size_t at = rest.find('@');
    if (at != std::string::npos) {
        occurrence = std::atoi(rest.c_str() + at + 1);
        rest = rest.substr(0, at);
    }
    int param = 0;
    const size_t colon = rest.find(':');
    if (colon != std::string::npos) {
        param = std::atoi(rest.c_str() + colon + 1);
        rest = rest.substr(0, colon);
    }

    Action a = Action::None;
    if (rest == "error") a = Action::ReturnError;
    else if (rest == "exit") a = Action::AbruptExit;
    else if (rest == "delay") a = Action::Delay;
    else {
        std::fprintf(stderr, "FIXSE_FAILPOINT: unknown action %s\n", rest.c_str());
        std::exit(2);
    }
    arm(std::move(name), a, param, occurrence);
}

Action fire(std::string_view name) {
    Action act = Action::None;
    int param = 0;
    {
        std::lock_guard<std::mutex> g(mu());
        auto it = table().find(std::string(name));
        if (it == table().end()) return Action::None;
        Armed& a = it->second;
        ++a.hits;
        if (a.hits != a.occurrence) return Action::None;
        act = a.action;
        param = a.param;
    }
    if (act == Action::AbruptExit) abrupt_exit(name);
    if (act == Action::Delay) {
        std::this_thread::sleep_for(std::chrono::milliseconds(param));
        return Action::Delay;
    }
    return act;
}

}  // namespace fixse::fp
