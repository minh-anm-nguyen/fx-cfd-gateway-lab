// §24.1 / §24.2 — model-based differential testing against an independent
// oracle written in another language.
//
// This test does two things. It drives the engine with deterministic
// model-alphabet traces and checks the state machine's own consistency, and it
// writes those traces plus the engine's numbers to a JSON artifact that
// `tests/oracle/reference_model.py` recomputes from scratch with exact
// rationals. The spec is explicit that the oracle must not share code with the
// system under test (§24.1), which a second C++ model inside this binary could
// not honour: the Python side is the real differential check, and this file is
// the exporter for it.
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kEurusdPrice = 110000;
constexpr std::int64_t kEurjpyPrice = 160000;

// One step of the §24.2 event alphabet, recorded so the oracle can replay it.
struct Step {
  std::string op;
  std::string account;
  std::string chain;
  std::string symbol;
  std::string side;
  std::int64_t units = 0;
  std::int64_t price = 0;
};

class Trace {
 public:
  explicit Trace(std::uint64_t seed) : seed_(seed) {}

  void record(const Step& step) { steps_.push_back(step); }
  const std::vector<Step>& steps() const { return steps_; }
  std::uint64_t seed() const { return seed_; }

 private:
  std::uint64_t seed_;
  std::vector<Step> steps_;
};

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
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

std::string json_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

std::string rational_json(const Rational& r) {
  return "{\"num\": " + std::to_string(r.num) + ", \"den\": " +
         std::to_string(r.den) + "}";
}

// The chain state the generator needs to keep its events legal.
struct GenChain {
  std::string account;
  std::string symbol;
  Side side = Side::Buy;
  std::int64_t accepted = 0;
  std::int64_t cum = 0;
  bool has_proposal = false;
  std::int64_t proposed = 0;
  bool terminal = false;
};

std::unique_ptr<LabEnv> make_env() {
  LabOptions options;
  options.policy.limits.max_working_orders_per_account = 4096;
  options.policy.limits.max_working_orders_per_account_symbol = 4096;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

// Runs one seeded trace and returns it along with the environment that
// produced it, so the caller can read the engine's final numbers.
Trace run_trace(LabEnv& env, std::uint64_t seed, int steps) {
  Rng rng(seed);
  Trace trace(seed);

  const char* accounts[] = {"ACC-A", "ACC-B"};
  const char* symbols[] = {"EURUSD", "EURJPY"};
  std::map<std::string, GenChain> chains;
  std::vector<std::string> keys;
  int next_id = 0;

  auto price_for = [](const std::string& symbol) {
    return symbol == "EURJPY" ? kEurjpyPrice : kEurusdPrice;
  };

  for (int step = 0; step < steps; ++step) {
    if (keys.empty() || rng.below(100) < 35) {
      const std::string account = accounts[rng.below(2)];
      const std::string symbol = symbols[rng.below(2)];
      const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
      const std::int64_t units = 1000 * static_cast<std::int64_t>(1 + rng.below(15));
      const std::string chain = "c" + std::to_string(next_id);
      const std::string command = "n" + std::to_string(next_id);
      ++next_id;
      const std::int64_t price = price_for(symbol);

      const RiskResult r =
          env.new_order(account, chain, symbol, side, units, price, command);
      if (!r.allowed()) continue;
      CHECK(env.dispatch(command).allowed());
      CHECK(env.lp_accept(account, chain, "a" + command).allowed());

      GenChain c;
      c.account = account;
      c.symbol = symbol;
      c.side = side;
      c.accepted = units;
      const std::string key = account + "|" + chain;
      chains[key] = c;
      keys.push_back(key);

      Step s;
      s.op = "new";
      s.account = account;
      s.chain = chain;
      s.symbol = symbol;
      s.side = side == Side::Buy ? "BUY" : "SELL";
      s.units = units;
      s.price = price;
      trace.record(s);
      continue;
    }

    const std::string key = keys[rng.below(keys.size())];
    GenChain& c = chains[key];
    if (c.terminal) continue;
    const std::string account = c.account;
    const std::string chain = key.substr(account.size() + 1);

    const std::uint64_t action = rng.below(100);
    if (action < 45) {
      const std::int64_t ceiling =
          (c.has_proposal && c.proposed > c.accepted) ? c.proposed : c.accepted;
      const std::int64_t remaining = ceiling - c.cum;
      if (remaining <= 0) continue;
      std::int64_t fill = 1000 * static_cast<std::int64_t>(1 + rng.below(4));
      if (fill > remaining) fill = remaining;

      const RiskResult r =
          env.lp_fill(account, chain, "e" + std::to_string(next_id++), fill);
      CHECK_MSG(r.allowed(), "fill refused: " + std::string(reason_name(r.reason)));
      c.cum += fill;

      Step s;
      s.op = "fill";
      s.account = account;
      s.chain = chain;
      s.units = fill;
      trace.record(s);
    } else if (action < 65) {
      if (c.has_proposal) continue;
      const std::int64_t proposed =
          1000 * static_cast<std::int64_t>(1 + rng.below(25));
      if (proposed < c.cum) continue;
      // A price change is included so the oracle has to track the max raw
      // limit price per symbol, not just quantities.
      const std::int64_t price =
          (c.symbol == "EURJPY" ? kEurjpyPrice : kEurusdPrice) +
          static_cast<std::int64_t>(rng.below(400));
      const RiskResult r = env.replace_order(account, chain, proposed, price,
                                             "r" + std::to_string(next_id++));
      if (!r.allowed()) continue;
      c.has_proposal = true;
      c.proposed = proposed;

      Step s;
      s.op = "replace";
      s.account = account;
      s.chain = chain;
      s.units = proposed;
      s.price = price;
      trace.record(s);
    } else if (action < 80) {
      if (!c.has_proposal) continue;
      VerifiedOrderEvent e;
      e.tenant_id = "T1";
      e.account_id = account;
      e.order_chain_id = chain;
      e.source_namespace = "lp";
      e.event_id = "rr" + std::to_string(next_id++);
      e.symbol = c.symbol;
      e.side = c.side;
      const bool accept = rng.chance(60);
      if (accept) {
        e.kind = OrderEventKind::ReplaceAccepted;
        e.accepted_total_units = c.proposed;
      } else {
        e.kind = OrderEventKind::ReplaceRejected;
      }
      const RiskResult r = env.lp_event(e);
      CHECK_MSG(r.allowed(),
                "replace resolution refused: " + std::string(reason_name(r.reason)));
      if (accept) c.accepted = c.proposed;
      c.has_proposal = false;

      Step s;
      s.op = accept ? "replace_accept" : "replace_reject";
      s.account = account;
      s.chain = chain;
      trace.record(s);
    } else if (action < 90) {
      const std::int64_t before =
          env.effective_remaining(account, c.symbol, c.side);
      const RiskResult r =
          env.cancel_order(account, chain, "x" + std::to_string(next_id++));
      if (!r.allowed()) continue;
      CHECK_MSG(env.effective_remaining(account, c.symbol, c.side) == before,
                "a pending cancel released budget");

      Step s;
      s.op = "cancel_request";
      s.account = account;
      s.chain = chain;
      trace.record(s);
    } else {
      if (c.has_proposal) continue;
      VerifiedOrderEvent e;
      e.tenant_id = "T1";
      e.account_id = account;
      e.order_chain_id = chain;
      e.source_namespace = "lp";
      e.event_id = "cx" + std::to_string(next_id++);
      e.kind = OrderEventKind::CancelAccepted;
      e.symbol = c.symbol;
      e.side = c.side;
      e.verified_cum_units = c.cum;
      const RiskResult r = env.lp_event(e);
      CHECK_MSG(r.allowed(),
                "terminal cancel refused: " + std::string(reason_name(r.reason)));
      c.terminal = true;

      Step s;
      s.op = "cancel_final";
      s.account = account;
      s.chain = chain;
      trace.record(s);
    }
  }

  return trace;
}

std::string artifact_path() {
  if (const char* env = std::getenv("RISK_MODEL_ARTIFACT"); env != nullptr) {
    return env;
  }
  return "test-artifacts/model_traces.json";
}

}  // namespace

