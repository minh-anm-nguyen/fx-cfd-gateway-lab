#include "jrnl/domain.hpp"

#include <algorithm>

namespace jrnl {

std::string_view to_string(OrdStatus s) {
    switch (s) {
        case OrdStatus::None: return "None";
        case OrdStatus::PendingNew: return "PendingNew";
        case OrdStatus::New: return "New";
        case OrdStatus::PartiallyFilled: return "PartiallyFilled";
        case OrdStatus::Filled: return "Filled";
        case OrdStatus::PendingCancel: return "PendingCancel";
        case OrdStatus::Canceled: return "Canceled";
        case OrdStatus::PendingReplace: return "PendingReplace";
        case OrdStatus::Replaced: return "Replaced";
        case OrdStatus::Rejected: return "Rejected";
        case OrdStatus::Expired: return "Expired";
    }
    return "None";
}

bool parse_ord_status(std::string_view s, OrdStatus* out) {
    static const OrdStatus kAll[] = {
        OrdStatus::None,          OrdStatus::PendingNew,     OrdStatus::New,
        OrdStatus::PartiallyFilled, OrdStatus::Filled,       OrdStatus::PendingCancel,
        OrdStatus::Canceled,      OrdStatus::PendingReplace, OrdStatus::Replaced,
        OrdStatus::Rejected,      OrdStatus::Expired};
    for (OrdStatus c : kAll) {
        if (to_string(c) == s) {
            if (out != nullptr) *out = c;
            return true;
        }
    }
    return false;
}

bool is_terminal(OrdStatus s) {
    // PendingCancel and PendingReplace are absent on purpose. JI-10: a
    // pending state is not terminal, must not release risk, and a fill can
    // still arrive against it.
    return s == OrdStatus::Filled || s == OrdStatus::Canceled || s == OrdStatus::Rejected ||
           s == OrdStatus::Expired || s == OrdStatus::Replaced;
}

bool OrderState::operator==(const OrderState& o) const {
    return exists == o.exists && chain == o.chain && account == o.account &&
           cl_ord_id == o.cl_ord_id && orig_cl_ord_id == o.orig_cl_ord_id &&
           symbol == o.symbol && side == o.side && order_qty == o.order_qty &&
           cum_qty == o.cum_qty && leaves_qty == o.leaves_qty && limit_px == o.limit_px &&
           exec_value == o.exec_value && fill_count == o.fill_count && status == o.status &&
           pending_cancel == o.pending_cancel && pending_replace == o.pending_replace &&
           pending_cl_ord_id == o.pending_cl_ord_id && pending_qty == o.pending_qty &&
           pending_px == o.pending_px && origin_command == o.origin_command &&
           risk_domain == o.risk_domain && last_px == o.last_px;
}

std::string_view to_string(ReservationState s) {
    switch (s) {
        case ReservationState::Held: return "Held";
        case ReservationState::Released: return "Released";
        case ReservationState::Consumed: return "Consumed";
    }
    return "Held";
}

bool parse_reservation_state(std::string_view s, ReservationState* out) {
    if (s == "Held") { if (out) *out = ReservationState::Held; return true; }
    if (s == "Released") { if (out) *out = ReservationState::Released; return true; }
    if (s == "Consumed") { if (out) *out = ReservationState::Consumed; return true; }
    return false;
}

bool RiskDomainState::operator==(const RiskDomainState& o) const {
    return exists == o.exists && domain == o.domain && limit == o.limit &&
           reserved == o.reserved && exposure == o.exposure && revision == o.revision &&
           reservations == o.reservations;
}

Digest state_digest(const CanonValue& canon_state) {
    return sha256(canonical_encode(canon_state));
}

namespace {

bool err(ReduceError* e, Incident inc, std::string detail) {
    if (e != nullptr) {
        e->incident = inc;
        e->detail = std::move(detail);
    }
    return false;
}

// Reads a quantity or price field and range-checks it in one step (G01-12).
bool read_amount(const CanonValue& p, std::string_view key, std::int64_t* out,
                 ReduceError* e) {
    std::int64_t v = 0;
    if (!p.get_int(key, &v)) {
        return err(e, Incident::SchemaIncompatible,
                   std::string("missing or non-integer field ") + std::string(key));
    }
    if (!amount_in_range(v)) {
        return err(e, Incident::SchemaIncompatible,
                   std::string("field ") + std::string(key) + " out of admissible range");
    }
    *out = v;
    return true;
}

bool read_text(const CanonValue& p, std::string_view key, std::string* out, ReduceError* e) {
    if (!p.get_str(key, out)) {
        return err(e, Incident::SchemaIncompatible,
                   std::string("missing or non-string field ") + std::string(key));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Order reducer
// ---------------------------------------------------------------------------

class OrderReducer final : public IOrderReducer {
public:
    explicit OrderReducer(std::string_view version, bool track_last_px)
        : version_(version), track_last_px_(track_last_px) {}

    std::string_view version() const override { return version_; }

    bool apply(OrderState* s, std::string_view type, const CanonValue& p,
               ReduceError* e) const override {
        if (type == ev::kOrderAccepted) return accepted(s, p, e);
        if (type == ev::kOrderRejected) return rejected(s, p, e);
        if (type == ev::kFillApplied) return fill(s, p, e);
        if (type == ev::kCancelRequested) return cancel_requested(s, p, e);
        if (type == ev::kCancelAccepted) return cancel_accepted(s, p, e);
        if (type == ev::kCancelRejected) return cancel_rejected(s, p, e);
        if (type == ev::kReplaceRequested) return replace_requested(s, p, e);
        if (type == ev::kReplaceAccepted) return replace_accepted(s, p, e);
        if (type == ev::kReplaceRejected) return replace_rejected(s, p, e);
        if (type == ev::kOrderExpired) return expired(s, p, e);
        if (type == ev::kOrderCorrected) return corrected(s, p, e);
        return err(e, Incident::SchemaIncompatible,
                   std::string("unknown order event type ") + std::string(type));
    }

    CanonValue to_canon(const OrderState& s) const override {
        Payload out;
        // The reducer version is part of the hashed state, as section 8 note
        // 3 requires: two reducers must not be able to produce the same hash
        // for what they each consider "the same" state.
        out.s("v", std::string(version_));
        out.b("exists", s.exists);
        if (!s.exists) return out.build();
        out.s("chain", s.chain);
        out.s("account", s.account);
        out.s("cl_ord_id", s.cl_ord_id);
        out.s("orig_cl_ord_id", s.orig_cl_ord_id);
        out.s("symbol", s.symbol);
        out.s("side", s.side);
        out.i("order_qty", s.order_qty);
        out.i("cum_qty", s.cum_qty);
        out.i("leaves_qty", s.leaves_qty);
        out.i("limit_px", s.limit_px);
        out.i("exec_value", s.exec_value);
        out.i("fill_count", s.fill_count);
        out.s("status", std::string(to_string(s.status)));
        out.b("pending_cancel", s.pending_cancel);
        out.b("pending_replace", s.pending_replace);
        out.s("pending_cl_ord_id", s.pending_cl_ord_id);
        out.i("pending_qty", s.pending_qty);
        out.i("pending_px", s.pending_px);
        out.s("origin_command", s.origin_command);
        out.s("risk_domain", s.risk_domain);
        if (track_last_px_) out.i("last_px", s.last_px);
        return out.build();
    }

    bool from_canon(const CanonValue& v, OrderState* out) const override {
        OrderState s;
        std::string ver;
        if (!v.get_str("v", &ver) || ver != version_) return false;
        if (!v.get_bool("exists", &s.exists)) return false;
        if (!s.exists) {
            *out = s;
            return true;
        }
        std::string status;
        const bool ok =
            v.get_str("chain", &s.chain) && v.get_str("account", &s.account) &&
            v.get_str("cl_ord_id", &s.cl_ord_id) &&
            v.get_str("orig_cl_ord_id", &s.orig_cl_ord_id) &&
            v.get_str("symbol", &s.symbol) && v.get_str("side", &s.side) &&
            v.get_int("order_qty", &s.order_qty) && v.get_int("cum_qty", &s.cum_qty) &&
            v.get_int("leaves_qty", &s.leaves_qty) && v.get_int("limit_px", &s.limit_px) &&
            v.get_int("exec_value", &s.exec_value) &&
            v.get_int("fill_count", &s.fill_count) && v.get_str("status", &status) &&
            v.get_bool("pending_cancel", &s.pending_cancel) &&
            v.get_bool("pending_replace", &s.pending_replace) &&
            v.get_str("pending_cl_ord_id", &s.pending_cl_ord_id) &&
            v.get_int("pending_qty", &s.pending_qty) &&
            v.get_int("pending_px", &s.pending_px) &&
            v.get_str("origin_command", &s.origin_command) &&
            v.get_str("risk_domain", &s.risk_domain) && parse_ord_status(status, &s.status);
        if (!ok) return false;
        if (track_last_px_ && !v.get_int("last_px", &s.last_px)) return false;
        *out = s;
        return true;
    }

private:
    bool accepted(OrderState* s, const CanonValue& p, ReduceError* e) const {
        if (s->exists) {
            return err(e, Incident::StreamVersionDuplicate,
                       "OrderAccepted on a chain that already exists");
        }
        OrderState n;
        n.exists = true;
        if (!read_text(p, "chain", &n.chain, e)) return false;
        if (!read_text(p, "account", &n.account, e)) return false;
        if (!read_text(p, "cl_ord_id", &n.cl_ord_id, e)) return false;
        if (!read_text(p, "symbol", &n.symbol, e)) return false;
        if (!read_text(p, "side", &n.side, e)) return false;
        if (!read_text(p, "command_id", &n.origin_command, e)) return false;
        if (!read_text(p, "risk_domain", &n.risk_domain, e)) return false;
        if (!read_amount(p, "order_qty", &n.order_qty, e)) return false;
        if (!read_amount(p, "limit_px", &n.limit_px, e)) return false;
        if (n.side != "BUY" && n.side != "SELL") {
            return err(e, Incident::SchemaIncompatible, "side must be BUY or SELL");
        }
        if (n.order_qty <= 0) {
            return err(e, Incident::SchemaIncompatible, "order_qty must be positive");
        }
        if (n.limit_px < 0) {
            return err(e, Incident::SchemaIncompatible, "limit_px must not be negative");
        }
        n.orig_cl_ord_id = n.cl_ord_id;
        n.leaves_qty = n.order_qty;
        n.status = OrdStatus::New;
        *s = n;
        return true;
    }

    bool rejected(OrderState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "OrderRejected on unknown chain");
        if (is_terminal(s->status)) {
            return err(e, Incident::HeadMismatch, "OrderRejected on a terminal order");
        }
        std::string reason;
        read_text(p, "reason", &reason, nullptr);
        s->status = OrdStatus::Rejected;
        s->leaves_qty = 0;
        s->pending_cancel = false;
        s->pending_replace = false;
        return true;
    }

    bool fill(OrderState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "fill on unknown chain");
        if (is_terminal(s->status)) {
            // A fill arriving after a terminal state is a contradiction
            // between our history and the counterparty's. JI-40: it is not
            // resolved by inventing a state, it is quarantined.
            return err(e, Incident::OverfillDetected,
                       std::string("fill against terminal order in status ") +
                           std::string(to_string(s->status)));
        }
        std::int64_t last_qty = 0, last_px = 0;
        if (!read_amount(p, "last_qty", &last_qty, e)) return false;
        if (!read_amount(p, "last_px", &last_px, e)) return false;
        if (last_qty <= 0) return err(e, Incident::OverfillDetected, "last_qty must be positive");
        if (last_px < 0) {
            return err(e, Incident::SchemaIncompatible, "last_px must not be negative");
        }
        if (last_qty > s->leaves_qty) {
            // JI-08. The quantity accounting is the invariant; there is no
            // policy under which the ledger is allowed to exceed the order.
            return err(e, Incident::OverfillDetected,
                       "last_qty " + std::to_string(last_qty) + " exceeds leaves_qty " +
                           std::to_string(s->leaves_qty));
        }
        s->cum_qty += last_qty;
        s->leaves_qty -= last_qty;
        s->exec_value += last_qty * last_px;
        s->fill_count += 1;
        if (track_last_px_) s->last_px = last_px;
        s->status = s->leaves_qty == 0 ? OrdStatus::Filled : OrdStatus::PartiallyFilled;
        return true;
    }

    bool cancel_requested(OrderState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "cancel on unknown chain");
        if (is_terminal(s->status)) {
            // JI-38: no cancel from a post-terminal state, including after a
            // recovery that replayed the terminal transition.
            return err(e, Incident::HeadMismatch, "cancel request against a terminal order");
        }
        if (s->pending_cancel) {
            return err(e, Incident::HeadMismatch, "cancel already pending");
        }
        if (!read_text(p, "cl_ord_id", &s->pending_cl_ord_id, e)) return false;
        s->pending_cancel = true;
        return true;
    }

