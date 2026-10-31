// P03 on the write path, and the defect that disappears when it is.
//
// The oracle test established that P04's reducer and order_core disagree about
// a buy filled above its own limit price: P04 applies it and reports the order
// Filled, order_core refuses it. §2 gives P03 order status and the fill ledger,
// so order_core is right, and P04's reducer was crediting a fill the order
// never authorised.
//
// Installing `gw::OrderCoreReducer` into `router::Store` is what fixes it, and
// the fourth case here is the proof: the same event sequence, through the same
// reducer entry points the router uses, now refuses the unauthorised fill. If
// that case ever passes for the wrong reason -- because the reducer silently
// fell back to P04's -- `aggregate_of` returns nullptr and the first case fails
// first.
#include <cstdio>
#include <string>

#include "deps/p03_osm.hpp"
#include "gw/order_reducer.hpp"
#include "order_core/aggregate.hpp"
#include "router/report.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* expr, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL line %d  %s%s%s\n", line, expr, detail.empty() ? "" : " -- ",
                detail.c_str());
}

#define CHECK(expr) check((expr), #expr, __LINE__, {})
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

constexpr int kPriceDigits = 5;
constexpr std::int64_t kQty = 10000;
constexpr std::int64_t kLimitPx = 110000;
constexpr const char* kWireSymbol = "EUR/USD";
constexpr const char* kInstrument = "EURUSD";

gw::OrderCoreReducer::Options reducer_options() {
    gw::OrderCoreReducer::Options o;
    o.wire_to_instrument = {{kWireSymbol, kInstrument}};
    return o;
}

router::p03::ChainState open_chain(router::p03::IOrderReducer& reducer) {
    return reducer.create(router::OrderChainId("CH-1"), router::TenantId("T1"),
                          router::AccountId("ACC1"), router::LpId("LP_SIM_01"), kWireSymbol,
                          router::Side::Buy, kPriceDigits, router::ClientOrderId("O-1"), kQty,
                          kLimitPx);
}

router::NormalizedReport report(const std::string& exec_id, router::ExecType exec_type,
                                router::OrdStatus status) {
    router::NormalizedReport r;
    r.session_key = "ROUTER04:LPSIM01:PRIMARY";
    r.epoch = 1;
    r.source_receipt = 1;
    r.lp = router::LpId("LP_SIM_01");
    r.chain_id = router::OrderChainId("CH-1");
    r.account = router::AccountId("ACC1");
    r.cl_ord_id = router::ClientOrderId("O-1");
    r.exec_id = router::ExecutionId(exec_id);
    r.order_id = router::OrderId("LP-ORD-1");
    r.kind = router::ReportKind::ExecutionReport;
    r.exec_type = exec_type;
    r.ord_status = status;
    r.wire_symbol = kWireSymbol;
    r.side = router::Side::Buy;
    r.order_qty = kQty;
    return r;
}

router::NormalizedReport ack() {
    router::NormalizedReport r = report("E-ACK", router::ExecType::New, router::OrdStatus::New);
    r.cum_qty = 0;
    r.leaves_qty = kQty;
    return r;
}

router::NormalizedReport fill(const std::string& exec_id, std::int64_t qty, std::int64_t px,
                              std::int64_t cum, router::OrdStatus status) {
    router::NormalizedReport r = report(exec_id, router::ExecType::Trade, status);
    r.last_qty = qty;
    r.last_px = px;
    r.cum_qty = cum;
    r.leaves_qty = kQty - cum;
    r.avg_px = px;
    return r;
}

// ---------------------------------------------------------------------------

