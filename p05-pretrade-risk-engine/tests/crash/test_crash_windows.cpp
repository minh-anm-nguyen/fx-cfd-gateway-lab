// §23.1 — the twenty named crash windows, plus the restart-equivalence oracle.
//
// Two flavours run here. The in-process cases arm a failpoint, observe the
// decision and then reopen the journal to prove the durable state matches. The
// out-of-process cases launch a child that leaves via _Exit at the window, so
// the journal is observed exactly as it reached the device -- no destructor, no
// buffered flush, no atexit handler.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "risk/recovery.hpp"
#include "risk/scenario.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;
constexpr std::int64_t kQty = 50000;

std::string crash_child_path() {
  std::string raw = "build/risk_crash_child.exe";
  if (const char* env = std::getenv("RISK_CRASH_CHILD"); env != nullptr) raw = env;
  // std::system goes through cmd.exe on Windows, which does not accept forward
  // slashes in the program path, so normalise to the platform's separator.
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(raw, ec);
  if (ec) return raw;
  return absolute.make_preferred().string();
}

// Reopens the journal in a fresh environment and runs the recovery gates, the
// way a restarted process would.
struct Recovered {
  std::unique_ptr<LabEnv> env;
  RecoveryReport report;
};

Recovered reopen(const std::string& journal_path) {
  LabOptions options;
  options.journal_path = journal_path;
  // Recovery decides whether admission opens; the harness must not pre-open it.
  options.open_admission = false;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(),
            std::string(reason_name(created.reason())) + ": " + created.detail());

  Recovered out;
  out.env = std::move(created.value());
  RecoveryOptions recovery;
  Result<RecoveryReport> report = run_recovery(out.env->coordinator(),
                                               out.env->store(), out.env->feed(),
                                               out.env->clock(), recovery);
  CHECK_MSG(report.ok(), report.detail());
  out.report = report.value();
  return out;
}

// Everything that must hold after any crash, at any window.
void check_post_crash_invariants(LabEnv& env, const char* where) {
  const Status cross = env.coordinator().cross_check_projection();
  CHECK_MSG(cross.ok(), std::string(where) + ": projection: " + cross.detail());

  const AuditChainReport chain = verify_audit_chain(env.store().audit());
  CHECK_MSG(chain.ok, std::string(where) + ": audit chain: " + chain.detail);

  // RI-01: no outbox row without a committed reservation behind it.
  for (const OutboxIntent& o : env.store().outbox()) {
    CHECK_MSG(env.store().find_reservation(o.reservation_id).has_value(),
              std::string(where) + ": orphan intent " + o.intent_id);
  }

  // Every surviving reservation is audited (RI-12).
  const Status audited =
      verify_every_reservation_audited(env.store().reservations(), env.store().audit());
  CHECK_MSG(audited.ok(), std::string(where) + ": " + audited.detail());

  // At most one intent per command id: no duplicate original message (RI-01).
  std::map<CommandId, int> per_command;
  for (const OutboxIntent& o : env.store().outbox()) ++per_command[o.command_id];
  for (const auto& [command, count] : per_command) {
    CHECK_MSG(count == 1, std::string(where) + ": " + std::to_string(count) +
                              " intents for command " + command);
  }
}

int run_child(const std::string& journal, const std::string& workload,
              const std::string& failpoint_spec) {
  const std::string inner = "\"" + crash_child_path() + "\" \"" + journal + "\" " +
                            workload +
                            (failpoint_spec.empty() ? "" : " " + failpoint_spec) +
                            " > \"" + journal + ".stdout\" 2>&1";
#ifdef _WIN32
  // cmd.exe strips the first and last quote of a command line that begins with
  // one, which mangles a quoted program path. Wrapping the whole thing in
  // `cmd /s /c "..."` is the documented way to keep the inner quoting intact;
  // without it the child silently never starts.
  const std::string command = "cmd /s /c \"" + inner + "\"";
#else
  const std::string command = inner;
#endif
  return std::system(command.c_str());
}

