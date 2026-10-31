#include "risk/scenario.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "risk/checked.hpp"
#include "risk/config.hpp"

namespace risk {
namespace {

constexpr const char* kFeedSource = "LAB-FEED";
constexpr const char* kLpNamespace = "lp";

std::string trim(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream stream(s);
  std::string token;
  while (stream >> token) out.push_back(token);
  return out;
}

// Parses `key=value` tokens into a map. A token without '=' is positional and
// stored under its index, which is how verbs like `set_mode NO_NEW` work.
std::map<std::string, std::string> parse_args(const std::vector<std::string>& tokens,
                                              std::size_t start) {
  std::map<std::string, std::string> args;
  int positional = 0;
  for (std::size_t i = start; i < tokens.size(); ++i) {
    const std::size_t eq = tokens[i].find('=');
    if (eq == std::string::npos) {
      args["$" + std::to_string(positional++)] = tokens[i];
    } else {
      args[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
  }
  return args;
}

bool arg_i64(const std::map<std::string, std::string>& args, const std::string& key,
             std::int64_t& out) {
  const auto it = args.find(key);
  if (it == args.end()) return false;
  const std::string& text = it->second;
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
  }
  std::int64_t acc = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    if (!mul_i64(acc, 10, acc)) return false;
    if (!add_i64(acc, static_cast<std::int64_t>(text[i] - '0'), acc)) return false;
  }
  out = negative ? -acc : acc;
  return true;
}

std::string arg_str(const std::map<std::string, std::string>& args,
                    const std::string& key, const std::string& fallback = {}) {
  const auto it = args.find(key);
  return it == args.end() ? fallback : it->second;
}

}  // namespace

std::string_view fixture_name(Fixture f) noexcept {
  switch (f) {
    case Fixture::F0Empty: return "F0_EMPTY";
    case Fixture::F1AtLimit: return "F1_AT_LIMIT";
    case Fixture::F2Partial: return "F2_PARTIAL";
    case Fixture::F3PendingCancel: return "F3_PENDING_CANCEL";
    case Fixture::F4PendingReplaceUp: return "F4_PENDING_REPLACE_UP";
    case Fixture::F5PendingReplaceDown: return "F5_PENDING_REPLACE_DOWN";
    case Fixture::F6UnknownNew: return "F6_UNKNOWN_NEW";
    case Fixture::F7MultiAccount: return "F7_MULTI_ACCOUNT";
    case Fixture::F8StaleFeed: return "F8_STALE_FEED";
    case Fixture::F9CrashPoint: return "F9_CRASH_POINT";
    case Fixture::F10BadExec: return "F10_BAD_EXEC";
    case Fixture::F11Breached: return "F11_BREACHED";
  }
  return "UNKNOWN_FIXTURE";
}

bool parse_fixture(std::string_view text, Fixture& out) noexcept {
  const Fixture all[] = {
      Fixture::F0Empty,          Fixture::F1AtLimit,
      Fixture::F2Partial,        Fixture::F3PendingCancel,
      Fixture::F4PendingReplaceUp, Fixture::F5PendingReplaceDown,
      Fixture::F6UnknownNew,     Fixture::F7MultiAccount,
      Fixture::F8StaleFeed,      Fixture::F9CrashPoint,
      Fixture::F10BadExec,       Fixture::F11Breached};
  for (const Fixture f : all) {
    if (fixture_name(f) == text) {
      out = f;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// LabEnv
// ---------------------------------------------------------------------------

LabEnv::~LabEnv() = default;

Result<std::unique_ptr<LabEnv>> LabEnv::create(const LabOptions& options) {
  if (const Status s = options.policy.validate(); !s) {
    return Result<std::unique_ptr<LabEnv>>::fail(s.reason(), s.detail());
  }

  auto env = std::unique_ptr<LabEnv>(new LabEnv());
  env->options_ = options;
  env->clock_ = std::make_unique<VirtualClock>();
  env->engine_clock_ = env->clock_.get();
  if (options.real_clock) {
    env->system_clock_ = std::make_unique<SystemClock>();
    env->engine_clock_ = env->system_clock_.get();
  }
  env->feed_ = std::make_unique<MarketFeed>();
  env->failpoints_ = std::make_unique<FailpointRegistry>();
  env->feed_->trust_source(kFeedSource);

  if (const Status s = env->build_store(); !s) {
    return Result<std::unique_ptr<LabEnv>>::fail(s.reason(), s.detail());
  }
  if (const Status s = env->build_coordinator(); !s) {
    return Result<std::unique_ptr<LabEnv>>::fail(s.reason(), s.detail());
  }

  env->trader_.principal_id = "trader-1";
  env->trader_.tenant_id = options.tenant_id;
  env->trader_.roles = {Role::Trader};
  for (const auto& [account, profile] : options.policy.accounts) {
    (void)profile;
    env->trader_.accounts.insert(account);
  }

  env->risk_admin_.principal_id = "risk-admin-1";
  env->risk_admin_.tenant_id = options.tenant_id;
  env->risk_admin_.roles = {Role::RiskAdmin};

  env->risk_admin2_.principal_id = "risk-admin-2";
  env->risk_admin2_.tenant_id = options.tenant_id;
  env->risk_admin2_.roles = {Role::RiskAdmin};

  env->operator_.principal_id = "operator-1";
  env->operator_.tenant_id = options.tenant_id;
  env->operator_.roles = {Role::Operator};

  if (options.publish_default_market) {
    if (const Status s = env->publish_default_market(); !s) {
      return Result<std::unique_ptr<LabEnv>>::fail(s.reason(), s.detail());
    }
  }
  if (options.open_admission) {
    const RiskResult r = env->coordinator_->open_admission(true, "lab bootstrap");
    if (!r.allowed()) {
      return Result<std::unique_ptr<LabEnv>>::fail(r.reason, r.detail);
    }
  }

  return Result<std::unique_ptr<LabEnv>>(std::move(env));
}

Status LabEnv::build_store() {
  RiskDomainState domain;
  domain.tenant_id = options_.tenant_id;
  domain.domain_id = options_.domain_id;
  domain.fencing_token = options_.fencing_token;
  domain.policy_version = options_.policy.version;
  domain.mode = options_.policy.mode;

  if (options_.journal_path.empty()) {
    store_ = std::make_unique<MemoryStore>(domain, options_.policy, failpoints_.get());
    return Status::success();
  }
  Result<std::unique_ptr<JournalStore>> journal = JournalStore::open(
      options_.journal_path, domain, options_.policy, failpoints_.get());
  if (!journal.ok()) return Status::fail(journal.reason(), journal.detail());
  store_ = std::move(journal.value());
  return Status::success();
}

Status LabEnv::build_coordinator() {
  RiskCoordinator::Options opts;
  opts.tenant_id = options_.tenant_id;
  opts.domain_id = options_.domain_id;
  opts.fencing_token = options_.fencing_token;
  opts.token_secret = options_.token_secret;
  coordinator_ = std::make_unique<RiskCoordinator>(*store_, *feed_, *engine_clock_,
                                                   *failpoints_, opts);
  return Status::success();
}

Status LabEnv::publish_quote(const SymbolId& symbol, std::int64_t bid_ticks,
                             std::int64_t ask_ticks, std::int64_t ttl_ms) {
  const InstrumentSpec* spec = options_.policy.find_instrument(symbol);
  if (spec == nullptr) return Status::fail(Reason::UnknownInstrument, symbol);

  Quote q;
  q.source_id = kFeedSource;
  q.symbol = symbol;
  q.bid_ticks = bid_ticks;
  q.ask_ticks = ask_ticks;
  q.event_time_utc = engine_clock_->utc_now();
  q.ingest_mono = engine_clock_->mono_now();
  q.sequence = ++quote_sequence_;
  q.version = feed_->next_version();
  q.ttl_ms = ttl_ms > 0 ? ttl_ms : spec->quote_ttl_ms;
  return feed_->publish_quote(q);
}

Status LabEnv::publish_fx(const std::string& from, const std::string& to,
                          const Rational& bid, const Rational& ask,
                          std::int64_t ttl_ms) {
  FxRate r;
  r.source_id = kFeedSource;
  r.from_currency = from;
  r.to_currency = to;
  r.bid = bid;
  r.ask = ask;
  r.event_time_utc = engine_clock_->utc_now();
  r.ingest_mono = engine_clock_->mono_now();
  r.sequence = ++quote_sequence_;
  r.version = feed_->next_version();
  r.ttl_ms = ttl_ms > 0 ? ttl_ms : options_.policy.fx_ttl_ms;
  return feed_->publish_fx(r);
}

Status LabEnv::publish_equity(const AccountId& account, std::int64_t equity_minor,
                              std::int64_t existing_margin_minor) {
  EquitySnapshot e;
  e.account_id = account;
  e.currency = options_.policy.account_currency.code;
  e.equity_minor = equity_minor;
  e.existing_margin_minor = existing_margin_minor;
  e.ingest_mono = engine_clock_->mono_now();
  e.version = feed_->next_version();
  e.ttl_ms = options_.policy.equity_ttl_ms;
  return feed_->publish_equity(e);
}

Status LabEnv::publish_default_market() {
  // EURUSD at 1.09990 / 1.10000 with a 1/100000 tick: 109990 / 110000 ticks.
  for (const auto& [symbol, spec] : options_.policy.instruments) {
    std::int64_t ask = 110000;
    if (spec.quote_currency == "JPY") ask = 160000;  // tick is 1/1000 for EURJPY
    const std::int64_t bid = ask - 10;
    if (const Status s = publish_quote(symbol, bid, ask); !s) return s;
  }

  // Direct quote-currency -> account-currency edges, plus one pivot leg so the
  // pivot path is exercised by the standard market.
  const std::string account_ccy = options_.policy.account_currency.code;
  for (const auto& [symbol, spec] : options_.policy.instruments) {
    (void)symbol;
    if (spec.quote_currency == account_ccy) continue;
    if (spec.quote_currency == "JPY") {
      // 1 JPY = 0.0067 USD, quoted as a bid/ask pair.
      if (const Status s = publish_fx("JPY", account_ccy, Rational{66, 10000},
                                      Rational{67, 10000});
          !s) {
        return s;
      }
    }
  }

  for (const auto& [account, profile] : options_.policy.accounts) {
    (void)profile;
    if (const Status s = publish_equity(account, 10'000'000, 0); !s) return s;
  }
  return Status::success();
}

RiskResult LabEnv::new_order(const AccountId& account, const OrderChainId& chain,
                             const SymbolId& symbol, Side side, std::int64_t units,
                             std::int64_t limit_price_ticks,
                             const CommandId& command_id) {
  NewOrderCommand cmd;
  cmd.tenant_id = options_.tenant_id;
  cmd.account_id = account;
  cmd.command_id =
      command_id.empty() ? "cmd-" + std::to_string(++command_counter_) : command_id;
  cmd.order_chain_id = chain;
  cmd.symbol = symbol;
  cmd.side = side;
  cmd.total_units = units;
  cmd.limit_price_ticks = limit_price_ticks;

  RiskResult result = coordinator_->reserve_new(cmd, trader_);
  if (result.token.has_value()) tokens_[cmd.command_id] = *result.token;
  return result;
}

RiskResult LabEnv::replace_order(const AccountId& account, const OrderChainId& chain,
                                 std::int64_t new_units, std::int64_t new_price_ticks,
                                 const CommandId& command_id) {
  ReplaceCommand cmd;
  cmd.tenant_id = options_.tenant_id;
  cmd.account_id = account;
  cmd.command_id =
      command_id.empty() ? "cmd-" + std::to_string(++command_counter_) : command_id;
  cmd.order_chain_id = chain;
  cmd.new_total_units = new_units;
  cmd.new_limit_price_ticks = new_price_ticks;

  RiskResult result = coordinator_->reserve_replace(cmd, trader_);
  if (result.token.has_value()) tokens_[cmd.command_id] = *result.token;
  return result;
}

RiskResult LabEnv::cancel_order(const AccountId& account, const OrderChainId& chain,
                                const CommandId& command_id) {
  CancelCommand cmd;
  cmd.tenant_id = options_.tenant_id;
  cmd.account_id = account;
  cmd.command_id =
      command_id.empty() ? "cmd-" + std::to_string(++command_counter_) : command_id;
  cmd.order_chain_id = chain;

  RiskResult result = coordinator_->request_cancel(cmd, trader_);
  if (result.token.has_value()) tokens_[cmd.command_id] = *result.token;
  return result;
}

RiskResult LabEnv::dispatch(const CommandId& command_id) {
  const auto it = tokens_.find(command_id);
  if (it == tokens_.end()) {
    RiskResult r;
    r.decision = RiskDecision::DenyDefinitive;
    r.reason = Reason::ReservationNotFound;
    r.detail = "no token recorded for " + command_id;
    return r;
  }
  return coordinator_->mark_dispatched(it->second);
}

RiskResult LabEnv::lp_event(const VerifiedOrderEvent& event) {
  return coordinator_->apply_verified_order_event(event);
}

RiskResult LabEnv::lp_accept(const AccountId& account, const OrderChainId& chain,
                             const EventId& event_id) {
  VerifiedOrderEvent e;
  e.tenant_id = options_.tenant_id;
  e.account_id = account;
  e.order_chain_id = chain;
  e.source_namespace = kLpNamespace;
  e.event_id = event_id;
  e.kind = OrderEventKind::OrderAccepted;
  if (std::optional<Reservation> r = reservation_for(account, chain); r.has_value()) {
    e.symbol = r->symbol;
    e.side = r->side;
    e.accepted_total_units = r->accepted_total_units;
  }
  return lp_event(e);
}

RiskResult LabEnv::lp_fill(const AccountId& account, const OrderChainId& chain,
                           const EventId& event_id, std::int64_t units) {
  VerifiedOrderEvent e;
  e.tenant_id = options_.tenant_id;
  e.account_id = account;
  e.order_chain_id = chain;
  e.source_namespace = kLpNamespace;
  e.event_id = event_id;
  e.kind = OrderEventKind::AppliedFill;
  e.fill_units = units;
  if (std::optional<Reservation> r = reservation_for(account, chain); r.has_value()) {
    e.symbol = r->symbol;
    e.side = r->side;
    e.verified_cum_units = r->cum_units + units;
  }
  return lp_event(e);
}

Status LabEnv::restart() {
  if (options_.journal_path.empty()) {
    return Status::fail(Reason::Unsupported,
                        "restart requires a journal-backed environment");
  }
  // Drop the coordinator first: it holds a reference to the old store.
  coordinator_.reset();
  store_.reset();
  tokens_.clear();

  if (const Status s = build_store(); !s) return s;
  if (const Status s = build_coordinator(); !s) return s;

  RecoveryOptions recovery_options;
  Result<RecoveryReport> report = run_recovery(*coordinator_, *store_, *feed_,
                                               *engine_clock_, recovery_options);
  if (!report.ok()) return Status::fail(report.reason(), report.detail());
  last_recovery_ = report.value();
  return Status::success();
}

std::optional<ReservationToken> LabEnv::token_for(const CommandId& command_id) const {
  const auto it = tokens_.find(command_id);
  if (it == tokens_.end()) return std::nullopt;
  return it->second;
}

std::optional<Reservation> LabEnv::reservation_for(const AccountId& account,
                                                   const OrderChainId& chain) const {
  std::optional<ReservationId> id =
      store_->find_chain(options_.tenant_id, account, chain);
  if (!id.has_value()) return std::nullopt;
  return store_->find_reservation(*id);
}

std::int64_t LabEnv::effective_remaining(const AccountId& account,
                                         const SymbolId& symbol, Side side) const {
  const DomainExposure exposure = store_->exposure();
  const AccountExposure* ae = exposure.find(account);
  if (ae == nullptr) return 0;
  const SymbolExposure* se = ae->find(symbol);
  if (se == nullptr) return 0;
  return side == Side::Buy ? se->pending_buy_units : se->pending_sell_units;
}

std::int64_t LabEnv::position(const AccountId& account, const SymbolId& symbol) const {
  const DomainExposure exposure = store_->exposure();
  const AccountExposure* ae = exposure.find(account);
  if (ae == nullptr) return 0;
  const SymbolExposure* se = ae->find(symbol);
  return se == nullptr ? 0 : se->position_units;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

Status apply_fixture(LabEnv& env, Fixture fixture) {
  const SymbolId symbol = "EURUSD";
  const std::int64_t price = 110000;

  switch (fixture) {
    case Fixture::F0Empty:
      // Fresh quotes, no positions, every cap under budget.
      return Status::success();

    case Fixture::F1AtLimit: {
      // Saturates every aggregate scope *exactly*, so the next unit of
      // exposure is the one that must be refused. The caps are set from the
      // measured notional rather than a guessed constant, which keeps the
      // fixture honest if the lab price or buffer changes.
      const RiskResult r =
          env.new_order("ACC-A", "f1-chain", symbol, Side::Buy, 100000, price, "f1-new");
      if (!r.allowed()) return Status::fail(r.reason, "F1 setup: " + r.detail);

      Result<DomainMetrics> metrics = env.coordinator().current_metrics();
      if (!metrics.ok()) {
        return Status::fail(metrics.reason(), "F1 metrics: " + metrics.detail());
      }
      const AccountMetrics* am = metrics.value().find("ACC-A");
      if (am == nullptr || am->symbols.count(symbol) == 0) {
        return Status::fail(Reason::InternalInvariant, "F1: no metrics for ACC-A");
      }
      const SymbolMetrics& sm = am->symbols.at(symbol);

      RiskPolicy at_limit = env.store().policy();
      at_limit.version += 1;
      at_limit.limits.max_account_symbol_net_minor = sm.net_notional_minor;
      at_limit.limits.max_account_symbol_gross_minor = sm.gross_notional_minor;
      at_limit.limits.max_account_total_gross_minor = am->total_gross_notional_minor;
      at_limit.limits.max_firm_gross_minor =
          metrics.value().firm_gross_notional_minor;
      const RiskResult installed = env.coordinator().install_policy(
          at_limit, env.risk_admin(), env.risk_admin2(), "F1 saturate caps");
      if (!installed.allowed()) {
        return Status::fail(installed.reason, "F1 policy: " + installed.detail);
      }
      // Exactly at the cap is not a breach, so the domain must still be NORMAL.
      if (env.store().domain_state().mode != DomainMode::Normal) {
        return Status::fail(Reason::InternalInvariant,
                            "F1: saturating the caps should not breach them");
      }
      return Status::success();
    }

    case Fixture::F2Partial: {
      const RiskResult r =
          env.new_order("ACC-A", "f2-chain", symbol, Side::Buy, 100000, price, "f2-new");
      if (!r.allowed()) return Status::fail(r.reason, "F2 setup: " + r.detail);
      if (const RiskResult d = env.dispatch("f2-new"); !d.allowed()) {
        return Status::fail(d.reason, "F2 dispatch: " + d.detail);
      }
      if (const RiskResult a = env.lp_accept("ACC-A", "f2-chain", "f2-ack");
          !a.allowed()) {
        return Status::fail(a.reason, "F2 accept: " + a.detail);
      }
      const RiskResult f = env.lp_fill("ACC-A", "f2-chain", "f2-exec-1", 40000);
      if (!f.allowed()) return Status::fail(f.reason, "F2 fill: " + f.detail);
      return Status::success();
    }

    case Fixture::F3PendingCancel: {
      if (const Status s = apply_fixture(env, Fixture::F2Partial); !s) return s;
      const RiskResult c = env.cancel_order("ACC-A", "f2-chain", "f3-cancel");
      if (!c.allowed()) return Status::fail(c.reason, "F3 cancel: " + c.detail);
      return Status::success();
    }

    case Fixture::F4PendingReplaceUp: {
      if (const Status s = apply_fixture(env, Fixture::F2Partial); !s) return s;
      // Proposed total 150k against accepted 100k with 40k filled:
      // max(60k, 110k) = 110k must be held.
      const RiskResult r =
          env.replace_order("ACC-A", "f2-chain", 150000, price, "f4-replace");
      if (!r.allowed()) return Status::fail(r.reason, "F4 replace: " + r.detail);
      return Status::success();
    }

    case Fixture::F5PendingReplaceDown: {
      if (const Status s = apply_fixture(env, Fixture::F2Partial); !s) return s;
      // Proposed total 50k against accepted 100k with 40k filled:
      // max(60k, 10k) = 60k, i.e. the old branch still governs.
      const RiskResult r =
          env.replace_order("ACC-A", "f2-chain", 50000, price, "f5-replace");
      if (!r.allowed()) return Status::fail(r.reason, "F5 replace: " + r.detail);
      return Status::success();
    }

    case Fixture::F6UnknownNew: {
      const RiskResult r =
          env.new_order("ACC-A", "f6-chain", symbol, Side::Buy, 50000, price, "f6-new");
      if (!r.allowed()) return Status::fail(r.reason, "F6 setup: " + r.detail);
      if (const RiskResult d = env.dispatch("f6-new"); !d.allowed()) {
        return Status::fail(d.reason, "F6 dispatch: " + d.detail);
      }
      VerifiedOrderEvent unknown;
      unknown.tenant_id = env.options().tenant_id;
      unknown.account_id = "ACC-A";
      unknown.order_chain_id = "f6-chain";
      unknown.source_namespace = "recon";
      unknown.event_id = "f6-unknown";
      unknown.kind = OrderEventKind::UnknownOutcome;
      unknown.symbol = symbol;
      unknown.side = Side::Buy;
      const RiskResult u = env.lp_event(unknown);
      // UnknownOutcome deliberately reports a hold, not an allow.
      if (u.decision != RiskDecision::HoldRetryable) {
        return Status::fail(u.reason, "F6 unknown: " + u.detail);
      }
      return Status::success();
    }

    case Fixture::F7MultiAccount: {
      // Two accounts contending for the same firm budget; no orders yet, so a
      // concurrency test can start both from the same committed revision.
      return Status::success();
    }

    case Fixture::F8StaleFeed: {
      // Push the clock past every TTL without republishing.
      std::int64_t longest = env.policy().equity_ttl_ms;
      for (const auto& [symbol_id, spec] : env.policy().instruments) {
        (void)symbol_id;
        longest = std::max(longest, spec.quote_ttl_ms);
      }
      longest = std::max(longest, env.policy().fx_ttl_ms);
      env.clock().advance_millis(longest + 1);
      return Status::success();
    }

    case Fixture::F9CrashPoint: {
      // A single live order, ready for a failpoint to be armed around it.
      const RiskResult r =
          env.new_order("ACC-A", "f9-chain", symbol, Side::Buy, 20000, price, "f9-new");
      if (!r.allowed()) return Status::fail(r.reason, "F9 setup: " + r.detail);
      return Status::success();
    }

    case Fixture::F10BadExec: {
      if (const Status s = apply_fixture(env, Fixture::F2Partial); !s) return s;
      // The contradictory second execution is left to the test so it can
      // assert on the exact quarantine it triggers.
      return Status::success();
    }

    case Fixture::F11Breached: {
      if (const Status s = apply_fixture(env, Fixture::F1AtLimit); !s) return s;
      // Tighten the caps under live exposure: revaluation must mark the domain
      // NO_NEW without deleting anything (PB-25).
      RiskPolicy tightened = env.store().policy();
      tightened.version += 1;
      tightened.limits.max_account_symbol_net_minor = 1;
      tightened.limits.max_account_symbol_gross_minor = 1;
      tightened.limits.max_account_total_gross_minor = 1;
      tightened.limits.max_firm_gross_minor = 1;
      const RiskResult r = env.coordinator().install_policy(
          tightened, env.risk_admin(), env.risk_admin2(), "F11 tighten");
      if (!r.allowed()) return Status::fail(r.reason, "F11 policy: " + r.detail);
      return Status::success();
    }
  }
  return Status::fail(Reason::Unsupported, "unknown fixture");
}

// ---------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------

std::string ScenarioRun::render() const {
  std::string out = "scenario_id=" + scenario_id + "\n";
  out += "seed=" + std::to_string(seed) + "\n";
  out += "fixture=" + std::string(fixture_name(fixture)) + "\n";
  out += std::string("store=") + (durable ? "journal" : "memory") + "\n";
  out += std::string("ok=") + (ok ? "1" : "0") + "\n";
  if (!failure.empty()) out += "failure=" + failure + "\n";
  for (const ScenarioStepResult& s : steps) {
    out += "line " + std::to_string(s.line) + " " + s.directive + " -> " +
           (s.ok ? "ok" : "FAIL") + (s.detail.empty() ? "" : " (" + s.detail + ")") +
           "\n";
  }
  return out;
}

namespace {

// Executes one `step` directive. Returns an empty string on success or the
// failure detail otherwise.
std::string run_step(LabEnv& env, const std::vector<std::string>& tokens) {
  const std::string verb = tokens[1];
  const std::map<std::string, std::string> args = parse_args(tokens, 2);

  auto expect_decision = [&](const RiskResult& result) -> std::string {
    const std::string expected = arg_str(args, "expect", "ALLOW_COMMITTED");
    const std::string actual = std::string(risk_decision_name(result.decision));
    if (expected != actual) {
      return "expected " + expected + " got " + actual + " (" +
             std::string(reason_name(result.reason)) + ": " + result.detail + ")";
    }
    const std::string expected_reason = arg_str(args, "reason");
    if (!expected_reason.empty() &&
        expected_reason != std::string(reason_name(result.reason))) {
      return "expected reason " + expected_reason + " got " +
             std::string(reason_name(result.reason));
    }
    return {};
  };

  if (verb == "open_admission") {
    const bool open = arg_str(args, "$0", "true") != "false";
    const RiskResult r = env.coordinator().open_admission(open, "scenario");
    return r.allowed() ? std::string{} : "open_admission failed: " + r.detail;
  }

  if (verb == "set_mode") {
    DomainMode mode = DomainMode::Normal;
    if (!parse_domain_mode(arg_str(args, "$0"), mode)) {
      return "bad mode " + arg_str(args, "$0");
    }
    const RiskResult r =
        env.coordinator().set_mode(mode, env.operator_principal(), "scenario");
    return r.allowed() ? std::string{} : "set_mode failed: " + r.detail;
  }

  if (verb == "advance_ms") {
    std::int64_t ms = 0;
    if (!arg_i64(args, "$0", ms)) return "advance_ms needs a value";
    env.clock().advance_millis(ms);
    return {};
  }

  if (verb == "publish_market") {
    const Status s = env.publish_default_market();
    return s ? std::string{} : "publish_market failed: " + s.detail();
  }

  if (verb == "publish_quote") {
    std::int64_t bid = 0, ask = 0, ttl = 0;
    if (!arg_i64(args, "bid", bid) || !arg_i64(args, "ask", ask)) {
      return "publish_quote needs bid= and ask=";
    }
    (void)arg_i64(args, "ttl_ms", ttl);
    const Status s = env.publish_quote(arg_str(args, "symbol", "EURUSD"), bid, ask, ttl);
    return s ? std::string{} : "publish_quote failed: " + s.detail();
  }

  if (verb == "failpoint") {
    Failpoint fp = Failpoint::None;
    if (!parse_failpoint(arg_str(args, "name"), fp)) {
      return "unknown failpoint " + arg_str(args, "name");
    }
    const std::string action = arg_str(args, "action", "commit_fail");
    FailAction fa = FailAction::CommitFail;
    if (action == "commit_fail") {
      fa = FailAction::CommitFail;
    } else if (action == "commit_ack_lost") {
      fa = FailAction::CommitAckLost;
    } else if (action == "fence_reject") {
      fa = FailAction::FenceReject;
    } else if (action == "abort") {
      fa = FailAction::Abort;
    } else {
      return "unknown failpoint action " + action;
    }
    std::int64_t times = 1, skip = 0;
    (void)arg_i64(args, "times", times);
    (void)arg_i64(args, "skip", skip);
    env.failpoints().arm(fp, fa, static_cast<int>(times), static_cast<int>(skip));
    return {};
  }

  if (verb == "disarm_failpoints") {
    env.failpoints().disarm_all();
    return {};
  }

  if (verb == "reserve_new") {
    Side side = Side::Buy;
    if (!parse_side(arg_str(args, "side", "BUY"), side)) {
      return "bad side " + arg_str(args, "side");
    }
    std::int64_t qty = 0, price = 0;
    if (!arg_i64(args, "qty", qty) || !arg_i64(args, "price", price)) {
      return "reserve_new needs qty= and price=";
    }
    const RiskResult r = env.new_order(
        arg_str(args, "account", "ACC-A"), arg_str(args, "chain", "chain-1"),
        arg_str(args, "symbol", "EURUSD"), side, qty, price,
        arg_str(args, "command_id"));
    return expect_decision(r);
  }

  if (verb == "reserve_replace") {
    std::int64_t qty = 0, price = 0;
    if (!arg_i64(args, "qty", qty) || !arg_i64(args, "price", price)) {
      return "reserve_replace needs qty= and price=";
    }
    const RiskResult r = env.replace_order(
        arg_str(args, "account", "ACC-A"), arg_str(args, "chain", "chain-1"), qty,
        price, arg_str(args, "command_id"));
    return expect_decision(r);
  }

  if (verb == "request_cancel") {
    const RiskResult r =
        env.cancel_order(arg_str(args, "account", "ACC-A"),
                         arg_str(args, "chain", "chain-1"), arg_str(args, "command_id"));
    return expect_decision(r);
  }

  if (verb == "dispatch") {
    const RiskResult r = env.dispatch(arg_str(args, "command_id"));
    return expect_decision(r);
  }

  if (verb == "lp_fill") {
    std::int64_t qty = 0;
    if (!arg_i64(args, "qty", qty)) return "lp_fill needs qty=";
    const RiskResult r =
        env.lp_fill(arg_str(args, "account", "ACC-A"),
                    arg_str(args, "chain", "chain-1"), arg_str(args, "event_id"), qty);
    return expect_decision(r);
  }

  if (verb == "event") {
    OrderEventKind kind = OrderEventKind::OrderAccepted;
    if (!parse_order_event_kind(arg_str(args, "kind"), kind)) {
      return "unknown event kind " + arg_str(args, "kind");
    }
    VerifiedOrderEvent e;
    e.tenant_id = env.options().tenant_id;
    e.account_id = arg_str(args, "account", "ACC-A");
    e.order_chain_id = arg_str(args, "chain", "chain-1");
    e.source_namespace = arg_str(args, "namespace", "lp");
    e.event_id = arg_str(args, "event_id");
    e.kind = kind;
    e.symbol = arg_str(args, "symbol");
    (void)parse_side(arg_str(args, "side", "BUY"), e.side);
    (void)arg_i64(args, "fill", e.fill_units);
    (void)arg_i64(args, "cum", e.verified_cum_units);
    (void)arg_i64(args, "accepted", e.accepted_total_units);
    e.related_command_id = arg_str(args, "command_id");
    if (e.symbol.empty()) {
      if (std::optional<Reservation> r =
              env.reservation_for(e.account_id, e.order_chain_id);
          r.has_value()) {
        e.symbol = r->symbol;
        e.side = r->side;
      }
    }
    const RiskResult r = env.lp_event(e);
    return expect_decision(r);
  }

  if (verb == "reconcile") {
    ReconcileEvidence ev;
    ev.tenant_id = env.options().tenant_id;
    ev.account_id = arg_str(args, "account", "ACC-A");
    ev.order_chain_id = arg_str(args, "chain", "chain-1");
    ev.evidence_id = arg_str(args, "evidence_id", "ev-1");
    if (!parse_reservation_state(arg_str(args, "state", "WORKING"),
                                 ev.resolved_state)) {
      return "bad resolved state " + arg_str(args, "state");
    }
    (void)arg_i64(args, "cum", ev.verified_cum_units);
    (void)arg_i64(args, "accepted", ev.verified_accepted_total_units);
    ev.note = arg_str(args, "note");
    const RiskResult r = env.coordinator().reconcile_with_evidence(
        ev, env.operator_principal(), env.risk_admin());
    return expect_decision(r);
  }

  if (verb == "restart") {
    const Status s = env.restart();
    return s ? std::string{} : "restart failed: " + s.detail();
  }

  if (verb == "assert") {
    const std::string account = arg_str(args, "account", "ACC-A");
    const std::string symbol = arg_str(args, "symbol", "EURUSD");
    const std::string chain = arg_str(args, "chain");

    std::int64_t expected = 0;
    if (arg_i64(args, "effective_buy_remaining", expected)) {
      const std::int64_t actual = env.effective_remaining(account, symbol, Side::Buy);
      if (actual != expected) {
        return "effective_buy_remaining expected " + std::to_string(expected) +
               " got " + std::to_string(actual);
      }
    }
    if (arg_i64(args, "effective_sell_remaining", expected)) {
      const std::int64_t actual = env.effective_remaining(account, symbol, Side::Sell);
      if (actual != expected) {
        return "effective_sell_remaining expected " + std::to_string(expected) +
               " got " + std::to_string(actual);
      }
    }
    if (arg_i64(args, "position", expected)) {
      const std::int64_t actual = env.position(account, symbol);
      if (actual != expected) {
        return "position expected " + std::to_string(expected) + " got " +
               std::to_string(actual);
      }
    }
    if (arg_i64(args, "outbox_count", expected)) {
      const std::int64_t actual = static_cast<std::int64_t>(env.store().outbox().size());
      if (actual != expected) {
        return "outbox_count expected " + std::to_string(expected) + " got " +
               std::to_string(actual);
      }
    }
    if (arg_i64(args, "confirmed_cum_qty", expected)) {
      std::optional<Reservation> r = env.reservation_for(account, chain);
      if (!r.has_value()) return "no reservation for chain " + chain;
      if (r->cum_units != expected) {
        return "confirmed_cum_qty expected " + std::to_string(expected) + " got " +
               std::to_string(r->cum_units);
      }
    }
    if (const std::string expected_state = arg_str(args, "reservation_state");
        !expected_state.empty()) {
      std::optional<Reservation> r = env.reservation_for(account, chain);
      if (!r.has_value()) return "no reservation for chain " + chain;
      if (std::string(reservation_state_name(r->state)) != expected_state) {
        return "reservation_state expected " + expected_state + " got " +
               std::string(reservation_state_name(r->state));
      }
    }
    if (const std::string expected_mode = arg_str(args, "mode"); !expected_mode.empty()) {
      const std::string actual =
          std::string(domain_mode_name(env.store().domain_state().mode));
      if (actual != expected_mode) {
        return "mode expected " + expected_mode + " got " + actual;
      }
    }
    if (arg_str(args, "all_risk_invariants") == "true") {
      if (const Status s = env.coordinator().cross_check_projection(); !s) {
        return "projection cross-check failed: " + s.detail();
      }
      const AuditChainReport chain_report = verify_audit_chain(env.store().audit());
      if (!chain_report.ok) {
        return "audit chain failed: " + chain_report.detail;
      }
    }
    return {};
  }

  return "unknown step verb: " + verb;
}

}  // namespace

Result<ScenarioRun> run_scenario_text(const std::string& text,
                                      const std::string& journal_path) {
  ScenarioRun run;

  // Header pass: the fixture and store choice must be known before the
  // environment is built.
  std::vector<std::pair<int, std::string>> step_lines;
  {
    std::istringstream stream(text);
    std::string raw;
    int number = 0;
    while (std::getline(stream, raw)) {
      ++number;
      const std::size_t comment = raw.find('#');
      const std::string line = trim(comment == std::string::npos
                                        ? raw
                                        : raw.substr(0, comment));
      if (line.empty()) continue;
      const std::vector<std::string> tokens = split_ws(line);
      if (tokens.empty()) continue;

      if (tokens[0] == "scenario_id" && tokens.size() >= 2) {
        run.scenario_id = tokens[1];
      } else if (tokens[0] == "seed" && tokens.size() >= 2) {
        const std::map<std::string, std::string> args = {{"v", tokens[1]}};
        (void)arg_i64(args, "v", run.seed);
      } else if (tokens[0] == "fixture" && tokens.size() >= 2) {
        if (!parse_fixture(tokens[1], run.fixture)) {
          return Result<ScenarioRun>::fail(Reason::Unsupported,
                                           "unknown fixture " + tokens[1]);
        }
      } else if (tokens[0] == "store" && tokens.size() >= 2) {
        if (tokens[1] == "journal") {
          run.durable = true;
        } else if (tokens[1] == "memory") {
          run.durable = false;
        } else {
          return Result<ScenarioRun>::fail(
              Reason::Unsupported,
              "line " + std::to_string(number) + ": store must be journal or memory");
        }
      } else if (tokens[0] == "step" && tokens.size() >= 2) {
        step_lines.emplace_back(number, line);
      } else {
        return Result<ScenarioRun>::fail(
            Reason::Unsupported,
            "line " + std::to_string(number) + ": unknown directive " + tokens[0]);
      }
    }
  }

  // A scenario that restarts needs a durable store. When the caller did not
  // name a journal, one is created in the temporary directory and removed
  // afterwards, so `step restart` works from a plain `risk_scenario file`.
  std::string resolved_journal = journal_path;
  bool owns_journal = false;
  if (resolved_journal.empty() && run.durable) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path(ec) /
        ("p05-scenario-" + std::to_string(counter.fetch_add(1)) + ".journal");
    if (ec) {
      return Result<ScenarioRun>::fail(Reason::PersistenceUnavailable,
                                       "no temporary directory available");
    }
    std::filesystem::remove(path, ec);
    resolved_journal = path.string();
    owns_journal = true;
  }

  struct JournalCleanup {
    std::string path;
    bool owned = false;
    ~JournalCleanup() {
      if (!owned) return;
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{resolved_journal, owns_journal};

  LabOptions options;
  options.journal_path = resolved_journal;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  if (!env.ok()) return Result<ScenarioRun>::fail(env.reason(), env.detail());

  if (const Status s = apply_fixture(*env.value(), run.fixture); !s) {
    run.ok = false;
    run.failure = "fixture " + std::string(fixture_name(run.fixture)) + ": " +
                  s.detail();
    return Result<ScenarioRun>(run);
  }

  for (const auto& [number, line] : step_lines) {
    ScenarioStepResult step;
    step.line = number;
    step.directive = line;
    const std::string failure = run_step(*env.value(), split_ws(line));
    step.ok = failure.empty();
    step.detail = failure;
    run.steps.push_back(step);
    if (!step.ok) {
      run.ok = false;
      run.failure = "line " + std::to_string(number) + ": " + failure;
      break;
    }
  }

  // A scenario that passed its own assertions still has to leave the engine
  // self-consistent.
  if (run.ok) {
    if (const Status s = env.value()->coordinator().cross_check_projection(); !s) {
      run.ok = false;
      run.failure = "post-run projection cross-check: " + s.detail();
    } else if (const AuditChainReport chain =
                   verify_audit_chain(env.value()->store().audit());
               !chain.ok) {
      run.ok = false;
      run.failure = "post-run audit chain: " + chain.detail;
    }
  }

  return Result<ScenarioRun>(run);
}

Result<ScenarioRun> run_scenario_file(const std::string& path,
                                      const std::string& journal_path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Result<ScenarioRun>::fail(Reason::Unsupported, "cannot open " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return run_scenario_text(buffer.str(), journal_path);
}

}  // namespace risk
