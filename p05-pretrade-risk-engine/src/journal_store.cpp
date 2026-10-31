#include "risk/journal_store.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace risk {
namespace {

constexpr std::uint32_t kMagic = 0x5030354u;  // "P05T"
constexpr std::uint32_t kFormatVersion = 1;

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3), table built once.
// ---------------------------------------------------------------------------
const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

// ---------------------------------------------------------------------------
// Byte codec. Little-endian fixed width for integers, length-prefixed strings.
// ---------------------------------------------------------------------------
class Writer {
 public:
  void u8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void boolean(bool v) { u8(v ? 1u : 0u); }
  void str(const std::string& v) {
    u32(static_cast<std::uint32_t>(v.size()));
    buf_.append(v);
  }
  void rational(const Rational& r) {
    i64(r.num);
    i64(r.den);
  }
  void opt_i64(const std::optional<std::int64_t>& v) {
    boolean(v.has_value());
    i64(v.value_or(0));
  }
  void opt_str(const std::optional<std::string>& v) {
    boolean(v.has_value());
    str(v.value_or(std::string{}));
  }

  std::string take() { return std::move(buf_); }

 private:
  std::string buf_;
};

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  bool u8(std::uint8_t& out) {
    if (pos_ + 1 > data_.size()) return fail();
    out = static_cast<std::uint8_t>(data_[pos_++]);
    return true;
  }
  bool u32(std::uint32_t& out) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      std::uint8_t b = 0;
      if (!u8(b)) return false;
      v |= static_cast<std::uint32_t>(b) << (8 * i);
    }
    out = v;
    return true;
  }
  bool u64(std::uint64_t& out) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      std::uint8_t b = 0;
      if (!u8(b)) return false;
      v |= static_cast<std::uint64_t>(b) << (8 * i);
    }
    out = v;
    return true;
  }
  bool i64(std::int64_t& out) {
    std::uint64_t v = 0;
    if (!u64(v)) return false;
    out = static_cast<std::int64_t>(v);
    return true;
  }
  bool boolean(bool& out) {
    std::uint8_t b = 0;
    if (!u8(b)) return false;
    if (b > 1) return fail();
    out = b != 0;
    return true;
  }
  bool str(std::string& out) {
    std::uint32_t len = 0;
    if (!u32(len)) return false;
    // Bound the allocation: a corrupt length must not become a huge malloc.
    if (len > (1u << 24) || pos_ + len > data_.size()) return fail();
    out.assign(data_.substr(pos_, len));
    pos_ += len;
    return true;
  }
  bool rational(Rational& out) {
    if (!i64(out.num) || !i64(out.den)) return false;
    if (out.den <= 0) return fail();
    return true;
  }
  bool opt_i64(std::optional<std::int64_t>& out) {
    bool present = false;
    std::int64_t v = 0;
    if (!boolean(present) || !i64(v)) return false;
    out = present ? std::optional<std::int64_t>(v) : std::nullopt;
    return true;
  }
  bool opt_str(std::optional<std::string>& out) {
    bool present = false;
    std::string v;
    if (!boolean(present) || !str(v)) return false;
    out = present ? std::optional<std::string>(v) : std::nullopt;
    return true;
  }

  bool at_end() const { return pos_ == data_.size(); }
  bool ok() const { return !failed_; }

 private:
  bool fail() {
    failed_ = true;
    return false;
  }

  std::string_view data_;
  std::size_t pos_ = 0;
  bool failed_ = false;
};

// ---------------------------------------------------------------------------
// Struct encoders
// ---------------------------------------------------------------------------

void write_snapshot_ref(Writer& w, const SnapshotRef& s) {
  w.u64(s.policy);
  w.u64(s.instrument);
  w.u64(s.quote);
  w.u64(s.fx);
  w.u64(s.equity);
  w.u64(s.ledger);
}

bool read_snapshot_ref(Reader& r, SnapshotRef& s) {
  return r.u64(s.policy) && r.u64(s.instrument) && r.u64(s.quote) && r.u64(s.fx) &&
         r.u64(s.equity) && r.u64(s.ledger);
}

void write_valuation(Writer& w, const Valuation& v) {
  w.i64(v.risk_price_ticks);
  w.rational(v.fx_upper);
  w.u64(v.quote_version);
  w.u64(v.fx_version);
  w.u8(static_cast<std::uint8_t>(v.fx_path_kind));
  w.str(v.fx_path);
}

bool read_valuation(Reader& r, Valuation& v) {
  std::uint8_t kind = 0;
  if (!r.i64(v.risk_price_ticks) || !r.rational(v.fx_upper) ||
      !r.u64(v.quote_version) || !r.u64(v.fx_version) || !r.u8(kind) ||
      !r.str(v.fx_path)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(FxPathKind::Pivot)) return false;
  v.fx_path_kind = static_cast<FxPathKind>(kind);
  return true;
}

