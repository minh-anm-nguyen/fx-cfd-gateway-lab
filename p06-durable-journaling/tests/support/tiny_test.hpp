// Minimal test harness.
//
// Every case carries the identifier of the scenario it exercises: a row from
// the 480-entry matrix in design doc section 24 (G01-01 and friends), a
// crash window from section 15 (W01..W24) or a playbook from section 25
// (PB-01..PB-36). Section 26.2 requires an artifact per test, so results are
// written as JSON and scripts/report.py turns them into the release report.
//
// BLOCKED is a first-class verdict. Section 30 is explicit that this
// document's claims are conditional, and a scenario that cannot run in this
// lab is recorded with its reason rather than quietly counted as a pass.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tt {

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

enum class Verdict { Pass, Fail, NotRun, Blocked };

struct Case {
    std::string id;       // matrix id, for example "B-01" or "PB-03"
    std::string title;
    std::string priority;  // P0, P1, P2
    std::function<void()> fn;
    std::string blocked_reason;  // non-empty marks the case as BLOCKED
};

struct Result {
    std::string id;
    std::string title;
    std::string priority;
    Verdict verdict = Verdict::NotRun;
    std::string detail;
    double seconds = 0.0;
};

std::vector<Case>& registry();
std::vector<Result>& results();

// Register a case. Use the macros below rather than calling this directly.
int register_case(Case c);

int run_all(int argc, char** argv, const std::string& suite);

// ---------------------------------------------------------------------------

#define TT_CONCAT_(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT_(a, b)

// SCENARIO("G01-01", "P0", "first append to an empty stream") { ... }
#define SCENARIO(id_, prio_, title_)                                                     \
    static void TT_CONCAT(tt_case_fn_, __LINE__)();                                      \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(            \
        ::tt::Case{id_, title_, prio_, &TT_CONCAT(tt_case_fn_, __LINE__), ""});          \
    static void TT_CONCAT(tt_case_fn_, __LINE__)()

// A case that cannot run here, with the reason recorded in the report
// instead of being silently skipped.
#define SCENARIO_BLOCKED(id_, prio_, title_, reason_)                           \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) =                        \
        ::tt::register_case(::tt::Case{id_, title_, prio_, nullptr, reason_});

#define TT_FAIL(msg)                                                                  \
    do {                                                                              \
        throw ::tt::AssertionFailure(std::string(__FILE__) + ":" +                     \
                                     std::to_string(__LINE__) + " " + (msg));          \
    } while (0)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) TT_FAIL(std::string("CHECK failed: ") + #cond);      \
    } while (0)

#define CHECK_MSG(cond, msg)                                                             \
    do {                                                                                 \
        if (!(cond))                                                                     \
            TT_FAIL(std::string("CHECK failed: ") + #cond + " -- " + ::tt::to_text(msg)); \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        auto tt_va = (a);                                                               \
        auto tt_vb = (b);                                                               \
        if (!(tt_va == tt_vb))                                                          \
            TT_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" +         \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")");         \
    } while (0)

#define CHECK_NE(a, b)                                                                  \
    do {                                                                                \
        auto tt_va = (a);                                                               \
        auto tt_vb = (b);                                                               \
        if (tt_va == tt_vb)                                                             \
            TT_FAIL(std::string("CHECK_NE failed: ") + #a + " != " + #b + " (both " +    \
                    ::tt::to_text(tt_va) + ")");                                         \
    } while (0)

}  // namespace tt

// Value rendering used by the assertion macros. Kept deliberately small: a FIX
// frame is printed with SOH shown as a vertical bar so a failure message stays
// readable in a terminal.
namespace tt {

inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(std::string_view s) { return std::string(s); }
inline std::string to_text(const char* s) { return s == nullptr ? "(null)" : std::string(s); }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
inline std::string to_text(char c) { return std::string(1, c); }

template <typename T>
inline std::string to_text(T v) {
    if constexpr (std::is_enum_v<T>) {
        // Enumerations in this project all provide to_string in their own
        // namespace; fall back to the numeric value when they do not.
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

// Printable form of raw bytes for failure output.
inline std::string wire_text(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == 1) out.push_back('|');
        else if (u < 0x20 || u > 0x7E) out.append("<?>");
        else out.push_back(c);
    }
    return out;
}

}  // namespace tt
