// The command and report service.
//
// This is the P04 side of the AtomicApplyV1 contract from section 6: it
// canonicalises a request, asks the pure reducers whether the resulting
// events are admissible, builds one TxPlan and hands it to the store. It
// performs no I/O of its own and holds no transaction.
//
// Two decisions here are worth stating because they shape every test:
//
//  * Risk is measured in quantity units. A new order reserves its full
//    quantity, a fill consumes the filled quantity and leaves the rest held,
//    and a terminal outcome releases what is left. This keeps the ledger
//    exact in integers, so "risk equals the sum of unique verified fills" is
//    an equality a test can assert rather than a tolerance.
//
//  * A contradictory report is detected by dry-running the pure reducer
//    BEFORE the transaction. The plan that results records the receipt as
//    QUARANTINED and opens a case, instead of letting the store roll the
//    whole transaction back and leave the receipt to be redelivered for
//    ever (section 9.2).
#pragma once

#include <optional>
#include <string>

#include "jrnl/metrics.hpp"
#include "jrnl/recovery.hpp"
#include "jrnl/stable_send.hpp"
#include "jrnl/store.hpp"

namespace jrnl {

struct NewOrderRequest {
    TenantId tenant;
    AccountId account;
    CommandId command_id;
    OrderChainId chain;
    ClOrdId cl_ord_id;
    std::string symbol;
    std::string side = "BUY";
    std::int64_t order_qty = 0;
    std::int64_t limit_px = 0;
    RiskDomainId risk_domain;
    SessionKey session_key;
    std::string fix_payload;  // the original outbound request, verbatim
};

struct CancelRequest {
    TenantId tenant;
    AccountId account;
    CommandId command_id;
    OrderChainId chain;
    ClOrdId cl_ord_id;  // the cancel's own ClOrdID
    RiskDomainId risk_domain;
    SessionKey session_key;
    std::string fix_payload;
};

struct ReplaceRequest {
    TenantId tenant;
    AccountId account;
    CommandId command_id;
    OrderChainId chain;
    ClOrdId cl_ord_id;
    std::int64_t order_qty = 0;
    std::int64_t limit_px = 0;
    RiskDomainId risk_domain;
    SessionKey session_key;
    std::string fix_payload;
};

// What arrives from P01 after normalisation by P04.
struct ExecutionReport {
    TenantId tenant;
    SessionKey session_key;
    std::uint64_t session_epoch = 1;
    std::uint64_t receipt_id = 0;
    std::string raw;  // raw FIX bytes as P01 stored them

    LpNamespace lp_namespace;
    // Only meaningful for a report that genuinely carries a unique execution
    // identity. An empty id, or the literal "0", is not a trade key: section
    // 9.2 says so explicitly and the service refuses to use one.
    ExecId exec_id;
    std::string exec_type = "TRADE";  // TRADE, CANCELED, REJECTED, REPLACED, EXPIRED

    OrderChainId chain;
    AccountId account;
    RiskDomainId risk_domain;
    std::int64_t last_qty = 0;
    std::int64_t last_px = 0;
    // The command whose outbound effect this report is evidence for.
    CommandId originating_command;
};

class GatewayService {
public:
    struct Options {
        const IAdmissionGate* gate = nullptr;  // null means no gate, tests only
        StableSendStore* link = nullptr;       // for the delivery acknowledgement
        Metrics* metrics = nullptr;
        PolicyVersion policy_version = 1;
    };

    GatewayService(SqliteAtomicStore& store, Options opt) : store_(store), opt_(std::move(opt)) {}

    // Establishes or raises the ceiling for a risk domain. A domain must
    // exist before anything can reserve against it.
    CommitOutcome set_risk_limit(const TenantId& tenant, const RiskDomainId& domain,
                                 std::int64_t limit, FenceToken token);

    CommitOutcome submit_new(const NewOrderRequest& r, FenceToken token);
    CommitOutcome submit_cancel(const CancelRequest& r, FenceToken token);
    CommitOutcome submit_replace(const ReplaceRequest& r, FenceToken token);

    // Applies one inbound report. Allowed while the supervisor is in
    // RECON_ONLY: refusing to learn what happened would make reconciliation
    // impossible. What RECON_ONLY blocks is new business and dispatch.
    CommitOutcome apply_report(const ExecutionReport& r, FenceToken token);

    // Acknowledges delivery to P01. Called only after apply_report returned
    // a durable outcome, which is the ordering section 11 requires.
    StoreError ack_delivery(const ExecutionReport& r);

    // The canonical hash of a request. Exposed because a retry must present
    // the same hash, and a test needs to be able to construct both.
    static Digest canonical_hash(const NewOrderRequest& r);
    static Digest canonical_hash(const CancelRequest& r);
    static Digest canonical_hash(const ReplaceRequest& r);

    // Reservation naming. One hold per order chain, derived so that a retry
    // of the same command names the same hold.
    static ReservationId reservation_for(const OrderChainId& chain) { return "res:" + chain; }

private:
    // One attempt each. The public entry points above rebuild the plan from
    // fresh stream heads and call these again when a concurrent writer
    // moved a stream, always under the same idempotency key.
    CommitOutcome submit_new_once(const NewOrderRequest& r, FenceToken token);
    CommitOutcome submit_cancel_once(const CancelRequest& r, FenceToken token);
    CommitOutcome submit_replace_once(const ReplaceRequest& r, FenceToken token);
    CommitOutcome apply_report_once(const ExecutionReport& r, FenceToken token);

    bool gate_blocks(const TenantId& tenant, const RiskDomainId& domain, std::string* why) const;
    CommitOutcome quarantine(const ExecutionReport& r, const std::string& reason,
                             FenceToken token);

    SqliteAtomicStore& store_;
    Options opt_;
};

}  // namespace jrnl
