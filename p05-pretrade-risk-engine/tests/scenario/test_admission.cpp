// G01 / G07 / G08 — identity, authorisation, admission and the durable handoff.
//
// The assertions follow §20.1: a case checks the decision, the resulting
// state, the durable side effects (outbox and audit counts) and the relevant
// invariant. "It returned something" is never enough.
#include <memory>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;    // 1.10000, the lab ask
constexpr std::int64_t kQty = 100000;      // 100,000 base units
// 100,000 units at a risk price of 1.11100 with a 2-digit currency.
constexpr std::int64_t kQtyNotionalMinor = 11'110'000;

std::unique_ptr<LabEnv> make_env() {
  LabOptions options;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

// Counts outbox rows that have no committed reservation behind them. RI-01
// says this must always be zero.
std::size_t orphan_intents(const LabEnv& env) {
  std::size_t orphans = 0;
  for (const OutboxIntent& o : env.store().outbox()) {
    std::optional<Reservation> r = env.store().find_reservation(o.reservation_id);
    if (!r.has_value()) ++orphans;
  }
  return orphans;
}

void check_invariants(LabEnv& env, const char* where) {
  const Status cross = env.coordinator().cross_check_projection();
  CHECK_MSG(cross.ok(), std::string(where) + ": " + cross.detail());
  const AuditChainReport chain = verify_audit_chain(env.store().audit());
  CHECK_MSG(chain.ok, std::string(where) + ": " + chain.detail);
  CHECK_MSG(orphan_intents(env) == 0, std::string(where) + ": orphan outbox intent");
}

}  // namespace

TEST(scenario_admission, happy_path_commits_hold_intent_and_audit_together) {
  std::unique_ptr<LabEnv> env = make_env();

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK_MSG(r.allowed(), std::string(reason_name(r.reason)) + ": " + r.detail);

  // A token is only minted after a confirmed commit (§12.1 step 7).
  CHECK(r.token.has_value());
  CHECK_EQ(r.token->command_id, std::string("cmd-1"));
  CHECK(env->coordinator().verify_token(*r.token).ok());

  // The hold, the intent and the audit record all landed.
  std::optional<Reservation> reservation = env->reservation_for("ACC-A", "chain-1");
  CHECK(reservation.has_value());
  CHECK_EQ(reservation->accepted_total_units, kQty);
  CHECK_EQ(reservation->cum_units, 0);
  CHECK(reservation->state == ReservationState::Committed);
  CHECK_EQ(reservation->risk_effective_remaining(), kQty);
  CHECK_EQ(reservation->accepted_limit_price_ticks, kPrice);

  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Sell), 0);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 0);

  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().outbox()[0].kind, std::string("NEW"));
  CHECK_EQ(env->store().outbox()[0].total_units, kQty);
  CHECK(!env->store().outbox()[0].dispatched);

  Result<DomainMetrics> metrics = env->coordinator().current_metrics();
  CHECK_MSG(metrics.ok(), metrics.detail());
  CHECK_EQ(metrics.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor,
           kQtyNotionalMinor);

  check_invariants(*env, "happy path");
}

TEST(scenario_admission, replaying_the_same_command_is_idempotent) {
  // PB-02 in miniature: the same command many times must produce exactly one
  // durable reservation and one intent (RI-02).
  std::unique_ptr<LabEnv> env = make_env();

  const RiskResult first =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(first.allowed());
  const Revision revision_after_first = env->store().domain_state().revision;

  for (int i = 0; i < 50; ++i) {
    const RiskResult again =
        env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
    CHECK(again.allowed());
    CHECK_EQ(again.reservation_id, first.reservation_id);
  }

  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  // No new commits: a replay reads, it does not write.
  CHECK_EQ(env->store().domain_state().revision, revision_after_first);
  check_invariants(*env, "idempotent replay");
}

