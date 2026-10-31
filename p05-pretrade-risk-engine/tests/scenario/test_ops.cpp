// G16 / G18 / G19 — dynamic policy, kill switch, approvals, security, audit,
// and the temporal edge cases.
//
// The theme is that every operational lever is audited, needs the right role,
// and cannot be used to make exposure disappear. In particular there is no
// `force_release`: the only way out of UNKNOWN or QUARANTINE is reconciliation
// with evidence and two distinct principals (§18.1).
#include <memory>

#include "risk/config.hpp"
#include "risk/scenario.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;

std::unique_ptr<LabEnv> make_env() {
  LabOptions options;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

std::unique_ptr<LabEnv> env_with_live_order() {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  return env;
}

int count_audit(const LabEnv& env, const std::string& type) {
  int count = 0;
  for (const AuditEvent& e : env.store().audit()) {
    if (e.event_type == type) ++count;
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// G16 — policy and the kill switch
// ---------------------------------------------------------------------------

TEST(scenario_ops, a_policy_change_needs_two_distinct_risk_admins) {
  std::unique_ptr<LabEnv> env = make_env();
  RiskPolicy next = env->store().policy();
  next.version += 1;

  {  // a trader may not install a policy at all
    const RiskResult r = env->coordinator().install_policy(
        next, env->trader(), env->risk_admin2(), "nope");
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthorized));
  }
  {  // one admin approving their own change is not dual approval
    const RiskResult r = env->coordinator().install_policy(
        next, env->risk_admin(), env->risk_admin(), "self-approved");
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ApprovalRequired));
  }
  {  // an operator is not a risk admin
    const RiskResult r = env->coordinator().install_policy(
        next, env->risk_admin(), env->operator_principal(), "wrong approver");
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ApprovalRequired));
  }

  CHECK_EQ(env->store().policy().version, static_cast<PolicyVersion>(1));

  const RiskResult good = env->coordinator().install_policy(
      next, env->risk_admin(), env->risk_admin2(), "approved");
  CHECK_MSG(good.allowed(), good.detail);
  CHECK_EQ(env->store().policy().version, static_cast<PolicyVersion>(2));
  CHECK_EQ(count_audit(*env, "PolicyInstalled"), 1);

  // The audit record names both principals.
  bool found = false;
  for (const AuditEvent& e : env->store().audit()) {
    if (e.event_type != "PolicyInstalled") continue;
    found = true;
    CHECK(e.actor.find(env->risk_admin().principal_id) != std::string::npos);
    CHECK(e.actor.find(env->risk_admin2().principal_id) != std::string::npos);
    CHECK_EQ(e.payload_hash, env->store().policy().hash());
  }
  CHECK(found);
}

TEST(scenario_ops, a_policy_version_may_not_regress_or_repeat) {
  std::unique_ptr<LabEnv> env = make_env();
  RiskPolicy same = env->store().policy();

  const RiskResult repeated = env->coordinator().install_policy(
      same, env->risk_admin(), env->risk_admin2(), "same version");
  CHECK_EQ(static_cast<int>(repeated.reason),
           static_cast<int>(Reason::PolicyVersionRegression));

  RiskPolicy older = same;
  older.version = 1;
  CHECK(env->coordinator()
            .install_policy(older, env->risk_admin(), env->risk_admin2(), "older")
            .reason == Reason::PolicyVersionRegression);
}

TEST(scenario_ops, an_invalid_policy_is_never_installed) {
  std::unique_ptr<LabEnv> env = make_env();
  const std::string hash_before = env->store().policy().hash();

  RiskPolicy broken = env->store().policy();
  broken.version += 1;
  broken.safety.auto_release_on_timeout = true;  // RI-26 forbids this
  const RiskResult r = env->coordinator().install_policy(
      broken, env->risk_admin(), env->risk_admin2(), "unsafe");
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::PolicyInvalid));
  CHECK_EQ(env->store().policy().hash(), hash_before);
}

