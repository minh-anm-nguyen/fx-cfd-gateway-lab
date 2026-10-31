// Minimal test harness for Project 07.
//
// Design doc 23 fixes the result vocabulary: PASS, FAIL, NOT_RUN, BLOCKED
// (dependency absent) and INCONCLUSIVE (PMU unavailable, clock invalid). A case
// that never exercised its failpoint is explicitly not a pass, so the harness
// records the oracle tags (23 "oracle shorthand") and the seed alongside every
// verdict and writes them out as JSON for scripts/report.py.
//
// Derived from the harness used by Project 01 so the two projects report in the
// same shape; the added fields are the ones Project 07 release gates need.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tt {

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

// Raised by a case that ran but could not reach a verdict, for example because
// the host has no PMU. Design doc 23 keeps this distinct from both PASS and
// FAIL so a missing measurement can never be read as a success.
struct Inconclusive : std::exception {
    std::string message;
    explicit Inconclusive(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

enum class Verdict { Pass, Fail, NotRun, Blocked, Inconclusive };

struct Case {
    std::string id;        // catalog id, for example "LL-021", "FI-01", "CW-16"
    std::string title;
    std::string priority;  // P0, P1
    std::string oracles;   // oracle shorthand from design doc 23, e.g. "Q,M"
    std::function<void()> fn;
    std::string blocked_reason;  // non-empty marks the case as BLOCKED
};

struct Result {
    std::string id;
    std::string title;
    std::string priority;
    std::string oracles;
    Verdict verdict = Verdict::NotRun;
    std::string detail;
    double seconds = 0.0;
    // Set by a case through tt::note_seed so the artifact carries the seed that
    // reproduces it (design doc 23 "required per-case artifact").
    std::uint64_t seed = 0;
    std::string state_hash;
};

std::vector<Case>& registry();
std::vector<Result>& results();

int register_case(Case c);

// Called from inside a case to record reproduction evidence in the artifact.
void note_seed(std::uint64_t seed);
void note_state_hash(std::string hex);

int run_all(int argc, char** argv, const std::string& suite);

// ---------------------------------------------------------------------------

#define TT_CONCAT_(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT_(a, b)

// SCENARIO("LL-021", "P0", "Q,M", "ring empty and consumer pops") { ... }
#define SCENARIO(id_, prio_, oracles_, title_)                                      \
    static void TT_CONCAT(tt_case_fn_, __LINE__)();                                 \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(       \
        ::tt::Case{id_, title_, prio_, oracles_, &TT_CONCAT(tt_case_fn_, __LINE__), \
                   ""});                                                            \
    static void TT_CONCAT(tt_case_fn_, __LINE__)()

// A case that cannot run on this host, with the reason recorded in the report
// rather than silently skipped. Design doc 23: BLOCKED is a real verdict.
#define SCENARIO_BLOCKED(id_, prio_, oracles_, title_, reason_)                        \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(          \
        ::tt::Case{id_, title_, prio_, oracles_, nullptr, reason_});

#define TT_FAIL(msg)                                                      \
    do {                                                                  \
        throw ::tt::AssertionFailure(std::string(__FILE__) + ":" +        \
                                     std::to_string(__LINE__) + " " + (msg)); \
    } while (0)

#define TT_INCONCLUSIVE(msg)                               \
    do {                                                   \
        throw ::tt::Inconclusive(std::string(msg));        \
    } while (0)

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) TT_FAIL(std::string("CHECK failed: ") + #cond); \
    } while (0)

#define CHECK_MSG(cond, msg)                                                              \
    do {                                                                                  \
        if (!(cond))                                                                      \
            TT_FAIL(std::string("CHECK failed: ") + #cond + " -- " + ::tt::to_text(msg)); \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto tt_va = (a);                                                       \
        auto tt_vb = (b);                                                       \
        if (!(tt_va == tt_vb))                                                  \
            TT_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" + \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")"); \
    } while (0)

#define CHECK_NE(a, b)                                                             \
    do {                                                                           \
        auto tt_va = (a);                                                          \
        auto tt_vb = (b);                                                          \
        if (tt_va == tt_vb)                                                        \
            TT_FAIL(std::string("CHECK_NE failed: ") + #a + " != " + #b + " (both " \
                    + ::tt::to_text(tt_va) + ")");                                  \
    } while (0)

#define CHECK_LE(a, b)                                                          \
    do {                                                                        \
        auto tt_va = (a);                                                       \
        auto tt_vb = (b);                                                       \
        if (!(tt_va <= tt_vb))                                                  \
            TT_FAIL(std::string("CHECK_LE failed: ") + #a + " <= " + #b + " (" + \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")"); \
    } while (0)

#define CHECK_LT(a, b)                                                         \
    do {                                                                       \
        auto tt_va = (a);                                                      \
        auto tt_vb = (b);                                                      \
        if (!(tt_va < tt_vb))                                                  \
            TT_FAIL(std::string("CHECK_LT failed: ") + #a + " < " + #b + " (" + \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")"); \
    } while (0)

}  // namespace tt

namespace tt {

inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(std::string_view s) { return std::string(s); }
inline std::string to_text(const char* s) { return s == nullptr ? "(null)" : std::string(s); }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
inline std::string to_text(char c) { return std::string(1, c); }

template <typename T>
inline std::string to_text(T v) {
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

}  // namespace tt
