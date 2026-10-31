#include "risk/config.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include "risk/checked.hpp"

namespace risk {
namespace {

struct Line {
  int indent = 0;
  std::string key;
  std::string value;   // empty when the line opens a block
  bool has_value = false;
  int number = 0;
};

std::string trim(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \r\n");
  if (begin == std::string::npos) return {};
  const std::size_t end = s.find_last_not_of(" \r\n");
  return s.substr(begin, end - begin + 1);
}

// Strips a trailing comment that is not inside quotes.
std::string strip_comment(const std::string& s) {
  bool in_quotes = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '"') in_quotes = !in_quotes;
    if (s[i] == '#' && !in_quotes) return s.substr(0, i);
  }
  return s;
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

Result<std::vector<std::string>> parse_inline_list(const std::string& raw, int line) {
  std::vector<std::string> out;
  if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
    return Result<std::vector<std::string>>::fail(
        Reason::PolicyInvalid, "line " + std::to_string(line) + ": bad list syntax");
  }
  const std::string body = trim(raw.substr(1, raw.size() - 2));
  if (body.empty()) return Result<std::vector<std::string>>(out);

  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t comma = body.find(',', start);
    const std::string item =
        trim(body.substr(start, comma == std::string::npos ? std::string::npos
                                                           : comma - start));
    if (item.empty()) {
      return Result<std::vector<std::string>>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(line) + ": empty list element");
    }
    out.push_back(unquote(item));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return Result<std::vector<std::string>>(out);
}

bool parse_i64(const std::string& text, std::int64_t& out) {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
  }
  if (i >= text.size()) return false;
  std::int64_t acc = 0;
  for (; i < text.size(); ++i) {
    // Underscores are rejected: "1_000" would otherwise parse as 1.
    if (text[i] < '0' || text[i] > '9') return false;
    if (!mul_i64(acc, 10, acc)) return false;
    if (!add_i64(acc, static_cast<std::int64_t>(text[i] - '0'), acc)) return false;
  }
  out = negative ? -acc : acc;
  return true;
}

bool parse_bool(const std::string& text, bool& out) {
  if (text == "true") {
    out = true;
    return true;
  }
  if (text == "false") {
    out = false;
    return true;
  }
  return false;
}

// Recursive descent over a pre-tokenised line list.
Result<YamlNode> build_block(const std::vector<Line>& lines, std::size_t& index,
                             int indent) {
  YamlNode node;
  node.kind = YamlNode::Kind::Map;
  if (index < lines.size()) node.line = lines[index].number;

  while (index < lines.size()) {
    const Line& line = lines[index];
    if (line.indent < indent) break;
    if (line.indent > indent) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(line.number) + ": unexpected indentation");
    }
    if (node.map.count(line.key) != 0) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(line.number) + ": duplicate key " + line.key);
    }

    if (line.has_value) {
      YamlNode leaf;
      leaf.line = line.number;
      if (!line.value.empty() && line.value.front() == '[') {
        Result<std::vector<std::string>> items =
            parse_inline_list(line.value, line.number);
        if (!items.ok()) return Result<YamlNode>::fail(items.reason(), items.detail());
        leaf.kind = YamlNode::Kind::List;
        leaf.list = items.value();
      } else {
        leaf.kind = YamlNode::Kind::Scalar;
        leaf.scalar = unquote(line.value);
      }
      node.map[line.key] = std::move(leaf);
      ++index;
      continue;
    }

    // Block opener: the child block must be indented further.
    const std::string key = line.key;
    const int opener_line = line.number;
    ++index;
    if (index >= lines.size() || lines[index].indent <= indent) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(opener_line) + ": " + key + " has no block");
    }
    Result<YamlNode> child = build_block(lines, index, lines[index].indent);
    if (!child.ok()) return child;
    node.map[key] = child.value();
  }

  return Result<YamlNode>(node);
}

Status require_known_keys(const YamlNode& node, const std::set<std::string>& allowed,
                          const std::string& where) {
  for (const auto& [key, child] : node.map) {
    (void)child;
    if (allowed.count(key) == 0) {
      return Status::fail(Reason::PolicyInvalid,
                          where + ": unknown key '" + key + "'");
    }
  }
  return Status::success();
}

