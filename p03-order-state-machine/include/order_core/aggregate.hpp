// The order aggregate: version chain, fill ledger, pending registry and the
// two state layers.
//
// Design doc 7.3 is explicit that one enum cannot carry the truth. What the
// counterparty has confirmed (`base`, `done_reason`, accepted versions, the
// ledger) is kept apart from what is merely in flight or in doubt (`pending`,
// peer pending flags, `confidence`, `reconciliation`). `lifecycle()` derives a
// single view for reporting and assertions, but nothing inside the reducer
// decides anything from that derived value.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "order_core/events.hpp"
#include "order_core/ids.hpp"
#include "order_core/math.hpp"

namespace order_core {

// Confirmed business phase. Terminal is reached only on proof.
enum class BaseState { Preparing, SentUnknown, Working, Done };

enum class DoneReason { None, Filled, Canceled, Expired, Rejected, DoneForDay };

// Whether the order's disposition is known. A timeout moves this to Unknown
// and nothing else; it never produces a cancel or a fill (invariant I18).
enum class Confidence { Confirmed, Unknown };

// Single derived view, ordered so that a proven terminal outranks an in-flight
// mutation and a contradiction outranks everything.
enum class Lifecycle {
    LocalPreparing,
    NewSentUnknown,
    Working,
    PartFilled,
    PendingCancel,
    PendingReplace,
    Filled,
    Canceled,
    Expired,
    Rejected,
    DoneForDay,
    ReconciliationRequired,
    Quarantined,
};
const char* to_string(Lifecycle l);
const char* to_string(BaseState s);
const char* to_string(DoneReason r);
const char* to_string(Confidence c);

struct AcceptedVersion {
    uint32_t version = 0;
    std::string cl_ord_id;
    int64_t qty_units = 0;
    int64_t price_ticks = 0;
    // Pinned at acceptance so a later config reload cannot reinterpret this
    // version's price (what-if D-11, P-01, P-02).
    uint32_t scale_id = 0;
    uint32_t policy_version = 0;
    uint64_t accepted_at_event = 0;
};

struct PendingRequest {
    CommandKind kind = CommandKind::New;
    enum class Phase {
        Prepared,     // admitted locally, outbox intent not yet committed
        SentUnknown,  // dispatched, no peer acknowledgement
        PeerPending,  // peer confirmed 150=6 / 150=E
        Resolved,
    };
    Phase phase = Phase::Prepared;
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    // Desired, not accepted. Reading these as if they were accepted is the
    // mistake invariant I04 exists to catch.
    int64_t desired_qty_units = 0;
    int64_t desired_price_ticks = 0;
    bool desired_qty_present = false;
    bool desired_price_present = false;
    uint64_t dispatch_intent_id = 0;
    // This request's own disposition is unknown (it timed out, or the
    // transport dropped). Deliberately separate from the chain's confidence: a
    // cancel whose fate is unknown does not make the order's own existence
    // unknown (design doc 7.3, what-if H-15).
    bool outcome_unknown = false;
};
const char* to_string(PendingRequest::Phase p);

struct FillLedgerEntry {
    std::string exec_id;
    std::string cl_ord_id;  // the version the counterparty reported it against
    int64_t qty_units = 0;
    int64_t price_ticks = 0;
    uint32_t accepted_version_seen = 0;
    bool effective = true;  // v1 never flips this; corrections are quarantined
    std::string source_report_id;
    uint64_t provenance_hash = 0;
};

struct QuarantineIncident {
    std::string reason_code;
    std::string detail;
    std::string exec_id;
    std::string cl_ord_id;
    uint64_t at_revision = 0;
};

// An execution report seen on this chain, with the business fingerprint it
// carried. Needed to tell a harmless redelivery from a mutated replay.
struct SeenReport {
    std::string exec_id;
    uint64_t fingerprint = 0;
};

struct OrderAggregate {
    OrderId id;
    uint64_t revision = 0;

    // Immutable identity, fixed when the chain is created.
    std::string firm;
    std::string business_day;
    std::string account;
    std::string symbol;
    Side side = Side::Unspecified;
    std::string initial_cl_ord_id;
    // Bound by the first peer report that is applied. A chain belongs to one
    // counterparty; a second peer reporting on it is ambiguous, not extra
    // information (what-if G-14).
    std::string peer_id;
    // Fingerprint of the New intent that opened this chain, so an identical
    // intent arriving under a new ClOrdID can be recognised.
    uint64_t new_intent_fingerprint = 0;

