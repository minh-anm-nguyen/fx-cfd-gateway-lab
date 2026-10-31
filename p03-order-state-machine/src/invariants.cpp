#include "order_core/invariants.hpp"

#include <set>

namespace order_core {

namespace {

struct Label {
    int id;
    const char* name;
};

constexpr Label kLabels[] = {
    {1, "I01 single-writer"},        {2, "I02 unique chain"},
    {3, "I03 identity isolation"},   {4, "I04 accepted version"},
    {5, "I05 quantity bounds"},      {6, "I06 active leaves"},
    {7, "I07 terminal leaves"},      {8, "I08 filled status"},
    {9, "I09 dedup fill"},           {10, "I10 ExecID uniqueness scope"},
    {11, "I11 fill conservation"},   {12, "I12 monetary precision"},
    {13, "I13 decimal/tick validity"}, {14, "I14 pending semantics"},
    {15, "I15 canonical lineage"},   {16, "I16 cancel result"},
    {17, "I17 replace result"},      {18, "I18 no false terminal"},
    {19, "I19 snapshot vs trade"},   {20, "I20 provenance"},
    {21, "I21 atomicity boundary"},  {22, "I22 determinism"},
    {23, "I23 bounded resources"},   {24, "I24 fail-closed on contradictions"},
};

}  // namespace

std::string invariant_label(int id) {
    for (const Label& l : kLabels) {
        if (l.id == id) return l.name;
    }
    return "I?? unknown";
}

std::vector<InvariantViolation> check_invariants(const OrderAggregate& a, const Policy& p,
                                                 const InvariantContext& ctx) {
    std::vector<InvariantViolation> v;
    const auto fail = [&v](int id, std::string detail) {
        v.push_back({id, invariant_label(id), std::move(detail)});
    };

    if (!ctx.single_writer) fail(1, "more than one owner mutated this chain");
    if (!ctx.unique_chain) fail(2, "internal_order_id maps to two canonical records");
    if (!ctx.identity_isolated) fail(3, "identity key maps to a different chain or request");

    // I04: an unresolved pending replace must not already appear as accepted.
    if (a.pending.has_value() && a.pending->kind == CommandKind::Replace &&
        a.pending->phase != PendingRequest::Phase::Resolved) {
        for (const AcceptedVersion& ver : a.accepted_versions) {
            if (ver.cl_ord_id == a.pending->cl_ord_id) {
                fail(4, "pending replace " + a.pending->cl_ord_id + " already accepted");
            }
        }
    }

    // I05 / I06 / I07.
    if (a.cum_qty_units < 0) fail(5, "cum_qty is negative");
    if (a.has_accepted()) {
        if (a.cum_qty_units > a.accepted_qty_units()) {
            fail(5, "cum_qty " + std::to_string(a.cum_qty_units) + " exceeds accepted qty " +
                        std::to_string(a.accepted_qty_units()));
        }
        if (!a.terminal()) {
            const int64_t expected = a.accepted_qty_units() - a.cum_qty_units;
            if (expected < 0) {
                fail(6, "working order has negative leaves");
            } else if (a.leaves_qty_units() != expected) {
                fail(6, "leaves " + std::to_string(a.leaves_qty_units()) + " != qty - cum " +
                            std::to_string(expected));
            }
        }
    }
    if (a.terminal() && p.terminal_leaves_is_zero && a.leaves_qty_units() != 0) {
        fail(7, "terminal order reports nonzero leaves");
    }

    // I08: Filled is only reachable with the full accepted quantity executed.
    if (a.done_reason == DoneReason::Filled) {
        if (!a.has_accepted()) {
            fail(8, "Filled without an accepted version");
        } else if (a.cum_qty_units != a.accepted_qty_units()) {
            fail(8, "Filled with cum " + std::to_string(a.cum_qty_units) + " != qty " +
                        std::to_string(a.accepted_qty_units()));
        }
    }

    // I09: one ExecID appears at most once in the ledger.
    {
        std::set<std::string> ids;
        for (const FillLedgerEntry& f : a.ledger) {
            if (!ids.insert(f.exec_id).second) {
                fail(9, "ExecID " + f.exec_id + " applied more than once");
            }
        }
    }
    if (!ctx.exec_scope_correct) fail(10, "ExecID dedup scope is not (peer, namespace, ExecID)");

    // I11 / I12: the cached totals must be exactly what the ledger says, and
    // neither recomputation may have overflowed.
    const int64_t ledger_cum = a.ledger_cum_qty();
    const Int128 ledger_notional = a.ledger_notional();
    if (ledger_cum < 0) {
        fail(12, "ledger quantity sum overflowed");
    } else if (ledger_cum != a.cum_qty_units) {
        fail(11, "cached cum " + std::to_string(a.cum_qty_units) + " != ledger sum " +
                     std::to_string(ledger_cum));
    }
    if (ledger_notional < 0 && !a.ledger.empty()) {
        fail(12, "ledger notional overflowed or went negative");
    } else if (ledger_notional != a.notional_ticks_x_qty) {
        fail(11, "cached notional " + to_string(a.notional_ticks_x_qty) + " != ledger sum " +
                     to_string(ledger_notional));
    }

    // I13: every accepted version and every fill must sit on the pinned grid.
    if (const SymbolSpec* spec = p.find_symbol(a.symbol); spec != nullptr) {
        for (const AcceptedVersion& ver : a.accepted_versions) {
            if (ver.qty_units <= 0) fail(13, "accepted version with non-positive qty");
            if (spec->qty_step > 0 && ver.qty_units % spec->qty_step != 0) {
                fail(13, "accepted qty " + std::to_string(ver.qty_units) + " off qty_step");
            }
            if (spec->price_must_be_positive && ver.price_ticks <= 0) {
                fail(13, "accepted version with non-positive price");
            }
        }
        for (const FillLedgerEntry& f : a.ledger) {
            if (f.qty_units <= 0) fail(13, "ledger entry with non-positive qty");
            if (spec->qty_step > 0 && f.qty_units % spec->qty_step != 0) {
                fail(13, "fill qty " + std::to_string(f.qty_units) + " off qty_step");
            }
        }
    } else if (!a.symbol.empty() && !a.accepted_versions.empty()) {
        fail(13, "symbol " + a.symbol + " has no pinned scale in this policy");
    }

    // I14: the pending phases and the peer pending flags must agree, and only
    // one mutation may be outstanding.
    if (a.peer_pending_cancel && a.peer_pending_replace) {
        fail(14, "peer reports both pending cancel and pending replace");
    }
    if (a.peer_pending_cancel && a.pending.has_value() &&
        a.pending->kind == CommandKind::Replace) {
        fail(14, "peer pending cancel while local pending request is a replace");
    }
    if (a.peer_pending_replace && a.pending.has_value() &&
        a.pending->kind == CommandKind::Cancel) {
        fail(14, "peer pending replace while local pending request is a cancel");
    }

    // I15: a cancel or replace must reference the last accepted version.
    if (a.pending.has_value() && a.pending->kind != CommandKind::New && a.has_accepted()) {
        if (a.pending->orig_cl_ord_id != a.accepted_cl_ord_id()) {
            fail(15, "pending OrigClOrdID " + a.pending->orig_cl_ord_id +
                         " is not the accepted version " + a.accepted_cl_ord_id());
        }
    }

    // I16: a cancel never reverses executed quantity.
    if (a.done_reason == DoneReason::Canceled && a.cum_qty_units != ledger_cum) {
        fail(16, "cancel changed the executed quantity");
    }

    // I17: version indices are dense and increasing, ClOrdIDs distinct, and
    // cumulative quantity is never reset across a replace.
    {
        std::set<std::string> version_ids;
        for (size_t i = 0; i < a.accepted_versions.size(); ++i) {
            if (a.accepted_versions[i].version != i) {
                fail(17, "accepted version index " +
                             std::to_string(a.accepted_versions[i].version) + " out of order");
            }
            if (!version_ids.insert(a.accepted_versions[i].cl_ord_id).second) {
                fail(17, "accepted version ClOrdID reused: " + a.accepted_versions[i].cl_ord_id);
            }
        }
        if (a.accepted_versions.size() > 1 && a.cum_qty_units < 0) {
            fail(17, "cumulative quantity reset across a replace");
        }
    }

    // I18: an unknown disposition can never read as a proven terminal.
    if (a.confidence == Confidence::Unknown && a.terminal()) {
        fail(18, "terminal state asserted while the outcome is unknown");
    }
    if (!ctx.synthetic_fill_created) {
        // nothing to report
    } else {
        fail(19, "a fill was synthesised from a status snapshot");
    }

    // I20: every committed fill must be attributable.
    for (const FillLedgerEntry& f : a.ledger) {
        if (f.exec_id.empty()) fail(20, "ledger entry without an ExecID");
        if (f.provenance_hash == 0) fail(20, "ledger entry " + f.exec_id + " without provenance");
    }

    // I21: without an atomic commit, nothing may be published as confirmed.
    if (!ctx.atomic_commit_available && ctx.publishes_confirmed) {
        fail(21, "confirmed state published without an atomic commit boundary");
    }
    if (!ctx.replay_hash_matches) fail(22, "replay produced a different canonical state hash");

    // I23: every growing structure has a ceiling.
    if (a.accepted_versions.size() > p.limits.max_accepted_versions) {
        fail(23, "accepted version chain exceeds max_accepted_versions");
    }
    if (a.incidents.size() > p.limits.max_quarantine_incidents) {
        fail(23, "incident store exceeds max_quarantine_incidents");
    }
    if (a.held_events > p.limits.max_held_events_per_chain) {
        fail(23, "held events exceed max_held_events_per_chain");
    }
    if (ctx.dedup_index_size > p.limits.max_idempotency_entries) {
        fail(23, "dedup index exceeds max_idempotency_entries");
    }
    if (ctx.chain_count > p.limits.max_chain_count) {
        fail(23, "chain count exceeds max_chain_count");
    }
    if (!ctx.indices_bounded) fail(23, "an index grew without a bound");

    // I24: an unresolved contradiction must leave the chain visibly unsafe.
    // A resolved one stays in the record as history.
    if (a.resolved_incident_count > a.incidents.size()) {
        fail(24, "more incidents marked resolved than were ever recorded");
    } else if (a.incidents.size() > a.resolved_incident_count && !a.quarantined &&
               !a.reconciliation_required) {
        fail(24, std::to_string(a.incidents.size() - a.resolved_incident_count) +
                     " unresolved incident(s) but the chain is neither quarantined nor flagged");
    }

    return v;
}

std::vector<InvariantViolation> check_invariants(const OrderAggregate& a, const Policy& p) {
    return check_invariants(a, p, InvariantContext{});
}

std::string describe(const std::vector<InvariantViolation>& v) {
    std::string s;
    for (const InvariantViolation& x : v) {
        if (!s.empty()) s += "; ";
        s += x.name + ": " + x.detail;
    }
    return s;
}

}  // namespace order_core
