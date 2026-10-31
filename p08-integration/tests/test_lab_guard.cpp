// The anti-live-trading guard.
//
// A guard like this fails silently when it is wrong: one that accepts
// everything looks exactly like one that works, and the way you find out is
// that a test order reached a venue. So the interesting case here is not "the
// lab default passes" -- it is the FQDN case, which asserts that P04's own
// Config::validate() accepts a configuration pointed at a real broker while
// this guard refuses it. That gap is the entire reason the guard exists, and if
// P04 ever closes it this file should start failing and then be deleted.
#include <cstdio>
#include <string>

#include "gw/lab_guard.hpp"
#include "router/config.hpp"

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

bool blocked_on(const gw::GateReport& r, std::string_view check_id) {
    if (r.status != gw::GateStatus::Blocked) return false;
    for (const gw::Finding& f : r.findings) {
        if (f.check == check_id) return true;
    }
    return false;
}

std::string findings_of(const gw::GateReport& r) {
    std::string out;
    for (const gw::Finding& f : r.findings) {
        out += std::string(f.check) + "; ";
    }
    return out.empty() ? "(none)" : out;
}

// ---------------------------------------------------------------------------

void only_unroutable_hosts_count_as_lab_hosts() {
    std::printf("only_unroutable_hosts_count_as_lab_hosts\n");

    // Never leaves the machine.
    CHECK(gw::is_lab_host("127.0.0.1"));
    CHECK(gw::is_lab_host("127.0.0.2"));  // the whole /8, not just the one address
    CHECK(gw::is_lab_host("localhost"));
    CHECK(gw::is_lab_host("::1"));
    CHECK(gw::is_lab_host("[::1]"));

    // A bare compose service name resolves only inside the lab network. P04's
    // own default allowlist contains one of these.
    CHECK(gw::is_lab_host("lp-sim"));
    CHECK(gw::is_lab_host("gateway"));

    // Anything that could route. The FQDN is the case that matters; the private
    // addresses are refused because a broker behind a VPN is indistinguishable
    // from them, and section 19 asks for isolated rather than merely private.
    CHECK(!gw::is_lab_host("fix.a-real-broker.com"));
    CHECK(!gw::is_lab_host("10.0.0.5"));
    CHECK(!gw::is_lab_host("192.168.1.10"));
    CHECK(!gw::is_lab_host("172.16.0.1"));
    CHECK(!gw::is_lab_host("2001:db8::1"));
    CHECK(!gw::is_lab_host(""));
}

void the_lab_default_is_isolated() {
    std::printf("the_lab_default_is_isolated\n");

    const router::Config cfg = router::Config::lab_default();
    const gw::GateReport r = gw::verify_isolation(cfg, "SIM_ONLY");
    CHECK_MSG(r.status == gw::GateStatus::Ready, findings_of(r));
    CHECK(r.findings.empty());
}

void a_lab_has_to_be_opted_into() {
    std::printf("a_lab_has_to_be_opted_into\n");

    const router::Config cfg = router::Config::lab_default();

    // Unset is a refusal, not a default. A default would be the one value
    // nobody ever reviews.
    CHECK(blocked_on(gw::verify_isolation(cfg, ""), "isolation.trading_mode"));
    CHECK(blocked_on(gw::verify_isolation(cfg, "LIVE"), "isolation.trading_mode"));
    // Not case-insensitive and not a prefix match.
    CHECK(blocked_on(gw::verify_isolation(cfg, "sim_only"), "isolation.trading_mode"));
    CHECK(blocked_on(gw::verify_isolation(cfg, "SIM_ONLY_BUT"), "isolation.trading_mode"));

    // And the environment reader does not invent one.
    const std::string from_env = gw::trading_mode_from_env();
    CHECK(from_env.empty() || from_env == "SIM_ONLY");
}

