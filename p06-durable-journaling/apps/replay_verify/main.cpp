// replay_verify — read-only integrity check over a journal.
//
// Section 14's runbook asks for projections to be rebuilt read-only and
// compared before anything is reopened. This is that step as a command an
// operator can run against a database nobody is writing to.
//
//   replay_verify --db <path> --tenant <id> [--stream <id>] [--rebuild]
//
// Exits 0 when everything verifies, 1 when it does not. It writes nothing
// unless --rebuild is given.
#include <cstdio>
#include <memory>
#include <string>

#include "jrnl/recovery.hpp"
#include "jrnl/replay.hpp"
#include "jrnl/store.hpp"

using namespace jrnl;

int main(int argc, char** argv) {
    std::string db, tenant = "t1", stream;
    bool rebuild = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db = argv[++i];
        else if (a == "--tenant" && i + 1 < argc) tenant = argv[++i];
        else if (a == "--stream" && i + 1 < argc) stream = argv[++i];
        else if (a == "--rebuild") rebuild = true;
        else {
            std::fprintf(stderr,
                         "usage: replay_verify --db <path> --tenant <id> [--stream <id>] "
                         "[--rebuild]\n");
            return 2;
        }
    }
    if (db.empty()) {
        std::fprintf(stderr, "replay_verify: --db is required\n");
        return 2;
    }

    SqliteAtomicStore::Options opt;
    opt.path = db;
    opt.create_if_missing = false;
    std::unique_ptr<SqliteAtomicStore> store;
    std::string why;
    const StoreError e = SqliteAtomicStore::open(opt, &store, &why);
    if (e != StoreError::None) {
        std::fprintf(stderr, "cannot open %s: %s (%s)\n", db.c_str(), why.c_str(),
                     std::string(to_string(e)).c_str());
        return 1;
    }

    if (rebuild) {
        const StoreError r = store->rebuild_projections(tenant, &why);
        if (r != StoreError::None) {
            std::fprintf(stderr, "rebuild failed: %s\n", why.c_str());
            return 1;
        }
        std::printf("projections rebuilt from the journal\n");
    }

    auto report_stream = [&](const StreamId& s) {
        const ReplayResult rr = replay_from_empty(*store, tenant, s);
        std::printf("%-28s version=%llu events=%zu hash=%s %s\n", s.c_str(),
                    static_cast<unsigned long long>(rr.version), rr.events_applied,
                    to_hex(rr.state_hash).substr(0, 16).c_str(), rr.ok ? "OK" : "FAILED");
        if (!rr.ok) std::printf("  %s\n", rr.detail.c_str());
        return rr.ok;
    };

    if (!stream.empty()) return report_stream(stream) ? 0 : 1;

    for (const StreamId& s : store->list_streams(tenant)) report_stream(s);

    const VerifyReport v = verify_all(*store, tenant);
    std::printf("\nstreams=%zu events=%zu storage=%s journal=%s projections=%s manifests=%s "
                "dedup=%s audit=%s\n",
                v.streams_checked, v.events_checked, v.storage_valid ? "ok" : "FAILED",
                v.journal_valid ? "ok" : "FAILED", v.projections_valid ? "ok" : "FAILED",
                v.manifests_valid ? "ok" : "FAILED", v.dedup_valid ? "ok" : "FAILED",
                v.audit_valid ? "ok" : "FAILED");
    for (const std::string& d : v.details) std::printf("  - %s\n", d.c_str());
    if (!v.frozen_domains.empty()) {
        std::printf("frozen risk domains:");
        for (const RiskDomainId& d : v.frozen_domains) std::printf(" %s", d.c_str());
        std::printf("\n");
    }
    return v.ok() ? 0 : 1;
}
