// G12 / PB-01 / PB-02 / PB-22 — concurrency, serialisation, fencing.
//
// These are barrier-started, not sleep-based: every worker is released at the
// same moment so the interleaving is contended on purpose rather than by luck
// (§24.4). The assertions are on the *committed* state, which is the only
// thing a safety argument can rest on.
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;
constexpr std::int64_t kQty = 100000;
// One 100,000-unit order at the lab risk price, in minor units.
constexpr std::int64_t kOrderNotional = 11'110'000;

// A policy with `accounts` accounts whose firm cap admits exactly
// `allowed_orders` orders of kQty, and whose per-account caps admit one each.
RiskPolicy contended_policy(int accounts, int allowed_orders) {
  RiskPolicy p = lab_policy();
  p.accounts.clear();
  for (int i = 0; i < accounts; ++i) {
    AccountProfile profile;
    profile.account_id = "ACC-" + std::to_string(i);
    profile.margin_budget_minor = 10'000'000;
    p.accounts[profile.account_id] = profile;
  }
  p.limits.max_order_notional_minor = kOrderNotional;
  p.limits.max_account_symbol_net_minor = kOrderNotional;
  p.limits.max_account_symbol_gross_minor = kOrderNotional;
  p.limits.max_account_total_gross_minor = kOrderNotional;
  p.limits.max_firm_gross_minor = kOrderNotional * allowed_orders;
  p.queues.admission_cap = 1024;
  return p;
}

std::unique_ptr<LabEnv> make_env(const RiskPolicy& policy) {
  LabOptions options;
  options.policy = policy;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

// Releases every worker at once.
class Barrier {
 public:
  explicit Barrier(int expected) : expected_(expected) {}

  void arrive_and_wait() {
    const int seen = arrived_.fetch_add(1) + 1;
    if (seen >= expected_) {
      open_.store(true, std::memory_order_release);
      return;
    }
    while (!open_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

 private:
  const int expected_;
  std::atomic<int> arrived_{0};
  std::atomic<bool> open_{false};
};

NewOrderCommand order(const std::string& account, const std::string& chain,
                      const std::string& command, std::int64_t units) {
  NewOrderCommand cmd;
  cmd.tenant_id = "T1";
  cmd.account_id = account;
  cmd.command_id = command;
  cmd.order_chain_id = chain;
  cmd.symbol = "EURUSD";
  cmd.side = Side::Buy;
  cmd.total_units = units;
  cmd.limit_price_ticks = kPrice;
  return cmd;
}

}  // namespace

TEST(concurrency_firm_cap, a_shared_budget_is_never_oversubscribed) {
  // PB-01: the read-then-write counterexample from §15.1. Eight accounts race
  // for a firm budget that fits three of their orders.
  constexpr int kWorkers = 8;
  constexpr int kAllowed = 3;

  std::unique_ptr<LabEnv> env = make_env(contended_policy(kWorkers, kAllowed));
  Principal trader = env->trader();
  for (int i = 0; i < kWorkers; ++i) trader.accounts.insert("ACC-" + std::to_string(i));

  Barrier barrier(kWorkers);
  std::atomic<int> allowed{0};
  std::atomic<int> denied{0};
  std::atomic<int> other{0};

  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd =
          order("ACC-" + std::to_string(i), "chain-" + std::to_string(i),
                "cmd-" + std::to_string(i), kQty);
      barrier.arrive_and_wait();
      const RiskResult r = env->coordinator().reserve_new(cmd, trader);
      switch (r.decision) {
        case RiskDecision::AllowCommitted: allowed.fetch_add(1); break;
        case RiskDecision::DenyDefinitive: denied.fetch_add(1); break;
        default: other.fetch_add(1); break;
      }
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(allowed.load(), kAllowed);
  CHECK_EQ(other.load(), 0);
  CHECK_EQ(denied.load(), kWorkers - kAllowed);

  Result<DomainMetrics> metrics = env->coordinator().current_metrics();
  CHECK_MSG(metrics.ok(), metrics.detail());
  CHECK_MSG(metrics.value().firm_gross_notional_minor <=
                env->policy().limits.max_firm_gross_minor,
            "firm commitment " +
                std::to_string(metrics.value().firm_gross_notional_minor) +
                " > cap " +
                std::to_string(env->policy().limits.max_firm_gross_minor));
  CHECK_EQ(metrics.value().firm_gross_notional_minor, kOrderNotional * kAllowed);

  // Exactly one intent per admitted order, and no phantom ones.
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(kAllowed));
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(kAllowed));
  CHECK(env->coordinator().cross_check_projection().ok());
  CHECK(verify_audit_chain(env->store().audit()).ok);
}

