// Durable append-only journal store.
//
// A commit is one length-prefixed, CRC-protected record appended to a single
// file and flushed to the device before the acknowledgement is returned. That
// gives genuine all-or-nothing durability: a process killed mid-write leaves a
// torn tail, which replay detects and discards, so the recovered state is
// exactly the prefix of fully-written commits (§16.3, CW-01..CW-20).
//
// This is the single-node lab adapter the spec sanctions (§1.3). It is not a
// substitute for the P06 PostgreSQL contract, and it does not claim multi-node
// consistency.
#pragma once

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "risk/memory_store.hpp"
#include "risk/store.hpp"

namespace risk {

struct JournalStats {
  std::uint64_t records_applied = 0;
  std::uint64_t bytes_read = 0;
  // Bytes at the end of the file that did not form a complete, valid record.
  // Non-zero after a crash mid-append; the data is discarded, never guessed at.
  std::uint64_t truncated_tail_bytes = 0;
  bool created = false;
};

class JournalStore final : public RiskStore {
 public:
  // Opens or creates `path`. On create, the bootstrap record pins the tenant,
  // domain and initial policy. On open, those come from the file and the
  // supplied values are used only to cross-check: a mismatch is reported as
  // Reason::SnapshotMismatch rather than silently overwritten.
  static Result<std::unique_ptr<JournalStore>> open(
      std::string path, const RiskDomainState& initial_domain,
      const RiskPolicy& initial_policy, FailpointRegistry* failpoints);

  ~JournalStore() override;

  JournalStore(const JournalStore&) = delete;
  JournalStore& operator=(const JournalStore&) = delete;

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

  bool durable() const override { return true; }
  std::string describe() const override { return "journal:" + path_; }

  const JournalStats& stats() const noexcept { return stats_; }
  const std::string& path() const noexcept { return path_; }

  // Closes the file without flushing anything further. Used by tests to model a
  // process exit between the durable write and the acknowledgement.
  void close_abruptly();

 private:
  JournalStore(std::string path, FailpointRegistry* failpoints);

  Status replay();
  Status append_record(const std::string& payload);

  std::string path_;
  std::FILE* file_ = nullptr;
  FailpointRegistry* failpoints_ = nullptr;

  mutable std::mutex mu_;
  StoreState state_;
  JournalStats stats_;
};

// Exposed for the journal-format unit tests.
std::uint32_t crc32_of(std::string_view data);
std::string encode_transaction(Revision revision, FencingToken token,
                               const TransactionData& data);
Result<TransactionData> decode_transaction(std::string_view payload,
                                           Revision& revision,
                                           FencingToken& token);

}  // namespace risk
