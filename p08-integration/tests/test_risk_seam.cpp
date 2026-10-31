// The P04 -> P05 seam.
//
// This is the first test in the lab that exercises two components against each
// other rather than one against its own fake, and what it really checks is the
// translation: P04 and P05 were written apart, and every disagreement between
// them is a wrong number in an account rather than a compile error.
//
// Each case below is one of those disagreements. The side case is the one worth
// reading twice: router::Side is {Buy=0, Sell=1} and risk::Side is {Buy=1,
// Sell=2}, so the obvious cast turns a sell order into a buy and nothing in
// either component would notice.
//
// P04's own suite keeps using its lab fake, which is right -- it tests the
// routing policy, not the risk engine. This file is where the real engine is on
// the other end.
#include <cstdio>
#include <optional>
#include <string>

#include "deps/p05_risk.hpp"
#include "gw/risk_adapter.hpp"
#include "risk/scenario.hpp"
#include "router/clock.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* expr, const char* file, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL %s:%d  %s%s%s\n", file, line, expr, detail.empty() ? "" : " -- ",
                detail.c_str());
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__, {})
#define CHECK_MSG(expr, detail) check((expr), #expr, __FILE__, __LINE__, (detail))

constexpr std::int64_t kPriceTicks = 110000;  // 1.10000 at the pinned 5 digits
constexpr std::int64_t kQty = 10000;          // a multiple of EURUSD's 1000 step

// The lab's symbol table. One entry, because one is enough to show that the two
// sides of the mapping are genuinely different strings.
gw::RiskAdmissionAdapter::Options lab_options(const risk::Principal& trader) {
    gw::RiskAdmissionAdapter::Options o;
    o.principal = trader;
    o.wire_to_instrument = {{"EUR/USD", "EURUSD"}};
    return o;
}

router::p05::RiskRequest new_order_request(const std::string& command, const std::string& chain,
                                           router::Side side) {
    router::p05::RiskRequest rr;
    rr.command_id = router::CommandId(command);
    rr.tenant = router::TenantId("T1");
    rr.account = router::AccountId("ACC-A");
    rr.wire_symbol = "EUR/USD";
    rr.side = side;
    rr.qty = kQty;
    rr.price_ticks = kPriceTicks;
    rr.increases_exposure = true;
    rr.order_chain_id = router::OrderChainId(chain);
    rr.price_scale = 5;
    // A New, so the absolute and the delta are the same number. That identity
    // is what the adapter uses to tell a New from a Replace.
    rr.total_units_after = kQty;
    return rr;
}

std::optional<risk::Reservation> reservation_for_chain(const risk::RiskStore& store,
                                                       const std::string& chain) {
    const std::optional<risk::ReservationId> id = store.find_chain("T1", "ACC-A", chain);
    if (!id.has_value()) return std::nullopt;
    return store.find_reservation(*id);
}

// ---------------------------------------------------------------------------

void side_mapping_is_explicit_because_the_cast_is_wrong() {
    std::printf("side_mapping_is_explicit_because_the_cast_is_wrong\n");

    // The premise. If these ever line up, the rest of this case stops being
    // interesting -- and the adapter's switch stops being load-bearing.
    CHECK(static_cast<int>(router::Side::Buy) != static_cast<int>(risk::Side::Buy));
    CHECK(static_cast<int>(router::Side::Sell) != static_cast<int>(risk::Side::Sell));
    // Specifically: a cast of P04's Sell lands on P05's Buy.
    CHECK(static_cast<int>(router::Side::Sell) == static_cast<int>(risk::Side::Buy));

    risk::Side out{};
    CHECK(gw::to_risk_side(router::Side::Buy, out));
    CHECK(out == risk::Side::Buy);
    CHECK(gw::to_risk_side(router::Side::Sell, out));
    CHECK(out == risk::Side::Sell);
}