// Proves the harness end to end: the child must run a workload *without* a
// failpoint, exit cleanly, and leave a journal that really contains the
// reservation. Probing only the exit code would let "the child could not
// start" pass as "the child crashed where we armed it".
bool crash_child_available() {
  static const bool available = [] {
    tests::TmpDir dir("sigkill-probe");
    const std::string journal = dir.file("risk.journal");
    if (run_child(journal, "reserve_new", {}) != 0) return false;

    LabOptions options;
    options.journal_path = journal;
    options.open_admission = false;
    Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(options);
    if (!replayed.ok()) return false;
    return replayed.value()->store().reservations().size() == 1 &&
           replayed.value()->store().outbox().size() == 1;
  }();
  return available;
}

}  // namespace

// ---------------------------------------------------------------------------
// In-process windows
// ---------------------------------------------------------------------------

TEST(crash_windows, cw01_to_cw04_leave_no_trace) {
  // The pre-commit windows: a failure there must not produce a reservation, an
  // intent, or even a recorded decision.
  const Failpoint windows[] = {
      Failpoint::BeforeAuthz,
      Failpoint::AfterAuthzBeforeIdempotency,
      Failpoint::AfterSnapshotBeforeMath,
      Failpoint::AfterMathBeforeDomainLock,
  };

  for (const Failpoint fp : windows) {
    tests::TmpDir dir("cw-pre");
    const std::string journal = dir.file("risk.journal");
    {
      LabOptions options;
      options.journal_path = journal;
      Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
      CHECK_MSG(created.ok(), created.detail());
      std::unique_ptr<LabEnv> env = std::move(created.value());

      env->failpoints().arm(fp, FailAction::CommitFail);
      const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                          kQty, kPrice, "cmd-1");
      CHECK_MSG(r.decision == RiskDecision::HoldRetryable,
                std::string(failpoint_window_id(fp)) + " gave " +
                    std::string(risk_decision_name(r.decision)));
      CHECK(!r.token.has_value());
    }

    Recovered recovered = reopen(journal);
    CHECK_EQ(recovered.env->store().reservations().size(),
             static_cast<std::size_t>(0));
    CHECK_EQ(recovered.env->store().outbox().size(), static_cast<std::size_t>(0));
    CHECK(!recovered.env->store().find_command("T1", "ACC-A", "cmd-1").has_value());
    check_post_crash_invariants(*recovered.env, failpoint_window_id(fp).data());
  }
}

TEST(crash_windows, cw05_to_cw11_roll_the_transaction_back_on_disk) {
  const Failpoint windows[] = {
      Failpoint::AfterLockBeforeBegin,  Failpoint::AfterCommandInsert,
      Failpoint::AfterReservationInsert, Failpoint::AfterProjectionUpdate,
      Failpoint::AfterAuditInsert,      Failpoint::AfterOutboxInsert,
      Failpoint::AfterRevisionCas,
  };

  for (const Failpoint fp : windows) {
    tests::TmpDir dir("cw-tx");
    const std::string journal = dir.file("risk.journal");
    Revision revision_before = 0;
    std::string hash_before;
    {
      LabOptions options;
      options.journal_path = journal;
      Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
      CHECK_MSG(created.ok(), created.detail());
      std::unique_ptr<LabEnv> env = std::move(created.value());
      revision_before = env->store().domain_state().revision;
      hash_before = env->store().state_hash();

      env->failpoints().arm(fp, FailAction::CommitFail);
      const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                          kQty, kPrice, "cmd-1");
      CHECK_MSG(r.decision == RiskDecision::HoldRetryable,
                std::string(failpoint_window_id(fp)) + ": " + r.detail);
      // Nothing was published in memory either.
      CHECK_EQ(env->store().state_hash(), hash_before);
      CHECK_EQ(env->store().domain_state().revision, revision_before);
    }

    Recovered recovered = reopen(journal);
    CHECK_MSG(recovered.env->store().reservations().empty(),
              std::string(failpoint_window_id(fp)) + ": a hold survived a rollback");
    CHECK(recovered.env->store().outbox().empty());
    check_post_crash_invariants(*recovered.env, failpoint_window_id(fp).data());
  }
}