TEST(scenario_ops, re_denominating_an_account_is_refused) {
  std::unique_ptr<LabEnv> env = make_env();
  RiskPolicy next = env->store().policy();
  next.version += 1;
  next.account_currency = Currency{"EUR", 2};
  const RiskResult r = env->coordinator().install_policy(
      next, env->risk_admin(), env->risk_admin2(), "re-denominate");
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::ScaleConversionUnsupported));
  CHECK_EQ(env->store().policy().account_currency.code, std::string("USD"));
}

TEST(scenario_ops, tightening_caps_under_live_exposure_breaches_without_deleting) {
  // PB-25 / §7.2 step 4: the new limits are below the live envelope, so the
  // domain goes NO_NEW. Nothing is deleted and no hold shrinks.
  std::unique_ptr<LabEnv> env = env_with_live_order();
  const std::int64_t held_before =
      env->effective_remaining("ACC-A", "EURUSD", Side::Buy);
  const std::size_t reservations_before = env->store().reservations().size();

  RiskPolicy tightened = env->store().policy();
  tightened.version += 1;
  tightened.limits.max_account_symbol_net_minor = 1;
  tightened.limits.max_account_symbol_gross_minor = 1;
  tightened.limits.max_account_total_gross_minor = 1;
  tightened.limits.max_firm_gross_minor = 1;

  const RiskResult r = env->coordinator().install_policy(
      tightened, env->risk_admin(), env->risk_admin2(), "tighten");
  CHECK_MSG(r.allowed(), r.detail);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::LimitBreached));

  CHECK(env->store().domain_state().mode == DomainMode::NoNew);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), held_before);
  CHECK_EQ(env->store().reservations().size(), reservations_before);

  // Risk-increasing commands are blocked...
  const RiskResult blocked =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2");
  CHECK_EQ(static_cast<int>(blocked.reason), static_cast<int>(Reason::KillSwitchNoNew));
  // ...but fills and cancels still flow (§17).
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 10000).allowed());
  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, the_f11_fixture_reproduces_the_breach_state) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(apply_fixture(*env, Fixture::F11Breached).ok());
  CHECK(env->store().domain_state().mode == DomainMode::NoNew);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 100000);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, a_mark_to_market_breach_forces_no_new) {
  // RI-27: a committed event revalued against a moved market can push the
  // account past a cap. The mode is tightened in the same transaction and the
  // breach is audited -- the exposure itself is never pretended away.
  //
  // Note that a *fill* alone can never breach a cap: moving f from B into P
  // leaves P + B unchanged and |P| + B + S no larger (§10.3), so the market has
  // to be the thing that moves.
  LabOptions options;
  // 100,000 units at the lab price is 11,110,000 minor units, which fits.
  options.policy.limits.max_account_symbol_net_minor = 12'000'000;
  options.policy.limits.max_account_symbol_gross_minor = 12'000'000;
  options.policy.limits.max_account_total_gross_minor = 12'000'000;
  options.policy.limits.max_firm_gross_minor = 12'000'000;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->store().domain_state().mode == DomainMode::Normal);

  // PB-06: the market spikes. The same 100,000 units are now worth
  // 100,000 * ceil(130000 * 1.01) / 100000 * 100 = 13,130,000 minor units.
  CHECK(env->publish_quote("EURUSD", 129990, 130000).ok());

  const RiskResult fill = env->lp_fill("ACC-A", "chain-1", "exec-1", 10000);
  CHECK_MSG(fill.allowed(), fill.detail);

  // The fill applied, and the breach closed the gate behind it.
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 10000);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 90000);
  CHECK(env->store().domain_state().mode == DomainMode::NoNew);
  CHECK_EQ(count_audit(*env, "LimitBreach"), 1);

  // Risk-increasing commands are now blocked, but evidence still flows.
  const RiskResult blocked =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2");
  CHECK_EQ(static_cast<int>(blocked.reason), static_cast<int>(Reason::KillSwitchNoNew));
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 10000).allowed());
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, the_gross_proxy_can_be_the_binding_cap) {
  // RI-09: two-sided exposure costs gross budget even though it barely moves
  // the net envelope, so a cap set just above one side refuses the other.
  LabOptions options;
  options.policy.limits.max_account_symbol_net_minor = 12'000'000;
  options.policy.limits.max_account_symbol_gross_minor = 12'000'000;
  options.policy.limits.max_account_total_gross_minor = 12'000'000;
  options.policy.limits.max_firm_gross_minor = 12'000'000;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());

  // A 10,000 SELL leaves the net envelope at 100,000 units but takes gross to
  // 110,000, which is 12,221,000 minor units and over the cap.
  const RiskResult r =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Sell, 10000, kPrice, "new-2");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::LimitAccountSymbolGross));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Sell), 0);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, mode_changes_require_a_role_and_are_audited) {
  std::unique_ptr<LabEnv> env = make_env();

  Principal plain_trader = env->trader();
  const RiskResult refused =
      env->coordinator().set_mode(DomainMode::NoNew, plain_trader, "trader");
  CHECK_EQ(static_cast<int>(refused.reason), static_cast<int>(Reason::Unauthorized));
  CHECK(env->store().domain_state().mode == DomainMode::Normal);

  CHECK(env->coordinator()
            .set_mode(DomainMode::CancelOnly, env->risk_admin(), "admin")
            .allowed());
  CHECK(env->store().domain_state().mode == DomainMode::CancelOnly);
  CHECK(env->coordinator()
            .set_mode(DomainMode::Normal, env->operator_principal(), "operator")
            .allowed());
  CHECK_EQ(count_audit(*env, "ModeChanged"), 2);
}

