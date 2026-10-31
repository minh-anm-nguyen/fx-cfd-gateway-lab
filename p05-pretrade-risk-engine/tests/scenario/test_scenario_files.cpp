// Runs every shipped scenario fixture through the DSL runner, so the files in
// `fixtures/` are part of the suite rather than something only the standalone
// `risk_scenario` binary ever executes.
//
// The last few cases check the runner itself: a harness that cannot fail would
// make every scenario above prove nothing.
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

std::vector<std::filesystem::path> scenario_files() {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  const std::filesystem::path dir("fixtures");
  if (!std::filesystem::exists(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".scenario") continue;
    out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(scenario_files, the_fixture_directory_is_not_empty) {
  // Guards against the suite below passing because it found nothing to run.
  const std::vector<std::filesystem::path> files = scenario_files();
  CHECK_MSG(files.size() >= 8,
            "found " + std::to_string(files.size()) +
                " scenario files under fixtures/ (run from the project root)");
}

TEST(scenario_files, every_shipped_scenario_passes) {
  const std::vector<std::filesystem::path> files = scenario_files();
  CHECK(!files.empty());

  for (const std::filesystem::path& file : files) {
    Result<ScenarioRun> run = run_scenario_file(file.string());
    CHECK_MSG(run.ok(), file.filename().string() + ": " +
                            std::string(reason_name(run.reason())) + ": " +
                            run.detail());
    CHECK_MSG(run.value().ok, file.filename().string() + ": " + run.value().failure);
    // A file that declared no steps would pass vacuously.
    CHECK_MSG(!run.value().steps.empty(),
              file.filename().string() + " has no steps");
    CHECK_MSG(!run.value().scenario_id.empty(),
              file.filename().string() + " has no scenario_id");
  }
}

TEST(scenario_files, a_failing_assertion_is_reported_not_swallowed) {
  const std::string text =
      "scenario_id SELF-CHECK-SHOULD-FAIL\n"
      "seed 1\n"
      "fixture F0_EMPTY\n"
      "store memory\n"
      "step reserve_new account=ACC-A chain=c1 symbol=EURUSD side=BUY qty=50000 "
      "price=110000 command_id=n1 expect=ALLOW_COMMITTED\n"
      "step assert account=ACC-A symbol=EURUSD effective_buy_remaining=99999\n";

  Result<ScenarioRun> run = run_scenario_text(text);
  CHECK(run.ok());
  CHECK(!run.value().ok);
  CHECK(run.value().failure.find("effective_buy_remaining") != std::string::npos);
}

TEST(scenario_files, an_unexpected_decision_is_reported) {
  // A 7-unit order violates the quantity step, so the engine denies it and the
  // runner must notice that this is not the expected decision.
  const std::string text =
      "scenario_id SELF-CHECK-WRONG-DECISION\n"
      "seed 1\n"
      "fixture F0_EMPTY\n"
      "store memory\n"
      "step reserve_new account=ACC-A chain=c1 symbol=EURUSD side=BUY qty=7 "
      "price=110000 command_id=n1 expect=ALLOW_COMMITTED\n";

  Result<ScenarioRun> run = run_scenario_text(text);
  CHECK(run.ok());
  CHECK(!run.value().ok);
  CHECK(run.value().failure.find("expected ALLOW_COMMITTED") != std::string::npos);
}

TEST(scenario_files, unknown_directives_and_verbs_are_errors) {
  CHECK(!run_scenario_text("scenario_id X\nnonsense 1\n").ok());
  CHECK(!run_scenario_text("scenario_id X\nfixture NOT_A_FIXTURE\n").ok());
  CHECK(!run_scenario_text("scenario_id X\nstore sqlite\n").ok());

  Result<ScenarioRun> bad_verb = run_scenario_text(
      "scenario_id X\nfixture F0_EMPTY\nstep teleport account=ACC-A\n");
  CHECK(bad_verb.ok());
  CHECK(!bad_verb.value().ok);
  CHECK(bad_verb.value().failure.find("unknown step verb") != std::string::npos);
}

TEST(scenario_files, a_restart_step_needs_a_durable_store) {
  // Asking for a restart on an in-memory store must fail loudly rather than
  // quietly skipping the restart and passing the later assertions.
  const std::string text =
      "scenario_id SELF-CHECK-RESTART-MEMORY\n"
      "fixture F0_EMPTY\n"
      "store memory\n"
      "step restart\n";
  Result<ScenarioRun> run = run_scenario_text(text);
  CHECK(run.ok());
  CHECK(!run.value().ok);
  CHECK(run.value().failure.find("restart") != std::string::npos);
}
