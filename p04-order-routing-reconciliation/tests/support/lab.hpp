// The deterministic test lab from design doc 22.1.
//
// It owns the three things that survive a crash -- the P06 image, the P01
// session image and the P02 book -- separately from the process-side objects
// that do not. `crash()` throws the process away; `boot()` builds a new one
// against the same images. That is the only honest way to test the twenty crash
// windows in 15.1: if the router kept its own state in the same object as the
// store, a "restart" would prove nothing.
//
// Everything is single threaded and driven by an explicit virtual clock. There
// is no sleep anywhere in the suite.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "deps/p01_session.hpp"
#include "deps/p02_lp.hpp"
#include "deps/p05_risk.hpp"
#include "router/invariants.hpp"
#include "router/router.hpp"
#include "tiny_test.hpp"

namespace lab {

using namespace router;

// The fixture from design doc 21.0: ACC1 on EUR/USD, tick 0.00001, qty step
// 1000, LP SIM01, session ACTIVE, risk APPROVE, 35=H off.
class Lab {
public:
    Lab() {
        cfg = Config::lab_default();
        session_disk.identity.sender_comp_id = "ROUTER04";
        session_disk.identity.target_comp_id = "LPSIM01";
        session_disk.identity.qualifier = "PRIMARY";
        session_disk.state = p01::SessionState::Active;
        risk = std::make_unique<p05::FakeRisk>(clock, cfg.limits.risk_verdict_ttl_micros);
        boot();
    }

    // Builds the process-side objects against the current images and runs the
    // recovery sequence. Safe to call repeatedly.
    void boot() {
        rt.reset();
        lp.reset();
        session.reset();
        store.reset();
        store = std::make_unique<Store>(disk, fp, clock);
        session = std::make_unique<p01::SessionEngine>(session_disk, fp);
        lp = std::make_unique<p02::LpSimulator>(lp_disk, *session);
        rt = std::make_unique<Router>(cfg, *store, *session, *risk, metrics, fp, clock);
        last_boot_ok = rt->boot().is_ok();
    }

    // Drops everything process-side without touching the images.
    void crash() {
        rt.reset();
        lp.reset();
        session.reset();
        store.reset();
    }

    // Runs `f` and reports whether it died at a failpoint. The images keep
    // whatever was committed before the crash, exactly like a real kill.
    template <typename F>
    bool crashes(F&& f) {
        try {
            f();
            return false;
        } catch (const CrashMarker&) {
            crash();
            return true;
        }
    }

    // Kill and restart in one step, the common shape of the O-series tests.
    template <typename F>
    bool crash_during(F&& f) {
        const bool died = crashes(std::forward<F>(f));
        boot();
        return died;
    }

    // Turns on the 35=H capability on both sides of the boundary. Nothing in
    // the lab enables it by default, because design doc 3.1 keeps it off until
    // the peer has agreed and the interop tests pass.
    void enable_status_queries(bool echo_query_id = true) {
        cfg.capabilities.order_status_request = true;
        cfg.capabilities.echo_ord_status_req_id = echo_query_id;
        cfg.finalize();
        lp_disk.capability_status_request = true;
    }

    // --- principals --------------------------------------------------------
    static Principal trader(const std::string& tenant = "T1",
                            const std::string& actor = "alice") {
        Principal p;
        p.actor = ActorId(actor);
        p.tenant = TenantId(tenant);
        p.role = Role::Trader;
        return p;
    }
    static Principal operator_principal(const std::string& actor = "ops1") {
        Principal p;
        p.actor = ActorId(actor);
        p.tenant = TenantId("T1");
        p.role = Role::Operator;
        return p;
    }
    static Principal admin(const std::string& actor = "admin1") {
        Principal p;
        p.actor = ActorId(actor);
        p.tenant = TenantId("T1");
        p.role = Role::Admin;
        return p;
    }
    static Principal viewer(const std::string& actor = "guest") {
        Principal p;
        p.actor = ActorId(actor);
        p.tenant = TenantId("T1");
        p.role = Role::Viewer;
        return p;
    }

    // --- command builders --------------------------------------------------
    CommandEnvelope new_order(const std::string& command_id, const std::string& qty = "100000",
                              const std::string& price = "1.08500") const {
        CommandEnvelope c;
        c.principal = trader();
        c.account = AccountId("ACC1");
        c.kind = CommandKind::New;
        c.client_command_id = command_id;
        c.symbol = "EUR/USD";
        c.side = Side::Buy;
        c.qty_text = qty;
        c.price_text = price;
        c.ord_type = OrdType::Limit;
        c.tif = TimeInForce::Day;
        return c;
    }

    CommandEnvelope cancel_order(const std::string& command_id, const OrderChainId& chain) const {
        CommandEnvelope c;
        c.principal = trader();
        c.account = AccountId("ACC1");
        c.kind = CommandKind::Cancel;
        c.client_command_id = command_id;
        c.chain_id = chain;
        return c;
    }

    CommandEnvelope replace_order(const std::string& command_id, const OrderChainId& chain,
                                  const std::string& qty, const std::string& price) const {
        CommandEnvelope c;
        c.principal = trader();
        c.account = AccountId("ACC1");
        c.kind = CommandKind::Replace;
        c.client_command_id = command_id;
        c.chain_id = chain;
        c.symbol = "EUR/USD";
        c.side = Side::Buy;
        c.qty_text = qty;
        c.price_text = price;
        c.ord_type = OrdType::Limit;
        c.tif = TimeInForce::Day;
        return c;
    }