Status read_i64(const YamlNode& node, const std::string& key, std::int64_t& out,
                const std::string& where, bool required) {
  const YamlNode* child = node.child(key);
  if (child == nullptr) {
    if (required) {
      return Status::fail(Reason::PolicyInvalid, where + ": missing " + key);
    }
    return Status::success();
  }
  if (child->kind != YamlNode::Kind::Scalar || !parse_i64(child->scalar, out)) {
    return Status::fail(Reason::PolicyInvalid,
                        where + ": " + key + " is not an integer");
  }
  return Status::success();
}

Status read_bool(const YamlNode& node, const std::string& key, bool& out,
                 const std::string& where) {
  const YamlNode* child = node.child(key);
  if (child == nullptr) return Status::success();
  if (child->kind != YamlNode::Kind::Scalar || !parse_bool(child->scalar, out)) {
    return Status::fail(Reason::PolicyInvalid,
                        where + ": " + key + " is not a boolean");
  }
  return Status::success();
}

Status read_string(const YamlNode& node, const std::string& key, std::string& out,
                   const std::string& where, bool required) {
  const YamlNode* child = node.child(key);
  if (child == nullptr) {
    if (required) {
      return Status::fail(Reason::PolicyInvalid, where + ": missing " + key);
    }
    return Status::success();
  }
  if (child->kind != YamlNode::Kind::Scalar) {
    return Status::fail(Reason::PolicyInvalid, where + ": " + key + " is not a scalar");
  }
  out = child->scalar;
  return Status::success();
}

Status read_rational(const YamlNode& node, const std::string& key, Rational& out,
                     const std::string& where, bool required) {
  std::string text;
  if (const Status s = read_string(node, key, text, where, required); !s) return s;
  if (text.empty()) return Status::success();
  if (!rational_parse(text, out)) {
    return Status::fail(Reason::PolicyInvalid,
                        where + ": " + key + " is not a rational (" + text + ")");
  }
  return Status::success();
}

}  // namespace

const YamlNode* YamlNode::child(const std::string& key) const {
  const auto it = map.find(key);
  return it == map.end() ? nullptr : &it->second;
}

Result<YamlNode> parse_yaml_subset(const std::string& text) {
  std::vector<Line> lines;
  std::istringstream stream(text);
  std::string raw;
  int number = 0;

  while (std::getline(stream, raw)) {
    ++number;
    if (raw.find('\t') != std::string::npos) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) + ": tabs are not allowed");
    }
    const std::string content = strip_comment(raw);
    if (trim(content).empty()) continue;
    if (trim(content) == "---" || trim(content) == "...") {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) + ": document markers are not supported");
    }
    if (trim(content).front() == '-') {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) +
              ": block sequences are not supported; use an inline [a, b] list");
    }
    if (content.find('&') != std::string::npos ||
        content.find('*') != std::string::npos) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) + ": anchors and aliases are not supported");
    }

    Line line;
    line.number = number;
    line.indent = static_cast<int>(content.find_first_not_of(' '));
    if (line.indent % 2 != 0) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) + ": indentation must be a multiple of 2");
    }

    const std::string body = trim(content);
    const std::size_t colon = body.find(':');
    if (colon == std::string::npos) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid,
          "line " + std::to_string(number) + ": expected 'key:' or 'key: value'");
    }
    line.key = trim(body.substr(0, colon));
    const std::string rest = trim(body.substr(colon + 1));
    if (line.key.empty()) {
      return Result<YamlNode>::fail(
          Reason::PolicyInvalid, "line " + std::to_string(number) + ": empty key");
    }
    if (!rest.empty()) {
      line.value = rest;
      line.has_value = true;
    }
    lines.push_back(std::move(line));
  }

  if (lines.empty()) {
    return Result<YamlNode>::fail(Reason::PolicyInvalid, "empty document");
  }
  if (lines.front().indent != 0) {
    return Result<YamlNode>::fail(Reason::PolicyInvalid,
                                  "document must start at indentation 0");
  }

  std::size_t index = 0;
  Result<YamlNode> root = build_block(lines, index, 0);
  if (!root.ok()) return root;
  if (index != lines.size()) {
    return Result<YamlNode>::fail(
        Reason::PolicyInvalid,
        "line " + std::to_string(lines[index].number) + ": unconsumed input");
  }
  return root;
}

