// FIX 4.4 initiator service.
//
// Loads the configuration, opens the durable store, drives the engine through
// the connector and stops gracefully on an interrupt. Metrics and the audit
// trail are written on the way out so a lab run leaves evidence behind (design
// doc 14 and release gate E).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fix/failpoint.hpp"
#include "fix/session_actor.hpp"
#include "fix/sqlite_store.hpp"
#include "fix/tcp_connector.hpp"

using namespace fixse;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

// A sink that only records. Application semantics belong to the later projects
// and this engine must not interpret them (design doc 1.4).
class LoggingSink final : public IApplicationSink {
public:
    explicit LoggingSink(bool verbose) : verbose_(verbose) {}
    bool deliver(uint64_t seq, std::string_view msg_type, std::string_view payload) override {
        ++count_;
        if (verbose_)
            std::printf("app in  seq=%llu type=%.*s bytes=%zu\n",
                        static_cast<unsigned long long>(seq), static_cast<int>(msg_type.size()),
                        msg_type.data(), payload.size());
        return true;
    }
    uint64_t count() const { return count_; }

private:
    bool verbose_;
    uint64_t count_ = 0;
};

void usage() {
    std::printf(
        "usage: fix_client --config <file> [options]\n"
        "\n"
        "  --duration-s <n>     stop after n seconds, 0 means run until interrupted\n"
        "  --send-every-ms <n>  submit a test application message every n ms\n"
        "  --app-type <type>    MsgType for those messages, default U1\n"
        "  --metrics-out <file> write the metric snapshot here on exit\n"
        "  --audit-out <file>   write the audit trail here on exit\n"
        "  --verbose            print each delivered application message\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string metrics_out;
    std::string audit_out;
    std::string app_type = "U1";
    long long duration_s = 0;
    long long send_every_ms = 0;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "--config" && has_next) config_path = argv[++i];
        else if (a == "--duration-s" && has_next) duration_s = std::atoll(argv[++i]);
        else if (a == "--send-every-ms" && has_next) send_every_ms = std::atoll(argv[++i]);
        else if (a == "--app-type" && has_next) app_type = argv[++i];
        else if (a == "--metrics-out" && has_next) metrics_out = argv[++i];
        else if (a == "--audit-out" && has_next) audit_out = argv[++i];
        else if (a == "--verbose") verbose = true;
        else {
            usage();
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

    fp::arm_from_env();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SystemClock clock;
    SqliteSessionStore::Options o;
    o.path = cfg.database_path;
    o.key.begin_string = cfg.begin_string;
    o.key.sender_comp_id = cfg.sender_comp_id;
    o.key.target_comp_id = cfg.target_comp_id;
    o.key.qualifier = cfg.qualifier;
    o.busy_timeout_ms = cfg.store_busy_timeout_ms;
    o.journal_mode = cfg.sqlite_journal_mode;
    o.synchronous = cfg.sqlite_synchronous;
    o.create_if_missing = cfg.create_database_if_missing;
    o.clock = &clock;

    std::unique_ptr<SqliteSessionStore> store;
    std::string why;
    const StoreError se = SqliteSessionStore::open(o, &store, &why);
    if (se != StoreError::None) {
        std::fprintf(stderr, "store: %s (%s)\n", why.c_str(), std::string(to_string(se)).c_str());
        return 3;
    }

    RealSocketOps ops;
    TcpConnector connector(cfg, clock, ops);
    LoggingSink sink(verbose);
    Metrics metrics;
    AuditLog audit;

    EngineDeps d;
    d.cfg = &cfg;
    d.store = store.get();
    d.clock = &clock;
    d.transport = &connector;
    d.controller = &connector;
    d.sink = &sink;
    d.metrics = &metrics;
    d.audit = &audit;
    SessionEngine engine(d);
    connector.bind_engine(&engine);

    if (engine.start() != StoreError::None) {
        std::fprintf(stderr, "engine refused to start: %s -- %s\n",
                     std::string(to_string(engine.fault_reason())).c_str(),
                     engine.fault_detail().c_str());
        return 4;
    }
    std::printf("session %s starting: epoch %llu next_in %llu next_out %llu\n",
                cfg.session_id().c_str(), static_cast<unsigned long long>(engine.epoch()),
                static_cast<unsigned long long>(engine.next_in()),
                static_cast<unsigned long long>(engine.next_out()));

    const auto started = std::chrono::steady_clock::now();
    auto next_send = started;
    bool stopping = false;

    while (true) {
        const bool alive = connector.poll_once(50);
        const auto now = std::chrono::steady_clock::now();
        const bool expired =
            duration_s > 0 && now - started >= std::chrono::seconds(duration_s);

        if (!stopping && (g_stop.load() || expired)) {
            std::printf("stopping gracefully\n");
            engine.request_graceful_stop();
            stopping = true;
        }
        if (!alive && (stopping || engine.state() == SessionState::Stopped)) break;
        if (engine.state() == SessionState::Faulted) {
            std::fprintf(stderr, "FAULTED: %s -- %s\n",
                         std::string(to_string(engine.fault_reason())).c_str(),
                         engine.fault_detail().c_str());
            break;
        }

        if (send_every_ms > 0 && now >= next_send && engine.state() == SessionState::Active) {
            const SubmitResult r = engine.submit_app_message(
                app_type, {{20001, "lab-" + std::to_string(engine.next_out())}});
            if (r != SubmitResult::Accepted && verbose)
                std::printf("submit rejected: %s\n", std::string(to_string(r)).c_str());
            next_send = now + std::chrono::milliseconds(send_every_ms);
        }
    }

    std::printf("final state %s epoch %llu next_in %llu next_out %llu delivered %llu\n",
                std::string(to_string(engine.state())).c_str(),
                static_cast<unsigned long long>(engine.epoch()),
                static_cast<unsigned long long>(engine.next_in()),
                static_cast<unsigned long long>(engine.next_out()),
                static_cast<unsigned long long>(sink.count()));
    if (engine.skipped_unrecovered() > 0)
        std::printf("WARNING: %llu sequences were skipped by a GapFill and never recovered\n",
                    static_cast<unsigned long long>(engine.skipped_unrecovered()));

    if (!metrics_out.empty()) {
        std::ofstream f(metrics_out, std::ios::binary);
        f << metrics.render();
    }
    if (!audit_out.empty()) {
        std::ofstream f(audit_out, std::ios::binary);
        for (const std::string& line : audit.lines()) f << line << "\n";
    }
    if (engine.state() != SessionState::Faulted) store->mark_clean_shutdown();
    return engine.state() == SessionState::Faulted ? 5 : 0;
}
