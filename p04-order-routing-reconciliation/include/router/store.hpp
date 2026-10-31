// P06 -- the atomic durable store, and the "disk" that survives a crash.
//
// Design doc 6.5 asks for one transaction that covers
// (applied inbound key, P03 projection, outbox, audit, reconcile task). This
// adapter provides exactly that and nothing weaker: a Txn either applies every
// staged change or none of them, and the caller cannot observe a half state.
//
// The split between StoreImage and Store is what makes the twenty crash windows
// in design doc 15.1 testable. StoreImage is the bytes on disk; it outlives the
// router object. Store is the process-side adapter. A "crash" in a test
// destroys the Store and the Router and keeps the image, then boots a new pair
// against it -- which is what a real restart does.
//
// Two rules are enforced here rather than left to callers:
//   * R-I20, fail closed on durability. A failed commit never looks successful.
//   * R-I19/OR-E-18, single owner. A stale fencing token cannot write.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "deps/p03_osm.hpp"
#include "router/audit.hpp"
#include "router/clock.hpp"
#include "router/failpoint.hpp"
#include "router/hash.hpp"
#include "router/ids.hpp"
#include "router/report.hpp"
#include "router/result.hpp"

namespace router {

enum class CommandKind { New, Cancel, Replace, StatusQuery };
std::string_view to_string(CommandKind k);

// Design doc 10.1. These describe *delivery*, never business outcome: section
// 1.4 is emphatic that ACCEPTED_DURABLE is not an LP acknowledgement and
// UNKNOWN_TRANSPORT is not a rejection.
enum class DeliveryState {
    Received,
    RejectedLocal,
    AcceptedDurable,
    Queued,
    Dispatching,
    OriginalSeqBound,
    AwaitBusiness,
    UnknownTransport,
    Reconciling,
    LpAcked,
    LpRejected,
    ManualHold,
};
std::string_view to_string(DeliveryState s);

// What the business outcome of a command is known to be, separately from the
// delivery state above (design doc 8.3).
enum class BusinessOutcome { Unknown, Acknowledged, Rejected, Canceled, Replaced, Filled };
std::string_view to_string(BusinessOutcome o);

enum class OutboxState { Pending, Claimed, Sent, Done, Failed, Held };
std::string_view to_string(OutboxState s);

// The immutable record of one admitted command. Everything needed to re-send
// the identical bytes after a restart is here, including the snapshot of the
// configuration that justified the route (design doc 15.2).
struct CommandIntent {
    TenantId tenant;
    CommandId command_id;
    OrderChainId chain_id;
    CommandKind kind = CommandKind::New;
    Hash request_hash;   // digest of the canonical client payload
    Hash payload_hash;   // digest of the immutable FIX application body
    ClientOrderId cl_ord_id;
    std::optional<ClientOrderId> orig_cl_ord_id;

    AccountId account;
    LpId lp;
    std::string session_key;
    std::uint64_t route_config_version = 0;
    std::uint64_t profile_version = 0;

    // Admission snapshot, so a replay does not re-read live config.
    std::string wire_symbol;
    Side side = Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price_ticks = 0;
    int price_scale = 5;
    OrdType ord_type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;
    std::string risk_reservation_id;
    WallMicros risk_valid_until = 0;

    std::string fix_body;  // the exact application fields, SOH separated
    OutboxId outbox_id = 0;
    DeliveryState delivery = DeliveryState::Received;
    BusinessOutcome business = BusinessOutcome::Unknown;
    std::optional<MsgSeqNum> original_seq;
    std::optional<HoldReason> hold;
    std::optional<ReconCaseId> recon_case;

    WallMicros created_wall = 0;
    MonoMicros created_mono = 0;
    // Optional rather than a sentinel: the monotonic clock legitimately starts
    // at zero, so "0" cannot mean "never dispatched".
    std::optional<MonoMicros> dispatched_mono;
    int send_attempts = 0;

    std::string key() const { return tenant.str() + "|" + command_id.str(); }
};

struct OutboxRow {
    OutboxId id = 0;
    TenantId tenant;
    CommandId command_id;
    Hash payload_hash;
    std::string immutable_fix_fields;
    OutboxState state = OutboxState::Pending;
    int attempts = 0;
    MonoMicros enqueued_mono = 0;
    std::optional<std::uint64_t> claim_fence;  // which router instance holds it
    int priority = 0;                          // cancel first, see Limits
};

// Transport level dedup key (design doc 15.2). Keyed by session identity *and*
// epoch, so a sequence reset cannot make an old receipt look new.
struct AppliedReceipt {
    std::string session_key;
    SessionEpoch epoch = 0;
    DurableReceiptId receipt_id = 0;
    Hash raw_hash;
    std::string outcome;  // Applied / Deduplicated / Quarantined

