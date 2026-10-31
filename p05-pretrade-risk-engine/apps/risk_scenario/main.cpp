// risk_scenario: runs one or more deterministic scenario files (§24.5).
//
// The exit code is the gate: 0 when every scenario passed its own assertions
// *and* the post-run invariant checks, 1 otherwise. "It did not crash" is not
// a pass (§20.1).
#include <iostream>
#include <string>
#include <vector>

#include "risk/scenario.hpp"

namespace {

int usage(const char* argv0) {
  std::cout << "usage: " << argv0
            << " [--journal <path>] [--quiet] <scenario.txt> [more.txt ...]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> files;
  std::string journal;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--journal" && i + 1 < argc) {
      journal = argv[++i];
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--help") {
      return usage(argv[0]);
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) return usage(argv[0]);

  int failures = 0;
  for (const std::string& file : files) {
    risk::Result<risk::ScenarioRun> run = risk::run_scenario_file(file, journal);
    if (!run.ok()) {
      std::cerr << file << ": " << risk::reason_name(run.reason()) << ": "
                << run.detail() << "\n";
      ++failures;
      continue;
    }
    if (!run.value().ok) ++failures;
    if (!quiet || !run.value().ok) {
      std::cout << "=== " << file << " ===\n" << run.value().render();
    } else {
      std::cout << "PASS " << run.value().scenario_id << "\n";
    }
  }

  std::cout << (failures == 0 ? "ALL SCENARIOS PASS" : "SCENARIO FAILURES") << ": "
            << files.size() << " run, " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
