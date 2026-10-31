// G03 / G04 — price feeds, freshness, source integrity, FX conversion paths.
//
// Everything here is about the two ways a feed can hurt you: data that is
// wrong (crossed, replayed, from an untrusted source) and data that is merely
// old. The first is refused at ingest; the second is refused at read time,
// measured on the monotonic axis so a wall-clock step cannot hide it (RI-12).
#include "risk/clock.hpp"
#include "risk/market.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr const char* kTrusted = "LAB-FEED";
constexpr const char* kUnknown = "SOMEONE-ELSE";

struct Harness {
  VirtualClock clock;
  MarketFeed feed;

  Harness() { feed.trust_source(kTrusted); }

  Quote quote(const SymbolId& symbol, std::int64_t bid, std::int64_t ask,
              std::uint64_t sequence, std::int64_t ttl_ms = 500,
              const char* source = kTrusted) {
    Quote q;
    q.source_id = source;
    q.symbol = symbol;
    q.bid_ticks = bid;
    q.ask_ticks = ask;
    q.event_time_utc = clock.utc_now();
    q.ingest_mono = clock.mono_now();
    q.sequence = sequence;
    q.version = sequence;
    q.ttl_ms = ttl_ms;
    return q;
  }

  FxRate fx(const std::string& from, const std::string& to, Rational bid,
            Rational ask, std::uint64_t sequence, std::int64_t ttl_ms = 500,
            const char* source = kTrusted) {
    FxRate r;
    r.source_id = source;
    r.from_currency = from;
    r.to_currency = to;
    r.bid = bid;
    r.ask = ask;
    r.event_time_utc = clock.utc_now();
    r.ingest_mono = clock.mono_now();
    r.sequence = sequence;
    r.version = sequence;
    r.ttl_ms = ttl_ms;
    return r;
  }
};

}  // namespace

TEST(unit_market, rejects_untrusted_source_at_ingest) {
  Harness h;
  const Status s = h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1, 500,
                                                kUnknown));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()),
           static_cast<int>(Reason::QuoteSourceUntrusted));
}

TEST(unit_market, rejects_crossed_and_non_positive_quotes) {
  Harness h;
  Status s = h.feed.publish_quote(h.quote("EURUSD", 110001, 110000, 1));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::QuoteCrossed));

  s = h.feed.publish_quote(h.quote("EURUSD", 0, 110000, 1));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::PriceNonPositive));

  s = h.feed.publish_quote(h.quote("EURUSD", -5, -1, 1));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::PriceNonPositive));
}

TEST(unit_market, rejects_sequence_regression_and_replay) {
  Harness h;
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 5)).ok());

  // Same sequence replayed.
  Status s = h.feed.publish_quote(h.quote("EURUSD", 100000, 100010, 5));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()),
           static_cast<int>(Reason::QuoteSequenceRegression));

  // Older sequence.
  s = h.feed.publish_quote(h.quote("EURUSD", 100000, 100010, 4));
  CHECK(!s.ok());

  // The good record must survive the rejected ones.
  const MarketView view = h.feed.snapshot();
  Result<Quote> q = view.quote("EURUSD", h.clock.mono_now(), 500);
  CHECK(q.ok());
  CHECK_EQ(q.value().ask_ticks, 110000);
}

TEST(unit_market, refuses_a_source_change_mid_stream) {
  Harness h;
  h.feed.trust_source("OTHER-FEED");
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1)).ok());
  // Both sources are trusted, but a symbol silently switching provider is the
  // shape of a poisoning attempt, not a normal update.
  const Status s =
      h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 2, 500, "OTHER-FEED"));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()),
           static_cast<int>(Reason::QuoteSourceUntrusted));
}

TEST(unit_market, ttl_boundary_is_exclusive) {
  Harness h;
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1, 500)).ok());
  const MarketView view = h.feed.snapshot();
  const MonoNanos ingest = h.clock.mono_now();

  // One nanosecond before the boundary: fresh.
  CHECK(view.quote("EURUSD", ingest + millis_to_nanos(500) - 1, 500).ok());
  // Exactly at the boundary: expired. PB-04 relies on this being the strict
  // reading rather than a <= that keeps the quote alive one tick too long.
  Result<Quote> at_boundary = view.quote("EURUSD", ingest + millis_to_nanos(500), 500);
  CHECK(!at_boundary.ok());
  CHECK_EQ(static_cast<int>(at_boundary.reason()),
           static_cast<int>(Reason::QuoteStale));
}