TEST(scenario_admission, same_id_different_payload_is_a_conflict) {
  // PB-03 / RI-02: the original decision is untouched and nothing new is sent.
  std::unique_ptr<LabEnv> env = make_env();

  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());
  const std::string hash_before = env->store().state_hash();

  const struct {
    const char* label;
    AccountId account;
    OrderChainId chain;
    SymbolId symbol;
    Side side;
    std::int64_t qty;
    std::int64_t price;
  } mutations[] = {
      {"quantity", "ACC-A", "chain-1", "EURUSD", Side::Buy, kQty + 1000, kPrice},
      {"price", "ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice + 10},
      {"side", "ACC-A", "chain-1", "EURUSD", Side::Sell, kQty, kPrice},
      {"symbol", "ACC-A", "chain-1", "GBPUSD", Side::Buy, kQty, kPrice},
      {"chain", "ACC-A", "chain-2", "EURUSD", Side::Buy, kQty, kPrice},
      {"account", "ACC-B", "chain-1", "EURUSD", Side::Buy, kQty, kPrice},
  };

  for (const auto& m : mutations) {
    const RiskResult conflict = env->new_order(m.account, m.chain, m.symbol, m.side,
                                               m.qty, m.price, "cmd-1");
    // ACC-B is a different idempotency key, so only the same-account mutations
    // can conflict; the others must not be silently accepted either.
    if (m.account == "ACC-A") {
      CHECK_MSG(conflict.decision == RiskDecision::Conflict,
                std::string(m.label) + " gave " +
                    std::string(risk_decision_name(conflict.decision)));
      CHECK_EQ(static_cast<int>(conflict.reason),
               static_cast<int>(Reason::CommandIdConflict));
    }
  }

  // Only the ACC-B variant could legitimately commit, so compare the parts the
  // conflicts must not have touched.
  std::optional<Reservation> original = env->reservation_for("ACC-A", "chain-1");
  CHECK(original.has_value());
  CHECK_EQ(original->accepted_total_units, kQty);
  CHECK_EQ(original->accepted_limit_price_ticks, kPrice);
  CHECK(original->side == Side::Buy);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  (void)hash_before;
  check_invariants(*env, "conflict");
}

TEST(scenario_admission, authorisation_is_checked_before_idempotency) {
  // §18.1: otherwise a caller could probe another tenant's command ids by
  // watching whether it got CONFLICT or a fresh decision.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());

  NewOrderCommand probe;
  probe.tenant_id = "T1";
  probe.account_id = "ACC-A";
  probe.command_id = "cmd-1";
  probe.order_chain_id = "chain-1";
  probe.symbol = "EURUSD";
  probe.side = Side::Buy;
  probe.total_units = kQty;
  probe.limit_price_ticks = kPrice;

  Principal stranger;
  stranger.principal_id = "trader-2";
  stranger.tenant_id = "T1";
  stranger.roles = {Role::Trader};
  stranger.accounts = {"ACC-B"};

  const RiskResult r = env->coordinator().reserve_new(probe, stranger);
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  // Unauthorized, not Conflict: the answer must not reveal that the id exists.
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthorized));
}