// ---------------------------------------------------------------------------
// G18 — security, isolation, audit
// ---------------------------------------------------------------------------

TEST(scenario_ops, there_is_no_force_release) {
  // §18.1: the only way out of UNKNOWN is reconciliation with evidence, and
  // that needs two distinct principals with the right roles.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(apply_fixture(*env, Fixture::F6UnknownNew).ok());
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);

  ReconcileEvidence evidence;
  evidence.tenant_id = "T1";
  evidence.account_id = "ACC-A";
  evidence.order_chain_id = "f6-chain";
  evidence.evidence_id = "ev-1";
  evidence.resolved_state = ReservationState::Final;
  evidence.verified_cum_units = 0;
  evidence.verified_accepted_total_units = 50000;

  {  // a trader cannot reconcile
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        evidence, env->trader(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthorized));
  }
  {  // the approver must be a risk admin
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        evidence, env->operator_principal(), env->trader());
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ApprovalRequired));
  }
  {  // and must be someone else
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        evidence, env->risk_admin(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ApprovalRequired));
  }
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);

  const RiskResult good = env->coordinator().reconcile_with_evidence(
      evidence, env->operator_principal(), env->risk_admin());
  CHECK_MSG(good.allowed(), good.detail);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  CHECK_EQ(count_audit(*env, "ReconcileResolved"), 1);
}

