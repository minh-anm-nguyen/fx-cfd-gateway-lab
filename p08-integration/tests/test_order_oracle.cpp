// Oracle A against the owner: P04's independent reducer versus order_core.
//
// Both are fed the same order chain -- a New acknowledgement and three fills --
// and compared after every event. P04's reducer knows nothing of order_core and
// shares no code with it, which is what §11.1 requires of an oracle and what
// EI-53 requires of a passing test.
//
// A disagreement here is a finding, not a bug in this file, so the run prints
// every one. What it fails on is narrower: the quantities that decide what a
// client is owed have to match exactly, while a difference in average price of
// a single tick is the documented consequence of P04 truncating where P03
// rounds, and is reported rather than treated as a defect.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "deps/p03_osm.hpp"
#include "order_core/aggregate.hpp"
#include "order_core/events.hpp"
#include "order_core/policy.hpp"
#include "order_core/reducer.hpp"
#include "order_oracle.hpp"
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
constexpr std::int64_t kOrderQty = 10000;
constexpr std::int64_t kLimitPx = 110000;
// P04's wire form and P03's instrument key, which are different strings.
constexpr const char* kWireSymbol = "EUR/USD";
constexpr const char* kInstrument = "EURUSD";

router::NormalizedReport base_report(const std::string& exec_id) {
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
    r.wire_symbol = kWireSymbol;
    r.side = router::Side::Buy;
    r.order_qty = kOrderQty;
    r.profile_version = "p1";
    return r;
}

// One acknowledgement, then three fills whose weighted average lands exactly on
// a half tick -- 110000.5 -- which is where a truncating and a rounding
// implementation have to part company if they are going to.
std::vector<router::NormalizedReport> chain_events(std::int64_t first_px,
                                                   std::int64_t second_px) {
    std::vector<router::NormalizedReport> out;

    router::NormalizedReport ack = base_report("E-ACK");
    ack.exec_type = router::ExecType::New;
    ack.ord_status = router::OrdStatus::New;
    ack.cum_qty = 0;
    ack.leaves_qty = kOrderQty;
    ack.avg_px = 0;
    out.push_back(ack);

    router::NormalizedReport fill1 = base_report("E-1");
    fill1.exec_type = router::ExecType::Trade;
    fill1.ord_status = router::OrdStatus::PartiallyFilled;
    fill1.last_qty = 5000;
    fill1.last_px = first_px;
    fill1.cum_qty = 5000;
    fill1.leaves_qty = 5000;
    fill1.avg_px = first_px;
    out.push_back(fill1);

    router::NormalizedReport fill2 = base_report("E-2");
    fill2.exec_type = router::ExecType::Trade;
    fill2.ord_status = router::OrdStatus::Filled;
    fill2.last_qty = 5000;
    fill2.last_px = second_px;
    fill2.cum_qty = kOrderQty;
    fill2.leaves_qty = 0;
    // 110000.5 exactly. Deliberately not what either side will compute, so the
    // comparison is between the two reducers rather than against the LP's own
    // claim.
    fill2.avg_px = first_px;
    out.push_back(fill2);

    return out;
}

order_core::LocalCommand new_command() {
    order_core::LocalCommand c;
    c.kind = order_core::CommandKind::New;
    c.account = "ACC1";
    c.symbol = kInstrument;
    c.side_raw = "1";  // FIX tag 54, Buy
    c.cl_ord_id = "O-1";
    c.qty_text = "10000";
    c.price_text = "1.10000";
    c.command_seq = 1;
    return c;
}

void print_disagreements(const std::vector<gw::oracle::ReducerDisagreement>& ds) {
    for (const gw::oracle::ReducerDisagreement& d : ds) {
        std::printf("    step %zu  %-11.*s  router=%-12s order_core=%s\n", d.step,
                    static_cast<int>(d.quantity.size()), d.quantity.data(), d.router.c_str(),
                    d.order_core.c_str());
    }
}

// ---------------------------------------------------------------------------

struct RunResult {
    std::vector<gw::oracle::ReducerDisagreement> disagreements;
    gw::oracle::Projection router_final;
    gw::oracle::Projection core_final;
};