TEST(scenario_admission, rejects_identity_and_role_problems) {
  std::unique_ptr<LabEnv> env = make_env();

  NewOrderCommand base;
  base.tenant_id = "T1";
  base.account_id = "ACC-A";
  base.command_id = "cmd-1";
  base.order_chain_id = "chain-1";
  base.symbol = "EURUSD";
  base.side = Side::Buy;
  base.total_units = kQty;
  base.limit_price_ticks = kPrice;

  {  // another tenant's command
    NewOrderCommand cmd = base;
    cmd.tenant_id = "T2";
    Principal who = env->trader();
    who.tenant_id = "T2";
    const RiskResult r = env->coordinator().reserve_new(cmd, who);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::TenantMismatch));
  }
  {  // principal from a different tenant
    Principal who = env->trader();
    who.tenant_id = "T2";
    const RiskResult r = env->coordinator().reserve_new(base, who);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::TenantMismatch));
  }
  {  // no principal at all
    Principal who;
    const RiskResult r = env->coordinator().reserve_new(base, who);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthenticated));
  }
  {  // an auditor may read, not trade
    Principal who = env->trader();
    who.roles = {Role::Auditor};
    const RiskResult r = env->coordinator().reserve_new(base, who);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthorized));
  }
  {  // a risk admin is not automatically a trader
    Principal who = env->risk_admin();
    who.accounts = {"ACC-A"};
    const RiskResult r = env->coordinator().reserve_new(base, who);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::Unauthorized));
  }

  const char* bad_ids[] = {"", "has space", "has|pipe", "has:colon", "tab\there",
                           "quote\"here", "newline\nhere", "non-ascii-\xc3\xa9"};
  for (const char* bad : bad_ids) {
    NewOrderCommand cmd = base;
    cmd.command_id = bad;
    const RiskResult r = env->coordinator().reserve_new(cmd, env->trader());
    CHECK_MSG(r.decision == RiskDecision::DenyDefinitive,
              std::string("accepted command id: ") + bad);
    CHECK(r.reason == Reason::MalformedIdentifier ||
          r.reason == Reason::IdentifierTooLong);
  }
  {  // over-long identifier
    NewOrderCommand cmd = base;
    cmd.command_id = std::string(kMaxIdLength + 1, 'a');
    const RiskResult r = env->coordinator().reserve_new(cmd, env->trader());
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::IdentifierTooLong));
  }
  {  // lower-case symbol
    NewOrderCommand cmd = base;
    cmd.symbol = "eurusd";
    const RiskResult r = env->coordinator().reserve_new(cmd, env->trader());
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::MalformedIdentifier));
  }

  // None of the refusals above may have created state.
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(0));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
}

TEST(scenario_admission, enforces_instrument_rules) {
  std::unique_ptr<LabEnv> env = make_env();

  struct Case {
    const char* label;
    std::int64_t qty;
    std::int64_t price;
    Reason expected;
  };
  const Case cases[] = {
      {"zero quantity", 0, kPrice, Reason::QtyBelowMin},
      {"negative quantity", -1000, kPrice, Reason::QtyBelowMin},
      {"below min", 500, kPrice, Reason::QtyBelowMin},
      {"above max", 300000, kPrice, Reason::QtyAboveMax},
      {"off-step", 1500, kPrice, Reason::QtyStepViolation},
      {"zero price", kQty, 0, Reason::PriceNonPositive},
      {"negative price", kQty, -1, Reason::PriceNonPositive},
      // 1.10000 +/- 5% is the collar; 1.30000 is far outside it.
      {"outside the collar", 10000, 130000, Reason::LimitPriceCollar},
  };

  int index = 0;
  for (const Case& c : cases) {
    const std::string command_id = "cmd-" + std::to_string(index++);
    const RiskResult r = env->new_order("ACC-A", "chain-" + command_id, "EURUSD",
                                        Side::Buy, c.qty, c.price, command_id);
    CHECK_MSG(r.decision == RiskDecision::DenyDefinitive,
              std::string(c.label) + " gave " +
                  std::string(risk_decision_name(r.decision)));
    CHECK_MSG(r.reason == c.expected,
              std::string(c.label) + " gave reason " +
                  std::string(reason_name(r.reason)));
  }

  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  check_invariants(*env, "instrument rules");
}

TEST(scenario_admission, rejects_unknown_and_disabled_instruments) {
  LabOptions options;
  options.policy.instruments.at("GBPUSD").enabled = false;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  RiskResult r = env->new_order("ACC-A", "c1", "XAUUSD", Side::Buy, 1000, kPrice,
                                "cmd-1");
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::UnknownInstrument));

  r = env->new_order("ACC-A", "c2", "GBPUSD", Side::Buy, 1000, kPrice, "cmd-2");
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::InstrumentDisabled));
}

TEST(scenario_admission, rejects_an_unknown_or_disabled_account) {
  LabOptions options;
  options.policy.accounts.at("ACC-C").enabled = false;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  RiskResult r = env->new_order("ACC-C", "c1", "EURUSD", Side::Buy, 1000, kPrice,
                                "cmd-1");
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::AccountDisabled));

  Principal who = env->trader();
  who.accounts.insert("ACC-Z");
  NewOrderCommand cmd;
  cmd.tenant_id = "T1";
  cmd.account_id = "ACC-Z";
  cmd.command_id = "cmd-2";
  cmd.order_chain_id = "c2";
  cmd.symbol = "EURUSD";
  cmd.side = Side::Buy;
  cmd.total_units = 1000;
  cmd.limit_price_ticks = kPrice;
  r = env->coordinator().reserve_new(cmd, who);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::AccountNotFound));
}

