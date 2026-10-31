// In-memory store ("IN_MEMORY_LAB" per §27) plus the shared staged-apply
// routine used by the durable store as well.
//
// The apply order mirrors §16.2 exactly, and a failpoint sits between each
// stage. Because the stages are applied to a *working copy* that is only
// published at the end, a failpoint firing mid-way rolls the whole transaction
// back -- which is what CW-06 through CW-11 require.
#pragma once

#include <array>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "risk/failpoint.hpp"
#include "risk/store.hpp"

namespace risk {

// An incremental digest over a keyed set of records.
//
// The state hash is taken on *every* commit, so hashing all N records each time
// would make a commit O(N) in SHA-256 operations and the whole workload
// quadratic (§25.1 explicitly rules that out). Instead each record's digest is
// cached and the set is summarised by the sum of those digests modulo 2^256 --
// the standard additive multiset hash. Adding, replacing or removing one
// record is then O(1) hashes.
//
// The trade-off is deliberate and worth stating: an additive set digest is
// order-independent and incremental, but its collision resistance is weaker
// than a single SHA-256 over the whole serialised set, because sums are
// malleable. It is used here as a *divergence detector* -- proving that a
// replayed journal reproduces the live state, and that no mutation went
// unaudited -- not as a commitment that resists an adversary who controls
// record contents. Integrity against a tampered journal comes from the
// per-record CRC and the all-or-nothing record framing instead.
class IncrementalSetDigest {
 public:
  // Inserts or replaces the record under `key`, whose canonical digest is
  // `digest_hex`.
  void put(const std::string& key, const std::string& digest_hex);
  void erase(const std::string& key);

  std::size_t size() const { return digests_.size(); }
  // Hex of the 256-bit sum. Equal sums plus equal sizes mean equal sets.
  std::string sum_hex() const;

  // Recomputes the sum from the cached digests. Used by the self-check that
  // the incremental path agrees with a from-scratch computation.
  std::string recomputed_sum_hex() const;

 private:
  std::array<std::uint8_t, 32> sum_{};
  std::map<std::string, std::string> digests_;
};

// The complete committed state of one risk domain.
struct StoreState {
  RiskDomainState domain;
  RiskPolicy policy;
  DomainExposure exposure;

  std::map<std::string, CommandRecord> commands;      // command_key -> record
  std::map<ReservationId, Reservation> reservations;
  std::map<std::string, ReservationId> chains;        // chain_key -> reservation
  std::map<std::string, AppliedEventRecord> applied;  // event_key -> record
  std::vector<AuditEvent> audit;
  std::vector<OutboxIntent> outbox;
  std::set<AccountId> quarantined_accounts;

  // Derived, not authoritative: rebuilt by the same apply_staged path during
  // replay, so a reopened journal produces identical values.
  IncrementalSetDigest reservation_digests;
  IncrementalSetDigest command_digests;
  IncrementalSetDigest applied_digests;
  IncrementalSetDigest outbox_digests;

  // Revaluation needs the highest raw limit price across the live reservations
  // on a symbol. A multiset keeps that O(log n) to maintain and O(1) to read,
  // instead of scanning every reservation on every admission.
  std::map<SymbolId, std::multiset<std::int64_t>> live_limit_prices;
  // Chains in UNKNOWN or QUARANTINE, for the backpressure cap in section 17.
  std::int64_t unresolved_count = 0;
  // intent_id -> position in `outbox`, so marking one intent dispatched does
  // not scan every intent ever created.
  std::map<std::string, std::size_t> outbox_index;
};

// Walks the in-transaction crash windows CW-05..CW-11 *before* anything is
// written, and returns true when one of them demands that the transaction not
// happen. `outcome` then carries what the failpoint asked for.
//
// Evaluating the windows up front is what lets `apply_staged` mutate the state
// in place. The alternative -- applying to a full copy and discarding it on
// failure -- made every commit O(total domain state), which �25.1 rules out.
// The observable result is identical: on a blocked transaction nothing is
// visible and nothing is durable, which is exactly what each window asserts.
//
// A CommitAckLost arming does not block: it means the transaction *did* happen
// and only the acknowledgement was lost (RI-24).
bool transaction_windows_block(FailpointRegistry* failpoints,
                               CommitOutcome& outcome);

// Applies `data` to `state` in place, stamping `new_revision` on the domain and
// on every record that carries a commit revision, and stamping the audit events
// with the state hash before and after. It cannot fail: the caller runs
// `transaction_windows_block` first and, for a durable store, appends the
// journal record before calling this.
void apply_staged(StoreState& state, const TransactionData& data,
                  Revision new_revision);

// Shared read-side helpers so both stores answer queries identically.
std::vector<Reservation> sorted_reservations(const StoreState& state);

// Canonical hash of all committed state except the audit log itself (the log
// records this value, so including it would be circular). Both stores use it,
// which is what makes "the replayed journal equals the live state" checkable.
std::string store_state_hash(const StoreState& state);

class MemoryStore final : public RiskStore {
 public:
  MemoryStore(RiskDomainState initial_domain, RiskPolicy policy,
              FailpointRegistry* failpoints);

  RiskDomainState domain_state() const override;
  DomainExposure exposure() const override;
  RiskPolicy policy() const override;

  std::optional<CommandRecord> find_command(const TenantId& tenant,
                                            const AccountId& account,
                                            const CommandId& command) const override;
  std::optional<Reservation> find_reservation(const ReservationId& id) const override;
  std::optional<ReservationId> find_chain(const TenantId& tenant,
                                          const AccountId& account,
                                          const OrderChainId& chain) const override;
  std::optional<AppliedEventRecord> find_applied_event(
      const TenantId& tenant, const std::string& ns,
      const EventId& event) const override;

  std::vector<Reservation> reservations() const override;
  std::vector<AuditEvent> audit() const override;
  std::vector<OutboxIntent> outbox() const override;
  bool account_quarantined(const AccountId& account) const override;
  std::vector<AccountId> quarantined_accounts() const override;
  std::int64_t max_live_limit_price(const SymbolId& symbol) const override;
  std::int64_t unresolved_reservation_count() const override;

  CommitOutcome commit(const CommitRequest& request) override;
  void install_fencing_token(FencingToken token) override;
  std::string state_hash() const override;

  bool durable() const override { return false; }
  std::string describe() const override { return "memory"; }

  // Test hook: seeds committed state without going through a transaction, for
  // building fixtures. Never used by the engine itself.
  void seed(const StoreState& state);
  StoreState snapshot_state() const;

 private:
  mutable std::mutex mu_;
  StoreState state_;
  FailpointRegistry* failpoints_;
};

}  // namespace risk
