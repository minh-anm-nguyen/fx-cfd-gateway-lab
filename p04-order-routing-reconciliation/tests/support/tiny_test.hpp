// Minimal test harness.
//
// Every case carries the what-if matrix identifier it exercises, because design
// doc 22.5 wants an evidence artifact keyed by scenario id with an explicit
// verdict, and 20 is emphatic that a case nobody ran is NOT_RUN rather than a
// quiet pass. BLOCKED is a real verdict too: design doc 23.2 point 3 says that
// if P01 lacks StableSendV1 the issue-once property stays BLOCKED / NOT PROVEN,
// so the harness has to be able to say that instead of failing or passing.
#pragma once

#include <cstdio>
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
    std::string id;        // "OR-A-01", "PB-07", "CW-13"
    std::string title;
    std::string priority;  // P0 or P1
    std::function<void()> fn;
    std::string blocked_reason;  // non-empty marks the case BLOCKED
    std::string group;           // derived from the id prefix at registration
};

struct Result {
    std::string id;
    std::string title;
    std::string priority;
    std::string group;
    Verdict verdict = Verdict::NotRun;
    std::string detail;
    double seconds = 0.0;
};

std::vector<Case>& registry();
std::vector<Result>& results();
int register_case(Case c);
int run_all(int argc, char** argv, const std::string& suite);

#define TT_CONCAT_(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT_(a, b)

// SCENARIO("OR-A-01", "P0", "a valid first New is admitted exactly once")
#define SCENARIO(id_, prio_, title_)                                                  \
    static void TT_CONCAT(tt_case_fn_, __LINE__)();                                   \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(         \
        ::tt::Case{id_, title_, prio_, &TT_CONCAT(tt_case_fn_, __LINE__), "", ""});   \
    static void TT_CONCAT(tt_case_fn_, __LINE__)()

// A case that cannot be run here, with the reason recorded rather than skipped.
#define SCENARIO_BLOCKED(id_, prio_, title_, reason_)                        \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) =                     \
        ::tt::register_case(::tt::Case{id_, title_, prio_, nullptr, reason_, ""});

#define TT_FAIL(msg)                                                               \
    do {                                                                           \
        throw ::tt::AssertionFailure(std::string(__FILE__) + ":" +                  \
                                     std::to_string(__LINE__) + " " + (msg));       \
    } while (0)

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) TT_FAIL(std::string("CHECK failed: ") + #cond);  \
    } while (0)

#define CHECK_MSG(cond, msg)                                                            \
    do {                                                                                \
        if (!(cond))                                                                    \
            TT_FAIL(std::string("CHECK failed: ") + #cond + " -- " + ::tt::to_text(msg)); \
    } while (0)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        auto tt_va = (a);                                                          \
        auto tt_vb = (b);                                                          \
        if (!(tt_va == tt_vb))                                                     \
            TT_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" +    \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")");    \
    } while (0)

#define CHECK_NE(a, b)                                                             \
    do {                                                                           \
        auto tt_va = (a);                                                          \
        auto tt_vb = (b);                                                          \
        if (tt_va == tt_vb)                                                        \
            TT_FAIL(std::string("CHECK_NE failed: ") + #a + " != " + #b +           \
                    " (both " + ::tt::to_text(tt_va) + ")");                        \
    } while (0)

// Value rendering for the assertion macros.
inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(std::string_view s) { return std::string(s); }
inline std::string to_text(const char* s) { return s == nullptr ? "(null)" : std::string(s); }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
inline std::string to_text(char c) { return std::string(1, c); }

template <typename T>
inline std::string to_text(T v) {
    if constexpr (std::is_enum_v<T>) {
        // Every enumeration in this project has a to_string in its own
        // namespace; the unqualified call finds it by argument dependent lookup
        // at instantiation, which keeps this header free of router includes.
        return std::string(to_string(v));
    } else {
        return std::to_string(v);
    }
}

}  // namespace tt
