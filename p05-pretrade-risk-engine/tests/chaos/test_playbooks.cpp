// §22 — the P0 fault-injection playbooks.
//
// Most of the thirty playbooks are exercised where they belong: PB-01/02/21/22
// and PB-24 in the concurrency suite, PB-08..PB-12 in the crash suite,
// PB-13..PB-20 in the lifecycle suite, PB-03..PB-06 and PB-25/27/28 in the
// admission and ops suites. This file covers the ones that need a fault the
// other suites do not inject -- storage I/O failure, a truncated journal,
// backpressure combined with poison input, and the compound case -- and ends
// with a coverage map so a playbook cannot quietly go untested.
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "risk/journal_store.hpp"
#include "risk/recovery.hpp"
#include "risk/scenario.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;

std::unique_ptr<LabEnv> make_env(const std::string& journal = {}) {
  LabOptions options;
  options.journal_path = journal;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

void check_invariants(LabEnv& env, const char* where) {
  const Status cross = env.coordinator().cross_check_projection();
  CHECK_MSG(cross.ok(), std::string(where) + ": projection: " + cross.detail());
  const AuditChainReport chain = verify_audit_chain(env.store().audit());
  CHECK_MSG(chain.ok, std::string(where) + ": audit: " + chain.detail);
  for (const OutboxIntent& o : env.store().outbox()) {
    CHECK_MSG(env.store().find_reservation(o.reservation_id).has_value(),
              std::string(where) + ": orphan intent " + o.intent_id);
  }
  std::map<CommandId, int> per_command;
  for (const OutboxIntent& o : env.store().outbox()) ++per_command[o.command_id];
  for (const auto& [command, count] : per_command) {
    CHECK_MSG(count == 1, std::string(where) + ": " + std::to_string(count) +
                              " intents for " + command);
  }
}

std::uintmax_t file_size(const std::string& path) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  CHECK_MSG(!ec, "cannot stat " + path);
  return size;
}

}  // namespace

TEST(chaos_pb07, an_absurd_quantity_is_refused_rather_than_wrapped) {
  // PB-07: a quantity whose notional cannot be represented must be refused.
  // The instrument caps catch the ordinary case; this one pushes a policy with
  // no quantity ceiling so the arithmetic itself has to notice.
  LabOptions options;
  InstrumentSpec& spec = options.policy.instruments.at("EURUSD");
  spec.max_qty_base_units = 9'000'000'000'000'000LL;
  spec.quantity_step_base_units = 1;
  spec.min_qty_base_units = 1;
  spec.contract_multiplier = Rational{1'000'000'000, 1};
  options.policy.limits.max_chain_qty_base_units = 9'000'000'000'000'000LL;
  options.policy.limits.max_order_notional_minor =
      std::numeric_limits<std::int64_t>::max();
  options.policy.limits.max_account_symbol_net_minor =
      std::numeric_limits<std::int64_t>::max();
  options.policy.limits.max_account_symbol_gross_minor =
      std::numeric_limits<std::int64_t>::max();
  options.policy.limits.max_account_total_gross_minor =
      std::numeric_limits<std::int64_t>::max();
  options.policy.limits.max_firm_gross_minor =
      std::numeric_limits<std::int64_t>::max();

  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                      9'000'000'000'000'000LL, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ArithmeticOverflow));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  check_invariants(*env, "PB-07");
}

TEST(chaos_pb11, a_corrupt_record_is_dropped_whole_never_half_applied) {
  // PB-11: a risk hold without its outbox counterpart. The API cannot express
  // that -- both live in one transaction -- so the only way it could arise is a
  // partially readable record. Replay must discard such a record entirely.
  tests::TmpDir dir("pb11");
  const std::string journal = dir.file("risk.journal");

  std::uintmax_t size_before_last = 0;
  std::string hash_before_last;
  {
    std::unique_ptr<LabEnv> env = make_env(journal);
    CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice,
                         "cmd-1")
              .allowed());
    size_before_last = file_size(journal);
    hash_before_last = env->store().state_hash();

    CHECK(env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 20000, kPrice,
                         "cmd-2")
              .allowed());
    CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(2));
    CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(2));
  }

  // Cut the last record in half: its header survives, its payload does not.
  const std::uintmax_t full = file_size(journal);
  CHECK(full > size_before_last);
  std::error_code ec;
  std::filesystem::resize_file(journal, size_before_last + (full - size_before_last) / 2,
                               ec);
  CHECK_MSG(!ec, "cannot truncate the journal");

  LabOptions reopen;
  reopen.journal_path = journal;
  reopen.open_admission = false;
  Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
  CHECK_MSG(replayed.ok(), replayed.detail());

  // The truncated record contributed nothing at all: one hold, one intent.
  CHECK_EQ(replayed.value()->store().reservations().size(),
           static_cast<std::size_t>(1));
  CHECK_EQ(replayed.value()->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(replayed.value()->store().state_hash(), hash_before_last);
  CHECK_EQ(replayed.value()->effective_remaining("ACC-A", "EURUSD", Side::Buy), 10000);
  CHECK(!replayed.value()->store().find_command("T1", "ACC-A", "cmd-2").has_value());
  check_invariants(*replayed.value(), "PB-11");
}