TEST(unit_market, the_stricter_of_record_and_policy_ttl_wins) {
  Harness h;
  // Record says 100ms, policy says 500ms: the record must win.
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1, 100)).ok());
  const MarketView view = h.feed.snapshot();
  const MonoNanos ingest = h.clock.mono_now();
  CHECK(!view.quote("EURUSD", ingest + millis_to_nanos(150), 500).ok());

  // And the other way round: policy 50ms against a record TTL of 100ms.
  CHECK(!view.quote("EURUSD", ingest + millis_to_nanos(60), 50).ok());
  CHECK(view.quote("EURUSD", ingest + millis_to_nanos(40), 50).ok());
}

TEST(unit_market, wall_clock_movement_does_not_affect_freshness) {
  // RI-12 / PB-28: freshness is a monotonic-axis property. A wall-clock
  // rollback must neither expire a good quote nor revive a stale one.
  Harness h;
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1, 500)).ok());
  const MarketView view = h.feed.snapshot();
  const MonoNanos ingest = h.clock.mono_now();

  h.clock.set_utc(0);                       // wall clock jumps to the epoch
  CHECK(view.quote("EURUSD", ingest + 1, 500).ok());

  h.clock.set_utc(4'000'000'000'000'000'000LL);  // and far into the future
  CHECK(!view.quote("EURUSD", ingest + millis_to_nanos(501), 500).ok());
}

TEST(unit_market, an_ingest_timestamp_in_the_future_is_refused) {
  Harness h;
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1)).ok());
  const MarketView view = h.feed.snapshot();
  // Reading "before" the record was ingested means the reading did not come
  // from this process's monotonic clock.
  Result<Quote> q = view.quote("EURUSD", h.clock.mono_now() - 1000, 500);
  CHECK(!q.ok());
  CHECK_EQ(static_cast<int>(q.reason()), static_cast<int>(Reason::ClockRollback));
}

TEST(unit_market, missing_quote_is_reported_as_missing) {
  Harness h;
  const MarketView view = h.feed.snapshot();
  Result<Quote> q = view.quote("EURUSD", h.clock.mono_now(), 500);
  CHECK(!q.ok());
  CHECK_EQ(static_cast<int>(q.reason()), static_cast<int>(Reason::QuoteMissing));
}

TEST(unit_fx, identity_conversion_needs_no_feed) {
  Harness h;
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("USD", "USD", h.clock.mono_now(), 500, {}, 0);
  CHECK(r.ok());
  CHECK_EQ(r.value().upper_bound.num, 1);
  CHECK_EQ(r.value().upper_bound.den, 1);
  CHECK(r.value().kind == FxPathKind::Identity);
}

TEST(unit_fx, direct_edge_uses_the_unfavourable_side) {
  Harness h;
  // 1 JPY buys between 0.0066 and 0.0067 USD. Converting JPY into USD, the
  // conservative bound is the *highest* USD per JPY: the ask.
  CHECK(h.feed.publish_fx(h.fx("JPY", "USD", Rational{66, 10000},
                               Rational{67, 10000}, 1))
            .ok());
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {}, 0);
  CHECK(r.ok());
  CHECK(r.value().kind == FxPathKind::Direct);
  CHECK_EQ(r.value().upper_bound.num, 67);
  CHECK_EQ(r.value().upper_bound.den, 10000);
}

TEST(unit_fx, inverse_edge_divides_by_the_bid) {
  Harness h;
  // Only USD->JPY is published; converting JPY->USD must invert, and the
  // unfavourable direction is the smallest USD->JPY rate, i.e. the bid.
  CHECK(h.feed.publish_fx(h.fx("USD", "JPY", Rational{150, 1}, Rational{151, 1}, 1))
            .ok());
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {}, 0);
  CHECK(r.ok());
  CHECK(r.value().kind == FxPathKind::Inverse);
  CHECK_EQ(r.value().upper_bound.num, 1);
  CHECK_EQ(r.value().upper_bound.den, 150);
}