    bool cancel_accepted(OrderState* s, const CanonValue&, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "cancel on unknown chain");
        // A pending cancel is NOT required. A counterparty may cancel an
        // order unsolicited, and that report is authoritative about their
        // side; refusing it would quarantine a legitimate terminal state.
        // What stays contradictory is a cancel against an order that is
        // already terminal, which the next check catches.
        if (is_terminal(s->status)) {
            return err(e, Incident::HeadMismatch,
                       std::string("cancel accepted against an order already in status ") +
                           std::string(to_string(s->status)));
        }
        s->pending_cancel = false;
        s->pending_replace = false;
        s->leaves_qty = 0;
        // A cancel that lands after the order was already fully filled leaves
        // the Filled status standing: the fills happened.
        s->status = s->cum_qty > 0 && s->cum_qty == s->order_qty ? OrdStatus::Filled
                                                                 : OrdStatus::Canceled;
        return true;
    }

    bool cancel_rejected(OrderState* s, const CanonValue&, ReduceError* e) const {
        if (!s->exists || !s->pending_cancel) {
            return err(e, Incident::HeadMismatch, "cancel rejected without a pending cancel");
        }
        s->pending_cancel = false;
        return true;
    }

    bool replace_requested(OrderState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "replace on unknown chain");
        if (is_terminal(s->status)) {
            return err(e, Incident::HeadMismatch, "replace request against a terminal order");
        }
        if (s->pending_replace) {
            return err(e, Incident::HeadMismatch, "replace already pending");
        }
        if (!read_text(p, "cl_ord_id", &s->pending_cl_ord_id, e)) return false;
        if (!read_amount(p, "order_qty", &s->pending_qty, e)) return false;
        if (!read_amount(p, "limit_px", &s->pending_px, e)) return false;
        if (s->pending_qty <= 0) {
            return err(e, Incident::SchemaIncompatible, "replacement order_qty must be positive");
        }
        s->pending_replace = true;
        return true;
    }

    bool replace_accepted(OrderState* s, const CanonValue&, ReduceError* e) const {
        if (!s->exists || !s->pending_replace) {
            return err(e, Incident::HeadMismatch, "replace accepted without a pending replace");
        }
        if (s->pending_qty < s->cum_qty) {
            // Shrinking an order below what has already traded would make
            // leaves negative. The race is real (G10) and the answer is to
            // refuse the transition, not to clamp it.
            return err(e, Incident::OverfillDetected,
                       "replacement qty " + std::to_string(s->pending_qty) +
                           " is below cum_qty " + std::to_string(s->cum_qty));
        }
        s->orig_cl_ord_id = s->cl_ord_id;
        s->cl_ord_id = s->pending_cl_ord_id;
        s->order_qty = s->pending_qty;
        s->limit_px = s->pending_px;
        s->leaves_qty = s->order_qty - s->cum_qty;
        s->pending_replace = false;
        s->pending_cl_ord_id.clear();
        s->pending_qty = 0;
        s->pending_px = 0;
        if (s->leaves_qty == 0) s->status = OrdStatus::Filled;
        else s->status = s->cum_qty > 0 ? OrdStatus::PartiallyFilled : OrdStatus::New;
        return true;
    }

    bool replace_rejected(OrderState* s, const CanonValue&, ReduceError* e) const {
        if (!s->exists || !s->pending_replace) {
            return err(e, Incident::HeadMismatch, "replace rejected without a pending replace");
        }
        s->pending_replace = false;
        s->pending_cl_ord_id.clear();
        s->pending_qty = 0;
        s->pending_px = 0;
        return true;
    }

    bool expired(OrderState* s, const CanonValue&, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "expiry on unknown chain");
        if (is_terminal(s->status)) {
            return err(e, Incident::HeadMismatch, "expiry against a terminal order");
        }
        s->status = OrdStatus::Expired;
        s->leaves_qty = 0;
        s->pending_cancel = false;
        s->pending_replace = false;
        return true;
    }

    bool corrected(OrderState* s, const CanonValue& p, ReduceError* e) const {
        // JI-02: history is never updated in place. A bust or a correction is
        // a new event, and it carries the authority that allowed it so the
        // audit trail shows who decided.
        if (!s->exists) return err(e, Incident::HeadMismatch, "correction on unknown chain");
        std::string who;
        if (!read_text(p, "authorized_by", &who, e)) return false;
        if (who.empty()) {
            return err(e, Incident::SchemaIncompatible, "correction needs an authorising party");
        }
        std::int64_t new_cum = 0;
        if (!read_amount(p, "cum_qty", &new_cum, e)) return false;
        if (new_cum < 0 || new_cum > s->order_qty) {
            return err(e, Incident::OverfillDetected, "corrected cum_qty outside order quantity");
        }
        std::int64_t new_value = 0;
        if (!read_amount(p, "exec_value", &new_value, e)) return false;
        s->cum_qty = new_cum;
        s->exec_value = new_value;
        s->leaves_qty = is_terminal(s->status) ? 0 : s->order_qty - new_cum;
        if (!is_terminal(s->status)) {
            s->status = s->leaves_qty == 0
                            ? OrdStatus::Filled
                            : (s->cum_qty > 0 ? OrdStatus::PartiallyFilled : OrdStatus::New);
        }
        return true;
    }

    std::string version_;
    bool track_last_px_;
};

