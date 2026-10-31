#include "jrnl/failpoint.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace jrnl::fp {
namespace {

struct Armed {
    Action action = Action::None;
    int param = 0;
    int occurrence = 1;  // 1-based hit on which the action fires
    int seen = 0;
};

struct Table {
    std::mutex mu;
    std::map<std::string, Armed, std::less<>> armed;
    std::map<std::string, int, std::less<>> hits;
};

Table& table() {
    static Table t;
    return t;
}

[[noreturn]] void bad_spec(const std::string& spec, const char* why) {
    std::fprintf(stderr, "jrnl: bad failpoint specification '%s': %s\n", spec.c_str(), why);
    // Startup refuses rather than running uninstrumented: a crash test that
    // silently armed nothing would report a pass it did not earn.
    std::exit(2);
}

}  // namespace

void arm(std::string name, Action action, int param, int occurrence) {
    Table& t = table();
    std::lock_guard<std::mutex> g(t.mu);
    t.armed[std::move(name)] = Armed{action, param, occurrence < 1 ? 1 : occurrence, 0};
}

void disarm(std::string_view name) {
    Table& t = table();
    std::lock_guard<std::mutex> g(t.mu);
    const auto it = t.armed.find(name);
    if (it != t.armed.end()) t.armed.erase(it);
}

void disarm_all() {
    Table& t = table();
    std::lock_guard<std::mutex> g(t.mu);
    t.armed.clear();
    t.hits.clear();
}

void arm_from_spec(const std::string& spec) {
    if (spec.empty()) return;
    // Several points may be armed at once, separated by commas.
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::string one =
            spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!one.empty()) {
            const std::size_t eq = one.find('=');
            if (eq == std::string::npos) bad_spec(spec, "expected name=action");
            std::string name = one.substr(0, eq);
            std::string rest = one.substr(eq + 1);

            int occurrence = 1;
            const std::size_t at = rest.find('@');
            if (at != std::string::npos) {
                const std::string n = rest.substr(at + 1);
                rest = rest.substr(0, at);
                if (n.empty()) bad_spec(spec, "empty occurrence after @");
                occurrence = std::atoi(n.c_str());
                if (occurrence < 1) bad_spec(spec, "occurrence must be >= 1");
            }

            int param = 0;
            const std::size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                param = std::atoi(rest.substr(colon + 1).c_str());
                rest = rest.substr(0, colon);
            }

            Action action;
            if (rest == "error") action = Action::ReturnError;
            else if (rest == "exit") action = Action::AbruptExit;
            else if (rest == "throw") action = Action::Throw;
            else if (rest == "delay") action = Action::Delay;
            else bad_spec(spec, "action must be error, exit, throw or delay");

            arm(std::move(name), action, param, occurrence);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

void arm_from_env() {
    const char* spec = std::getenv("JRNL_FAILPOINT");
    if (spec != nullptr) arm_from_spec(spec);
}

Action fire(std::string_view name) {
    Action action = Action::None;
    int param = 0;
    {
        Table& t = table();
        std::lock_guard<std::mutex> g(t.mu);
        auto hit = t.hits.find(name);
        if (hit == t.hits.end()) {
            t.hits.emplace(std::string(name), 1);
        } else {
            ++hit->second;
        }

        const auto it = t.armed.find(name);
        if (it == t.armed.end()) return Action::None;
        Armed& a = it->second;
        ++a.seen;
        if (a.seen != a.occurrence) return Action::None;
        action = a.action;
        param = a.param;
    }

    switch (action) {
        case Action::AbruptExit:
            // No unwinding, no flush, no atexit handlers. _Exit is the
            // standard spelling of "stop now" that both platforms honour.
            std::_Exit(137);
        case Action::Delay:
            std::this_thread::sleep_for(std::chrono::milliseconds(param));
            return Action::Delay;
        case Action::Throw:
            throw InjectedFault(std::string("injected fault at ") + std::string(name));
        case Action::ReturnError:
        case Action::None:
            return action;
    }
    return Action::None;
}

int hits(std::string_view name) {
    Table& t = table();
    std::lock_guard<std::mutex> g(t.mu);
    const auto it = t.hits.find(name);
    return it == t.hits.end() ? 0 : it->second;
}

}  // namespace jrnl::fp