RunResult run_chain(std::int64_t first_px, std::int64_t second_px) {
    RunResult result;

    // P04's side: a local projection, then the reports.
    router::p03::ChainState router_state = router::p03::default_order_reducer().create(
        router::OrderChainId("CH-1"), router::TenantId("T1"), router::AccountId("ACC1"),
        router::LpId("LP_SIM_01"), kWireSymbol, router::Side::Buy, kPriceDigits,
        router::ClientOrderId("O-1"), kOrderQty, 110000);

    // P03's side: the chain is opened by the local New command, which is the
    // event P04 models as create() rather than as an event at all.
    // P03 pins a scale per symbol and refuses a chain for a symbol it does not
    // hold, so the lab policy is the one that carries EURUSD at 5 digits.
    const order_core::Policy policy = order_core::default_lab_policy();
    order_core::OrderAggregate aggregate;
    const order_core::Reduction opened =
        order_core::reduce(aggregate, order_core::OrderEvent{new_command()}, policy, 0);
    CHECK_MSG(opened.mutates(), std::string("open: ") + order_core::to_string(opened.decision) +
                                    " " + opened.detail);
    if (opened.mutates()) aggregate = opened.next;


    std::size_t step = 1;
    for (const router::NormalizedReport& report : chain_events(first_px, second_px)) {
        const router::p03::ReduceResult router_result =
            router::p03::default_order_reducer().apply(router_state, report);
        (void)router_result;

        const order_core::PeerReport peer =
            gw::oracle::to_peer_report(report, kPriceDigits, kInstrument);
        const order_core::Reduction reduction =
            order_core::reduce(aggregate, order_core::OrderEvent{peer}, policy, step);
        if (reduction.mutates()) aggregate = reduction.next;

        const gw::oracle::Projection a = gw::oracle::project(router_state);
        const gw::oracle::Projection b = gw::oracle::project(aggregate);
        const std::vector<gw::oracle::ReducerDisagreement> ds =
            gw::oracle::compare(a, b, step);
        result.disagreements.insert(result.disagreements.end(), ds.begin(), ds.end());
        result.router_final = a;
        result.core_final = b;

        std::printf("    step %zu: router cum=%lld leaves=%lld avg=%lld term=%d | "
                    "order_core cum=%lld leaves=%lld avg=%lld term=%d (%s)\n",
                    step, static_cast<long long>(a.cum_qty),
                    static_cast<long long>(a.leaves_qty),
                    static_cast<long long>(a.avg_px_ticks), a.terminal ? 1 : 0,
                    static_cast<long long>(b.cum_qty), static_cast<long long>(b.leaves_qty),
                    static_cast<long long>(b.avg_px_ticks), b.terminal ? 1 : 0,
                    order_core::to_string(reduction.decision));
        ++step;
    }

    if (!result.disagreements.empty()) {
        std::printf("  disagreements:\n");
        print_disagreements(result.disagreements);
    }
    return result;
}

void two_independent_reducers_agree_on_a_clean_chain() {
    std::printf("two_independent_reducers_agree_on_a_clean_chain\n");

    // Both fills at or inside the limit, and a weighted average landing on an
    // exact half tick (109999.5) so truncation and rounding have to part company
    // if they are going to.
    const RunResult r = run_chain(109999, kLimitPx);

    // What has to match exactly. A difference in executed quantity is a
    // difference in what the firm owes, and there is no tolerance for it.
    for (const gw::oracle::ReducerDisagreement& d : r.disagreements) {
        if (d.quantity == "cum_qty" || d.quantity == "leaves_qty" ||
            d.quantity == "terminal" || d.quantity == "status") {
            CHECK_MSG(false, std::string(d.quantity) + ": router=" + d.router +
                                 " order_core=" + d.order_core);
        }
    }

    // Average price may differ by the one tick that separates truncation from
    // rounding. More than that is not a rounding mode, it is a different
    // computation.
    for (const gw::oracle::ReducerDisagreement& d : r.disagreements) {
        if (d.quantity != "avg_px") continue;
        const long long router_px = std::stoll(d.router);
        const long long core_px = std::stoll(d.order_core);
        const long long delta = router_px > core_px ? router_px - core_px : core_px - router_px;
        CHECK_MSG(delta <= 1, "avg_px differs by " + std::to_string(delta) + " ticks");
    }

    // The chain finished, on both sides.
    CHECK(r.router_final.cum_qty == kOrderQty);
    CHECK(r.core_final.cum_qty == kOrderQty);
    CHECK(r.router_final.terminal);
    CHECK(r.core_final.terminal);
}