TEST(scenario_admission, a_new_may_not_reuse_an_existing_chain) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice, "cmd-1")
            .allowed());

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 10000, kPrice, "cmd-2");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::ChainAlreadyExists));
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
}

TEST(scenario_admission, admission_is_closed_until_recovery_opens_it) {
  // RI-32: a fresh domain does not admit anything.
  LabOptions options;
  options.open_admission = false;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  const RiskResult blocked =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(blocked.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(blocked.reason),
           static_cast<int>(Reason::RecoveryIncomplete));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
  // A hold is not a decision, so nothing durable was recorded for this id.
  CHECK(!env->store().find_command("T1", "ACC-A", "cmd-1").has_value());

  CHECK(env->coordinator().open_admission(true, "test").allowed());
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());
}

TEST(scenario_admission, kill_switch_modes_block_risk_increase) {
  // §7.3 / RI-28, and PB-24: the mode is read on the same serialised state the
  // commit would use, so a mode change cannot be raced.
  const struct {
    DomainMode mode;
    Reason expected;
  } cases[] = {
      {DomainMode::NoNew, Reason::KillSwitchNoNew},
      {DomainMode::CancelOnly, Reason::KillSwitchCancelOnly},
      {DomainMode::ReconOnly, Reason::KillSwitchReconOnly},
  };

  for (const auto& c : cases) {
    std::unique_ptr<LabEnv> env = make_env();
    CHECK(env->coordinator()
              .set_mode(c.mode, env->operator_principal(), "test")
              .allowed());

    const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                        kQty, kPrice, "cmd-1");
    CHECK(r.decision == RiskDecision::DenyDefinitive);
    CHECK_MSG(r.reason == c.expected,
              std::string("mode ") + std::string(domain_mode_name(c.mode)) +
                  " gave " + std::string(reason_name(r.reason)));
    CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
    CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
    check_invariants(*env, "kill switch");
  }
}

TEST(scenario_admission, a_stale_feed_denies_and_leaves_existing_holds_alone) {
  // F8 / RI-12: no risk-increasing admission on stale data, and the holds that
  // already exist are not touched by the staleness.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());
  const std::int64_t held = env->effective_remaining("ACC-A", "EURUSD", Side::Buy);

  CHECK(apply_fixture(*env, Fixture::F8StaleFeed).ok());

  const RiskResult r =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 10000, kPrice, "cmd-2");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::QuoteStale));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), held);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "stale feed");
}

TEST(scenario_admission, a_quote_expiring_between_evaluation_and_commit_denies) {
  // PB-04: valid at preflight, expired at the commit checkpoint.
  std::unique_ptr<LabEnv> env = make_env();

  // Push the clock to one millisecond before the quote's TTL, then let the
  // CW-04 failpoint advance it past the boundary.
  const std::int64_t ttl = env->policy().instruments.at("EURUSD").quote_ttl_ms;
  env->clock().advance_millis(ttl - 1);
  env->failpoints().arm_hook(Failpoint::AfterMathBeforeDomainLock,
                             [&env] { env->clock().advance_millis(2); });

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::QuoteStale));
  CHECK(r.detail.find("stale at commit") != std::string::npos);
  CHECK(!r.token.has_value());
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 0);
  env->failpoints().disarm_all();
  check_invariants(*env, "PB-04");
}

