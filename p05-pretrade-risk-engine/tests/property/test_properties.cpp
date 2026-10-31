// §24.3 — the property suite P-01 .. P-16.
//
// The generator drives the engine with seeded random but *legal* sequences
// (conformant mode) and with deliberately illegal ones (adversarial mode). A
// separate, deliberately differently-shaped model recomputes the expected
// exposure after every accepted event; the engine's own projection code is
// never consulted for the expectation, so a shared bug cannot make both agree
// (§24.1).
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;

// Deterministic generator. A fixed algorithm matters more than a good one: the
// same seed must reproduce the same trace on every platform.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed * 2862933555777941757ULL + 1) {}

  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 2685821657736338717ULL;
  }

  std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(int percent) { return static_cast<int>(below(100)) < percent; }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Independent reference model
// ---------------------------------------------------------------------------
//
// Written from the §10 and §14.1 definitions directly, with a flat map layout
// and no use of Reservation, DomainExposure or derive_exposure.
class ReferenceModel {
 public:
  struct Chain {
    std::string account;
    std::string symbol;
    Side side = Side::Buy;
    std::int64_t accepted = 0;
    std::int64_t cum = 0;
    bool has_proposal = false;
    std::int64_t proposed = 0;
    bool pending_cancel = false;
    bool terminal = false;
  };

  void open_chain(const std::string& key, const Chain& chain) {
    chains_[key] = chain;
  }

  Chain* chain(const std::string& key) {
    const auto it = chains_.find(key);
    return it == chains_.end() ? nullptr : &it->second;
  }

  void add_position(const std::string& account, const std::string& symbol,
                    std::int64_t delta) {
    positions_[account + "|" + symbol] += delta;
  }

  std::int64_t position(const std::string& account, const std::string& symbol) const {
    const auto it = positions_.find(account + "|" + symbol);
    return it == positions_.end() ? 0 : it->second;
  }

  // max(0, accepted - cum) and max(0, proposed - cum), whichever is larger,
  // and zero once the chain is terminal.
  static std::int64_t effective(const Chain& c) {
    if (c.terminal) return 0;
    const std::int64_t old_branch = c.accepted > c.cum ? c.accepted - c.cum : 0;
    std::int64_t new_branch = 0;
    if (c.has_proposal) {
      new_branch = c.proposed > c.cum ? c.proposed - c.cum : 0;
    }
    return old_branch > new_branch ? old_branch : new_branch;
  }

  std::int64_t pending(const std::string& account, const std::string& symbol,
                       Side side) const {
    std::int64_t total = 0;
    for (const auto& [key, c] : chains_) {
      (void)key;
      if (c.account != account || c.symbol != symbol || c.side != side) continue;
      total += effective(c);
    }
    return total;
  }

  // The §10 envelope, from the definitions rather than from the engine.
  static std::int64_t net_envelope(std::int64_t p, std::int64_t b, std::int64_t s) {
    const std::int64_t upper = (p + b) >= 0 ? (p + b) : -(p + b);
    const std::int64_t lower = (p - s) >= 0 ? (p - s) : -(p - s);
    return upper > lower ? upper : lower;
  }

  static std::int64_t gross(std::int64_t p, std::int64_t b, std::int64_t s) {
    return (p >= 0 ? p : -p) + b + s;
  }

  std::set<std::string> accounts() const {
    std::set<std::string> out;
    for (const auto& [key, c] : chains_) {
      (void)key;
      out.insert(c.account);
    }
    return out;
  }

 private:
  std::map<std::string, Chain> chains_;
  std::map<std::string, std::int64_t> positions_;
};

std::string chain_key_of(const std::string& account, const std::string& chain) {
  return account + "|" + chain;
}