TEST(concurrency_firm_cap, the_cap_holds_at_every_committed_revision) {
  // Stronger than the end state: a watcher samples the committed projection
  // while the writers run, so an intermediate overshoot would be caught.
  constexpr int kWorkers = 16;
  constexpr int kAllowed = 5;

  std::unique_ptr<LabEnv> env = make_env(contended_policy(kWorkers, kAllowed));
  Principal trader = env->trader();
  for (int i = 0; i < kWorkers; ++i) trader.accounts.insert("ACC-" + std::to_string(i));

  std::atomic<bool> running{true};
  std::atomic<std::int64_t> worst_observed{0};
  std::atomic<int> samples{0};

  std::thread watcher([&] {
    while (running.load(std::memory_order_acquire)) {
      Result<DomainMetrics> metrics = env->coordinator().current_metrics();
      if (metrics.ok()) {
        samples.fetch_add(1);
        std::int64_t previous = worst_observed.load();
        while (metrics.value().firm_gross_notional_minor > previous &&
               !worst_observed.compare_exchange_weak(
                   previous, metrics.value().firm_gross_notional_minor)) {
        }
      }
      std::this_thread::yield();
    }
  });

  Barrier barrier(kWorkers);
  std::atomic<int> allowed{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd =
          order("ACC-" + std::to_string(i), "chain-" + std::to_string(i),
                "cmd-" + std::to_string(i), kQty);
      barrier.arrive_and_wait();
      if (env->coordinator().reserve_new(cmd, trader).allowed()) allowed.fetch_add(1);
    });
  }
  for (std::thread& t : workers) t.join();
  running.store(false, std::memory_order_release);
  watcher.join();

  CHECK_EQ(allowed.load(), kAllowed);
  CHECK(samples.load() > 0);
  CHECK_MSG(worst_observed.load() <= kOrderNotional * kAllowed,
            "observed " + std::to_string(worst_observed.load()) + " > cap " +
                std::to_string(kOrderNotional * kAllowed));
}

