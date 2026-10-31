// The atomic unit of work.
//
// Section 9.0 of the design doc is the whole contract in one sentence: the
// journal events, the dedup keys, the order and risk projections, the command
// intent and the outbox intents go into ONE transaction, and there is no code
// path that commits a subset. Everything in this header exists to make that
// statement checkable.
//
// Two rules shape the interface:
//
//  * A TxPlan carries events, not projections. The store runs the pure
//    reducers itself, inside the transaction, and writes what they produce.
//    That is what makes "replay equals projection" (JI-11, JI-12) true by
//    construction instead of by convention, and it is why a test can corrupt
//    a projection row and watch the verifier find it.
//
//  * No call in here performs I/O outside the database. Section 9.1 forbids
//    an RPC or a socket write inside a SQL transaction, so the outbox records
//    an intent and a separate dispatcher acts on it after the commit.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "jrnl/canon.hpp"
#include "jrnl/clock.hpp"
#include "jrnl/domain.hpp"
#include "jrnl/hash.hpp"
#include "jrnl/ids.hpp"
#include "jrnl/result.hpp"
#include "jrnl/sql.hpp"

namespace jrnl {

// ---------------------------------------------------------------------------
// Write-set members
// ---------------------------------------------------------------------------

struct AppendEvent {
    StreamId stream;
    // The version the stream must currently be at. A mismatch is a conflict,
    // never an append at whatever version happens to be there (G01-05).
    Version expected_version = 0;
    EventId event_id;
    std::string event_type;
    SchemaVersion schema_version = 1;
    std::string reducer_version;
    PolicyVersion policy_version = 1;
    CanonValue payload;
    // Position within the transaction manifest. Unique per (tenant, tx).
    int ordinal = 0;
};

// Transport-level dedup key: the same raw FIX receipt delivered twice.
struct InboxReceipt {
    SessionKey session_key;
    std::uint64_t session_epoch = 0;
    std::uint64_t receipt_id = 0;
    Digest payload_hash{};
};

// Client-level dedup key: the same command submitted twice.
struct CommandIntentPlan {
    AccountId account;
    CommandId command_id;
    OrderChainId chain;
    Digest canonical_hash{};
    ReservationId reservation;  // may be empty
    std::string decision = "ACCEPTED";
};

// Business-level dedup key: the same execution reported twice.
struct ExecKeyPlan {
    LpNamespace lp_namespace;
    ExecId semantic_exec_id;
    // Covers the economically meaningful fields. Same key with a different
    // fingerprint is a contradiction, not a duplicate (JI-07).
    Digest fingerprint{};
    OrderChainId chain;
    std::int64_t last_qty = 0;
    std::int64_t last_px = 0;
};

struct OutboxIntent {
    OutboxId outbox_id;
    AccountId account;
    CommandId command_id;
    SessionKey session_key;
    Digest payload_hash{};
    std::string payload;  // the original FIX request, verbatim
};

struct RecoveryCasePlan {
    CaseId case_id;
    OrderChainId chain;
    std::string reason_code;
    Digest evidence_hash{};
    std::string status = "OPEN";
};

struct AuditPlan {
    std::string category;
    std::string detail;
    CanonValue data;
};

// A state transition applied to an existing outbox row in the same business
// transaction, for example CONFIRMED when a fill proves the order arrived.
struct OutboxTransition {
    OutboxId outbox_id;
    std::string new_state;
    std::string reason;
};

struct TxPlan {
    TenantId tenant;
    TxId tx_id;

    // Ownership. Every mutation is checked against the current token for the
    // domain in the same transaction, which is what makes a stale writer's
    // write fail rather than merely be unlikely (JI-21, section 16).
    RiskDomainId fence_domain;
    FenceToken fence_token = 0;

    std::optional<InboxReceipt> receipt;
    std::optional<CommandIntentPlan> intent;
    std::optional<ExecKeyPlan> exec_key;

    std::vector<AppendEvent> events;
    std::vector<OutboxIntent> effects;
    std::vector<OutboxTransition> outbox_updates;
    std::vector<RecoveryCasePlan> cases;
    std::vector<AuditPlan> audit;