void an_allowlisted_broker_passes_p04_and_fails_here() {
    std::printf("an_allowlisted_broker_passes_p04_and_fails_here\n");

    router::Config cfg = router::Config::lab_default();
    // The mistake this guard exists for: point a route at a real venue and add
    // that venue to the allowlist. Now the allowlist agrees with itself.
    cfg.endpoints.allowed_hosts.push_back("fix.a-real-broker.com");
    for (router::Route& r : cfg.routes) {
        if (r.account.str() == "ACC1") r.endpoint = "fix.a-real-broker.com:9443";
    }
    ++cfg.route_config_version;
    cfg.finalize();

    // P04 is satisfied. Its check is "is the host allowlisted", and it is --
    // which is the limit of what a component can verify about its own
    // configuration. require_tls is off in the lab default, so OR-Q-16's
    // plaintext-downgrade refusal does not fire either.
    CHECK_MSG(cfg.validate().is_ok(),
              "P04 now rejects this; if that is deliberate, this guard's "
              "allowlist check is redundant and should go");

    // The lab is not satisfied.
    const gw::GateReport r = gw::verify_isolation(cfg, "SIM_ONLY");
    CHECK(blocked_on(r, "isolation.allowlist_host"));
    CHECK(blocked_on(r, "isolation.route_host"));
}

void a_route_outside_the_allowlist_is_blocked() {
    std::printf("a_route_outside_the_allowlist_is_blocked\n");

    router::Config cfg = router::Config::lab_default();
    for (router::Route& r : cfg.routes) {
        if (r.account.str() == "ACC1") r.endpoint = "fix.a-real-broker.com:9443";
    }
    ++cfg.route_config_version;
    cfg.finalize();

    // Here P04 does refuse, and so does this. The overlap is deliberate:
    // validate() is the caller's to invoke, and a lab that forgot the call
    // should not reach the wire on the strength of a passing contract gate.
    CHECK(cfg.validate().is_err());
    CHECK(blocked_on(gw::verify_isolation(cfg, "SIM_ONLY"), "isolation.route_not_allowlisted"));
}

void an_empty_allowlist_proves_nothing() {
    std::printf("an_empty_allowlist_proves_nothing\n");

    router::Config cfg = router::Config::lab_default();
    cfg.endpoints.allowed_hosts.clear();
    cfg.finalize();

    // An empty list is not "nothing is allowed" in any useful sense: it means
    // no route can be shown to stay inside the lab, which is a refusal rather
    // than a vacuous pass.
    CHECK(blocked_on(gw::verify_isolation(cfg, "SIM_ONLY"), "isolation.allowlist_empty"));
}

void an_inactive_route_is_not_inspected() {
    std::printf("an_inactive_route_is_not_inspected\n");

    router::Config cfg = router::Config::lab_default();
    cfg.endpoints.allowed_hosts.push_back("fix.a-real-broker.com");
    for (router::Route& r : cfg.routes) {
        if (r.account.str() == "ACC1") {
            r.endpoint = "fix.a-real-broker.com:9443";
            r.active = false;
        }
    }
    cfg.finalize();

    const gw::GateReport r = gw::verify_isolation(cfg, "SIM_ONLY");
    // The route is dormant, so it raises nothing by itself -- but the allowlist
    // entry is still there and is still refused. Deactivating a route does not
    // make a venue an acceptable thing to have on the list, because re-enabling
    // it is one edit away.
    CHECK(!blocked_on(r, "isolation.route_host"));
    CHECK(blocked_on(r, "isolation.allowlist_host"));
}

}  // namespace

int main() {
    std::printf("P08 lab guard: section 19 endpoint isolation\n\n");

    only_unroutable_hosts_count_as_lab_hosts();
    the_lab_default_is_isolated();
    a_lab_has_to_be_opted_into();
    an_allowlisted_broker_passes_p04_and_fails_here();
    a_route_outside_the_allowlist_is_blocked();
    an_empty_allowlist_proves_nothing();
    an_inactive_route_is_not_inspected();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