TEST(crash_windows, cw12_commit_ack_loss_is_recoverable_by_command_id) {
  // PB-08 / RI-24: the commit is durable; the acknowledgement is not. Restart
  // must find exactly one hold and one intent.
  tests::TmpDir dir("cw12");
  const std::string journal = dir.file("risk.journal");
  {
    LabOptions options;
    options.journal_path = journal;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    std::unique_ptr<LabEnv> env = std::move(created.value());

    env->failpoints().arm(Failpoint::AfterDurableCommitBeforeAck,
                          FailAction::CommitAckLost);
    const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                        kQty, kPrice, "cmd-1");
    CHECK(r.decision == RiskDecision::HoldRetryable);
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::PersistenceCommitUnknown));
  }

  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(recovered.env->store().outbox().size(), static_cast<std::size_t>(1));
  std::optional<CommandRecord> command =
      recovered.env->store().find_command("T1", "ACC-A", "cmd-1");
  CHECK(command.has_value());
  CHECK_EQ(command->decision, std::string("ALLOW_COMMITTED"));
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);

  // The intent was never dispatched, which recovery reports rather than
  // guessing at (CW-13/CW-14).
  CHECK_EQ(recovered.report.undispatched_intents, static_cast<std::size_t>(1));
  CHECK(!recovered.report.clean());
  check_post_crash_invariants(*recovered.env, "CW-12");
}

TEST(crash_windows, cw13_to_cw16_keep_the_hold_when_dispatch_is_uncertain) {
  // RI-26: nothing about an uncertain send releases anything.
  const struct {
    Failpoint fp;
    bool expect_dispatched;
  } windows[] = {
      {Failpoint::AfterAllowBeforeClaim, false},
      {Failpoint::AfterClaimBeforeSequence, false},
      {Failpoint::AfterSocketWriteBeforeAck, true},
  };

  for (const auto& w : windows) {
    tests::TmpDir dir("cw-dispatch");
    const std::string journal = dir.file("risk.journal");
    {
      LabOptions options;
      options.journal_path = journal;
      Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
      CHECK_MSG(created.ok(), created.detail());
      std::unique_ptr<LabEnv> env = std::move(created.value());

      CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice,
                           "cmd-1")
                .allowed());
      env->failpoints().arm(w.fp, FailAction::CommitFail);
      const RiskResult dispatched = env->dispatch("cmd-1");
      CHECK_MSG(dispatched.decision == RiskDecision::HoldRetryable,
                std::string(failpoint_window_id(w.fp)) + " gave " +
                    std::string(risk_decision_name(dispatched.decision)));
    }

    Recovered recovered = reopen(journal);
    CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
    CHECK_EQ(recovered.env->store().outbox().size(), static_cast<std::size_t>(1));
    CHECK_MSG(recovered.env->store().outbox()[0].dispatched == w.expect_dispatched,
              std::string(failpoint_window_id(w.fp)) + ": unexpected dispatch flag");
    check_post_crash_invariants(*recovered.env, failpoint_window_id(w.fp).data());
  }
}

TEST(crash_windows, cw17_and_cw18_roll_an_event_back_without_double_counting) {
  for (const Failpoint fp : {Failpoint::AfterInboundDurableBeforeApply,
                             Failpoint::AfterLedgerStageBeforeCommit}) {
    tests::TmpDir dir("cw-event");
    const std::string journal = dir.file("risk.journal");
    {
      LabOptions options;
      options.journal_path = journal;
      Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
      CHECK_MSG(created.ok(), created.detail());
      std::unique_ptr<LabEnv> env = std::move(created.value());

      CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice,
                           "cmd-1")
                .allowed());
      CHECK(env->dispatch("cmd-1").allowed());
      CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());

      env->failpoints().arm(fp, FailAction::CommitFail);
      const RiskResult fill = env->lp_fill("ACC-A", "chain-1", "exec-1", 20000);
      CHECK_MSG(fill.decision == RiskDecision::HoldRetryable,
                std::string(failpoint_window_id(fp)) + ": " + fill.detail);
      CHECK_EQ(env->position("ACC-A", "EURUSD"), 0);
      env->failpoints().disarm_all();

      // Re-delivering the same event after the failure must apply it exactly
      // once (CW-17: replay the inbox).
      CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 20000).allowed());
      CHECK_EQ(env->position("ACC-A", "EURUSD"), 20000);
      CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty - 20000);
    }

    Recovered recovered = reopen(journal);
    CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 20000);
    CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy),
             kQty - 20000);
    check_post_crash_invariants(*recovered.env, failpoint_window_id(fp).data());
  }
}