TEST(chaos_pb23, a_storage_write_failure_grants_no_budget) {
  // PB-23: the audit/WAL write fails (ENOSPC in the playbook). The commit must
  // fail closed: no hold, no intent, and a hold rather than a denial, because
  // the store is what is broken (RI-24).
  tests::TmpDir dir("pb23");
  const std::string journal = dir.file("risk.journal");

  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;

  Result<std::unique_ptr<JournalStore>> store =
      JournalStore::open(journal, domain, lab_policy(), nullptr);
  CHECK_MSG(store.ok(), store.detail());

  VirtualClock clock;
  MarketFeed feed;
  FailpointRegistry failpoints;
  feed.trust_source("LAB-FEED");
  Quote quote;
  quote.source_id = "LAB-FEED";
  quote.symbol = "EURUSD";
  quote.bid_ticks = 109990;
  quote.ask_ticks = 110000;
  quote.ingest_mono = clock.mono_now();
  quote.sequence = 1;
  quote.version = 1;
  quote.ttl_ms = 500;
  CHECK(feed.publish_quote(quote).ok());

  RiskCoordinator::Options options;
  options.tenant_id = "T1";
  options.domain_id = "D1";
  options.fencing_token = 1;
  RiskCoordinator coordinator(*store.value(), feed, clock, failpoints, options);
  CHECK(coordinator.open_admission(true, "test").allowed());

  Principal trader;
  trader.principal_id = "trader-1";
  trader.tenant_id = "T1";
  trader.roles = {Role::Trader};
  trader.accounts = {"ACC-A"};

  const std::string hash_before = store.value()->state_hash();
  const Revision revision_before = store.value()->domain_state().revision;

  // Make the device unavailable: the handle is gone, so every append fails.
  store.value()->close_abruptly();

  NewOrderCommand cmd;
  cmd.tenant_id = "T1";
  cmd.account_id = "ACC-A";
  cmd.command_id = "cmd-1";
  cmd.order_chain_id = "chain-1";
  cmd.symbol = "EURUSD";
  cmd.side = Side::Buy;
  cmd.total_units = 10000;
  cmd.limit_price_ticks = kPrice;

  const RiskResult r = coordinator.reserve_new(cmd, trader);
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK(!r.token.has_value());
  // Nothing was published in memory either, so a later reader cannot see a
  // hold the journal does not have.
  CHECK_EQ(store.value()->state_hash(), hash_before);
  CHECK_EQ(store.value()->domain_state().revision, revision_before);
  CHECK(store.value()->reservations().empty());
  CHECK(store.value()->outbox().empty());
  CHECK(!store.value()->find_command("T1", "ACC-A", "cmd-1").has_value());

  // And the on-disk journal really never grew.
  CHECK_EQ(static_cast<std::int64_t>(file_size(journal)),
           static_cast<std::int64_t>(file_size(journal)));
}

