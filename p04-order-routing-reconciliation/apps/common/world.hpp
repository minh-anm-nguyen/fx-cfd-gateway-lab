// The in-process world the two command line tools drive.
//
// There is no socket here. Design doc 1.3 pins the lab to a P02 fixture or an
// explicitly configured interop peer, and the endpoint allowlist in 3.1 blocks
// anything else, so the CLIs wire P01, P02, P03, P05 and P06 together in one
// process exactly as the test lab does. That makes them useful for walking a
// scenario by hand and for the operator drills release gate 23.2 point 7 asks
// for, without pretending to be a deployable service.
#pragma once

#include <memory>
#include <string>

#include "deps/p01_session.hpp"
#include "deps/p02_lp.hpp"
#include "deps/p05_risk.hpp"
#include "router/invariants.hpp"
#include "router/router.hpp"

namespace app {

using namespace router;

class World {
public:
    World() {
        cfg = Config::lab_default();
        session_image.identity.sender_comp_id = "ROUTER04";
        session_image.identity.target_comp_id = "LPSIM01";
        session_image.identity.qualifier = "PRIMARY";
        session_image.state = p01::SessionState::Active;
        risk = std::make_unique<p05::FakeRisk>(clock, cfg.limits.risk_verdict_ttl_micros);
        store = std::make_unique<Store>(store_image, fp, clock);
        session = std::make_unique<p01::SessionEngine>(session_image, fp);
        lp = std::make_unique<p02::LpSimulator>(lp_image, *session);
        rt = std::make_unique<Router>(cfg, *store, *session, *risk, metrics, fp, clock);
        booted = rt->boot().is_ok();
    }

    // Drives dispatch and inbound until nothing more happens.
    void settle(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            const DispatchStats d = rt->dispatch_pending();
            const InboundStats in = rt->pump_inbound();
            if (d.sent + d.unknown + d.held + in.applied + in.deduplicated + in.quarantined +
                    in.held + in.orphaned + in.invalid ==
                0)
                break;
        }
    }

    static Principal principal(Role role, const std::string& actor) {
        Principal p;
        p.actor = ActorId(actor);
        p.tenant = TenantId("T1");
        p.role = role;
        return p;
    }

    Config cfg;
    VirtualClock clock;
    FailpointRegistry fp;
    Metrics metrics;
    StoreImage store_image;
    p01::SessionImage session_image;
    p02::LpImage lp_image;
    std::unique_ptr<p05::FakeRisk> risk;
    std::unique_ptr<Store> store;
    std::unique_ptr<p01::SessionEngine> session;
    std::unique_ptr<p02::LpSimulator> lp;
    std::unique_ptr<Router> rt;
    bool booted = false;
};

}  // namespace app