void write_instrument(Writer& w, const InstrumentSpec& s) {
  w.str(s.symbol);
  w.str(s.base_currency);
  w.str(s.quote_currency);
  w.i64(s.quantity_step_base_units);
  w.i64(s.min_qty_base_units);
  w.i64(s.max_qty_base_units);
  w.rational(s.tick_size);
  w.rational(s.contract_multiplier);
  w.i64(s.quote_ttl_ms);
  w.i64(s.risk_price_upper_buffer_bps);
  w.i64(s.max_price_deviation_bps);
  w.u8(static_cast<std::uint8_t>(s.product_mode));
  w.boolean(s.enabled);
  w.u64(s.version);
}

bool read_instrument(Reader& r, InstrumentSpec& s) {
  std::uint8_t mode = 0;
  if (!r.str(s.symbol) || !r.str(s.base_currency) || !r.str(s.quote_currency) ||
      !r.i64(s.quantity_step_base_units) || !r.i64(s.min_qty_base_units) ||
      !r.i64(s.max_qty_base_units) || !r.rational(s.tick_size) ||
      !r.rational(s.contract_multiplier) || !r.i64(s.quote_ttl_ms) ||
      !r.i64(s.risk_price_upper_buffer_bps) || !r.i64(s.max_price_deviation_bps) ||
      !r.u8(mode) || !r.boolean(s.enabled) || !r.u64(s.version)) {
    return false;
  }
  if (mode != static_cast<std::uint8_t>(ProductMode::SpotCfd)) return false;
  s.product_mode = ProductMode::SpotCfd;
  return true;
}

void write_policy(Writer& w, const RiskPolicy& p) {
  w.u64(p.version);
  w.u8(static_cast<std::uint8_t>(p.mode));
  w.str(p.account_mode);
  w.str(p.account_currency.code);
  w.i64(p.account_currency.minor_scale);
  w.i64(p.fx_ttl_ms);
  w.i64(p.equity_ttl_ms);
  w.i64(p.fx_upper_buffer_bps);

  w.u32(static_cast<std::uint32_t>(p.fx_pivot_currencies.size()));
  for (const std::string& c : p.fx_pivot_currencies) w.str(c);

  w.u32(static_cast<std::uint32_t>(p.instruments.size()));
  for (const auto& [_, spec] : p.instruments) write_instrument(w, spec);

  w.u32(static_cast<std::uint32_t>(p.accounts.size()));
  for (const auto& [_, a] : p.accounts) {
    w.str(a.account_id);
    w.i64(a.margin_budget_minor);
    w.boolean(a.use_equity_feed);
    w.boolean(a.enabled);
  }

  w.i64(p.limits.max_order_notional_minor);
  w.i64(p.limits.max_chain_qty_base_units);
  w.i64(p.limits.max_account_symbol_net_minor);
  w.i64(p.limits.max_account_symbol_gross_minor);
  w.i64(p.limits.max_account_total_gross_minor);
  w.i64(p.limits.max_firm_gross_minor);
  w.i64(p.limits.min_margin_buffer_minor);
  w.i64(p.limits.max_working_orders_per_account);
  w.i64(p.limits.max_working_orders_per_account_symbol);
  w.i64(p.limits.max_admissions_per_second);

  w.rational(p.margin_proxy_rate);

  w.i64(p.queues.admission_cap);
  w.i64(p.queues.event_cap);
  w.i64(p.queues.recon_cap);

  w.boolean(p.safety.require_atomic_outbox);
  w.boolean(p.safety.abort_on_stale_data);
  w.boolean(p.safety.allow_market_orders);
  w.boolean(p.safety.allow_reduce_only_new);
  w.boolean(p.safety.auto_release_on_timeout);
  w.i64(p.safety.max_pending_modify_per_chain);
}

