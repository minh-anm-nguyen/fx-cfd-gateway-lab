// Core identifier and quantity types.
//
// Units are always *base units* of the instrument, never lots or contracts
// (§8). Money is always minor currency units paired with an explicit currency
// code and scale, never a bare integer, so the limit checker cannot compare two
// amounts denominated differently.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "risk/result.hpp"

namespace risk {

using TenantId = std::string;
using DomainId = std::string;
using AccountId = std::string;
using CommandId = std::string;
using OrderChainId = std::string;
using SymbolId = std::string;
using ReservationId = std::string;
using EventId = std::string;

// Monotonically increasing durable counters. `Revision` is the domain commit
// index; `FencingToken` is the writer-ownership epoch enforced at the store
// boundary (RI-25).
using Revision = std::uint64_t;
using FencingToken = std::uint64_t;
using PolicyVersion = std::uint64_t;
using DataVersion = std::uint64_t;

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };

constexpr std::string_view side_name(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

bool parse_side(std::string_view text, Side& out) noexcept;

// Signed in the direction of the side: +1 for BUY, -1 for SELL.
constexpr std::int64_t side_sign(Side s) noexcept {
  return s == Side::Buy ? 1 : -1;
}

enum class OrderType : std::uint8_t { Limit = 1 };
enum class TimeInForce : std::uint8_t { Day = 1 };

bool parse_order_type(std::string_view text, OrderType& out) noexcept;
bool parse_time_in_force(std::string_view text, TimeInForce& out) noexcept;

// Base units. Nonnegative for order quantities; `position_units` is signed and
// carried as a plain std::int64_t to make the sign explicit at the use site.
struct Units {
  std::int64_t value = 0;

  constexpr Units() = default;
  constexpr explicit Units(std::int64_t v) : value(v) {}

  friend constexpr bool operator==(Units a, Units b) { return a.value == b.value; }
  friend constexpr bool operator!=(Units a, Units b) { return a.value != b.value; }
  friend constexpr bool operator<(Units a, Units b) { return a.value < b.value; }
};

// ISO 4217 code plus the number of decimal places used for its minor unit.
struct Currency {
  std::string code;   // "USD"
  int minor_scale = 2;  // 10^2 minor units per major unit

  bool valid() const noexcept;
};

// An amount in minor units of a named currency. Two MoneyMinor values may only
// be compared or summed when their currency codes match.
struct MoneyMinor {
  std::int64_t minor = 0;
  std::string currency;

  bool same_currency(const MoneyMinor& o) const noexcept {
    return currency == o.currency;
  }
};

// Every decision pins the exact versions of the reference data it used
// (RI-30). A decision whose snapshot refs are missing is not auditable and is
// therefore not allowed to commit.
struct SnapshotRef {
  PolicyVersion policy = 0;
  DataVersion instrument = 0;
  DataVersion quote = 0;
  DataVersion fx = 0;
  DataVersion equity = 0;
  Revision ledger = 0;

  std::string digest() const;
};

// Identifier hygiene (G01/G18): canonical ASCII, bounded length, no control
// characters, no separators that would let one field impersonate another when
// fields are concatenated for hashing.
constexpr std::size_t kMaxIdLength = 64;

Status validate_identifier(std::string_view value, std::string_view field);

// Symbols additionally must be upper-case alphanumeric.
Status validate_symbol(std::string_view value);

}  // namespace risk