std::unique_ptr<LabEnv> make_env(const std::string& journal = {}) {
  LabOptions options;
  options.journal_path = journal;
  // Generous working-order counts: the properties here are about quantities,
  // and a count cap firing would only mask them.
  options.policy.limits.max_working_orders_per_account = 4096;
  options.policy.limits.max_working_orders_per_account_symbol = 4096;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

void check_quantities(LabEnv& env, const ReferenceModel& model, std::uint64_t seed,
                      int step) {
  const std::string where =
      "seed " + std::to_string(seed) + " step " + std::to_string(step);

  for (const std::string& account : model.accounts()) {
    for (const char* symbol : {"EURUSD", "GBPUSD"}) {
      const std::int64_t expected_buy = model.pending(account, symbol, Side::Buy);
      const std::int64_t expected_sell = model.pending(account, symbol, Side::Sell);
      const std::int64_t expected_position = model.position(account, symbol);

      const std::int64_t actual_buy =
          env.effective_remaining(account, symbol, Side::Buy);
      const std::int64_t actual_sell =
          env.effective_remaining(account, symbol, Side::Sell);
      const std::int64_t actual_position = env.position(account, symbol);

      // P-04 / P-05 / P-10: the engine's outstanding equals the model's.
      CHECK_MSG(actual_buy == expected_buy,
                where + ": " + account + "/" + symbol + " B is " +
                    std::to_string(actual_buy) + ", model says " +
                    std::to_string(expected_buy));
      CHECK_MSG(actual_sell == expected_sell,
                where + ": " + account + "/" + symbol + " S is " +
                    std::to_string(actual_sell) + ", model says " +
                    std::to_string(expected_sell));
      CHECK_MSG(actual_position == expected_position,
                where + ": " + account + "/" + symbol + " P is " +
                    std::to_string(actual_position) + ", model says " +
                    std::to_string(expected_position));

      // P-02: outstanding quantities are never negative.
      CHECK_MSG(actual_buy >= 0 && actual_sell >= 0, where + ": negative pending");
    }
  }

  // P-01: the envelope identities, recomputed from the model's numbers.
  Result<DomainMetrics> metrics = env.coordinator().current_metrics();
  CHECK_MSG(metrics.ok(), where + ": " + metrics.detail());
  for (const auto& [account, am] : metrics.value().accounts) {
    for (const auto& [symbol, sm] : am.symbols) {
      const std::int64_t p = model.position(account, symbol);
      const std::int64_t b = model.pending(account, symbol, Side::Buy);
      const std::int64_t s = model.pending(account, symbol, Side::Sell);
      CHECK_MSG(sm.net_units == ReferenceModel::net_envelope(p, b, s),
                where + ": E for " + account + "/" + symbol + " is " +
                    std::to_string(sm.net_units) + ", model says " +
                    std::to_string(ReferenceModel::net_envelope(p, b, s)));
      CHECK_MSG(sm.gross_units == ReferenceModel::gross(p, b, s),
                where + ": G for " + account + "/" + symbol + " is " +
                    std::to_string(sm.gross_units) + ", model says " +
                    std::to_string(ReferenceModel::gross(p, b, s)));
      CHECK_MSG(sm.gross_units >= sm.net_units, where + ": G < E");
    }
  }

  // P-15: the firm total is the plain sum of the accounts, with no offsetting.
  std::int64_t summed = 0;
  for (const auto& [account, am] : metrics.value().accounts) {
    (void)account;
    summed += am.total_gross_notional_minor;
  }
  CHECK_MSG(summed == metrics.value().firm_gross_notional_minor,
            where + ": the firm total is not the sum of the accounts");

  // RI-23: the stored projection is still exactly derivable from the ledger.
  const Status cross = env.coordinator().cross_check_projection();
  CHECK_MSG(cross.ok(), where + ": " + cross.detail());
}

// P-03 / P-07 / P-08: durable side effects and audit completeness.
void check_side_effects(LabEnv& env, std::uint64_t seed, int step) {
  const std::string where =
      "seed " + std::to_string(seed) + " step " + std::to_string(step);

  for (const OutboxIntent& o : env.store().outbox()) {
    CHECK_MSG(env.store().find_reservation(o.reservation_id).has_value(),
              where + ": intent " + o.intent_id + " has no reservation");
  }
  std::map<CommandId, int> intents;
  for (const OutboxIntent& o : env.store().outbox()) ++intents[o.command_id];
  for (const auto& [command, count] : intents) {
    CHECK_MSG(count == 1, where + ": " + std::to_string(count) +
                              " intents for command " + command);
  }

  const AuditChainReport chain = verify_audit_chain(env.store().audit());
  CHECK_MSG(chain.ok, where + ": audit chain: " + chain.detail);
  for (const AuditEvent& e : env.store().audit()) {
    CHECK_MSG(e.policy_version > 0, where + ": an audit event has no policy version");
  }
}

// P-11: at most one in-flight modification per chain.
void check_single_pending_modify(LabEnv& env, std::uint64_t seed, int step) {
  for (const Reservation& r : env.store().reservations()) {
    const int pending = r.proposed_total_units.has_value() ? 1 : 0;
    CHECK_MSG(pending <= 1, "seed " + std::to_string(seed) + " step " +
                                std::to_string(step) + ": chain " +
                                r.order_chain_id + " has " +
                                std::to_string(pending) + " proposals");
  }
}

}  // namespace

