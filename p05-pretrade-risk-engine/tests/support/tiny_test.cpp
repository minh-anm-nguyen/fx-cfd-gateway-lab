#include "tiny_test.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace tiny {
namespace {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

std::string& report_path() {
  static std::string path;
  return path;
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

void register_test(std::string suite, std::string name, std::function<void()> body) {
  registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

void set_report_path(std::string path) { report_path() = std::move(path); }

void fail_now(std::string_view file, int line, std::string message) {
  std::string where(file);
  const std::size_t slash = where.find_last_of("/\\");
  if (slash != std::string::npos) where = where.substr(slash + 1);
  throw Failure{where + ":" + std::to_string(line) + ": " + std::move(message)};
}

std::string describe(long long v) { return std::to_string(v); }
std::string describe(unsigned long long v) { return std::to_string(v); }
std::string describe(bool v) { return v ? "true" : "false"; }
std::string describe(const std::string& v) { return "\"" + v + "\""; }
std::string describe(std::string_view v) { return "\"" + std::string(v) + "\""; }
std::string describe(const char* v) { return v == nullptr ? "null" : std::string("\"") + v + "\""; }

int run_all(std::string_view filter) {
  auto& cases = registry();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& a, const TestCase& b) {
    if (a.suite != b.suite) return a.suite < b.suite;
    return a.name < b.name;
  });

  struct Record {
    std::string id;
    bool passed = false;
    std::string message;
    double millis = 0.0;
  };
  std::vector<Record> records;

  int failures = 0;
  int ran = 0;
  int skipped = 0;
  std::string current_suite;

  for (const TestCase& tc : cases) {
    const std::string id = tc.suite + "." + tc.name;
    if (!filter.empty() && id.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    if (tc.suite != current_suite) {
      current_suite = tc.suite;
      std::cout << "\n[" << current_suite << "]\n";
    }
    ++ran;

    Record rec;
    rec.id = id;
    const auto start = std::chrono::steady_clock::now();
    try {
      tc.body();
      rec.passed = true;
    } catch (const Failure& f) {
      rec.message = f.message;
    } catch (const std::exception& e) {
      rec.message = std::string("unexpected exception: ") + e.what();
    } catch (...) {
      rec.message = "unexpected non-standard exception";
    }
    rec.millis = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();

    if (rec.passed) {
      std::cout << "  PASS  " << tc.name << "\n";
    } else {
      ++failures;
      std::cout << "  FAIL  " << tc.name << "\n        " << rec.message << "\n";
    }
    records.push_back(std::move(rec));
  }

  std::cout << "\n" << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << ran
            << " run, " << failures << " failed";
  if (skipped > 0) std::cout << ", " << skipped << " filtered out";
  std::cout << "\n";

  if (!report_path().empty()) {
    std::ofstream out(report_path(), std::ios::binary);
    if (out) {
      out << "{\n  \"run\": " << ran << ",\n  \"failed\": " << failures
          << ",\n  \"cases\": [\n";
      for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        out << "    {\"id\": \"" << json_escape(r.id) << "\", \"passed\": "
            << (r.passed ? "true" : "false") << ", \"millis\": " << r.millis
            << ", \"message\": \"" << json_escape(r.message) << "\"}"
            << (i + 1 == records.size() ? "\n" : ",\n");
      }
      out << "  ]\n}\n";
    }
  }

  return failures;
}

}  // namespace tiny

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--filter=", 0) == 0) {
      filter = arg.substr(9);
    } else if (arg.rfind("--report=", 0) == 0) {
      tiny::set_report_path(arg.substr(9));
    } else if (arg == "--help") {
      std::cout << "usage: " << argv[0]
                << " [--filter=substring] [--report=path.json]\n";
      return 0;
    } else {
      filter = arg;
    }
  }
  if (const char* env = std::getenv("RISK_TEST_REPORT"); env != nullptr) {
    tiny::set_report_path(env);
  }
  return tiny::run_all(filter) == 0 ? 0 : 1;
}