void the_installed_reducer_is_order_core() {
    std::printf("the_installed_reducer_is_order_core\n");

    gw::OrderCoreReducer reducer(reducer_options());
    const router::p03::ChainState chain = open_chain(reducer);

    // The aggregate riding in the projection is the proof. P04's own reducer
    // leaves it empty, so every later assertion rests on this one.
    const order_core::OrderAggregate* agg = gw::aggregate_of(chain);
    CHECK(agg != nullptr);
    if (agg == nullptr) return;

    CHECK(agg->symbol == kInstrument);  // translated, not the wire form
    CHECK(agg->account == "ACC1");
    CHECK(agg->side == order_core::Side::Buy);
    // No accepted version yet, and that is the point: create() is a locally
    // admitted New, not an acknowledged order. Acceptance comes from the peer,
    // so P03 sits in LocalPreparing and `accepted_versions` is empty. P04's own
    // reducer fills its accepted_* fields immediately, which is the difference
    // between a projection of intent and a record of what the LP is working.
    CHECK(!agg->has_accepted());
    CHECK(agg->lifecycle() == order_core::Lifecycle::LocalPreparing);
    CHECK(!chain.hold.has_value());

    // The acknowledgement is what creates the accepted version.
    router::p03::ChainState acked = chain;
    CHECK(reducer.apply(acked, ack()).outcome == router::p03::ReduceOutcome::Applied);
    const order_core::OrderAggregate* after = gw::aggregate_of(acked);
    CHECK(after != nullptr);
    if (after != nullptr) {
        CHECK(after->has_accepted());
        CHECK(after->accepted_qty_units() == kQty);
        CHECK(after->accepted_price_ticks() == kLimitPx);
        CHECK(after->lifecycle() == order_core::Lifecycle::Working);
    }

    // And P04's own reducer carries nothing, which is what makes the check
    // above meaningful rather than tautological.
    const router::p03::ChainState theirs = open_chain(router::p03::default_order_reducer());
    CHECK(gw::aggregate_of(theirs) == nullptr);
}

void a_fill_lands_in_order_cores_ledger_and_in_p04s_projection() {
    std::printf("a_fill_lands_in_order_cores_ledger_and_in_p04s_projection\n");

    gw::OrderCoreReducer reducer(reducer_options());
    router::p03::ChainState chain = open_chain(reducer);

    CHECK(reducer.apply(chain, ack()).outcome == router::p03::ReduceOutcome::Applied);

    const router::p03::ReduceResult r = reducer.apply(
        chain, fill("E-1", 5000, kLimitPx, 5000, router::OrdStatus::PartiallyFilled));
    CHECK_MSG(r.outcome == router::p03::ReduceOutcome::Applied, r.note);

    // order_core's ledger is the record.
    const order_core::OrderAggregate* agg = gw::aggregate_of(chain);
    CHECK(agg != nullptr);
    if (agg != nullptr) {
        CHECK(agg->ledger.size() == 1);
        CHECK(agg->cum_qty_units == 5000);
        CHECK(agg->find_fill("E-1") != nullptr);
    }

    // And P04's projection agrees, because that is what the router reads.
    CHECK(chain.cum_qty == 5000);
    CHECK(chain.leaves_qty == 5000);
    CHECK(chain.fills.size() == 1);
    CHECK(chain.status == router::OrdStatus::PartiallyFilled);
    CHECK(reducer.refusals() == 0);
}

void a_redelivered_fill_is_a_noop_not_a_second_fill() {
    std::printf("a_redelivered_fill_is_a_noop_not_a_second_fill\n");

    gw::OrderCoreReducer reducer(reducer_options());
    router::p03::ChainState chain = open_chain(reducer);
    reducer.apply(chain, ack());
    reducer.apply(chain, fill("E-1", 5000, kLimitPx, 5000, router::OrdStatus::PartiallyFilled));

    // The same execution again, which is what a FIX resend or a restart-driven
    // redelivery looks like. EI-11: one execution applies once.
    const router::p03::ReduceResult again = reducer.apply(
        chain, fill("E-1", 5000, kLimitPx, 5000, router::OrdStatus::PartiallyFilled));
    CHECK_MSG(again.outcome == router::p03::ReduceOutcome::Noop, again.note);

    const order_core::OrderAggregate* agg = gw::aggregate_of(chain);
    CHECK(agg != nullptr);
    if (agg != nullptr) {
        CHECK(agg->ledger.size() == 1);
        CHECK(agg->cum_qty_units == 5000);
    }
    CHECK(chain.cum_qty == 5000);
}

