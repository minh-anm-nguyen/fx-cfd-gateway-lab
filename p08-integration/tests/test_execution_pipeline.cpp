// P07's queue and pool carrying the gateway's work, on two threads.
//
// What a test of a threaded component can honestly assert is an invariant, not
// an interleaving. Timing decides how many submissions meet a full ring, so
// pinning that number would only pin the machine this ran on. What must hold on
// every machine is the accounting:
//
//   EI-45  accepted == processed + still queued, and every submission came back
//          either accepted or refused with a reason. Nothing disappears.
//   EI-43  a full ring and an exhausted pool are told apart and reported, never
//          absorbed.
//   EI-42  one producer and one consumer. A second producer is refused by the
//          ring's ownership guard rather than quietly corrupting the queue.
//
// The router here is wired with P04's own in-tree risk and session rather than
// the real adapters. That is deliberate: this file is about the hand-off, and a
// deterministic, fast stack keeps the subject the queue rather than the seams
// other tests already cover.
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "deps/p01_session.hpp"
#include "deps/p02_lp.hpp"
#include "deps/p05_risk.hpp"
#include "gw/execution_pipeline.hpp"
#include "router/router.hpp"

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

// The router and everything single-owner behind it.
struct Lab {
    router::Config cfg = router::Config::lab_default();
    router::VirtualClock clock;
    router::FailpointRegistry fp;
    router::Metrics metrics;
    router::StoreImage store_image;
    router::p01::SessionImage session_image;
    router::p02::LpImage lp_image;

    std::unique_ptr<router::p05::FakeRisk> risk;
    std::unique_ptr<router::Store> store;
    std::unique_ptr<router::p01::SessionEngine> session;
    std::unique_ptr<router::p02::LpSimulator> lp;
    std::unique_ptr<router::Router> rt;
    bool booted = false;

    Lab() {
        session_image.identity = {"ROUTER04", "LPSIM01", "PRIMARY"};
        session_image.state = router::p01::SessionState::Active;
        risk = std::make_unique<router::p05::FakeRisk>(clock, cfg.limits.risk_verdict_ttl_micros);
        store = std::make_unique<router::Store>(store_image, fp, clock);
        session = std::make_unique<router::p01::SessionEngine>(session_image, fp);
        lp = std::make_unique<router::p02::LpSimulator>(lp_image, *session);
        rt = std::make_unique<router::Router>(cfg, *store, *session, *risk, metrics, fp, clock);
        booted = rt->boot().is_ok();
    }

    router::CommandEnvelope new_order(const std::string& id) const {
        router::CommandEnvelope cmd;
        cmd.principal.actor = router::ActorId("trader-1");
        cmd.principal.tenant = router::TenantId("T1");
        cmd.principal.role = router::Role::Trader;
        cmd.account = router::AccountId("ACC1");
        cmd.kind = router::CommandKind::New;
        cmd.client_command_id = id;
        cmd.symbol = "EUR/USD";
        cmd.side = router::Side::Buy;
        cmd.qty_text = "10000";
        cmd.price_text = "1.10000";
        return cmd;
    }
};

gw::ExecutionPipeline::Options small_options() {
    gw::ExecutionPipeline::Options o;
    // A small batch and a small pool, so a burst actually meets the bounds
    // rather than sailing past them on a fast machine.
    o.drain_batch = 4;
    o.pool_slabs = 1;
    o.pool_slots_per_slab = 16;
    return o;
}

// ---------------------------------------------------------------------------

void work_crosses_the_ring_and_reaches_the_router() {
    std::printf("work_crosses_the_ring_and_reaches_the_router\n");

    Lab lab;
    CHECK(lab.booted);
    if (!lab.booted) return;

    gw::ExecutionPipeline pipeline(*lab.rt, small_options());
    pipeline.start();

    constexpr int kOrders = 8;
    int accepted = 0;
    for (int i = 0; i < kOrders; ++i) {
        if (pipeline.submit(lab.new_order("K-" + std::to_string(i))) ==
            gw::SubmitStatus::Accepted) {
            ++accepted;
        }
    }
    pipeline.stop();

    CHECK(accepted > 0);
    CHECK_MSG(pipeline.processed() == static_cast<std::uint64_t>(accepted),
              std::to_string(pipeline.processed()) + " processed of " +
                  std::to_string(accepted) + " accepted");
    // The router answered every one of them, and the orders really went through
    // admission rather than being counted on the way past.
    CHECK(pipeline.every_processed_command_was_answered());
    CHECK(pipeline.admitted_by_router() > 0);
    CHECK(lab.rt->command_status(router::TenantId("T1"), router::CommandId("K-0")).has_value());
}