TEST(crash_windows, cw19_replaying_after_a_lost_cursor_ack_is_a_no_op) {
  tests::TmpDir dir("cw19");
  const std::string journal = dir.file("risk.journal");
  std::string hash_after_fill;
  {
    LabOptions options;
    options.journal_path = journal;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    std::unique_ptr<LabEnv> env = std::move(created.value());

    CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice,
                         "cmd-1")
              .allowed());
    CHECK(env->dispatch("cmd-1").allowed());
    CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());

    // The commit lands but the consumer cursor is never advanced.
    env->failpoints().arm(Failpoint::AfterApplyCommitBeforeCursorAck,
                          FailAction::CommitFail);
    const RiskResult fill = env->lp_fill("ACC-A", "chain-1", "exec-1", 20000);
    CHECK(fill.decision == RiskDecision::HoldRetryable);
    // The effect is durable even though the caller was told "hold".
    CHECK_EQ(env->position("ACC-A", "EURUSD"), 20000);
    hash_after_fill = env->store().state_hash();
    env->failpoints().disarm_all();

    // The replay that follows an un-acked cursor must change nothing.
    VerifiedOrderEvent replay;
    replay.tenant_id = "T1";
    replay.account_id = "ACC-A";
    replay.order_chain_id = "chain-1";
    replay.source_namespace = "lp";
    replay.event_id = "exec-1";
    replay.kind = OrderEventKind::AppliedFill;
    replay.symbol = "EURUSD";
    replay.side = Side::Buy;
    replay.fill_units = 20000;
    replay.verified_cum_units = 20000;
    const RiskResult again = env->lp_event(replay);
    CHECK(again.allowed());
    CHECK_EQ(static_cast<int>(again.reason), static_cast<int>(Reason::DuplicateEvent));
    CHECK_EQ(env->store().state_hash(), hash_after_fill);
  }

  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 20000);
  check_post_crash_invariants(*recovered.env, "CW-19");
}

TEST(crash_windows, cw20_a_release_commits_exactly_once) {
  tests::TmpDir dir("cw20");
  const std::string journal = dir.file("risk.journal");
  {
    LabOptions options;
    options.journal_path = journal;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    std::unique_ptr<LabEnv> env = std::move(created.value());

    CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice,
                         "cmd-1")
              .allowed());
    CHECK(env->dispatch("cmd-1").allowed());
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

    ReconcileEvidence evidence;
    evidence.tenant_id = "T1";
    evidence.account_id = "ACC-A";
    evidence.order_chain_id = "chain-1";
    evidence.evidence_id = "ev-1";
    evidence.resolved_state = ReservationState::Final;
    evidence.verified_cum_units = 0;
    evidence.verified_accepted_total_units = kQty;
    evidence.note = "LP confirms never accepted";

    env->failpoints().arm(Failpoint::AfterReleaseCommitBeforeAck,
                          FailAction::CommitFail);
    const RiskResult first = env->coordinator().reconcile_with_evidence(
        evidence, env->operator_principal(), env->risk_admin());
    CHECK(first.decision == RiskDecision::HoldRetryable);
    // The release is durable; only the acknowledgement was lost.
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
    env->failpoints().disarm_all();

    // Retrying the same evidence must be a dedup no-op, not a second release.
    const Revision revision = env->store().domain_state().revision;
    const RiskResult retry = env->coordinator().reconcile_with_evidence(
        evidence, env->operator_principal(), env->risk_admin());
    CHECK(retry.allowed());
    CHECK_EQ(static_cast<int>(retry.reason), static_cast<int>(Reason::DuplicateEvent));
    CHECK_EQ(env->store().domain_state().revision, revision);
  }

  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 0);
  check_post_crash_invariants(*recovered.env, "CW-20");
}

// ---------------------------------------------------------------------------
// Restart equivalence
// ---------------------------------------------------------------------------

