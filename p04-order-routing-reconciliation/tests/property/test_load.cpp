// Load and soak measurement.
//
// Design doc 2.3 asks for p50/p95/p99 on two paths and a throughput figure, and
// 23.2 point 6 asks for the saturation point and the queue profile over time.
// These cases measure and write the numbers to an artifact rather than
// asserting a target: the document is explicit that the CV's figures are not
// P04's SLO, so there is nothing here to pass or fail on speed. What is
// asserted is correctness under the load.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

#include "support/model.hpp"

using namespace router;
using lab::Lab;

namespace {

// Wall clock percentiles, in microseconds, over a vector of samples.
struct WallStats {
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
    std::size_t count = 0;
};

WallStats wall_percentiles(std::vector<double> samples) {
    WallStats w;
    if (samples.empty()) return w;
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double q) {
        return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
    };
    w.count = samples.size();
    w.p50 = at(0.50);
    w.p95 = at(0.95);
    w.p99 = at(0.99);
    w.max = samples.back();
    return w;
}

// Appends one measurement block to the perf artifact, named after the scenario
// so a CI run can diff them.
//
// A note on the two kinds of number here. The router's own histograms are fed
// from the virtual clock, which only moves when a scenario advances it, so in
// this harness they are zero by construction: they prove the instrumentation is
// wired, not how fast anything is. The `wall_*` figures are real measurements
// taken with a steady clock around the call under test, and those are the ones
// worth reading.
void write_perf(const std::string& scenario, const Metrics& m,
                const std::vector<std::pair<std::string, double>>& extra,
                const WallStats& wall = {}) {
    std::error_code ec;
    std::filesystem::create_directories("test-artifacts", ec);
    std::ofstream f("test-artifacts/perf-" + scenario + ".json",
                    std::ios::binary | std::ios::trunc);
    if (!f) return;
    const Metrics::Percentiles admit = m.percentiles(metric::kCommandToIntentLatency);
    const Metrics::Percentiles apply = m.percentiles(metric::kInboundCommitLatency);
    f << "{\n  \"scenario\": \"" << scenario << "\",\n";
    f << "  \"command_to_intent_us\": {\"count\": " << admit.count << ", \"p50\": " << admit.p50
      << ", \"p95\": " << admit.p95 << ", \"p99\": " << admit.p99 << ", \"max\": " << admit.max
      << "},\n";
    f << "  \"inbound_commit_us\": {\"count\": " << apply.count << ", \"p50\": " << apply.p50
      << ", \"p95\": " << apply.p95 << ", \"p99\": " << apply.p99 << ", \"max\": " << apply.max
      << "},\n";
    f << "  \"virtual_clock_note\": \"the two histograms above come from the virtual clock "
         "and are zero by construction in this harness; wall_us is the real measurement\",\n";
    f << "  \"wall_us\": {\"count\": " << wall.count << ", \"p50\": " << wall.p50
      << ", \"p95\": " << wall.p95 << ", \"p99\": " << wall.p99 << ", \"max\": " << wall.max
      << "}";
    for (const auto& [k, v] : extra) f << ",\n  \"" << k << "\": " << v;
    f << "\n}\n";
}

}  // namespace

SCENARIO("LOAD-01", "P1", "a scaled deterministic soak with reconnects and duplicates") {
    // The full twenty-four hour soak needs real wall clock and OS sampling and
    // is recorded as BLOCKED in OR-P-17. This is the deterministic equivalent:
    // fifty thousand mixed steps with faults, duplicates and reconnects, from a
    // retained seed.
    lab::Model::Options o;
    o.seed = 20261030;
    o.max_chains = 24;
    o.inject_faults = true;
    o.inject_id_collisions = true;
    o.enable_status_queries = true;
    o.full_check_every = 100;
    lab::Model m(o);
    m.run(50000);

    // Correctness under the load: no duplicate execution anywhere, the audit
    // chain intact, every bound respected.
    std::size_t fills = 0;
    for (const auto& [id, c] : m.lab.disk.chains) {
        (void)id;
        std::set<std::string> ids;
        for (const p03::Fill& f : c.fills) {
            CHECK(ids.insert(f.exec_id.str()).second);
            ++fills;
        }
    }
    CHECK(m.lab.disk.audit.verify());
    CHECK(m.lab.store->pending_outbox().size() <= m.lab.cfg.limits.global_command_queue);
    CHECK(m.lab.quarantine_depth() <= m.lab.cfg.limits.orphan_quota);
    // Bounded label space, so a long run cannot grow the metric series.
    CHECK(m.lab.metrics.series_count() < std::size_t{200});

    write_perf("soak", m.lab.metrics,
               {{"steps", 50000.0},
                {"chains", static_cast<double>(m.lab.disk.chains.size())},
                {"fills", static_cast<double>(fills)},
                {"crashes", static_cast<double>(m.crashes)},
                {"quarantine", static_cast<double>(m.lab.quarantine_depth())},
                {"recon_cases", static_cast<double>(m.lab.disk.recon_cases.size())},
                {"metric_series", static_cast<double>(m.lab.metrics.series_count())}});
}