TEST(scenario_ops, reconciliation_refuses_incoherent_evidence) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(apply_fixture(*env, Fixture::F6UnknownNew).ok());

  ReconcileEvidence base;
  base.tenant_id = "T1";
  base.account_id = "ACC-A";
  base.order_chain_id = "f6-chain";
  base.evidence_id = "ev-1";
  base.resolved_state = ReservationState::Working;
  base.verified_cum_units = 10000;
  base.verified_accepted_total_units = 50000;

  {  // a cumulative above the accepted total is not a coherent account
    ReconcileEvidence bad = base;
    bad.verified_cum_units = 60000;
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        bad, env->operator_principal(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::EvidenceInsufficient));
  }
  {  // a resolution to a non-terminal, non-working state is meaningless
    ReconcileEvidence bad = base;
    bad.resolved_state = ReservationState::Committed;
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        bad, env->operator_principal(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::EvidenceInsufficient));
  }
  {  // negative quantities
    ReconcileEvidence bad = base;
    bad.verified_cum_units = -1;
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        bad, env->operator_principal(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::EvidenceInsufficient));
  }
  {  // another tenant's chain
    ReconcileEvidence bad = base;
    bad.tenant_id = "T2";
    const RiskResult r = env->coordinator().reconcile_with_evidence(
        bad, env->operator_principal(), env->risk_admin());
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::TenantMismatch));
  }

  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);
}

TEST(scenario_ops, reconciliation_moves_the_position_to_the_verified_cumulative) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 50000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());

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

  // The LP is later found to have filled 30,000 of it.
  ReconcileEvidence evidence;
  evidence.tenant_id = "T1";
  evidence.account_id = "ACC-A";
  evidence.order_chain_id = "chain-1";
  evidence.evidence_id = "ev-1";
  evidence.resolved_state = ReservationState::Final;
  evidence.verified_cum_units = 30000;
  evidence.verified_accepted_total_units = 50000;
  evidence.note = "LP history shows 30k executed, order then cancelled";

  const RiskResult r = env->coordinator().reconcile_with_evidence(
      evidence, env->operator_principal(), env->risk_admin());
  CHECK_MSG(r.allowed(), r.detail);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 30000);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, a_quarantine_cannot_be_cleared_before_the_chain_is_resolved) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 50000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 10000).allowed());
  // A contradictory redelivery quarantines the account.
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 20000).decision ==
        RiskDecision::Quarantined);
  CHECK(env->store().account_quarantined("ACC-A"));

  const RiskResult premature = env->coordinator().clear_account_quarantine(
      "ACC-A", env->operator_principal(), env->risk_admin(), "please");
  CHECK_EQ(static_cast<int>(premature.reason),
           static_cast<int>(Reason::EvidenceInsufficient));
  CHECK(env->store().account_quarantined("ACC-A"));

  // Reconciling the chain clears the account in the same transaction.
  ReconcileEvidence evidence;
  evidence.tenant_id = "T1";
  evidence.account_id = "ACC-A";
  evidence.order_chain_id = "chain-1";
  evidence.evidence_id = "ev-1";
  evidence.resolved_state = ReservationState::Final;
  evidence.verified_cum_units = 10000;
  evidence.verified_accepted_total_units = 50000;
  const RiskResult resolved = env->coordinator().reconcile_with_evidence(
      evidence, env->operator_principal(), env->risk_admin());
  CHECK_MSG(resolved.allowed(), resolved.detail);
  CHECK(!env->store().account_quarantined("ACC-A"));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 10000);
  CHECK(env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2")
            .allowed());
}

TEST(scenario_ops, tenants_and_accounts_are_isolated) {
  // RI-14 / G18: a reservation belongs to one tenant and account, and editing
  // the payload cannot move it.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice,
                       "cmd-1")
            .allowed());

  // The same chain id under a different account is a different chain.
  CHECK(env->new_order("ACC-B", "chain-1", "EURUSD", Side::Buy, 10000, kPrice,
                       "cmd-2")
            .allowed());
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 10000);
  CHECK_EQ(env->effective_remaining("ACC-B", "EURUSD", Side::Buy), 10000);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(2));

  // An event naming the wrong account does not find the other account's chain.
  VerifiedOrderEvent misdirected;
  misdirected.tenant_id = "T1";
  misdirected.account_id = "ACC-C";
  misdirected.order_chain_id = "chain-1";
  misdirected.source_namespace = "lp";
  misdirected.event_id = "exec-1";
  misdirected.kind = OrderEventKind::AppliedFill;
  misdirected.symbol = "EURUSD";
  misdirected.side = Side::Buy;
  misdirected.fill_units = 1000;
  const RiskResult r = env->lp_event(misdirected);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::UnknownChain));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 0);
  CHECK_EQ(env->position("ACC-B", "EURUSD"), 0);
  CHECK(env->store().account_quarantined("ACC-C"));
  CHECK(!env->store().account_quarantined("ACC-A"));
}