bool read_policy(Reader& r, RiskPolicy& p) {
  std::uint8_t mode = 0;
  if (!r.u64(p.version) || !r.u8(mode) || !r.str(p.account_mode) ||
      !r.str(p.account_currency.code)) {
    return false;
  }
  if (mode > static_cast<std::uint8_t>(DomainMode::ReconOnly)) return false;
  p.mode = static_cast<DomainMode>(mode);

  std::int64_t scale = 0;
  if (!r.i64(scale)) return false;
  if (scale < 0 || scale > 8) return false;
  p.account_currency.minor_scale = static_cast<int>(scale);

  if (!r.i64(p.fx_ttl_ms) || !r.i64(p.equity_ttl_ms) || !r.i64(p.fx_upper_buffer_bps)) {
    return false;
  }

  std::uint32_t count = 0;
  if (!r.u32(count)) return false;
  p.fx_pivot_currencies.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string c;
    if (!r.str(c)) return false;
    p.fx_pivot_currencies.push_back(c);
  }

  if (!r.u32(count)) return false;
  p.instruments.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    InstrumentSpec s;
    if (!read_instrument(r, s)) return false;
    p.instruments[s.symbol] = s;
  }

  if (!r.u32(count)) return false;
  p.accounts.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    AccountProfile a;
    if (!r.str(a.account_id) || !r.i64(a.margin_budget_minor) ||
        !r.boolean(a.use_equity_feed) || !r.boolean(a.enabled)) {
      return false;
    }
    p.accounts[a.account_id] = a;
  }

  if (!r.i64(p.limits.max_order_notional_minor) ||
      !r.i64(p.limits.max_chain_qty_base_units) ||
      !r.i64(p.limits.max_account_symbol_net_minor) ||
      !r.i64(p.limits.max_account_symbol_gross_minor) ||
      !r.i64(p.limits.max_account_total_gross_minor) ||
      !r.i64(p.limits.max_firm_gross_minor) ||
      !r.i64(p.limits.min_margin_buffer_minor) ||
      !r.i64(p.limits.max_working_orders_per_account) ||
      !r.i64(p.limits.max_working_orders_per_account_symbol) ||
      !r.i64(p.limits.max_admissions_per_second)) {
    return false;
  }
  if (!r.rational(p.margin_proxy_rate)) return false;
  if (!r.i64(p.queues.admission_cap) || !r.i64(p.queues.event_cap) ||
      !r.i64(p.queues.recon_cap)) {
    return false;
  }
  if (!r.boolean(p.safety.require_atomic_outbox) ||
      !r.boolean(p.safety.abort_on_stale_data) ||
      !r.boolean(p.safety.allow_market_orders) ||
      !r.boolean(p.safety.allow_reduce_only_new) ||
      !r.boolean(p.safety.auto_release_on_timeout) ||
      !r.i64(p.safety.max_pending_modify_per_chain)) {
    return false;
  }

  p.order_types = {OrderType::Limit};
  p.time_in_force = {TimeInForce::Day};
  return true;
}

void write_reservation(Writer& w, const Reservation& v) {
  w.str(v.reservation_id);
  w.str(v.tenant_id);
  w.str(v.domain_id);
  w.str(v.account_id);
  w.str(v.order_chain_id);
  w.str(v.command_id);
  w.str(v.symbol);
  w.u8(static_cast<std::uint8_t>(v.side));
  w.i64(v.accepted_total_units);
  w.i64(v.cum_units);
  w.i64(v.accepted_limit_price_ticks);
  w.opt_i64(v.proposed_limit_price_ticks);
  w.opt_i64(v.proposed_total_units);
  w.opt_str(v.pending_modify_command_id);

  w.boolean(v.proposed_valuation.has_value());
  write_valuation(w, v.proposed_valuation.value_or(Valuation{}));

  w.boolean(v.pending_cancel);
  w.opt_str(v.pending_cancel_command_id);
  w.u8(static_cast<std::uint8_t>(v.state));
  w.u64(v.policy_version);
  w.str(v.policy_hash);
  write_valuation(w, v.accepted_valuation);
  w.u64(v.revision);
  w.u32(static_cast<std::uint32_t>(v.terminal_reason));
  w.str(v.note);
}

bool read_reservation(Reader& r, Reservation& v) {
  std::uint8_t side = 0, state = 0;
  std::uint32_t reason = 0;
  bool has_proposed_valuation = false;
  Valuation proposed;

  if (!r.str(v.reservation_id) || !r.str(v.tenant_id) || !r.str(v.domain_id) ||
      !r.str(v.account_id) || !r.str(v.order_chain_id) || !r.str(v.command_id) ||
      !r.str(v.symbol) || !r.u8(side) || !r.i64(v.accepted_total_units) ||
      !r.i64(v.cum_units) || !r.i64(v.accepted_limit_price_ticks) ||
      !r.opt_i64(v.proposed_limit_price_ticks) ||
      !r.opt_i64(v.proposed_total_units) ||
      !r.opt_str(v.pending_modify_command_id) ||
      !r.boolean(has_proposed_valuation) || !read_valuation(r, proposed) ||
      !r.boolean(v.pending_cancel) || !r.opt_str(v.pending_cancel_command_id) ||
      !r.u8(state) || !r.u64(v.policy_version) || !r.str(v.policy_hash) ||
      !read_valuation(r, v.accepted_valuation) || !r.u64(v.revision) ||
      !r.u32(reason) || !r.str(v.note)) {
    return false;
  }
  if (side != 1 && side != 2) return false;
  if (state > static_cast<std::uint8_t>(ReservationState::Quarantine)) return false;
  v.side = static_cast<Side>(side);
  v.state = static_cast<ReservationState>(state);
  v.terminal_reason = static_cast<Reason>(reason);
  v.proposed_valuation =
      has_proposed_valuation ? std::optional<Valuation>(proposed) : std::nullopt;
  return true;
}