TEST(crash_restart, a_restart_reproduces_the_live_state_exactly) {
  // P-13: the state after a restart equals the replay of the committed journal.
  tests::TmpDir dir("restart-equiv");
  const std::string journal = dir.file("risk.journal");

  LabOptions options;
  options.journal_path = journal;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  // A workload that touches every kind of mutation.
  CHECK(env->new_order("ACC-A", "c1", "EURUSD", Side::Buy, 100000, kPrice, "n1")
            .allowed());
  CHECK(env->dispatch("n1").allowed());
  CHECK(env->lp_accept("ACC-A", "c1", "a1").allowed());
  CHECK(env->lp_fill("ACC-A", "c1", "e1", 40000).allowed());
  CHECK(env->replace_order("ACC-A", "c1", 150000, kPrice, "r1").allowed());
  CHECK(env->lp_fill("ACC-A", "c1", "e2", 20000).allowed());
  CHECK(env->new_order("ACC-B", "c2", "GBPUSD", Side::Sell, 30000, kPrice, "n2")
            .allowed());
  CHECK(env->cancel_order("ACC-B", "c2", "x1").allowed());

  const std::string live_hash = env->store().state_hash();
  const std::size_t live_reservations = env->store().reservations().size();
  const std::size_t live_outbox = env->store().outbox().size();
  const std::size_t live_audit = env->store().audit().size();

  // Restart in place and compare before recovery changes the mode.
  {
    LabOptions reopen_options;
    reopen_options.journal_path = journal;
    reopen_options.open_admission = false;
    Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen_options);
    CHECK_MSG(replayed.ok(), replayed.detail());
    const Status equivalent =
        assert_replay_equivalence(env->store(), replayed.value()->store());
    CHECK_MSG(equivalent.ok(), equivalent.detail());
    CHECK_EQ(replayed.value()->store().state_hash(), live_hash);
    CHECK_EQ(replayed.value()->store().reservations().size(), live_reservations);
    CHECK_EQ(replayed.value()->store().outbox().size(), live_outbox);
    CHECK_EQ(replayed.value()->store().audit().size(), live_audit);
  }

  // And the quantities themselves survive.
  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 60000);
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);
  CHECK_EQ(recovered.env->effective_remaining("ACC-B", "GBPUSD", Side::Sell), 30000);
  check_post_crash_invariants(*recovered.env, "restart equivalence");
}

TEST(crash_restart, recovery_keeps_admission_closed_while_anything_is_unresolved) {
  // RI-32 / §16.3: unresolved outcomes mean RECON_ONLY with the holds intact.
  tests::TmpDir dir("restart-recon");
  const std::string journal = dir.file("risk.journal");
  {
    LabOptions options;
    options.journal_path = journal;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    std::unique_ptr<LabEnv> env = std::move(created.value());
    CHECK(apply_fixture(*env, Fixture::F6UnknownNew).ok());
  }

  Recovered recovered = reopen(journal);
  CHECK(recovered.report.unknown_outcomes >= 1);
  CHECK(!recovered.report.clean());
  CHECK(recovered.report.resulting_mode == DomainMode::ReconOnly);
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);

  // In RECON_ONLY nothing risk-increasing is admitted, but evidence is.
  const RiskResult blocked = recovered.env->new_order("ACC-A", "chain-new", "EURUSD",
                                                      Side::Buy, 1000, kPrice, "new-x");
  CHECK(blocked.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(blocked.reason),
           static_cast<int>(Reason::KillSwitchReconOnly));

  ReconcileEvidence evidence;
  evidence.tenant_id = "T1";
  evidence.account_id = "ACC-A";
  evidence.order_chain_id = "f6-chain";
  evidence.evidence_id = "ev-1";
  evidence.resolved_state = ReservationState::Final;
  evidence.verified_cum_units = 0;
  evidence.verified_accepted_total_units = 50000;
  const RiskResult resolved = recovered.env->coordinator().reconcile_with_evidence(
      evidence, recovered.env->operator_principal(), recovered.env->risk_admin());
  CHECK_MSG(resolved.allowed(), resolved.detail);
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);

  // A second restart now finds nothing unresolved, so recovery opens up.
  Recovered again = reopen(journal);
  CHECK_EQ(again.report.unknown_outcomes, static_cast<std::size_t>(0));
  CHECK(again.report.admission_open);
  check_post_crash_invariants(*again.env, "recovery after reconciliation");
}