// ---------------------------------------------------------------------------
// Risk reducer
// ---------------------------------------------------------------------------

class RiskReducer final : public IRiskReducer {
public:
    std::string_view version() const override { return reducer_version::kRiskV1; }

    bool apply(RiskDomainState* s, std::string_view type, const CanonValue& p,
               ReduceError* e) const override {
        if (type == ev::kRiskLimitSet) return limit_set(s, p, e);
        if (type == ev::kRiskReserved) return reserved(s, p, e);
        if (type == ev::kRiskReleased) return released(s, p, e);
        if (type == ev::kRiskConsumed) return consumed(s, p, e);
        return err(e, Incident::SchemaIncompatible,
                   std::string("unknown risk event type ") + std::string(type));
    }

    CanonValue to_canon(const RiskDomainState& s) const override {
        Payload out;
        out.s("v", std::string(reducer_version::kRiskV1));
        out.b("exists", s.exists);
        if (!s.exists) return out.build();
        out.s("domain", s.domain);
        out.i("limit", s.limit);
        out.i("reserved", s.reserved);
        out.i("exposure", s.exposure);
        out.i("revision", static_cast<std::int64_t>(s.revision));
        // std::map iterates in key order, so the list is deterministic
        // without an explicit sort.
        CanonList rs;
        for (const auto& entry : s.reservations) {
            Payload r;
            r.s("id", entry.first);
            r.i("amount", entry.second.amount);
            r.s("state", std::string(to_string(entry.second.state)));
            r.s("account", entry.second.account);
            r.s("chain", entry.second.chain);
            rs.push_back(r.build());
        }
        out.l("reservations", std::move(rs));
        return out.build();
    }