TEST(concurrency_idempotency, the_same_command_from_many_threads_commits_once) {
  // PB-02: 10,000 retries in the spec; 64 concurrent ones here, which is what
  // actually exercises the race. Exactly one reservation, one intent.
  constexpr int kWorkers = 64;
  std::unique_ptr<LabEnv> env = make_env(lab_policy());

  Barrier barrier(kWorkers);
  std::atomic<int> allowed{0};
  std::atomic<int> conflicts{0};
  std::atomic<int> holds{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);

  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&] {
      const NewOrderCommand cmd =
          order("ACC-A", "the-one-chain", "the-one-command", 10000);
      barrier.arrive_and_wait();
      const RiskResult r = env->coordinator().reserve_new(cmd, env->trader());
      switch (r.decision) {
        case RiskDecision::AllowCommitted: allowed.fetch_add(1); break;
        case RiskDecision::Conflict: conflicts.fetch_add(1); break;
        case RiskDecision::HoldRetryable: holds.fetch_add(1); break;
        default: break;
      }
    });
  }
  for (std::thread& t : workers) t.join();

  // Every caller is told the same thing, and there is no conflict: the payload
  // is identical, so each one either commits or replays the committed answer.
  CHECK_EQ(allowed.load(), kWorkers);
  CHECK_EQ(conflicts.load(), 0);
  CHECK_EQ(holds.load(), 0);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 10000);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(concurrency_idempotency, concurrent_conflicting_payloads_change_nothing) {
  // One thread's payload wins; the rest, sharing the id with a different
  // quantity, must all be told CONFLICT (RI-02).
  constexpr int kWorkers = 32;
  std::unique_ptr<LabEnv> env = make_env(lab_policy());

  Barrier barrier(kWorkers);
  std::atomic<int> allowed{0};
  std::atomic<int> conflicts{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);

  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      // Every worker proposes a different quantity under one command id.
      const NewOrderCommand cmd =
          order("ACC-A", "shared-chain", "shared-id", 1000 * (i + 1));
      barrier.arrive_and_wait();
      const RiskResult r = env->coordinator().reserve_new(cmd, env->trader());
      if (r.decision == RiskDecision::AllowCommitted) allowed.fetch_add(1);
      if (r.decision == RiskDecision::Conflict) conflicts.fetch_add(1);
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(allowed.load(), 1);
  CHECK_EQ(conflicts.load(), kWorkers - 1);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(concurrency_events, fills_interleave_without_drift) {
  // Many chains filled concurrently. The projection must stay exactly
  // derivable from the reservation ledger (RI-23 cross-check).
  constexpr int kChains = 12;
  RiskPolicy policy = lab_policy();
  policy.limits.max_working_orders_per_account = 256;
  policy.limits.max_working_orders_per_account_symbol = 256;
  std::unique_ptr<LabEnv> env = make_env(policy);

  for (int i = 0; i < kChains; ++i) {
    const std::string id = std::to_string(i);
    CHECK(env->new_order("ACC-A", "chain-" + id, "EURUSD", Side::Buy, 10000, kPrice,
                         "seed-" + id)
              .allowed());
    CHECK(env->dispatch("seed-" + id).allowed());
    CHECK(env->lp_accept("ACC-A", "chain-" + id, "ack-" + id).allowed());
  }
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 10000 * kChains);

  Barrier barrier(kChains);
  std::atomic<int> applied{0};
  std::vector<std::thread> workers;
  workers.reserve(kChains);
  for (int i = 0; i < kChains; ++i) {
    workers.emplace_back([&, i] {
      const std::string id = std::to_string(i);
      barrier.arrive_and_wait();
      for (int f = 0; f < 2; ++f) {
        const RiskResult r =
            env->lp_fill("ACC-A", "chain-" + id,
                         "exec-" + id + "-" + std::to_string(f), 2500);
        if (r.allowed()) applied.fetch_add(1);
      }
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(applied.load(), kChains * 2);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 5000 * kChains);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 5000 * kChains);
  // P + B is still the original total.
  CHECK_EQ(env->position("ACC-A", "EURUSD") +
               env->effective_remaining("ACC-A", "EURUSD", Side::Buy),
           10000 * kChains);
  CHECK(env->coordinator().cross_check_projection().ok());
  CHECK(verify_audit_chain(env->store().audit()).ok);
}

TEST(concurrency_events, the_same_fill_from_many_threads_applies_once) {
  // RI-03 under contention: one canonical execution, many deliveries.
  constexpr int kWorkers = 32;
  std::unique_ptr<LabEnv> env = make_env(lab_policy());
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());

  VerifiedOrderEvent fill;
  fill.tenant_id = "T1";
  fill.account_id = "ACC-A";
  fill.order_chain_id = "chain-1";
  fill.source_namespace = "lp";
  fill.event_id = "exec-1";
  fill.kind = OrderEventKind::AppliedFill;
  fill.symbol = "EURUSD";
  fill.side = Side::Buy;
  fill.fill_units = 40000;
  fill.verified_cum_units = 40000;

  Barrier barrier(kWorkers);
  std::atomic<int> quarantined{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&] {
      barrier.arrive_and_wait();
      const RiskResult r = env->coordinator().apply_verified_order_event(fill);
      if (r.decision == RiskDecision::Quarantined) quarantined.fetch_add(1);
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(quarantined.load(), 0);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 60000);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(concurrency_fencing, two_coordinators_cannot_both_write) {
  // RI-25 / PB-21: two writers share one store, as two processes would. The
  // one holding the lower fencing token is refused at the storage boundary.
  LabOptions options;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  RiskCoordinator::Options second_options;
  second_options.tenant_id = "T1";
  second_options.domain_id = "D1";
  second_options.fencing_token = 2;  // the new leader
  second_options.token_secret = options.token_secret;
  RiskCoordinator new_leader(env->store(), env->feed(), env->clock(),
                             env->failpoints(), second_options);

  CHECK(new_leader
            .reserve_new(order("ACC-A", "chain-new", "new-leader-1", 10000),
                         env->trader())
            .allowed());

  // The old leader (token 1) now writes with a stale token.
  const RiskResult stale = env->coordinator().reserve_new(
      order("ACC-A", "chain-old", "old-leader-1", 10000), env->trader());
  CHECK(stale.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(stale.reason), static_cast<int>(Reason::FencingTokenStale));
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(concurrency_fencing, a_fenced_writer_racing_the_new_leader_commits_nothing) {
  constexpr int kWorkers = 8;
  LabOptions options;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  RiskCoordinator::Options leader_options;
  leader_options.tenant_id = "T1";
  leader_options.domain_id = "D1";
  leader_options.fencing_token = 10;
  leader_options.token_secret = options.token_secret;
  RiskCoordinator leader(env->store(), env->feed(), env->clock(),
                         env->failpoints(), leader_options);

  // Establish the higher token durably.
  CHECK(leader.reserve_new(order("ACC-A", "chain-boot", "leader-boot", 1000),
                           env->trader())
            .allowed());

  Barrier barrier(kWorkers);
  std::atomic<int> stale_allowed{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd =
          order("ACC-A", "chain-stale-" + std::to_string(i),
                "stale-" + std::to_string(i), 1000);
      barrier.arrive_and_wait();
      if (env->coordinator().reserve_new(cmd, env->trader()).allowed()) {
        stale_allowed.fetch_add(1);
      }
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(stale_allowed.load(), 0);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
}

TEST(concurrency_backpressure, the_admission_concurrency_cap_is_enforced) {
  // §17: over the cap the answer is BUSY, never a partial commit. The cap is
  // set to 1 and the first worker is parked inside the critical section by a
  // hook, so the overflow is deterministic rather than timing-dependent.
  constexpr int kWorkers = 6;
  LabOptions options;
  options.policy.queues.admission_cap = 1;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());
  const Principal trader = env->trader();

  std::atomic<int> inside{0};
  std::atomic<bool> release{false};
  env->failpoints().arm_hook(
      Failpoint::AfterSnapshotBeforeMath,
      [&] {
        inside.fetch_add(1);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
      },
      1);

  std::atomic<int> allowed{0};
  std::atomic<int> busy{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd = order("ACC-A", "chain-" + std::to_string(i),
                                        "cmd-" + std::to_string(i), 1000);
      const RiskResult r = env->coordinator().reserve_new(cmd, trader);
      if (r.allowed()) allowed.fetch_add(1);
      if (r.reason == Reason::QueueFull) busy.fetch_add(1);
    });
  }

  // Wait for the parked worker, then let everyone through.
  while (inside.load() == 0) std::this_thread::yield();
  for (int spin = 0; spin < 10000; ++spin) std::this_thread::yield();
  release.store(true, std::memory_order_release);
  for (std::thread& t : workers) t.join();

  CHECK_MSG(busy.load() > 0, "the concurrency cap never engaged");
  CHECK_EQ(allowed.load() + busy.load(), kWorkers);
  // Every BUSY answer committed nothing, so holds and intents must match.
  CHECK_EQ(env->store().reservations().size(),
           static_cast<std::size_t>(allowed.load()));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(allowed.load()));
  CHECK(env->coordinator().cross_check_projection().ok());
  env->failpoints().disarm_all();
}

