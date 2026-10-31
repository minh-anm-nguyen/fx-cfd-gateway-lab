#include "risk/market.hpp"

#include <algorithm>

#include "risk/checked.hpp"

namespace risk {
namespace {

std::string edge_key(const std::string& from, const std::string& to) {
  return from + ">" + to;
}

// Effective TTL is the stricter of the record's own TTL and the policy TTL, so
// neither a generous feed nor a generous policy can extend the other.
std::int64_t effective_ttl_ms(std::int64_t record_ttl_ms,
                              std::int64_t policy_ttl_ms) {
  if (record_ttl_ms <= 0) return policy_ttl_ms;
  if (policy_ttl_ms <= 0) return record_ttl_ms;
  return std::min(record_ttl_ms, policy_ttl_ms);
}

enum class Freshness { Fresh, Expired, Future };

Freshness freshness_of(MonoNanos ingest_mono, MonoNanos now_mono,
                       std::int64_t ttl_ms) {
  if (ingest_mono > now_mono) return Freshness::Future;
  const MonoNanos age = now_mono - ingest_mono;
  // The boundary is exclusive: at exactly ttl the record is expired. PB-04
  // advances the clock to the boundary and expects a denial.
  return age < millis_to_nanos(ttl_ms) ? Freshness::Fresh : Freshness::Expired;
}

}  // namespace

bool apply_upper_buffer_bps(const Rational& r, std::int64_t bps, Rational& out) {
  if (bps < 0) return false;
  if (bps == 0) {
    out = r;
    return true;
  }
  std::int64_t num = 0;
  if (!add_i64(10000, bps, num)) return false;
  return rational_mul(r, Rational{num, 10000}, out);
}

// ---------------------------------------------------------------------------
// MarketFeed
// ---------------------------------------------------------------------------

void MarketFeed::trust_source(std::string source_id) {
  trusted_sources_.insert(std::move(source_id));
}

bool MarketFeed::is_trusted(const std::string& source_id) const {
  return trusted_sources_.count(source_id) != 0;
}

Status MarketFeed::publish_quote(const Quote& q) {
  if (const Status s = validate_symbol(q.symbol); !s) return s;
  if (!is_trusted(q.source_id)) {
    return Status::fail(Reason::QuoteSourceUntrusted, q.source_id);
  }
  if (q.bid_ticks <= 0 || q.ask_ticks <= 0) {
    return Status::fail(Reason::PriceNonPositive, q.symbol);
  }
  if (q.bid_ticks > q.ask_ticks) {
    return Status::fail(Reason::QuoteCrossed, q.symbol);
  }
  if (q.version == 0) {
    return Status::fail(Reason::QuoteMissing, "version must be > 0");
  }
  // A sequence that goes backwards is a replay or a forged record; the newer
  // record is refused rather than overwriting good data (G03).
  const auto it = quotes_.find(q.symbol);
  if (it != quotes_.end()) {
    if (q.source_id != it->second.source_id) {
      return Status::fail(Reason::QuoteSourceUntrusted,
                          q.symbol + ": source changed mid-stream");
    }
    if (q.sequence <= it->second.sequence) {
      return Status::fail(Reason::QuoteSequenceRegression, q.symbol);
    }
  }
  quotes_[q.symbol] = q;
  return Status::success();
}

Status MarketFeed::publish_fx(const FxRate& r) {
  if (!Currency{r.from_currency, 2}.valid() || !Currency{r.to_currency, 2}.valid()) {
    return Status::fail(Reason::FxMissing, "invalid currency code");
  }
  if (r.from_currency == r.to_currency) {
    return Status::fail(Reason::FxMissing, "identity edge is implicit");
  }
  if (!is_trusted(r.source_id)) {
    return Status::fail(Reason::QuoteSourceUntrusted, r.source_id);
  }
  if (!r.bid.positive() || !r.ask.positive()) {
    return Status::fail(Reason::FxNonPositive, edge_key(r.from_currency, r.to_currency));
  }
  int cmp = 0;
  if (!rational_compare(r.bid, r.ask, cmp)) {
    return Status::fail(Reason::ArithmeticOverflow, "fx spread comparison");
  }
  if (cmp > 0) {
    return Status::fail(Reason::QuoteCrossed, edge_key(r.from_currency, r.to_currency));
  }
  if (r.version == 0) {
    return Status::fail(Reason::FxMissing, "version must be > 0");
  }
  const std::string key = edge_key(r.from_currency, r.to_currency);
  const auto it = fx_.find(key);
  if (it != fx_.end() && r.sequence <= it->second.sequence) {
    return Status::fail(Reason::QuoteSequenceRegression, key);
  }
  fx_[key] = r;
  return Status::success();
}

Status MarketFeed::publish_equity(const EquitySnapshot& e) {
  if (const Status s = validate_identifier(e.account_id, "account_id"); !s) return s;
  if (!Currency{e.currency, 2}.valid()) {
    return Status::fail(Reason::EquityCurrencyMismatch, e.currency);
  }
  if (e.equity_minor < 0 || e.existing_margin_minor < 0) {
    return Status::fail(Reason::EquityMissing, "negative equity figures");
  }
  if (e.version == 0) {
    return Status::fail(Reason::EquityMissing, "version must be > 0");
  }
  equity_[e.account_id] = e;
  return Status::success();
}

MarketFeed::View MarketFeed::snapshot() const {
  View v;
  v.quotes_ = quotes_;
  v.fx_ = fx_;
  v.equity_ = equity_;
  v.trusted_sources_ = trusted_sources_;
  return v;
}

// ---------------------------------------------------------------------------
// MarketFeed::View
// ---------------------------------------------------------------------------

Result<Quote> MarketFeed::View::quote(const SymbolId& symbol, MonoNanos now_mono,
                                      std::int64_t ttl_ms) const {
  const auto it = quotes_.find(symbol);
  if (it == quotes_.end()) {
    return Result<Quote>::fail(Reason::QuoteMissing, symbol);
  }
  const Quote& q = it->second;
  if (!trusted_sources_.count(q.source_id)) {
    return Result<Quote>::fail(Reason::QuoteSourceUntrusted, q.source_id);
  }
  if (q.bid_ticks <= 0 || q.ask_ticks <= 0) {
    return Result<Quote>::fail(Reason::PriceNonPositive, symbol);
  }
  if (q.bid_ticks > q.ask_ticks) {
    return Result<Quote>::fail(Reason::QuoteCrossed, symbol);
  }
  switch (freshness_of(q.ingest_mono, now_mono, effective_ttl_ms(q.ttl_ms, ttl_ms))) {
    case Freshness::Fresh:
      return Result<Quote>(q);
    case Freshness::Expired:
      return Result<Quote>::fail(Reason::QuoteStale, symbol);
    case Freshness::Future:
      // Ingest timestamp ahead of now on the monotonic axis means the reading
      // did not come from this process's clock: refuse rather than guess.
      return Result<Quote>::fail(Reason::ClockRollback, symbol);
  }
  return Result<Quote>::fail(Reason::InternalInvariant, symbol);
}

const FxRate* MarketFeed::View::find_edge(const std::string& from,
                                          const std::string& to) const {
  const auto it = fx_.find(edge_key(from, to));
  return it == fx_.end() ? nullptr : &it->second;
}

Result<FxResolution> MarketFeed::View::direct_or_inverse(
    const std::string& from, const std::string& to, MonoNanos now_mono,
    std::int64_t ttl_ms) const {
  auto usable = [&](const FxRate* r) -> bool {
    if (r == nullptr) return false;
    if (!trusted_sources_.count(r->source_id)) return false;
    return freshness_of(r->ingest_mono, now_mono,
                        effective_ttl_ms(r->ttl_ms, ttl_ms)) == Freshness::Fresh;
  };

  // A direct edge is the pinned canonical path; it wins over an inverted one so
  // the resolution is deterministic rather than ambiguous (§9.1).
  if (const FxRate* d = find_edge(from, to); d != nullptr) {
    if (usable(d)) {
      FxResolution r;
      r.upper_bound = d->ask;  // most `to` per `from`: the unfavourable side
      r.kind = FxPathKind::Direct;
      r.version = d->version;
      r.path = edge_key(from, to);
      return Result<FxResolution>(r);
    }
    return Result<FxResolution>::fail(Reason::FxStale, edge_key(from, to));
  }

  if (const FxRate* inv = find_edge(to, from); inv != nullptr) {
    if (!usable(inv)) {
      return Result<FxResolution>::fail(Reason::FxStale, edge_key(to, from));
    }
    if (!inv->bid.positive()) {
      return Result<FxResolution>::fail(Reason::FxNonPositive, edge_key(to, from));
    }
    // Converting from->to using a to->from quote means dividing. The
    // unfavourable direction is the *smallest* to->from rate, i.e. bid.
    FxResolution r;
    r.upper_bound = Rational{inv->bid.den, inv->bid.num};
    if (!rational_reduce(r.upper_bound)) {
      return Result<FxResolution>::fail(Reason::ArithmeticOverflow, "fx inverse");
    }
    r.kind = FxPathKind::Inverse;
    r.version = inv->version;
    r.path = edge_key(from, to) + "(inv)";
    return Result<FxResolution>(r);
  }

  return Result<FxResolution>::fail(Reason::FxPathMissing, edge_key(from, to));
}

Result<FxResolution> MarketFeed::View::fx_upper(
    const std::string& from, const std::string& to, MonoNanos now_mono,
    std::int64_t ttl_ms, const std::vector<std::string>& pivots,
    std::int64_t buffer_bps) const {
  if (!Currency{from, 2}.valid() || !Currency{to, 2}.valid()) {
    return Result<FxResolution>::fail(Reason::FxMissing, "invalid currency code");
  }
  if (from == to) {
    FxResolution r;
    r.upper_bound = Rational{1, 1};
    r.kind = FxPathKind::Identity;
    r.version = 0;
    r.path = from;
    return Result<FxResolution>(r);
  }

  Result<FxResolution> direct = direct_or_inverse(from, to, now_mono, ttl_ms);
  if (direct.ok()) {
    FxResolution r = direct.value();
    Rational buffered;
    if (!apply_upper_buffer_bps(r.upper_bound, buffer_bps, buffered)) {
      return Result<FxResolution>::fail(Reason::ArithmeticOverflow, "fx buffer");
    }
    r.upper_bound = buffered;
    return Result<FxResolution>(r);
  }
  // A stale direct edge is a stale conversion, not an invitation to route
  // around it through a pivot: that would silently change the pinned path.
  if (direct.reason() != Reason::FxPathMissing) return direct;

  std::optional<FxResolution> chosen;
  int resolved_paths = 0;
  for (const std::string& pivot : pivots) {
    if (pivot == from || pivot == to) continue;
    Result<FxResolution> leg1 = direct_or_inverse(from, pivot, now_mono, ttl_ms);
    if (!leg1.ok()) continue;
    Result<FxResolution> leg2 = direct_or_inverse(pivot, to, now_mono, ttl_ms);
    if (!leg2.ok()) continue;

    Rational product;
    if (!rational_mul(leg1.value().upper_bound, leg2.value().upper_bound, product)) {
      return Result<FxResolution>::fail(Reason::ArithmeticOverflow, "fx pivot");
    }
    ++resolved_paths;
    if (!chosen.has_value()) {
      FxResolution r;
      r.upper_bound = product;
      r.kind = FxPathKind::Pivot;
      r.version = std::max(leg1.value().version, leg2.value().version);
      r.path = from + "->" + pivot + "->" + to;
      chosen = r;
      continue;
    }
    // Two distinct pivots both resolve: the conversion is ambiguous and the
    // two paths may disagree. Deny rather than pick one (§9.1).
    int cmp = 0;
    if (!rational_compare(chosen->upper_bound, product, cmp) || cmp != 0) {
      return Result<FxResolution>::fail(
          Reason::FxPathAmbiguous,
          chosen->path + " vs " + from + "->" + pivot + "->" + to);
    }
  }

  if (resolved_paths == 0 || !chosen.has_value()) {
    return Result<FxResolution>::fail(Reason::FxPathMissing, edge_key(from, to));
  }

  Rational buffered;
  if (!apply_upper_buffer_bps(chosen->upper_bound, buffer_bps, buffered)) {
    return Result<FxResolution>::fail(Reason::ArithmeticOverflow, "fx buffer");
  }
  chosen->upper_bound = buffered;
  return Result<FxResolution>(*chosen);
}

Result<EquitySnapshot> MarketFeed::View::equity(const AccountId& account,
                                                MonoNanos now_mono,
                                                std::int64_t ttl_ms) const {
  const auto it = equity_.find(account);
  if (it == equity_.end()) {
    return Result<EquitySnapshot>::fail(Reason::EquityMissing, account);
  }
  const EquitySnapshot& e = it->second;
  switch (freshness_of(e.ingest_mono, now_mono, effective_ttl_ms(e.ttl_ms, ttl_ms))) {
    case Freshness::Fresh:
      return Result<EquitySnapshot>(e);
    case Freshness::Expired:
      return Result<EquitySnapshot>::fail(Reason::EquityStale, account);
    case Freshness::Future:
      return Result<EquitySnapshot>::fail(Reason::ClockRollback, account);
  }
  return Result<EquitySnapshot>::fail(Reason::InternalInvariant, account);
}

DataVersion MarketFeed::View::max_quote_version() const {
  DataVersion v = 0;
  for (const auto& [_, q] : quotes_) v = std::max(v, q.version);
  return v;
}

DataVersion MarketFeed::View::max_fx_version() const {
  DataVersion v = 0;
  for (const auto& [_, r] : fx_) v = std::max(v, r.version);
  return v;
}

}  // namespace risk
