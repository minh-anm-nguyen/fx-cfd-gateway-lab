// order_lab_cli -- a small driver for exercising the state machine by hand.
//
// It reads the same verbs as order_trace_replay from standard input, so a
// session can be typed and then saved as a regression fixture. It is a lab
// tool: no network, no real account, and it says so on startup.
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "order_core/engine.hpp"

using namespace order_core;

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        const size_t start = pos;
        while (pos < line.size() && line[pos] != ' ') ++pos;
        if (pos > start) out.push_back(line.substr(start, pos - start));
    }
    return out;
}

// "qty=100000" -> "100000"; a bare token yields itself.
std::string value_of(const std::vector<std::string>& tokens, const std::string& key,
                     const std::string& fallback = "") {
    const std::string prefix = key + "=";
    for (const std::string& t : tokens) {
        if (t.rfind(prefix, 0) == 0) return t.substr(prefix.size());
    }
    return fallback;
}

std::string at(const std::vector<std::string>& tokens, size_t i) {
    return i < tokens.size() ? tokens[i] : std::string();
}

void report(const EngineResult& r) {
    std::printf("%s %s %s\n", to_string(r.decision), to_string(r.reason), r.detail.c_str());
}

}  // namespace

int main() {
    std::printf("order_lab_cli -- FX/CFD order state machine (lab only, no live account)\n");
    const Policy policy = default_lab_policy();
    const ConfigError e = policy.validate();
    if (!e.ok()) {
        std::printf("config rejected: %s (%s)\n", to_string(e.code), e.detail.c_str());
        return 1;
    }
    std::printf("profile strict_v1, namespace %s, symbols:", policy.business_namespace.c_str());
    for (const auto& [symbol, spec] : policy.symbols) {
        std::printf(" %s(tick=%lld/%lld step=%lld)", symbol.c_str(),
                    static_cast<long long>(spec.tick_numerator),
                    static_cast<long long>(spec.tick_denominator),
                    static_cast<long long>(spec.qty_step));
    }
    std::printf("\ndurability %s -- this store does not prove crash safety (that is P06)\n",
                to_string(DurabilityMode::InMemory));
    std::printf("\ncommands:\n"
                "  new <id> qty=<n> px=<p>\n"
                "  ack <id>\n"
                "  fill <execid> cl=<id> qty=<n> [px=<p>]\n"
                "  cancel <id> orig=<id>\n"
                "  replace <id> orig=<id> qty=<n>\n"
                "  dispatch <id> | timeout <id>\n"
                "  show | recover | quit\n\n");

    MemoryStore store;
    Engine engine(store, policy);
    uint64_t seq = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        const std::vector<std::string> t = split(line);
        if (t.empty()) continue;
        const std::string verb = t[0];
        if (verb == "quit" || verb == "exit") break;

        if (verb == "new" || verb == "cancel" || verb == "replace") {
            LocalCommand cmd;
            cmd.kind = verb == "new"       ? CommandKind::New
                       : verb == "cancel" ? CommandKind::Cancel
                                          : CommandKind::Replace;
            cmd.account = value_of(t, "acct", "ACC1");
            cmd.symbol = value_of(t, "sym", "EURUSD");
            cmd.side_raw = value_of(t, "side", "1");
            cmd.cl_ord_id = at(t, 1);
            cmd.orig_cl_ord_id = value_of(t, "orig");
            cmd.qty_text = value_of(t, "qty");
            cmd.price_text = value_of(t, "px");
            report(engine.submit(OrderEvent{cmd}));
        } else if (verb == "dispatch" || verb == "timeout") {
            SystemEvent s;
            s.kind = verb == "dispatch" ? SystemKind::Dispatched : SystemKind::AckTimeout;
            s.target_cl_ord_id = at(t, 1);
            report(engine.submit(OrderEvent{s}));
        } else if (verb == "ack" || verb == "fill") {
            PeerReport r;
            r.msg_type = '8';
            r.account = value_of(t, "acct", "ACC1");
            r.symbol = value_of(t, "sym", "EURUSD");
            r.side_raw = value_of(t, "side", "1");
            r.source_seq = ++seq;
            if (verb == "ack") {
                r.exec_type = exec_type::kNew;
                r.ord_status = ord_status::kNew;
                r.cl_ord_id = at(t, 1);
                r.exec_id = "E-ACK-" + r.cl_ord_id;
                r.order_id = "LPO-" + r.cl_ord_id;
            } else {
                r.exec_type = exec_type::kTrade;
                r.ord_status = ord_status::kPartiallyFilled;
                r.exec_id = at(t, 1);
                r.cl_ord_id = value_of(t, "cl");
                r.last_qty = value_of(t, "qty");
                r.last_px = value_of(t, "px", "1.08490");
                if (!value_of(t, "cum").empty()) r.cum_qty = value_of(t, "cum");
            }
            report(engine.submit(OrderEvent{r}));
        } else if (verb == "recover") {
            const RecoveryReport rep = engine.recover();
            std::printf("%s replayed=%zu chains=%zu trading=%s %s\n", to_string(rep.status),
                        rep.records_replayed, rep.chains_recovered,
                        rep.trading_allowed ? "allowed" : "blocked", rep.detail.c_str());
        } else if (verb == "show") {
            for (uint64_t id = 1; id <= engine.chain_count() + 8; ++id) {
                const OrderAggregate* a = engine.find(OrderId{id});
                if (a == nullptr) continue;
                std::printf("chain %llu  %-8s %-24s cum=%lld leaves=%lld fills=%zu rev=%llu\n",
                            static_cast<unsigned long long>(id), a->initial_cl_ord_id.c_str(),
                            to_string(a->lifecycle()), static_cast<long long>(a->cum_qty_units),
                            static_cast<long long>(a->leaves_qty_units()), a->ledger.size(),
                            static_cast<unsigned long long>(a->revision));
                const std::vector<InvariantViolation> v = check_invariants(*a, policy);
                if (!v.empty()) std::printf("  VIOLATIONS: %s\n", describe(v).c_str());
            }
            const LatencySummary s = engine.event_to_commit_latency();
            if (s.samples > 0) {
                std::printf("latency %s: p50=%lluns p99=%lluns over %zu samples\n",
                            s.what_is_measured(), static_cast<unsigned long long>(s.p50_ns),
                            static_cast<unsigned long long>(s.p99_ns), s.samples);
            }
        } else {
            std::printf("unknown command '%s'\n", verb.c_str());
        }
    }
    return 0;
}