void write_exposure(Writer& w, const DomainExposure& e) {
  w.u32(static_cast<std::uint32_t>(e.accounts.size()));
  for (const auto& [account, ae] : e.accounts) {
    w.str(account);
    w.u32(static_cast<std::uint32_t>(ae.symbols.size()));
    for (const auto& [symbol, se] : ae.symbols) {
      w.str(symbol);
      w.i64(se.position_units);
      w.i64(se.pending_buy_units);
      w.i64(se.pending_sell_units);
      w.i64(se.working_orders);
    }
  }
}

bool read_exposure(Reader& r, DomainExposure& e) {
  std::uint32_t accounts = 0;
  if (!r.u32(accounts)) return false;
  e.accounts.clear();
  for (std::uint32_t i = 0; i < accounts; ++i) {
    std::string account;
    std::uint32_t symbols = 0;
    if (!r.str(account) || !r.u32(symbols)) return false;
    AccountExposure ae;
    for (std::uint32_t j = 0; j < symbols; ++j) {
      std::string symbol;
      SymbolExposure se;
      if (!r.str(symbol) || !r.i64(se.position_units) ||
          !r.i64(se.pending_buy_units) || !r.i64(se.pending_sell_units) ||
          !r.i64(se.working_orders)) {
        return false;
      }
      if (!se.structurally_valid()) return false;
      ae.symbols[symbol] = se;
    }
    e.accounts[account] = std::move(ae);
  }
  return true;
}

void write_audit(Writer& w, const AuditEvent& a) {
  w.str(a.tenant_id);
  w.str(a.domain_id);
  w.u64(a.commit_revision);
  w.i64(a.event_ordinal);
  w.str(a.event_type);
  w.str(a.command_id);
  w.str(a.reservation_id);
  w.str(a.account_id);
  w.str(a.symbol);
  w.str(a.source_event_id);
  w.u64(a.policy_version);
  write_snapshot_ref(w, a.snapshots);
  w.str(a.payload_hash);
  w.str(a.old_state_hash);
  w.str(a.new_state_hash);
  w.u32(static_cast<std::uint32_t>(a.reason));
  w.str(a.actor);
  w.str(a.note);
  w.i64(a.commit_utc);
  w.i64(a.commit_mono);
  w.str(a.component_version);
}

bool read_audit(Reader& r, AuditEvent& a) {
  std::int64_t ordinal = 0;
  std::uint32_t reason = 0;
  if (!r.str(a.tenant_id) || !r.str(a.domain_id) || !r.u64(a.commit_revision) ||
      !r.i64(ordinal) || !r.str(a.event_type) || !r.str(a.command_id) ||
      !r.str(a.reservation_id) || !r.str(a.account_id) || !r.str(a.symbol) ||
      !r.str(a.source_event_id) || !r.u64(a.policy_version) ||
      !read_snapshot_ref(r, a.snapshots) || !r.str(a.payload_hash) ||
      !r.str(a.old_state_hash) || !r.str(a.new_state_hash) || !r.u32(reason) ||
      !r.str(a.actor) || !r.str(a.note) || !r.i64(a.commit_utc) ||
      !r.i64(a.commit_mono) || !r.str(a.component_version)) {
    return false;
  }
  a.event_ordinal = static_cast<int>(ordinal);
  a.reason = static_cast<Reason>(reason);
  return true;
}

void write_outbox(Writer& w, const OutboxIntent& o) {
  w.str(o.intent_id);
  w.str(o.tenant_id);
  w.str(o.account_id);
  w.str(o.order_chain_id);
  w.str(o.command_id);
  w.str(o.reservation_id);
  w.str(o.kind);
  w.str(o.symbol);
  w.u8(static_cast<std::uint8_t>(o.side));
  w.i64(o.total_units);
  w.i64(o.limit_price_ticks);
  w.u64(o.commit_revision);
  w.str(o.canonical_hash);
  w.boolean(o.claimed);
  w.boolean(o.dispatched);
}

