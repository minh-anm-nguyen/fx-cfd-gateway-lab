// The gateway on a real socket.
//
// `apps/gateway` runs the assembled components in one process with P04's
// in-tree LP standing in for the counterparty. This is the same gateway with
// that stand-in removed: P01's real session over `fixse::TcpConnector`, talking
// to P02's `lp_simulator` as a separate process over TCP.
//
// §6 is why it matters. P02 is "a peer that may be faulty or hostile", not a
// library in the same address space, and until the frames cross a socket none of
// the network crash windows -- a cut connection, a partial write, netem delay --
// can be injected at all.
//
// Every component here is the real one. P04 routes, P05 reserves, P06 holds the
// issue-once binding and the durable receipts, P03 owns status and the fill
// ledger, P01 owns the session and the sequence numbers, P02 is another
// process. P07's pipeline is deliberately left out: one thread keeps the run
// readable, and tests/test_execution_pipeline.cpp covers the hand-off.
//
// Usage:
//   interop_gateway --session-config <p01.toml> --state-dir <dir> [--orders N]
//
// The LP has to be listening first, on the port that config names.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "deps/p01_session.hpp"
#include "fix/audit.hpp"
#include "fix/config.hpp"
#include "fix/metrics.hpp"
#include "fix/session_actor.hpp"
#include "fix/sqlite_store.hpp"
#include "fix/tcp_connector.hpp"
#include "gw/contract.hpp"
#include "gw/fix_transport.hpp"
#include "gw/lab_guard.hpp"
#include "gw/order_reducer.hpp"
#include "gw/risk_adapter.hpp"
#include "gw/session_adapter.hpp"
#include "jrnl/stable_send.hpp"
#include "risk/policy.hpp"
#include "risk/scenario.hpp"
#include "router/router.hpp"

namespace {

constexpr const char* kAccount = "ACC1";
// What the client asks for, and what actually goes on the wire. P02's config
// names its instrument EURUSD, as do P03's policy and P05's; P04's lab default
// quotes EUR/USD. The LP is the one holding the wire contract, so EURUSD is the
// wire symbol here and P04's alias table is what keeps the client's spelling.
constexpr const char* kClientSymbol = "EUR/USD";
constexpr const char* kWireSymbol = "EURUSD";
constexpr const char* kInstrument = "EURUSD";

// A clock that moves.
//
// P04 ships only a VirtualClock, pinned to the lab epoch of 2026-10-30T08:00Z,
// because its own suite needs determinism and a fixed clock is the cheapest way
// to get it. A real peer does not accept that: P04 stamps TransactTime from this
// clock, and P02 rejected the order with 103=8, "TransactTime too old", comparing
// it against its own wall clock. So the interop run needs a real one, and
// building it here rather than in P04 keeps P04's determinism intact.
class RealRouterClock final : public router::IClock {
public:
    router::MonoMicros mono_now() const override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<router::MonoMicros>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }
    router::WallMicros wall_now() const override {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<router::WallMicros>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }
};

void usage() {
    std::printf("usage: interop_gateway --session-config <file> --state-dir <dir> "
                "[--orders <n>] [--seconds <n>]\n");
}

int report_gate(const char* what, const gw::GateReport& r) {
    for (const gw::Finding& f : r.findings) {
        std::printf("  %-32.*s %s\n", static_cast<int>(f.check.size()), f.check.data(),
                    f.detail.c_str());
    }
    const std::string_view verdict = gw::to_string(r.status);
    std::printf("%s: %.*s\n", what, static_cast<int>(verdict.size()), verdict.data());
    return r.status == gw::GateStatus::Ready ? 0 : 2;
}

router::CommandEnvelope new_order(const std::string& id) {
    router::CommandEnvelope cmd;
    cmd.principal.actor = router::ActorId("trader-1");
    cmd.principal.tenant = router::TenantId("T1");
    cmd.principal.role = router::Role::Trader;
    cmd.account = router::AccountId(kAccount);
    cmd.kind = router::CommandKind::New;
    cmd.client_command_id = id;
    cmd.symbol = kClientSymbol;
    cmd.side = router::Side::Buy;
    cmd.qty_text = "10000";
    cmd.price_text = "1.10000";
    return cmd;
}

}  // namespace

