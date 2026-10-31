// Scenario runner: executes a scripted timeline against a clean simulator and
// prints a reproducible report.
//
// Reference: design doc 14.2 (scenario DSL) and 18.5 (commands).
//
// The report carries the seed, the config hash, the scenario hash and the
// canonical trace hash, because a run without those cannot be cited as evidence
// (M-12, M-20). Two runs of the same scenario against a clean store must print
// the same trace hash; --repeat asserts exactly that in one invocation.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "lp/config.hpp"
#include "lp/failpoint.hpp"
#include "lp/order_actor.hpp"
#include "lp/scenario.hpp"
#include "lp/session.hpp"
#include "lp/sqlite_store.hpp"
#include "lp/validator.hpp"

using namespace lpsim;

namespace {

void usage() {
    std::printf(
        "usage: scenario_runner --scenario <file> [--config <file>] [--db <path>]\n"
        "                       [--repeat <n>] [--trace]\n"
        "\n"
        "Runs the scenario against a clean store. --repeat n runs it n times\n"
        "from scratch and fails unless every run produces the same canonical\n"
        "trace hash.\n");
}

struct Instance {
    VirtualClock clock;
    SqliteStore store;
    QuoteBook book;
    Scheduler sched;
    RecordingTransport transport;
    std::unique_ptr<OrderActor> actor;
    std::unique_ptr<AcceptorSession> session;

