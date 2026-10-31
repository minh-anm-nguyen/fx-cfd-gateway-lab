// Minimal test harness.
//
// Every case carries the identifier of the what-if matrix row it exercises,
// because design doc 16.1 requires each scenario to map to a test function and
// produce a machine-readable verdict. Results are written as JSON so that
// scripts/report.py can build the P0/P1 matrix report the release gates of
// section 19 are judged against.
//
// There are five verdicts, and the three that are not PASS exist for a reason
// the design doc is emphatic about (16.1, M-07): a skipped or absent test is
// NOT a pass. BLOCKED means the case cannot run in this environment, and
// NOT_IMPLEMENTED means the profile deliberately does not implement the
// behaviour (IOC, FOK, Market, TLS, multi-firm). Both are reported separately
// and neither counts towards a gate.
#pragma once

#include <chrono>
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

enum class Verdict { Pass, Fail, NotRun, Blocked, NotImplemented };

struct Case {
    std::string id;        // matrix id, for example "D-01" or "PB-06"
    std::string title;
    std::string priority;  // P0, P1, P2
    std::function<void()> fn;
    std::string blocked_reason;
    bool not_implemented = false;
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

int register_case(Case c);
int run_all(int argc, char** argv, const std::string& suite);

std::string_view to_string(Verdict v);

// ---------------------------------------------------------------------------

#define TT_CONCAT_(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT_(a, b)

// SCENARIO("D-01", "P0", "a marketable buy fills against ask liquidity") { ... }
#define SCENARIO(id_, prio_, title_)                                                   \
    static void TT_CONCAT(tt_case_fn_, __LINE__)();                                    \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case(          \
        ::tt::Case{id_, title_, prio_, &TT_CONCAT(tt_case_fn_, __LINE__), "", false}); \
    static void TT_CONCAT(tt_case_fn_, __LINE__)()

// A case that cannot run here, with the reason recorded in the report instead
// of being silently absent.
#define SCENARIO_BLOCKED(id_, prio_, title_, reason_)                          \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case( \
        ::tt::Case{id_, title_, prio_, nullptr, reason_, false});

// A behaviour this profile does not implement. Design doc 16.1: it must be
// recorded as NOT IMPLEMENTED and must not be used as evidence of a pass.
#define SCENARIO_NOT_IMPLEMENTED(id_, prio_, title_, reason_)                  \
    static const int TT_CONCAT(tt_case_reg_, __LINE__) = ::tt::register_case( \
        ::tt::Case{id_, title_, prio_, nullptr, reason_, true});

#define TT_FAIL(msg)                                                              \
    do {                                                                          \
        throw ::tt::AssertionFailure(std::string(__FILE__) + ":" +                \
                                     std::to_string(__LINE__) + " " + (msg));     \
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

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        auto tt_va = (a);                                                        \
        auto tt_vb = (b);                                                        \
        if (!(tt_va == tt_vb))                                                   \
            TT_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" + \
                    ::tt::to_text(tt_va) + " vs " + ::tt::to_text(tt_vb) + ")"); \
    } while (0)

#define CHECK_NE(a, b)                                                    \
    do {                                                                  \
        auto tt_va = (a);                                                 \
        auto tt_vb = (b);                                                 \
        if (tt_va == tt_vb)                                               \
            TT_FAIL(std::string("CHECK_NE failed: ") + #a + " != " + #b + \
                    " (both " + ::tt::to_text(tt_va) + ")");              \
    } while (0)

}  // namespace tt

// Value rendering used by the assertion macros. A FIX frame is printed with SOH
// shown as a vertical bar so a failure message stays readable in a terminal.
namespace tt {

inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(std::string_view s) { return std::string(s); }
inline std::string to_text(const char* s) { return s == nullptr ? "(null)" : std::string(s); }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
inline std::string to_text(char c) { return std::string(1, c); }

template <typename T>
inline std::string to_text(T v) {
    if constexpr (std::is_enum_v<T>) {
        // Every enumeration in this project provides to_string in its own
        // namespace; fall back to the numeric value when it does not.
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

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