bool read_outbox(Reader& r, OutboxIntent& o) {
  std::uint8_t side = 0;
  if (!r.str(o.intent_id) || !r.str(o.tenant_id) || !r.str(o.account_id) ||
      !r.str(o.order_chain_id) || !r.str(o.command_id) || !r.str(o.reservation_id) ||
      !r.str(o.kind) || !r.str(o.symbol) || !r.u8(side) || !r.i64(o.total_units) ||
      !r.i64(o.limit_price_ticks) || !r.u64(o.commit_revision) ||
      !r.str(o.canonical_hash) || !r.boolean(o.claimed) || !r.boolean(o.dispatched)) {
    return false;
  }
  if (side != 1 && side != 2) return false;
  o.side = static_cast<Side>(side);
  return true;
}

}  // namespace

std::uint32_t crc32_of(std::string_view data) {
  const auto& table = crc_table();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char ch : data) {
    const std::uint8_t byte = static_cast<std::uint8_t>(ch);
    crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::string encode_transaction(Revision revision, FencingToken token,
                               const TransactionData& data) {
  Writer w;
  w.u32(kFormatVersion);
  w.u64(revision);
  w.u64(token);

  w.u32(static_cast<std::uint32_t>(data.commands.size()));
  for (const CommandRecord& c : data.commands) {
    w.str(c.tenant_id);
    w.str(c.account_id);
    w.str(c.command_id);
    w.str(c.canonical_hash);
    w.str(c.decision);
    w.u32(static_cast<std::uint32_t>(c.reason));
    w.str(c.reservation_id);
    w.u64(c.commit_revision);
    w.u64(c.policy_version);
  }

  w.u32(static_cast<std::uint32_t>(data.reservations.size()));
  for (const Reservation& v : data.reservations) write_reservation(w, v);

  w.u32(static_cast<std::uint32_t>(data.applied_events.size()));
  for (const AppliedEventRecord& e : data.applied_events) {
    w.str(e.tenant_id);
    w.str(e.source_namespace);
    w.str(e.event_id);
    w.str(e.canonical_hash);
    w.u64(e.applied_revision);
  }

  w.u32(static_cast<std::uint32_t>(data.audit.size()));
  for (const AuditEvent& a : data.audit) write_audit(w, a);

  w.u32(static_cast<std::uint32_t>(data.outbox_inserts.size()));
  for (const OutboxIntent& o : data.outbox_inserts) write_outbox(w, o);

  w.u32(static_cast<std::uint32_t>(data.outbox_updates.size()));
  for (const auto& [id, flags] : data.outbox_updates) {
    w.str(id);
    w.boolean(flags.first);
    w.boolean(flags.second);
  }

  w.opt_str(data.tenant_id);
  w.opt_str(data.domain_id);

  w.boolean(data.exposure.has_value());
  write_exposure(w, data.exposure.value_or(DomainExposure{}));

  w.boolean(data.policy.has_value());
  write_policy(w, data.policy.value_or(RiskPolicy{}));

  w.boolean(data.mode.has_value());
  w.u8(static_cast<std::uint8_t>(data.mode.value_or(DomainMode::Normal)));
  w.boolean(data.quarantined.has_value());
  w.boolean(data.quarantined.value_or(false));
  w.boolean(data.admission_open.has_value());
  w.boolean(data.admission_open.value_or(false));

  w.u32(static_cast<std::uint32_t>(data.chain_index.size()));
  for (const auto& [key, id] : data.chain_index) {
    w.str(key);
    w.str(id);
  }

  w.u32(static_cast<std::uint32_t>(data.account_quarantine.size()));
  for (const auto& [account, flag] : data.account_quarantine) {
    w.str(account);
    w.boolean(flag);
  }

  return w.take();
}

Result<TransactionData> decode_transaction(std::string_view payload,
                                           Revision& revision,
                                           FencingToken& token) {
  Reader r(payload);
  std::uint32_t format = 0;
  if (!r.u32(format) || format != kFormatVersion) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt,
                                         "unsupported journal format");
  }
  if (!r.u64(revision) || !r.u64(token)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "header");
  }

  TransactionData data;
  std::uint32_t count = 0;

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "commands");
  for (std::uint32_t i = 0; i < count; ++i) {
    CommandRecord c;
    std::uint32_t reason = 0;
    if (!r.str(c.tenant_id) || !r.str(c.account_id) || !r.str(c.command_id) ||
        !r.str(c.canonical_hash) || !r.str(c.decision) || !r.u32(reason) ||
        !r.str(c.reservation_id) || !r.u64(c.commit_revision) ||
        !r.u64(c.policy_version)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "command record");
    }
    c.reason = static_cast<Reason>(reason);
    data.commands.push_back(std::move(c));
  }

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "reservations");
  for (std::uint32_t i = 0; i < count; ++i) {
    Reservation v;
    if (!read_reservation(r, v)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "reservation");
    }
    data.reservations.push_back(std::move(v));
  }

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "events");
  for (std::uint32_t i = 0; i < count; ++i) {
    AppliedEventRecord e;
    if (!r.str(e.tenant_id) || !r.str(e.source_namespace) || !r.str(e.event_id) ||
        !r.str(e.canonical_hash) || !r.u64(e.applied_revision)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "applied event");
    }
    data.applied_events.push_back(std::move(e));
  }

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "audit");
  for (std::uint32_t i = 0; i < count; ++i) {
    AuditEvent a;
    if (!read_audit(r, a)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "audit event");
    }
    data.audit.push_back(std::move(a));
  }

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "outbox");
  for (std::uint32_t i = 0; i < count; ++i) {
    OutboxIntent o;
    if (!read_outbox(r, o)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "outbox intent");
    }
    data.outbox_inserts.push_back(std::move(o));
  }

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "outbox updates");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string id;
    bool claimed = false, dispatched = false;
    if (!r.str(id) || !r.boolean(claimed) || !r.boolean(dispatched)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "outbox update");
    }
    data.outbox_updates.emplace_back(id, std::make_pair(claimed, dispatched));
  }

  if (!r.opt_str(data.tenant_id) || !r.opt_str(data.domain_id)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "identity");
  }

  bool present = false;
  DomainExposure exposure;
  if (!r.boolean(present) || !read_exposure(r, exposure)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "exposure");
  }
  if (present) data.exposure = exposure;

  RiskPolicy policy;
  if (!r.boolean(present) || !read_policy(r, policy)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "policy");
  }
  if (present) data.policy = policy;

  std::uint8_t mode = 0;
  bool flag = false;
  if (!r.boolean(present) || !r.u8(mode)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "mode");
  }
  if (mode > static_cast<std::uint8_t>(DomainMode::ReconOnly)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "mode value");
  }
  if (present) data.mode = static_cast<DomainMode>(mode);

  if (!r.boolean(present) || !r.boolean(flag)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "quarantine");
  }
  if (present) data.quarantined = flag;

  if (!r.boolean(present) || !r.boolean(flag)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "admission");
  }
  if (present) data.admission_open = flag;

  if (!r.u32(count)) return Result<TransactionData>::fail(Reason::JournalCorrupt, "chains");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key, id;
    if (!r.str(key) || !r.str(id)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt, "chain index");
    }
    data.chain_index.emplace_back(key, id);
  }

  if (!r.u32(count)) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "account quarantine");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string account;
    bool quarantined = false;
    if (!r.str(account) || !r.boolean(quarantined)) {
      return Result<TransactionData>::fail(Reason::JournalCorrupt,
                                           "account quarantine entry");
    }
    data.account_quarantine.emplace_back(account, quarantined);
  }

  if (!r.ok() || !r.at_end()) {
    return Result<TransactionData>::fail(Reason::JournalCorrupt, "trailing bytes");
  }
  return Result<TransactionData>(data);
}