int main(int argc, char** argv) {
    std::string session_config;
    std::string state_dir;
    int orders = 1;
    int seconds = 10;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "--session-config" && has_next) session_config = argv[++i];
        else if (a == "--state-dir" && has_next) state_dir = argv[++i];
        else if (a == "--orders" && has_next) orders = std::atoi(argv[++i]);
        else if (a == "--seconds" && has_next) seconds = std::atoi(argv[++i]);
        else {
            usage();
            return 1;
        }
    }
    if (session_config.empty() || state_dir.empty()) {
        usage();
        return 1;
    }

    std::printf("P08 interop gateway -- P01 on a real socket to P02\n\n");

    router::Config cfg = router::Config::lab_default();
    if (const int rc = report_gate("contracts", gw::verify_contracts()); rc != 0) return rc;
    if (const int rc =
            report_gate("isolation", gw::verify_isolation(cfg, gw::trading_mode_from_env()));
        rc != 0) {
        return rc;
    }
    std::printf("\n");

    std::error_code ec;
    std::filesystem::create_directories(state_dir, ec);

    // --- P01, real, on a real socket ---------------------------------------
    fixse::SessionConfig scfg;
    std::vector<fixse::ConfigError> errors;
    if (!fixse::load_config_file(session_config, &scfg, &errors)) {
        for (const fixse::ConfigError& e : errors) {
            std::fprintf(stderr, "session config %s: %s\n", e.field.c_str(), e.reason.c_str());
        }
        return 2;
    }

    fixse::SystemClock fix_clock;
    fixse::SqliteSessionStore::Options so;
    so.path = scfg.database_path;
    so.key.begin_string = scfg.begin_string;
    so.key.sender_comp_id = scfg.sender_comp_id;
    so.key.target_comp_id = scfg.target_comp_id;
    so.key.qualifier = scfg.qualifier;
    so.busy_timeout_ms = scfg.store_busy_timeout_ms;
    so.journal_mode = scfg.sqlite_journal_mode;
    so.synchronous = scfg.sqlite_synchronous;
    so.create_if_missing = true;
    so.clock = &fix_clock;

    std::unique_ptr<fixse::SqliteSessionStore> fix_store;
    std::string why;
    if (fixse::SqliteSessionStore::open(so, &fix_store, &why) != fixse::StoreError::None) {
        std::fprintf(stderr, "P01 store: %s\n", why.c_str());
        return 3;
    }

    // --- P05, real ----------------------------------------------------------
    risk::LabOptions ro;
    ro.journal_path = (std::filesystem::path(state_dir) / "risk.journal").string();
    risk::AccountProfile acc = ro.policy.accounts.at("ACC-A");
    acc.account_id = kAccount;
    ro.policy.accounts[kAccount] = acc;
    auto risk_lab = risk::LabEnv::create(ro);
    if (!risk_lab.ok()) {
        std::fprintf(stderr, "P05 lab: %s\n", risk_lab.detail().c_str());
        return 4;
    }
    risk::LabEnv& risk_env = *risk_lab.value();

    // --- P04's machinery, and P03 installed into it -------------------------
    // The route must name the session the gateway is actually attached to.
    // P04 refuses otherwise, and it is right to (OR-B-06): sending one peer's
    // order to another is the mistake that check exists for. P01's config is the
    // authority here, because its values are the ones that go on the wire, so
    // the route is reconciled to it rather than the other way round.
    // The wire symbol has to be the one the LP knows. P04 already models this
    // as an alias: the client keeps saying EUR/USD and the profile is keyed by
    // what goes in tag 55. Without it the LP records the request and creates no
    // order -- which is exactly what it did before this was set.
    router::SymbolProfile wire_profile = cfg.symbols.at(kClientSymbol);
    wire_profile.wire_symbol = kWireSymbol;
    cfg.symbols[kWireSymbol] = wire_profile;
    cfg.aliases[kClientSymbol] = {kWireSymbol};

    for (router::Route& r : cfg.routes) {
        r.sender_comp_id = scfg.sender_comp_id;
        r.target_comp_id = scfg.target_comp_id;
        r.session_qualifier = scfg.qualifier;
        r.endpoint = scfg.host + ":" + std::to_string(scfg.port);
    }
    ++cfg.route_config_version;
    cfg.finalize();
    if (const auto valid = cfg.validate(); valid.is_err()) {
        const std::string_view e = router::to_string(valid.error());
        std::fprintf(stderr, "P04 config refused after reconciling the route: %.*s\n",
                     static_cast<int>(e.size()), e.data());
        return 8;
    }
    // And the reconciled endpoint still has to be a lab endpoint.
    if (const int rc = report_gate("isolation (reconciled)",
                                   gw::verify_isolation(cfg, gw::trading_mode_from_env()));
        rc != 0) {
        return rc;
    }

    RealRouterClock clock;
    router::FailpointRegistry fp;
    router::Metrics metrics;
    router::StoreImage store_image;

    gw::OrderCoreReducer::Options reducer_opts;
    reducer_opts.wire_to_instrument = {{kWireSymbol, kInstrument}};
    gw::OrderCoreReducer reducer(reducer_opts);
    router::Store store(store_image, fp, clock, &reducer);

    // --- P06, real, and the wire that now goes to P01 -----------------------
    //
    // The session engine does not exist yet, so the callback resolves its target
    // at call time. The ordering P06 insists on is unchanged: the sequence is
    // durable before these bytes are offered to anyone.
    auto engine_slot = std::make_shared<fixse::SessionEngine*>(nullptr);
    jrnl::StableSendStore::Options sso;
    sso.path = (std::filesystem::path(state_dir) / "stable_send.db").string();
    sso.wire = [engine_slot](const jrnl::SessionKey&, std::uint64_t,
                             const std::string& payload) {
        if (*engine_slot == nullptr) return false;
        return gw::send_to_session(**engine_slot, payload);
    };
    std::unique_ptr<jrnl::StableSendStore> send_store;
    if (jrnl::StableSendStore::open(sso, &send_store, &why) != jrnl::StoreError::None) {
        std::fprintf(stderr, "P06 stable send store: %s\n", why.c_str());
        return 5;
    }

    // --- the adapters -------------------------------------------------------
    router::p01::SessionImage session_image;
    session_image.identity = {scfg.sender_comp_id, scfg.target_comp_id, scfg.qualifier};
    // Not up yet; the connector decides when it is.
    session_image.state = router::p01::SessionState::Connecting;
    gw::SessionAdapter session(session_image, fp, *send_store,
                               gw::SessionAdapter::Options{scfg.session_id()});

    gw::RiskAdmissionAdapter::Options ao;
    ao.principal = risk_env.trader();
    ao.wire_to_instrument = {{kWireSymbol, kInstrument}};
    gw::RiskAdmissionAdapter risk(risk_env.coordinator(), risk_env.store(), risk_env.clock(),
                                  clock, ao);

    // --- P01's engine, with the bridge as its sink --------------------------
    gw::FixSink sink(session);
    fixse::RealSocketOps ops;
    fixse::TcpConnector connector(scfg, fix_clock, ops);
    fixse::Metrics fix_metrics;
    fixse::AuditLog fix_audit;

    fixse::EngineDeps deps;
    deps.cfg = &scfg;
    deps.store = fix_store.get();
    deps.clock = &fix_clock;
    deps.transport = &connector;
    deps.controller = &connector;
    deps.sink = &sink;
    deps.metrics = &fix_metrics;
    deps.audit = &fix_audit;
    fixse::SessionEngine engine(deps);
    connector.bind_engine(&engine);
    *engine_slot = &engine;

    if (engine.start() != fixse::StoreError::None) {
        std::fprintf(stderr, "P01 engine refused to start: %s -- %s\n",
                     std::string(fixse::to_string(engine.fault_reason())).c_str(),
                     engine.fault_detail().c_str());
        return 6;
    }
    std::printf("P01  session %s starting, epoch %llu next_in %llu next_out %llu\n",
                scfg.session_id().c_str(), static_cast<unsigned long long>(engine.epoch()),
                static_cast<unsigned long long>(engine.next_in()),
                static_cast<unsigned long long>(engine.next_out()));

    router::Router rt(cfg, store, session, risk, metrics, fp, clock);
    if (rt.boot().is_err()) {
        std::fprintf(stderr, "P04 boot refused\n");
        return 7;
    }

    // --- run ----------------------------------------------------------------
    const auto started = std::chrono::steady_clock::now();
    int submitted = 0;
    bool was_active = false;

    while (true) {
        connector.poll_once(50);

        // The router may only dispatch once the session is up, and P01 is the
        // only thing that knows when that is. Mirroring it into the adapter's
        // image is what makes the router's own readiness check work.
        const bool active = engine.state() == fixse::SessionState::Active;
        session_image.state = active ? router::p01::SessionState::Active
                                     : router::p01::SessionState::Connecting;
        if (active && !was_active) {
            std::printf("P01  session ACTIVE\n");
            was_active = true;
        }

        if (active && submitted < orders) {
            const std::string id = "K-" + std::to_string(submitted + 1);
            const auto accepted = rt.submit(new_order(id));
            if (accepted.is_ok()) {
                std::printf("P04  accepted %s chain=%s cl_ord_id=%s\n", id.c_str(),
                            accepted.value().chain_id.str().c_str(),
                            accepted.value().cl_ord_id.str().c_str());
            } else {
                const std::string_view e = router::to_string(accepted.error());
                std::printf("P04  refused %s: %.*s\n", id.c_str(), static_cast<int>(e.size()),
                            e.data());
            }
            ++submitted;
        }

        const router::DispatchStats d = rt.dispatch_pending();
        const router::InboundStats in = rt.pump_inbound();
        if (d.sent + d.unknown + d.held + in.applied + in.deduplicated + in.quarantined +
                in.held + in.orphaned + in.invalid >
            0) {
            std::printf("     dispatch sent=%zu unknown=%zu held=%zu | inbound applied=%zu dedup=%zu quarantined=%zu held=%zu orphaned=%zu invalid=%zu\n",
                        d.sent, d.unknown, d.held, in.applied, in.deduplicated,
                        in.quarantined, in.held, in.orphaned, in.invalid);
        }

        if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds)) break;
    }

    engine.request_graceful_stop();
    for (int i = 0; i < 40 && connector.poll_once(50); ++i) {
    }

    // --- what each component believes ---------------------------------------
    std::printf("\nP01  final state %s, next_in %llu next_out %llu\n",
                std::string(fixse::to_string(engine.state())).c_str(),
                static_cast<unsigned long long>(engine.next_in()),
                static_cast<unsigned long long>(engine.next_out()));
    std::printf("P01  bridge delivered=%llu unconvertible=%llu\n",
                static_cast<unsigned long long>(sink.delivered()),
                static_cast<unsigned long long>(sink.unconvertible()));
    std::printf("P06  sequences allocated=%llu unconfirmed=%zu inbound unacked=%zu\n",
                static_cast<unsigned long long>(session.allocated_sequences()),
                session.unconfirmed_sends(), session.pending_inbound());
    std::printf("P05  stranded=%zu scale_mismatches=%zu unmapped_symbols=%zu\n",
                risk.stranded_reservations().size(), risk.scale_mismatches(),
                risk.unmapped_symbols());
    for (const std::string& a : rt.alerts()) {
        std::printf("P04  alert: %s\n", a.c_str());
    }
    std::printf("P03  refusals=%zu\n", reducer.refusals());

    const std::string_view mode = router::to_string(rt.mode());
    std::printf("P04  mode=%.*s original_sends=%zu open_unknown=%zu open_recon=%zu\n",
                static_cast<int>(mode.size()), mode.data(), rt.original_sends(),
                rt.open_unknown_outcomes(), rt.open_recon_cases());

    for (int i = 1; i <= submitted; ++i) {
        const router::CommandId id(("K-" + std::to_string(i)));
        if (const auto st = rt.command_status(router::TenantId("T1"), id)) {
            const std::string_view d = router::to_string(st->delivery);
            const std::string_view b = router::to_string(st->business);
            std::printf("P04  %s delivery=%.*s business=%.*s\n", id.str().c_str(),
                        static_cast<int>(d.size()), d.data(), static_cast<int>(b.size()),
                        b.data());
        }
    }

    // At most one original sequence per accepted command. EI-09 and JI-19 in the
    // same number, and the only thing this run asserts rather than reports.
    const bool one_original =
        session.allocated_sequences() <= static_cast<std::uint64_t>(submitted);
    std::printf("\nEI-09/JI-19 at most one original per command: %s\n",
                one_original ? "held" : "VIOLATED");
    return one_original ? 0 : 1;
}