TEST(crash_restart, recovery_refuses_to_open_on_a_quarantined_account) {
  tests::TmpDir dir("restart-quarantine");
  const std::string journal = dir.file("risk.journal");
  {
    LabOptions options;
    options.journal_path = journal;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    std::unique_ptr<LabEnv> env = std::move(created.value());

    CHECK(env->new_order("ACC-A", "c1", "EURUSD", Side::Buy, kQty, kPrice, "n1")
              .allowed());
    CHECK(env->dispatch("n1").allowed());
    CHECK(env->lp_accept("ACC-A", "c1", "a1").allowed());
    CHECK(env->lp_fill("ACC-A", "c1", "e1", 10000).allowed());
    // Contradictory redelivery of the same execution id.
    CHECK(env->lp_fill("ACC-A", "c1", "e1", 20000).decision ==
          RiskDecision::Quarantined);
  }

  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.report.quarantined_chains, static_cast<std::size_t>(1));
  CHECK_EQ(recovered.report.quarantined_accounts, static_cast<std::size_t>(1));
  CHECK(!recovered.report.clean());
  CHECK(recovered.report.resulting_mode == DomainMode::ReconOnly);
  // The quarantined chain still holds its budget across the restart.
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy),
           kQty - 10000);
  CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 10000);
  check_post_crash_invariants(*recovered.env, "quarantine across restart");
}

TEST(crash_restart, the_scenario_runner_restart_step_works_end_to_end) {
  tests::TmpDir dir("restart-dsl");
  const std::string text =
      "scenario_id RESTART-SMOKE\n"
      "seed 1\n"
      "fixture F0_EMPTY\n"
      "step reserve_new account=ACC-A chain=c1 symbol=EURUSD side=BUY qty=50000 "
      "price=110000 command_id=n1 expect=ALLOW_COMMITTED\n"
      "step assert effective_buy_remaining=50000 account=ACC-A symbol=EURUSD\n"
      "step restart\n"
      "step assert effective_buy_remaining=50000 account=ACC-A symbol=EURUSD "
      "all_risk_invariants=true\n";

  Result<ScenarioRun> run = run_scenario_text(text, dir.file("risk.journal"));
  CHECK_MSG(run.ok(), run.detail());
  CHECK_MSG(run.value().ok, run.value().failure);
}

// ---------------------------------------------------------------------------
// Out-of-process windows
// ---------------------------------------------------------------------------

TEST(crash_sigkill, the_child_harness_is_runnable) {
  // If this fails the out-of-process cases below are skipped, so it is
  // reported separately rather than silently passing.
  CHECK_MSG(crash_child_available(),
            std::string("cannot run ") + crash_child_path() +
                "; set RISK_CRASH_CHILD");
}

TEST(crash_sigkill, a_hard_exit_before_the_journal_write_leaves_nothing) {
  if (!crash_child_available()) return;

  // CW-06/CW-10/CW-11: the record never reached the device.
  const char* windows[] = {"CW-06", "CW-10", "CW-11"};
  for (const char* window : windows) {
    tests::TmpDir dir("sigkill-pre");
    const std::string journal = dir.file("risk.journal");
    const int rc = run_child(journal, "reserve_new", std::string(window) + ":abort");
    CHECK_MSG(rc != 0, std::string(window) + ": the child exited cleanly");

    Recovered recovered = reopen(journal);
    CHECK_MSG(recovered.env->store().reservations().empty(),
              std::string(window) + ": a hold survived an aborted transaction");
    CHECK(recovered.env->store().outbox().empty());
    CHECK(!recovered.env->store().find_command("T1", "ACC-A", "crash-new").has_value());
    check_post_crash_invariants(*recovered.env, window);
  }
}

TEST(crash_sigkill, a_hard_exit_after_the_journal_write_keeps_exactly_one_hold) {
  if (!crash_child_available()) return;

  // CW-12: the record reached the device and the process died before it could
  // tell anyone. Exactly one hold and one intent must be recoverable.
  tests::TmpDir dir("sigkill-post");
  const std::string journal = dir.file("risk.journal");
  const int rc = run_child(journal, "reserve_new", "CW-12:abort");
  CHECK(rc != 0);

  Recovered recovered = reopen(journal);
  CHECK_EQ(recovered.env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(recovered.env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);
  std::optional<CommandRecord> command =
      recovered.env->store().find_command("T1", "ACC-A", "crash-new");
  CHECK(command.has_value());
  CHECK_EQ(command->decision, std::string("ALLOW_COMMITTED"));
  check_post_crash_invariants(*recovered.env, "CW-12 sigkill");
}

TEST(crash_sigkill, a_hard_exit_around_dispatch_keeps_the_hold) {
  if (!crash_child_available()) return;

  // CW-13..CW-16: dispatch is uncertain, so the hold stays and the outcome is
  // reported as unresolved rather than released (RI-26).
  const char* windows[] = {"CW-13", "CW-14", "CW-16"};
  for (const char* window : windows) {
    tests::TmpDir dir("sigkill-dispatch");
    const std::string journal = dir.file("risk.journal");
    const int rc =
        run_child(journal, "reserve_and_dispatch", std::string(window) + ":abort");
    CHECK_MSG(rc != 0, std::string(window) + ": the child exited cleanly");

    Recovered recovered = reopen(journal);
    CHECK_MSG(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy) == 50000,
              std::string(window) + ": the hold changed");
    CHECK_EQ(recovered.env->store().outbox().size(), static_cast<std::size_t>(1));
    check_post_crash_invariants(*recovered.env, window);
  }
}

