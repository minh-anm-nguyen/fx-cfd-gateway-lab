// The anti-live-trading guard.
//
// Section 19, first bullet: "FIX/IP endpoints must resolve only within isolated
// lab allowlist; refuse startup when outbound peer host/IP not allowlisted or
// TRADING_MODE != SIM_ONLY; block egress at network/host firewall, not merely
// app flag."
//
// P04 already does the middle clause, and does it properly:
// router::Config::validate() refuses a route whose host is absent from
// EndpointPolicy::allowed_hosts, at startup rather than at send time (OR-B-17).
// There is no point in a second copy of that check here. What it leaves open is
// narrow and worth closing:
//
//   * nothing validates the allowlist itself. Setting
//     `allowed_hosts = {"fix.a-real-broker.com"}` makes the list agree with
//     itself, validate() passes, and the lab is now pointed at a venue. EI-55
//     is about the endpoints being synthetic, not about the config being
//     self-consistent.
//   * P04 has no notion of TRADING_MODE, so nothing requires the operator to
//     have opted into a simulation at all.
//
// Those two are what this adds, and nothing more.
//
// The same sentence in section 19 says the app-flag half is not enough, so it
// is worth being exact about what this buys. It can refuse to start, and a
// process that will not connect cannot send an order to a real venue -- cheap,
// early, testable. It cannot constrain a process that does not ask: a stray
// script, a debugger, or code that opens its own socket are stopped by the
// network and by nothing else. The probe prints that caveat rather than letting
// READY imply more than it proves.
#pragma once

#include <string>
#include <string_view>

#include "gw/contract.hpp"
#include "router/config.hpp"

namespace gw {

inline constexpr std::string_view kRequiredTradingMode = "SIM_ONLY";
inline constexpr std::string_view kTradingModeVar = "TRADING_MODE";

// True when `host` cannot route outside an isolated lab.
//
// Two shapes qualify, and the reasoning is reachability rather than naming
// convention:
//
//   * a loopback literal (127.x.x.x, ::1) or the name "localhost", which by
//     definition never leaves the machine;
//   * a bare name carrying no dot and no colon, which is how Docker Compose
//     publishes a service. Such a name resolves only through the compose
//     network's DNS and so cannot be a public venue. P04's own lab default,
//     "lp-sim", is one of these.
//
// Everything else is refused, private ranges included. 10.x and 192.168.x are
// perfectly routable on a corporate network, and a broker reached over a VPN
// looks exactly like them; section 19 asks for an isolated lab, not a private
// one.
[[nodiscard]] bool is_lab_host(std::string_view host);

// The TRADING_MODE environment variable, or empty when it is unset. Empty is a
// refusal in verify_isolation(), so a simulation has to be opted into rather
// than defaulted to -- a default would be the one value nobody ever reviews.
[[nodiscard]] std::string trading_mode_from_env();

// Blocked on the first thing that would let traffic leave the lab, with one
// finding per problem so an operator fixes them in a single pass rather than
// one restart at a time.
[[nodiscard]] GateReport verify_isolation(const router::Config& cfg,
                                          std::string_view trading_mode);

}  // namespace gw
