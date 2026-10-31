// recon_operator_cli -- the operator side of reconciliation.
//
// This is the tool design doc 14.4 describes: it lists open cases, prints the
// escalation bundle for one, and performs a resolution. The refusals are the
// point of it. A resolution needs the role, a reason, an evidence digest and
// two distinct approvers, and there is deliberately no command that marks an
// order filled -- 14.4 says there must be no one-click "force filled" without
// evidence, so none exists.
//
// It starts from a scripted incident (an order the LP never acknowledged, with
// the status query capability off) so the drill has something to work on.
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../common/world.hpp"

using namespace app;

namespace {

Role parse_role(const std::string& s) {
    if (s == "admin") return Role::Admin;
    if (s == "operator" || s == "ops") return Role::Operator;
    if (s == "trader") return Role::Trader;
    return Role::Viewer;
}

void print_help() {
    std::puts(
        "commands:\n"
        "  cases                                      list every reconciliation case\n"
        "  bundle <case_id> [role]                    print the escalation bundle\n"
        "  resolve <case_id> <role> <actor> <ev> <a1> <a2> <reason...>\n"
        "                                             attempt a resolution\n"
        "  route-off <role> <actor> <reason...>       disable the ACC1 route\n"
        "  resume <role> <actor> <reason...>          resume after a halt\n"
        "  quarantine                                 list quarantined events\n"
        "  audit                                      print the audit trail\n"
        "  invariants                                 run the R-I01..R-I26 oracle\n"
        "  help | quit\n"
        "\n"
        "roles: viewer, trader, operator, admin. <ev> is any string; it is hashed\n"
        "into the evidence digest the resolution is recorded against, and the\n"
        "literal word 'none' produces the zero digest, which is refused.");
}

// Builds the incident: a New the peer never answers, with 35=H unavailable, so
// the coordinator has no probe to send and the case lands on a human.
void stage_incident(World& w) {
    w.lp_image.suppress_reports = true;
    CommandEnvelope c;
    c.principal = World::principal(Role::Trader, "cli");
    c.account = AccountId("ACC1");
    c.kind = CommandKind::New;
    c.client_command_id = "K1";
    c.symbol = "EUR/USD";
    c.side = Side::Buy;
    c.qty_text = "100000";
    c.price_text = "1.08500";
    auto r = w.rt->submit(c);
    if (r.is_err()) {
        std::fprintf(stderr, "could not stage the incident: %s\n",
                     std::string(to_string(r.error())).c_str());
        return;
    }
    w.settle();
    for (int i = 0; i < 5; ++i) {
        w.clock.advance(w.cfg.limits.ack_timeout_micros * 2);
        w.rt->on_timer();
        w.settle();
    }
    std::printf("staged: order %s on chain %s, the peer has said nothing\n",
                r.value().cl_ord_id.str().c_str(), r.value().chain_id.str().c_str());
}

void list_cases(World& w) {
    if (w.store_image.recon_cases.empty()) {
        std::puts("(no cases)");
        return;
    }
    std::printf("%-10s %-8s %-22s %-22s %5s %-12s %s\n", "case", "chain", "cause", "state",
                "tries", "resolved_by", "note");
    for (const auto& [k, c] : w.store_image.recon_cases) {
        (void)k;
        std::printf("%-10s %-8s %-22s %-22s %5d %-12s %s\n", c.case_id.str().c_str(),
                    c.chain_id.str().c_str(), std::string(to_string(c.cause)).c_str(),
                    std::string(to_string(c.state)).c_str(), c.attempt_count,
                    c.resolved_by.empty() ? "-" : c.resolved_by.c_str(),
                    c.notes.empty() ? "" : c.notes.back().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    World w;
    if (!w.booted) {
        std::fprintf(stderr, "router refused to start: %s\n", w.rt->halt_reason().c_str());
        return 1;
    }
    std::puts("P04 reconciliation operator console (in-process lab)");
    stage_incident(w);
    list_cases(w);

    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    std::puts("\ntype 'help' for commands");

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string verb;
        if (!(in >> verb)) continue;

        if (verb == "quit" || verb == "exit") break;
        if (verb == "help") {
            print_help();
        } else if (verb == "cases") {
            list_cases(w);
        } else if (verb == "bundle") {
            std::string id, role = "operator";
            in >> id >> role;
            const Principal who = World::principal(parse_role(role), "console");
            std::puts(w.rt->escalation_bundle(who, ReconCaseId(id)).c_str());
        } else if (verb == "resolve") {
            std::string id, role, actor, evidence, a1, a2, reason, word;
            in >> id >> role >> actor >> evidence >> a1 >> a2;
            while (in >> word) reason += (reason.empty() ? "" : " ") + word;
            const Principal who = World::principal(parse_role(role), actor);
            const Hash digest = evidence == "none" ? Hash{} : sha256(evidence);
            auto r = w.rt->operator_resolve_case(who, ReconCaseId(id), reason, digest, {a1, a2});
            if (r.is_err())
                std::printf("DENIED %s -- the case is unchanged\n",
                            std::string(to_string(r.error())).c_str());
            else
                std::puts("resolved, with the actor, reason, approvers and evidence recorded");
        } else if (verb == "route-off") {
            std::string role, actor, reason, word;
            in >> role >> actor;
            while (in >> word) reason += (reason.empty() ? "" : " ") + word;
            auto r = w.rt->operator_set_route_active(World::principal(parse_role(role), actor),
                                                     AccountId("ACC1"), false, reason);
            std::printf("%s\n", r.is_err() ? "DENIED" : "route disabled and audited");
        } else if (verb == "resume") {
            std::string role, actor, reason, word;
            in >> role >> actor;
            while (in >> word) reason += (reason.empty() ? "" : " ") + word;
            auto r = w.rt->operator_resume(World::principal(parse_role(role), actor), reason);
            std::printf("%s mode=%s\n", r.is_err() ? "DENIED" : "resumed",
                        std::string(to_string(w.rt->mode())).c_str());
        } else if (verb == "quarantine") {
            if (w.store_image.quarantine.empty()) std::puts("(nothing quarantined)");
            for (const QuarantineEntry& q : w.store_image.quarantine)
                std::printf("  %4llu %-22s receipt=%llu hash=%s resolved=%s %s\n",
                            static_cast<unsigned long long>(q.id),
                            std::string(to_string(q.reason)).c_str(),
                            static_cast<unsigned long long>(q.receipt_id),
                            q.raw_hash.short_hex().c_str(), q.resolved ? "yes" : "no",
                            q.note.c_str());
        } else if (verb == "audit") {
            for (const AuditRecord& r : w.store_image.audit.records())
                std::printf("  %4llu %-22s %-10s %-16s %-28s %s\n",
                            static_cast<unsigned long long>(r.seq),
                            std::string(to_string(r.kind)).c_str(), r.actor.c_str(),
                            r.subject.c_str(), r.reason.c_str(), r.detail.c_str());
            std::printf("  chain verifies: %s\n", w.store_image.audit.verify() ? "yes" : "NO");
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