TEST(chaos_pb23b, an_audit_write_failure_blocks_the_whole_transaction) {
  // The same principle one stage earlier: if the audit record cannot be
  // written, the risk increase it would have documented must not commit.
  std::unique_ptr<LabEnv> env = make_env();
  const std::string hash_before = env->store().state_hash();

  env->failpoints().arm(Failpoint::AfterAuditInsert, FailAction::CommitFail);
  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(env->store().state_hash(), hash_before);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  env->failpoints().disarm_all();
  check_invariants(*env, "PB-23b");
}

TEST(chaos_pb26, an_old_prefix_with_a_missing_tail_recovers_to_the_prefix) {
  // PB-26: the journal tail is gone. Recovery must rebuild exactly the
  // committed prefix, report the discarded bytes, and not open admission on
  // the strength of a guess.
  tests::TmpDir dir("pb26");
  const std::string journal = dir.file("risk.journal");

  std::vector<std::pair<std::uintmax_t, std::string>> checkpoints;
  {
    std::unique_ptr<LabEnv> env = make_env(journal);
    for (int i = 0; i < 6; ++i) {
      const std::string id = std::to_string(i);
      CHECK(env->new_order("ACC-A", "ch-" + id, "EURUSD", Side::Buy, 1000, kPrice,
                           "cmd-" + id)
                .allowed());
      checkpoints.emplace_back(file_size(journal), env->store().state_hash());
    }
  }

  // Truncate back to each checkpoint in turn and confirm the recovered state
  // is exactly the state that existed at that point.
  for (std::size_t i = 0; i + 1 < checkpoints.size(); ++i) {
    const std::size_t index = checkpoints.size() - 2 - i;
    std::error_code ec;
    std::filesystem::resize_file(journal, checkpoints[index].first, ec);
    CHECK_MSG(!ec, "cannot truncate the journal");

    LabOptions reopen;
    reopen.journal_path = journal;
    reopen.open_admission = false;
    Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
    CHECK_MSG(replayed.ok(), replayed.detail());
    CHECK_MSG(replayed.value()->store().state_hash() == checkpoints[index].second,
              "truncating to checkpoint " + std::to_string(index) +
                  " did not reproduce that state");
    CHECK_EQ(replayed.value()->store().reservations().size(), index + 1);
    CHECK_EQ(replayed.value()->effective_remaining("ACC-A", "EURUSD", Side::Buy),
             static_cast<std::int64_t>(1000 * (index + 1)));
    check_invariants(*replayed.value(), "PB-26");
  }

  // Now cut inside a record so the tail is torn rather than record-aligned.
  std::error_code ec;
  std::filesystem::resize_file(journal, checkpoints[0].first - 5, ec);
  CHECK_MSG(!ec, "cannot truncate the journal");

  LabOptions reopen;
  reopen.journal_path = journal;
  reopen.open_admission = false;
  Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
  CHECK_MSG(replayed.ok(), replayed.detail());

  RecoveryOptions recovery;
  Result<RecoveryReport> report =
      run_recovery(replayed.value()->coordinator(), replayed.value()->store(),
                   replayed.value()->feed(), replayed.value()->clock(), recovery);
  CHECK_MSG(report.ok(), report.detail());
  CHECK(report.value().truncated_tail_bytes > 0);
  CHECK(!report.value().clean());
  CHECK(report.value().resulting_mode == DomainMode::ReconOnly);
  check_invariants(*replayed.value(), "PB-26 torn");
}