// ---------------------------------------------------------------------------
// JournalStore
// ---------------------------------------------------------------------------

JournalStore::JournalStore(std::string path, FailpointRegistry* failpoints)
    : path_(std::move(path)), failpoints_(failpoints) {}

JournalStore::~JournalStore() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

Result<std::unique_ptr<JournalStore>> JournalStore::open(
    std::string path, const RiskDomainState& initial_domain,
    const RiskPolicy& initial_policy, FailpointRegistry* failpoints) {
  if (const Status s = initial_policy.validate(); !s) {
    return Result<std::unique_ptr<JournalStore>>::fail(s.reason(), s.detail());
  }

  auto store = std::unique_ptr<JournalStore>(new JournalStore(path, failpoints));
  store->state_.domain = initial_domain;
  store->state_.policy = initial_policy;
  store->state_.domain.policy_version = initial_policy.version;

  // "r+b" fails when the file does not exist, which is how a fresh journal is
  // distinguished from an existing one without a separate stat() race.
  std::FILE* existing = std::fopen(path.c_str(), "r+b");
  if (existing == nullptr) {
    std::FILE* created = std::fopen(path.c_str(), "w+b");
    if (created == nullptr) {
      return Result<std::unique_ptr<JournalStore>>::fail(
          Reason::PersistenceUnavailable, "cannot create " + path);
    }
    store->file_ = created;
    store->stats_.created = true;

    // Bootstrap record: pins tenant/domain/policy so a later open can
    // cross-check rather than inherit whatever the caller passed.
    TransactionData boot;
    boot.tenant_id = initial_domain.tenant_id;
    boot.domain_id = initial_domain.domain_id;
    boot.policy = initial_policy;
    boot.mode = initial_domain.mode;
    boot.quarantined = initial_domain.quarantined;
    boot.admission_open = initial_domain.admission_open;
    const std::string payload =
        encode_transaction(0, initial_domain.fencing_token, boot);
    if (const Status s = store->append_record(payload); !s) {
      return Result<std::unique_ptr<JournalStore>>::fail(s.reason(), s.detail());
    }
    return Result<std::unique_ptr<JournalStore>>(std::move(store));
  }

  store->file_ = existing;
  if (const Status s = store->replay(); !s) {
    return Result<std::unique_ptr<JournalStore>>::fail(s.reason(), s.detail());
  }
  if (store->state_.domain.tenant_id != initial_domain.tenant_id ||
      store->state_.domain.domain_id != initial_domain.domain_id) {
    return Result<std::unique_ptr<JournalStore>>::fail(
        Reason::SnapshotMismatch,
        "journal belongs to a different tenant/domain");
  }
  return Result<std::unique_ptr<JournalStore>>(std::move(store));
}