TEST(scenario_ops, every_mutation_is_audited_and_the_chain_is_unbroken) {
  // RI-12 / §18.2: the audit trail is self-verifying, and each state hash
  // chains onto the previous one.
  std::unique_ptr<LabEnv> env = env_with_live_order();
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 20000).allowed());
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());
  CHECK(env->cancel_order("ACC-B", "nope", "cancel-x").decision ==
        RiskDecision::DenyDefinitive);

  const AuditChainReport chain = verify_audit_chain(env->store().audit());
  CHECK_MSG(chain.ok, chain.detail);
  CHECK(chain.events > 5);
  // Every event mutated something: a no-op commit would show up as an event
  // whose two state hashes are equal.
  CHECK_EQ(chain.mutations, chain.events);

  const Status audited = verify_every_reservation_audited(env->store().reservations(),
                                                          env->store().audit());
  CHECK_MSG(audited.ok(), audited.detail());

  // Each audit event names the policy version and the snapshot refs it used.
  for (const AuditEvent& e : env->store().audit()) {
    CHECK_MSG(e.policy_version > 0, e.event_type + " has no policy version");
    CHECK_MSG(!e.snapshots.digest().empty(), e.event_type + " has no snapshot refs");
    CHECK_MSG(!e.component_version.empty(),
              e.event_type + " has no component version");
  }
}

TEST(scenario_ops, the_metrics_surface_the_documented_counters) {
  std::unique_ptr<LabEnv> env = env_with_live_order();
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 10000).allowed());
  VerifiedOrderEvent replay;
  replay.tenant_id = "T1";
  replay.account_id = "ACC-A";
  replay.order_chain_id = "chain-1";
  replay.source_namespace = "lp";
  replay.event_id = "exec-1";
  replay.kind = OrderEventKind::AppliedFill;
  replay.symbol = "EURUSD";
  replay.side = Side::Buy;
  replay.fill_units = 10000;
  replay.verified_cum_units = 10000;
  CHECK(env->lp_event(replay).allowed());

  const Metrics& metrics = env->coordinator().metrics();
  CHECK(metrics.labeled(metric::kAdmissionTotal, "op=new") > 0);
  CHECK(metrics.labeled(metric::kEventDedup, "result=duplicate_noop") > 0);
  CHECK(metrics.labeled(metric::kCommitOutcome, "outcome=COMMITTED") > 0);
  CHECK(metrics.admission_commit_seconds().count() > 0);

  // Rendering must not crash and must mention the histogram.
  const std::string rendered = metrics.render();
  CHECK(rendered.find("risk_admission_commit_micros_count") != std::string::npos);
  CHECK(rendered.find("risk_admission_total") != std::string::npos);
}

// ---------------------------------------------------------------------------
// G19 — temporal semantics and lifecycle edges
// ---------------------------------------------------------------------------

TEST(scenario_ops, a_day_rollover_releases_nothing) {
  // RI-26: a DAY order spanning a date change is not silently expired by the
  // risk engine. Only a verified terminal report ends it.
  std::unique_ptr<LabEnv> env = env_with_live_order();
  const std::int64_t held = env->effective_remaining("ACC-A", "EURUSD", Side::Buy);

  env->clock().advance_millis(26LL * 3600 * 1000);  // more than a day
  CHECK(env->publish_default_market().ok());

  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), held);
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Working);
  CHECK(env->coordinator().cross_check_projection().ok());
}