    // Set when the plan records a contradiction rather than a change: the
    // receipt is stored as QUARANTINED, a case is opened, and no domain
    // mutation happens (section 9.2).
    bool quarantine = false;
    std::string quarantine_reason;
};

// ---------------------------------------------------------------------------
// Read models
// ---------------------------------------------------------------------------

struct StreamHead {
    bool exists = false;
    Version version = 0;
    Digest state_hash{};
    std::string reducer_version;
    FenceToken fence = 0;
};

struct JournalRecord {
    StreamId stream;
    Version version = 0;
    EventId event_id;
    TxId tx_id;
    int ordinal = 0;
    std::string event_type;
    SchemaVersion schema_version = 1;
    std::string reducer_version;
    PolicyVersion policy_version = 1;
    std::string payload_text;  // exactly the bytes stored, for rehashing
    Digest canonical_hash{};
    Digest prev_state_hash{};
    Digest state_hash{};
    std::int64_t created_at = 0;
};

struct CommandRecord {
    bool found = false;
    OrderChainId chain;
    Digest canonical_hash{};
    ReservationId reservation;
    std::string decision;
    std::string result;
    TxId tx;
};

struct ReceiptRecord {
    bool found = false;
    Digest payload_hash{};
    std::string apply_state;
    TxId tx;
};

struct ExecKeyRecord {
    bool found = false;
    Digest fingerprint{};
    OrderChainId chain;
    TxId tx;
};

struct OutboxRow {
    OutboxId outbox_id;
    TenantId tenant;
    AccountId account;
    CommandId command_id;
    SessionKey session_key;
    Digest payload_hash{};
    std::string payload;
    std::string state;
    std::optional<std::uint64_t> original_seq;
    std::string claim_owner;
    FenceToken claim_token = 0;
    std::int64_t claim_expires_at = 0;
    int attempts = 0;
    std::string last_error;
};

// ---------------------------------------------------------------------------
// Interfaces from section 20 of the design doc
// ---------------------------------------------------------------------------

class IAtomicStore {
public:
    virtual ~IAtomicStore() = default;
    virtual CommitOutcome apply(const TxPlan& plan) = 0;
    virtual CommandRecord lookup(const TenantId& tenant, const AccountId& account,
                                 const CommandId& command) const = 0;
};

class IJournalReader {
public:
    virtual ~IJournalReader() = default;
    virtual StoreError read_stream(const TenantId& tenant, const StreamId& stream, Version from,
                                   std::size_t max, std::vector<JournalRecord>* out) const = 0;
    virtual StreamHead head(const TenantId& tenant, const StreamId& stream) const = 0;
    virtual std::vector<StreamId> list_streams(const TenantId& tenant) const = 0;
};

// ---------------------------------------------------------------------------
// SQLite implementation
// ---------------------------------------------------------------------------

class SqliteAtomicStore final : public IAtomicStore, public IJournalReader {
public:
    struct Options {
        std::string path;
        bool create_if_missing = true;
        int busy_timeout_ms = 5000;
        std::string journal_mode = "WAL";
        std::string synchronous = "FULL";
        const IClock* clock = nullptr;
        const ReducerRegistry* reducers = nullptr;
        // Bounded admission from section 19. A plan larger than this is
        // refused rather than allowed to hold locks for an unbounded time.
        std::size_t max_events_per_tx = 64;
        std::int64_t max_outbox_backlog = 10000;
    };

    static StoreError open(const Options& opt, std::unique_ptr<SqliteAtomicStore>* out,
                           std::string* why);

    ~SqliteAtomicStore() override;

    // --- IAtomicStore ---
    CommitOutcome apply(const TxPlan& plan) override;
    CommandRecord lookup(const TenantId& tenant, const AccountId& account,
                         const CommandId& command) const override;

    // --- IJournalReader ---
    StoreError read_stream(const TenantId& tenant, const StreamId& stream, Version from,
                           std::size_t max, std::vector<JournalRecord>* out) const override;
    StreamHead head(const TenantId& tenant, const StreamId& stream) const override;
    std::vector<StreamId> list_streams(const TenantId& tenant) const override;

    // --- Projections ---
    bool load_order(const TenantId& tenant, const OrderChainId& chain, OrderState* out) const;
    bool load_risk(const TenantId& tenant, const RiskDomainId& domain,
                   RiskDomainState* out) const;
    ReceiptRecord lookup_receipt(const TenantId& tenant, const InboxReceipt& r) const;
    ExecKeyRecord lookup_exec_key(const TenantId& tenant, const LpNamespace& ns,
                                  const ExecId& id) const;

    // --- Fencing (section 16) ---
    // Grants the next token for a domain. Monotonic: a previous owner's token
    // is strictly lower and every mutation compares against the stored value.
    StoreError acquire_fence(const TenantId& tenant, const RiskDomainId& domain,
                             const std::string& owner, std::int64_t lease_ms, FenceToken* out);
    FenceToken current_fence(const TenantId& tenant, const RiskDomainId& domain) const;

