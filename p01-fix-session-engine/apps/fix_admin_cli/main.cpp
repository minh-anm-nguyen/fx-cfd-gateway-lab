// Operator command line: inspect session state, verify store consistency and
// perform a coordinated sequence reset under dual control.
//
// A reset needs two distinct approvers and is always audited (design doc 11.2,
// scenarios J-15 and K-11). There is deliberately no single-flag reset.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "fix/config.hpp"
#include "fix/sqlite_store.hpp"

using namespace fixse;

namespace {

void usage() {
    std::printf(
        "usage: fix_admin_cli <command> --config <file> [options]\n"
        "\n"
        "commands:\n"
        "  status    print the persisted sequence state\n"
        "  verify    run the startup consistency scan and print the verdict\n"
        "  reset     coordinated sequence reset, requires two distinct approvers:\n"
        "              --approver <name> --approver <name> --reason <text>\n"
        "\n"
        "A reset starts a new epoch and sets both sequences back to one. It does\n"
        "not delete history. Do not run it without the counterparty agreeing.\n");
}

int open_store(const SessionConfig& cfg, const IClock& clock,
               std::unique_ptr<SqliteSessionStore>* store) {
    SqliteSessionStore::Options o;
    o.path = cfg.database_path;
    o.key.begin_string = cfg.begin_string;
    o.key.sender_comp_id = cfg.sender_comp_id;
    o.key.target_comp_id = cfg.target_comp_id;
    o.key.qualifier = cfg.qualifier;
    o.busy_timeout_ms = cfg.store_busy_timeout_ms;
    o.journal_mode = cfg.sqlite_journal_mode;
    o.synchronous = cfg.sqlite_synchronous;
    o.create_if_missing = false;
    o.clock = &clock;
    std::string why;
    const StoreError e = SqliteSessionStore::open(o, store, &why);
    if (e != StoreError::None) {
        std::fprintf(stderr, "cannot open store: %s (%s)\n", why.c_str(),
                     std::string(to_string(e)).c_str());
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string command = argv[1];
    std::string config_path;
    std::string reason;
    std::vector<std::string> approvers;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "--config" && has_next) config_path = argv[++i];
        else if (a == "--approver" && has_next) approvers.push_back(argv[++i]);
        else if (a == "--reason" && has_next) reason = argv[++i];
        else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            return 1;
        }
    }
    if (config_path.empty()) {
        usage();
        return 1;
    }

    SessionConfig cfg;
    std::vector<ConfigError> errors;
    if (!load_config_file(config_path, &cfg, &errors)) {
        for (const ConfigError& e : errors)
            std::fprintf(stderr, "config %s: %s\n", e.field.c_str(), e.reason.c_str());
        return 2;
    }

    SystemClock clock;
    std::unique_ptr<SqliteSessionStore> store;
    const int rc = open_store(cfg, clock, &store);
    if (rc != 0) return rc;

    // Opening the store marks it as having a live owner. A read-only command
    // must not leave the store looking as though it crashed, so the flag it
    // found is restored on the way out.
    const bool was_clean = store->verify_consistency().clean_shutdown;
    const auto restore = [&] {
        if (was_clean) store->mark_clean_shutdown();
    };

    if (command == "status") {
        PersistedState st;
        if (store->load_state(&st) != StoreError::None) {
            std::fprintf(stderr, "state is unreadable or failed its integrity check\n");
            return 3;
        }
        std::printf("session   %s\n", cfg.session_id().c_str());
        std::printf("epoch     %llu\n", static_cast<unsigned long long>(st.epoch));
        std::printf("next_in   %llu\n", static_cast<unsigned long long>(st.next_in));
        std::printf("next_out  %llu\n", static_cast<unsigned long long>(st.next_out));
        std::printf("version   %llu\n", static_cast<unsigned long long>(st.version));
        return 0;
    }

    if (command == "verify") {
        const ConsistencyReport r = store->verify_consistency();
        std::printf("consistent      %s\n", r.ok ? "yes" : "NO");
        std::printf("checksum valid  %s\n", r.integrity_checksum_valid ? "yes" : "NO");
        std::printf("clean shutdown  %s\n", r.clean_shutdown ? "yes" : "no");
        std::printf("max outbound    %llu\n", static_cast<unsigned long long>(r.max_outbound_seq));
        std::printf("max inbox       %llu\n", static_cast<unsigned long long>(r.max_inbox_seq));
        std::printf("pending inbox   %zu\n", r.pending_inbox_rows);
        if (!r.ok) std::printf("problem         %s\n", r.problem.c_str());
        return r.ok ? 0 : 4;
    }

    if (command == "reset") {
        if (approvers.size() != 2 || approvers[0] == approvers[1] || reason.empty()) {
            std::fprintf(stderr, "a coordinated reset needs two DIFFERENT approvers and a reason\n");
            return 1;
        }
        const StoreError e = store->coordinated_reset(approvers[0], approvers[1], reason);
        if (e != StoreError::None) {
            std::fprintf(stderr, "reset refused: %s\n", std::string(to_string(e)).c_str());
            return 5;
        }
        PersistedState st;
        store->load_state(&st);
        std::printf("reset applied: epoch %llu, next_in 1, next_out 1\n",
                    static_cast<unsigned long long>(st.epoch));
        std::printf("history from earlier epochs is retained for reconciliation\n");
        store->mark_clean_shutdown();
        return 0;
    }

    usage();
    return 1;
}