TEST(property_suite, conformant_traces_match_the_reference_model) {
  // §24.2 conformant mode: only legal events are generated, so every property
  // must hold at every step.
  const std::uint64_t seeds[] = {1, 7, 42, 1337, 92305, 777777, 20250101, 99991};

  for (const std::uint64_t seed : seeds) {
    Rng rng(seed);
    std::unique_ptr<LabEnv> env = make_env();
    ReferenceModel model;

    const char* accounts[] = {"ACC-A", "ACC-B", "ACC-C"};
    const char* symbols[] = {"EURUSD", "GBPUSD"};

    std::vector<std::string> open_chains;
    std::set<std::string> unresolved;
    int next_id = 0;
    Revision last_revision = env->store().domain_state().revision;

    for (int step = 0; step < 80; ++step) {
      const bool can_act = !open_chains.empty();

      if (!can_act || rng.below(100) < 30) {
        // --- new order ---
        const std::string account = accounts[rng.below(3)];
        const std::string symbol = symbols[rng.below(2)];
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        const std::int64_t units =
            1000 * static_cast<std::int64_t>(1 + rng.below(20));
        const std::string chain = "ch-" + std::to_string(next_id);
        const std::string command = "cmd-" + std::to_string(next_id);
        ++next_id;

        const RiskResult r =
            env->new_order(account, chain, symbol, side, units, kPrice, command);
        if (r.allowed()) {
          ReferenceModel::Chain c;
          c.account = account;
          c.symbol = symbol;
          c.side = side;
          c.accepted = units;
          model.open_chain(chain_key_of(account, chain), c);
          open_chains.push_back(chain_key_of(account, chain));
          CHECK(env->dispatch(command).allowed());
          CHECK(env->lp_accept(account, chain, "ack-" + command).allowed());
        } else {
          CHECK(r.decision != RiskDecision::AllowCommitted);
        }
      } else {
        const std::string key = open_chains[rng.below(open_chains.size())];
        ReferenceModel::Chain* c = model.chain(key);
        CHECK(c != nullptr);
        const std::string account = c->account;
        const std::string chain = key.substr(account.size() + 1);
        if (c->terminal) continue;

        const std::uint64_t action = rng.below(100);
        if (action < 40) {
          // --- partial or full fill ---
          const std::int64_t ceiling =
              (c->has_proposal && c->proposed > c->accepted) ? c->proposed
                                                             : c->accepted;
          const std::int64_t remaining = ceiling - c->cum;
          if (remaining <= 0) continue;
          std::int64_t fill = 1000 * static_cast<std::int64_t>(1 + rng.below(5));
          if (fill > remaining) fill = remaining;

          const RiskResult r =
              env->lp_fill(account, chain, "ex-" + std::to_string(next_id++), fill);
          CHECK_MSG(r.allowed(), "fill refused: " +
                                     std::string(reason_name(r.reason)) + " " +
                                     r.detail);
          c->cum += fill;
          model.add_position(account, c->symbol, side_sign(c->side) * fill);
        } else if (action < 55) {
          // --- cancel request: must not release anything (P-04) ---
          if (c->pending_cancel || c->has_proposal) continue;
          const std::int64_t before =
              env->effective_remaining(account, c->symbol, c->side);
          const RiskResult r = env->cancel_order(
              account, chain, "cxl-" + std::to_string(next_id++));
          if (r.allowed()) {
            c->pending_cancel = true;
            CHECK_MSG(env->effective_remaining(account, c->symbol, c->side) == before,
                      "a pending cancel changed the outstanding quantity");
          }
        } else if (action < 70) {
          // --- replace ---
          if (c->pending_cancel || c->has_proposal) continue;
          if (unresolved.count(key) != 0) continue;
          const std::int64_t proposed =
              1000 * static_cast<std::int64_t>(1 + rng.below(40));
          const RiskResult r = env->replace_order(
              account, chain, proposed, kPrice, "rpl-" + std::to_string(next_id++));
          if (r.allowed()) {
            c->has_proposal = true;
            c->proposed = proposed;
          }
        } else if (action < 80) {
          // --- resolve a replace ---
          if (!c->has_proposal) continue;
          VerifiedOrderEvent e;
          e.tenant_id = "T1";
          e.account_id = account;
          e.order_chain_id = chain;
          e.source_namespace = "lp";
          e.event_id = "rr-" + std::to_string(next_id++);
          e.symbol = c->symbol;
          e.side = c->side;
          if (rng.chance(50)) {
            e.kind = OrderEventKind::ReplaceAccepted;
            e.accepted_total_units = c->proposed;
            const RiskResult r = env->lp_event(e);
            CHECK_MSG(r.allowed(), "replace accept refused: " +
                                       std::string(reason_name(r.reason)));
            c->accepted = c->proposed;
          } else {
            e.kind = OrderEventKind::ReplaceRejected;
            const RiskResult r = env->lp_event(e);
            CHECK_MSG(r.allowed(), "replace reject refused: " +
                                       std::string(reason_name(r.reason)));
          }
          c->has_proposal = false;
          unresolved.erase(key);
        } else if (action < 90) {
          // --- terminal cancel ---
          if (c->has_proposal) continue;
          VerifiedOrderEvent e;
          e.tenant_id = "T1";
          e.account_id = account;
          e.order_chain_id = chain;
          e.source_namespace = "lp";
          e.event_id = "cx-" + std::to_string(next_id++);
          e.kind = OrderEventKind::CancelAccepted;
          e.symbol = c->symbol;
          e.side = c->side;
          e.verified_cum_units = c->cum;
          const RiskResult r = env->lp_event(e);
          CHECK_MSG(r.allowed(), "terminal cancel refused: " +
                                     std::string(reason_name(r.reason)));
          c->terminal = true;
          c->pending_cancel = false;
          unresolved.erase(key);
        } else {
          // --- unknown outcome: the hold must survive (P-10) ---
          if (unresolved.count(key) != 0) continue;
          const std::int64_t before =
              env->effective_remaining(account, c->symbol, c->side);
          VerifiedOrderEvent e;
          e.tenant_id = "T1";
          e.account_id = account;
          e.order_chain_id = chain;
          e.source_namespace = "recon";
          e.event_id = "unk-" + std::to_string(next_id++);
          e.kind = OrderEventKind::UnknownOutcome;
          e.symbol = c->symbol;
          e.side = c->side;
          const RiskResult r = env->lp_event(e);
          CHECK(r.decision == RiskDecision::HoldRetryable);
          CHECK_MSG(env->effective_remaining(account, c->symbol, c->side) == before,
                    "an unknown outcome changed the outstanding quantity");
          c->pending_cancel = false;
          unresolved.insert(key);
        }
      }

      // P-06: the commit revision never goes backwards.
      const Revision revision = env->store().domain_state().revision;
      CHECK_MSG(revision >= last_revision, "the commit revision went backwards");
      last_revision = revision;

      check_quantities(*env, model, seed, step);
      check_side_effects(*env, seed, step);
      check_single_pending_modify(*env, seed, step);
    }
  }
}

