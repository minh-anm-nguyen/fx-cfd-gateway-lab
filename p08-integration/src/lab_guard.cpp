#include "gw/lab_guard.hpp"

#include <algorithm>
#include <cstdlib>

namespace gw {
namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string host_of(const std::string& endpoint) {
    // rfind, matching how P04 splits the same string: an IPv6 literal carries
    // colons of its own and only the last one separates the port.
    const std::size_t colon = endpoint.rfind(':');
    return colon == std::string::npos ? endpoint : endpoint.substr(0, colon);
}

}  // namespace

bool is_lab_host(std::string_view host) {
    if (host.empty()) return false;
    if (host == "localhost" || host == "::1" || host == "[::1]") return true;
    // The whole 127/8 block rather than 127.0.0.1 alone: a lab that binds
    // 127.0.0.2 to keep two sessions apart is still a lab.
    if (starts_with(host, "127.")) return true;

    // A bare service name. A dot could begin a public FQDN and a colon could be
    // an IPv6 literal pointing anywhere, so neither gets through.
    return host.find('.') == std::string_view::npos && host.find(':') == std::string_view::npos;
}

std::string trading_mode_from_env() {
    const char* raw = std::getenv(std::string(kTradingModeVar).c_str());
    return raw == nullptr ? std::string{} : std::string(raw);
}

GateReport verify_isolation(const router::Config& cfg, std::string_view trading_mode) {
    GateReport r;

    if (trading_mode.empty()) {
        r.findings.push_back(Finding{"isolation.trading_mode",
                                     std::string(kTradingModeVar) + " is unset; it must be " +
                                         std::string(kRequiredTradingMode)});
    } else if (trading_mode != kRequiredTradingMode) {
        r.findings.push_back(Finding{"isolation.trading_mode",
                                     std::string(kTradingModeVar) + " is \"" +
                                         std::string(trading_mode) + "\", not \"" +
                                         std::string(kRequiredTradingMode) + "\""});
    }

    // The allowlist itself. This is the check P04 cannot make about its own
    // configuration, because from inside it the list is the authority.
    if (cfg.endpoints.allowed_hosts.empty()) {
        r.findings.push_back(Finding{"isolation.allowlist_empty",
                                     "the endpoint allowlist is empty, so no route can be "
                                     "proven to stay inside the lab"});
    }
    for (const std::string& host : cfg.endpoints.allowed_hosts) {
        if (is_lab_host(host)) continue;
        r.findings.push_back(Finding{"isolation.allowlist_host",
                                     "allowlisted host \"" + host +
                                         "\" can route outside the lab; only loopback and bare "
                                         "compose service names may be allowlisted"});
    }

    // Belt and braces on the routes. P04 refuses these in Config::validate(),
    // but that call is the caller's to make, and a lab that forgot it should not
    // reach the wire on the strength of a passing contract gate.
    for (const router::Route& route : cfg.routes) {
        if (!route.active) continue;
        const std::string host = host_of(route.endpoint);
        const auto& allowed = cfg.endpoints.allowed_hosts;
        if (std::find(allowed.begin(), allowed.end(), host) == allowed.end()) {
            r.findings.push_back(Finding{"isolation.route_not_allowlisted",
                                         "route " + route.account.str() + " -> " + route.lp.str() +
                                             " targets " + route.endpoint +
                                             ", whose host is not allowlisted"});
        } else if (!is_lab_host(host)) {
            r.findings.push_back(Finding{"isolation.route_host",
                                         "route " + route.account.str() + " -> " + route.lp.str() +
                                             " targets " + route.endpoint +
                                             ", which can route outside the lab"});
        }
    }

    r.status = r.findings.empty() ? GateStatus::Ready : GateStatus::Blocked;
    return r;
}

}  // namespace gw
