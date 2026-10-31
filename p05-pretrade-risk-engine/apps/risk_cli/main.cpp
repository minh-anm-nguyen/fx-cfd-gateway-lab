// risk_cli: inspect a risk domain, validate a policy, run recovery.
//
// Read-mostly by design. There is no subcommand that releases a hold or edits
// a reserved number, because no such operation exists in the engine (§18.1).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "risk/config.hpp"
#include "risk/recovery.hpp"
#include "risk/scenario.hpp"

namespace {

int usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " <command> [options]\n\n"
      << "  validate-policy <file.yaml>     parse and validate a policy bundle\n"
      << "  print-policy [file.yaml]        render the policy back as YAML\n"
      << "  policy-hash [file.yaml]         print the canonical policy hash\n"
      << "  inspect --journal <path>        show domain state, holds and metrics\n"
      << "  recover --journal <path>        replay, run the recovery gates, report\n"
      << "  fixtures                        list the standard fixtures\n"
      << "  failpoints                      list the named crash windows\n"
      << "  bench [--orders N] [--writers W] [--journal <path>]\n"
      << "                                  lab throughput/latency measurement\n";
  return 2;
}

std::string option(const std::vector<std::string>& args, const std::string& name,
                   const std::string& fallback = {}) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return args[i + 1];
  }
  return fallback;
}

int cmd_validate_policy(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "validate-policy needs a file\n";
    return 2;
  }
  risk::Result<risk::RiskPolicy> policy = risk::load_policy_file(args[1]);
  if (!policy.ok()) {
    std::cerr << "INVALID: " << risk::reason_name(policy.reason()) << ": "
              << policy.detail() << "\n";
    return 1;
  }
  std::cout << "VALID\n";
  std::cout << "version=" << policy.value().version << "\n";
  std::cout << "mode=" << risk::domain_mode_name(policy.value().mode) << "\n";
  std::cout << "instruments=" << policy.value().instruments.size() << "\n";
  std::cout << "accounts=" << policy.value().accounts.size() << "\n";
  std::cout << "policy_hash=" << policy.value().hash() << "\n";
  return 0;
}

int cmd_print_policy(const std::vector<std::string>& args) {
  risk::RiskPolicy policy = risk::lab_policy();
  if (args.size() >= 2) {
    risk::Result<risk::RiskPolicy> loaded = risk::load_policy_file(args[1]);
    if (!loaded.ok()) {
      std::cerr << risk::reason_name(loaded.reason()) << ": " << loaded.detail()
                << "\n";
      return 1;
    }
    policy = loaded.value();
  }
  std::cout << risk::policy_to_yaml(policy);
  return 0;
}

int cmd_policy_hash(const std::vector<std::string>& args) {
  risk::RiskPolicy policy = risk::lab_policy();
  if (args.size() >= 2) {
    risk::Result<risk::RiskPolicy> loaded = risk::load_policy_file(args[1]);
    if (!loaded.ok()) {
      std::cerr << risk::reason_name(loaded.reason()) << ": " << loaded.detail()
                << "\n";
      return 1;
    }
    policy = loaded.value();
  }
  std::cout << policy.hash() << "\n";
  return 0;
}

int build_env(const std::vector<std::string>& args,
              std::unique_ptr<risk::LabEnv>& env) {
  const std::string journal = option(args, "--journal");
  if (journal.empty()) {
    std::cerr << "--journal <path> is required\n";
    return 2;
  }
  risk::LabOptions options;
  options.journal_path = journal;
  // Admission is left closed: opening it is recovery's decision, not the
  // CLI's (RI-32).
  options.open_admission = false;
  options.publish_default_market = false;

  const std::string policy_file = option(args, "--policy");
  if (!policy_file.empty()) {
    risk::Result<risk::RiskPolicy> loaded = risk::load_policy_file(policy_file);
    if (!loaded.ok()) {
      std::cerr << risk::reason_name(loaded.reason()) << ": " << loaded.detail()
                << "\n";
      return 1;
    }
    options.policy = loaded.value();
  }

  risk::Result<std::unique_ptr<risk::LabEnv>> created =
      risk::LabEnv::create(options);
  if (!created.ok()) {
    std::cerr << risk::reason_name(created.reason()) << ": " << created.detail()
              << "\n";
    return 1;
  }
  env = std::move(created.value());
  return 0;
}