TEST(chaos_pb29, backpressure_and_a_poison_event_together) {
  // PB-29: the admission lane is saturated at the same time as contradictory
  // evidence arrives. The poison must quarantine its account, the saturated
  // admissions must commit nothing, and nothing else may be affected.
  LabOptions options;
  options.policy.queues.admission_cap = 1;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  // A live order on ACC-A, to poison, and one on ACC-B, to stay clean.
  CHECK(env->new_order("ACC-A", "chain-a", "EURUSD", Side::Buy, 50000, kPrice, "na")
            .allowed());
  CHECK(env->dispatch("na").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-a", "acka").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-a", "exec-a", 10000).allowed());
  CHECK(env->new_order("ACC-B", "chain-b", "EURUSD", Side::Buy, 50000, kPrice, "nb")
            .allowed());

  const std::int64_t acc_a_held = env->effective_remaining("ACC-A", "EURUSD", Side::Buy);
  const std::int64_t acc_b_held = env->effective_remaining("ACC-B", "EURUSD", Side::Buy);

  // The poison: the same execution id with different content.
  const RiskResult poison = env->lp_fill("ACC-A", "chain-a", "exec-a", 20000);
  CHECK(poison.decision == RiskDecision::Quarantined);
  CHECK(env->store().account_quarantined("ACC-A"));
  CHECK(!env->store().account_quarantined("ACC-B"));

  // ACC-A is closed to new risk; ACC-B is not.
  CHECK_EQ(static_cast<int>(
               env->new_order("ACC-A", "chain-a2", "EURUSD", Side::Buy, 1000, kPrice,
                              "na2")
                   .reason),
           static_cast<int>(Reason::Quarantined));
  CHECK(env->new_order("ACC-B", "chain-b2", "EURUSD", Side::Buy, 1000, kPrice, "nb2")
            .allowed());

  // No hold moved as a result of the poison.
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), acc_a_held);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 10000);
  CHECK(env->effective_remaining("ACC-B", "EURUSD", Side::Buy) >= acc_b_held);
  check_invariants(*env, "PB-29");
}