    std::string key() const {
        return session_key + "|" + std::to_string(epoch) + "|" + std::to_string(receipt_id);
    }
};

// Business level dedup key. Only populated for reports that actually carry an
// execution identity; a status snapshot with ExecID=0 must never land here
// (R-I08, OR-K-09).
struct BusinessExecKey {
    std::string lp_namespace;
    std::string semantic_exec_id;
    Hash payload_fingerprint;
    OrderChainId chain_id;
    DurableReceiptId first_receipt = 0;

    std::string key() const { return lp_namespace + "|" + semantic_exec_id; }
};

enum class ReconState {
    Open,
    StatusQueryPending,
    StatusReceived,
    HistoryRequired,
    Reconciled,
    ManualHold,
    Quarantined,
};
std::string_view to_string(ReconState s);

struct ReconCase {
    ReconCaseId case_id;
    OrderChainId chain_id;
    std::optional<CommandId> command_id;
    HoldReason cause = HoldReason::None;
    ReconState state = ReconState::Open;
    int attempt_count = 0;
    MonoMicros next_due_mono = 0;
    MonoMicros opened_mono = 0;
    Hash last_evidence_hash;
    std::vector<std::string> query_ids;   // 790 values issued for this case
    std::vector<std::string> notes;       // sanitised
    std::string resolved_by;
    std::vector<std::string> approvals;   // two-person rule for risky actions
};

// An inbound event that could not be applied. Kept durably so that a poison
// message cannot block the pipeline forever and cannot vanish either
// (OR-L-18, R-I25).
struct QuarantineEntry {
    std::uint64_t id = 0;
    HoldReason reason = HoldReason::None;
    std::string session_key;
    SessionEpoch epoch = 0;
    DurableReceiptId receipt_id = 0;
    std::string redacted_frame;
    Hash raw_hash;
    std::optional<OrderChainId> chain_hint;
    MonoMicros created_mono = 0;
    bool resolved = false;
    std::string note;
};

// One committed transaction, in order. Replaying the journal through the P03
// reducer must reproduce the chain projection exactly (OR-R-18, PB-28).
struct JournalRecord {
    std::uint64_t seq = 0;
    enum class Kind { IntentCommitted, ReportApplied, ChainMutated, ReconMutated, Pruned } kind =
        Kind::IntentCommitted;
    std::optional<NormalizedReport> report;
    std::optional<CommandIntent> intent;
    std::optional<OrderChainId> chain_id;
    // For a mutation the reducer did not perform -- a hold or a quarantine,
    // which the router writes -- the resulting projection is recorded verbatim.
    // Without it a replay could not reproduce a held chain and the equivalence
    // proof in OR-R-18 would be vacuous.
    std::optional<p03::ChainState> chain_after;
    Hash resulting_state_hash;
};

// The durable image. Plain data: no behaviour, so a test can inspect or corrupt
// it the way a disk can be inspected or corrupted.
struct StoreImage {
    std::uint32_t schema_version = 1;
    bool full = false;       // ENOSPC
    bool corrupt = false;    // checksum failure
    bool available = true;   // database down

    std::uint64_t next_outbox_id = 1;
    std::uint64_t next_chain_seq = 1;
    std::uint64_t next_case_seq = 1;
    std::uint64_t next_journal_seq = 1;
    std::uint64_t next_quarantine_id = 1;
    std::uint64_t next_cl_ord_seq = 1;
    std::uint64_t owner_fence = 0;
    // Maintained counters, so the admission path does not have to walk the
    // whole outbox or the whole quarantine table to find out how full they are.
    // Scanning them per command made admission quadratic, which showed up as a
    // throughput cliff in LOAD-02 long before any configured bound was reached.
    std::size_t pending_outbox_count = 0;
    std::size_t unresolved_quarantine_count = 0;

    std::map<std::string, CommandIntent> intents;        // tenant|command_id
    std::map<std::string, std::string> cl_ord_index;     // lp|cl_ord_id -> intent key
    std::map<OutboxId, OutboxRow> outbox;
    std::map<std::string, AppliedReceipt> applied_receipts;
    std::map<std::string, BusinessExecKey> exec_keys;
    std::map<std::string, p03::ChainState> chains;       // chain_id
    std::map<std::string, ReconCase> recon_cases;        // case_id
    std::vector<QuarantineEntry> quarantine;
    std::vector<JournalRecord> journal;
    AuditLog audit;
};

class Store;

// Staged, all-or-nothing change set.
class Txn {
public:
    void put_intent(CommandIntent v);
    void put_outbox(OutboxRow v);
    void put_chain(p03::ChainState v);
    void put_recon(ReconCase v);
    void add_applied_receipt(AppliedReceipt v);
    void put_exec_key(BusinessExecKey v);
    void add_quarantine(QuarantineEntry v);
    // Marks a quarantined row resolved, inside the same transaction that applies
    // whatever resolved it. Nothing is ever erased: the evidence stays (R-I25).
    void resolve_quarantine(std::uint64_t id, const std::string& note);
    void add_audit(AuditRecord v);
    void add_journal(JournalRecord v);
    // Unique index insert for (lp, ClOrdID). A collision is a hard conflict:
    // design doc 18.2 forbids dropping the uniqueness to work around it
    // (OR-C-17).
    void reserve_cl_ord_id(const LpId& lp, const ClientOrderId& id, const std::string& intent_key);
    void drop_outbox(OutboxId id);

