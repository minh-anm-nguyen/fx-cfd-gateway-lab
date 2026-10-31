// Rules of Engagement, routes, symbol profiles and the bounded-resource limits.
//
// Design doc 3.1 requires every one of these to be pinned and versioned before a
// single application message may be sent, and 2.2 R-I02 requires an accepted
// command to keep the route it was admitted with even if the configuration moves
// underneath it. That is why a command stores `route_config_version` and
// `profile_version` rather than a pointer to live config.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "router/hash.hpp"
#include "router/ids.hpp"
#include "router/result.hpp"

namespace router {

// How a symbol is quoted and sized. OR-A-10, OR-B-12 and OR-B-14 all turn on
// these four numbers being authoritative rather than guessed.
struct SymbolProfile {
    std::string wire_symbol;       // exactly what goes on the wire, "EUR/USD"
    int price_scale = 5;           // fraction digits, so tick = 10^-scale
    std::int64_t tick_ticks = 1;   // price must be a multiple of this many ticks
    std::int64_t qty_step = 1000;  // quantity must be a multiple of this
    std::int64_t min_qty = 1000;
    std::int64_t max_qty = 100000000;
    std::int64_t max_price_ticks = 100000000;  // sanity bound before serialising
};

// What the peer has actually agreed to do. Nothing here defaults to true: the
// pre-interop checklist in design doc 3.3 has to be walked before a capability
// is turned on, and OR-N-01 proves the router emits zero 35=H while it is off.
struct Capabilities {
    bool order_status_request = false;  // 35=H
    bool pending_cancel_ack = true;     // peer sends 8/150=6 before 8/150=4
    bool pending_replace_ack = true;    // peer sends 8/150=E before 8/150=5
    bool trade_history_query = false;   // a real fill history interface exists
    bool echo_ord_status_req_id = false;  // peer echoes 790
};

// One account to one LP session. Design doc 8.2: exactly one active route, no
// smart order routing, no silent failover.
struct Route {
    AccountId account;
    LpId lp;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string session_qualifier;
    std::string endpoint;  // host:port, checked against the allowlist
    bool active = true;
    int priority = 0;  // two active routes with equal priority is a config error
};

// Which order shapes the profile permits at all (design doc 3.1).
struct OrderProfile {
    std::vector<OrdType> allowed_ord_types{OrdType::Limit};
    std::vector<TimeInForce> allowed_tif{TimeInForce::Day};
    bool allow_side_change_on_replace = false;
    bool allow_symbol_change_on_replace = false;
    bool allow_optimistic_chaining = false;  // strict pessimistic in v1
    // Tags the peer may add that we store but do not act on. Anything outside
    // this list is a must-understand violation (OR-D-19, OR-Q-17).
    std::vector<int> tolerated_custom_tags{};
};

// Everything in design doc 16.1. Defaults are the documented lab examples.
struct Limits {
    std::size_t global_command_queue = 65536;
    std::size_t per_chain_mailbox = 128;
    std::size_t orphan_quota = 10000;
    std::size_t status_queries_in_flight = 100;
    std::size_t admission_rate_per_account_per_sec = 500;
    std::size_t max_payload_bytes = 1024 * 1024;
    std::size_t max_field_bytes = 256;      // bounded parser, OR-D-13
    std::size_t max_fields_per_message = 128;
    // Weighted credits across chains: cancel first, but never at the cost of
    // per-chain ordering (design doc 16.1 fairness caveat).
    int credit_cancel = 4;
    int credit_recon = 2;
    int credit_replace = 1;
    int credit_new = 1;
    // Reconciliation budget from design doc 14.3.
    std::uint64_t ack_timeout_micros = 1000000;       // 1s
    std::uint64_t recon_initial_backoff_micros = 1000000;
    int recon_max_probes = 3;
    std::uint64_t risk_verdict_ttl_micros = 2000000;
    std::uint64_t cancel_transport_ttl_micros = 30000000;  // OR-H-16
    std::size_t log_rate_limit_per_reason = 100;           // OR-Q-19
};

// Lab safety: a production broker endpoint must not be reachable from a test
// build at all (OR-B-17). The allowlist is checked at startup, not at send time.
struct EndpointPolicy {
    std::vector<std::string> allowed_hosts{"127.0.0.1", "localhost", "lp-sim"};
    bool require_tls = false;         // lab loopback; OR-Q-16 flips this
    bool allow_plaintext_remote = false;
};

class Config {
public:
    // Fully formed lab configuration from design doc 21.0: ACC1 on EUR/USD via
    // SIM01, tick 0.00001, qty step 1000, H disabled.
    static Config lab_default();

    // Recomputes `checksum` and bumps the versions. Call after any mutation; a
    // config whose checksum does not match its content is refused at startup
    // (OR-B-16).
    void finalize();

    Result<bool, RoutingError> validate() const;

    const SymbolProfile* symbol(const std::string& user_symbol) const;
    // Returns ValidationError-free alias resolution: a user alias that maps to
    // more than one wire symbol is ambiguous and must be refused (OR-B-11).
    Result<std::string, RoutingError> resolve_symbol_alias(const std::string& alias) const;

    const Route* route_for(const AccountId& account) const;
    bool account_belongs_to(const TenantId& tenant, const AccountId& account) const;
    bool entitled(const AccountId& account) const;

    bool ord_type_allowed(OrdType t) const;
    bool tif_allowed(TimeInForce t) const;

    // Mutators used by configuration tests; each one bumps a version.
    void set_route_active(const AccountId& account, bool active);
    void set_route_lp(const AccountId& account, const LpId& lp, const std::string& target_comp);
    void add_route(Route r);
    void revoke_entitlement(const AccountId& account);
    void set_symbol_price_scale(const std::string& user_symbol, int scale, std::int64_t tick);
    void add_symbol_alias(const std::string& alias, const std::string& wire_symbol);

    std::map<std::string, SymbolProfile> symbols;      // keyed by user symbol
    std::map<std::string, std::vector<std::string>> aliases;  // alias -> wire symbols
    std::vector<Route> routes;
    std::map<std::string, std::vector<std::string>> tenant_accounts;  // tenant -> accounts
    std::vector<std::string> revoked_accounts;
    Capabilities capabilities;
    OrderProfile order_profile;
    Limits limits;
    EndpointPolicy endpoints;

    std::string roe_profile_name = "fix44_lab_v1";
    std::uint64_t route_config_version = 1;
    std::uint64_t profile_version = 1;
    Hash checksum;
    bool checksum_present = true;  // OR-B-16 clears this to prove fail-closed

private:
    Hash compute_checksum() const;
};

}  // namespace router