TEST(crash_sigkill, a_hard_exit_around_a_fill_never_double_counts) {
  if (!crash_child_available()) return;

  // CW-18 (staged, not committed) and CW-19 (committed, cursor not acked).
  const struct {
    const char* window;
    std::int64_t expected_position;
  } cases[] = {{"CW-18", 0}, {"CW-19", 20000}};

  for (const auto& c : cases) {
    tests::TmpDir dir("sigkill-fill");
    const std::string journal = dir.file("risk.journal");
    const int rc =
        run_child(journal, "reserve_and_fill", std::string(c.window) + ":abort");
    CHECK_MSG(rc != 0, std::string(c.window) + ": the child exited cleanly");

    Recovered recovered = reopen(journal);
    CHECK_MSG(recovered.env->position("ACC-A", "EURUSD") == c.expected_position,
              std::string(c.window) + ": position is " +
                  std::to_string(recovered.env->position("ACC-A", "EURUSD")) +
                  ", expected " + std::to_string(c.expected_position));
    // P + B is invariant whichever side of the commit the crash landed on.
    CHECK_EQ(recovered.env->position("ACC-A", "EURUSD") +
                 recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy),
             50000);
    check_post_crash_invariants(*recovered.env, c.window);
  }
}

TEST(crash_sigkill, a_hard_exit_during_a_replace_keeps_the_larger_branch) {
  if (!crash_child_available()) return;

  // The §24.5 scenario's crash step: killed while the replace is in flight, the
  // worst-case branch must survive the restart.
  tests::TmpDir dir("sigkill-replace");
  const std::string journal = dir.file("risk.journal");
  const int rc = run_child(journal, "replace_up", "CW-12:abort");
  CHECK(rc != 0);

  Recovered recovered = reopen(journal);
  std::optional<Reservation> r = recovered.env->reservation_for("ACC-A", "crash-chain");
  CHECK(r.has_value());
  CHECK_EQ(r->accepted_total_units, 100000);
  CHECK_EQ(r->cum_units, 40000);
  CHECK(r->proposed_total_units.has_value());
  CHECK_EQ(*r->proposed_total_units, 150000);
  // max(100k - 40k, 150k - 40k) = 110k
  CHECK_EQ(r->risk_effective_remaining(), 110000);
  CHECK_EQ(recovered.env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 110000);
  CHECK_EQ(recovered.env->position("ACC-A", "EURUSD"), 40000);
  check_post_crash_invariants(*recovered.env, "replace crash");
}

TEST(crash_sigkill, repeating_the_same_crash_is_deterministic) {
  if (!crash_child_available()) return;

  // §20.3: run the same seed repeatedly. The recovered state hash must be
  // identical every time, or the crash path is not deterministic.
  std::string first_hash;
  for (int attempt = 0; attempt < 3; ++attempt) {
    tests::TmpDir dir("sigkill-determinism");
    const std::string journal = dir.file("risk.journal");
    const int rc = run_child(journal, "reserve_and_fill", "CW-19:abort");
    CHECK(rc != 0);

    LabOptions options;
    options.journal_path = journal;
    options.open_admission = false;
    Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(options);
    CHECK_MSG(replayed.ok(), replayed.detail());
    const std::string hash = replayed.value()->store().state_hash();
    if (attempt == 0) {
      first_hash = hash;
    } else {
      CHECK_MSG(hash == first_hash,
                "attempt " + std::to_string(attempt) + " differs");
    }
  }
}