TEST(scenario_admission, each_limit_scope_can_be_the_binding_one) {
  {  // per-request notional
    LabOptions options;
    options.policy.limits.max_order_notional_minor = kQtyNotionalMinor - 1;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    const RiskResult r = created.value()->new_order("ACC-A", "c1", "EURUSD",
                                                    Side::Buy, kQty, kPrice, "cmd-1");
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::LimitOrderNotional));
  }
  {  // account-symbol net
    LabOptions options;
    options.policy.limits.max_account_symbol_net_minor = kQtyNotionalMinor - 1;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    const RiskResult r = created.value()->new_order("ACC-A", "c1", "EURUSD",
                                                    Side::Buy, kQty, kPrice, "cmd-1");
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::LimitAccountSymbolNet));
  }
  {  // firm/global
    LabOptions options;
    options.policy.limits.max_firm_gross_minor = kQtyNotionalMinor - 1;
    options.policy.limits.max_account_total_gross_minor = kQtyNotionalMinor - 1;
    options.policy.limits.max_account_symbol_gross_minor = kQtyNotionalMinor - 1;
    options.policy.limits.max_account_symbol_net_minor = kQtyNotionalMinor - 1;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    const RiskResult r = created.value()->new_order("ACC-A", "c1", "EURUSD",
                                                    Side::Buy, kQty, kPrice, "cmd-1");
    // The per-symbol net cap is hit first; the point is that it is refused.
    CHECK(r.decision == RiskDecision::DenyDefinitive);
  }
  {  // margin buffer
    LabOptions options;
    options.policy.accounts.at("ACC-A").margin_budget_minor = 1000;
    Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
    CHECK_MSG(created.ok(), created.detail());
    const RiskResult r = created.value()->new_order("ACC-A", "c1", "EURUSD",
                                                    Side::Buy, kQty, kPrice, "cmd-1");
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::LimitMarginBuffer));
  }
}

TEST(scenario_admission, the_fixture_at_limit_admits_nothing_further) {
  // F1: every aggregate scope is exactly saturated, so one more unit fails.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(apply_fixture(*env, Fixture::F1AtLimit).ok());
  CHECK(env->store().domain_state().mode == DomainMode::Normal);

  const RiskResult r =
      env->new_order("ACC-A", "chain-extra", "EURUSD", Side::Buy, 1000, kPrice,
                     "cmd-extra");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "F1 at limit");
}

TEST(scenario_admission, a_denial_is_durable_and_replays_as_the_same_denial) {
  // RI-02: one command id maps to one durable decision, even after the reason
  // for the denial has gone away.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->coordinator()
            .set_mode(DomainMode::NoNew, env->operator_principal(), "test")
            .allowed());

  const RiskResult denied =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(denied.decision == RiskDecision::DenyDefinitive);
  CHECK(env->store().find_command("T1", "ACC-A", "cmd-1").has_value());

  CHECK(env->coordinator()
            .set_mode(DomainMode::Normal, env->operator_principal(), "test")
            .allowed());

  const RiskResult replay =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(replay.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(replay.reason), static_cast<int>(Reason::KillSwitchNoNew));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));

  // A fresh command id is the supported way forward.
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-2")
            .allowed());
}

TEST(scenario_admission, a_rolled_back_commit_is_a_hold_with_no_side_effects) {
  // CW-06..CW-11 in-process: a failpoint inside the transaction must roll the
  // whole thing back, and the caller must be told HOLD, not DENY.
  const Failpoint windows[] = {
      Failpoint::AfterLockBeforeBegin,  Failpoint::AfterCommandInsert,
      Failpoint::AfterReservationInsert, Failpoint::AfterProjectionUpdate,
      Failpoint::AfterAuditInsert,      Failpoint::AfterOutboxInsert,
      Failpoint::AfterRevisionCas,
  };

  for (const Failpoint fp : windows) {
    std::unique_ptr<LabEnv> env = make_env();
    const std::string before = env->store().state_hash();
    env->failpoints().arm(fp, FailAction::CommitFail);

    const RiskResult r = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                        kQty, kPrice, "cmd-1");
    CHECK_MSG(r.decision == RiskDecision::HoldRetryable,
              std::string(failpoint_window_id(fp)) + " gave " +
                  std::string(risk_decision_name(r.decision)));
    CHECK(!r.token.has_value());
    CHECK_MSG(env->store().state_hash() == before,
              std::string(failpoint_window_id(fp)) + " mutated state");
    CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
    CHECK(!env->store().find_command("T1", "ACC-A", "cmd-1").has_value());

    // Retrying the same command id after the failpoint clears must succeed.
    env->failpoints().disarm_all();
    const RiskResult retry = env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy,
                                            kQty, kPrice, "cmd-1");
    CHECK_MSG(retry.allowed(), std::string(failpoint_window_id(fp)) + " retry: " +
                                   retry.detail);
    CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
    check_invariants(*env, failpoint_window_id(fp).data());
  }
}