Status JournalStore::replay() {
  if (std::fseek(file_, 0, SEEK_END) != 0) {
    return Status::fail(Reason::PersistenceUnavailable, "seek end");
  }
  const long size = std::ftell(file_);
  if (size < 0) return Status::fail(Reason::PersistenceUnavailable, "ftell");
  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    return Status::fail(Reason::PersistenceUnavailable, "seek start");
  }

  std::string blob;
  blob.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t read =
        std::fread(blob.data(), 1, static_cast<std::size_t>(size), file_);
    if (read != static_cast<std::size_t>(size)) {
      return Status::fail(Reason::PersistenceUnavailable, "short read");
    }
  }

  std::size_t pos = 0;
  std::uint64_t applied = 0;

  // The identity is taken from the journal, not from the caller: the point of
  // the check in open() is to catch a file that belongs to another domain, and
  // restoring the caller's expectation afterwards would make it vacuous.
  state_.domain.tenant_id.clear();
  state_.domain.domain_id.clear();

  while (pos + 12 <= blob.size()) {
    Reader header(std::string_view(blob).substr(pos, 12));
    std::uint32_t magic = 0, len = 0, crc = 0;
    if (!header.u32(magic) || !header.u32(len) || !header.u32(crc)) break;
    if (magic != kMagic) {
      // Not a record boundary: the rest of the file is garbage, not a record
      // we are allowed to guess at.
      break;
    }
    if (len > (1u << 26) || pos + 12 + len > blob.size()) break;  // torn tail

    const std::string_view payload =
        std::string_view(blob).substr(pos + 12, len);
    if (crc32_of(payload) != crc) break;  // torn or corrupt record

    Revision revision = 0;
    FencingToken token = 0;
    Result<TransactionData> decoded = decode_transaction(payload, revision, token);
    if (!decoded.ok()) {
      return Status::fail(Reason::JournalCorrupt,
                          "record " + std::to_string(applied) + ": " +
                              decoded.detail());
    }

    if (token > state_.domain.fencing_token) state_.domain.fencing_token = token;
    // Replay never fires failpoints: recovery must be deterministic.
    apply_staged(state_, decoded.value(), revision);

    ++applied;
    pos += 12 + len;
  }

  stats_.records_applied = applied;
  stats_.bytes_read = pos;
  stats_.truncated_tail_bytes = blob.size() - pos;

  // Position the handle at the end of the last good record so the next append
  // overwrites any torn tail instead of appending after it.
  if (std::fseek(file_, static_cast<long>(pos), SEEK_SET) != 0) {
    return Status::fail(Reason::PersistenceUnavailable, "seek to tail");
  }
  return Status::success();
}

Status JournalStore::append_record(const std::string& payload) {
  if (file_ == nullptr) {
    return Status::fail(Reason::PersistenceUnavailable, "journal closed");
  }
  Writer header;
  header.u32(kMagic);
  header.u32(static_cast<std::uint32_t>(payload.size()));
  header.u32(crc32_of(payload));
  const std::string head = header.take();

  if (std::fwrite(head.data(), 1, head.size(), file_) != head.size()) {
    return Status::fail(Reason::PersistenceCommitFailed, "header write");
  }
  if (std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
    return Status::fail(Reason::PersistenceCommitFailed, "payload write");
  }
  if (std::fflush(file_) != 0) {
    return Status::fail(Reason::PersistenceCommitFailed, "flush");
  }
#ifdef _WIN32
  if (_commit(_fileno(file_)) != 0) {
    return Status::fail(Reason::PersistenceCommitFailed, "commit");
  }
#else
  if (fsync(fileno(file_)) != 0) {
    return Status::fail(Reason::PersistenceCommitFailed, "fsync");
  }
#endif
  return Status::success();
}

