// Minimal assertion/registration harness.
//
// Deliberately dependency-free: the suite has to run on the lab toolchain with
// nothing but a C++20 compiler, and a crash test needs a binary that can be
// SIGKILLed without a framework's atexit handlers rewriting the result file.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tiny {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

// Thrown by the assertion macros; caught by the runner so one failing case does
// not abort the rest of the suite.
struct Failure {
  std::string message;
};

void register_test(std::string suite, std::string name, std::function<void()> body);

// Runs every registered case whose "suite.name" contains `filter`.
// Returns the number of failures.
int run_all(std::string_view filter);

// Opt-in machine-readable report, written when RISK_TEST_REPORT is set.
void set_report_path(std::string path);

struct Registrar {
  Registrar(std::string suite, std::string name, std::function<void()> body) {
    register_test(std::move(suite), std::move(name), std::move(body));
  }
};

std::string describe(long long v);
std::string describe(unsigned long long v);
std::string describe(bool v);
std::string describe(const std::string& v);
std::string describe(std::string_view v);
std::string describe(const char* v);

template <typename T>
std::string describe(const T& v) {
  return "<value>";
}

void fail_now(std::string_view file, int line, std::string message);

}  // namespace tiny

#define TINY_CONCAT_INNER(a, b) a##b
#define TINY_CONCAT(a, b) TINY_CONCAT_INNER(a, b)

#define TEST(suite_name, case_name)                                        \
  static void TINY_CONCAT(tiny_body_, __LINE__)();                         \
  static ::tiny::Registrar TINY_CONCAT(tiny_reg_, __LINE__){               \
      #suite_name, #case_name, &TINY_CONCAT(tiny_body_, __LINE__)};        \
  static void TINY_CONCAT(tiny_body_, __LINE__)()

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                          \
      ::tiny::fail_now(__FILE__, __LINE__, "CHECK failed: " #cond);         \
    }                                                                       \
  } while (0)

#define CHECK_MSG(cond, msg)                                               \
  do {                                                                     \
    if (!(cond)) {                                                          \
      ::tiny::fail_now(__FILE__, __LINE__,                                   \
                       std::string("CHECK failed: " #cond " -- ") + (msg));  \
    }                                                                       \
  } while (0)

#define CHECK_EQ(lhs, rhs)                                                 \
  do {                                                                     \
    const auto tiny_l = (lhs);                                             \
    const auto tiny_r = (rhs);                                             \
    if (!(tiny_l == tiny_r)) {                                             \
      ::tiny::fail_now(__FILE__, __LINE__,                                  \
                       std::string("CHECK_EQ failed: " #lhs " == " #rhs     \
                                   "\n      lhs = ") +                     \
                           ::tiny::describe(tiny_l) + "\n      rhs = " +   \
                           ::tiny::describe(tiny_r));                      \
    }                                                                       \
  } while (0)

#define CHECK_NE(lhs, rhs)                                                 \
  do {                                                                     \
    const auto tiny_l = (lhs);                                             \
    const auto tiny_r = (rhs);                                             \
    if (tiny_l == tiny_r) {                                                \
      ::tiny::fail_now(__FILE__, __LINE__,                                  \
                       std::string("CHECK_NE failed: " #lhs " != " #rhs));  \
    }                                                                       \
  } while (0)
