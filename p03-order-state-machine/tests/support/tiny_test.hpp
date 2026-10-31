// Minimal test harness.
//
// Every case carries the what-if matrix identifier it exercises (design doc
// 20.6 requires each scenario to produce an artifact and an explicit verdict).
// Results are written as JSON so scripts/report.py can build the P0/P1 matrix
// report that release gates are judged against.
#pragma once

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

enum class Verdict { Pass, Fail, NotRun, Blocked };

struct Case {
    std::string id;        // matrix id, for example "B-01" or "PB-03"
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

// SCENARIO("B-01", "P0", "a partial fill updates the ledger") { ... }
#define SCENARIO(id_, prio_, title_)                                            \
    static void TT_CONCAT(tt_case_fn_, __LINE__)();                             \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(   \
        ::tt::Case{id_, title_, prio_, &TT_CONCAT(tt_case_fn_, __LINE__), ""}); \
    static void TT_CONCAT(tt_case_fn_, __LINE__)()

// A case that cannot run here, with the reason recorded in the report instead
// of being silently skipped. BLOCKED is a real verdict, never a pass: the
// release gate in design doc 21.6 rejects a skipped P0 marked passed.
#define SCENARIO_BLOCKED(id_, prio_, title_, reason_)                           \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) =                        \
        ::tt::register_case(::tt::Case{id_, title_, prio_, nullptr, reason_});

#define TT_FAIL(msg)                                                      \
    do {                                                                  \
        throw ::tt::AssertionFailure(std::string(__FILE__) + ":" +        \
                                     std::to_string(__LINE__) + " " + (msg)); \
    } while (0)

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) TT_FAIL(std::string("CHECK failed: ") + #cond); \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                  \
    do {                                                                                      \
        if (!(cond))                                                                          \
            TT_FAIL(std::string("CHECK failed: ") + #cond + " -- " + ::tt::to_text(msg));     \
    } while (0)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        auto tt_va = (a);                                                          \
        auto tt_vb = (b);                                                          \
        if (!(tt_va == tt_vb))                                                     \
            TT_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" +   \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")");   \
    } while (0)

#define CHECK_NE(a, b)                                                            \
    do {                                                                          \
        auto tt_va = (a);                                                         \
        auto tt_vb = (b);                                                         \
        if (tt_va == tt_vb)                                                       \
            TT_FAIL(std::string("CHECK_NE failed: ") + #a + " != " + #b +         \
                    " (both " + ::tt::to_text(tt_va) + ")");                      \
    } while (0)

}  // namespace tt

// Value rendering used by the assertion macros. Enumerations in this project
// all provide to_string in their own namespace, which is found by argument
// dependent lookup.
namespace tt {

inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(std::string_view s) { return std::string(s); }
inline std::string to_text(const char* s) { return s == nullptr ? "(null)" : std::string(s); }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
inline std::string to_text(char c) { return std::string(1, c); }

template <typename T>
inline std::string to_text(T v) {
    if constexpr (std::is_enum_v<T>) {
        return std::string(to_string(v));
    } else {
        return std::to_string(v);
    }
}

}  // namespace tt
