// recon_cli — what an operator needs during an incident.
//
// Lists the effects whose outcome is unknown, the open reconciliation
// cases and the current recovery phase, and can close a case with
// recorded approvals. Section 14's runbook and section 22's alert table
// are what this exists to serve.
//
//   recon_cli --db <path> [--session <path>] --tenant <id> <command>
//
//     status                 phase, gates and counts
//     unresolved             outbound effects that need a decision
//     cases                  open reconciliation cases
//     reconcile              ask P01 what, if anything, was ever bound
//     resolve <id> <who>     close a case; approvals are mandatory
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "jrnl/outbox.hpp"
#include "jrnl/recovery.hpp"
#include "jrnl/stable_send.hpp"
#include "jrnl/store.hpp"

using namespace jrnl;

int main(int argc, char** argv) {
    std::string db, session_db, tenant = "t1";
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db = argv[++i];
        else if (a == "--session" && i + 1 < argc) session_db = argv[++i];
        else if (a == "--tenant" && i + 1 < argc) tenant = argv[++i];
        else rest.push_back(a);
    }
    if (db.empty() || rest.empty()) {
        std::fprintf(stderr,
                     "usage: recon_cli --db <path> [--session <path>] --tenant <id> "
                     "{status|unresolved|cases|reconcile|resolve <id> <who>}\n");
        return 2;
    }

    SqliteAtomicStore::Options opt;
    opt.path = db;
    opt.create_if_missing = false;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    if (SqliteAtomicStore::open(opt, &store, &why) != StoreError::None) {
        std::fprintf(stderr, "cannot open %s: %s\n", db.c_str(), why.c_str());
        return 1;
    }

    const std::string cmd = rest[0];

    if (cmd == "status") {
        RecoverySupervisor::Options ropt;
        ropt.tenant = tenant;
        RecoverySupervisor sup(*store, ropt);
        const RecoveryReport r = sup.verify_and_replay(RecoveryMode::ProcessRestart);
        std::printf("phase=%s dispatch=%s streams=%zu events=%zu unresolved=%zu cases=%zu\n",
                    std::string(to_string(r.phase)).c_str(),
                    r.dispatch_enabled ? "enabled" : "disabled", r.streams_checked,
                    r.events_checked, r.unresolved_effects, r.open_cases);
        for (const std::string& d : r.details) std::printf("  - %s\n", d.c_str());
        // RECON_ONLY is not an error: it is the designed answer to
        // unresolved evidence, and an operator reading this needs that to
        // be plain rather than inferred from an exit code.
        return (r.phase == Phase::Faulted || r.phase == Phase::Quarantine) ? 1 : 0;
    }

    if (cmd == "unresolved") {
        int n = 0;
        for (const char* state : {"READY", "CLAIMED", "BOUND", "UNKNOWN", "HELD"}) {
            for (const OutboxRow& row : store->outbox_rows(tenant, state)) {
                const std::string seq =
                    row.original_seq.has_value() ? std::to_string(*row.original_seq) : "-";
                std::printf("%-10s %-20s account=%-8s seq=%-8s %s\n", row.state.c_str(),
                            row.command_id.c_str(), row.account.c_str(), seq.c_str(),
                            row.last_error.c_str());
                ++n;
            }
        }
        std::printf("%d effect(s) awaiting resolution\n", n);
        return 0;
    }

    if (cmd == "cases") {
        const std::vector<RecoveryCasePlan> cases = store->open_cases(tenant);
        for (const RecoveryCasePlan& c : cases) {
            std::printf("%-34s %-24s chain=%-12s evidence=%s\n", c.case_id.c_str(),
                        c.reason_code.c_str(), c.chain.c_str(),
                        to_hex(c.evidence_hash).substr(0, 16).c_str());
        }
        std::printf("%zu open case(s)\n", cases.size());
        return 0;
    }

    if (cmd == "reconcile") {
        if (session_db.empty()) {
            std::fprintf(stderr, "reconcile needs --session: the answer comes from P01\n");
            return 2;
        }
        StableSendStore::Options lopt;
        lopt.path = session_db;
        std::unique_ptr<StableSendStore> link;
        if (StableSendStore::open(lopt, &link, &why) != StoreError::None) {
            std::fprintf(stderr, "cannot open the session store: %s\n", why.c_str());
            return 1;
        }
        OutboxDispatcher::Options dopt;
        OutboxDispatcher d(*store, *link, dopt);
        const std::size_t n = d.reconcile_unknown(tenant, *link);
        std::printf("reconciled %zu unknown effect(s)\n", n);
        std::printf("note: a bound sequence is held, not resent. Counterparty evidence is "
                    "required before anything is declared complete.\n");
        return 0;
    }

    if (cmd == "resolve") {
        if (rest.size() < 3) {
            std::fprintf(stderr, "resolve needs a case id and the approving parties\n");
            return 2;
        }
        const StoreError e = store->resolve_case(tenant, rest[1], "RESOLVED", rest[2]);
        if (e != StoreError::None) {
            std::fprintf(stderr, "refused: %s\n", std::string(to_string(e)).c_str());
            return 1;
        }
        std::printf("case %s resolved, approvals recorded: %s\n", rest[1].c_str(),
                    rest[2].c_str());
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