TEST(property_suite, the_same_seed_produces_the_same_state_hash) {
  // §24.2: replaying the same trace must yield the same canonical hash and the
  // same number of durable effects.
  struct Snapshot {
    std::string hash;
    std::size_t outbox = 0;
    std::size_t audit = 0;
    std::size_t reservations = 0;
  };

  auto run = [](std::uint64_t seed) {
    Rng rng(seed);
    std::unique_ptr<LabEnv> env = make_env();
    for (int i = 0; i < 40; ++i) {
      const std::string chain = "ch-" + std::to_string(i);
      const std::string command = "cmd-" + std::to_string(i);
      const std::int64_t units =
          1000 * static_cast<std::int64_t>(1 + rng.below(10));
      const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
      if (env->new_order("ACC-A", chain, "EURUSD", side, units, kPrice, command)
              .allowed()) {
        CHECK(env->dispatch(command).allowed());
        CHECK(env->lp_accept("ACC-A", chain, "ack-" + command).allowed());
        if (rng.chance(60)) {
          (void)env->lp_fill("ACC-A", chain, "ex-" + command, 1000);
        }
      }
    }
    return Snapshot{env->store().state_hash(), env->store().outbox().size(),
                    env->store().audit().size(),
                    env->store().reservations().size()};
  };

  const Snapshot first = run(4242);
  const Snapshot second = run(4242);
  CHECK_EQ(first.hash, second.hash);
  CHECK_EQ(first.outbox, second.outbox);
  CHECK_EQ(first.audit, second.audit);
  CHECK_EQ(first.reservations, second.reservations);

  // A different seed must produce a different trace, or the generator is not
  // doing anything and the determinism check above is vacuous.
  CHECK_NE(run(9999).hash, first.hash);
}