RiskDomainState JournalStore::domain_state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.domain;
}

DomainExposure JournalStore::exposure() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.exposure;
}

RiskPolicy JournalStore::policy() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.policy;
}

std::optional<CommandRecord> JournalStore::find_command(
    const TenantId& tenant, const AccountId& account,
    const CommandId& command) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.commands.find(command_key(tenant, account, command));
  if (it == state_.commands.end()) return std::nullopt;
  return it->second;
}

std::optional<Reservation> JournalStore::find_reservation(
    const ReservationId& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.reservations.find(id);
  if (it == state_.reservations.end()) return std::nullopt;
  return it->second;
}

std::optional<ReservationId> JournalStore::find_chain(
    const TenantId& tenant, const AccountId& account,
    const OrderChainId& chain) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.chains.find(chain_key(tenant, account, chain));
  if (it == state_.chains.end()) return std::nullopt;
  return it->second;
}

std::optional<AppliedEventRecord> JournalStore::find_applied_event(
    const TenantId& tenant, const std::string& ns, const EventId& event) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.applied.find(event_key(tenant, ns, event));
  if (it == state_.applied.end()) return std::nullopt;
  return it->second;
}

std::vector<Reservation> JournalStore::reservations() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sorted_reservations(state_);
}

std::vector<AuditEvent> JournalStore::audit() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.audit;
}

std::vector<OutboxIntent> JournalStore::outbox() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.outbox;
}

bool JournalStore::account_quarantined(const AccountId& account) const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.quarantined_accounts.count(account) != 0;
}

std::vector<AccountId> JournalStore::quarantined_accounts() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::vector<AccountId>(state_.quarantined_accounts.begin(),
                                state_.quarantined_accounts.end());
}

std::int64_t JournalStore::max_live_limit_price(const SymbolId& symbol) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.live_limit_prices.find(symbol);
  if (it == state_.live_limit_prices.end() || it->second.empty()) return 0;
  return *it->second.rbegin();
}

std::int64_t JournalStore::unresolved_reservation_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.unresolved_count;
}

CommitOutcome JournalStore::commit(const CommitRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);

  if (request.fencing_token < state_.domain.fencing_token) {
    return CommitOutcome::Fenced;
  }
  if (request.expected_revision != state_.domain.revision) {
    return CommitOutcome::RolledBack;
  }

  // The in-transaction crash windows are evaluated before anything is written
  // or appended, so a blocked transaction leaves neither memory nor the device
  // touched.
  CommitOutcome outcome = CommitOutcome::Committed;
  if (transaction_windows_block(failpoints_, outcome)) {
    return outcome == CommitOutcome::Committed ? CommitOutcome::RolledBack : outcome;
  }

  const Revision new_revision = state_.domain.revision + 1;
  const FencingToken token =
      std::max(request.fencing_token, state_.domain.fencing_token);

  // Durability boundary: the record is on the device before the in-memory state
  // is published, so a crash can never leave memory ahead of the journal.
  const std::string payload = encode_transaction(new_revision, token, request.data);
  if (const Status s = append_record(payload); !s) {
    return CommitOutcome::RolledBack;
  }

  state_.domain.fencing_token = token;
  apply_staged(state_, request.data, new_revision);

  CommitOutcome ack = CommitOutcome::Committed;
  FailpointRegistry::Hit hit;
  if (failpoints_ != nullptr &&
      failpoints_->reach(Failpoint::AfterDurableCommitBeforeAck, hit)) {
    switch (hit.action) {
      case FailAction::CommitAckLost:
        ack = CommitOutcome::UnknownButDurable;
        break;
      case FailAction::Abort:
        std::_Exit(99);
      case FailAction::Hook:
        if (hit.hook) hit.hook();
        break;
      default:
        break;
    }
  }
  if (ack == CommitOutcome::UnknownButDurable) return ack;
  return outcome;
}

std::string JournalStore::state_hash() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_state_hash(state_);
}

void JournalStore::install_fencing_token(FencingToken token) {
  std::lock_guard<std::mutex> lock(mu_);
  if (token <= state_.domain.fencing_token) return;
  state_.domain.fencing_token = token;
  // A leadership change is itself durable: otherwise a new owner could install
  // a token, die before committing anything, and let the previous owner's
  // stale token look current again after restart (RI-25, PB-21). The record
  // carries the unchanged revision, so it fences without advancing the ledger.
  const std::string payload =
      encode_transaction(state_.domain.revision, token, TransactionData{});
  (void)append_record(payload);
}

void JournalStore::close_abruptly() {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace risk