TEST(scenario_ops, a_wall_clock_rollback_does_not_revive_stale_data) {
  // PB-28: moving the wall clock backwards must not make an expired quote
  // usable again, because freshness lives on the monotonic axis.
  std::unique_ptr<LabEnv> env = make_env();
  const std::int64_t ttl = env->policy().instruments.at("EURUSD").quote_ttl_ms;
  env->clock().advance_millis(ttl + 1);

  // Rewind the wall clock far into the past, leaving the monotonic axis alone.
  env->clock().set_utc(0);

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK(r.reason == Reason::QuoteStale || r.reason == Reason::FxStale ||
        r.reason == Reason::EquityStale);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
}

TEST(scenario_ops, an_equity_feed_account_stops_new_risk_when_the_feed_goes_stale) {
  // §9.3: a stale equity snapshot means STOP NEW, not "use the old number".
  LabOptions options;
  options.policy.accounts.at("ACC-A").use_equity_feed = true;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice,
                       "cmd-1")
            .allowed());

  // Age only the equity snapshot: republish the quote and the FX edges.
  env->clock().advance_millis(env->policy().equity_ttl_ms + 1);
  for (const auto& [symbol, spec] : env->policy().instruments) {
    const std::int64_t ask = spec.quote_currency == "JPY" ? 160000 : 110000;
    CHECK(env->publish_quote(symbol, ask - 10, ask).ok());
  }
  CHECK(env->publish_fx("JPY", "USD", Rational{66, 10000}, Rational{67, 10000}).ok());

  const RiskResult r =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 10000, kPrice, "cmd-2");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::EquityStale));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 10000);
}

TEST(scenario_ops, an_equity_feed_account_respects_the_available_buffer) {
  LabOptions options;
  options.policy.accounts.at("ACC-A").use_equity_feed = true;
  options.policy.limits.min_margin_buffer_minor = 100'000;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  // A 100,000-unit order needs 5% of 11,110,000 = 555,500 plus the 100,000
  // buffer, so 655,500 of available equity.
  CHECK(env->publish_equity("ACC-A", 655'499, 0).ok());
  const RiskResult tight =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice, "cmd-1");
  CHECK_EQ(static_cast<int>(tight.reason),
           static_cast<int>(Reason::LimitMarginBuffer));

  CHECK(env->publish_equity("ACC-A", 655'500, 0).ok());
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "cmd-2")
            .allowed());

  // Existing broker margin is subtracted from the available equity.
  CHECK(env->publish_equity("ACC-A", 10'000'000, 9'999'999).ok());
  const RiskResult encumbered =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "cmd-3");
  CHECK_EQ(static_cast<int>(encumbered.reason),
           static_cast<int>(Reason::LimitMarginBuffer));
}

TEST(scenario_ops, an_equity_snapshot_in_the_wrong_currency_is_refused) {
  // PB-27: a snapshot for the right account but the wrong currency must not be
  // compared against a USD margin requirement.
  LabOptions options;
  options.policy.accounts.at("ACC-A").use_equity_feed = true;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  EquitySnapshot wrong;
  wrong.account_id = "ACC-A";
  wrong.currency = "EUR";
  wrong.equity_minor = 100'000'000;
  wrong.ingest_mono = env->clock().mono_now();
  wrong.version = 99;
  wrong.ttl_ms = 1000;
  CHECK(env->feed().publish_equity(wrong).ok());

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::EquityCurrencyMismatch));
}

TEST(scenario_ops, the_cli_policy_round_trip_matches_the_engine) {
  // A small end-to-end check that the shipped lab config loads and hashes the
  // same as the built-in policy the tests use.
  Result<RiskPolicy> loaded = load_policy_file("configs/risk_policy.lab.yaml");
  CHECK_MSG(loaded.ok(), std::string(reason_name(loaded.reason())) + ": " +
                             loaded.detail());
  CHECK_EQ(loaded.value().hash(), lab_policy().hash());
}
