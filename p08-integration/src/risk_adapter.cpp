#include "gw/risk_adapter.hpp"

#include <utility>

namespace gw {
namespace {

// P04 reports a verdict in three values; P05 decides in five. The collapse is
// where the care is needed, because two of the five read easily as "try again"
// and must not.
router::p05::Verdict to_router_verdict(risk::RiskDecision d) {
    switch (d) {
        case risk::RiskDecision::AllowCommitted:
            return router::p05::Verdict::Approve;
        case risk::RiskDecision::DenyDefinitive:
            return router::p05::Verdict::Reject;
        case risk::RiskDecision::HoldRetryable:
            // The one genuinely transient answer. P04 fails closed on it for a
            // New and a Replace (OR-A-16), which is the behaviour this wants.
            return router::p05::Verdict::Unavailable;
        case risk::RiskDecision::Conflict:
            // Same command id, different payload. Definitive: retrying it
            // unchanged cannot succeed, and UNAVAILABLE would invite exactly
            // that retry.
            return router::p05::Verdict::Reject;
        case risk::RiskDecision::Quarantined:
            // The account's evidence is no longer trusted. P05 keeps the
            // exposure and stops believing the reports, so the answer is a
            // refusal rather than an outage: UNAVAILABLE lets P04's cancel lane
            // through, and a cancel on a quarantined account belongs in
            // reconciliation rather than on the wire.
            return router::p05::Verdict::Reject;
    }
    // An unrecognised decision is not an approval.
    return router::p05::Verdict::Reject;
}

}  // namespace

bool to_risk_side(router::Side in, risk::Side& out) {
    switch (in) {
        case router::Side::Buy:
            out = risk::Side::Buy;
            return true;
        case router::Side::Sell:
            out = risk::Side::Sell;
            return true;
    }
    return false;
}

router::WallMicros deadline_to_router_wall(risk::MonoNanos deadline_mono,
                                           risk::MonoNanos risk_mono_now,
                                           router::WallMicros router_wall_now) {
    // Both are signed, so a deadline already past stays past instead of
    // wrapping into a far-future instant -- which is what unsigned arithmetic
    // would produce here, and what would make an expired verdict look
    // permanently fresh.
    const risk::MonoNanos remaining_ns = deadline_mono - risk_mono_now;
    return router_wall_now + static_cast<router::WallMicros>(remaining_ns / 1000);
}

RiskAdmissionAdapter::RiskAdmissionAdapter(risk::RiskCoordinator& coordinator,
                                           const risk::RiskStore& store,
                                           const risk::Clock& risk_clock,
                                           const router::IClock& router_clock, Options options)
    : coordinator_(coordinator),
      store_(store),
      risk_clock_(risk_clock),
      router_clock_(router_clock),
      options_(std::move(options)) {}

router::p05::RiskDecision RiskAdmissionAdapter::refuse(router::p05::Verdict v,
                                                       std::string reason) const {
    router::p05::RiskDecision d;
    d.verdict = v;
    d.reason = std::move(reason);
    // No reservation id and no expiry: there is nothing to hand back and
    // nothing to outlive.
    return d;
}

router::p05::RiskDecision RiskAdmissionAdapter::reserve(const router::p05::RiskRequest& req) {
    if (req.price_scale != options_.price_digits) {
        ++scale_mismatches_;
        return refuse(router::p05::Verdict::Reject,
                      "price scale " + std::to_string(req.price_scale) + " is not the " +
                          std::to_string(options_.price_digits) + " this engine runs at");
    }

    risk::Side side{};
    if (!to_risk_side(req.side, side)) {
        ++unmappable_sides_;
        return refuse(router::p05::Verdict::Reject, "side has no representation in P05");
    }

    // A chainless request cannot be attributed, and attributing it to the
    // command instead would make a later replace look like a second order
    // (EI-23).
    if (req.order_chain_id.empty()) {
        return refuse(router::p05::Verdict::Reject, "request carries no order chain");
    }

    // The wire symbol is resolved before the command is built, on every path
    // that carries one, so a configuration that has drifted is caught here
    // rather than surfacing as a missing instrument deep inside P05.
    risk::SymbolId instrument;
    if (!req.wire_symbol.empty()) {
        const auto it = options_.wire_to_instrument.find(req.wire_symbol);
        if (it == options_.wire_to_instrument.end()) {
            ++unmapped_symbols_;
            return refuse(router::p05::Verdict::Reject,
                          "wire symbol " + req.wire_symbol + " maps to no P05 instrument");
        }
        instrument = it->second;
    }

    risk::RiskResult result;
    if (req.increases_exposure) {
        // P04 fills `qty` with the delta on the replace path and with the whole
        // size on the New path, so the two agreeing is what identifies a New.
        const bool is_replace = req.total_units_after != req.qty;
        if (is_replace) {
            risk::ReplaceCommand c;
            c.tenant_id = req.tenant.str();
            c.account_id = req.account.str();
            c.command_id = req.command_id.str();
            c.order_chain_id = req.order_chain_id.str();
            // The absolute total, not the delta: P05 tracks the chain's size
            // itself, and adding a delta to its own figure is how two views of
            // one position drift apart.
            c.new_total_units = req.total_units_after;
            c.new_limit_price_ticks = req.price_ticks;
            result = coordinator_.reserve_replace(c, options_.principal);
        } else {
            risk::NewOrderCommand c;
            c.tenant_id = req.tenant.str();
            c.account_id = req.account.str();
            c.command_id = req.command_id.str();
            c.order_chain_id = req.order_chain_id.str();
            c.symbol = instrument;
            c.side = side;
            c.total_units = req.total_units_after;
            c.limit_price_ticks = req.price_ticks;
            // The only shapes P05 models. P04 screens the others out before
            // admission (UnsupportedOrdType, UnsupportedTif), so whatever
            // arrives here is already a day limit; saying so keeps the
            // assumption visible rather than implied.
            c.order_type = risk::OrderType::Limit;
            c.time_in_force = risk::TimeInForce::Day;
            result = coordinator_.reserve_new(c, options_.principal);
        }
    } else {
        risk::CancelCommand c;
        c.tenant_id = req.tenant.str();
        c.account_id = req.account.str();
        c.command_id = req.command_id.str();
        c.order_chain_id = req.order_chain_id.str();
        result = coordinator_.request_cancel(c, options_.principal);
    }

    router::p05::RiskDecision out;
    out.verdict = to_router_verdict(result.decision);
    out.reason = std::string(risk::reason_name(result.reason));
    if (!result.detail.empty()) out.reason += ": " + result.detail;
    out.reservation_id = result.reservation_id;
    if (result.token.has_value()) {
        out.valid_until = deadline_to_router_wall(result.token->dispatch_deadline_mono,
                                                  risk_clock_.mono_now(),
                                                  router_clock_.wall_now());
    }
    return out;
}

void RiskAdmissionAdapter::release(const std::string& reservation_id) {
    if (reservation_id.empty()) return;

    // Idempotent in the sense P04's port asks for: a second call adds nothing.
    for (const std::string& id : stranded_) {
        if (id == reservation_id) return;
    }

    // Nothing else happens, and that is a decision rather than an omission.
    // See stranded_reservations(): P05 offers no release, the transition one
    // would need is refused by name, and reaching FINAL through IN_FLIGHT would
    // assert a dispatch that never occurred. The hold stays -- the conservative
    // direction -- and the id is surfaced so a scenario can prove whether the
    // budget was ever actually freed.
    stranded_.push_back(reservation_id);
}

bool RiskAdmissionAdapter::held(const std::string& reservation_id) const {
    const std::optional<risk::Reservation> r = store_.find_reservation(reservation_id);
    if (!r.has_value()) return false;
    // P05's own predicate, so UNKNOWN and QUARANTINE both count as held: not
    // knowing the outcome is not evidence that the order is gone (RI-26).
    return r->holds_budget();
}

void RiskAdmissionAdapter::note_cancel_requested(const router::TenantId& tenant,
                                                 const router::AccountId& account,
                                                 const router::OrderChainId& chain,
                                                 const router::CommandId& command_id) {
    // EI-24: the marker is what makes P05 keep headroom for the fills that can
    // still arrive before the LP acts on the cancel. P05 records it and leaves
    // the held quantity untouched, which is the whole point of it.
    risk::VerifiedOrderEvent e;
    e.tenant_id = tenant.str();
    e.account_id = account.str();
    e.order_chain_id = chain.str();
    // The provenance is the router's own durable commit, not an LP execution.
    // Saying so keeps it out of the ExecID namespace where a real fill lives.
    e.source_namespace = "router-cancel-request";
    e.event_id = command_id.str();
    e.kind = risk::OrderEventKind::PendingCancel;
    e.related_command_id = command_id.str();

    const risk::RiskResult result = coordinator_.apply_verified_order_event(e);
    if (!result.allowed()) ++rejected_cancel_markers_;
}

}  // namespace gw