    // Applies everything or nothing. The failpoints inside model a crash during
    // the transaction (CW-02, CW-13) and a lost commit acknowledgement (CW-19).
    Result<std::uint64_t, StoreError> commit();

    bool empty() const { return ops_.empty(); }

private:
    friend class Store;
    explicit Txn(Store* s) : store_(s) {}

    struct Op {
        std::function<void(StoreImage&)> apply;
        std::function<std::optional<StoreError>(const StoreImage&)> precheck;
    };
    Store* store_;
    std::vector<Op> ops_;
};

class Store {
public:
    // `reducer` is the thing that owns order status and the fill ledger. It is
    // a parameter because replay runs through it too: the reducer that rebuilt a
    // chain after a restart has to be the one that built it in the first place,
    // or the equivalence proof compares two different machines. Defaulted so a
    // caller that has no opinion gets P04's own.
    Store(StoreImage& image, FailpointRegistry& fp, const IClock& clock,
          p03::IOrderReducer* reducer = nullptr);

    p03::IOrderReducer& reducer() const {
        return reducer_ == nullptr ? p03::default_order_reducer() : *reducer_;
    }

    // Takes ownership of the route namespace. A later instance wins and the
    // earlier one starts failing with NotOwner, which is what a lease fence
    // does (OR-E-18, OR-R-12).
    Result<std::uint64_t, StoreError> acquire_ownership();
    bool owns() const;
    std::uint64_t fence() const { return fence_; }

    Result<bool, StoreError> check_health() const;

    Txn begin() { return Txn(this); }

    // --- reads -------------------------------------------------------------
    const CommandIntent* find_intent(const TenantId& t, const CommandId& c) const;
    const CommandIntent* find_intent_by_cl_ord_id(const LpId& lp, const ClientOrderId& id) const;
    const p03::ChainState* find_chain(const OrderChainId& id) const;
    const BusinessExecKey* find_exec_key(const std::string& lp_ns, const std::string& exec_id) const;
    bool receipt_applied(const std::string& session_key, SessionEpoch epoch,
                         DurableReceiptId receipt) const;
    const ReconCase* find_recon(const ReconCaseId& id) const;
    std::vector<const OutboxRow*> pending_outbox() const;
    // The same number without materialising or sorting the list.
    std::size_t pending_outbox_count() const { return image_.pending_outbox_count; }
    std::vector<const ReconCase*> open_recon_cases() const;
    std::size_t quarantine_depth() const;

    OutboxId next_outbox_id() const { return image_.next_outbox_id; }
    StoreImage& image() { return image_; }
    const StoreImage& image() const { return image_; }
    AuditLog& audit() { return image_.audit; }

    // Allocates identifiers from durable counters, so a restart cannot reuse a
    // ClOrdID (OR-C-01, OR-C-17). The counter only moves inside a commit.
    std::string peek_chain_id() const;
    std::string peek_cl_ord_suffix() const;

    // Journal maintenance. Pruning is a crash window of its own (CW-18).
    Result<bool, StoreError> prune_journal(std::size_t keep_last);

private:
    friend class Txn;
    StoreImage& image_;
    FailpointRegistry& fp_;
    const IClock& clock_;
    p03::IOrderReducer* reducer_ = nullptr;
    std::uint64_t fence_ = 0;
};

// Rebuilds the chain projection from the journal alone, using the same P03
// reducer. Used by the replay equivalence proof; the comparison is on
// ChainState::state_hash so audit timestamps do not matter (OR-R-18).
// Rebuilds the chain projections from the journal.
//
// The reducer is a parameter for the same reason the store takes one: a replay
// that used a different reducer than the live path would compare two machines
// and call the result equivalence. Defaulted to P04's own, which is what its
// own suite should keep testing.
std::map<std::string, p03::ChainState> replay_chains_from_journal(
    const StoreImage& image, p03::IOrderReducer* reducer = nullptr);

}  // namespace router
