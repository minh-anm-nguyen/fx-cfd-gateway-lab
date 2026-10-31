#include "order_core/reducer.hpp"

#include <algorithm>

#include "order_core/decimal.hpp"

namespace order_core {

const char* to_string(Decision d) {
    switch (d) {
        case Decision::Applied: return "APPLIED";
        case Decision::DuplicateNoop: return "DUPLICATE_NOOP";
        case Decision::HeldForCausality: return "HELD_FOR_CAUSALITY";
        case Decision::Quarantined: return "QUARANTINED";
        case Decision::RetryCommit: return "RETRY_COMMIT";
        case Decision::RejectLocal: return "REJECT_LOCAL";
    }
    return "UNKNOWN";
}

const char* to_string(Reason r) {
    switch (r) {
        case Reason::None: return "NONE";
        case Reason::InvalidQty: return "INVALID_QTY";
        case Reason::InvalidPrice: return "INVALID_PRICE";
        case Reason::InvalidSide: return "INVALID_SIDE";
        case Reason::MissingField: return "MISSING_FIELD";
        case Reason::UnknownSymbol: return "UNKNOWN_SYMBOL";
        case Reason::QtyStepMismatch: return "QTY_STEP_MISMATCH";
        case Reason::QtyOutOfRange: return "QTY_OUT_OF_RANGE";
        case Reason::IdTooLong: return "ID_TOO_LONG";
        case Reason::IdCollision: return "ID_COLLISION";
        case Reason::LineageMismatch: return "LINEAGE_MISMATCH";
        case Reason::PendingMutation: return "PENDING_MUTATION";
        case Reason::TerminalState: return "TERMINAL_STATE";
        case Reason::UnsafeDependentCommand: return "UNSAFE_DEPENDENT_COMMAND";
        case Reason::QtyBelowCum: return "QTY_BELOW_CUM";
        case Reason::ZeroWorkingLeaves: return "ZERO_WORKING_LEAVES";
        case Reason::ResourceExhausted: return "RESOURCE_EXHAUSTED";
        case Reason::NotAuthorized: return "NOT_AUTHORIZED";
        case Reason::DuplicateSamePayload: return "DUPLICATE_SAME_PAYLOAD";
        case Reason::ConflictingDuplicate: return "CONFLICTING_DUPLICATE";
        case Reason::Overfill: return "OVERFILL";
        case Reason::InvalidLastQty: return "INVALID_LAST_QTY";
        case Reason::CumRegression: return "CUM_REGRESSION";
        case Reason::MissingFillEvidence: return "MISSING_FILL_EVIDENCE";
        case Reason::CumMismatch: return "CUM_MISMATCH";
        case Reason::LeavesInconsistent: return "LEAVES_INCONSISTENT";
        case Reason::AccountMismatch: return "ACCOUNT_MISMATCH";
        case Reason::PeerMismatch: return "PEER_MISMATCH";
        case Reason::SymbolMismatch: return "SYMBOL_MISMATCH";
        case Reason::SideMismatch: return "SIDE_MISMATCH";
        case Reason::IdentityInsufficient: return "IDENTITY_INSUFFICIENT";
        case Reason::AliasCollision: return "ALIAS_COLLISION";
        case Reason::UnknownRequest: return "UNKNOWN_REQUEST";
        case Reason::UnexpectedPending: return "UNEXPECTED_PENDING";
        case Reason::ObsoleteOrdStatus: return "OBSOLETE_ORD_STATUS";
        case Reason::WrongFixVersionSemantics: return "WRONG_FIX_VERSION_SEMANTICS";
        case Reason::UnsupportedEvent: return "UNSUPPORTED_EVENT";
        case Reason::UnknownExecType: return "UNKNOWN_EXEC_TYPE";
        case Reason::MissingCorrectionReference: return "MISSING_CORRECTION_REFERENCE";
        case Reason::UnknownCorrectionReference: return "UNKNOWN_CORRECTION_REFERENCE";
        case Reason::PriceOutsideLimit: return "PRICE_OUTSIDE_LIMIT";
        case Reason::ScaleMismatch: return "SCALE_MISMATCH";
        case Reason::PostTerminalEvent: return "POST_TERMINAL_EVENT";
        case Reason::LateReportNoRegression: return "LATE_REPORT_NO_REGRESSION";
        case Reason::ContradictoryResponse: return "CONTRADICTORY_RESPONSE";
        case Reason::StatusInconsistent: return "STATUS_INCONSISTENT";
        case Reason::MissingSourceCursor: return "MISSING_SOURCE_CURSOR";
        case Reason::StaleGeneration: return "STALE_GENERATION";
        case Reason::AwaitingCorrelation: return "AWAITING_CORRELATION";
        case Reason::AwaitingFillEvidence: return "AWAITING_FILL_EVIDENCE";
        case Reason::UnknownOutcome: return "UNKNOWN_OUTCOME";
        case Reason::InvariantViolated: return "INVARIANT_VIOLATED";
        case Reason::MalformedNumber: return "MALFORMED_NUMBER";
        case Reason::ProtocolMismatch: return "PROTOCOL_MISMATCH";
    }
    return "UNKNOWN";
}

const char* to_string(EffectIntent::Kind k) {
    switch (k) {
        case EffectIntent::Kind::RouteNew: return "ROUTE_NEW";
        case EffectIntent::Kind::RouteCancel: return "ROUTE_CANCEL";
        case EffectIntent::Kind::RouteReplace: return "ROUTE_REPLACE";
        case EffectIntent::Kind::RiskReserve: return "RISK_RESERVE";
        case EffectIntent::Kind::RiskAdjust: return "RISK_ADJUST";
        case EffectIntent::Kind::RiskRelease: return "RISK_RELEASE";
        case EffectIntent::Kind::QueryOrderStatus: return "QUERY_ORDER_STATUS";
        case EffectIntent::Kind::ReconciliationRequired: return "RECONCILIATION_REQUIRED";
        case EffectIntent::Kind::Alert: return "ALERT";
        case EffectIntent::Kind::Audit: return "AUDIT";
    }
    return "UNKNOWN";
}

namespace {

// Ceiling on a durable dedup key, independent of the per-field ceilings: the
// index must stay bounded whatever arrives on the wire.
constexpr size_t kMaxSemanticKeyBytes = 512;

// ---------------------------------------------------------------------------
// Result builders
// ---------------------------------------------------------------------------

Reduction unchanged(const OrderAggregate& prev, Decision d, Reason r, std::string detail) {
    Reduction out;
    out.next = prev;
    out.decision = d;
    out.reason = r;
    out.detail = std::move(detail);
    return out;
}

// Quarantine still writes: the incident, the reconciliation flag and the
// evidence must be durable, otherwise the contradiction disappears on restart.
// What does not change is any business quantity.
Reduction quarantine(const OrderAggregate& prev, Reason r, const std::string& detail,
                     const std::string& exec_id, const std::string& cl_ord_id) {
    Reduction out;
    out.next = prev;
    out.decision = Decision::Quarantined;
    out.reason = r;
    out.detail = detail;
    out.next.quarantined = true;
    out.next.reconciliation_required = true;
    QuarantineIncident incident;
    incident.reason_code = to_string(r);
    incident.detail = detail;
    incident.exec_id = exec_id;
    incident.cl_ord_id = cl_ord_id;
    incident.at_revision = prev.revision;
    out.next.incidents.push_back(incident);
    const std::string key = "quarantine|" + std::to_string(prev.id.value) + "|" +
                            std::to_string(prev.revision) + "|" + to_string(r);
    out.intents.push_back({EffectIntent::Kind::Alert, key, detail});
    out.intents.push_back(
        {EffectIntent::Kind::ReconciliationRequired, key + "|reconcile", detail});
    return out;
}

Reduction reject_local(const OrderAggregate& prev, Reason r, std::string detail) {
    return unchanged(prev, Decision::RejectLocal, r, std::move(detail));
}

Reduction hold(const OrderAggregate& prev, Reason r, std::string detail) {
    Reduction out = unchanged(prev, Decision::HeldForCausality, r, std::move(detail));
    out.next.held_events = prev.held_events + 1;
    return out;
}

// ---------------------------------------------------------------------------
// Field validation
// ---------------------------------------------------------------------------

// An identifier must be present, within its byte ceiling, and free of control
// bytes. A NUL inside a ClOrdID would make a truncated comparison succeed
// against a different order (what-if O-12, E-20).
bool identifier_ok(const std::string& s, size_t max_bytes) {
    if (s.empty() || s.size() > max_bytes) return false;
    for (const unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

struct QtyCheck {
    Reason reason = Reason::None;
    int64_t units = 0;
};

QtyCheck check_qty(const std::string& text, const SymbolSpec& spec) {
    QtyCheck out;
    const DecimalError e = parse_units(text, out.units);
    if (e != DecimalError::None) {
        out.reason = (e == DecimalError::Overflow || e == DecimalError::TooManyDigits)
                         ? Reason::MalformedNumber
                         : Reason::InvalidQty;
        return out;
    }
    if (out.units <= 0) {
        out.reason = Reason::InvalidQty;
        return out;
    }
    if (spec.qty_step > 0 && out.units % spec.qty_step != 0) {
        out.reason = Reason::QtyStepMismatch;
        return out;
    }
    if (out.units < spec.min_qty) {
        out.reason = Reason::QtyOutOfRange;
        return out;
    }
    if (spec.max_qty != 0 && out.units > spec.max_qty) {
        out.reason = Reason::QtyOutOfRange;
        return out;
    }
    return out;
}

struct PxCheck {
    Reason reason = Reason::None;
    int64_t ticks = 0;
};

PxCheck check_price(const std::string& text, const SymbolSpec& spec) {
    PxCheck out;
    const DecimalError e =
        parse_ticks(text, spec.tick_numerator, spec.tick_denominator, out.ticks);
    if (e != DecimalError::None) {
        switch (e) {
            case DecimalError::Overflow:
            case DecimalError::TooManyDigits:
                out.reason = Reason::MalformedNumber;
                break;
            case DecimalError::OffTick:
                out.reason = Reason::ScaleMismatch;
                break;
            default:
                out.reason = Reason::InvalidPrice;
                break;
        }
        return out;
    }
    if (spec.price_must_be_positive && out.ticks <= 0) {
        out.reason = Reason::InvalidPrice;
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Local commands
// ---------------------------------------------------------------------------

Reduction reduce_new(const OrderAggregate& prev, const LocalCommand& c, const Policy& pol,
                     uint64_t event_index) {
    if (prev.has_accepted() || prev.pending.has_value() || prev.base != BaseState::Preparing) {
        // The caller handed a New for a chain that already exists. Correlation
        // should have caught it; refuse rather than restart the chain.
        return reject_local(prev, Reason::IdCollision,
                            "New for an existing chain " + c.cl_ord_id);
    }
    if (c.firm.empty() || c.business_day.empty() || c.account.empty() || c.symbol.empty()) {
        return reject_local(prev, Reason::MissingField, "firm/day/account/symbol required");
    }
    if (!identifier_ok(c.cl_ord_id, pol.limits.max_cl_ord_id_bytes)) {
        return reject_local(prev, Reason::IdTooLong, "ClOrdID absent, oversized or non-printable");
    }
    const Side side = side_from_fix(c.side_raw);
    if (side == Side::Unspecified) {
        return reject_local(prev, Reason::InvalidSide, "Side(54)='" + c.side_raw + "'");
    }
    const SymbolSpec* spec = pol.find_symbol(c.symbol);
    if (spec == nullptr) {
        return reject_local(prev, Reason::UnknownSymbol, c.symbol + " has no pinned scale");
    }
    const QtyCheck q = check_qty(c.qty_text, *spec);
    if (q.reason != Reason::None) {
        return reject_local(prev, q.reason, "OrderQty='" + c.qty_text + "'");
    }
    const PxCheck px = check_price(c.price_text, *spec);
    if (px.reason != Reason::None) {
        return reject_local(prev, px.reason, "Price='" + c.price_text + "'");
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.firm = c.firm;
    out.next.business_day = c.business_day;
    out.next.account = c.account;
    out.next.symbol = c.symbol;
    out.next.side = side;
    out.next.initial_cl_ord_id = c.cl_ord_id;
    out.next.new_intent_fingerprint = intent_fingerprint(c);
    out.next.base = BaseState::Preparing;

    PendingRequest p;
    p.kind = CommandKind::New;
    p.phase = PendingRequest::Phase::Prepared;
    p.cl_ord_id = c.cl_ord_id;
    p.desired_qty_units = q.units;
    p.desired_price_ticks = px.ticks;
    p.desired_qty_present = true;
    p.desired_price_present = true;
    p.dispatch_intent_id = event_index;
    out.next.pending = p;

    const std::string key = c.request_key().text();
    out.intents.push_back({EffectIntent::Kind::RouteNew, "route-new|" + key, key});
    out.intents.push_back({EffectIntent::Kind::RiskReserve, "risk-reserve|" + key,
                           c.account + "|" + c.symbol + "|" + std::to_string(q.units)});
    return out;
}

// Shared gate for cancel and replace: the order must exist, be known, be
// non-terminal and have no other mutation outstanding.
Reason mutation_gate(const OrderAggregate& prev, const LocalCommand& c, const Policy& pol,
                     std::string& detail) {
    if (!identifier_ok(c.cl_ord_id, pol.limits.max_cl_ord_id_bytes)) {
        detail = "ClOrdID absent, oversized or non-printable";
        return Reason::IdTooLong;
    }
    if (c.orig_cl_ord_id.empty()) {
        detail = "OrigClOrdID(41) required";
        return Reason::MissingField;
    }
    if (prev.quarantined) {
        detail = "chain is quarantined";
        return Reason::TerminalState;
    }
    if (prev.terminal()) {
        detail = std::string("order is already ") + to_string(prev.done_reason);
        return Reason::TerminalState;
    }
    if (!prev.has_accepted()) {
        // No accepted version means the initial request's fate is unknown.
        // Acting on it would be a blind dependent command (what-if H-05).
        detail = "no accepted version yet; reconcile the New first";
        return Reason::UnsafeDependentCommand;
    }
    if (prev.confidence == Confidence::Unknown) {
        detail = "order disposition is unknown; reconcile first";
        return Reason::UnsafeDependentCommand;
    }
    if (prev.pending.has_value()) {
        detail = std::string("a ") + to_string(prev.pending->kind) + " request is already pending";
        return Reason::PendingMutation;
    }
    if (c.orig_cl_ord_id != prev.accepted_cl_ord_id()) {
        // Strict v1: OrigClOrdID must be the last accepted version, never a
        // pending one and never a superseded one (what-if E-06, E-08, I-09).
        detail = "OrigClOrdID " + c.orig_cl_ord_id + " is not the accepted version " +
                 prev.accepted_cl_ord_id();
        return Reason::LineageMismatch;
    }
    return Reason::None;
}

Reduction reduce_cancel(const OrderAggregate& prev, const LocalCommand& c, const Policy& pol,
                        uint64_t event_index) {
    std::string detail;
    if (const Reason r = mutation_gate(prev, c, pol, detail); r != Reason::None) {
        return reject_local(prev, r, detail);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    PendingRequest p;
    p.kind = CommandKind::Cancel;
    p.phase = PendingRequest::Phase::Prepared;
    p.cl_ord_id = c.cl_ord_id;
    p.orig_cl_ord_id = c.orig_cl_ord_id;
    p.dispatch_intent_id = event_index;
    out.next.pending = p;

    const std::string key = c.request_key().text();
    out.intents.push_back({EffectIntent::Kind::RouteCancel, "route-cancel|" + key, key});
    return out;
}

Reduction reduce_replace(const OrderAggregate& prev, const LocalCommand& c, const Policy& pol,
                         uint64_t event_index) {
    std::string detail;
    if (const Reason r = mutation_gate(prev, c, pol, detail); r != Reason::None) {
        return reject_local(prev, r, detail);
    }
    const SymbolSpec* spec = pol.find_symbol(prev.symbol);
    if (spec == nullptr) {
        return reject_local(prev, Reason::UnknownSymbol,
                            prev.symbol + " scale disappeared from config");
    }

    // A replace may change quantity, price or both; an absent field means
    // "keep the accepted value".
    int64_t qty = prev.accepted_qty_units();
    bool qty_present = false;
    if (!c.qty_text.empty()) {
        const QtyCheck q = check_qty(c.qty_text, *spec);
        if (q.reason != Reason::None) {
            return reject_local(prev, q.reason, "OrderQty='" + c.qty_text + "'");
        }
        qty = q.units;
        qty_present = true;
    }
    int64_t px = prev.accepted_price_ticks();
    bool px_present = false;
    if (!c.price_text.empty()) {
        const PxCheck p = check_price(c.price_text, *spec);
        if (p.reason != Reason::None) {
            return reject_local(prev, p.reason, "Price='" + c.price_text + "'");
        }
        px = p.ticks;
        px_present = true;
    }

    if (qty < prev.cum_qty_units) {
        // Strict v1 refuses to ask for a quantity already executed. Real
        // counterparties differ; the choice is pinned in the Rules of
        // Engagement (what-if I-06).
        if (pol.reject_qty_below_cum_on_replace) {
            return reject_local(prev, Reason::QtyBelowCum,
                                "new qty " + std::to_string(qty) + " below CumQty " +
                                    std::to_string(prev.cum_qty_units));
        }
    } else if (qty == prev.cum_qty_units && prev.cum_qty_units > 0 &&
               pol.reject_replace_qty_equal_cum) {
        // Leaves would become zero, which is a cancel expressed as a replace.
        // The boundary is a policy decision, made explicit here (what-if I-05).
        return reject_local(prev, Reason::ZeroWorkingLeaves,
                            "new qty equals CumQty; use cancel instead");
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    PendingRequest p;
    p.kind = CommandKind::Replace;
    p.phase = PendingRequest::Phase::Prepared;
    p.cl_ord_id = c.cl_ord_id;
    p.orig_cl_ord_id = c.orig_cl_ord_id;
    p.desired_qty_units = qty;
    p.desired_price_ticks = px;
    p.desired_qty_present = qty_present;
    p.desired_price_present = px_present;
    p.dispatch_intent_id = event_index;
    out.next.pending = p;

    const std::string key = c.request_key().text();
    out.intents.push_back({EffectIntent::Kind::RouteReplace, "route-replace|" + key, key});
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Peer reports
// ---------------------------------------------------------------------------

namespace {

// Dedup key text for a report that carries no ExecID (35=9 has none).
std::string reject_seen_key(const PeerReport& r) {
    return "rej:" + r.cl_ord_id + ":" + r.cxl_rej_response_to;
}

std::string report_seen_key(const PeerReport& r) {
    return r.msg_type == '9' ? reject_seen_key(r) : r.exec_id;
}

// Does the report's declared identity match the chain it was correlated to?
// Checked before anything is applied, so a spoofed or misrouted report cannot
// touch another account's order (invariant I03, what-if E-14 to E-16, M-04).
Reason identity_consistent(const OrderAggregate& prev, const PeerReport& r, std::string& detail) {
    if (!r.has_source_cursor) {
        detail = "normalized event carries no source cursor";
        return Reason::MissingSourceCursor;
    }
    if (!prev.peer_id.empty() && r.peer_id != prev.peer_id) {
        detail = "report from peer " + r.peer_id + " on a chain owned by " + prev.peer_id;
        return Reason::PeerMismatch;
    }
    if (!r.account.empty() && !prev.account.empty() && r.account != prev.account) {
        detail = "report account " + r.account + " != chain account " + prev.account;
        return Reason::AccountMismatch;
    }
    if (!r.symbol.empty() && !prev.symbol.empty() && r.symbol != prev.symbol) {
        detail = "report symbol " + r.symbol + " != chain symbol " + prev.symbol;
        return Reason::SymbolMismatch;
    }
    if (!r.side_raw.empty() && prev.side != Side::Unspecified) {
        const Side s = side_from_fix(r.side_raw);
        if (s == Side::Unspecified) {
            detail = "Side(54)='" + r.side_raw + "' is not a recognised code";
            return Reason::SideMismatch;
        }
        if (s != prev.side) {
            detail = std::string("report side ") + to_string(s) + " != chain side " +
                     to_string(prev.side);
            return Reason::SideMismatch;
        }
    }
    return Reason::None;
}

// OrdStatus(39) must be a code this profile knows, and must not contradict the
// ExecType it arrived with.
Reason status_consistent(const PeerReport& r, std::string& detail) {
    if (r.ord_status.empty()) return Reason::None;  // 35=9 may omit it
    if (r.ord_status == ord_status::kReplacedObsolete) {
        // FIX 4.4 no longer represents current state as Replaced; ExecType=5
        // carries the replacement event (design doc 3.1).
        detail = "OrdStatus=5 (Replaced) is not a FIX 4.4 current state";
        return Reason::ObsoleteOrdStatus;
    }
    if (ord_status_precedence(r.ord_status) == 0) {
        detail = "OrdStatus='" + r.ord_status + "' is not a configured code";
        return Reason::UnsupportedEvent;
    }
    if (r.exec_type == exec_type::kTrade) {
        // A trade can be reported while a cancel or replace is pending, and
        // while partially or fully filled. It cannot be reported as Rejected,
        // Canceled or Expired (what-if O-08).
        const bool ok = r.ord_status == ord_status::kPartiallyFilled ||
                        r.ord_status == ord_status::kFilled ||
                        r.ord_status == ord_status::kPendingCancel ||
                        r.ord_status == ord_status::kPendingReplace ||
                        r.ord_status == ord_status::kDoneForDay;
        if (!ok) {
            detail = "ExecType=F with OrdStatus=" + r.ord_status;
            return Reason::StatusInconsistent;
        }
    }
    return Reason::None;
}

struct OptNum {
    bool present = false;
    bool malformed = false;
    int64_t value = 0;
};

OptNum opt_units(const std::optional<std::string>& field) {
    OptNum out;
    if (!field.has_value()) return out;
    out.present = true;
    if (parse_units(*field, out.value) != DecimalError::None) out.malformed = true;
    return out;
}

OptNum opt_ticks(const std::optional<std::string>& field, const SymbolSpec& spec) {
    OptNum out;
    if (!field.has_value()) return out;
    out.present = true;
    if (parse_ticks(*field, spec.tick_numerator, spec.tick_denominator, out.value) !=
        DecimalError::None) {
        out.malformed = true;
    }
    return out;
}

void record_seen(OrderAggregate& a, const PeerReport& r, uint64_t fingerprint) {
    const std::string key = report_seen_key(r);
    if (key.empty()) return;
    a.seen_reports.push_back({key, fingerprint});
}

void advance_cursor(OrderAggregate& a, const PeerReport& r) {
    a.max_source_seq = std::max(a.max_source_seq, r.source_seq);
}

void add_alias(OrderAggregate& a, const PeerReport& r) {
    if (r.order_id.empty() || a.has_alias(r.peer_id, r.order_id)) return;
    a.order_id_aliases.push_back(r.peer_id + "|" + r.order_id);
}

// --- individual report handlers --------------------------------------------

Reduction on_new_ack(const OrderAggregate& prev, const PeerReport& r, const Policy& pol,
                     uint64_t event_index) {
    if (prev.has_accepted()) {
        // An acknowledgement for a version already installed is late, not new.
        // Nothing regresses (what-if B-17, G-16).
        if (r.cl_ord_id == prev.accepted_versions.front().cl_ord_id) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                             "New Ack after the version was already accepted");
        }
        return quarantine(prev, Reason::ConflictingDuplicate,
                          "New Ack for " + r.cl_ord_id + " on a chain already accepted as " +
                              prev.accepted_cl_ord_id(),
                          r.exec_id, r.cl_ord_id);
    }
    if (r.order_id.empty()) {
        // OrderID(37) is mandatory on an ExecutionReport. Accepting a version
        // without the counterparty's own identity leaves nothing to correlate
        // later with (what-if A-15).
        return quarantine(prev, Reason::IdentityInsufficient,
                          "New Ack without OrderID(37)", r.exec_id, r.cl_ord_id);
    }
    if (!prev.pending.has_value() || prev.pending->kind != CommandKind::New) {
        return quarantine(prev, Reason::UnknownRequest,
                          "New Ack with no New request outstanding", r.exec_id, r.cl_ord_id);
    }
    if (r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "New Ack ClOrdID " + r.cl_ord_id + " != requested " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }

    const SymbolSpec* spec = pol.find_symbol(prev.symbol);
    if (spec == nullptr) {
        return quarantine(prev, Reason::UnknownSymbol, "no pinned scale for " + prev.symbol,
                          r.exec_id, r.cl_ord_id);
    }

    const OptNum cum = opt_units(r.cum_qty);
    if (cum.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "CumQty='" + *r.cum_qty + "'", r.exec_id,
                          r.cl_ord_id);
    }
    if (cum.present && cum.value != 0) {
        // An acknowledgement claiming executed quantity without any execution
        // identity leaves the ledger unable to account for it. Synthesising
        // fills here is exactly what ADR-07 forbids (what-if A-20).
        return quarantine(prev, Reason::MissingFillEvidence,
                          "New Ack reports CumQty=" + std::to_string(cum.value) +
                              " with no execution details",
                          r.exec_id, r.cl_ord_id);
    }

    // An echoed quantity or price that differs from what was requested is a
    // conformance problem, not a new accepted value to adopt.
    const OptNum oqty = opt_units(r.order_qty);
    if (oqty.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "OrderQty='" + *r.order_qty + "'",
                          r.exec_id, r.cl_ord_id);
    }
    if (oqty.present && oqty.value != prev.pending->desired_qty_units) {
        return quarantine(prev, Reason::CumMismatch,
                          "ack OrderQty " + std::to_string(oqty.value) + " != requested " +
                              std::to_string(prev.pending->desired_qty_units),
                          r.exec_id, r.cl_ord_id);
    }
    const OptNum opx = opt_ticks(r.price, *spec);
    if (opx.malformed) {
        return quarantine(prev, Reason::ScaleMismatch, "Price='" + *r.price + "'", r.exec_id,
                          r.cl_ord_id);
    }
    if (opx.present && opx.value != prev.pending->desired_price_ticks) {
        return quarantine(prev, Reason::ScaleMismatch,
                          "ack Price " + std::to_string(opx.value) + " != requested " +
                              std::to_string(prev.pending->desired_price_ticks),
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    AcceptedVersion v;
    v.version = 0;
    v.cl_ord_id = prev.pending->cl_ord_id;
    v.qty_units = prev.pending->desired_qty_units;
    v.price_ticks = prev.pending->desired_price_ticks;
    v.scale_id = spec->scale_id;
    v.policy_version = pol.policy_version;
    v.accepted_at_event = event_index;
    out.next.accepted_versions.push_back(v);
    out.next.base = BaseState::Working;
    out.next.pending.reset();
    // The New's disposition is now known, so any uncertainty it created is
    // resolved (what-if A-19).
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    add_alias(out.next, r);
    return out;
}

Reduction on_order_rejected(const OrderAggregate& prev, const PeerReport& r) {
    if (!prev.ledger.empty()) {
        // An order cannot be rejected after it has executed; one of the two
        // statements is false and the ledger is the one with evidence behind
        // it (what-if A-17).
        return quarantine(prev, Reason::ContradictoryResponse,
                          "Reject received after " + std::to_string(prev.ledger.size()) +
                              " recorded fill(s)",
                          r.exec_id, r.cl_ord_id);
    }
    if (prev.terminal()) {
        if (prev.done_reason == DoneReason::Rejected) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                             "duplicate Reject");
        }
        return quarantine(prev, Reason::PostTerminalEvent,
                          std::string("Reject after terminal ") + to_string(prev.done_reason),
                          r.exec_id, r.cl_ord_id);
    }
    if (prev.pending.has_value() && prev.pending->kind != CommandKind::New &&
        r.cl_ord_id == prev.pending->cl_ord_id) {
        // The peer answered a cancel or replace request with an
        // ExecutionReport reject. FIX 4.4 requires 35=9 there, and rejecting
        // the *order* because its *cancel* was refused would be a false
        // terminal (what-if H-20).
        return quarantine(prev, Reason::ProtocolMismatch,
                          "ExecType=8 names the pending " +
                              std::string(to_string(prev.pending->kind)) + " request " +
                              r.cl_ord_id + "; OrderCancelReject (35=9) is required",
                          r.exec_id, r.cl_ord_id);
    }
    const bool known = r.cl_ord_id.empty() || r.cl_ord_id == prev.initial_cl_ord_id ||
                       r.cl_ord_id == prev.accepted_cl_ord_id() ||
                       (prev.pending.has_value() && r.cl_ord_id == prev.pending->cl_ord_id);
    if (!known) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Reject for unknown ClOrdID " + r.cl_ord_id, r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.base = BaseState::Done;
    out.next.done_reason = DoneReason::Rejected;
    out.next.pending.reset();
    out.next.peer_pending_cancel = false;
    out.next.peer_pending_replace = false;
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    const std::string key = "risk-release|" + std::to_string(prev.id.value) + "|rejected";
    out.intents.push_back({EffectIntent::Kind::RiskRelease, key, "order rejected"});
    return out;
}

Reduction on_trade(const OrderAggregate& prev, const PeerReport& r, const Policy& pol,
                   uint64_t event_index) {
    if (r.exec_id.empty()) {
        return quarantine(prev, Reason::IdentityInsufficient, "Trade without ExecID(17)",
                          r.exec_id, r.cl_ord_id);
    }
    if (!prev.has_accepted()) {
        // The fill may be real and the acknowledgement merely late. Park it
        // bounded instead of inventing a chain state (what-if G-03).
        return hold(prev, Reason::AwaitingCorrelation,
                    "Trade before any accepted version; awaiting New Ack");
    }
    if (prev.terminal()) {
        return quarantine(prev, Reason::PostTerminalEvent,
                          std::string("Trade after terminal ") + to_string(prev.done_reason),
                          r.exec_id, r.cl_ord_id);
    }

    const SymbolSpec* spec = pol.find_symbol(prev.symbol);
    if (spec == nullptr) {
        return quarantine(prev, Reason::UnknownSymbol, "no pinned scale for " + prev.symbol,
                          r.exec_id, r.cl_ord_id);
    }

    // Lineage: a fill must name a version this chain has accepted. While a
    // replace is pending the accepted version is still the old one, which is
    // precisely where the fill belongs (design doc 11.2).
    //
    // An OrderID alias is enough to correlate a report to a chain, but not to
    // attribute executed quantity to a version, so a trade without ClOrdID
    // fails closed (what-if O-02).
    if (r.cl_ord_id.empty()) {
        return quarantine(prev, Reason::IdentityInsufficient,
                          "Trade without ClOrdID(11); an OrderID alias alone cannot attribute "
                          "executed quantity to an accepted version",
                          r.exec_id, r.cl_ord_id);
    }
    {
        const AcceptedVersion* matched = nullptr;
        for (const AcceptedVersion& v : prev.accepted_versions) {
            if (v.cl_ord_id == r.cl_ord_id) matched = &v;
        }
        if (matched == nullptr) {
            return quarantine(prev, Reason::LineageMismatch,
                              "Trade ClOrdID " + r.cl_ord_id + " is not an accepted version",
                              r.exec_id, r.cl_ord_id);
        }
        if (matched->version != prev.accepted_versions.back().version) {
            // A fill on a superseded version arriving after the replacement was
            // confirmed cannot be placed in time without proof (what-if G-09,
            // J-10).
            return quarantine(prev, Reason::LineageMismatch,
                              "Trade on superseded version " + r.cl_ord_id +
                                  " after a replacement was accepted",
                              r.exec_id, r.cl_ord_id);
        }
    }

    if (!r.last_qty.has_value()) {
        return quarantine(prev, Reason::InvalidLastQty, "Trade without LastQty(32)", r.exec_id,
                          r.cl_ord_id);
    }
    int64_t last_qty = 0;
    if (const DecimalError e = parse_units(*r.last_qty, last_qty); e != DecimalError::None) {
        return quarantine(prev, Reason::MalformedNumber,
                          std::string("LastQty='") + *r.last_qty + "' (" + to_string(e) + ")",
                          r.exec_id, r.cl_ord_id);
    }
    if (last_qty <= 0) {
        return quarantine(prev, Reason::InvalidLastQty,
                          "LastQty=" + std::to_string(last_qty) + " on ExecType=F", r.exec_id,
                          r.cl_ord_id);
    }
    if (spec->qty_step > 0 && last_qty % spec->qty_step != 0) {
        return quarantine(prev, Reason::QtyStepMismatch,
                          "LastQty " + std::to_string(last_qty) + " is off qty_step " +
                              std::to_string(spec->qty_step),
                          r.exec_id, r.cl_ord_id);
    }

    if (!r.last_px.has_value()) {
        return quarantine(prev, Reason::InvalidPrice, "Trade without LastPx(31)", r.exec_id,
                          r.cl_ord_id);
    }
    int64_t last_px = 0;
    if (const DecimalError e =
            parse_ticks(*r.last_px, spec->tick_numerator, spec->tick_denominator, last_px);
        e != DecimalError::None) {
        return quarantine(prev, Reason::ScaleMismatch,
                          std::string("LastPx='") + *r.last_px + "' (" + to_string(e) + ")",
                          r.exec_id, r.cl_ord_id);
    }
    if (spec->price_must_be_positive && last_px <= 0) {
        return quarantine(prev, Reason::InvalidPrice, "LastPx is not positive", r.exec_id,
                          r.cl_ord_id);
    }
    if (pol.enforce_limit_price_on_fill) {
        const int64_t limit = prev.accepted_price_ticks();
        const bool violated = (prev.side == Side::Buy && last_px > limit) ||
                              (prev.side == Side::Sell && last_px < limit);
        if (violated) {
            return quarantine(prev, Reason::PriceOutsideLimit,
                              "fill at " + std::to_string(last_px) + " ticks violates the " +
                                  to_string(prev.side) + " limit " + std::to_string(limit),
                              r.exec_id, r.cl_ord_id);
        }
    }

    int64_t new_cum = 0;
    if (!checked_add(prev.cum_qty_units, last_qty, new_cum)) {
        return quarantine(prev, Reason::Overfill, "CumQty addition overflowed int64", r.exec_id,
                          r.cl_ord_id);
    }
    const int64_t accepted_qty = prev.accepted_qty_units();
    if (new_cum > accepted_qty) {
        return quarantine(prev, Reason::Overfill,
                          "fill would take CumQty to " + std::to_string(new_cum) +
                              " against accepted qty " + std::to_string(accepted_qty),
                          r.exec_id, r.cl_ord_id);
    }

    // A trade echoing an OrderQty that is not the accepted quantity describes
    // a different order than the one we hold, including the degenerate case of
    // zero against a chain that has already executed (what-if C-12).
    const OptNum rep_order_qty = opt_units(r.order_qty);
    if (rep_order_qty.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "OrderQty='" + *r.order_qty + "'",
                          r.exec_id, r.cl_ord_id);
    }
    if (rep_order_qty.present && rep_order_qty.value != accepted_qty) {
        return quarantine(prev, Reason::CumMismatch,
                          "Trade reports OrderQty " + std::to_string(rep_order_qty.value) +
                              " but the accepted version is " + std::to_string(accepted_qty),
                          r.exec_id, r.cl_ord_id);
    }

    // The counterparty's own cumulative figures are cross-checked against the
    // ledger, never used to overwrite it (design doc 9.4).
    const OptNum rep_cum = opt_units(r.cum_qty);
    if (rep_cum.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "CumQty='" + *r.cum_qty + "'", r.exec_id,
                          r.cl_ord_id);
    }
    if (rep_cum.present) {
        if (rep_cum.value < new_cum) {
            return quarantine(prev, Reason::CumRegression,
                              "reported CumQty " + std::to_string(rep_cum.value) +
                                  " is below the ledger total " + std::to_string(new_cum),
                              r.exec_id, r.cl_ord_id);
        }
        if (rep_cum.value > new_cum) {
            return quarantine(prev, Reason::MissingFillEvidence,
                              "reported CumQty " + std::to_string(rep_cum.value) +
                                  " exceeds the ledger total " + std::to_string(new_cum),
                              r.exec_id, r.cl_ord_id);
        }
    }
    const OptNum rep_leaves = opt_units(r.leaves_qty);
    if (rep_leaves.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "LeavesQty='" + *r.leaves_qty + "'",
                          r.exec_id, r.cl_ord_id);
    }
    if (rep_leaves.present) {
        if (rep_leaves.value < 0 || rep_leaves.value > accepted_qty) {
            return quarantine(prev, Reason::LeavesInconsistent,
                              "reported LeavesQty " + std::to_string(rep_leaves.value) +
                                  " outside [0, " + std::to_string(accepted_qty) + "]",
                              r.exec_id, r.cl_ord_id);
        }
        if (rep_leaves.value != accepted_qty - new_cum) {
            return quarantine(prev, Reason::LeavesInconsistent,
                              "reported LeavesQty " + std::to_string(rep_leaves.value) +
                                  " != qty - cum " + std::to_string(accepted_qty - new_cum),
                              r.exec_id, r.cl_ord_id);
        }
    }

    Int128 term = 0;
    if (!checked_mul_wide(last_qty, last_px, term)) {
        return quarantine(prev, Reason::MalformedNumber, "qty x price overflowed the wide type",
                          r.exec_id, r.cl_ord_id);
    }
    Int128 new_notional = 0;
    if (!checked_add_wide(prev.notional_ticks_x_qty, term, new_notional)) {
        return quarantine(prev, Reason::MalformedNumber, "notional accumulation overflowed",
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    FillLedgerEntry f;
    f.exec_id = r.exec_id;
    f.cl_ord_id = r.cl_ord_id.empty() ? prev.accepted_cl_ord_id() : r.cl_ord_id;
    f.qty_units = last_qty;
    f.price_ticks = last_px;
    f.accepted_version_seen = prev.accepted_versions.back().version;
    f.effective = true;
    f.source_report_id = r.session_id + ":" + std::to_string(r.source_seq);
    f.provenance_hash = business_fingerprint(r);
    out.next.ledger.push_back(f);
    out.next.cum_qty_units = new_cum;
    out.next.notional_ticks_x_qty = new_notional;
    add_alias(out.next, r);

    // A fill is proof the order reached the counterparty, so a New whose
    // acknowledgement was in doubt is no longer in doubt. A pending cancel or
    // replace keeps its own uncertainty.
    if (out.next.confidence == Confidence::Unknown) {
        out.next.confidence = Confidence::Confirmed;
    }

    if (new_cum == accepted_qty) {
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Filled;
        // The pending cancel or replace is deliberately left in place: its
        // disposition is still unresolved and belongs in reconciliation
        // (what-if H-14, J-08).
        if (out.next.pending.has_value()) {
            out.next.reconciliation_required = true;
        }
    }

    const std::string key = "risk-adjust|" + std::to_string(prev.id.value) + "|" + r.exec_id;
    out.intents.push_back({EffectIntent::Kind::RiskAdjust, key,
                           "cum=" + std::to_string(new_cum) + " leaves=" +
                               std::to_string(out.next.leaves_qty_units())});
    (void)event_index;
    return out;
}

Reduction on_pending_cancel(const OrderAggregate& prev, const PeerReport& r) {
    if (prev.terminal()) {
        return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                         "Pending Cancel after the order was already terminal");
    }
    if (!prev.pending.has_value() || prev.pending->kind != CommandKind::Cancel) {
        // An unsolicited pending-cancel has no request to attach to. Marking
        // the order pending on the counterparty's word alone would hide a
        // protocol divergence (what-if B-16).
        return quarantine(prev, Reason::UnexpectedPending,
                          "Pending Cancel with no cancel request outstanding", r.exec_id,
                          r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Pending Cancel ClOrdID " + r.cl_ord_id + " != pending " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.peer_pending_cancel = true;
    out.next.pending->phase = PendingRequest::Phase::PeerPending;
    return out;
}

Reduction on_cancel_accepted(const OrderAggregate& prev, const PeerReport& r) {
    if (prev.terminal()) {
        if (prev.done_reason == DoneReason::Canceled) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                             "duplicate final Canceled");
        }
        // Filled and Canceled are not both true. Which came first is a
        // reconciliation question, not something to overwrite (what-if J-04).
        return quarantine(prev, Reason::ContradictoryResponse,
                          std::string("final Canceled after terminal ") +
                              to_string(prev.done_reason),
                          r.exec_id, r.cl_ord_id);
    }
    if (!prev.pending.has_value() || prev.pending->kind != CommandKind::Cancel) {
        return quarantine(prev, Reason::UnknownRequest,
                          "final Canceled with no cancel request outstanding", r.exec_id,
                          r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Canceled ClOrdID " + r.cl_ord_id + " != pending " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }

    const OptNum rep_cum = opt_units(r.cum_qty);
    if (rep_cum.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "CumQty='" + *r.cum_qty + "'", r.exec_id,
                          r.cl_ord_id);
    }
    if (rep_cum.present) {
        if (rep_cum.value < prev.cum_qty_units) {
            return quarantine(prev, Reason::CumRegression,
                              "Canceled reports CumQty " + std::to_string(rep_cum.value) +
                                  " below the ledger total " +
                                  std::to_string(prev.cum_qty_units),
                              r.exec_id, r.cl_ord_id);
        }
        if (rep_cum.value > prev.cum_qty_units) {
            return quarantine(prev, Reason::MissingFillEvidence,
                              "Canceled reports CumQty " + std::to_string(rep_cum.value) +
                                  " above the ledger total " +
                                  std::to_string(prev.cum_qty_units),
                              r.exec_id, r.cl_ord_id);
        }
    }
    const OptNum rep_leaves = opt_units(r.leaves_qty);
    if (rep_leaves.present && rep_leaves.value != 0) {
        return quarantine(prev, Reason::LeavesInconsistent,
                          "final Canceled with LeavesQty=" + std::to_string(rep_leaves.value),
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.base = BaseState::Done;
    out.next.done_reason = DoneReason::Canceled;
    out.next.pending.reset();
    out.next.peer_pending_cancel = false;
    out.next.peer_pending_replace = false;
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    const std::string key = "risk-release|" + std::to_string(prev.id.value) + "|canceled";
    out.intents.push_back({EffectIntent::Kind::RiskRelease, key,
                           "canceled with cum=" + std::to_string(prev.cum_qty_units)});
    return out;
}

Reduction on_pending_replace(const OrderAggregate& prev, const PeerReport& r) {
    if (prev.terminal()) {
        return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                         "Pending Replace after the order was already terminal");
    }
    // A pending-replace report arriving after the replacement was accepted
    // must not drag the order back to pending (what-if J-09).
    for (const AcceptedVersion& v : prev.accepted_versions) {
        if (!r.cl_ord_id.empty() && v.cl_ord_id == r.cl_ord_id) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                             "Pending Replace for an already accepted version");
        }
    }
    if (!prev.pending.has_value() || prev.pending->kind != CommandKind::Replace) {
        return quarantine(prev, Reason::UnexpectedPending,
                          "Pending Replace with no replace request outstanding", r.exec_id,
                          r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Pending Replace ClOrdID " + r.cl_ord_id + " != pending " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.peer_pending_replace = true;
    out.next.pending->phase = PendingRequest::Phase::PeerPending;
    // The desired quantity and price stay in the pending request. Nothing
    // about the accepted version changes here (invariant I04, what-if B-06,
    // I-10, I-17).
    return out;
}

Reduction on_replace_accepted(const OrderAggregate& prev, const PeerReport& r, const Policy& pol,
                              uint64_t event_index) {
    if (prev.terminal()) {
        return quarantine(prev, Reason::PostTerminalEvent,
                          std::string("Replaced after terminal ") + to_string(prev.done_reason),
                          r.exec_id, r.cl_ord_id);
    }
    if (!prev.pending.has_value() || prev.pending->kind != CommandKind::Replace) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Replaced with no replace request outstanding", r.exec_id, r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "Replaced ClOrdID " + r.cl_ord_id + " != pending " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }
    if (!r.orig_cl_ord_id.empty() && r.orig_cl_ord_id != prev.accepted_cl_ord_id()) {
        return quarantine(prev, Reason::LineageMismatch,
                          "Replaced OrigClOrdID " + r.orig_cl_ord_id + " != accepted " +
                              prev.accepted_cl_ord_id(),
                          r.exec_id, r.cl_ord_id);
    }

    const SymbolSpec* spec = pol.find_symbol(prev.symbol);
    if (spec == nullptr) {
        return quarantine(prev, Reason::UnknownSymbol, "no pinned scale for " + prev.symbol,
                          r.exec_id, r.cl_ord_id);
    }

    int64_t qty = prev.pending->desired_qty_units;
    const OptNum rep_qty = opt_units(r.order_qty);
    if (rep_qty.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "OrderQty='" + *r.order_qty + "'",
                          r.exec_id, r.cl_ord_id);
    }
    if (rep_qty.present) qty = rep_qty.value;

    int64_t px = prev.pending->desired_price_ticks;
    const OptNum rep_px = opt_ticks(r.price, *spec);
    if (rep_px.malformed) {
        // A price that is not on this symbol's tick grid cannot be stored
        // without losing information (what-if I-14).
        return quarantine(prev, Reason::ScaleMismatch,
                          "Replaced Price='" + *r.price + "' is off the pinned tick grid",
                          r.exec_id, r.cl_ord_id);
    }
    if (rep_px.present) px = rep_px.value;

    if (qty <= 0 || (spec->qty_step > 0 && qty % spec->qty_step != 0)) {
        return quarantine(prev, Reason::QtyStepMismatch,
                          "Replaced qty " + std::to_string(qty) + " is not admissible", r.exec_id,
                          r.cl_ord_id);
    }
    if (qty < prev.cum_qty_units) {
        // Committing this version would make CumQty exceed OrderQty, which no
        // later check could undo (what-if I-13).
        return quarantine(prev, Reason::QtyBelowCum,
                          "Replaced qty " + std::to_string(qty) + " is below CumQty " +
                              std::to_string(prev.cum_qty_units),
                          r.exec_id, r.cl_ord_id);
    }
    if (spec->price_must_be_positive && px <= 0) {
        return quarantine(prev, Reason::InvalidPrice, "Replaced price is not positive", r.exec_id,
                          r.cl_ord_id);
    }
    if (prev.accepted_versions.size() + 1 > pol.limits.max_accepted_versions) {
        return quarantine(prev, Reason::ResourceExhausted,
                          "accepted version chain is at its configured ceiling", r.exec_id,
                          r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    AcceptedVersion v;
    v.version = static_cast<uint32_t>(prev.accepted_versions.size());
    v.cl_ord_id = prev.pending->cl_ord_id;
    v.qty_units = qty;
    v.price_ticks = px;
    v.scale_id = spec->scale_id;
    v.policy_version = pol.policy_version;
    v.accepted_at_event = event_index;
    out.next.accepted_versions.push_back(v);
    out.next.pending.reset();
    out.next.peer_pending_replace = false;
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    // CumQty and the ledger are untouched: executed quantity accumulates
    // across versions (invariant I17, what-if I-20).
    add_alias(out.next, r);
    const std::string key = "risk-adjust|" + std::to_string(prev.id.value) + "|v" +
                            std::to_string(v.version);
    out.intents.push_back({EffectIntent::Kind::RiskAdjust, key,
                           "new accepted qty=" + std::to_string(qty)});
    return out;
}

Reduction on_cancel_reject(const OrderAggregate& prev, const PeerReport& r) {
    if (r.cxl_rej_response_to != "1" && r.cxl_rej_response_to != "2") {
        // Without CxlRejResponseTo(434) there is no way to know which request
        // was refused (what-if O-10).
        return quarantine(prev, Reason::ProtocolMismatch,
                          "OrderCancelReject without a valid CxlRejResponseTo(434)", r.exec_id,
                          r.cl_ord_id);
    }
    if (!prev.pending.has_value()) {
        if (prev.terminal() && prev.done_reason == DoneReason::Canceled) {
            // The same request cannot have been both accepted and refused
            // (what-if G-17).
            return quarantine(prev, Reason::ContradictoryResponse,
                              "cancel reject after the cancel was already accepted", r.exec_id,
                              r.cl_ord_id);
        }
        return quarantine(prev, Reason::UnknownRequest,
                          "cancel reject with no request outstanding", r.exec_id, r.cl_ord_id);
    }
    const CommandKind expected =
        r.cxl_rej_response_to == "1" ? CommandKind::Cancel : CommandKind::Replace;
    if (prev.pending->kind != expected) {
        return quarantine(prev, Reason::ContradictoryResponse,
                          std::string("CxlRejResponseTo=") + r.cxl_rej_response_to +
                              " does not match the pending " + to_string(prev.pending->kind),
                          r.exec_id, r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && r.cl_ord_id != prev.pending->cl_ord_id) {
        return quarantine(prev, Reason::UnknownRequest,
                          "reject ClOrdID " + r.cl_ord_id + " != pending " +
                              prev.pending->cl_ord_id,
                          r.exec_id, r.cl_ord_id);
    }
    if (!r.orig_cl_ord_id.empty() && prev.has_accepted() &&
        r.orig_cl_ord_id != prev.accepted_cl_ord_id()) {
        return quarantine(prev, Reason::LineageMismatch,
                          "reject OrigClOrdID " + r.orig_cl_ord_id + " != accepted " +
                              prev.accepted_cl_ord_id(),
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.pending.reset();
    out.next.peer_pending_cancel = false;
    out.next.peer_pending_replace = false;
    // The accepted version is untouched; a refused request changes the
    // request's state, never the order's quantities (invariants I16, I17).

    if (r.cxl_rej_reason == "1") {
        // CxlRejReason=UnknownOrder while we hold a local order means one side
        // is wrong about the order existing. Resending is exactly what must
        // not happen next (what-if H-11).
        out.next.confidence = Confidence::Unknown;
        out.next.reconciliation_required = true;
        const std::string key = "query-status|" + std::to_string(prev.id.value) + "|unknown-order";
        out.intents.push_back({EffectIntent::Kind::QueryOrderStatus, key,
                               "counterparty reports the order as unknown"});
        out.reason = Reason::UnknownOutcome;
    }
    return out;
}

Reduction on_status_snapshot(const OrderAggregate& prev, const PeerReport& r, const Policy& pol) {
    if (r.reconciliation_generation != 0 &&
        r.reconciliation_generation < prev.reconciliation_generation) {
        // An answer to a superseded query must not overwrite a newer one. The
        // generation, not the clock, decides (what-if G-10, G-11).
        return unchanged(prev, Decision::DuplicateNoop, Reason::StaleGeneration,
                         "status snapshot from generation " +
                             std::to_string(r.reconciliation_generation) + " < " +
                             std::to_string(prev.reconciliation_generation));
    }

    const OptNum rep_cum = opt_units(r.cum_qty);
    if (rep_cum.malformed) {
        return quarantine(prev, Reason::MalformedNumber, "CumQty='" + *r.cum_qty + "'", r.exec_id,
                          r.cl_ord_id);
    }
    if (rep_cum.present) {
        if (rep_cum.value > prev.cum_qty_units) {
            // The snapshot claims more executed quantity than the ledger can
            // account for. A status report may never create a fill, so the
            // only honest outcome is to stop and reconcile (ADR-07, what-if
            // B-15, C-09, G-15).
            return quarantine(prev, Reason::MissingFillEvidence,
                              "status reports CumQty " + std::to_string(rep_cum.value) +
                                  " but the ledger totals " +
                                  std::to_string(prev.cum_qty_units),
                              r.exec_id, r.cl_ord_id);
        }
        if (rep_cum.value < prev.cum_qty_units) {
            return quarantine(prev, Reason::CumRegression,
                              "status reports CumQty " + std::to_string(rep_cum.value) +
                                  " below the ledger total " +
                                  std::to_string(prev.cum_qty_units),
                              r.exec_id, r.cl_ord_id);
        }
    }

    if (r.ord_status == ord_status::kFilled && prev.has_accepted() &&
        prev.cum_qty_units != prev.accepted_qty_units()) {
        return quarantine(prev, Reason::MissingFillEvidence,
                          "status claims Filled but the ledger totals " +
                              std::to_string(prev.cum_qty_units) + " of " +
                              std::to_string(prev.accepted_qty_units()),
                          r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.reason = Reason::None;
    add_alias(out.next, r);

    const bool complete = rep_cum.present && !r.ord_status.empty() && !r.order_id.empty();
    const bool resolving = prev.reconciliation_required || prev.confidence == Confidence::Unknown;
    if (!resolving || !complete) {
        // Consistent but not a resolution: metadata only, no lifecycle change.
        return out;
    }

    // A complete and consistent snapshot is the evidence a reconciliation was
    // waiting for. Only the statuses this profile can prove are accepted.
    if (r.ord_status == ord_status::kCanceled) {
        if (prev.pending.has_value() && prev.pending->kind != CommandKind::Cancel) {
            return quarantine(prev, Reason::StatusInconsistent,
                              "status Canceled while a replace is pending", r.exec_id,
                              r.cl_ord_id);
        }
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Canceled;
        out.next.pending.reset();
        out.next.peer_pending_cancel = false;
    } else if (r.ord_status == ord_status::kFilled) {
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Filled;
    } else if (r.ord_status == ord_status::kRejected) {
        if (!prev.ledger.empty()) {
            return quarantine(prev, Reason::ContradictoryResponse,
                              "status Rejected with recorded fills", r.exec_id, r.cl_ord_id);
        }
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Rejected;
        out.next.pending.reset();
    } else if (r.ord_status == ord_status::kExpired) {
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Expired;
        out.next.pending.reset();
    } else if (r.ord_status == ord_status::kNew ||
               r.ord_status == ord_status::kPartiallyFilled) {
        out.next.base = BaseState::Working;
    } else {
        // Pending Cancel, Pending Replace, Suspended and the rest do not
        // resolve anything; keep waiting rather than guess.
        return out;
    }
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    out.next.held_events = 0;
    (void)pol;
    return out;
}

Reduction on_expired_or_dfd(const OrderAggregate& prev, const PeerReport& r, DoneReason reason) {
    if (prev.terminal()) {
        if (prev.done_reason == reason) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::LateReportNoRegression,
                             "duplicate terminal report");
        }
        // Expiry arriving after a fill terminal would silently drop executed
        // quantity if applied (what-if G-19).
        return quarantine(prev, Reason::PostTerminalEvent,
                          std::string("terminal ") + to_string(reason) + " after terminal " +
                              to_string(prev.done_reason),
                          r.exec_id, r.cl_ord_id);
    }
    if (!prev.has_accepted()) {
        return quarantine(prev, Reason::UnknownRequest,
                          "terminal report before any accepted version", r.exec_id, r.cl_ord_id);
    }

    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;
    out.next.base = BaseState::Done;
    out.next.done_reason = reason;
    out.next.pending.reset();
    out.next.peer_pending_cancel = false;
    out.next.peer_pending_replace = false;
    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    const std::string key =
        "risk-release|" + std::to_string(prev.id.value) + "|" + to_string(reason);
    out.intents.push_back({EffectIntent::Kind::RiskRelease, key, to_string(reason)});
    return out;
}

Reduction on_correction(const OrderAggregate& prev, const PeerReport& r, const Policy& pol) {
    const bool is_bust = r.exec_type == exec_type::kTradeCancel;
    const char* what = is_bust ? "TradeCancel(150=H)" : "TradeCorrect(150=G)";
    if (r.exec_ref_id.empty()) {
        return quarantine(prev, Reason::MissingCorrectionReference,
                          std::string(what) + " without ExecRefID(19)", r.exec_id, r.cl_ord_id);
    }
    if (prev.find_fill(r.exec_ref_id) == nullptr) {
        return quarantine(prev, Reason::UnknownCorrectionReference,
                          std::string(what) + " references ExecID " + r.exec_ref_id +
                              " which is not in the ledger",
                          r.exec_id, r.cl_ord_id);
    }
    if (pol.trade_corrections == UnsupportedPolicy::Reject) {
        return reject_local(prev, Reason::UnsupportedEvent,
                            std::string(what) + " is not supported by this profile");
    }
    // v1 has no reversible ledger, so the only safe answer is to stop and let
    // reconciliation decide. Subtracting quantity here is what ADR-06 forbids.
    return quarantine(prev, Reason::UnsupportedEvent,
                      std::string(what) + " is unsupported in this profile; ledger untouched",
                      r.exec_id, r.cl_ord_id);
}

Reduction reduce_report(const OrderAggregate& prev, const PeerReport& r, const Policy& pol,
                        uint64_t event_index) {
    // 1. Structural and identity validation, before anything is applied.
    if (r.exec_id.size() > pol.limits.max_exec_id_bytes ||
        r.cl_ord_id.size() > pol.limits.max_cl_ord_id_bytes) {
        return quarantine(prev, Reason::IdTooLong, "an identifier exceeds its configured ceiling",
                          r.exec_id, r.cl_ord_id);
    }
    if (!r.exec_id.empty() && !identifier_ok(r.exec_id, pol.limits.max_exec_id_bytes)) {
        return quarantine(prev, Reason::IdentityInsufficient,
                          "ExecID contains a control byte or is empty", r.exec_id, r.cl_ord_id);
    }
    if (!r.cl_ord_id.empty() && !identifier_ok(r.cl_ord_id, pol.limits.max_cl_ord_id_bytes)) {
        return quarantine(prev, Reason::IdentityInsufficient,
                          "ClOrdID contains a control byte", r.exec_id, r.cl_ord_id);
    }
    std::string detail;
    if (const Reason r2 = identity_consistent(prev, r, detail); r2 != Reason::None) {
        if (r2 == Reason::MissingSourceCursor) {
            return hold(prev, r2, detail);
        }
        return quarantine(prev, r2, detail, r.exec_id, r.cl_ord_id);
    }
    if (const Reason r3 = status_consistent(r, detail); r3 != Reason::None) {
        return quarantine(prev, r3, detail, r.exec_id, r.cl_ord_id);
    }

    // 2. Semantic dedup. The fingerprint deliberately ignores transport
    // fields, so a FIX resend of the same execution is a no-op while the same
    // ExecID carrying different business values is a conflict.
    const uint64_t fp = business_fingerprint(r);
    if (const SeenReport* seen = prev.find_seen(report_seen_key(r)); seen != nullptr) {
        if (seen->fingerprint == fp) {
            return unchanged(prev, Decision::DuplicateNoop, Reason::DuplicateSamePayload,
                             "already applied " + report_seen_key(r));
        }
        return quarantine(prev, Reason::ConflictingDuplicate,
                          "key " + report_seen_key(r) +
                              " was already seen with different business fields",
                          r.exec_id, r.cl_ord_id);
    }

    // 3. Dispatch by message type and ExecType.
    Reduction out;
    if (r.msg_type == '9') {
        out = on_cancel_reject(prev, r);
    } else if (r.msg_type != '8') {
        out = quarantine(prev, Reason::ProtocolMismatch,
                         std::string("unexpected MsgType '") + r.msg_type + "'", r.exec_id,
                         r.cl_ord_id);
    } else if (r.exec_type == exec_type::kNew) {
        out = on_new_ack(prev, r, pol, event_index);
    } else if (r.exec_type == exec_type::kRejected) {
        out = on_order_rejected(prev, r);
    } else if (r.exec_type == exec_type::kTrade) {
        out = on_trade(prev, r, pol, event_index);
    } else if (r.exec_type == exec_type::kPendingCancel) {
        out = on_pending_cancel(prev, r);
    } else if (r.exec_type == exec_type::kCanceled) {
        out = on_cancel_accepted(prev, r);
    } else if (r.exec_type == exec_type::kPendingReplace) {
        out = on_pending_replace(prev, r);
    } else if (r.exec_type == exec_type::kReplaced) {
        out = on_replace_accepted(prev, r, pol, event_index);
    } else if (r.exec_type == exec_type::kOrderStatus) {
        out = on_status_snapshot(prev, r, pol);
    } else if (r.exec_type == exec_type::kExpired) {
        out = on_expired_or_dfd(prev, r, DoneReason::Expired);
    } else if (r.exec_type == exec_type::kDoneForDay) {
        out = on_expired_or_dfd(prev, r, DoneReason::DoneForDay);
    } else if (r.exec_type == exec_type::kTradeCorrect ||
               r.exec_type == exec_type::kTradeCancel) {
        out = on_correction(prev, r, pol);
    } else if (r.exec_type == exec_type::kPendingNew) {
        if (prev.pending.has_value() && prev.pending->kind == CommandKind::New) {
            out.next = prev;
            out.decision = Decision::Applied;
            out.next.pending->phase = PendingRequest::Phase::PeerPending;
        } else {
            out = quarantine(prev, Reason::UnexpectedPending,
                             "Pending New with no New request outstanding", r.exec_id,
                             r.cl_ord_id);
        }
    } else if (r.exec_type == exec_type::kRestated) {
        out = quarantine(prev, Reason::UnsupportedEvent,
                         r.text.empty() ? "ExecutionRestated without a reason"
                                        : "ExecutionRestated is unsupported in this profile",
                         r.exec_id, r.cl_ord_id);
    } else if (r.exec_type == exec_type::kStopped || r.exec_type == exec_type::kSuspended ||
               r.exec_type == exec_type::kCalculated) {
        out = quarantine(prev, Reason::UnsupportedEvent,
                         "ExecType=" + r.exec_type + " is outside the v1 profile", r.exec_id,
                         r.cl_ord_id);
    } else if (r.exec_type == "1" || r.exec_type == "2") {
        // FIX 4.2 used ExecType 1/2 for partial and full fill. Reading them as
        // trades under a 4.4 dictionary would invent executions (what-if B-13).
        out = quarantine(prev, Reason::WrongFixVersionSemantics,
                         "ExecType=" + r.exec_type +
                             " is FIX 4.2 partial/full fill semantics, not FIX 4.4",
                         r.exec_id, r.cl_ord_id);
    } else {
        out = quarantine(prev, Reason::UnknownExecType,
                         "ExecType='" + r.exec_type + "' has no configured handler", r.exec_id,
                         r.cl_ord_id);
    }

    if (out.decision == Decision::Applied || out.decision == Decision::Quarantined) {
        record_seen(out.next, r, fp);
        advance_cursor(out.next, r);
    }
    if (out.decision == Decision::Applied && out.next.peer_id.empty()) {
        out.next.peer_id = r.peer_id;
    }
    return out;
}

// ---------------------------------------------------------------------------
// System events
// ---------------------------------------------------------------------------

Reduction apply_resolution(const OrderAggregate& prev, const SystemEvent& s) {
    Reduction out;
    out.next = prev;
    out.decision = Decision::Applied;

    if (s.resolved_status == "CANCELED") {
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Canceled;
        out.next.pending.reset();
        out.next.peer_pending_cancel = false;
    } else if (s.resolved_status == "FILLED") {
        if (!prev.has_accepted() || prev.cum_qty_units != prev.accepted_qty_units()) {
            // Declaring an order filled without the ledger to back it would be
            // inventing executions through the back door.
            return quarantine(prev, Reason::MissingFillEvidence,
                              "resolution claims Filled but the ledger totals " +
                                  std::to_string(prev.cum_qty_units),
                              "", s.target_cl_ord_id);
        }
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Filled;
    } else if (s.resolved_status == "REJECTED") {
        if (!prev.ledger.empty()) {
            return quarantine(prev, Reason::ContradictoryResponse,
                              "resolution claims Rejected with recorded fills", "",
                              s.target_cl_ord_id);
        }
        out.next.base = BaseState::Done;
        out.next.done_reason = DoneReason::Rejected;
        out.next.pending.reset();
    } else if (s.resolved_status == "WORKING") {
        out.next.base = BaseState::Working;
        out.next.pending.reset();
        out.next.peer_pending_cancel = false;
        out.next.peer_pending_replace = false;
    } else {
        return reject_local(prev, Reason::MissingField,
                            "resolution status '" + s.resolved_status + "' is not recognised");
    }

    out.next.confidence = Confidence::Confirmed;
    out.next.reconciliation_required = false;
    out.next.quarantined = false;
    out.next.held_events = 0;
    // The incidents stay as history, but they are now accounted for: an
    // audited resolution is exactly the case where a chain may be safe again
    // with its incident record intact (invariant I24).
    out.next.resolved_incident_count = out.next.incidents.size();
    return out;
}

Reduction reduce_system(const OrderAggregate& prev, const SystemEvent& s, const Policy& pol) {
    switch (s.kind) {
        case SystemKind::Dispatched: {
            if (!prev.pending.has_value()) {
                return reject_local(prev, Reason::UnknownRequest,
                                    "dispatch notice with no request outstanding");
            }
            if (!s.target_cl_ord_id.empty() &&
                s.target_cl_ord_id != prev.pending->cl_ord_id) {
                return reject_local(prev, Reason::UnknownRequest,
                                    "dispatch notice for " + s.target_cl_ord_id +
                                        " != pending " + prev.pending->cl_ord_id);
            }
            Reduction out;
            out.next = prev;
            out.decision = Decision::Applied;
            out.next.pending->phase = PendingRequest::Phase::SentUnknown;
            if (prev.pending->kind == CommandKind::New && prev.base == BaseState::Preparing) {
                // Sent, outcome unknown. Explicitly not Working: no
                // acknowledgement has arrived (what-if A-10).
                out.next.base = BaseState::SentUnknown;
            }
            return out;
        }

        case SystemKind::AckTimeout: {
            if (!prev.pending.has_value()) {
                return reject_local(prev, Reason::UnknownRequest,
                                    "timeout with no request outstanding");
            }
            if (!s.target_cl_ord_id.empty() &&
                s.target_cl_ord_id != prev.pending->cl_ord_id) {
                return reject_local(prev, Reason::UnknownRequest,
                                    "timeout for " + s.target_cl_ord_id + " != pending " +
                                        prev.pending->cl_ord_id);
            }
            Reduction out;
            out.next = prev;
            out.decision = Decision::Applied;
            out.reason = Reason::UnknownOutcome;
            // A deadline passing is an observation about us, not a decision by
            // the counterparty. It can never produce a cancel or a fill
            // (invariant I18, what-if A-18, H-15, I-18).
            out.next.pending->outcome_unknown = true;
            out.next.reconciliation_required = true;
            if (prev.pending->kind == CommandKind::New) {
                out.next.confidence = Confidence::Unknown;
            }
            out.next.reconcile_attempts = prev.reconcile_attempts + 1;
            const std::string key =
                "query-status|" + std::to_string(prev.id.value) + "|" + prev.pending->cl_ord_id;
            if (out.next.reconcile_attempts > pol.limits.max_reconcile_attempts) {
                // Escalate instead of retrying forever, and never record the
                // escalation as a success (what-if Q-04, P-17).
                out.intents.push_back({EffectIntent::Kind::Alert, key + "|escalate",
                                       "reconciliation attempts exhausted"});
            } else {
                out.intents.push_back(
                    {EffectIntent::Kind::QueryOrderStatus, key, "acknowledgement deadline passed"});
            }
            out.intents.push_back(
                {EffectIntent::Kind::ReconciliationRequired, key + "|reconcile", "timeout"});
            return out;
        }

        case SystemKind::TransportDisconnected: {
            Reduction out;
            out.next = prev;
            out.decision = Decision::Applied;
            out.reason = Reason::UnknownOutcome;
            if (out.next.pending.has_value()) {
                out.next.pending->outcome_unknown = true;
                out.next.reconciliation_required = true;
            }
            if (prev.base == BaseState::SentUnknown) {
                out.next.confidence = Confidence::Unknown;
            }
            return out;
        }

        case SystemKind::ReconcileStarted: {
            Reduction out;
            out.next = prev;
            out.decision = Decision::Applied;
            out.next.reconciliation_generation = prev.reconciliation_generation + 1;
            out.next.reconciliation_required = true;
            return out;
        }

        case SystemKind::ReconcileResolved: {
            if (!s.evidence_provided) {
                // Clearing an unknown state without evidence is the
                // release-blocker the design doc calls out by name.
                return reject_local(prev, Reason::NotAuthorized,
                                    "resolution without evidence is refused");
            }
            if (s.reconciliation_generation != 0 &&
                s.reconciliation_generation < prev.reconciliation_generation) {
                return unchanged(prev, Decision::DuplicateNoop, Reason::StaleGeneration,
                                 "resolution from a superseded generation");
            }
            return apply_resolution(prev, s);
        }

        case SystemKind::StorageFailed: {
            Reduction out;
            out.next = prev;
            out.decision = Decision::Applied;
            out.reason = Reason::UnknownOutcome;
            out.next.reconciliation_required = true;
            out.intents.push_back({EffectIntent::Kind::Alert,
                                   "storage|" + std::to_string(prev.id.value) + "|" +
                                       std::to_string(prev.revision),
                                   s.detail});
            return out;
        }

        case SystemKind::ManualOverride: {
            if (!s.evidence_provided || !s.dual_control_signed) {
                // Dual control plus evidence, or nothing (what-if P-17, Q-13).
                return reject_local(prev, Reason::NotAuthorized,
                                    "manual override requires evidence and dual control");
            }
            return apply_resolution(prev, s);
        }
    }
    return reject_local(prev, Reason::UnsupportedEvent, "unhandled system event");
}

}  // namespace

// ---------------------------------------------------------------------------

Reduction reduce(const OrderAggregate& previous, const OrderEvent& event, const Policy& policy,
                 uint64_t event_index) {
    Reduction out;
    AppliedKey key;

    if (const auto* c = std::get_if<LocalCommand>(&event)) {
        switch (c->kind) {
            case CommandKind::New: out = reduce_new(previous, *c, policy, event_index); break;
            case CommandKind::Cancel:
                out = reduce_cancel(previous, *c, policy, event_index);
                break;
            case CommandKind::Replace:
                out = reduce_replace(previous, *c, policy, event_index);
                break;
        }
        key.scope = "request";
        key.semantic_key = c->request_key().text() + "|" + to_string(c->kind);
        key.payload_hash = business_fingerprint(*c);
    } else if (const auto* r = std::get_if<PeerReport>(&event)) {
        out = reduce_report(previous, *r, policy, event_index);
        key.scope = "exec";
        key.semantic_key = ExecKey{r->peer_id, policy.business_namespace, report_seen_key(*r)}.text();
        key.payload_hash = business_fingerprint(*r);
    } else {
        const auto& s = std::get<SystemEvent>(event);
        out = reduce_system(previous, s, policy);
        key.scope = "system";
        key.semantic_key = std::string(to_string(s.kind)) + "|" + s.target_cl_ord_id + "|" +
                           std::to_string(s.reconciliation_generation) + "|" +
                           std::to_string(event_index);
        key.payload_hash = fnv1a(key.semantic_key);
    }
    if (out.decision == Decision::Quarantined &&
        out.next.incidents.size() > policy.limits.max_quarantine_incidents) {
        // Bounded incident store. Under a flood the earliest incidents are the
        // diagnostically useful ones, so they are kept and the newest is
        // dropped rather than growing the vector without a limit
        // (invariant I23, what-if N-20).
        out.next.incidents.resize(policy.limits.max_quarantine_incidents);
    }
    if (out.decision == Decision::Quarantined) {
        // The incident is a new row, not an application of the offending
        // event. Keying it by (event key, reason) makes the first incident
        // durable and a repeat of the same contradiction a no-op rather than a
        // growing pile of identical rows (what-if K-11).
        key.scope = "quarantine";
        key.semantic_key = "q|" + key.semantic_key + "|" + to_string(out.reason);
    }
    // Every durable key is length bounded. An adversarial 64 KiB identifier
    // is refused on its own merits, but it must not reach the dedup index on
    // the way out either (invariant I23, what-if O-17).
    if (key.semantic_key.size() > kMaxSemanticKeyBytes) {
        key.semantic_key = "digest:" + std::to_string(fnv1a(key.semantic_key)) + "|" +
                           std::to_string(key.semantic_key.size());
    }
    out.applied_key = key;

    // Invariants are evaluated on the *proposed* state. A proposal that
    // violates one is never committed: the chain is quarantined with the
    // evidence instead (design doc 5.2, invariant I24).
    if (out.decision == Decision::Applied) {
        out.next.revision = previous.revision + 1;
        out.next.last_applied_event_index = event_index;
        out.violations = check_invariants(out.next, policy);
        if (!out.violations.empty()) {
            Reduction q = quarantine(previous, Reason::InvariantViolated, describe(out.violations),
                                     "", "");
            q.applied_key = key;
            q.violations = out.violations;
            q.next.revision = previous.revision + 1;
            q.next.last_applied_event_index = event_index;
            return q;
        }
    } else if (out.decision == Decision::Quarantined) {
        out.next.revision = previous.revision + 1;
        out.next.last_applied_event_index = event_index;
    }
    return out;
}

}  // namespace order_core