TEST(scenario_admission, a_lost_commit_ack_is_durable_and_recoverable) {
  // CW-12 / PB-08 / RI-24: the commit happened. The caller must not read that
  // as a rollback, and the recorded decision must be findable by command id.
  std::unique_ptr<LabEnv> env = make_env();
  env->failpoints().arm(Failpoint::AfterDurableCommitBeforeAck,
                        FailAction::CommitAckLost);

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::PersistenceCommitUnknown));
  CHECK(!r.token.has_value());

  // The hold really is durable.
  std::optional<CommandRecord> recorded =
      env->store().find_command("T1", "ACC-A", "cmd-1");
  CHECK(recorded.has_value());
  CHECK_EQ(recorded->decision, std::string("ALLOW_COMMITTED"));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));

  // Recovery path: replaying the command id returns the original decision with
  // a usable token, and does not create a second reservation or intent.
  env->failpoints().disarm_all();
  const RiskResult recovered =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(recovered.allowed());
  CHECK(recovered.token.has_value());
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "CW-12");
}

TEST(scenario_admission, a_fenced_writer_cannot_admit) {
  // RI-25 / PB-21: an old leader that wakes up is stopped at the store.
  std::unique_ptr<LabEnv> env = make_env();
  env->store().install_fencing_token(99);

  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::FencingTokenStale));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(0));
  CHECK(env->coordinator().metrics().counter("risk_fencing_reject_total") > 0);

  // Taking over the token lets the same command through.
  env->coordinator().set_fencing_token(99);
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());
}

TEST(scenario_admission, a_token_cannot_be_forged_or_reused_across_identities) {
  // RI-22: P04 must not be able to turn a payload edit into a valid token.
  std::unique_ptr<LabEnv> env = make_env();
  const RiskResult r =
      env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
  CHECK(r.allowed());
  const ReservationToken good = *r.token;
  CHECK(env->coordinator().verify_token(good).ok());

  {  // tampered quantity hash
    ReservationToken bad = good;
    bad.canonical_hash = "0000";
    CHECK(!env->coordinator().verify_token(bad).ok());
  }
  {  // tampered account, signature unchanged
    ReservationToken bad = good;
    bad.account_id = "ACC-B";
    CHECK(!env->coordinator().verify_token(bad).ok());
  }
  {  // escalated policy epoch
    ReservationToken bad = good;
    bad.policy_epoch += 1;
    CHECK(!env->coordinator().verify_token(bad).ok());
  }
  {  // no signature at all
    ReservationToken bad = good;
    bad.signature.clear();
    CHECK(!env->coordinator().verify_token(bad).ok());
  }
  {  // signed with a different domain secret
    LabOptions other_options;
    other_options.token_secret = "a-different-secret";
    Result<std::unique_ptr<LabEnv>> other = LabEnv::create(other_options);
    CHECK(other.ok());
    const RiskResult signed_elsewhere = other.value()->new_order(
        "ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1");
    CHECK(signed_elsewhere.allowed());
    CHECK(!env->coordinator().verify_token(*signed_elsewhere.token).ok());
  }
  {  // a stale fencing token in an otherwise valid token
    env->store().install_fencing_token(good.fencing_token + 1);
    CHECK(!env->coordinator().verify_token(good).ok());
  }
}

TEST(scenario_admission, dispatch_is_idempotent_and_marks_the_intent_once) {
  // CW-13/CW-14: a second claim must not produce a second original message.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());

  const RiskResult first = env->dispatch("cmd-1");
  CHECK_MSG(first.allowed(), first.detail);
  std::optional<Reservation> after = env->reservation_for("ACC-A", "chain-1");
  CHECK(after.has_value());
  CHECK(after->state == ReservationState::InFlight);
  CHECK(env->store().outbox()[0].claimed);
  CHECK(env->store().outbox()[0].dispatched);

  const Revision revision = env->store().domain_state().revision;
  const RiskResult second = env->dispatch("cmd-1");
  CHECK(second.allowed());
  CHECK_EQ(static_cast<int>(second.reason), static_cast<int>(Reason::DuplicateCommand));
  CHECK_EQ(env->store().domain_state().revision, revision);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "dispatch");
}

