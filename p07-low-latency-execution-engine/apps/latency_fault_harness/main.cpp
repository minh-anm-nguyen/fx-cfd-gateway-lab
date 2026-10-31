// Operator-facing fault harness.
//
// Design doc 20 lists apps/latency_fault_harness alongside the benchmark. Its
// job is different: it drives one named scenario with one failpoint armed and
// prints what the engine did, so an operator can reproduce a playbook by hand
// and read the evidence doc 23 requires -- the failpoint hit count, the mode
// transitions, the outcome accounting and the final state hash.
//
// It links the lab library, which has the failpoints compiled in. That is the
// opposite choice from the benchmark and it is deliberate (ADR-08): this tool
// exists to inject faults, so it must never be used to produce a timing.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "llx/engine.hpp"

using namespace llx;

namespace {

constexpr DomainId kDomain = 1;
constexpr SymbolId kSymbol = 7;
constexpr Qty kLimit = 1000000 * kQtyScale;

void usage() {
    std::printf(
        "latency_fault_harness -- drive one scenario with one fault armed\n"
        "\n"
        "  --scenario=NAME    new_order | fill_cycle | cancel_race | saturate |\n"
        "                     unknown_commit | failover\n"
        "  --failpoint=SPEC   name=error|exit|delay:MS|pause|corrupt:N[@occurrence]\n"
        "                     comma-separated for several\n"
        "  --journal=PATH     file-backed journal, so a crash can be inspected\n"
        "  --count=N          events for the scenario (default 100)\n"
        "  --capacity=N       ring capacity: 256|1024|8192|65536\n"
        "  --list-failpoints  print every instrumented boundary and exit\n"
        "\n"
        "This tool injects faults. Never use it to measure latency: the failpoint\n"
        "checks are on the hot path in this build (ADR-08).\n"
        "\n"
        "Examples\n"
        "  latency_fault_harness --scenario=unknown_commit \\\n"
        "      --failpoint=store.commit_lost_ack=error\n"
        "  latency_fault_harness --scenario=new_order --journal=/tmp/j.bin \\\n"
        "      --failpoint=store.after_outbox=exit\n");
}

void list_failpoints() {
    std::printf("instrumented boundaries (design doc 25 and 26):\n");
    const char* names[] = {
        "ring.before_slot_write", "ring.after_slot_write", "ring.after_head_release",
        "ring.after_consumer_acquire", "ring.before_tail_release",
        "pool.after_lease", "pool.before_release", "pool.after_release", "pool.exhaust",
        "owner.after_dequeue", "owner.after_reduce", "owner.after_risk_compute",
        "owner.after_risk_stage", "owner.before_commit", "owner.after_commit_result",
        "store.after_begin", "store.after_inbox", "store.after_journal",
        "store.after_projection", "store.after_risk_row", "store.after_outbox",
        "store.commit_lost_ack", "store.commit_stall", "store.after_commit",
        "dispatch.after_claim", "dispatch.before_encode", "dispatch.after_encode",
        "dispatch.partial_write", "dispatch.after_write",
        "inbound.after_parse", "telemetry.buffer_full", "clock.rewind",
        "affinity.pin_fail",
    };
    for (const char* n : names) std::printf("  %s\n", n);
    std::printf(
        "\nactions: error (the operation reports a typed failure), exit (terminate\n"
        "immediately, no unwinding -- the SIGKILL stand-in), delay:MS, pause (park\n"
        "until released), corrupt:N\n");
}

InputEnvelope new_order(OrderId id, Qty qty, Side side = Side::Buy) {
    InputEnvelope e;
    e.event_id = EventId{0xFA, id};
    e.type = EventType::NewOrder;
    e.lane = Lane::NewOrder;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.side = side;
    e.qty = qty;
    e.px = 1 * kPxScale;
    e.order_id = id;
    e.receive_ns = MonotonicClock::now_ns();
    return e;
}

InputEnvelope exec_report(OrderId id, ExecId xid, ExecType t, Qty qty = 0) {
    InputEnvelope e;
    e.event_id = EventId{0xFB, xid};
    e.type = EventType::ExecutionReport;
    e.lane = Lane::AcceptedReceipt;
    e.accepted_evidence = true;
    e.risk_domain = kDomain;
    e.symbol = kSymbol;
    e.order_id = id;
    e.exec_id = xid;
    e.exec_type = t;
    e.last_qty = qty;
    e.last_px = 1 * kPxScale;
    e.receive_ns = MonotonicClock::now_ns();
    return e;
}

InputEnvelope cancel_request(OrderId id, OrderId orig) {
    InputEnvelope e;
    e.event_id = EventId{0xFC, id};
    e.type = EventType::CancelRequest;
    e.lane = Lane::CancelReplace;
    e.risk_domain = kDomain;
    e.order_id = id;
    e.orig_order_id = orig;
    return e;
}

void drive(Engine& engine, const std::string& scenario, int count) {
    if (scenario == "new_order") {
        for (int i = 1; i <= count; ++i) {
            engine.offer(new_order(static_cast<OrderId>(i), 10 * kQtyScale));
            engine.run_until_idle();
        }
    } else if (scenario == "fill_cycle") {
        ExecId x = 1000;
        for (int i = 1; i <= count; ++i) {
            const auto id = static_cast<OrderId>(i);
            engine.offer(new_order(id, 10 * kQtyScale));
            engine.run_until_idle();
            engine.offer(exec_report(id, x++, ExecType::New));
            engine.run_until_idle();
            engine.offer(exec_report(id, x++, ExecType::PartialFill, 4 * kQtyScale));
            engine.run_until_idle();
            engine.offer(exec_report(id, x++, ExecType::Fill, 6 * kQtyScale));
            engine.run_until_idle();
        }
    } else if (scenario == "cancel_race") {
        ExecId x = 2000;
        for (int i = 1; i <= count; ++i) {
            const auto id = static_cast<OrderId>(i * 2);
            engine.offer(new_order(id, 10 * kQtyScale));
            engine.run_until_idle();
            engine.offer(exec_report(id, x++, ExecType::New));
            engine.run_until_idle();
            engine.offer(cancel_request(id + 1, id));
            engine.run_until_idle();
            // The fill causally precedes the cancel's confirmation.
            engine.offer(exec_report(id, x++, ExecType::PartialFill, 3 * kQtyScale));
            engine.run_until_idle();
            engine.offer(exec_report(id, x++, ExecType::Canceled));
            engine.run_until_idle();
        }
    } else if (scenario == "saturate") {
        // Offer without draining, so the queue fills and the service-mode
        // ladder is exercised.
        for (int i = 1; i <= count; ++i)
            engine.offer(new_order(static_cast<OrderId>(i), 10 * kQtyScale));
        engine.run_until_idle();
    } else if (scenario == "unknown_commit") {
        engine.offer(new_order(1, 10 * kQtyScale));
        engine.run_until_idle();
        // Anything offered afterwards should be held, not issued.
        for (int i = 2; i <= count; ++i) {
            engine.offer(new_order(static_cast<OrderId>(i), 10 * kQtyScale));
            engine.run_until_idle();
        }
    } else if (scenario == "failover") {
        for (int i = 1; i <= count; ++i) {
            engine.offer(new_order(static_cast<OrderId>(i), 10 * kQtyScale));
            engine.run_until_idle();
            if (i == count / 2) engine.owner().promote_to_epoch(2);
        }
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenario = "new_order";
    std::string failpoint;
    std::string journal;
    int count = 100;
    std::size_t capacity = 8192;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--list-failpoints") {
            list_failpoints();
            return 0;
        } else if (a.rfind("--scenario=", 0) == 0) scenario = a.substr(11);
        else if (a.rfind("--failpoint=", 0) == 0) failpoint = a.substr(12);
        else if (a.rfind("--journal=", 0) == 0) journal = a.substr(10);
        else if (a.rfind("--count=", 0) == 0) count = std::atoi(a.c_str() + 8);
        else if (a.rfind("--capacity=", 0) == 0)
            capacity = std::strtoull(a.c_str() + 11, nullptr, 10);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 1;
        }
    }