SCENARIO("LOAD-02", "P1", "command admission throughput and latency are measured") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 1000000;
    L.cfg.finalize();

    constexpr int kCommands = 20000;
    std::vector<double> samples;
    samples.reserve(kCommands);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCommands; ++i) {
        const auto a0 = std::chrono::steady_clock::now();
        auto r = L.submit(L.new_order("K" + std::to_string(i)));
        samples.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a0)
                .count());
        CHECK(r.is_ok());
        // One microsecond of virtual time per command, so the rate limiter and
        // the risk verdict expiry behave as they would in a real stream.
        L.advance(1);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    CHECK_EQ(L.disk.intents.size(), std::size_t{kCommands});
    CHECK_EQ(L.disk.outbox.size(), std::size_t{kCommands});
    const Metrics::Percentiles p = L.metrics.percentiles(metric::kCommandToIntentLatency);
    CHECK(p.count > 0);

    write_perf("admission", L.metrics,
               {{"commands", static_cast<double>(kCommands)},
                {"wall_seconds", seconds},
                {"commands_per_second", seconds > 0 ? kCommands / seconds : 0.0}},
               wall_percentiles(samples));
}

SCENARIO("LOAD-03", "P1", "inbound apply throughput and latency are measured") {
    Lab L;
    L.cfg.limits.admission_rate_per_account_per_sec = 1000000;
    std::vector<AcceptedIntent> chains;
    for (int i = 0; i < 200; ++i) {
        chains.push_back(L.must_submit_new("K" + std::to_string(i), "100000", "1.08500"));
        L.advance(1);
    }
    L.drive(64);

    constexpr int kFillsPerChain = 50;
    for (int round = 0; round < kFillsPerChain; ++round)
        for (const AcceptedIntent& a : chains) L.lp->execute(a.cl_ord_id.str(), 2000, 108500);

    std::vector<double> samples;
    const auto t0 = std::chrono::steady_clock::now();
    // Pumped in small batches so each batch is a wall clock sample.
    for (int i = 0; i < 200 * kFillsPerChain / 50 + 20; ++i) {
        const auto b0 = std::chrono::steady_clock::now();
        const InboundStats st = L.rt->pump_inbound(50);
        if (st.applied + st.deduplicated + st.quarantined == 0) break;
        samples.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0)
                .count() /
            50.0);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    for (const AcceptedIntent& a : chains) {
        const p03::ChainState* c = L.chain(a.chain_id);
        CHECK_EQ(c->fills.size(), std::size_t{kFillsPerChain});
        CHECK_EQ(c->cum_qty, 100000);
        CHECK_EQ(c->status, OrdStatus::Filled);
    }
    const Metrics::Percentiles p = L.metrics.percentiles(metric::kInboundCommitLatency);
    CHECK(p.count > 0);

    const double events = 200.0 * kFillsPerChain;
    write_perf("inbound", L.metrics,
               {{"reports", events},
                {"wall_seconds", seconds},
                {"reports_per_second", seconds > 0 ? events / seconds : 0.0}},
               wall_percentiles(samples));
}

SCENARIO("LOAD-04", "P1", "the saturation point of the bounded queue is located and reported") {
    Lab L;
    // A small bound so the knee is reached quickly; the shape is what matters.
    L.cfg.limits.global_command_queue = 500;
    L.cfg.limits.admission_rate_per_account_per_sec = 1000000;
    L.session->disconnect();

    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t knee = 0;
    for (int i = 0; i < 2000; ++i) {
        auto r = L.submit(L.new_order("K" + std::to_string(i)));
        if (r.is_ok()) {
            ++accepted;
        } else {
            if (knee == 0) knee = accepted;
            ++refused;
        }
        L.advance(1);
    }
    CHECK_EQ(knee, L.cfg.limits.global_command_queue);
    CHECK_EQ(accepted, L.cfg.limits.global_command_queue);
    CHECK_EQ(refused, std::size_t{2000} - accepted);
    CHECK_EQ(L.disk.intents.size(), accepted);

    // Draining recovers fully, with one send per accepted command.
    L.session->reconnect(false);
    L.drive(128);
    CHECK_EQ(L.rt->original_sends(), accepted);

    write_perf("saturation", L.metrics,
               {{"queue_bound", static_cast<double>(L.cfg.limits.global_command_queue)},
                {"accepted", static_cast<double>(accepted)},
                {"refused", static_cast<double>(refused)},
                {"knee_at", static_cast<double>(knee)}});
}