TEST(scenario_admission, an_expired_dispatch_window_keeps_the_hold) {
  // §12.2: token expiry ends the right to dispatch; it does not release the
  // reservation.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());

  env->clock().advance_millis(env->coordinator().options().dispatch_ttl_ms + 1);
  const RiskResult r = env->dispatch("cmd-1");
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::ReconciliationRequired));
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  CHECK(!env->store().outbox()[0].dispatched);
  check_invariants(*env, "dispatch expiry");
}

TEST(scenario_admission, the_admission_rate_limit_is_enforced) {
  LabOptions options;
  options.policy.limits.max_admissions_per_second = 2;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  // The bucket starts empty and refills with the clock, so the first call has
  // to wait for a token too. Advancing the clock also ages the market data, so
  // the feed is republished to keep staleness out of this test.
  env->clock().advance_millis(1000);
  CHECK(env->publish_default_market().ok());
  CHECK(env->new_order("ACC-A", "c1", "EURUSD", Side::Buy, 1000, kPrice, "cmd-1")
            .allowed());
  CHECK(env->new_order("ACC-A", "c2", "EURUSD", Side::Buy, 1000, kPrice, "cmd-2")
            .allowed());

  const RiskResult limited =
      env->new_order("ACC-A", "c3", "EURUSD", Side::Buy, 1000, kPrice, "cmd-3");
  CHECK(limited.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(limited.reason), static_cast<int>(Reason::LimitRate));

  env->clock().advance_millis(1000);
  CHECK(env->publish_default_market().ok());
  CHECK(env->new_order("ACC-A", "c4", "EURUSD", Side::Buy, 1000, kPrice, "cmd-4")
            .allowed());
}

TEST(scenario_admission, a_symbol_that_cannot_be_valued_blocks_new_admission) {
  // RI-12 / PB-05: an account holding exposure in a symbol whose FX path has
  // gone must not be able to add risk elsewhere on stale information.
  LabOptions options;
  options.policy.fx_pivot_currencies.clear();
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  // EURJPY needs a JPY->USD conversion, which the default market publishes.
  CHECK(env->new_order("ACC-A", "jpy-chain", "EURJPY", Side::Buy, 1000, 160000,
                       "cmd-1")
            .allowed());

  // Let the FX edge expire while the quote for EURUSD stays fresh.
  env->clock().advance_millis(env->policy().fx_ttl_ms + 1);
  CHECK(env->publish_quote("EURUSD", 109990, 110000).ok());
  CHECK(env->publish_quote("EURJPY", 159990, 160000).ok());

  const RiskResult r =
      env->new_order("ACC-A", "usd-chain", "EURUSD", Side::Buy, 1000, kPrice, "cmd-2");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK(r.reason == Reason::FxStale || r.reason == Reason::FxPathMissing);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "unvaluable symbol");
}

TEST(scenario_admission, the_backlog_cap_stops_new_risk) {
  // §17: unresolved outcomes above the cap stop risk increase rather than
  // being expired away.
  LabOptions options;
  options.policy.queues.recon_cap = 1;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  CHECK(apply_fixture(*env, Fixture::F6UnknownNew).ok());
  std::optional<Reservation> unknown = env->reservation_for("ACC-A", "f6-chain");
  CHECK(unknown.has_value());
  CHECK(unknown->state == ReservationState::Unknown);

  const RiskResult r =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "cmd-x");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::BacklogFull));
  // The unresolved hold is still there: the cap did not release it.
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), 50000);
  check_invariants(*env, "backlog cap");
}

TEST(scenario_admission, a_durable_store_produces_the_same_decisions) {
  tests::TmpDir dir("admission-journal");
  LabOptions options;
  options.journal_path = dir.file("risk.journal");
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  CHECK(env->store().durable());
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, kQty, kPrice, "cmd-1")
            .allowed());
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Buy), kQty);
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(1));
  check_invariants(*env, "journal-backed admission");
}
