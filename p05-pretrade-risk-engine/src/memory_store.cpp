#include "risk/memory_store.hpp"

#include <algorithm>
#include <cstdlib>

#include "risk/hash.hpp"

namespace risk {
namespace {

// Translates a fired failpoint into a commit outcome. `Abort` leaves the
// process immediately so the on-disk journal is observed exactly as it was
// (the out-of-process crash harness relies on this).
bool failpoint_blocks(FailpointRegistry* failpoints, Failpoint fp,
                      CommitOutcome& outcome) {
  if (failpoints == nullptr) return false;
  FailpointRegistry::Hit hit;
  if (!failpoints->reach(fp, hit)) return false;

  switch (hit.action) {
    case FailAction::CommitFail:
      outcome = CommitOutcome::RolledBack;
      return true;
    case FailAction::CommitAckLost:
      // Durable, but the caller never learns that. Signalled by *not* blocking
      // here; the store publishes the state and then reports the lost ack.
      outcome = CommitOutcome::UnknownButDurable;
      return false;
    case FailAction::FenceReject:
      outcome = CommitOutcome::Fenced;
      return true;
    case FailAction::Hook:
      if (hit.hook) hit.hook();
      return false;
    case FailAction::Abort:
      std::_Exit(99);
  }
  return false;
}

}  // namespace

namespace {

// Parses 64 hex characters into 32 bytes. A malformed digest is treated as
// zero, which would show up immediately as a state-hash mismatch rather than
// silently corrupting the running sum.
std::array<std::uint8_t, 32> bytes_from_hex(const std::string& hex) {
  std::array<std::uint8_t, 32> out{};
  if (hex.size() != 64) return out;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::array<std::uint8_t, 32>{};
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

// 256-bit addition and subtraction, little-endian carry over a big-endian
// byte array (index 31 is the least significant byte).
void add_256(std::array<std::uint8_t, 32>& acc,
             const std::array<std::uint8_t, 32>& value) {
  unsigned carry = 0;
  for (int i = 31; i >= 0; --i) {
    const unsigned sum =
        static_cast<unsigned>(acc[static_cast<std::size_t>(i)]) +
        static_cast<unsigned>(value[static_cast<std::size_t>(i)]) + carry;
    acc[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(sum & 0xFFu);
    carry = sum >> 8;
  }
}

void sub_256(std::array<std::uint8_t, 32>& acc,
             const std::array<std::uint8_t, 32>& value) {
  unsigned borrow = 0;
  for (int i = 31; i >= 0; --i) {
    const int diff = static_cast<int>(acc[static_cast<std::size_t>(i)]) -
                     static_cast<int>(value[static_cast<std::size_t>(i)]) -
                     static_cast<int>(borrow);
    if (diff < 0) {
      acc[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(diff + 256);
      borrow = 1;
    } else {
      acc[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(diff);
      borrow = 0;
    }
  }
}

std::string hex_of(const std::array<std::uint8_t, 32>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

}  // namespace

void IncrementalSetDigest::put(const std::string& key,
                               const std::string& digest_hex) {
  const auto it = digests_.find(key);
  if (it != digests_.end()) {
    if (it->second == digest_hex) return;
    sub_256(sum_, bytes_from_hex(it->second));
    it->second = digest_hex;
  } else {
    digests_.emplace(key, digest_hex);
  }
  add_256(sum_, bytes_from_hex(digest_hex));
}

void IncrementalSetDigest::erase(const std::string& key) {
  const auto it = digests_.find(key);
  if (it == digests_.end()) return;
  sub_256(sum_, bytes_from_hex(it->second));
  digests_.erase(it);
}

std::string IncrementalSetDigest::sum_hex() const { return hex_of(sum_); }

std::string IncrementalSetDigest::recomputed_sum_hex() const {
  std::array<std::uint8_t, 32> acc{};
  for (const auto& [key, digest] : digests_) {
    (void)key;
    add_256(acc, bytes_from_hex(digest));
  }
  return hex_of(acc);
}

namespace {

std::string command_digest(const std::string& key, const CommandRecord& c) {
  Hasher h;
  h.named("key", key)
      .named("canonical_hash", c.canonical_hash)
      .named("decision", c.decision)
      .named("reason", reason_name(c.reason))
      .named("reservation", c.reservation_id)
      .named("commit_revision", c.commit_revision)
      .named("policy_version", c.policy_version);
  return h.hex();
}

std::string applied_digest(const std::string& key, const AppliedEventRecord& e) {
  Hasher h;
  h.named("key", key)
      .named("canonical_hash", e.canonical_hash)
      .named("applied_revision", e.applied_revision);
  return h.hex();
}

std::string outbox_digest(const OutboxIntent& o) {
  Hasher h;
  h.named("intent_id", o.intent_id)
      .named("tenant", o.tenant_id)
      .named("account", o.account_id)
      .named("chain", o.order_chain_id)
      .named("command", o.command_id)
      .named("reservation", o.reservation_id)
      .named("kind", o.kind)
      .named("symbol", o.symbol)
      .named("side", side_name(o.side))
      .named("total_units", o.total_units)
      .named("limit_price_ticks", o.limit_price_ticks)
      .named("commit_revision", o.commit_revision)
      .named("canonical_hash", o.canonical_hash);
  h.flag(o.claimed);
  h.flag(o.dispatched);
  return h.hex();
}

}  // namespace

bool transaction_windows_block(FailpointRegistry* failpoints,
                               CommitOutcome& outcome) {
  outcome = CommitOutcome::Committed;
  // In the order of §16.2, so a test arming one window still sees the engine
  // reach the earlier ones first.
  const Failpoint windows[] = {
      Failpoint::AfterLockBeforeBegin,    // CW-05
      Failpoint::AfterCommandInsert,      // CW-06
      Failpoint::AfterReservationInsert,  // CW-07
      Failpoint::AfterProjectionUpdate,   // CW-08
      Failpoint::AfterAuditInsert,        // CW-09
      Failpoint::AfterOutboxInsert,       // CW-10
      Failpoint::AfterRevisionCas,        // CW-11
  };
  for (const Failpoint fp : windows) {
    if (failpoint_blocks(failpoints, fp, outcome)) return true;
  }
  return false;
}

void apply_staged(StoreState& state, const TransactionData& data,
                  Revision new_revision) {
  // The pre-image hash is taken by the store, not supplied by the caller, so
  // the audit chain cannot be forged from outside (§18.2).
  const std::string pre_hash = store_state_hash(state);
  const std::size_t audit_begin = state.audit.size();

  // --- 1. canonical commands --------------------------------------------
  for (const CommandRecord& c : data.commands) {
    CommandRecord stamped = c;
    stamped.commit_revision = new_revision;
    const std::string key = command_key(c.tenant_id, c.account_id, c.command_id);
    state.commands[key] = stamped;
    state.command_digests.put(key, command_digest(key, stamped));
  }

  // --- 2. reservations and the chain index -------------------------------
  for (const Reservation& r : data.reservations) {
    Reservation stamped = r;
    stamped.revision = new_revision;

    // Retire the previous version's contributions before installing the new
    // one, so the derived indexes stay exact across an update.
    const auto previous = state.reservations.find(r.reservation_id);
    if (previous != state.reservations.end()) {
      const Reservation& old = previous->second;
      if (old.holds_budget()) {
        auto& prices = state.live_limit_prices[old.symbol];
        const auto at = prices.find(old.effective_limit_price_ticks());
        if (at != prices.end()) prices.erase(at);
        if (prices.empty()) state.live_limit_prices.erase(old.symbol);
      }
      if (old.state == ReservationState::Unknown ||
          old.state == ReservationState::Quarantine) {
        --state.unresolved_count;
      }
    }
    if (stamped.holds_budget()) {
      state.live_limit_prices[stamped.symbol].insert(
          stamped.effective_limit_price_ticks());
    }
    if (stamped.state == ReservationState::Unknown ||
        stamped.state == ReservationState::Quarantine) {
      ++state.unresolved_count;
    }

    state.reservations[r.reservation_id] = stamped;
    state.reservation_digests.put(r.reservation_id, stamped.digest());
  }
  for (const auto& [key, reservation_id] : data.chain_index) {
    state.chains[key] = reservation_id;
  }

  // --- 3. projection ------------------------------------------------------
  if (data.exposure.has_value()) {
    state.exposure = *data.exposure;
    state.exposure.prune();
  }
  // --- 3b. event dedup keys ----------------------------------------------
  for (const AppliedEventRecord& e : data.applied_events) {
    AppliedEventRecord stamped = e;
    stamped.applied_revision = new_revision;
    const std::string key = event_key(e.tenant_id, e.source_namespace, e.event_id);
    state.applied[key] = stamped;
    state.applied_digests.put(key, applied_digest(key, stamped));
  }

  // --- 4. audit -----------------------------------------------------------
  int ordinal = 0;
  for (const AuditEvent& a : data.audit) {
    AuditEvent stamped = a;
    stamped.commit_revision = new_revision;
    stamped.event_ordinal = ordinal++;
    state.audit.push_back(stamped);
  }

  // --- 5. router outbox ---------------------------------------------------
  for (const OutboxIntent& o : data.outbox_inserts) {
    OutboxIntent stamped = o;
    stamped.commit_revision = new_revision;
    state.outbox_index[stamped.intent_id] = state.outbox.size();
    state.outbox.push_back(stamped);
    state.outbox_digests.put(stamped.intent_id, outbox_digest(stamped));
  }
  for (const auto& [intent_id, flags] : data.outbox_updates) {
    const auto at = state.outbox_index.find(intent_id);
    if (at == state.outbox_index.end()) continue;
    OutboxIntent& o = state.outbox[at->second];
    o.claimed = flags.first;
    o.dispatched = flags.second;
    state.outbox_digests.put(o.intent_id, outbox_digest(o));
  }

  // --- 6. domain metadata and the revision CAS ---------------------------
  for (const auto& [account, flag] : data.account_quarantine) {
    if (flag) {
      state.quarantined_accounts.insert(account);
    } else {
      state.quarantined_accounts.erase(account);
    }
  }
  if (data.tenant_id.has_value()) state.domain.tenant_id = *data.tenant_id;
  if (data.domain_id.has_value()) state.domain.domain_id = *data.domain_id;
  if (data.policy.has_value()) {
    state.policy = *data.policy;
    state.domain.policy_version = state.policy.version;
  }
  if (data.mode.has_value()) state.domain.mode = *data.mode;
  if (data.quarantined.has_value()) state.domain.quarantined = *data.quarantined;
  if (data.admission_open.has_value()) {
    state.domain.admission_open = *data.admission_open;
  }
  state.domain.revision = new_revision;

  // Stamp the audit chain now that the post-image is final.
  const std::string post_hash = store_state_hash(state);
  for (std::size_t i = audit_begin; i < state.audit.size(); ++i) {
    state.audit[i].old_state_hash = pre_hash;
    state.audit[i].new_state_hash = post_hash;
  }
}

std::string store_state_hash(const StoreState& state) {
  Hasher h;
  h.field("risk_store_state_v1");
  h.field(state.domain.digest());
  h.field(state.exposure.state_hash());
  h.field(state.policy.hash());

  h.field("quarantined_accounts");
  for (const AccountId& a : state.quarantined_accounts) h.field(a);

  // Each collection contributes its incremental set digest plus its size, so a
  // commit does not have to re-hash every record (see IncrementalSetDigest).
  h.named("reservations", state.reservation_digests.sum_hex());
  h.field(static_cast<std::uint64_t>(state.reservation_digests.size()));
  h.named("commands", state.command_digests.sum_hex());
  h.field(static_cast<std::uint64_t>(state.command_digests.size()));
  h.named("applied_events", state.applied_digests.sum_hex());
  h.field(static_cast<std::uint64_t>(state.applied_digests.size()));
  h.named("outbox", state.outbox_digests.sum_hex());
  h.field(static_cast<std::uint64_t>(state.outbox_digests.size()));

  return h.hex();
}

std::vector<Reservation> sorted_reservations(const StoreState& state) {
  std::vector<Reservation> out;
  out.reserve(state.reservations.size());
  for (const auto& [_, r] : state.reservations) out.push_back(r);
  std::sort(out.begin(), out.end(), [](const Reservation& a, const Reservation& b) {
    return a.reservation_id < b.reservation_id;
  });
  return out;
}

// ---------------------------------------------------------------------------
// MemoryStore
// ---------------------------------------------------------------------------

MemoryStore::MemoryStore(RiskDomainState initial_domain, RiskPolicy policy,
                         FailpointRegistry* failpoints)
    : failpoints_(failpoints) {
  state_.domain = std::move(initial_domain);
  state_.policy = std::move(policy);
  state_.domain.policy_version = state_.policy.version;
}

RiskDomainState MemoryStore::domain_state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.domain;
}

DomainExposure MemoryStore::exposure() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.exposure;
}

RiskPolicy MemoryStore::policy() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.policy;
}

std::optional<CommandRecord> MemoryStore::find_command(
    const TenantId& tenant, const AccountId& account,
    const CommandId& command) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.commands.find(command_key(tenant, account, command));
  if (it == state_.commands.end()) return std::nullopt;
  return it->second;
}

std::optional<Reservation> MemoryStore::find_reservation(
    const ReservationId& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.reservations.find(id);
  if (it == state_.reservations.end()) return std::nullopt;
  return it->second;
}

std::optional<ReservationId> MemoryStore::find_chain(
    const TenantId& tenant, const AccountId& account,
    const OrderChainId& chain) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.chains.find(chain_key(tenant, account, chain));
  if (it == state_.chains.end()) return std::nullopt;
  return it->second;
}

std::optional<AppliedEventRecord> MemoryStore::find_applied_event(
    const TenantId& tenant, const std::string& ns, const EventId& event) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.applied.find(event_key(tenant, ns, event));
  if (it == state_.applied.end()) return std::nullopt;
  return it->second;
}