    Result<AcceptedIntent, RoutingError> submit(const CommandEnvelope& c) { return rt->submit(c); }

    // Submits a New and returns the accepted intent, failing the test if it was
    // refused. Used by the many scenarios whose setup is "a working order".
    AcceptedIntent must_submit_new(const std::string& command_id,
                                   const std::string& qty = "100000",
                                   const std::string& price = "1.08500") {
        auto r = rt->submit(new_order(command_id, qty, price));
        if (r.is_err())
            throw tt::AssertionFailure(std::string("submit_new refused: ") +
                                       std::string(to_string(r.error())));
        return r.value();
    }

    AcceptedIntent must_submit_cancel(const std::string& command_id, const OrderChainId& chain) {
        auto r = rt->submit(cancel_order(command_id, chain));
        if (r.is_err())
            throw tt::AssertionFailure(std::string("submit_cancel refused: ") +
                                       std::string(to_string(r.error())));
        return r.value();
    }

    AcceptedIntent must_submit_replace(const std::string& command_id, const OrderChainId& chain,
                                       const std::string& qty, const std::string& price) {
        auto r = rt->submit(replace_order(command_id, chain, qty, price));
        if (r.is_err())
            throw tt::AssertionFailure(std::string("submit_replace refused: ") +
                                       std::string(to_string(r.error())));
        return r.value();
    }

    // --- driving -----------------------------------------------------------
    // Dispatches and pumps until nothing more happens. Bounded, so a scenario
    // cannot spin.
    void drive(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            const DispatchStats d = rt->dispatch_pending();
            const InboundStats in = rt->pump_inbound();
            const std::size_t work = d.sent + d.unknown + d.held + in.applied + in.deduplicated +
                                     in.quarantined + in.held + in.orphaned + in.invalid;
            if (work == 0) break;
        }
    }

    void advance(std::uint64_t micros) { clock.advance(micros); }
    void tick(std::uint64_t micros = 2000000) {
        clock.advance(micros);
        rt->on_timer();
        drive();
    }

    // Hands a report straight to the session inbox and pumps it through.
    void inject(const p02::ReportSpec& spec) {
        lp->send(spec);
        rt->pump_inbound();
    }

    // --- read models -------------------------------------------------------
    const p03::ChainState* chain(const OrderChainId& id) const { return store->find_chain(id); }
    const p03::ChainState* only_chain() const {
        if (disk.chains.size() != 1) return nullptr;
        return &disk.chains.begin()->second;
    }
    const CommandIntent* intent(const std::string& command_id,
                                const std::string& tenant = "T1") const {
        return store->find_intent(TenantId(tenant), CommandId(command_id));
    }

    std::vector<Violation> violations() const {
        InvariantInputs in{disk, session_disk, cfg, 0, 0, 0, 0, false};
        return check_invariants(in);
    }

    // Same oracle, skipping the journal and audit prefixes a previous call
    // already verified. Used by the random walk, which checks after every one
    // of thousands of steps.
    std::vector<Violation> violations_incremental(std::size_t journal_from,
                                                  std::size_t audit_from,
                                                  std::size_t quarantine_from,
                                                  bool skip_command_tables) const {
        InvariantInputs in{disk,       session_disk,    cfg, 0, journal_from,
                           audit_from, quarantine_from, skip_command_tables};
        return check_invariants(in);
    }

    // How many application frames of a given MsgType reached the wire. OR-N-01
    // and friends assert that this is exactly zero for 'H'.
    std::size_t wire_count(char msg_type) const {
        std::size_t n = 0;
        const std::string needle = std::string("35=") + msg_type + "\x01";
        for (const std::string& f : session_disk.wire_log)
            if (f.rfind(needle, 0) == 0) ++n;
        return n;
    }

    std::size_t quarantine_depth() const { return store->quarantine_depth(); }
    std::size_t recon_cases() const { return disk.recon_cases.size(); }

    const ReconCase* first_case() const {
        if (disk.recon_cases.empty()) return nullptr;
        return &disk.recon_cases.begin()->second;
    }
    const ReconCase* case_for(const OrderChainId& chain_id) const {
        for (const auto& [k, c] : disk.recon_cases) {
            (void)k;
            if (c.chain_id == chain_id) return &c;
        }
        return nullptr;
    }

    std::size_t audit_count(AuditKind k) const { return disk.audit.count(k); }

    // Images: the disks.
    Config cfg;
    VirtualClock clock;
    FailpointRegistry fp;
    Metrics metrics;
    StoreImage disk;
    p01::SessionImage session_disk;
    p02::LpImage lp_disk;

    // Process side.
    std::unique_ptr<p05::FakeRisk> risk;
    std::unique_ptr<Store> store;
    std::unique_ptr<p01::SessionEngine> session;
    std::unique_ptr<p02::LpSimulator> lp;
    std::unique_ptr<Router> rt;
    bool last_boot_ok = false;
};

}  // namespace lab

// Asserts every safety invariant. Used after each step, per design doc 19.4.
#define CHECK_INV(lab_)                                                      \
    do {                                                                     \
        const auto tt_viol = (lab_).violations();                            \
        if (!tt_viol.empty())                                                \
            TT_FAIL("invariant violation: " + ::router::render_violations(tt_viol)); \
    } while (0)