int cmd_inspect(const std::vector<std::string>& args) {
  std::unique_ptr<risk::LabEnv> env;
  if (const int rc = build_env(args, env); rc != 0) return rc;

  const risk::RiskDomainState domain = env->store().domain_state();
  std::cout << "tenant=" << domain.tenant_id << " domain=" << domain.domain_id
            << "\nrevision=" << domain.revision
            << "\nfencing_token=" << domain.fencing_token
            << "\npolicy_version=" << domain.policy_version
            << "\nmode=" << risk::domain_mode_name(domain.mode)
            << "\nquarantined=" << (domain.quarantined ? 1 : 0)
            << "\nadmission_open=" << (domain.admission_open ? 1 : 0)
            << "\nstate_hash=" << env->store().state_hash() << "\n";

  std::cout << "\n-- reservations --\n";
  for (const risk::Reservation& r : env->store().reservations()) {
    std::cout << r.reservation_id << " " << r.account_id << " " << r.symbol << " "
              << risk::side_name(r.side) << " accepted=" << r.accepted_total_units
              << " cum=" << r.cum_units;
    if (r.proposed_total_units.has_value()) {
      std::cout << " proposed=" << *r.proposed_total_units;
    }
    std::cout << " effective=" << r.risk_effective_remaining()
              << " state=" << risk::reservation_state_name(r.state)
              << (r.pending_cancel ? " pending_cancel" : "") << "\n";
  }

  std::cout << "\n-- projection --\n";
  for (const auto& [account, ae] : env->store().exposure().accounts) {
    for (const auto& [symbol, se] : ae.symbols) {
      std::cout << account << "/" << symbol << " P=" << se.position_units
                << " B=" << se.pending_buy_units << " S=" << se.pending_sell_units
                << " working=" << se.working_orders << "\n";
    }
  }

  const std::vector<risk::AccountId> quarantined = env->store().quarantined_accounts();
  if (!quarantined.empty()) {
    std::cout << "\n-- quarantined accounts --\n";
    for (const risk::AccountId& a : quarantined) std::cout << a << "\n";
  }

  std::cout << "\n-- outbox --\n";
  for (const risk::OutboxIntent& o : env->store().outbox()) {
    std::cout << o.intent_id << " " << o.kind << " " << o.command_id
              << " claimed=" << (o.claimed ? 1 : 0)
              << " dispatched=" << (o.dispatched ? 1 : 0) << "\n";
  }

  const risk::AuditChainReport chain = risk::verify_audit_chain(env->store().audit());
  std::cout << "\naudit_events=" << chain.events << " mutations=" << chain.mutations
            << " chain_ok=" << (chain.ok ? 1 : 0);
  if (!chain.ok) std::cout << " detail=" << chain.detail;
  std::cout << "\n";

  const risk::Status cross = env->coordinator().cross_check_projection();
  std::cout << "projection_cross_check=" << (cross.ok() ? "ok" : cross.detail())
            << "\n";
  return cross.ok() && chain.ok ? 0 : 1;
}

int cmd_recover(const std::vector<std::string>& args) {
  std::unique_ptr<risk::LabEnv> env;
  if (const int rc = build_env(args, env); rc != 0) return rc;

  risk::RecoveryOptions options;
  risk::Result<risk::RecoveryReport> report = risk::run_recovery(
      env->coordinator(), env->store(), env->feed(), env->clock(), options);
  if (!report.ok()) {
    std::cerr << risk::reason_name(report.reason()) << ": " << report.detail() << "\n";
    return 1;
  }
  std::cout << report.value().render();
  return report.value().clean() ? 0 : 1;
}