    bool from_canon(const CanonValue& v, RiskDomainState* out) const override {
        RiskDomainState s;
        std::string ver;
        if (!v.get_str("v", &ver) || ver != reducer_version::kRiskV1) return false;
        if (!v.get_bool("exists", &s.exists)) return false;
        if (!s.exists) {
            *out = s;
            return true;
        }
        std::int64_t rev = 0;
        if (!(v.get_str("domain", &s.domain) && v.get_int("limit", &s.limit) &&
              v.get_int("reserved", &s.reserved) && v.get_int("exposure", &s.exposure) &&
              v.get_int("revision", &rev))) {
            return false;
        }
        s.revision = static_cast<std::uint64_t>(rev);
        const CanonValue* rl = v.find("reservations");
        if (rl == nullptr) return false;
        const CanonList* list = rl->as_list();
        if (list == nullptr) return false;
        for (const CanonValue& r : *list) {
            std::string id, state, account, chain;
            std::int64_t amount = 0;
            if (!(r.get_str("id", &id) && r.get_int("amount", &amount) &&
                  r.get_str("state", &state) && r.get_str("account", &account) &&
                  r.get_str("chain", &chain))) {
                return false;
            }
            Reservation res;
            res.amount = amount;
            res.account = account;
            res.chain = chain;
            if (!parse_reservation_state(state, &res.state)) return false;
            s.reservations.emplace(id, res);
        }
        *out = s;
        return true;
    }

private:
    bool limit_set(RiskDomainState* s, const CanonValue& p, ReduceError* e) const {
        std::string domain;
        if (!read_text(p, "domain", &domain, e)) return false;
        std::int64_t limit = 0;
        if (!read_amount(p, "limit", &limit, e)) return false;
        if (limit < 0) return err(e, Incident::SchemaIncompatible, "limit must not be negative");
        if (s->exists && s->domain != domain) {
            return err(e, Incident::CrossTenantLeak, "risk event addressed to another domain");
        }
        if (!s->exists) {
            s->exists = true;
            s->domain = domain;
        }
        if (limit < s->utilised()) {
            // Lowering a limit below what is already committed would make the
            // state instantly non-compliant. The reduction is refused; the
            // operator reduces exposure first.
            return err(e, Incident::RiskLedgerDiff,
                       "new limit " + std::to_string(limit) + " is below utilised " +
                           std::to_string(s->utilised()));
        }
        s->limit = limit;
        s->revision += 1;
        return true;
    }