TEST(concurrency_retry, a_serialisation_retry_does_not_duplicate_an_intent) {
  // PB-22: a retry storm. The revision CAS makes a losing writer retry with
  // the same command id, which must not produce a second intent.
  constexpr int kWorkers = 16;
  std::unique_ptr<LabEnv> env = make_env(lab_policy());
  const Principal trader = env->trader();

  Barrier barrier(kWorkers);
  std::atomic<int> committed{0};
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd = order("ACC-A", "chain-" + std::to_string(i),
                                        "cmd-" + std::to_string(i), 1000);
      barrier.arrive_and_wait();
      // Bounded retries with the same command id, as §15.2 requires.
      for (int attempt = 0; attempt < 16; ++attempt) {
        const RiskResult r = env->coordinator().reserve_new(cmd, trader);
        if (r.allowed()) {
          committed.fetch_add(1);
          return;
        }
        if (r.decision != RiskDecision::HoldRetryable) return;
        std::this_thread::yield();
      }
    });
  }
  for (std::thread& t : workers) t.join();

  CHECK_EQ(committed.load(), kWorkers);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(kWorkers));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(kWorkers));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 1000 * kWorkers);
  CHECK(env->coordinator().cross_check_projection().ok());
  CHECK(verify_audit_chain(env->store().audit()).ok);
}

