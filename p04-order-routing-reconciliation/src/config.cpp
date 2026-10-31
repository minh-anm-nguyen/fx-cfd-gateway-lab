#include "router/config.hpp"

#include <algorithm>

namespace router {

Config Config::lab_default() {
    Config c;

    SymbolProfile eurusd;
    eurusd.wire_symbol = "EUR/USD";
    eurusd.price_scale = 5;
    eurusd.tick_ticks = 1;
    eurusd.qty_step = 1000;
    eurusd.min_qty = 1000;
    eurusd.max_qty = 100000000;
    eurusd.max_price_ticks = 1000000;  // 10.00000, far above any FX rate
    c.symbols["EUR/USD"] = eurusd;

    SymbolProfile gbpusd = eurusd;
    gbpusd.wire_symbol = "GBP/USD";
    c.symbols["GBP/USD"] = gbpusd;

    SymbolProfile usdjpy = eurusd;
    usdjpy.wire_symbol = "USD/JPY";
    usdjpy.price_scale = 3;  // JPY pairs quote to three places
    usdjpy.max_price_ticks = 1000000;
    c.symbols["USD/JPY"] = usdjpy;

    c.aliases["EUR/USD"] = {"EUR/USD"};
    c.aliases["GBP/USD"] = {"GBP/USD"};
    c.aliases["USD/JPY"] = {"USD/JPY"};

    Route r;
    r.account = AccountId("ACC1");
    r.lp = LpId("LP_SIM_01");
    r.sender_comp_id = "ROUTER04";
    r.target_comp_id = "LPSIM01";
    r.session_qualifier = "PRIMARY";
    r.endpoint = "127.0.0.1:19004";
    r.active = true;
    r.priority = 0;
    c.routes.push_back(r);

    Route r2 = r;
    r2.account = AccountId("ACC2");
    r2.endpoint = "127.0.0.1:19004";
    c.routes.push_back(r2);

    c.tenant_accounts["T1"] = {"ACC1"};
    c.tenant_accounts["T2"] = {"ACC2"};

    c.finalize();
    return c;
}

Hash Config::compute_checksum() const {
    Sha256 s;
    s.field(roe_profile_name);
    for (const auto& [k, p] : symbols) {
        s.field(k);
        s.field(p.wire_symbol);
        s.update(static_cast<std::uint64_t>(p.price_scale));
        s.update(static_cast<std::uint64_t>(p.tick_ticks));
        s.update(static_cast<std::uint64_t>(p.qty_step));
    }
    for (const Route& r : routes) {
        s.field(r.account.str());
        s.field(r.lp.str());
        s.field(r.sender_comp_id);
        s.field(r.target_comp_id);
        s.field(r.endpoint);
        s.update(static_cast<std::uint64_t>(r.active ? 1 : 0));
    }
    s.update(static_cast<std::uint64_t>(capabilities.order_status_request ? 1 : 0));
    s.update(route_config_version);
    s.update(profile_version);
    return s.finish();
}

void Config::finalize() { checksum = compute_checksum(); }

Result<bool, RoutingError> Config::validate() const {
    using R = Result<bool, RoutingError>;

    // A configuration with no checksum, or one that does not match its content,
    // cannot be trusted to describe what we are about to trade (OR-B-16).
    if (!checksum_present) return R::err(RoutingError::ConfigInvalid);
    if (checksum != compute_checksum()) return R::err(RoutingError::ConfigInvalid);

    // Lab boundary: a configuration that can reach a real broker is refused at
    // startup rather than at send time (OR-B-17).
    for (const Route& r : routes) {
        const std::size_t colon = r.endpoint.rfind(':');
        const std::string host = colon == std::string::npos ? r.endpoint : r.endpoint.substr(0, colon);
        if (std::find(endpoints.allowed_hosts.begin(), endpoints.allowed_hosts.end(), host) ==
            endpoints.allowed_hosts.end())
            return R::err(RoutingError::ConfigInvalid);
        if (endpoints.require_tls && !endpoints.allow_plaintext_remote && host != "127.0.0.1" &&
            host != "localhost")
            return R::err(RoutingError::ConfigInvalid);
    }

    // Two active routes for one account with equal priority is ambiguous. Doc
    // 8.2 forbids a deterministic-looking random choice, so this is fatal and
    // not resolved by sorting (OR-B-03).
    std::map<std::string, std::vector<const Route*>> by_account;
    for (const Route& r : routes)
        if (r.active) by_account[r.account.str()].push_back(&r);
    for (const auto& [acct, rs] : by_account) {
        (void)acct;
        if (rs.size() < 2) continue;
        int best = rs[0]->priority;
        int count_at_best = 0;
        for (const Route* r : rs) best = std::min(best, r->priority);
        for (const Route* r : rs)
            if (r->priority == best) ++count_at_best;
        if (count_at_best > 1) return R::err(RoutingError::ConfigInvalid);
    }

    // A route whose CompIDs belong to a different peer would cross-send
    // (OR-B-06). In the lab every route to one LP must agree on its CompIDs.
    std::map<std::string, std::string> lp_target;
    for (const Route& r : routes) {
        auto it = lp_target.find(r.lp.str());
        if (it == lp_target.end())
            lp_target[r.lp.str()] = r.target_comp_id;
        else if (it->second != r.target_comp_id)
            return R::err(RoutingError::ConfigInvalid);
    }

    for (const auto& [k, p] : symbols) {
        (void)k;
        if (p.price_scale < 0 || p.price_scale > 9) return R::err(RoutingError::ConfigInvalid);
        if (p.tick_ticks <= 0 || p.qty_step <= 0) return R::err(RoutingError::ConfigInvalid);
    }
    return R::ok(true);
}

const SymbolProfile* Config::symbol(const std::string& user_symbol) const {
    auto it = symbols.find(user_symbol);
    return it == symbols.end() ? nullptr : &it->second;
}

Result<std::string, RoutingError> Config::resolve_symbol_alias(const std::string& alias) const {
    using R = Result<std::string, RoutingError>;
    auto it = aliases.find(alias);
    if (it == aliases.end()) return R::err(RoutingError::UnsupportedSymbol);
    if (it->second.size() != 1) return R::err(RoutingError::AmbiguousSymbol);
    return R::ok(it->second.front());
}

const Route* Config::route_for(const AccountId& account) const {
    const Route* best = nullptr;
    for (const Route& r : routes) {
        if (!r.active || r.account != account) continue;
        if (best == nullptr || r.priority < best->priority) best = &r;
    }
    return best;
}

bool Config::account_belongs_to(const TenantId& tenant, const AccountId& account) const {
    auto it = tenant_accounts.find(tenant.str());
    if (it == tenant_accounts.end()) return false;
    return std::find(it->second.begin(), it->second.end(), account.str()) != it->second.end();
}

bool Config::entitled(const AccountId& account) const {
    return std::find(revoked_accounts.begin(), revoked_accounts.end(), account.str()) ==
           revoked_accounts.end();
}

bool Config::ord_type_allowed(OrdType t) const {
    const auto& v = order_profile.allowed_ord_types;
    return std::find(v.begin(), v.end(), t) != v.end();
}

bool Config::tif_allowed(TimeInForce t) const {
    const auto& v = order_profile.allowed_tif;
    return std::find(v.begin(), v.end(), t) != v.end();
}

void Config::set_route_active(const AccountId& account, bool active) {
    for (Route& r : routes)
        if (r.account == account) r.active = active;
    ++route_config_version;
    finalize();
}

void Config::set_route_lp(const AccountId& account, const LpId& lp,
                          const std::string& target_comp) {
    for (Route& r : routes)
        if (r.account == account) {
            r.lp = lp;
            r.target_comp_id = target_comp;
        }
    ++route_config_version;
    finalize();
}

void Config::add_route(Route r) {
    routes.push_back(std::move(r));
    ++route_config_version;
    finalize();
}

void Config::revoke_entitlement(const AccountId& account) {
    revoked_accounts.push_back(account.str());
    ++route_config_version;
    finalize();
}

void Config::set_symbol_price_scale(const std::string& user_symbol, int scale,
                                    std::int64_t tick) {
    auto it = symbols.find(user_symbol);
    if (it == symbols.end()) return;
    it->second.price_scale = scale;
    it->second.tick_ticks = tick;
    ++profile_version;
    finalize();
}

void Config::add_symbol_alias(const std::string& alias, const std::string& wire_symbol) {
    aliases[alias].push_back(wire_symbol);
    ++profile_version;
    finalize();
}

}  // namespace router