#ifndef LLX_ENABLE_FAILPOINTS
    std::fprintf(stderr,
                 "this binary was built without failpoints, so no fault can be "
                 "injected. Link llx, not llx_measured.\n");
    return 3;
#else
    if (!failpoint.empty()) {
        // A malformed specification aborts rather than running an
        // uninstrumented binary a playbook would then score as a pass.
        fp::arm_from_spec(failpoint);
        std::printf("armed: %s\n", failpoint.c_str());
    }
    fp::arm_from_env();

    Engine::Config cfg;
    cfg.spsc_capacity = capacity;
    cfg.pool_slots_per_slab = 8192;
    cfg.pool_slabs = static_cast<std::uint32_t>((capacity + 8192) / 8192 + 1);
    cfg.journal_path = journal;
    Engine engine(cfg);
    std::string reject;
    if (!engine.preflight(reject)) {
        std::fprintf(stderr, "preflight rejected: %s\n", reject.c_str());
        return 2;
    }
    engine.store().configure_risk_domain(kDomain, kLimit);
    engine.refresh_mode();

    std::printf("scenario=%s count=%d capacity=%zu journal=%s\n", scenario.c_str(), count,
                capacity, journal.empty() ? "(memory only)" : journal.c_str());
    std::printf("starting mode: %s\n",
                std::string(to_string(engine.modes().mode())).c_str());

    drive(engine, scenario, count);

    // Everything doc 23 wants in a per-case artifact, printed for a human.
    std::printf("\n--- engine state ---\n");
    std::printf("mode: %s\n", std::string(to_string(engine.modes().mode())).c_str());
    std::printf("orders=%zu fills=%zu outbox=%zu journal_records=%zu\n",
                engine.store().state().orders.size(), engine.store().state().fills.size(),
                engine.store().outbox_snapshot().size(), engine.store().record_count());
    std::printf("state_hash=%s\n", engine.store().state_hash().c_str());

    const OwnerStats& os = engine.owner().stats();
    std::printf("\nowner: seen=%llu accepted=%llu committed=%llu rejected=%llu\n",
                static_cast<unsigned long long>(os.events_seen),
                static_cast<unsigned long long>(os.accepted),
                static_cast<unsigned long long>(os.committed),
                static_cast<unsigned long long>(os.rejected_business));
    std::printf("       duplicates=%llu quarantined=%llu unknown=%llu stale_epoch=%llu "
                "reordered=%llu\n",
                static_cast<unsigned long long>(os.duplicates),
                static_cast<unsigned long long>(os.quarantined),
                static_cast<unsigned long long>(os.commit_unknown),
                static_cast<unsigned long long>(os.stale_epoch),
                static_cast<unsigned long long>(os.receipts_reordered));

    const StoreStats& ss = engine.store().stats();
    std::printf("store: committed=%llu rejected=%llu unknown=%llu stale_epoch=%llu "
                "out_of_order=%llu inbox_dup=%llu syncs=%llu discarded=%llu\n",
                static_cast<unsigned long long>(ss.commits_committed),
                static_cast<unsigned long long>(ss.commits_rejected),
                static_cast<unsigned long long>(ss.commits_unknown),
                static_cast<unsigned long long>(ss.stale_epoch_refusals),
                static_cast<unsigned long long>(ss.out_of_order_refusals),
                static_cast<unsigned long long>(ss.inbox_duplicates),
                static_cast<unsigned long long>(ss.syncs),
                static_cast<unsigned long long>(ss.records_discarded));

    const DispatchStats& ds = engine.dispatcher().stats();
    std::printf("dispatch: claimed=%llu sent=%llu ambiguous=%llu failed=%llu "
                "duplicate_original_attempts=%llu\n",
                static_cast<unsigned long long>(ds.claimed),
                static_cast<unsigned long long>(ds.sent),
                static_cast<unsigned long long>(ds.ambiguous),
                static_cast<unsigned long long>(ds.failed),
                static_cast<unsigned long long>(ds.duplicate_original_attempts));

    std::printf("\noutcomes: %s\n", engine.outcomes().to_json().c_str());

    std::printf("\nmode transitions:\n");
    if (engine.modes().transitions().empty()) std::printf("  (none)\n");
    for (const ModeTransition& t : engine.modes().transitions())
        std::printf("  %s -> %s: %s\n", std::string(to_string(t.from)).c_str(),
                    std::string(to_string(t.to)).c_str(), t.reason.c_str());

    if (!failpoint.empty()) {
        // Doc 25: a playbook may not be scored unless the failpoint fired.
        std::printf("\nfailpoint evidence:\n");
        std::size_t start = 0;
        while (start <= failpoint.size()) {
            const std::size_t comma = failpoint.find(',', start);
            const std::string entry = failpoint.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            const std::size_t eq = entry.find('=');
            if (eq != std::string::npos) {
                const std::string name = entry.substr(0, eq);
                std::printf("  %s: hits=%llu fired=%llu%s\n", name.c_str(),
                            static_cast<unsigned long long>(fp::hits(name)),
                            static_cast<unsigned long long>(fp::fired(name)),
                            fp::fired(name) == 0
                                ? "   <-- NEVER FIRED: this run proves nothing"
                                : "");
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    const Engine::SelfCheck sc = engine.self_check();
    std::printf("\nself check: %s\n", sc.ok() ? "all green" : sc.detail.c_str());
    const BusinessState::Check bc = engine.store().state().check();
    std::printf("business invariants: %s\n", bc.ok() ? "all green" : bc.detail.c_str());

    const DrainReport dr = engine.graceful_stop();
    std::printf("\ngraceful stop: %s%s\n", dr.complete ? "complete" : "INCOMPLETE",
                dr.detail.empty() ? "" : (" -- " + dr.detail).c_str());

    return (sc.ok() && bc.ok()) ? 0 : 1;
#endif
}