void a_buy_filled_above_its_limit_is_now_refused() {
    std::printf("a_buy_filled_above_its_limit_is_now_refused\n");

    gw::OrderCoreReducer reducer(reducer_options());
    router::p03::ChainState chain = open_chain(reducer);
    reducer.apply(chain, ack());
    reducer.apply(chain, fill("E-1", 5000, kLimitPx, 5000, router::OrdStatus::PartiallyFilled));

    // One tick above the limit of a buy. The LP has reported an execution the
    // order did not authorise, and enforce_limit_price_on_fill is what catches
    // it (what-if D-14, D-15).
    const router::p03::ReduceResult r =
        reducer.apply(chain, fill("E-2", 5000, kLimitPx + 1, kQty, router::OrdStatus::Filled));

    CHECK_MSG(r.outcome == router::p03::ReduceOutcome::Quarantine, r.note);
    CHECK(reducer.refusals() == 1);

    // The ledger holds only what the order authorised.
    const order_core::OrderAggregate* agg = gw::aggregate_of(chain);
    CHECK(agg != nullptr);
    if (agg != nullptr) {
        CHECK(agg->ledger.size() == 1);
        CHECK_MSG(agg->cum_qty_units == 5000, std::to_string(agg->cum_qty_units));
        CHECK(!agg->terminal());
    }
    CHECK(chain.cum_qty == 5000);

    // The same sequence through P04's own reducer, for contrast: it credits the
    // fill and calls the order done. Asserted, so that if P04's reducer is ever
    // fixed this stops being true out loud rather than quietly.
    router::p03::IOrderReducer& theirs = router::p03::default_order_reducer();
    router::p03::ChainState other = open_chain(theirs);
    theirs.apply(other, ack());
    theirs.apply(other, fill("E-1", 5000, kLimitPx, 5000, router::OrdStatus::PartiallyFilled));
    theirs.apply(other, fill("E-2", 5000, kLimitPx + 1, kQty, router::OrdStatus::Filled));
    CHECK(other.cum_qty == kQty);
    CHECK(other.terminal());
}

void an_unmapped_symbol_fails_closed() {
    std::printf("an_unmapped_symbol_fails_closed\n");

    gw::OrderCoreReducer::Options o;  // no symbol table at all
    gw::OrderCoreReducer reducer(o);
    const router::p03::ChainState chain = open_chain(reducer);

    // §2 pins the symbol policy, and P03 holds no scale for a symbol it does
    // not know. Refusing beats reducing against a guessed tick size.
    CHECK(reducer.unmapped_symbols() == 1);
    CHECK(chain.hold.has_value());
    CHECK(gw::aggregate_of(chain) == nullptr);
}

void a_wrong_scale_fails_closed() {
    std::printf("a_wrong_scale_fails_closed\n");

    gw::OrderCoreReducer reducer(reducer_options());
    // Three digits where the contract pins five. EI-15 wants this rejected
    // rather than rescaled.
    const router::p03::ChainState chain =
        reducer.create(router::OrderChainId("CH-2"), router::TenantId("T1"),
                       router::AccountId("ACC1"), router::LpId("LP_SIM_01"), kWireSymbol,
                       router::Side::Buy, 3, router::ClientOrderId("O-2"), kQty, kLimitPx);

    CHECK(reducer.scale_mismatches() == 1);
    CHECK(chain.hold == router::HoldReason::SchemaViolation);
    CHECK(gw::aggregate_of(chain) == nullptr);
}

}  // namespace

int main() {
    std::printf("P08 write path: order_core owns status and the fill ledger\n\n");

    the_installed_reducer_is_order_core();
    a_fill_lands_in_order_cores_ledger_and_in_p04s_projection();
    a_redelivered_fill_is_a_noop_not_a_second_fill();
    a_buy_filled_above_its_limit_is_now_refused();
    an_unmapped_symbol_fails_closed();
    a_wrong_scale_fails_closed();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