TEST(concurrency_mode, a_kill_switch_racing_admissions_is_linearised) {
  // PB-24: flipping the mode while admissions run. Every committed admission
  // must sit at a revision before the mode change, so the audit trail cannot
  // contain an admission that slipped past the kill switch.
  constexpr int kWorkers = 12;
  std::unique_ptr<LabEnv> env = make_env(lab_policy());
  const Principal trader = env->trader();

  Barrier barrier(kWorkers + 1);
  std::atomic<int> allowed{0};
  std::atomic<int> blocked{0};

  std::vector<std::thread> workers;
  workers.reserve(kWorkers + 1);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      const NewOrderCommand cmd = order("ACC-A", "chain-" + std::to_string(i),
                                        "cmd-" + std::to_string(i), 1000);
      barrier.arrive_and_wait();
      const RiskResult r = env->coordinator().reserve_new(cmd, trader);
      if (r.allowed()) allowed.fetch_add(1);
      if (r.reason == Reason::KillSwitchNoNew) blocked.fetch_add(1);
    });
  }
  workers.emplace_back([&] {
    barrier.arrive_and_wait();
    (void)env->coordinator().set_mode(DomainMode::NoNew, env->operator_principal(),
                                      "race");
  });
  for (std::thread& t : workers) t.join();

  CHECK(env->store().domain_state().mode == DomainMode::NoNew);
  CHECK_EQ(allowed.load() + blocked.load(), kWorkers);

  Revision mode_revision = 0;
  for (const AuditEvent& e : env->store().audit()) {
    if (e.event_type == "ModeChanged" &&
        e.note.find("-> NO_NEW") != std::string::npos) {
      mode_revision = e.commit_revision;
    }
  }
  CHECK(mode_revision > 0);
  for (const AuditEvent& e : env->store().audit()) {
    if (e.event_type != "NewCommitted") continue;
    CHECK_MSG(e.commit_revision < mode_revision,
              "an admission committed at revision " +
                  std::to_string(e.commit_revision) +
                  " at or after the kill switch at " +
                  std::to_string(mode_revision));
  }
  CHECK(env->coordinator().cross_check_projection().ok());
}