TEST(unit_fx, a_direct_edge_wins_over_an_inverse_one) {
  Harness h;
  CHECK(h.feed.publish_fx(h.fx("JPY", "USD", Rational{66, 10000},
                               Rational{67, 10000}, 1))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("USD", "JPY", Rational{150, 1}, Rational{151, 1}, 2))
            .ok());
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {}, 0);
  CHECK(r.ok());
  // Pinning the direct edge makes the resolution deterministic instead of
  // dependent on which record happened to be stored (§9.1).
  CHECK(r.value().kind == FxPathKind::Direct);
}

TEST(unit_fx, a_stale_direct_edge_is_not_routed_around) {
  Harness h;
  CHECK(h.feed.publish_fx(h.fx("JPY", "USD", Rational{66, 10000},
                               Rational{67, 10000}, 1, 100))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("JPY", "EUR", Rational{6, 1000}, Rational{7, 1000}, 2))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("EUR", "USD", Rational{11, 10}, Rational{12, 10}, 3))
            .ok());

  const MarketView view = h.feed.snapshot();
  const MonoNanos later = h.clock.mono_now() + millis_to_nanos(200);
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", later, 500, {"EUR"}, 0);
  // Falling back to the pivot would silently change the pinned path.
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()), static_cast<int>(Reason::FxStale));
}

TEST(unit_fx, pivot_path_multiplies_both_legs) {
  Harness h;
  CHECK(h.feed.publish_fx(h.fx("JPY", "EUR", Rational{6, 1000}, Rational{7, 1000}, 1))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("EUR", "USD", Rational{11, 10}, Rational{12, 10}, 2))
            .ok());
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {"EUR"}, 0);
  CHECK(r.ok());
  CHECK(r.value().kind == FxPathKind::Pivot);
  // 7/1000 * 12/10 = 84/10000 = 21/2500
  CHECK_EQ(r.value().upper_bound.num, 21);
  CHECK_EQ(r.value().upper_bound.den, 2500);
}

TEST(unit_fx, two_disagreeing_pivots_are_ambiguous_and_denied) {
  Harness h;
  CHECK(h.feed.publish_fx(h.fx("JPY", "EUR", Rational{6, 1000}, Rational{7, 1000}, 1))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("EUR", "USD", Rational{11, 10}, Rational{12, 10}, 2))
            .ok());
  // Via EUR: 7/1000 * 12/10 = 84/10000. Via GBP: 6/1000 * 15/10 = 90/10000.
  // The two routes disagree, so neither may be picked silently.
  CHECK(h.feed.publish_fx(h.fx("JPY", "GBP", Rational{5, 1000}, Rational{6, 1000}, 3))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("GBP", "USD", Rational{14, 10}, Rational{15, 10}, 4))
            .ok());

  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {"EUR", "GBP"}, 0);
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()),
           static_cast<int>(Reason::FxPathAmbiguous));
}

TEST(unit_fx, two_agreeing_pivots_are_not_ambiguous) {
  Harness h;
  // Both routes give exactly 84/10000, so there is nothing to disagree about.
  CHECK(h.feed.publish_fx(h.fx("JPY", "EUR", Rational{6, 1000}, Rational{7, 1000}, 1))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("EUR", "USD", Rational{11, 10}, Rational{12, 10}, 2))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("JPY", "GBP", Rational{7, 2000}, Rational{7, 2000}, 3))
            .ok());
  CHECK(h.feed.publish_fx(h.fx("GBP", "USD", Rational{24, 10}, Rational{24, 10}, 4))
            .ok());

  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {"EUR", "GBP"}, 0);
  CHECK_MSG(r.ok(), std::string(reason_name(r.reason())));
  CHECK_EQ(r.value().upper_bound.num, 21);
  CHECK_EQ(r.value().upper_bound.den, 2500);
}

TEST(unit_fx, no_path_at_all_is_reported_as_missing) {
  Harness h;
  const MarketView view = h.feed.snapshot();
  Result<FxResolution> r =
      view.fx_upper("JPY", "USD", h.clock.mono_now(), 500, {"EUR"}, 0);
  CHECK(!r.ok());
  CHECK_EQ(static_cast<int>(r.reason()), static_cast<int>(Reason::FxPathMissing));
}