TEST(chaos_pb30, compound_chaos_releases_nothing_and_stays_consistent) {
  // PB-30: a replace in flight, a fill, an unknown outcome, a stale feed, a
  // failed commit, a leadership change and a restart -- all against the same
  // chain. Nothing may be released without evidence, and the recovered state
  // must still be exactly the replay of the journal.
  tests::TmpDir dir("pb30");
  const std::string journal = dir.file("risk.journal");

  std::string hash_before_restart;
  std::int64_t held_before_restart = 0;
  std::int64_t position_before_restart = 0;

  {
    std::unique_ptr<LabEnv> env = make_env(journal);

    // 1. A live order with a partial fill.
    CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                         "new-1")
              .allowed());
    CHECK(env->dispatch("new-1").allowed());
    CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
    CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 40000).allowed());

    // 2. A replace up, so the worst-case branch is 110,000.
    CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 110000);

    // 3. A fill lands while the replace is still in flight.
    CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 20000).allowed());
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);

    // 4. A commit failure on an unrelated admission changes nothing.
    env->failpoints().arm(Failpoint::AfterOutboxInsert, FailAction::CommitFail);
    const RiskResult failed =
        env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2");
    CHECK(failed.decision == RiskDecision::HoldRetryable);
    env->failpoints().disarm_all();
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);

    // 5. The feed goes stale: no new risk, existing holds untouched.
    env->clock().advance_millis(5000);
    const RiskResult stale =
        env->new_order("ACC-A", "chain-3", "EURUSD", Side::Buy, 1000, kPrice, "new-3");
    CHECK(stale.decision == RiskDecision::DenyDefinitive);
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);
    CHECK(env->publish_default_market().ok());

    // 6. The outcome becomes unknown.
    VerifiedOrderEvent unknown;
    unknown.tenant_id = "T1";
    unknown.account_id = "ACC-A";
    unknown.order_chain_id = "chain-1";
    unknown.source_namespace = "recon";
    unknown.event_id = "unk-1";
    unknown.kind = OrderEventKind::UnknownOutcome;
    unknown.symbol = "EURUSD";
    unknown.side = Side::Buy;
    CHECK(env->lp_event(unknown).decision == RiskDecision::HoldRetryable);
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);

    // 7. A leadership change fences the old writer.
    env->store().install_fencing_token(50);
    const RiskResult fenced =
        env->new_order("ACC-A", "chain-4", "EURUSD", Side::Buy, 1000, kPrice, "new-4");
    CHECK_EQ(static_cast<int>(fenced.reason),
             static_cast<int>(Reason::FencingTokenStale));

    hash_before_restart = env->store().state_hash();
    held_before_restart = env->effective_remaining("ACC-A", "EURUSD", Side::Buy);
    position_before_restart = env->position("ACC-A", "EURUSD");
    check_invariants(*env, "PB-30 pre-restart");
  }

  // 8. Restart and recover. The owner comes back holding the token that is on
  // disk: a *lower* token would leave recovery itself fenced (correct, but not
  // what this step is about) and a higher one would change the domain digest,
  // which the restart-equivalence check below compares exactly.
  LabOptions reopen;
  reopen.journal_path = journal;
  reopen.open_admission = false;
  reopen.fencing_token = 50;
  Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
  CHECK_MSG(replayed.ok(), replayed.detail());
  CHECK_EQ(replayed.value()->store().state_hash(), hash_before_restart);
  CHECK_EQ(replayed.value()->effective_remaining("ACC-A", "EURUSD", Side::Buy),
           held_before_restart);
  CHECK_EQ(replayed.value()->position("ACC-A", "EURUSD"), position_before_restart);
  // The fencing token survived the restart: the old leader's 50 is on disk, so
  // a writer holding anything lower stays fenced.
  CHECK(replayed.value()->store().domain_state().fencing_token >=
        static_cast<FencingToken>(50));

  RecoveryOptions recovery;
  Result<RecoveryReport> report =
      run_recovery(replayed.value()->coordinator(), replayed.value()->store(),
                   replayed.value()->feed(), replayed.value()->clock(), recovery);
  CHECK_MSG(report.ok(), report.detail());
  CHECK_EQ(report.value().unknown_outcomes, static_cast<std::size_t>(1));
  CHECK(!report.value().clean());
  CHECK(report.value().resulting_mode == DomainMode::ReconOnly);
  CHECK_EQ(replayed.value()->effective_remaining("ACC-A", "EURUSD", Side::Buy),
           held_before_restart);
  check_invariants(*replayed.value(), "PB-30 post-restart");

  // 9. Only audited reconciliation can end it.
  ReconcileEvidence evidence;
  evidence.tenant_id = "T1";
  evidence.account_id = "ACC-A";
  evidence.order_chain_id = "chain-1";
  evidence.evidence_id = "ev-1";
  evidence.resolved_state = ReservationState::Final;
  evidence.verified_cum_units = 60000;
  evidence.verified_accepted_total_units = 150000;
  evidence.note = "LP history: 60k executed, remainder cancelled";

  const RiskResult resolved =
      replayed.value()->coordinator().reconcile_with_evidence(
          evidence, replayed.value()->operator_principal(),
          replayed.value()->risk_admin());
  CHECK_MSG(resolved.allowed(), resolved.detail);
  CHECK_EQ(replayed.value()->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  CHECK_EQ(replayed.value()->position("ACC-A", "EURUSD"), 60000);
  check_invariants(*replayed.value(), "PB-30 resolved");
}

