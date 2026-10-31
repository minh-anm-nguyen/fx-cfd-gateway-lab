#include "risk/types.hpp"

#include "risk/hash.hpp"

namespace risk {

bool parse_side(std::string_view text, Side& out) noexcept {
  if (text == "BUY") {
    out = Side::Buy;
    return true;
  }
  if (text == "SELL") {
    out = Side::Sell;
    return true;
  }
  return false;
}

bool parse_order_type(std::string_view text, OrderType& out) noexcept {
  if (text == "LIMIT") {
    out = OrderType::Limit;
    return true;
  }
  return false;
}

bool parse_time_in_force(std::string_view text, TimeInForce& out) noexcept {
  if (text == "DAY") {
    out = TimeInForce::Day;
    return true;
  }
  return false;
}

bool Currency::valid() const noexcept {
  if (code.size() != 3) return false;
  for (const char c : code) {
    if (c < 'A' || c > 'Z') return false;
  }
  return minor_scale >= 0 && minor_scale <= 8;
}

std::string SnapshotRef::digest() const {
  Hasher h;
  h.named("policy", policy)
      .named("instrument", instrument)
      .named("quote", quote)
      .named("fx", fx)
      .named("equity", equity)
      .named("ledger", ledger);
  return h.hex();
}

Status validate_identifier(std::string_view value, std::string_view field) {
  if (value.empty()) {
    return Status::fail(Reason::MalformedIdentifier,
                        std::string(field) + " is empty");
  }
  if (value.size() > kMaxIdLength) {
    return Status::fail(Reason::IdentifierTooLong,
                        std::string(field) + " exceeds " +
                            std::to_string(kMaxIdLength) + " bytes");
  }
  for (const char c : value) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    const bool allowed_punct = (c == '-' || c == '_' || c == '.');
    if (!alnum && !allowed_punct) {
      // Rejects control characters, whitespace, non-ASCII bytes (so no Unicode
      // normalisation ambiguity), quotes, and the '|'/':' framing characters
      // used by the canonical hasher.
      return Status::fail(Reason::MalformedIdentifier,
                          std::string(field) + " has a disallowed character");
    }
  }
  return Status::success();
}

Status validate_symbol(std::string_view value) {
  const Status base = validate_identifier(value, "symbol");
  if (!base) return base;
  for (const char c : value) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.';
    if (!ok) {
      return Status::fail(Reason::MalformedIdentifier,
                          "symbol must be upper-case alphanumeric");
    }
  }
  return Status::success();
}

}  // namespace risk