void a_new_order_takes_real_budget_in_p05() {
    std::printf("a_new_order_takes_real_budget_in_p05\n");

    risk::LabOptions options;
    auto env = risk::LabEnv::create(options);
    CHECK_MSG(env.ok(), std::string(risk::reason_name(env.reason())) + ": " + env.detail());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     lab_options(lab.trader()));

    const router::p05::RiskDecision d =
        adapter.reserve(new_order_request("C-1", "CH-1", router::Side::Buy));

    CHECK_MSG(d.verdict == router::p05::Verdict::Approve, d.reason);
    CHECK(!d.reservation_id.empty());
    // Not the adapter's own bookkeeping: P05's store says the hold exists.
    CHECK(adapter.held(d.reservation_id));

    const std::optional<risk::Reservation> r = reservation_for_chain(lab.store(), "CH-1");
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r->accepted_total_units == kQty);
        CHECK(r->symbol == "EURUSD");  // translated, not passed through
        CHECK(r->holds_budget());
    }

    // Nothing was refused on the way through.
    CHECK(adapter.scale_mismatches() == 0);
    CHECK(adapter.unmappable_sides() == 0);
    CHECK(adapter.unmapped_symbols() == 0);
    CHECK(adapter.stranded_reservations().empty());

    // The verdict expiry arrived in P04's clock domain. P05's deadline is a
    // steady-clock reading, so an unconverted value would sit nowhere near the
    // router's wall clock.
    CHECK(d.valid_until > router_clock.wall_now());
}

void a_sell_stays_a_sell() {
    std::printf("a_sell_stays_a_sell\n");

    risk::LabOptions options;
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     lab_options(lab.trader()));

    const router::p05::RiskDecision d =
        adapter.reserve(new_order_request("C-2", "CH-2", router::Side::Sell));
    CHECK_MSG(d.verdict == router::p05::Verdict::Approve, d.reason);

    const std::optional<risk::Reservation> r = reservation_for_chain(lab.store(), "CH-2");
    CHECK(r.has_value());
    if (r.has_value()) {
        // The whole point. A cast would have recorded a Buy here, and the
        // account's exposure would have pointed the wrong way.
        CHECK(r->side == risk::Side::Sell);
    }
}

void unrepresentable_requests_are_refused_rather_than_approximated() {
    std::printf("unrepresentable_requests_are_refused_rather_than_approximated\n");

    risk::LabOptions options;
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     lab_options(lab.trader()));

    // A scale P05 is not configured for. EI-15 wants this rejected; rescaling
    // would be the guess it forbids.
    router::p05::RiskRequest wrong_scale = new_order_request("C-3", "CH-3", router::Side::Buy);
    wrong_scale.price_scale = 3;
    const router::p05::RiskDecision scale = adapter.reserve(wrong_scale);
    CHECK(scale.verdict == router::p05::Verdict::Reject);
    CHECK(adapter.scale_mismatches() == 1);
    // Definitive, not transient: UNAVAILABLE would invite a retry that cannot
    // succeed.
    CHECK(scale.verdict != router::p05::Verdict::Unavailable);

    // A symbol with no instrument behind it. §2 pins the symbol policy, and an
    // instrument P05 holds no limits for is not one this lab may trade.
    router::p05::RiskRequest unknown_symbol = new_order_request("C-4", "CH-4", router::Side::Buy);
    unknown_symbol.wire_symbol = "XAU/USD";
    CHECK(adapter.reserve(unknown_symbol).verdict == router::p05::Verdict::Reject);
    CHECK(adapter.unmapped_symbols() == 1);

    // No chain. Attributing it to the command instead would make a later
    // replace look like a second order and charge the account twice (EI-23).
    router::p05::RiskRequest no_chain = new_order_request("C-5", "", router::Side::Buy);
    CHECK(adapter.reserve(no_chain).verdict == router::p05::Verdict::Reject);

    // None of the three reached the engine.
    CHECK(!reservation_for_chain(lab.store(), "CH-3").has_value());
    CHECK(!reservation_for_chain(lab.store(), "CH-4").has_value());
}