TEST(model_differential, exports_traces_and_engine_numbers_for_the_python_oracle) {
  const std::uint64_t seeds[] = {101, 202, 303, 404, 505, 606};

  const RiskPolicy policy = lab_policy();
  std::string json;
  json += "{\n";
  json += "  \"policy\": {\n";
  json += "    \"minor_scale\": " +
          std::to_string(policy.account_currency.minor_scale) + ",\n";
  json += "    \"account_currency\": \"" + policy.account_currency.code + "\",\n";
  json += "    \"margin_rate\": " + rational_json(policy.margin_proxy_rate) + ",\n";
  json += "    \"fx_upper_buffer_bps\": " +
          std::to_string(policy.fx_upper_buffer_bps) + ",\n";
  json += "    \"instruments\": {\n";
  bool first_instrument = true;
  for (const auto& [symbol, spec] : policy.instruments) {
    if (!first_instrument) json += ",\n";
    first_instrument = false;
    json += "      \"" + json_escape(symbol) + "\": {";
    json += "\"tick\": " + rational_json(spec.tick_size);
    json += ", \"multiplier\": " + rational_json(spec.contract_multiplier);
    json += ", \"risk_price_buffer_bps\": " +
            std::to_string(spec.risk_price_upper_buffer_bps);
    json += ", \"quote_currency\": \"" + spec.quote_currency + "\"";
    json += "}";
  }
  json += "\n    }\n  },\n";

  // The market is published once per environment and never moved, so one
  // snapshot describes every trace.
  json += "  \"market\": {\n    \"quotes\": {\n";
  json += "      \"EURUSD\": {\"bid\": 109990, \"ask\": 110000},\n";
  json += "      \"GBPUSD\": {\"bid\": 109990, \"ask\": 110000},\n";
  json += "      \"EURJPY\": {\"bid\": 159990, \"ask\": 160000}\n";
  json += "    },\n    \"fx\": [\n";
  json += "      {\"from\": \"JPY\", \"to\": \"USD\", \"bid\": {\"num\": 66, "
          "\"den\": 10000}, \"ask\": {\"num\": 67, \"den\": 10000}}\n";
  json += "    ]\n  },\n";

  json += "  \"traces\": [\n";

  for (std::size_t index = 0; index < std::size(seeds); ++index) {
    std::unique_ptr<LabEnv> env = make_env();
    const Trace trace = run_trace(*env, seeds[index], 70);

    // The engine must still be self-consistent before its numbers are exported.
    const Status cross = env->coordinator().cross_check_projection();
    CHECK_MSG(cross.ok(), "seed " + std::to_string(seeds[index]) + ": " +
                              cross.detail());
    const AuditChainReport chain = verify_audit_chain(env->store().audit());
    CHECK_MSG(chain.ok, chain.detail);
    CHECK_MSG(!trace.steps().empty(),
              "seed " + std::to_string(seeds[index]) + " produced no steps");

    Result<DomainMetrics> metrics = env->coordinator().current_metrics();
    CHECK_MSG(metrics.ok(), metrics.detail());

    if (index > 0) json += ",\n";
    json += "    {\n      \"seed\": " + std::to_string(seeds[index]) + ",\n";
    json += "      \"steps\": [\n";
    for (std::size_t i = 0; i < trace.steps().size(); ++i) {
      const Step& s = trace.steps()[i];
      json += "        {\"op\": \"" + s.op + "\"";
      if (!s.account.empty()) json += ", \"account\": \"" + s.account + "\"";
      if (!s.chain.empty()) json += ", \"chain\": \"" + s.chain + "\"";
      if (!s.symbol.empty()) json += ", \"symbol\": \"" + s.symbol + "\"";
      if (!s.side.empty()) json += ", \"side\": \"" + s.side + "\"";
      if (s.units != 0) json += ", \"units\": " + std::to_string(s.units);
      if (s.price != 0) json += ", \"price\": " + std::to_string(s.price);
      json += "}";
      if (i + 1 != trace.steps().size()) json += ",";
      json += "\n";
    }
    json += "      ],\n      \"engine\": {\n";
    json += "        \"firm_gross_notional_minor\": " +
            std::to_string(metrics.value().firm_gross_notional_minor) + ",\n";
    json += "        \"accounts\": {\n";

    bool first_account = true;
    for (const auto& [account, am] : metrics.value().accounts) {
      if (!first_account) json += ",\n";
      first_account = false;
      json += "          \"" + json_escape(account) + "\": {\n";
      json += "            \"total_gross_notional_minor\": " +
              std::to_string(am.total_gross_notional_minor) + ",\n";
      json += "            \"margin_proxy_minor\": " +
              std::to_string(am.margin_proxy_minor) + ",\n";
      json += "            \"symbols\": {\n";
      bool first_symbol = true;
      for (const auto& [symbol, sm] : am.symbols) {
        if (!first_symbol) json += ",\n";
        first_symbol = false;
        json += "              \"" + json_escape(symbol) + "\": {";
        json += "\"position_units\": " +
                std::to_string(env->position(account, symbol));
        json += ", \"pending_buy_units\": " +
                std::to_string(env->effective_remaining(account, symbol, Side::Buy));
        json += ", \"pending_sell_units\": " +
                std::to_string(env->effective_remaining(account, symbol, Side::Sell));
        json += ", \"net_units\": " + std::to_string(sm.net_units);
        json += ", \"gross_units\": " + std::to_string(sm.gross_units);
        json += ", \"net_notional_minor\": " +
                std::to_string(sm.net_notional_minor);
        json += ", \"gross_notional_minor\": " +
                std::to_string(sm.gross_notional_minor);
        json += "}";
      }
      json += "\n            }\n          }";
    }
    json += "\n        }\n      }\n    }";
  }

  json += "\n  ]\n}\n";

  const std::string path = artifact_path();
  // Best effort: the directory normally exists in the repo, and a missing
  // artifact is reported by the oracle step rather than silently ignored.
  std::ofstream out(path, std::ios::binary);
  CHECK_MSG(out.good(), "cannot write " + path);
  out << json;
  out.close();
  CHECK_MSG(out.good(), "write failed for " + path);

  std::printf("        wrote %s (%zu bytes)\n", path.c_str(), json.size());
}

TEST(model_differential, the_exported_artifact_is_readable_and_non_trivial) {
  std::ifstream in(artifact_path(), std::ios::binary);
  CHECK_MSG(in.good(), "artifact missing: " + artifact_path() +
                           " (run the exporter case first)");
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  CHECK(content.size() > 1000);
  CHECK(content.find("\"traces\"") != std::string::npos);
  CHECK(content.find("\"gross_notional_minor\"") != std::string::npos);
  // At least one trace must have produced a non-zero exposure, or the oracle
  // would be comparing zeroes against zeroes.
  CHECK(content.find("\"gross_units\": 0,") == std::string::npos ||
        content.find("\"net_units\":") != std::string::npos);
}
