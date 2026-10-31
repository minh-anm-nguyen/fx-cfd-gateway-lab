#include "order_core/aggregate.hpp"

namespace order_core {

const char* to_string(Lifecycle l) {
    switch (l) {
        case Lifecycle::LocalPreparing: return "LOCAL_PREPARING";
        case Lifecycle::NewSentUnknown: return "NEW_SENT_UNKNOWN";
        case Lifecycle::Working: return "WORKING";
        case Lifecycle::PartFilled: return "PART_FILLED";
        case Lifecycle::PendingCancel: return "PENDING_CANCEL";
        case Lifecycle::PendingReplace: return "PENDING_REPLACE";
        case Lifecycle::Filled: return "FILLED";
        case Lifecycle::Canceled: return "CANCELED";
        case Lifecycle::Expired: return "EXPIRED";
        case Lifecycle::Rejected: return "REJECTED";
        case Lifecycle::DoneForDay: return "DONE_FOR_DAY";
        case Lifecycle::ReconciliationRequired: return "RECONCILIATION_REQUIRED";
        case Lifecycle::Quarantined: return "QUARANTINED";
    }
    return "UNKNOWN";
}

const char* to_string(BaseState s) {
    switch (s) {
        case BaseState::Preparing: return "PREPARING";
        case BaseState::SentUnknown: return "SENT_UNKNOWN";
        case BaseState::Working: return "WORKING";
        case BaseState::Done: return "DONE";
    }
    return "UNKNOWN";
}

const char* to_string(DoneReason r) {
    switch (r) {
        case DoneReason::None: return "NONE";
        case DoneReason::Filled: return "FILLED";
        case DoneReason::Canceled: return "CANCELED";
        case DoneReason::Expired: return "EXPIRED";
        case DoneReason::Rejected: return "REJECTED";
        case DoneReason::DoneForDay: return "DONE_FOR_DAY";
    }
    return "UNKNOWN";
}

const char* to_string(Confidence c) {
    switch (c) {
        case Confidence::Confirmed: return "CONFIRMED";
        case Confidence::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* to_string(PendingRequest::Phase p) {
    switch (p) {
        case PendingRequest::Phase::Prepared: return "PREPARED";
        case PendingRequest::Phase::SentUnknown: return "SENT_UNKNOWN";
        case PendingRequest::Phase::PeerPending: return "PEER_PENDING";
        case PendingRequest::Phase::Resolved: return "RESOLVED";
    }
    return "UNKNOWN";
}

Lifecycle OrderAggregate::lifecycle() const {
    // A contradiction outranks every other reading of the state: nothing about
    // a quarantined chain should look actionable.
    if (quarantined) return Lifecycle::Quarantined;

    // A proven terminal outranks an in-flight mutation. A cancel request still
    // outstanding against a fully filled order does not make the order less
    // filled; it makes the cancel's disposition a reconciliation item, which
    // the pending registry still records (what-if H-14, J-03).
    if (base == BaseState::Done) {
        switch (done_reason) {
            case DoneReason::Filled: return Lifecycle::Filled;
            case DoneReason::Canceled: return Lifecycle::Canceled;
            case DoneReason::Expired: return Lifecycle::Expired;
            case DoneReason::Rejected: return Lifecycle::Rejected;
            case DoneReason::DoneForDay: return Lifecycle::DoneForDay;
            case DoneReason::None: return Lifecycle::ReconciliationRequired;
        }
    }

    // Unknown disposition must not be presentable as a working order.
    if (reconciliation_required || confidence == Confidence::Unknown) {
        return Lifecycle::ReconciliationRequired;
    }

    // FIX precedence: Pending Cancel above Pending Replace.
    const bool cancel_pending =
        peer_pending_cancel || (pending.has_value() && pending->kind == CommandKind::Cancel);
    if (cancel_pending) return Lifecycle::PendingCancel;
    const bool replace_pending =
        peer_pending_replace || (pending.has_value() && pending->kind == CommandKind::Replace);
    if (replace_pending) return Lifecycle::PendingReplace;

    if (base == BaseState::Preparing) return Lifecycle::LocalPreparing;
    if (base == BaseState::SentUnknown) return Lifecycle::NewSentUnknown;
    return cum_qty_units > 0 ? Lifecycle::PartFilled : Lifecycle::Working;
}

int64_t OrderAggregate::ledger_cum_qty() const {
    int64_t sum = 0;
    for (const FillLedgerEntry& f : ledger) {
        if (!f.effective) continue;
        if (!checked_add(sum, f.qty_units, sum)) return -1;  // caller treats as a violation
    }
    return sum;
}

Int128 OrderAggregate::ledger_notional() const {
    Int128 sum = 0;
    for (const FillLedgerEntry& f : ledger) {
        if (!f.effective) continue;
        Int128 term = 0;
        if (!checked_mul_wide(f.qty_units, f.price_ticks, term)) return -1;
        if (!checked_add_wide(sum, term, sum)) return -1;
    }
    return sum;
}

const FillLedgerEntry* OrderAggregate::find_fill(const std::string& exec_id) const {
    for (const FillLedgerEntry& f : ledger) {
        if (f.exec_id == exec_id) return &f;
    }
    return nullptr;
}

const SeenReport* OrderAggregate::find_seen(const std::string& exec_id) const {
    for (const SeenReport& s : seen_reports) {
        if (s.exec_id == exec_id) return &s;
    }
    return nullptr;
}

bool OrderAggregate::has_alias(const std::string& peer, const std::string& order_id) const {
    const std::string qualified = peer + "|" + order_id;
    for (const std::string& a : order_id_aliases) {
        if (a == qualified) return true;
    }
    return false;
}

bool OrderAggregate::has_order_id(const std::string& order_id) const {
    const std::string suffix = "|" + order_id;
    for (const std::string& a : order_id_aliases) {
        if (a.size() >= suffix.size() &&
            a.compare(a.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

bool OrderAggregate::avg_px_rational(Int128& numerator, Int128& denominator) const {
    if (cum_qty_units <= 0) return false;
    numerator = notional_ticks_x_qty;
    denominator = cum_qty_units;
    return true;
}

bool OrderAggregate::avg_px_ticks_rounded(int64_t& out) const {
    Int128 num = 0;
    Int128 den = 0;
    if (!avg_px_rational(num, den)) return false;
    Int128 rounded = 0;
    if (!divide_round_half_even(num, den, rounded)) return false;
    if (!int128_fits_int64(rounded)) return false;
    out = static_cast<int64_t>(rounded);
    return true;
}

std::string OrderAggregate::canonical_text() const {
    std::string s;
    const auto field = [&s](const std::string& k, const std::string& v) {
        s += k;
        s.push_back('=');
        s += v;
        s.push_back('\x1f');
    };

    field("firm", firm);
    field("day", business_day);
    field("account", account);
    field("symbol", symbol);
    field("side", to_string(side));
    field("initial", initial_cl_ord_id);
    field("peer", peer_id);
    field("base", to_string(base));
    field("done", to_string(done_reason));
    field("cum", std::to_string(cum_qty_units));
    field("notional", to_string(notional_ticks_x_qty));
    field("leaves", std::to_string(leaves_qty_units()));
    field("confidence", confidence == Confidence::Confirmed ? "CONFIRMED" : "UNKNOWN");
    field("reconcile", reconciliation_required ? "1" : "0");
    field("recon_gen", std::to_string(reconciliation_generation));
    field("quarantined", quarantined ? "1" : "0");
    field("peer_pc", peer_pending_cancel ? "1" : "0");
    field("peer_pr", peer_pending_replace ? "1" : "0");

    field("versions", std::to_string(accepted_versions.size()));
    for (const AcceptedVersion& v : accepted_versions) {
        field("v", std::to_string(v.version) + "/" + v.cl_ord_id + "/" +
                       std::to_string(v.qty_units) + "/" + std::to_string(v.price_ticks) + "/" +
                       std::to_string(v.scale_id) + "/" + std::to_string(v.policy_version));
    }

    if (pending.has_value()) {
        field("pending", std::string(to_string(pending->kind)) + "/" + pending->cl_ord_id + "/" +
                             pending->orig_cl_ord_id + "/" + to_string(pending->phase) + "/" +
                             std::to_string(pending->desired_qty_units) + "/" +
                             std::to_string(pending->desired_price_ticks) + "/" +
                             (pending->outcome_unknown ? "unknown" : "known"));
    } else {
        field("pending", "none");
    }

    // The ledger is ordered by application, which is part of the business
    // state: two traces that apply the same fills in a different order are not
    // the same history.
    field("fills", std::to_string(ledger.size()));
    for (const FillLedgerEntry& f : ledger) {
        field("f", f.exec_id + "/" + f.cl_ord_id + "/" + std::to_string(f.qty_units) + "/" +
                       std::to_string(f.price_ticks) + "/" +
                       std::to_string(f.accepted_version_seen) + "/" + (f.effective ? "1" : "0"));
    }
    field("aliases", std::to_string(order_id_aliases.size()));
    for (const std::string& a : order_id_aliases) field("a", a);

    // Incident reasons are part of the safety-relevant state; their count and
    // codes are included, their free-text detail is not.
    field("incidents", std::to_string(incidents.size()));
    field("resolved_incidents", std::to_string(resolved_incident_count));
    for (const QuarantineIncident& i : incidents) field("i", i.reason_code);

    // Deliberately excluded: revision, held_events, reconcile_attempts,
    // max_source_seq, last_applied_event_index, seen_reports. These are
    // bookkeeping about how the state was reached, not the business state.
    return s;
}

}  // namespace order_core