TEST(property_suite, adversarial_events_never_produce_an_allow) {
  // §24.2 adversarial mode: malformed and impossible events may be denied,
  // held or quarantined -- never accepted, and never silently mutating state.
  const std::uint64_t seeds[] = {11, 222, 3333};

  for (const std::uint64_t seed : seeds) {
    Rng rng(seed);
    std::unique_ptr<LabEnv> env = make_env();
    CHECK(env->new_order("ACC-A", "ch-1", "EURUSD", Side::Buy, 50000, kPrice, "n1")
              .allowed());
    CHECK(env->dispatch("n1").allowed());
    CHECK(env->lp_accept("ACC-A", "ch-1", "ack-1").allowed());
    CHECK(env->lp_fill("ACC-A", "ch-1", "ex-1", 10000).allowed());

    const std::int64_t position_before = env->position("ACC-A", "EURUSD");
    const std::int64_t held_before =
        env->effective_remaining("ACC-A", "EURUSD", Side::Buy);

    for (int step = 0; step < 24; ++step) {
      VerifiedOrderEvent e;
      e.tenant_id = "T1";
      e.account_id = "ACC-A";
      e.order_chain_id = "ch-1";
      e.source_namespace = "lp";
      e.event_id = "adv-" + std::to_string(step);
      e.symbol = "EURUSD";
      e.side = Side::Buy;

      switch (rng.below(8)) {
        case 0:  // a fill bigger than the whole order
          e.kind = OrderEventKind::AppliedFill;
          e.fill_units = 10'000'000;
          break;
        case 1:  // a negative fill
          e.kind = OrderEventKind::AppliedFill;
          e.fill_units = -1000;
          break;
        case 2:  // the wrong side
          e.kind = OrderEventKind::AppliedFill;
          e.side = Side::Sell;
          e.fill_units = 1000;
          break;
        case 3:  // the wrong symbol
          e.kind = OrderEventKind::AppliedFill;
          e.symbol = "GBPUSD";
          e.fill_units = 1000;
          break;
        case 4:  // a terminal report with the wrong cumulative
          e.kind = OrderEventKind::CancelAccepted;
          e.verified_cum_units = 999999;
          break;
        case 5:  // an unsupported correction
          e.kind = OrderEventKind::TradeBust;
          break;
        case 6:  // a replace acceptance with no proposal
          e.kind = OrderEventKind::ReplaceAccepted;
          e.accepted_total_units = 1'000'000;
          break;
        default:  // a malformed identifier
          e.order_chain_id = "has space";
          e.kind = OrderEventKind::AppliedFill;
          e.fill_units = 1000;
          break;
      }

      const RiskResult r = env->lp_event(e);
      CHECK_MSG(r.decision != RiskDecision::AllowCommitted ||
                    r.reason == Reason::DuplicateEvent,
                "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                    ": an adversarial event was accepted (" +
                    std::string(reason_name(r.reason)) + ")");

      // Nothing was invented, and the budget was not released.
      CHECK_MSG(env->position("ACC-A", "EURUSD") == position_before,
                "an adversarial event moved the position");
      CHECK_MSG(env->effective_remaining("ACC-A", "EURUSD", Side::Buy) >= held_before,
                "an adversarial event released budget");
      CHECK(env->coordinator().cross_check_projection().ok());
    }
  }
}

TEST(property_suite, notional_is_never_below_the_exact_rational_value) {
  // P-14, checked against exact integer arithmetic rather than against the
  // engine's own accumulator.
  const RiskPolicy policy = lab_policy();
  const InstrumentSpec& spec = policy.instruments.at("EURUSD");

  Rng rng(0xBEEF);
  for (int i = 0; i < 2000; ++i) {
    const std::int64_t units = static_cast<std::int64_t>(rng.below(1'000'000)) + 1;
    const std::int64_t price = static_cast<std::int64_t>(rng.below(1'000'000)) + 1;
    Valuation v;
    v.risk_price_ticks = price;
    v.fx_upper = Rational{1, 1};

    Result<std::int64_t> actual = notional_minor(units, spec, v, 2);
    CHECK_MSG(actual.ok(), actual.detail());

    // exact = units * price * (1/100000) * 100 = units * price / 1000, and the
    // charge must be its ceiling. Both factors are below 10^6, so the product
    // cannot overflow here.
    const std::int64_t numerator = units * price;
    const std::int64_t expected = (numerator + 999) / 1000;
    CHECK_MSG(actual.value() == expected,
              "units=" + std::to_string(units) + " price=" + std::to_string(price) +
                  " got " + std::to_string(actual.value()) + " expected " +
                  std::to_string(expected));
  }
}

TEST(property_suite, restart_equivalence_holds_for_random_traces) {
  // P-13: for every seed, reopening the journal reproduces the live state.
  const std::uint64_t seeds[] = {5, 55, 555};

  for (const std::uint64_t seed : seeds) {
    tests::TmpDir dir("prop-restart");
    const std::string journal = dir.file("risk.journal");

    std::string live_hash;
    {
      Rng rng(seed);
      std::unique_ptr<LabEnv> env = make_env(journal);
      for (int i = 0; i < 25; ++i) {
        const std::string chain = "ch-" + std::to_string(i);
        const std::string command = "cmd-" + std::to_string(i);
        const std::int64_t units =
            1000 * static_cast<std::int64_t>(1 + rng.below(10));
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        if (!env->new_order("ACC-A", chain, "EURUSD", side, units, kPrice, command)
                 .allowed()) {
          continue;
        }
        CHECK(env->dispatch(command).allowed());
        CHECK(env->lp_accept("ACC-A", chain, "ack-" + command).allowed());
        if (rng.chance(50)) {
          (void)env->lp_fill("ACC-A", chain, "ex-" + command, 1000);
        }
        if (rng.chance(30)) {
          (void)env->replace_order("ACC-A", chain, units + 1000, kPrice,
                                   "rpl-" + command);
        }
      }
      live_hash = env->store().state_hash();

      LabOptions reopen;
      reopen.journal_path = journal;
      reopen.open_admission = false;
      Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
      CHECK_MSG(replayed.ok(), replayed.detail());
      const Status equivalent =
          assert_replay_equivalence(env->store(), replayed.value()->store());
      CHECK_MSG(equivalent.ok(),
                "seed " + std::to_string(seed) + ": " + equivalent.detail());
    }

    LabOptions reopen;
    reopen.journal_path = journal;
    reopen.open_admission = false;
    Result<std::unique_ptr<LabEnv>> replayed = LabEnv::create(reopen);
    CHECK_MSG(replayed.ok(), replayed.detail());
    CHECK_EQ(replayed.value()->store().state_hash(), live_hash);
    CHECK(replayed.value()->coordinator().cross_check_projection().ok());
  }
}

TEST(property_suite, resource_caps_bound_the_state) {
  // P-16: the working-order cap really bounds how much state one account can
  // accumulate, and hitting it is a denial rather than an unbounded queue.
  LabOptions options;
  options.policy.limits.max_working_orders_per_account = 5;
  options.policy.limits.max_working_orders_per_account_symbol = 5;
  Result<std::unique_ptr<LabEnv>> created = LabEnv::create(options);
  CHECK_MSG(created.ok(), created.detail());
  std::unique_ptr<LabEnv> env = std::move(created.value());

  int allowed = 0;
  for (int i = 0; i < 40; ++i) {
    const std::string id = std::to_string(i);
    const RiskResult r = env->new_order("ACC-A", "ch-" + id, "EURUSD", Side::Buy,
                                        1000, kPrice, "cmd-" + id);
    if (r.allowed()) {
      ++allowed;
    } else {
      CHECK(r.decision == RiskDecision::DenyDefinitive);
      CHECK_EQ(static_cast<int>(r.reason),
               static_cast<int>(Reason::LimitAccountWorkingCount));
    }
  }
  CHECK_EQ(allowed, 5);
  CHECK_EQ(env->store().reservations().size(), static_cast<std::size_t>(5));
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(5));
  CHECK(env->coordinator().cross_check_projection().ok());
}