void a_release_strands_the_hold_instead_of_freeing_it() {
    std::printf("a_release_strands_the_hold_instead_of_freeing_it\n");

    risk::LabOptions options;
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     lab_options(lab.trader()));

    const router::p05::RiskDecision d =
        adapter.reserve(new_order_request("C-6", "CH-6", router::Side::Buy));
    CHECK(d.verdict == router::p05::Verdict::Approve);

    adapter.release(d.reservation_id);

    // The documented decision, asserted so it cannot change by accident: P05
    // has no release, COMMITTED -> FINAL is refused by name, and forcing it
    // through IN_FLIGHT would assert a dispatch that never happened. So the
    // budget stays held -- over-reserving costs availability, under-reserving
    // costs money -- and the id is surfaced rather than the loss being silent.
    CHECK(adapter.held(d.reservation_id));
    CHECK(adapter.stranded_reservations().size() == 1);
    CHECK(adapter.stranded_reservations().front() == d.reservation_id);

    // Idempotent, as P04's port requires.
    adapter.release(d.reservation_id);
    CHECK(adapter.stranded_reservations().size() == 1);
}

void a_cancel_marker_keeps_the_headroom() {
    std::printf("a_cancel_marker_keeps_the_headroom\n");

    risk::LabOptions options;
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     lab_options(lab.trader()));

    const router::p05::RiskDecision d =
        adapter.reserve(new_order_request("C-7", "CH-7", router::Side::Buy));
    CHECK(d.verdict == router::p05::Verdict::Approve);

    adapter.note_cancel_requested(router::TenantId("T1"), router::AccountId("ACC-A"),
                                  router::OrderChainId("CH-7"), router::CommandId("C-7-CXL"));

    const std::optional<risk::Reservation> r = reservation_for_chain(lab.store(), "CH-7");
    CHECK(r.has_value());
    if (r.has_value()) {
        // EI-24: recorded, and the held quantity untouched, because fills can
        // still arrive before the LP acts on the cancel.
        CHECK(r->pending_cancel);
        CHECK(r->accepted_total_units == kQty);
        CHECK(r->holds_budget());
    }
    CHECK(adapter.rejected_cancel_markers() == 0);
}

void the_dispatch_deadline_crosses_as_a_duration() {
    std::printf("the_dispatch_deadline_crosses_as_a_duration\n");

    // Two clocks that share no epoch, which is the realistic case: a steady
    // clock counts from an arbitrary point and a wall clock from 1970.
    const risk::MonoNanos mono_now = 5'000'000'000;
    const router::WallMicros wall_now = router::VirtualClock::kLabEpochUtc;

    // Two seconds of dispatch eligibility left.
    const router::WallMicros in_two_seconds =
        gw::deadline_to_router_wall(mono_now + 2'000'000'000, mono_now, wall_now);
    CHECK(in_two_seconds == wall_now + 2'000'000);

    // Already expired, and it has to stay expired: a value that wrapped forward
    // would make a stale verdict look permanently fresh, and P04 compares this
    // number directly against its wall clock to decide (OR-A-18).
    const router::WallMicros a_second_ago =
        gw::deadline_to_router_wall(mono_now - 1'000'000'000, mono_now, wall_now);
    CHECK(a_second_ago == wall_now - 1'000'000);
    CHECK(a_second_ago < wall_now);
}

}  // namespace

int main() {
    std::printf("P08 seam: P04 admission port -> P05 risk engine\n\n");

    side_mapping_is_explicit_because_the_cast_is_wrong();
    a_new_order_takes_real_budget_in_p05();
    a_sell_stays_a_sell();
    unrepresentable_requests_are_refused_rather_than_approximated();
    a_release_strands_the_hold_instead_of_freeing_it();
    a_cancel_marker_keeps_the_headroom();
    the_dispatch_deadline_crosses_as_a_duration();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