int cmd_fixtures() {
  const risk::Fixture all[] = {
      risk::Fixture::F0Empty,          risk::Fixture::F1AtLimit,
      risk::Fixture::F2Partial,        risk::Fixture::F3PendingCancel,
      risk::Fixture::F4PendingReplaceUp, risk::Fixture::F5PendingReplaceDown,
      risk::Fixture::F6UnknownNew,     risk::Fixture::F7MultiAccount,
      risk::Fixture::F8StaleFeed,      risk::Fixture::F9CrashPoint,
      risk::Fixture::F10BadExec,       risk::Fixture::F11Breached};
  for (const risk::Fixture f : all) std::cout << risk::fixture_name(f) << "\n";
  return 0;
}

// §25 — a lab measurement, not a benchmark claim. It prints the environment
// facts §25.2 requires alongside the numbers, and it reports the deny/hold
// rates next to the percentiles so a fast accepted path cannot hide overload.
int cmd_bench(const std::vector<std::string>& args) {
  long orders = 20000;
  long writers = 1;
  if (const std::string value = option(args, "--orders"); !value.empty()) {
    orders = std::strtol(value.c_str(), nullptr, 10);
  }
  if (const std::string value = option(args, "--writers"); !value.empty()) {
    writers = std::strtol(value.c_str(), nullptr, 10);
  }
  if (orders <= 0 || writers <= 0) {
    std::cerr << "--orders and --writers must be positive\n";
    return 2;
  }

  risk::LabOptions options;
  options.journal_path = option(args, "--journal");
  // A measured latency needs a real clock; the virtual one only moves when a
  // test tells it to, which would report every commit as instantaneous.
  options.real_clock = true;
  // With a real clock the market data would age out mid-run, so the TTLs are
  // widened. Freshness itself is covered by the test suite, not here.
  for (auto& [symbol, spec] : options.policy.instruments) {
    (void)symbol;
    spec.quote_ttl_ms = 3'600'000;
  }
  options.policy.fx_ttl_ms = 3'600'000;
  options.policy.equity_ttl_ms = 3'600'000;
  // The caps are raised out of the way: this measures the commit path, and a
  // limit refusal partway through would turn it into a measurement of denials.
  options.policy.limits.max_working_orders_per_account = 1'000'000;
  options.policy.limits.max_working_orders_per_account_symbol = 1'000'000;
  options.policy.limits.max_account_symbol_net_minor = 1'000'000'000'000LL;
  options.policy.limits.max_account_symbol_gross_minor = 1'000'000'000'000LL;
  options.policy.limits.max_account_total_gross_minor = 1'000'000'000'000LL;
  options.policy.limits.max_firm_gross_minor = 1'000'000'000'000LL;
  options.policy.queues.admission_cap = 1'000'000;
  for (auto& [id, profile] : options.policy.accounts) {
    (void)id;
    profile.margin_budget_minor = 1'000'000'000'000LL;
  }

  risk::Result<std::unique_ptr<risk::LabEnv>> created =
      risk::LabEnv::create(options);
  if (!created.ok()) {
    std::cerr << risk::reason_name(created.reason()) << ": " << created.detail()
              << "\n";
    return 1;
  }
  std::unique_ptr<risk::LabEnv> env = std::move(created.value());

  std::atomic<long> allowed{0};
  std::atomic<long> denied{0};
  std::atomic<long> held{0};

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(writers));
  for (long w = 0; w < writers; ++w) {
    workers.emplace_back([&, w] {
      const long per_worker = orders / writers;
      for (long i = 0; i < per_worker; ++i) {
        const std::string id = std::to_string(w) + "-" + std::to_string(i);
        const risk::RiskResult r =
            env->new_order("ACC-A", "ch-" + id, "EURUSD", risk::Side::Buy, 1000,
                           110000, "cmd-" + id);
        switch (r.decision) {
          case risk::RiskDecision::AllowCommitted: allowed.fetch_add(1); break;
          case risk::RiskDecision::HoldRetryable: held.fetch_add(1); break;
          default: denied.fetch_add(1); break;
        }
      }
    });
  }
  for (std::thread& t : workers) t.join();
  const auto finish = std::chrono::steady_clock::now();

  const double seconds =
      std::chrono::duration<double>(finish - start).count();
  const long total = allowed.load() + denied.load() + held.load();
  const risk::Histogram& histogram =
      env->coordinator().metrics().admission_commit_seconds();

  std::cout << "-- environment (§25.2 requires these alongside any number) --\n";
  std::cout << "store=" << env->store().describe() << "\n";
  std::cout << "durable=" << (env->store().durable() ? 1 : 0) << "\n";
  std::cout << "writers=" << writers << "\n";
  std::cout << "hardware_concurrency=" << std::thread::hardware_concurrency() << "\n";
  std::cout << "compiler=" <<
