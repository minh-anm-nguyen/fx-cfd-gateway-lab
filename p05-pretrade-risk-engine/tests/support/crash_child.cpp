// Out-of-process crash victim for the CW-01..CW-20 windows.
//
// The parent test launches this program with a journal path, a workload name
// and a failpoint spec. When the armed failpoint fires with action `abort` the
// process leaves via _Exit, so no destructor, no buffered stdio flush and no
// atexit handler runs -- which is the point: the journal on disk must be
// recoverable from exactly what reached the device.
//
// Each workload arms the failpoint immediately before the step under test.
// Arming up front would make a window that several steps share (CW-12 fires on
// every commit, CW-17..CW-19 on every event) kill the setup instead, and the
// test would then assert against the wrong moment.
#include <cstdlib>
#include <iostream>
#include <string>

#include "risk/scenario.hpp"

namespace {

const char* kAccount = "ACC-A";
const char* kSymbol = "EURUSD";
constexpr std::int64_t kPrice = 110000;

bool arm(risk::LabEnv& env, const std::string& spec) {
  if (spec.empty()) {
    risk::arm_from_environment(env.failpoints());
    return true;
  }
  if (!risk::arm_from_spec(env.failpoints(), spec)) {
    std::cerr << "bad failpoint spec: " << spec << "\n";
    return false;
  }
  return true;
}

int run_workload(risk::LabEnv& env, const std::string& workload,
                 const std::string& spec) {
  if (workload == "reserve_new") {
    if (!arm(env, spec)) return 2;
    const risk::RiskResult r = env.new_order(kAccount, "crash-chain", kSymbol,
                                             risk::Side::Buy, 50000, kPrice,
                                             "crash-new");
    std::cout << "reserve=" << risk::risk_decision_name(r.decision)
              << " reason=" << risk::reason_name(r.reason) << "\n";
    return r.allowed() ? 0 : 1;
  }

  if (workload == "reserve_and_dispatch") {
    if (!env.new_order(kAccount, "crash-chain", kSymbol, risk::Side::Buy, 50000,
                       kPrice, "crash-new")
             .allowed()) {
      return 1;
    }
    if (!arm(env, spec)) return 2;
    const risk::RiskResult d = env.dispatch("crash-new");
    std::cout << "dispatch=" << risk::risk_decision_name(d.decision) << "\n";
    return d.allowed() ? 0 : 1;
  }

  if (workload == "reserve_and_fill") {
    if (!env.new_order(kAccount, "crash-chain", kSymbol, risk::Side::Buy, 50000,
                       kPrice, "crash-new")
             .allowed()) {
      return 1;
    }
    if (!env.dispatch("crash-new").allowed()) return 1;
    if (!env.lp_accept(kAccount, "crash-chain", "crash-ack").allowed()) return 1;
    if (!arm(env, spec)) return 2;
    const risk::RiskResult f =
        env.lp_fill(kAccount, "crash-chain", "crash-exec-1", 20000);
    std::cout << "fill=" << risk::risk_decision_name(f.decision) << "\n";
    return f.allowed() ? 0 : 1;
  }

  if (workload == "replace_up") {
    if (!env.new_order(kAccount, "crash-chain", kSymbol, risk::Side::Buy, 100000,
                       kPrice, "crash-new")
             .allowed()) {
      return 1;
    }
    if (!env.dispatch("crash-new").allowed()) return 1;
    if (!env.lp_accept(kAccount, "crash-chain", "crash-ack").allowed()) return 1;
    if (!env.lp_fill(kAccount, "crash-chain", "crash-exec-1", 40000).allowed()) {
      return 1;
    }
    if (!arm(env, spec)) return 2;
    const risk::RiskResult g =
        env.replace_order(kAccount, "crash-chain", 150000, kPrice, "crash-replace");
    std::cout << "replace=" << risk::risk_decision_name(g.decision) << "\n";
    return g.allowed() ? 0 : 1;
  }

  std::cerr << "unknown workload: " << workload << "\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0]
              << " <journal-path> <workload> [failpoint-spec]\n";
    return 2;
  }
  const std::string journal = argv[1];
  const std::string workload = argv[2];
  // The spec comes in on argv rather than through the environment: quoting an
  // env assignment through std::system differs between shells, and a silently
  // un-armed crash case would look like a pass.
  const std::string failpoint_spec = argc >= 4 ? argv[3] : std::string{};

  risk::LabOptions options;
  options.journal_path = journal;
  risk::Result<std::unique_ptr<risk::LabEnv>> env = risk::LabEnv::create(options);
  if (!env.ok()) {
    std::cerr << "env: " << risk::reason_name(env.reason()) << ": " << env.detail()
              << "\n";
    return 2;
  }

  const int rc = run_workload(*env.value(), workload, failpoint_spec);
  std::cout << "state_hash=" << env.value()->store().state_hash() << "\n";
  std::cout << "revision=" << env.value()->store().domain_state().revision << "\n";
  return rc;
}