Result<RiskPolicy> policy_from_yaml(const YamlNode& root) {
  if (root.kind != YamlNode::Kind::Map) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid, "root is not a mapping");
  }

  static const std::set<std::string> kTopLevel = {
      "risk_policy_version", "mode",         "account_mode",
      "order_types",         "time_in_force", "instruments",
      "accounts",            "account_currency", "account_currency_minor_scale",
      "fx_ttl_ms",           "equity_ttl_ms", "fx_upper_buffer_bps",
      "fx_pivot_currencies", "limits",       "margin_proxy_rate",
      "queues",              "safety"};
  if (const Status s = require_known_keys(root, kTopLevel, "root"); !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }

  RiskPolicy policy;

  std::int64_t version = 0;
  if (const Status s = read_i64(root, "risk_policy_version", version, "root", true);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  if (version <= 0) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                    "risk_policy_version must be > 0");
  }
  policy.version = static_cast<PolicyVersion>(version);

  std::string mode_text = "NORMAL";
  if (const Status s = read_string(root, "mode", mode_text, "root", false); !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  if (!parse_domain_mode(mode_text, policy.mode)) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid, "bad mode: " + mode_text);
  }

  if (const Status s =
          read_string(root, "account_mode", policy.account_mode, "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }

  if (const YamlNode* types = root.child("order_types"); types != nullptr) {
    if (types->kind != YamlNode::Kind::List) {
      return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                      "order_types must be an inline list");
    }
    policy.order_types.clear();
    for (const std::string& t : types->list) {
      OrderType parsed;
      if (!parse_order_type(t, parsed)) {
        return Result<RiskPolicy>::fail(Reason::UnsupportedOrderType, t);
      }
      policy.order_types.push_back(parsed);
    }
  }
  if (const YamlNode* tifs = root.child("time_in_force"); tifs != nullptr) {
    if (tifs->kind != YamlNode::Kind::List) {
      return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                      "time_in_force must be an inline list");
    }
    policy.time_in_force.clear();
    for (const std::string& t : tifs->list) {
      TimeInForce parsed;
      if (!parse_time_in_force(t, parsed)) {
        return Result<RiskPolicy>::fail(Reason::UnsupportedTimeInForce, t);
      }
      policy.time_in_force.push_back(parsed);
    }
  }

  if (const Status s = read_string(root, "account_currency",
                                   policy.account_currency.code, "root", true);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  std::int64_t minor_scale = policy.account_currency.minor_scale;
  if (const Status s = read_i64(root, "account_currency_minor_scale", minor_scale,
                                "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  policy.account_currency.minor_scale = static_cast<int>(minor_scale);

  if (const Status s = read_i64(root, "fx_ttl_ms", policy.fx_ttl_ms, "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  if (const Status s =
          read_i64(root, "equity_ttl_ms", policy.equity_ttl_ms, "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  if (const Status s = read_i64(root, "fx_upper_buffer_bps",
                                policy.fx_upper_buffer_bps, "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  if (const YamlNode* pivots = root.child("fx_pivot_currencies"); pivots != nullptr) {
    if (pivots->kind != YamlNode::Kind::List) {
      return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                      "fx_pivot_currencies must be an inline list");
    }
    policy.fx_pivot_currencies = pivots->list;
  }
  if (const Status s = read_rational(root, "margin_proxy_rate",
                                     policy.margin_proxy_rate, "root", false);
      !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }

  // --- instruments -------------------------------------------------------
  const YamlNode* instruments = root.child("instruments");
  if (instruments == nullptr || instruments->kind != YamlNode::Kind::Map) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                    "instruments block is required");
  }
  static const std::set<std::string> kInstrumentKeys = {
      "base_currency",        "quote_currency",
      "quantity_step_base_units", "min_qty_base_units",
      "max_qty_base_units",   "tick_size",
      "contract_multiplier",  "quote_ttl_ms",
      "risk_price_upper_buffer_bps", "max_price_deviation_bps",
      "product_mode",         "enabled",
      "version"};
  for (const auto& [symbol, node] : instruments->map) {
    if (node.kind != YamlNode::Kind::Map) {
      return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                      symbol + " is not a mapping");
    }
    if (const Status s = require_known_keys(node, kInstrumentKeys, symbol); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    InstrumentSpec spec;
    spec.symbol = symbol;
    if (const Status s = read_string(node, "base_currency", spec.base_currency,
                                     symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_string(node, "quote_currency", spec.quote_currency,
                                     symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "quantity_step_base_units",
                                  spec.quantity_step_base_units, symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "min_qty_base_units",
                                  spec.min_qty_base_units, symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "max_qty_base_units",
                                  spec.max_qty_base_units, symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_rational(node, "tick_size", spec.tick_size, symbol, true);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_rational(node, "contract_multiplier",
                                       spec.contract_multiplier, symbol, false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "quote_ttl_ms", spec.quote_ttl_ms, symbol,
                                  false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "risk_price_upper_buffer_bps",
                                  spec.risk_price_upper_buffer_bps, symbol, false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(node, "max_price_deviation_bps",
                                  spec.max_price_deviation_bps, symbol, false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    std::string product_mode = "SPOT_CFD";
    if (const Status s = read_string(node, "product_mode", product_mode, symbol, false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (!parse_product_mode(product_mode, spec.product_mode)) {
      return Result<RiskPolicy>::fail(Reason::UnsupportedProductMode, product_mode);
    }
    if (const Status s = read_bool(node, "enabled", spec.enabled, symbol); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    std::int64_t spec_version = static_cast<std::int64_t>(policy.version);
    if (const Status s = read_i64(node, "version", spec_version, symbol, false); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    spec.version = static_cast<DataVersion>(spec_version);
    policy.instruments[symbol] = spec;
  }

  // --- accounts ----------------------------------------------------------
  static const std::set<std::string> kAccountKeys = {"margin_budget_minor",
                                                     "use_equity_feed", "enabled"};
  if (const YamlNode* accounts = root.child("accounts"); accounts != nullptr) {
    if (accounts->kind != YamlNode::Kind::Map) {
      return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                      "accounts must be a mapping");
    }
    for (const auto& [account_id, node] : accounts->map) {
      if (node.kind != YamlNode::Kind::Map) {
        return Result<RiskPolicy>::fail(Reason::PolicyInvalid,
                                        account_id + " is not a mapping");
      }
      if (const Status s = require_known_keys(node, kAccountKeys, account_id); !s) {
        return Result<RiskPolicy>::fail(s.reason(), s.detail());
      }
      AccountProfile profile;
      profile.account_id = account_id;
      if (const Status s = read_i64(node, "margin_budget_minor",
                                    profile.margin_budget_minor, account_id, true);
          !s) {
        return Result<RiskPolicy>::fail(s.reason(), s.detail());
      }
      if (const Status s =
              read_bool(node, "use_equity_feed", profile.use_equity_feed, account_id);
          !s) {
        return Result<RiskPolicy>::fail(s.reason(), s.detail());
      }
      if (const Status s = read_bool(node, "enabled", profile.enabled, account_id); !s) {
        return Result<RiskPolicy>::fail(s.reason(), s.detail());
      }
      policy.accounts[account_id] = profile;
    }
  }

  // --- limits ------------------------------------------------------------
  static const std::set<std::string> kLimitKeys = {
      "max_order_notional_minor",      "max_chain_qty_base_units",
      "max_account_symbol_net_minor",  "max_account_symbol_gross_minor",
      "max_account_total_gross_minor", "max_firm_gross_minor",
      "min_margin_buffer_minor",       "max_working_orders_per_account",
      "max_working_orders_per_account_symbol", "max_admissions_per_second"};
  const YamlNode* limits = root.child("limits");
  if (limits == nullptr || limits->kind != YamlNode::Kind::Map) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid, "limits block is required");
  }
  if (const Status s = require_known_keys(*limits, kLimitKeys, "limits"); !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  Limits& L = policy.limits;
  const std::pair<const char*, std::int64_t*> limit_fields[] = {
      {"max_order_notional_minor", &L.max_order_notional_minor},
      {"max_chain_qty_base_units", &L.max_chain_qty_base_units},
      {"max_account_symbol_net_minor", &L.max_account_symbol_net_minor},
      {"max_account_symbol_gross_minor", &L.max_account_symbol_gross_minor},
      {"max_account_total_gross_minor", &L.max_account_total_gross_minor},
      {"max_firm_gross_minor", &L.max_firm_gross_minor},
      {"min_margin_buffer_minor", &L.min_margin_buffer_minor},
      {"max_working_orders_per_account", &L.max_working_orders_per_account},
      {"max_working_orders_per_account_symbol",
       &L.max_working_orders_per_account_symbol},
      {"max_admissions_per_second", &L.max_admissions_per_second},
  };
  for (const auto& [name, target] : limit_fields) {
    if (const Status s = read_i64(*limits, name, *target, "limits", false); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
  }

  // --- queues ------------------------------------------------------------
  if (const YamlNode* queues = root.child("queues"); queues != nullptr) {
    static const std::set<std::string> kQueueKeys = {"admission_cap", "event_cap",
                                                     "recon_cap"};
    if (const Status s = require_known_keys(*queues, kQueueKeys, "queues"); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s = read_i64(*queues, "admission_cap",
                                  policy.queues.admission_cap, "queues", false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s =
            read_i64(*queues, "event_cap", policy.queues.event_cap, "queues", false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    if (const Status s =
            read_i64(*queues, "recon_cap", policy.queues.recon_cap, "queues", false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
  }

  // --- safety ------------------------------------------------------------
  if (const YamlNode* safety = root.child("safety"); safety != nullptr) {
    static const std::set<std::string> kSafetyKeys = {
        "require_atomic_outbox",  "abort_on_stale_data",
        "allow_market_orders",    "allow_reduce_only_new",
        "auto_release_on_timeout", "max_pending_modify_per_chain"};
    if (const Status s = require_known_keys(*safety, kSafetyKeys, "safety"); !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
    SafetyFlags& F = policy.safety;
    const std::pair<const char*, bool*> flags[] = {
        {"require_atomic_outbox", &F.require_atomic_outbox},
        {"abort_on_stale_data", &F.abort_on_stale_data},
        {"allow_market_orders", &F.allow_market_orders},
        {"allow_reduce_only_new", &F.allow_reduce_only_new},
        {"auto_release_on_timeout", &F.auto_release_on_timeout},
    };
    for (const auto& [name, target] : flags) {
      if (const Status s = read_bool(*safety, name, *target, "safety"); !s) {
        return Result<RiskPolicy>::fail(s.reason(), s.detail());
      }
    }
    if (const Status s = read_i64(*safety, "max_pending_modify_per_chain",
                                  F.max_pending_modify_per_chain, "safety", false);
        !s) {
      return Result<RiskPolicy>::fail(s.reason(), s.detail());
    }
  }

  if (const Status s = policy.validate(); !s) {
    return Result<RiskPolicy>::fail(s.reason(), s.detail());
  }
  return Result<RiskPolicy>(policy);
}

Result<RiskPolicy> load_policy_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Result<RiskPolicy>::fail(Reason::PolicyInvalid, "cannot open " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  Result<YamlNode> root = parse_yaml_subset(buffer.str());
  if (!root.ok()) return Result<RiskPolicy>::fail(root.reason(), root.detail());
  return policy_from_yaml(root.value());
}

std::string policy_to_yaml(const RiskPolicy& policy) {
  std::string out;
  out += "risk_policy_version: " + std::to_string(policy.version) + "\n";
  out += "mode: " + std::string(domain_mode_name(policy.mode)) + "\n";
  out += "account_mode: " + policy.account_mode + "\n";
  out += "order_types: [LIMIT]\n";
  out += "time_in_force: [DAY]\n";
  out += "account_currency: " + policy.account_currency.code + "\n";
  out += "account_currency_minor_scale: " +
         std::to_string(policy.account_currency.minor_scale) + "\n";
  out += "fx_ttl_ms: " + std::to_string(policy.fx_ttl_ms) + "\n";
  out += "equity_ttl_ms: " + std::to_string(policy.equity_ttl_ms) + "\n";
  out += "fx_upper_buffer_bps: " + std::to_string(policy.fx_upper_buffer_bps) + "\n";

  out += "fx_pivot_currencies: [";
  for (std::size_t i = 0; i < policy.fx_pivot_currencies.size(); ++i) {
    if (i > 0) out += ", ";
    out += policy.fx_pivot_currencies[i];
  }
  out += "]\n";

  out += "margin_proxy_rate: \"" + rational_to_string(policy.margin_proxy_rate) + "\"\n";

  out += "instruments:\n";
  for (const auto& [symbol, spec] : policy.instruments) {
    out += "  " + symbol + ":\n";
    out += "    base_currency: " + spec.base_currency + "\n";
    out += "    quote_currency: " + spec.quote_currency + "\n";
    out += "    quantity_step_base_units: " +
           std::to_string(spec.quantity_step_base_units) + "\n";
    out += "    min_qty_base_units: " + std::to_string(spec.min_qty_base_units) + "\n";
    out += "    max_qty_base_units: " + std::to_string(spec.max_qty_base_units) + "\n";
    out += "    tick_size: \"" + rational_to_string(spec.tick_size) + "\"\n";
    out += "    contract_multiplier: \"" +
           rational_to_string(spec.contract_multiplier) + "\"\n";
    out += "    quote_ttl_ms: " + std::to_string(spec.quote_ttl_ms) + "\n";
    out += "    risk_price_upper_buffer_bps: " +
           std::to_string(spec.risk_price_upper_buffer_bps) + "\n";
    out += "    max_price_deviation_bps: " +
           std::to_string(spec.max_price_deviation_bps) + "\n";
    out += "    product_mode: SPOT_CFD\n";
    out += std::string("    enabled: ") + (spec.enabled ? "true" : "false") + "\n";
    out += "    version: " + std::to_string(spec.version) + "\n";
  }

  out += "accounts:\n";
  for (const auto& [id, profile] : policy.accounts) {
    out += "  " + id + ":\n";
    out += "    margin_budget_minor: " + std::to_string(profile.margin_budget_minor) +
           "\n";
    out += std::string("    use_equity_feed: ") +
           (profile.use_equity_feed ? "true" : "false") + "\n";
    out += std::string("    enabled: ") + (profile.enabled ? "true" : "false") + "\n";
  }

  const Limits& L = policy.limits;
  out += "limits:\n";
  out += "  max_order_notional_minor: " + std::to_string(L.max_order_notional_minor) + "\n";
  out += "  max_chain_qty_base_units: " + std::to_string(L.max_chain_qty_base_units) + "\n";
  out += "  max_account_symbol_net_minor: " +
         std::to_string(L.max_account_symbol_net_minor) + "\n";
  out += "  max_account_symbol_gross_minor: " +
         std::to_string(L.max_account_symbol_gross_minor) + "\n";
  out += "  max_account_total_gross_minor: " +
         std::to_string(L.max_account_total_gross_minor) + "\n";
  out += "  max_firm_gross_minor: " + std::to_string(L.max_firm_gross_minor) + "\n";
  out += "  min_margin_buffer_minor: " + std::to_string(L.min_margin_buffer_minor) + "\n";
  out += "  max_working_orders_per_account: " +
         std::to_string(L.max_working_orders_per_account) + "\n";
  out += "  max_working_orders_per_account_symbol: " +
         std::to_string(L.max_working_orders_per_account_symbol) + "\n";
  out += "  max_admissions_per_second: " +
         std::to_string(L.max_admissions_per_second) + "\n";

  out += "queues:\n";
  out += "  admission_cap: " + std::to_string(policy.queues.admission_cap) + "\n";
  out += "  event_cap: " + std::to_string(policy.queues.event_cap) + "\n";
  out += "  recon_cap: " + std::to_string(policy.queues.recon_cap) + "\n";

  const SafetyFlags& F = policy.safety;
  out += "safety:\n";
  out += std::string("  require_atomic_outbox: ") +
         (F.require_atomic_outbox ? "true" : "false") + "\n";
  out += std::string("  abort_on_stale_data: ") +
         (F.abort_on_stale_data ? "true" : "false") + "\n";
  out += std::string("  allow_market_orders: ") +
         (F.allow_market_orders ? "true" : "false") + "\n";
  out += std::string("  allow_reduce_only_new: ") +
         (F.allow_reduce_only_new ? "true" : "false") + "\n";
  out += std::string("  auto_release_on_timeout: ") +
         (F.auto_release_on_timeout ? "true" : "false") + "\n";
  out += "  max_pending_modify_per_chain: " +
         std::to_string(F.max_pending_modify_per_chain) + "\n";

  return out;
}

}  // namespace risk