    Instance() : sched(100000) {}
};

bool build_instance(Instance* inst, const SimulatorConfig& cfg, const ScenarioSpec& spec,
                    std::string* why) {
    if (inst->store.open(cfg.store) != StoreError::None) {
        *why = "cannot open the store at " + cfg.store.path;
        return false;
    }
    ActorConfig ac = cfg.actor;
    // The scenario owns these three: a timeline that needs a pending cancel
    // must be able to ask for one without editing the simulator config.
    ac.fill_trigger = spec.fill_trigger;
    ac.emit_pending_cancel = spec.emit_pending_cancel;
    ac.emit_pending_replace = spec.emit_pending_replace;

    inst->actor =
        std::make_unique<OrderActor>(ac, inst->store, inst->book, inst->sched, inst->clock);
    if (!inst->actor->recover()) {
        *why = "recovery refused to continue: " + inst->actor->halt_detail();
        return false;
    }
    inst->session = std::make_unique<AcceptorSession>(cfg.session, inst->store, *inst->actor,
                                                      inst->clock, inst->transport);
    if (!inst->session->recover()) {
        *why = "session recovery refused to continue: " + inst->session->halt_detail();
        return false;
    }
    return true;
}

SimulatorConfig default_config() {
    SimulatorConfig cfg;
    cfg.instance_id = "lp-scenario";
    cfg.session.session_id = cfg.instance_id;
    cfg.actor.session_id = cfg.instance_id;
    cfg.store.session_id = cfg.instance_id;
    cfg.session.sender_comp_id = "SIMLP01";
    cfg.session.target_comp_id = "TRADER01";
    cfg.store.sender_comp_id = "SIMLP01";
    cfg.store.target_comp_id = "TRADER01";
    cfg.actor.firm = "TRADER01";
    // A placeholder that passes validation. Each run replaces it with a path in
    // its own clean directory, so this value is never actually opened.
    cfg.store.path = "./var/lp-scenario.db";

    SymbolProfile eur;
    eur.symbol = "EURUSD";
    eur.price.scale = 5;
    eur.price.tick_step = 1;
    eur.lot_step = 1000;
    eur.quote_stale_after_ms = 2000;
    cfg.actor.symbols.push_back(eur);
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenario_path;
    std::string config_path;
    std::string db_path;
    int repeat = 1;
    bool show_trace = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) scenario_path = argv[++i];
        else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];
        else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeat = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--trace") == 0) show_trace = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (scenario_path.empty() || repeat < 1) {
        usage();
        return 2;
    }

    fp::arm_from_env();

    ConfLimits limits;
    ConfError err;

    // --- scenario ----------------------------------------------------------
    std::string scenario_text;
    if (!read_conf_file(scenario_path, limits, &scenario_text, &err)) {
        std::fprintf(stderr, "scenario: %s\n", err.message.c_str());
        return 2;
    }
    ConfNode scenario_doc;
    if (!parse_conf(scenario_text, limits, &scenario_doc, &err)) {
        std::fprintf(stderr, "scenario line %d: %s\n", err.line, err.message.c_str());
        return 2;
    }
    ScenarioSpec spec;
    if (!parse_scenario(scenario_doc, &spec, &err)) {
        std::fprintf(stderr, "scenario: %s\n", err.message.c_str());
        return 2;
    }

    // --- configuration -----------------------------------------------------
    SimulatorConfig cfg = default_config();
    std::string config_text;
    if (!config_path.empty()) {
        if (!read_conf_file(config_path, limits, &config_text, &err)) {
            std::fprintf(stderr, "config: %s\n", err.message.c_str());
            return 2;
        }
        ConfNode config_doc;
        if (!parse_conf(config_text, limits, &config_doc, &err)) {
            std::fprintf(stderr, "config line %d: %s\n", err.line, err.message.c_str());
            return 2;
        }
        if (!build_simulator_config(config_doc, &cfg, &err)) {
            std::fprintf(stderr, "config: %s\n", err.message.c_str());
            return 2;
        }
    } else if (!validate_simulator_config(cfg, &err)) {
        std::fprintf(stderr, "built-in config is invalid: %s\n", err.message.c_str());
        return 2;
    }

    const std::uint64_t config_hash = canonical_hash64(config_text);
    const std::uint64_t scenario_hash = canonical_hash64(scenario_text);

    std::printf("scenario_id   %s\n", spec.scenario_id.c_str());
    std::printf("seed          %llu\n", static_cast<unsigned long long>(spec.seed));
    std::printf("clock         %s\n", std::string(to_string(spec.clock)).c_str());
    std::printf("fill_trigger  %s\n", std::string(to_string(spec.fill_trigger)).c_str());
    std::printf("config_hash   %016llx\n", static_cast<unsigned long long>(config_hash));
    std::printf("scenario_hash %016llx\n", static_cast<unsigned long long>(scenario_hash));
    std::printf("steps         %zu\n", spec.steps.size());

    std::uint64_t first_hash = 0;
    int exit_code = 0;

    for (int run = 1; run <= repeat; ++run) {
        // Every run starts from a clean store in its own directory, so run n
        // cannot inherit state from run n-1 (M-04).
        const std::string dir =
            db_path.empty()
                ? (std::filesystem::temp_directory_path() /
                   ("lpsim-scenario-" + spec.scenario_id + "-" + std::to_string(run)))
                      .string()
                : db_path + "-" + std::to_string(run);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        SimulatorConfig run_cfg = cfg;
        run_cfg.store.path = (std::filesystem::path(dir) / "lp.db").string();

        Instance inst;
        std::string why;
        if (!build_instance(&inst, run_cfg, spec, &why)) {
            std::fprintf(stderr, "run %d: %s\n", run, why.c_str());
            return 4;
        }

        ScenarioRunner runner(spec, inst.clock, *inst.actor, *inst.session, inst.book,
                              run_cfg.actor.symbols, inst.transport);
        const ScenarioResult res = runner.run();

        std::printf("\nrun %d/%d  %s\n", run, repeat, res.ok ? "PASS" : "FAIL");
        std::printf("  trace_hash  %016llx\n",
                    static_cast<unsigned long long>(res.trace_hash));
        std::printf("  fills       %zu\n", res.fills);
        std::printf("  refusals    %zu\n", res.refusals);
        std::printf("  frames out  %zu\n", res.reports);
        if (!res.ok) {
            std::printf("  failure     %s\n", res.failure.c_str());
            exit_code = 1;
        }
        if (show_trace) {
            for (const std::string& line : res.trace) std::printf("    %s\n", line.c_str());
        }

        if (run == 1) {
            first_hash = res.trace_hash;
        } else if (res.trace_hash != first_hash) {
            // H-11 and M-05: the same seed and fixture must produce the same
            // canonical event log, or the run is not reproducible evidence.
            std::printf("  NOT REPRODUCIBLE: trace hash differs from run 1 (%016llx)\n",
                        static_cast<unsigned long long>(first_hash));
            exit_code = 1;
        }

        inst.session.reset();
        inst.actor.reset();
        inst.store.close();
        if (db_path.empty()) std::filesystem::remove_all(dir, ec);
    }

    std::printf("\n%s\n", exit_code == 0 ? "SCENARIO PASS" : "SCENARIO FAIL");
    return exit_code;
}