#if defined(__clang__)
      "clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
      "gcc " << __GNUC__ << "." << __GNUC_MINOR__
#else
      "unknown"
#endif
            << "\n";
  std::cout << "build_type=" << RISK_BUILD_TYPE << "\n";
  std::cout << "\n-- results --\n";
  std::cout << "offered=" << total << "\n";
  std::cout << "allowed=" << allowed.load() << " denied=" << denied.load()
            << " held=" << held.load() << "\n";
  std::cout << "wall_seconds=" << seconds << "\n";
  if (seconds > 0.0) {
    std::cout << "accepted_per_second="
              << static_cast<double>(allowed.load()) / seconds << "\n";
  }
  std::cout << "p50_micros=" << histogram.quantile_micros(0.50) << "\n";
  std::cout << "p95_micros=" << histogram.quantile_micros(0.95) << "\n";
  std::cout << "p99_micros=" << histogram.quantile_micros(0.99) << "\n";
  std::cout << "p999_micros=" << histogram.quantile_micros(0.999) << "\n";
  std::cout << "max_micros=" << histogram.max_observed_micros() << "\n";
  std::cout << "samples=" << histogram.count() << "\n";

  // A measurement that left the engine inconsistent is not a measurement.
  const risk::Status cross = env->coordinator().cross_check_projection();
  std::cout << "\nprojection_cross_check=" << (cross.ok() ? "ok" : cross.detail())
            << "\n";
  const risk::AuditChainReport chain =
      risk::verify_audit_chain(env->store().audit());
  std::cout << "audit_chain_ok=" << (chain.ok ? 1 : 0) << "\n";
  std::cout << "\nNOTE: lab figures only. Not comparable to the CV's "
               "receive-to-route numbers, which exclude a risk commit (§25.2).\n";

  return (cross.ok() && chain.ok) ? 0 : 1;
}

int cmd_failpoints() {
  for (int i = 1; i < static_cast<int>(risk::Failpoint::Count); ++i) {
    const auto fp = static_cast<risk::Failpoint>(i);
    std::cout << risk::failpoint_window_id(fp) << "  " << risk::failpoint_name(fp)
              << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage(argv[0]);

  const std::string& command = args[0];
  if (command == "validate-policy") return cmd_validate_policy(args);
  if (command == "print-policy") return cmd_print_policy(args);
  if (command == "policy-hash") return cmd_policy_hash(args);
  if (command == "inspect") return cmd_inspect(args);
  if (command == "recover") return cmd_recover(args);
  if (command == "fixtures") return cmd_fixtures();
  if (command == "failpoints") return cmd_failpoints();
  if (command == "bench") return cmd_bench(args);
  return usage(argv[0]);
}
