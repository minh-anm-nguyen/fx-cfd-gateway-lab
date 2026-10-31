// Versioned market reference data with conservative freshness gates.
//
// Everything here is a *snapshot*: once an admission starts, it reads one
// immutable `MarketView` and the same view is re-validated immediately before
// commit (§12.1 step 6, PB-04). Freshness is measured on the monotonic axis so
// a wall-clock step cannot resurrect an expired quote (RI-12).
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "risk/clock.hpp"
#include "risk/rational.hpp"
#include "risk/result.hpp"
#include "risk/types.hpp"

namespace risk {

struct Quote {
  std::string source_id;
  SymbolId symbol;
  std::int64_t bid_ticks = 0;
  std::int64_t ask_ticks = 0;

  UtcNanos event_time_utc = 0;  // audit only
  MonoNanos ingest_mono = 0;    // the axis TTLs are measured on
  std::uint64_t sequence = 0;
  DataVersion version = 0;
  std::int64_t ttl_ms = 0;
};

// An exchange rate quoted as `to` units per one `from` unit. Carrying both
// sides of the spread is what lets the valuation pick the unfavourable
// direction rather than a midpoint (§9.1).
struct FxRate {
  std::string source_id;
  std::string from_currency;
  std::string to_currency;
  Rational bid{1, 1};  // lowest `to` per `from`
  Rational ask{1, 1};  // highest `to` per `from`

  UtcNanos event_time_utc = 0;
  MonoNanos ingest_mono = 0;
  std::uint64_t sequence = 0;
  DataVersion version = 0;
  std::int64_t ttl_ms = 0;
};

struct EquitySnapshot {
  AccountId account_id;
  std::string currency;
  std::int64_t equity_minor = 0;
  std::int64_t existing_margin_minor = 0;

  MonoNanos ingest_mono = 0;
  DataVersion version = 0;
  std::int64_t ttl_ms = 0;
};

// How a currency conversion was resolved. Recorded in the audit so a later
// review can tell a direct quote from an inverted or pivoted one.
enum class FxPathKind : std::uint8_t { Identity, Direct, Inverse, Pivot };

struct FxResolution {
  Rational upper_bound{1, 1};  // conservative `to` per `from`
  FxPathKind kind = FxPathKind::Identity;
  DataVersion version = 0;
  std::string path;  // e.g. "JPY->USD" or "JPY->EUR->USD"
};

// Mutable feed ingest side. Rejects data that is internally inconsistent at
// ingest time so a poisoned record never reaches a decision (G03).
class MarketFeed {
 public:
  // Sources must be whitelisted before their data is accepted (RI-12: trust
  // provenance, not merely the transport).
  void trust_source(std::string source_id);
  bool is_trusted(const std::string& source_id) const;

  Status publish_quote(const Quote& q);
  Status publish_fx(const FxRate& r);
  Status publish_equity(const EquitySnapshot& e);

  // Immutable read side; the returned view is a copy, so a feed update racing
  // an in-flight decision cannot change the numbers under it.
  class View;
  View snapshot() const;

  DataVersion next_version() { return ++version_counter_; }

 private:
  std::map<SymbolId, Quote> quotes_;
  std::map<std::string, FxRate> fx_;  // key: "FROM>TO"
  std::map<AccountId, EquitySnapshot> equity_;
  std::set<std::string> trusted_sources_;
  DataVersion version_counter_ = 0;

  friend class MarketFeed::View;
};

class MarketFeed::View {
 public:
  View() = default;

  // Returns the quote only when it is present, from a trusted source, not
  // crossed, strictly positive and within `ttl_ms` of `now_mono`.
  Result<Quote> quote(const SymbolId& symbol, MonoNanos now_mono,
                      std::int64_t ttl_ms) const;

  // Conservative upper bound for converting `from` into `to`.
  // `pivots` is the ordered whitelist of intermediate currencies; a conversion
  // resolvable through two different pivots is ambiguous and denied (§9.1).
  Result<FxResolution> fx_upper(const std::string& from, const std::string& to,
                                MonoNanos now_mono, std::int64_t ttl_ms,
                                const std::vector<std::string>& pivots,
                                std::int64_t buffer_bps) const;

  Result<EquitySnapshot> equity(const AccountId& account, MonoNanos now_mono,
                                std::int64_t ttl_ms) const;

  // Highest version seen across the view, used as the pinned `quote`/`fx`
  // version in a SnapshotRef when no single record dominates.
  DataVersion max_quote_version() const;
  DataVersion max_fx_version() const;

 private:
  friend class MarketFeed;

  const FxRate* find_edge(const std::string& from, const std::string& to) const;
  Result<FxResolution> direct_or_inverse(const std::string& from,
                                         const std::string& to,
                                         MonoNanos now_mono,
                                         std::int64_t ttl_ms) const;

  std::map<SymbolId, Quote> quotes_;
  std::map<std::string, FxRate> fx_;
  std::map<AccountId, EquitySnapshot> equity_;
  std::set<std::string> trusted_sources_;
};

using MarketView = MarketFeed::View;

// Applies a bps buffer to a conservative upper bound: r * (10000 + bps)/10000.
bool apply_upper_buffer_bps(const Rational& r, std::int64_t bps, Rational& out);

}  // namespace risk