std::vector<Reservation> MemoryStore::reservations() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sorted_reservations(state_);
}

std::vector<AuditEvent> MemoryStore::audit() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.audit;
}

std::vector<OutboxIntent> MemoryStore::outbox() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.outbox;
}

bool MemoryStore::account_quarantined(const AccountId& account) const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.quarantined_accounts.count(account) != 0;
}

std::vector<AccountId> MemoryStore::quarantined_accounts() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::vector<AccountId>(state_.quarantined_accounts.begin(),
                                state_.quarantined_accounts.end());
}

std::int64_t MemoryStore::max_live_limit_price(const SymbolId& symbol) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = state_.live_limit_prices.find(symbol);
  if (it == state_.live_limit_prices.end() || it->second.empty()) return 0;
  return *it->second.rbegin();
}

std::int64_t MemoryStore::unresolved_reservation_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.unresolved_count;
}

CommitOutcome MemoryStore::commit(const CommitRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);

  // RI-25: a writer that lost ownership is refused at the storage boundary,
  // not merely asked politely to stop.
  if (request.fencing_token < state_.domain.fencing_token) {
    return CommitOutcome::Fenced;
  }
  // RI-15: the CAS guard. A concurrent commit moves the revision on, so a
  // decision computed against an older snapshot cannot land.
  if (request.expected_revision != state_.domain.revision) {
    return CommitOutcome::RolledBack;
  }

  CommitOutcome outcome = CommitOutcome::Committed;
  if (transaction_windows_block(failpoints_, outcome)) {
    // Nothing was written, so there is nothing to undo.
    return outcome == CommitOutcome::Committed ? CommitOutcome::RolledBack : outcome;
  }

  if (request.fencing_token > state_.domain.fencing_token) {
    state_.domain.fencing_token = request.fencing_token;
  }
  apply_staged(state_, request.data, state_.domain.revision + 1);

  // CW-12: durable, acknowledgement lost.
  CommitOutcome ack = CommitOutcome::Committed;
  failpoint_blocks(failpoints_, Failpoint::AfterDurableCommitBeforeAck, ack);
  if (ack == CommitOutcome::UnknownButDurable) return ack;

  return outcome;
}

std::string MemoryStore::state_hash() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_state_hash(state_);
}

void MemoryStore::install_fencing_token(FencingToken token) {
  std::lock_guard<std::mutex> lock(mu_);
  state_.domain.fencing_token = token;
}

void MemoryStore::seed(const StoreState& state) {
  std::lock_guard<std::mutex> lock(mu_);
  state_ = state;
}

StoreState MemoryStore::snapshot_state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

}  // namespace risk
