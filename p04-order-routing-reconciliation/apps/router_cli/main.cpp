// order_router_cli -- walk an order through the router by hand.
//
// It reads one command per line from stdin, or runs a named demo. Everything is
// in process (see ../common/world.hpp), so this is a way to look at the state
// machine rather than a deployable client.
//
// The output deliberately separates the two things design doc 1.4 insists are
// not the same: the local delivery state of a command, and the business outcome
// of the order. "ACCEPTED_DURABLE / UNKNOWN" is the honest answer to a submit.
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../common/world.hpp"
#include "router/numeric.hpp"

using namespace app;

namespace {

void print_help() {
    std::puts(
        "commands:\n"
        "  new <cmd_id> <qty> <price>        submit a NewOrderSingle intent\n"
        "  cancel <cmd_id> <chain_id>        submit an OrderCancelRequest\n"
        "  replace <cmd_id> <chain> <q> <p>  submit an OrderCancelReplaceRequest\n"
        "  fill <cl_ord_id> <qty> <px>       make the LP execute against the order\n"
        "  ack <cl_ord_id>                   make the LP acknowledge the order\n"
        "  accept-cancel <cl_ord_id>         make the LP confirm a cancel request\n"
        "  accept-replace <cl_ord_id>        make the LP accept a replace request\n"
        "  down | up                         take the FIX session down or up\n"
        "  tick [micros]                     advance the clock and run the timers\n"
        "  crash                             drop the process state, keep the disks\n"
        "  chains                            show the order chains\n"
        "  commands                          show the command records\n"
        "  cases                             show the reconciliation cases\n"
        "  wire                              show the application frames sent\n"
        "  audit                             show the audit trail\n"
        "  metrics                           show the counters\n"
        "  invariants                        run the R-I01..R-I26 oracle\n"
        "  demo                              run a full order lifecycle\n"
        "  help | quit");
}

void show_chains(World& w) {
    if (w.store_image.chains.empty()) {
        std::puts("(no chains)");
        return;
    }
    std::printf("%-8s %-8s %-10s %-16s %9s %9s %9s %5s %s\n", "chain", "accepted", "status",
                "lp_order_id", "cum", "leaves", "avg_px", "ver", "hold");
    for (const auto& [id, c] : w.store_image.chains) {
        std::printf("%-8s %-8s %-10s %-16s %9lld %9lld %9s %5llu %s\n", id.c_str(),
                    c.accepted_cl_ord_id.str().c_str(), std::string(to_string(c.status)).c_str(),
                    (c.lp_order_id ? c.lp_order_id->str() : "-").c_str(),
                    static_cast<long long>(c.cum_qty), static_cast<long long>(c.leaves_qty),
                    format_decimal_ticks(c.avg_px(), c.price_scale).c_str(),
                    static_cast<unsigned long long>(c.version),
                    (c.hold ? std::string(to_string(*c.hold)) : std::string("-")).c_str());
        for (const p03::Fill& f : c.fills)
            std::printf("    fill %-12s qty=%-8lld px=%s\n", f.exec_id.str().c_str(),
                        static_cast<long long>(f.qty),
                        format_decimal_ticks(f.px, c.price_scale).c_str());
    }
}

void show_commands(World& w) {
    if (w.store_image.intents.empty()) {
        std::puts("(no commands)");
        return;
    }
    std::printf("%-14s %-11s %-8s %-8s %-18s %-13s %-8s %s\n", "command", "kind", "chain",
                "clordid", "delivery", "business", "orig_seq", "hold");
    for (const auto& [key, in] : w.store_image.intents) {
        (void)key;
        const std::string seq = in.original_seq ? std::to_string(*in.original_seq) : "-";
        std::printf("%-14s %-11s %-8s %-8s %-18s %-13s %-8s %s\n", in.command_id.str().c_str(),
                    std::string(to_string(in.kind)).c_str(), in.chain_id.str().c_str(),
                    in.cl_ord_id.str().c_str(), std::string(to_string(in.delivery)).c_str(),
                    std::string(to_string(in.business)).c_str(), seq.c_str(),
                    (in.hold ? std::string(to_string(*in.hold)) : std::string("-")).c_str());
    }
}

void show_cases(World& w) {
    if (w.store_image.recon_cases.empty()) {
        std::puts("(no reconciliation cases)");
        return;
    }
    std::printf("%-10s %-8s %-22s %-22s %8s\n", "case", "chain", "cause", "state", "attempts");
    for (const auto& [k, c] : w.store_image.recon_cases) {
        (void)k;
        std::printf("%-10s %-8s %-22s %-22s %8d\n", c.case_id.str().c_str(),
                    c.chain_id.str().c_str(), std::string(to_string(c.cause)).c_str(),
                    std::string(to_string(c.state)).c_str(), c.attempt_count);
    }
}

void report(const Result<AcceptedIntent, RoutingError>& r) {
    if (r.is_err()) {
        std::printf("REFUSED %s\n", std::string(to_string(r.error())).c_str());
        return;
    }
    const AcceptedIntent& a = r.value();
    // The two columns that matter, kept apart on purpose.
    std::printf("command=%s chain=%s clordid=%s route=%s delivery=%s business=%s%s\n",
                a.command_id.str().c_str(), a.chain_id.str().c_str(), a.cl_ord_id.str().c_str(),
                a.route.str().c_str(), std::string(to_string(a.delivery)).c_str(),
                std::string(to_string(a.business)).c_str(),
                a.idempotent_replay ? " (idempotent replay)" : "");
}

CommandEnvelope new_order(const std::string& id, const std::string& qty,
                          const std::string& price) {
    CommandEnvelope c;
    c.principal = World::principal(Role::Trader, "cli");
    c.account = AccountId("ACC1");
    c.kind = CommandKind::New;
    c.client_command_id = id;
    c.symbol = "EUR/USD";
    c.side = Side::Buy;
    c.qty_text = qty;
    c.price_text = price;
    return c;
}

void restart(World& w) {
    // The disks survive; everything process-side is rebuilt, which is what a
    // real restart does (design doc 15.3).
    w.rt.reset();
    w.lp.reset();
    w.session.reset();
    w.store.reset();
    w.store = std::make_unique<Store>(w.store_image, w.fp, w.clock);
    w.session = std::make_unique<p01::SessionEngine>(w.session_image, w.fp);
    w.lp = std::make_unique<p02::LpSimulator>(w.lp_image, *w.session);
    w.rt = std::make_unique<Router>(w.cfg, *w.store, *w.session, *w.risk, w.metrics, w.fp,
                                    w.clock);
    const bool ok = w.rt->boot().is_ok();
    std::printf("restarted: boot=%s mode=%s\n", ok ? "ok" : "refused",
                std::string(to_string(w.rt->mode())).c_str());
}

void run_demo(World& w) {
    std::puts("-- submit a New for 100,000 EUR/USD at 1.08500");
    report(w.rt->submit(new_order("K1", "100000", "1.08500")));
    std::puts("-- dispatch and pump: the LP acknowledges");
    w.settle();
    show_chains(w);

    std::puts("\n-- the LP fills 40,000");
    w.lp->execute("O-1", 40000, 108500);
    w.settle();
    show_chains(w);

    std::puts("\n-- submit a cancel for the remainder");
    CommandEnvelope c;
    c.principal = World::principal(Role::Trader, "cli");
    c.account = AccountId("ACC1");
    c.kind = CommandKind::Cancel;
    c.client_command_id = "C1";
    c.chain_id = OrderChainId("ord-1");
    report(w.rt->submit(c));
    w.settle();
    show_chains(w);

    std::puts("\n-- the LP confirms the cancel");
    w.lp->accept_cancel("C-2");
    w.settle();
    show_chains(w);

    std::puts("\n-- commands");
    show_commands(w);
    std::printf("\naudit records: %zu (chain verifies: %s)\n", w.store_image.audit.size(),
                w.store_image.audit.verify() ? "yes" : "NO");
    const auto v = check_invariants(InvariantInputs{w.store_image, w.session_image, w.cfg});
    std::printf("invariant violations: %zu\n", v.size());
}

}  // namespace