    bool reserved(RiskDomainState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "reservation in an unset domain");
        std::string id, account, chain;
        if (!read_text(p, "reservation_id", &id, e)) return false;
        if (!read_text(p, "account", &account, e)) return false;
        if (!read_text(p, "chain", &chain, e)) return false;
        std::int64_t amount = 0;
        if (!read_amount(p, "amount", &amount, e)) return false;
        if (amount <= 0) {
            return err(e, Incident::SchemaIncompatible, "reservation amount must be positive");
        }
        if (s->reservations.count(id) != 0) {
            return err(e, Incident::StreamVersionDuplicate, "reservation id already exists");
        }
        if (amount > s->headroom()) {
            // JI-09 and the firm-limit half of G09: two accounts drawing on
            // the same domain cannot both be admitted past the ceiling, and
            // the check is on the committed state, not a cached copy.
            return err(e, Incident::RiskLedgerDiff,
                       "reservation " + std::to_string(amount) + " exceeds headroom " +
                           std::to_string(s->headroom()));
        }
        Reservation r;
        r.amount = amount;
        r.state = ReservationState::Held;
        r.account = account;
        r.chain = chain;
        s->reservations.emplace(id, r);
        s->reserved += amount;
        s->revision += 1;
        return true;
    }

    bool released(RiskDomainState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "release in an unset domain");
        std::string id;
        if (!read_text(p, "reservation_id", &id, e)) return false;
        std::int64_t amount = 0;
        if (!read_amount(p, "amount", &amount, e)) return false;
        const auto it = s->reservations.find(id);
        if (it == s->reservations.end()) {
            return err(e, Incident::OverReleaseDetected, "release of an unknown reservation");
        }
        if (it->second.state != ReservationState::Held) {
            return err(e, Incident::OverReleaseDetected,
                       "release of a reservation already in state " +
                           std::string(to_string(it->second.state)));
        }
        if (amount != it->second.amount) {
            // JI-09 over-release. A partial release would leave the ledger
            // and the held amount disagreeing, so the amount must match what
            // remains held.
            return err(e, Incident::OverReleaseDetected,
                       "release amount " + std::to_string(amount) + " does not match held " +
                           std::to_string(it->second.amount));
        }
        s->reserved -= amount;
        it->second.state = ReservationState::Released;
        it->second.amount = 0;
        s->revision += 1;
        return true;
    }

    bool consumed(RiskDomainState* s, const CanonValue& p, ReduceError* e) const {
        if (!s->exists) return err(e, Incident::HeadMismatch, "consumption in an unset domain");
        std::string id;
        if (!read_text(p, "reservation_id", &id, e)) return false;
        std::int64_t amount = 0;
        if (!read_amount(p, "amount", &amount, e)) return false;
        if (amount <= 0) {
            return err(e, Incident::SchemaIncompatible, "consumed amount must be positive");
        }
        const auto it = s->reservations.find(id);
        if (it == s->reservations.end()) {
            return err(e, Incident::RiskLedgerDiff, "consumption of an unknown reservation");
        }
        if (it->second.state != ReservationState::Held) {
            return err(e, Incident::RiskLedgerDiff,
                       "consumption of a reservation in state " +
                           std::string(to_string(it->second.state)));
        }
        if (amount > it->second.amount) {
            return err(e, Incident::RiskLedgerDiff,
                       "consumption " + std::to_string(amount) + " exceeds held " +
                           std::to_string(it->second.amount));
        }
        // A partial fill consumes part of the hold and leaves the rest held,
        // which is what keeps the remaining exposure covered.
        it->second.amount -= amount;
        s->reserved -= amount;
        s->exposure += amount;
        if (it->second.amount == 0) it->second.state = ReservationState::Consumed;
        s->revision += 1;
        return true;
    }
};

}  // namespace

ReducerRegistry::ReducerRegistry() {
    orders_.emplace(std::string(reducer_version::kOrderV1),
                    std::make_shared<OrderReducer>(reducer_version::kOrderV1, false));
    orders_.emplace(std::string(reducer_version::kOrderV2),
                    std::make_shared<OrderReducer>(reducer_version::kOrderV2, true));
    risks_.emplace(std::string(reducer_version::kRiskV1), std::make_shared<RiskReducer>());
}

const IOrderReducer* ReducerRegistry::order(std::string_view version) const {
    const auto it = orders_.find(version);
    return it == orders_.end() ? nullptr : it->second.get();
}

const IRiskReducer* ReducerRegistry::risk(std::string_view version) const {
    const auto it = risks_.find(version);
    return it == risks_.end() ? nullptr : it->second.get();
}

const ReducerRegistry& ReducerRegistry::instance() {
    static const ReducerRegistry r;
    return r;
}

}  // namespace jrnl