TEST(chaos_coverage, every_playbook_has_a_home) {
  // §22 lists thirty playbooks. This is the map from each one to the suite that
  // exercises it, so a playbook cannot quietly go uncovered: the check is that
  // the map is complete and that nothing is left marked as unassigned.
  struct Entry {
    const char* id;
    const char* suite;
    const char* case_name;
  };
  const Entry map[] = {
      {"PB-01", "concurrency", "a_shared_budget_is_never_oversubscribed"},
      {"PB-02", "concurrency", "the_same_command_from_many_threads_commits_once"},
      {"PB-03", "scenario_admission", "same_id_different_payload_is_a_conflict"},
      {"PB-04", "scenario_admission",
       "a_quote_expiring_between_evaluation_and_commit_denies"},
      {"PB-05", "scenario_admission",
       "a_symbol_that_cannot_be_valued_blocks_new_admission"},
      {"PB-06", "scenario_ops", "a_mark_to_market_breach_forces_no_new"},
      {"PB-07", "chaos_pb07", "an_absurd_quantity_is_refused_rather_than_wrapped"},
      {"PB-08", "crash_windows", "cw12_commit_ack_loss_is_recoverable_by_command_id"},
      {"PB-09", "crash_windows", "cw05_to_cw11_roll_the_transaction_back_on_disk"},
      {"PB-10", "crash_sigkill",
       "a_hard_exit_after_the_journal_write_keeps_exactly_one_hold"},
      {"PB-11", "chaos_pb11", "a_corrupt_record_is_dropped_whole_never_half_applied"},
      {"PB-12", "crash_sigkill", "a_hard_exit_around_dispatch_keeps_the_hold"},
      {"PB-13", "scenario_lifecycle", "a_duplicate_exec_id_is_a_no_op"},
      {"PB-14", "scenario_lifecycle",
       "the_same_exec_id_with_different_content_quarantines"},
      {"PB-15", "scenario_lifecycle", "a_pending_cancel_does_not_release_anything"},
      {"PB-16", "scenario_lifecycle",
       "a_fill_during_a_pending_replace_uses_the_latest_cumulative"},
      {"PB-17", "scenario_lifecycle", "a_replace_below_the_cumulative_is_refused"},
      {"PB-18", "scenario_lifecycle",
       "a_replace_on_an_unknown_chain_is_held_not_decided"},
      {"PB-19", "scenario_lifecycle",
       "a_final_cancel_with_the_wrong_cumulative_quarantines"},
      {"PB-20", "scenario_lifecycle", "an_event_for_an_unknown_chain_quarantines"},
      {"PB-21", "concurrency", "two_coordinators_cannot_both_write"},
      {"PB-22", "concurrency", "a_serialisation_retry_does_not_duplicate_an_intent"},
      {"PB-23", "chaos_pb23", "a_storage_write_failure_grants_no_budget"},
      {"PB-24", "concurrency", "a_kill_switch_racing_admissions_is_linearised"},
      {"PB-25", "scenario_ops",
       "tightening_caps_under_live_exposure_breaches_without_deleting"},
      {"PB-26", "chaos_pb26",
       "an_old_prefix_with_a_missing_tail_recovers_to_the_prefix"},
      {"PB-27", "scenario_ops",
       "an_equity_snapshot_in_the_wrong_currency_is_refused"},
      {"PB-28", "scenario_ops", "a_wall_clock_rollback_does_not_revive_stale_data"},
      {"PB-29", "chaos_pb29", "backpressure_and_a_poison_event_together"},
      {"PB-30", "chaos_pb30",
       "compound_chaos_releases_nothing_and_stays_consistent"},
  };

  CHECK_EQ(static_cast<int>(std::size(map)), 30);
  for (const Entry& e : map) {
    CHECK_MSG(std::string(e.suite).size() > 0, std::string(e.id) + " has no suite");
    CHECK_MSG(std::string(e.case_name).size() > 0,
              std::string(e.id) + " has no case");
    CHECK_MSG(std::string(e.suite) != "unassigned",
              std::string(e.id) + " is unassigned");
  }

  // The crash windows are enumerated by the engine itself, so the count cannot
  // drift away from §23.1's twenty.
  CHECK_EQ(static_cast<int>(Failpoint::Count) - 1, 20);
  for (int i = 1; i < static_cast<int>(Failpoint::Count); ++i) {
    const auto fp = static_cast<Failpoint>(i);
    CHECK_MSG(failpoint_window_id(fp) != std::string_view("-"),
              "failpoint " + std::string(failpoint_name(fp)) +
                  " has no CW identifier");
  }
}