TEST(unit_fx, buffer_only_ever_raises_the_bound) {
  Rational out;
  CHECK(apply_upper_buffer_bps(Rational{1, 1}, 0, out));
  CHECK_EQ(out.num, 1);
  CHECK_EQ(out.den, 1);

  CHECK(apply_upper_buffer_bps(Rational{1, 1}, 50, out));
  // 1 * 10050/10000 = 201/200
  CHECK_EQ(out.num, 201);
  CHECK_EQ(out.den, 200);
  int cmp = 0;
  CHECK(rational_compare(out, Rational{1, 1}, cmp));
  CHECK_EQ(cmp, 1);

  // A negative buffer would lower the bound, so it is refused.
  CHECK(!apply_upper_buffer_bps(Rational{1, 1}, -1, out));
}

TEST(unit_fx, rejects_identity_edges_and_bad_currency_codes) {
  Harness h;
  Status s = h.feed.publish_fx(h.fx("USD", "USD", Rational{1, 1}, Rational{1, 1}, 1));
  CHECK(!s.ok());
  s = h.feed.publish_fx(h.fx("US", "EUR", Rational{1, 1}, Rational{1, 1}, 1));
  CHECK(!s.ok());
  s = h.feed.publish_fx(h.fx("usd", "EUR", Rational{1, 1}, Rational{1, 1}, 1));
  CHECK(!s.ok());
}

TEST(unit_fx, rejects_crossed_and_non_positive_rates) {
  Harness h;
  Status s = h.feed.publish_fx(h.fx("JPY", "USD", Rational{67, 10000},
                                    Rational{66, 10000}, 1));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::QuoteCrossed));

  s = h.feed.publish_fx(h.fx("JPY", "USD", Rational{0, 1}, Rational{1, 1}, 1));
  CHECK(!s.ok());
  CHECK_EQ(static_cast<int>(s.reason()), static_cast<int>(Reason::FxNonPositive));
}

TEST(unit_market, equity_snapshot_freshness_and_validation) {
  Harness h;
  EquitySnapshot e;
  e.account_id = "ACC-A";
  e.currency = "USD";
  e.equity_minor = 1'000'000;
  e.existing_margin_minor = 10'000;
  e.ingest_mono = h.clock.mono_now();
  e.version = 1;
  e.ttl_ms = 1000;
  CHECK(h.feed.publish_equity(e).ok());

  const MarketView view = h.feed.snapshot();
  CHECK(view.equity("ACC-A", h.clock.mono_now(), 1000).ok());
  Result<EquitySnapshot> stale =
      view.equity("ACC-A", h.clock.mono_now() + millis_to_nanos(1000), 1000);
  CHECK(!stale.ok());
  CHECK_EQ(static_cast<int>(stale.reason()), static_cast<int>(Reason::EquityStale));

  Result<EquitySnapshot> missing = view.equity("ACC-Z", h.clock.mono_now(), 1000);
  CHECK(!missing.ok());
  CHECK_EQ(static_cast<int>(missing.reason()),
           static_cast<int>(Reason::EquityMissing));

  e.equity_minor = -1;
  CHECK(!h.feed.publish_equity(e).ok());
}

TEST(unit_market, a_snapshot_is_isolated_from_later_feed_updates) {
  // The decision path takes one view and must not see a concurrent update
  // change the numbers underneath it.
  Harness h;
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 109990, 110000, 1)).ok());
  const MarketView view = h.feed.snapshot();
  CHECK(h.feed.publish_quote(h.quote("EURUSD", 119990, 120000, 2)).ok());

  Result<Quote> from_snapshot = view.quote("EURUSD", h.clock.mono_now(), 500);
  CHECK(from_snapshot.ok());
  CHECK_EQ(from_snapshot.value().ask_ticks, 110000);

  Result<Quote> from_fresh =
      h.feed.snapshot().quote("EURUSD", h.clock.mono_now(), 500);
  CHECK(from_fresh.ok());
  CHECK_EQ(from_fresh.value().ask_ticks, 120000);
}