void a_buy_filled_above_its_limit_is_where_they_part() {
    std::printf("a_buy_filled_above_its_limit_is_where_they_part\n");

    // The finding this oracle was built to produce. The second fill prints one
    // tick above the limit price of a buy order, which is not a rounding
    // question: the LP has reported an execution the order did not authorise.
    //
    // P03 refuses it. Its policy carries enforce_limit_price_on_fill (what-if
    // D-14, D-15), the chain goes to QUARANTINED, and cum_qty stays at the 5000
    // it can actually prove. P04's reducer applies it and reports the order
    // Filled for 10000.
    //
    // Section 2 gives P03 order status and the fill ledger, so P03 is right and
    // this is a defect in P04's reducer -- exactly what an oracle sharing no
    // code with either side exists to surface. It is asserted rather than
    // merely printed, so fixing P04 makes this case fail and forces someone to
    // revisit this comment.
    const RunResult r = run_chain(kLimitPx, kLimitPx + 1);

    bool cum_diverged = false;
    bool terminal_diverged = false;
    for (const gw::oracle::ReducerDisagreement& d : r.disagreements) {
        if (d.quantity == "cum_qty") cum_diverged = true;
        if (d.quantity == "terminal") terminal_diverged = true;
    }
    CHECK(cum_diverged);
    CHECK(terminal_diverged);

    // P04 credited the unauthorised fill; P03 did not.
    CHECK(r.router_final.cum_qty == kOrderQty);
    CHECK(r.router_final.terminal);
    CHECK_MSG(r.core_final.cum_qty == 5000, std::to_string(r.core_final.cum_qty));
    CHECK(!r.core_final.terminal);

    // What each side now claims about the order. A QUARANTINED reduction is not
    // applied, so P03's aggregate still holds the state it could prove at step
    // 2 -- partially filled for 5000 -- rather than moving to a state of its
    // own. The quarantine itself is an incident for the engine to record; the
    // pure reducer's job here is to refuse, and refusing means leaving the
    // aggregate alone.
    CHECK(r.core_final.status == router::OrdStatus::PartiallyFilled);
    CHECK(r.router_final.status == router::OrdStatus::Filled);
}

void a_p03_only_state_is_not_a_disagreement() {
    std::printf("a_p03_only_state_is_not_a_disagreement\n");

    // Quarantined, ReconciliationRequired, LocalPreparing and NewSentUnknown
    // are statements about what the gateway knows, and FIX tag 39 has no value
    // for any of them. Flattening one onto a neighbouring status would report a
    // contradiction as a working order, which is what EI-19 forbids.
    CHECK(!gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::Quarantined).has_value());
    CHECK(!gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::ReconciliationRequired)
               .has_value());
    CHECK(!gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::LocalPreparing).has_value());
    CHECK(!gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::NewSentUnknown).has_value());

    // And the ones that do map, map to the FIX status of the same name.
    CHECK(gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::Filled) ==
          router::OrdStatus::Filled);
    CHECK(gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::PartFilled) ==
          router::OrdStatus::PartiallyFilled);
    CHECK(gw::oracle::lifecycle_to_ord_status(order_core::Lifecycle::PendingCancel) ==
          router::OrdStatus::PendingCancel);

    // A projection where one side has no FIX status raises nothing.
    gw::oracle::Projection router_side;
    router_side.status = router::OrdStatus::New;
    gw::oracle::Projection core_side;
    core_side.status = std::nullopt;
    CHECK(gw::oracle::compare(router_side, core_side, 0).empty());
}

void the_rendering_carries_the_scale() {
    std::printf("the_rendering_carries_the_scale\n");

    router::NormalizedReport r = base_report("E-1");
    r.exec_type = router::ExecType::Trade;
    r.ord_status = router::OrdStatus::PartiallyFilled;
    r.last_qty = 5000;
    r.last_px = 110000;
    r.cum_qty = 5000;
    r.leaves_qty = 5000;
    r.avg_px = 110000;

    const order_core::PeerReport at_five = gw::oracle::to_peer_report(r, 5, kInstrument);
    CHECK(at_five.last_px.has_value());
    if (at_five.last_px.has_value()) CHECK_MSG(*at_five.last_px == "1.10000", *at_five.last_px);
    // Quantities are counts of base units, so they render at scale 0. Rendering
    // them at the price scale would multiply every size by 100000.
    CHECK(at_five.last_qty.has_value());
    if (at_five.last_qty.has_value()) CHECK_MSG(*at_five.last_qty == "5000", *at_five.last_qty);

    // The same ticks under a different scale are a different price. This is the
    // EI-15 surface: the number alone does not carry its own meaning, which is
    // why the contract gate pins the scale and the adapters refuse a mismatch.
    const order_core::PeerReport at_three = gw::oracle::to_peer_report(r, 3, kInstrument);
    CHECK(at_three.last_px.has_value());
    if (at_three.last_px.has_value()) CHECK_MSG(*at_three.last_px == "110.000", *at_three.last_px);

    // The enum mapping comes from P04's own dictionary rather than a second
    // table here, so the oracle agrees with what the router actually encodes.
    CHECK_MSG(at_five.exec_type == "F", at_five.exec_type);
    CHECK_MSG(at_five.ord_status == "1", at_five.ord_status);
    CHECK_MSG(at_five.msg_type == '8', std::string(1, at_five.msg_type));
}

}  // namespace

int main() {
    std::printf("P08 Oracle A: P04's independent reducer vs order_core\n\n");

    two_independent_reducers_agree_on_a_clean_chain();
    a_buy_filled_above_its_limit_is_where_they_part();
    a_p03_only_state_is_not_a_disagreement();
    the_rendering_carries_the_scale();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