    // --- Outbox (section 10) ---
    StoreError claim_outbox(const TenantId& tenant, const std::string& owner, FenceToken token,
                            std::size_t limit, std::int64_t lease_ms,
                            std::vector<OutboxRow>* out);
    StoreError set_outbox_state(const TenantId& tenant, const OutboxId& id,
                                const std::string& state, FenceToken token,
                                std::optional<std::uint64_t> original_seq,
                                const std::string& note);
    StoreError release_expired_claims(const TenantId& tenant, std::int64_t now_ms,
                                      int* released);
    // Records, durably and on its own, that a dispatcher is about to call
    // P01 for this row. Section 10: without this the engine cannot tell an
    // expired claim that never sent from one that may have (JI-20).
    StoreError mark_send_attempted(const TenantId& tenant, const OutboxId& id, FenceToken token);
    std::vector<OutboxRow> outbox_rows(const TenantId& tenant,
                                       const std::string& state_filter = "") const;
    bool outbox_row(const TenantId& tenant, const OutboxId& id, OutboxRow* out) const;
    // By the business key rather than the row id. There is a UNIQUE index
    // on it, so this is the lookup the inbound path uses: scanning every
    // row to find the one effect a report is evidence for turned an O(1)
    // question into an O(n) table scan per message.
    bool outbox_for_command(const TenantId& tenant, const AccountId& account,
                            const CommandId& command, OutboxRow* out) const;

    // Rebuilds every order and risk projection for a tenant by replaying
    // its streams. Section 14's runbook calls for exactly this after a
    // restore, and it is also the cheapest proof that the restored journal
    // replays: a stream that does not verify makes the rebuild fail rather
    // than producing a plausible-looking projection.
    StoreError rebuild_projections(const TenantId& tenant, std::string* why);

    // --- Recovery cases ---
    StoreError open_case(const TenantId& tenant, const RecoveryCasePlan& c);
    StoreError resolve_case(const TenantId& tenant, const CaseId& id, const std::string& status,
                            const std::string& approvals);
    std::vector<RecoveryCasePlan> open_cases(const TenantId& tenant) const;

    // --- Audit (section 18) ---
    StoreError append_audit(const TenantId& tenant, const AuditPlan& a, const TxId& tx);
    // Walks the audit chain. A removed or edited row breaks it.
    bool audit_chain_intact(const TenantId& tenant, std::string* why) const;
    std::size_t audit_count(const TenantId& tenant) const;

    Db& db() { return *db_; }
    const Db& db() const { return *db_; }
    const IClock& clock() const { return *clock_; }
    const ReducerRegistry& reducers() const { return *reducers_; }
    const Options& options() const { return opt_; }

    // Last plan's rejection detail, for tests and audit lines.
    const std::string& last_detail() const { return last_detail_; }

private:
    SqliteAtomicStore() = default;

    // Everything below runs inside an open transaction.
    CommitOutcome apply_locked(const TxPlan& plan, std::string* detail);
    StoreError check_fence(const TxPlan& plan, std::string* detail);
    StoreError write_event(const TxPlan& plan, const AppendEvent& e,
                           std::map<StreamId, StreamHead>* heads,
                           std::map<OrderChainId, OrderState>* orders,
                           std::map<RiskDomainId, RiskDomainState>* risks, std::string* detail);
    StoreError write_order_projection(const TenantId& tenant, const OrderState& s, Version v,
                                      const Digest& hash, const TxId& tx,
                                      const std::string& reducer_version);
    StoreError write_risk_projection(const TenantId& tenant, const RiskDomainState& s, Version v,
                                     const Digest& hash, const TxId& tx);
    StoreError write_fill_row(const TenantId& tenant, const ExecKeyPlan& k, const TxId& tx);
    Digest audit_tip(const TenantId& tenant) const;

    // The store owns ONE connection, and SQLite allows one transaction per
    // connection. Callers on several threads therefore queue here rather
    // than discovering "cannot start a transaction within a transaction"
    // at the point of commit. The lock covers only the methods that open a
    // transaction; reads go straight through.
    mutable std::mutex tx_mu_;

    std::unique_ptr<Db> db_;
    const IClock* clock_ = nullptr;
    const ReducerRegistry* reducers_ = nullptr;
    std::unique_ptr<SystemClock> owned_clock_;
    Options opt_;
    std::string last_detail_;
};

// Deterministic transaction id from the plan's identity. Two retries of the
// same command produce the same tx id, so a retry cannot create a second
// manifest for work that already committed (JI-27).
TxId derive_tx_id(const TenantId& tenant, const std::string& scope, const Digest& key);

// Canonical fingerprint of an execution report: the fields whose change would
// make it a different trade.
Digest exec_fingerprint(const OrderChainId& chain, std::int64_t last_qty, std::int64_t last_px,
                        const std::string& exec_type);

}  // namespace jrnl