void nothing_is_lost_however_the_burst_lands() {
    std::printf("nothing_is_lost_however_the_burst_lands\n");

    Lab lab;
    CHECK(lab.booted);
    if (!lab.booted) return;

    gw::ExecutionPipeline pipeline(*lab.rt, small_options());
    pipeline.start();

    // More than the ring and the pool can hold at once, submitted as fast as
    // one thread can. How many are refused depends on the machine; that none
    // vanishes does not.
    constexpr int kBurst = 500;
    std::uint64_t accepted = 0;
    std::uint64_t queue_full = 0;
    std::uint64_t pool_exhausted = 0;
    std::uint64_t other = 0;
    for (int i = 0; i < kBurst; ++i) {
        switch (pipeline.submit(lab.new_order("B-" + std::to_string(i)))) {
            case gw::SubmitStatus::Accepted: ++accepted; break;
            case gw::SubmitStatus::QueueFull: ++queue_full; break;
            case gw::SubmitStatus::PoolExhausted: ++pool_exhausted; break;
            default: ++other; break;
        }
    }

    // Every submission got an answer. EI-43: a refusal is explicit, and there is
    // no fourth outcome where the event is simply gone.
    CHECK_MSG(accepted + queue_full + pool_exhausted + other ==
                  static_cast<std::uint64_t>(kBurst),
              "accounted " + std::to_string(accepted + queue_full + pool_exhausted + other) +
                  " of " + std::to_string(kBurst));
    CHECK(other == 0);
    CHECK(pipeline.rejected_queue_full() == queue_full);
    CHECK(pipeline.rejected_pool_exhausted() == pool_exhausted);

    pipeline.stop();

    // EI-45, after the drain: what was accepted was processed, and the ring is
    // empty rather than holding work nobody will finish.
    CHECK(pipeline.queue_depth() == 0);
    CHECK_MSG(pipeline.processed() == accepted,
              std::to_string(pipeline.processed()) + " processed of " +
                  std::to_string(accepted) + " accepted");
    CHECK(pipeline.accounting_balances());
    CHECK(pipeline.every_processed_command_was_answered());

    std::printf("    accepted=%llu queue_full=%llu pool_exhausted=%llu processed=%llu\n",
                static_cast<unsigned long long>(accepted),
                static_cast<unsigned long long>(queue_full),
                static_cast<unsigned long long>(pool_exhausted),
                static_cast<unsigned long long>(pipeline.processed()));

    // Whether the bounds were actually met is a property of the machine. The
    // accounting passed either way, but a run that reached neither bound proved
    // nothing about them, and saying so beats a silent pass.
    if (queue_full + pool_exhausted == 0) {
        std::printf("    note: neither bound was reached on this run\n");
    }
}

void a_second_producer_is_refused() {
    std::printf("a_second_producer_is_refused\n");

    Lab lab;
    CHECK(lab.booted);
    if (!lab.booted) return;

    gw::ExecutionPipeline pipeline(*lab.rt, small_options());
    pipeline.start();

    // The first thread to push claims the producer role for the lifetime of the
    // ring.
    CHECK(pipeline.submit(lab.new_order("P-0")) == gw::SubmitStatus::Accepted);

    std::atomic<int> wrong_producer{0};
    std::atomic<int> other{0};
    std::thread intruder([&] {
        for (int i = 0; i < 4; ++i) {
            if (pipeline.submit(lab.new_order("X-" + std::to_string(i))) ==
                gw::SubmitStatus::WrongProducer) {
                ++wrong_producer;
            } else {
                ++other;
            }
        }
    });
    intruder.join();

    // EI-42. A single-producer ring that accepted a second producer would
    // corrupt its own counters, so it refuses and records the attempt instead.
    CHECK_MSG(wrong_producer.load() == 4,
              std::to_string(wrong_producer.load()) + " of 4 refused");
    CHECK(other.load() == 0);
    CHECK(pipeline.guard_violations() > 0);

    pipeline.stop();
    // And the intruder's work never entered the system.
    CHECK(!lab.rt->command_status(router::TenantId("T1"), router::CommandId("X-0")).has_value());
    CHECK(pipeline.accounting_balances());
}

void submitting_before_start_or_after_stop_is_refused() {
    std::printf("submitting_before_start_or_after_stop_is_refused\n");

    Lab lab;
    CHECK(lab.booted);
    if (!lab.booted) return;

    gw::ExecutionPipeline pipeline(*lab.rt, small_options());

    // Accepting work with nobody to drain it would make the accounting a
    // promise the pipeline cannot keep.
    CHECK(pipeline.submit(lab.new_order("E-0")) == gw::SubmitStatus::NotRunning);

    pipeline.start();
    CHECK(pipeline.submit(lab.new_order("E-1")) == gw::SubmitStatus::Accepted);
    pipeline.stop();

    CHECK(pipeline.submit(lab.new_order("E-2")) == gw::SubmitStatus::NotRunning);
    CHECK(pipeline.accounting_balances());
    CHECK(pipeline.processed() == 1);
}

}  // namespace

int main() {
    std::printf("P08 execution pipeline: P07's ring and pool on two threads\n\n");

    work_crosses_the_ring_and_reaches_the_router();
    nothing_is_lost_however_the_burst_lands();
    a_second_producer_is_refused();
    submitting_before_start_or_after_stop_is_refused();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