int main(int argc, char** argv) {
    World w;
    if (!w.booted) {
        std::fprintf(stderr, "router refused to start: mode=%s reason=%s\n",
                     std::string(to_string(w.rt->mode())).c_str(), w.rt->halt_reason().c_str());
        return 1;
    }
    std::printf("P04 order router, in-process lab. route=LP_SIM_01 session=%s H=%s\n",
                w.session_image.identity.key().c_str(),
                w.cfg.capabilities.order_status_request ? "enabled" : "disabled");

    if (argc > 1 && std::string(argv[1]) == "demo") {
        run_demo(w);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    std::puts("type 'help' for commands, 'demo' for a full lifecycle");

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string verb;
        if (!(in >> verb)) continue;

        if (verb == "quit" || verb == "exit") break;
        if (verb == "help") {
            print_help();
        } else if (verb == "demo") {
            run_demo(w);
        } else if (verb == "new") {
            std::string id, qty, px;
            in >> id >> qty >> px;
            report(w.rt->submit(new_order(id, qty, px)));
        } else if (verb == "cancel") {
            std::string id, chain;
            in >> id >> chain;
            CommandEnvelope c;
            c.principal = World::principal(Role::Trader, "cli");
            c.account = AccountId("ACC1");
            c.kind = CommandKind::Cancel;
            c.client_command_id = id;
            c.chain_id = OrderChainId(chain);
            report(w.rt->submit(c));
        } else if (verb == "replace") {
            std::string id, chain, qty, px;
            in >> id >> chain >> qty >> px;
            CommandEnvelope c;
            c.principal = World::principal(Role::Trader, "cli");
            c.account = AccountId("ACC1");
            c.kind = CommandKind::Replace;
            c.client_command_id = id;
            c.chain_id = OrderChainId(chain);
            c.symbol = "EUR/USD";
            c.side = Side::Buy;
            c.qty_text = qty;
            c.price_text = px;
            report(w.rt->submit(c));
        } else if (verb == "fill") {
            std::string cl;
            long long qty = 0, px = 0;
            in >> cl >> qty >> px;
            w.lp->execute(cl, qty, px);
            w.settle();
        } else if (verb == "ack") {
            std::string cl;
            in >> cl;
            w.lp->send(w.lp->new_ack(cl));
            w.settle();
        } else if (verb == "accept-cancel") {
            std::string cl;
            in >> cl;
            w.lp->accept_cancel(cl);
            w.settle();
        } else if (verb == "accept-replace") {
            std::string cl;
            in >> cl;
            w.lp->accept_replace(cl);
            w.settle();
        } else if (verb == "down") {
            w.session->disconnect();
            std::puts("session down");
        } else if (verb == "up") {
            w.session->reconnect(false);
            w.settle();
            std::puts("session active");
        } else if (verb == "tick") {
            long long micros = 2000000;
            in >> micros;
            w.clock.advance(static_cast<std::uint64_t>(micros));
            w.rt->on_timer();
            w.settle();
            std::printf("mode=%s open_unknown=%zu open_cases=%zu\n",
                        std::string(to_string(w.rt->mode())).c_str(),
                        w.rt->open_unknown_outcomes(), w.rt->open_recon_cases());
        } else if (verb == "crash") {
            restart(w);
        } else if (verb == "chains") {
            show_chains(w);
        } else if (verb == "commands") {
            show_commands(w);
        } else if (verb == "cases") {
            show_cases(w);
        } else if (verb == "wire") {
            for (const std::string& f : w.session_image.wire_log)
                std::printf("  %s\n", wire_text(redact_fix_frame(f)).c_str());
        } else if (verb == "audit") {
            for (const AuditRecord& r : w.store_image.audit.records())
                std::printf("  %4llu %-22s %-10s %-16s %-28s %s\n",
                            static_cast<unsigned long long>(r.seq),
                            std::string(to_string(r.kind)).c_str(), r.actor.c_str(),
                            r.subject.c_str(), r.reason.c_str(), r.detail.c_str());
            std::printf("  chain verifies: %s\n", w.store_image.audit.verify() ? "yes" : "NO");
        } else if (verb == "metrics") {
            std::fputs(w.metrics.render().c_str(), stdout);
        } else if (verb == "invariants") {
            const auto v =
                check_invariants(InvariantInputs{w.store_image, w.session_image, w.cfg});
            if (v.empty())
                std::puts("all R-I01..R-I26 hold");
            else
                for (const Violation& x : v)
                    std::printf("  %s %s [%s]\n", x.invariant.c_str(), x.detail.c_str(),
                                x.subject.c_str());
        } else {
            std::printf("unknown command '%s' (try help)\n", verb.c_str());
        }
    }
    return 0;
}
