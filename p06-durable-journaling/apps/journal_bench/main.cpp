// journal_bench — measures what this host does, and says so.
//
// Section 19.1 is blunt about the difference between a measurement and an
// SLO: a number from a machine that is not the target describes that
// machine. This prints commit and replay rates for the host it runs on,
// labelled as such, and makes no claim about any target.
//
//   journal_bench [--orders N] [--fills-per-order N] [--db <path>]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "jrnl/replay.hpp"
#include "jrnl/service.hpp"
#include "jrnl/snapshot.hpp"
#include "jrnl/store.hpp"

using namespace jrnl;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    int orders = 200;
    int fills_per_order = 5;
    std::string db;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc) orders = std::atoi(argv[++i]);
        else if (a == "--fills-per-order" && i + 1 < argc) fills_per_order = std::atoi(argv[++i]);
        else if (a == "--db" && i + 1 < argc) db = argv[++i];
        else {
            std::fprintf(stderr,
                         "usage: journal_bench [--orders N] [--fills-per-order N] "
                         "[--db <path>]\n");
            return 2;
        }
    }
    if (orders <= 0 || fills_per_order < 0) {
        std::fprintf(stderr, "journal_bench: counts must be positive\n");
        return 2;
    }

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jrnl-bench";
    std::filesystem::create_directories(dir, ec);
    if (db.empty()) db = (dir / "bench.db").string();
    std::filesystem::remove(db, ec);

    SqliteAtomicStore::Options opt;
    opt.path = db;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    if (SqliteAtomicStore::open(opt, &store, &why) != StoreError::None) {
        std::fprintf(stderr, "cannot open %s: %s\n", db.c_str(), why.c_str());
        return 1;
    }

    const TenantId tenant = "bench";
    const RiskDomainId domain = "D1";
    FenceToken token = 0;
    store->acquire_fence(tenant, domain, "bench", 600000, &token);

    GatewayService::Options gopt;
    GatewayService svc(*store, gopt);
    const std::int64_t qty = 1000;
    const std::int64_t limit = static_cast<std::int64_t>(orders) * qty + 1;
    if (svc.set_risk_limit(tenant, domain, limit, token).kind != CommitKind::Committed) {
        std::fprintf(stderr, "cannot set the bench risk limit\n");
        return 1;
    }

    // --- Admission ---------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();
    int admitted = 0;
    for (int i = 0; i < orders; ++i) {
        NewOrderRequest r;
        r.tenant = tenant;
        r.account = "A1";
        r.command_id = "K" + std::to_string(i);
        r.chain = "O" + std::to_string(i);
        r.cl_ord_id = r.command_id + "-cl";
        r.symbol = "EURUSD";
        r.side = "BUY";
        r.order_qty = qty;
        r.limit_px = 10000;
        r.risk_domain = domain;
        r.session_key = "S1";
        r.fix_payload = "35=D|11=" + r.cl_ord_id;
        if (svc.submit_new(r, token).kind == CommitKind::Committed) ++admitted;
    }
    const double admit_secs = seconds_since(t0);

    // --- Inbound reports ---------------------------------------------------
    t0 = std::chrono::steady_clock::now();
    int applied = 0;
    for (int i = 0; i < orders; ++i) {
        for (int f = 0; f < fills_per_order; ++f) {
            ExecutionReport e;
            e.tenant = tenant;
            e.session_key = "S1";
            e.session_epoch = 1;
            e.receipt_id = static_cast<std::uint64_t>(i) * 1000 + static_cast<std::uint64_t>(f);
            e.lp_namespace = "LP1";
            e.exec_id = "E" + std::to_string(i) + "-" + std::to_string(f);
            e.exec_type = "TRADE";
            e.chain = "O" + std::to_string(i);
            e.account = "A1";
            e.risk_domain = domain;
            e.last_qty = qty / (fills_per_order > 0 ? fills_per_order : 1);
            e.last_px = 10000;
            e.originating_command = "K" + std::to_string(i);
            e.raw = "35=8|17=" + e.exec_id;
            if (svc.apply_report(e, token).kind == CommitKind::Committed) ++applied;
        }
    }
    const double apply_secs = seconds_since(t0);

    // --- Replay ------------------------------------------------------------
    t0 = std::chrono::steady_clock::now();
    std::size_t replayed = 0;
    bool all_ok = true;
    for (const StreamId& s : store->list_streams(tenant)) {
        const ReplayResult r = replay_from_empty(*store, tenant, s);
        replayed += r.events_applied;
        if (!r.ok) all_ok = false;
    }
    const double replay_secs = seconds_since(t0);

    // --- Replay from a verified snapshot ------------------------------------
    SnapshotStore snaps(*store);
    for (const StreamId& s : store->list_streams(tenant)) {
        Version at = 0;
        std::string w;
        snaps.checkpoint(tenant, s, &at, &w);
    }
    t0 = std::chrono::steady_clock::now();
    std::size_t tail_events = 0;
    for (const StreamId& s : store->list_streams(tenant)) {
        SnapshotRecord snap;
        if (!snaps.newest_usable(tenant, s, &snap)) continue;
        tail_events += replay_stream(*store, tenant, s, &snap).events_applied;
    }
    const double snap_secs = seconds_since(t0);

    auto rate = [](double n, double secs) { return secs > 0 ? n / secs : 0.0; };

    std::printf("journal_bench, measured on THIS host only. Section 19.1: a number from a\n");
    std::printf("machine that is not the deployment target is not an SLO.\n\n");
    std::printf("backend            SQLite, journal_mode=WAL, synchronous=FULL\n");
    std::printf("database           %s\n\n", db.c_str());
    std::printf("admission          %d orders in %.3fs  (%.0f commits/s)\n", admitted,
                admit_secs, rate(admitted, admit_secs));
    std::printf("inbound reports    %d applied in %.3fs  (%.0f commits/s)\n", applied,
                apply_secs, rate(applied, apply_secs));
    std::printf("full replay        %zu events in %.3fs  (%.0f events/s) %s\n", replayed,
                replay_secs, rate(static_cast<double>(replayed), replay_secs),
                all_ok ? "verified" : "FAILED");
    std::printf("replay from snaps  %zu tail events in %.3fs\n", tail_events, snap_secs);
    std::printf("\nEvery commit above includes a durable flush. The admission rate is\n");
    std::printf("therefore a storage measurement, not a CPU-path measurement, and the two\n");
    std::printf("must not be quoted interchangeably.\n");
    return all_ok ? 0 : 1;
}