    // --- confirmed business layer -------------------------------------------
    BaseState base = BaseState::Preparing;
    DoneReason done_reason = DoneReason::None;
    std::vector<AcceptedVersion> accepted_versions;
    int64_t cum_qty_units = 0;
    Int128 notional_ticks_x_qty = 0;  // sum(fill_qty * fill_price_ticks)
    std::vector<FillLedgerEntry> ledger;
    std::vector<SeenReport> seen_reports;
    // Each entry is "peer|OrderID". An LP OrderID is only meaningful inside
    // its own peer's namespace, so two counterparties handing out the same
    // string are two different aliases (design doc 3.3, what-if E-12).
    std::vector<std::string> order_id_aliases;

    // --- operational uncertainty layer --------------------------------------
    std::optional<PendingRequest> pending;
    bool peer_pending_cancel = false;
    bool peer_pending_replace = false;
    Confidence confidence = Confidence::Confirmed;
    bool reconciliation_required = false;
    uint64_t reconciliation_generation = 0;
    size_t reconcile_attempts = 0;
    bool quarantined = false;
    std::vector<QuarantineIncident> incidents;
    // How many of those incidents an audited resolution has closed. Incidents
    // beyond this count are live contradictions and must leave the chain
    // visibly unsafe (invariant I24).
    size_t resolved_incident_count = 0;
    // Events parked because a causal precondition may still arrive.
    size_t held_events = 0;
    // Highest source cursor applied, so a stale snapshot cannot overwrite a
    // newer one (what-if G-10, G-11).
    uint64_t max_source_seq = 0;
    uint64_t last_applied_event_index = 0;

    // --- queries ------------------------------------------------------------
    [[nodiscard]] const AcceptedVersion* accepted() const {
        return accepted_versions.empty() ? nullptr : &accepted_versions.back();
    }
    [[nodiscard]] bool has_accepted() const { return !accepted_versions.empty(); }
    [[nodiscard]] int64_t accepted_qty_units() const {
        const AcceptedVersion* v = accepted();
        return v == nullptr ? 0 : v->qty_units;
    }
    [[nodiscard]] int64_t accepted_price_ticks() const {
        const AcceptedVersion* v = accepted();
        return v == nullptr ? 0 : v->price_ticks;
    }
    [[nodiscard]] std::string accepted_cl_ord_id() const {
        const AcceptedVersion* v = accepted();
        return v == nullptr ? std::string() : v->cl_ord_id;
    }
    [[nodiscard]] bool terminal() const { return base == BaseState::Done; }

    // Invariant I06 and I07: a working order's leaves is the remainder, a
    // terminal order's leaves is zero even when CumQty never reached OrderQty.
    [[nodiscard]] int64_t leaves_qty_units() const {
        if (terminal()) return 0;
        if (!has_accepted()) return 0;
        const int64_t remaining = accepted_qty_units() - cum_qty_units;
        return remaining > 0 ? remaining : 0;
    }

    [[nodiscard]] Lifecycle lifecycle() const;

    // Recomputes CumQty and notional from the ledger. The cached values exist
    // for the hot path; this is what proves they were never wrong (I11).
    [[nodiscard]] int64_t ledger_cum_qty() const;
    [[nodiscard]] Int128 ledger_notional() const;

    [[nodiscard]] const FillLedgerEntry* find_fill(const std::string& exec_id) const;
    [[nodiscard]] const SeenReport* find_seen(const std::string& exec_id) const;
    [[nodiscard]] bool has_alias(const std::string& peer, const std::string& order_id) const;
    // Any peer. Used where only the wire value is at hand.
    [[nodiscard]] bool has_order_id(const std::string& order_id) const;

    // Exact weighted average execution price as a rational in ticks. Kept
    // unrounded; rounding happens once, at a serialization boundary.
    [[nodiscard]] bool avg_px_rational(Int128& numerator, Int128& denominator) const;
    // Round-half-to-even rendering of the above.
    [[nodiscard]] bool avg_px_ticks_rounded(int64_t& out) const;

    // Canonical digest of the confirmed and uncertain business state. Excludes
    // observational data such as elapsed time or counters, so the same ordered
    // input always yields the same value (invariant I22).
    [[nodiscard]] std::string canonical_text() const;
    [[nodiscard]] uint64_t state_hash() const { return fnv1a(canonical_text()); }
};

}  // namespace order_core
