// Playbooks: the scenario files in scenarios/, executed and checked.
//
// Matrix coverage: PB-06 to PB-09, PB-18, H-01, H-11, M-03, M-05, M-12, M-19,
// M-20, L-04 to L-07, L-09 and L-13.
//
// Two things make this suite worth having beyond the unit suites. First, the
// scenario files ARE the playbooks from design doc 17, so running them here
// means the documented procedures are executable rather than prose. Second,
// every scenario runs twice from a clean store and the canonical trace hashes
// are compared, which is the determinism requirement (H-11, M-05) applied to
// the whole stack rather than to the scheduler alone.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lp/config.hpp"
#include "lp/metrics.hpp"
#include "lp/order_actor.hpp"
#include "lp/scenario.hpp"
#include "lp/session.hpp"
#include "lp/sqlite_store.hpp"
#include "../support/tiny_test.hpp"
#include "../support/tmpdir.hpp"

using namespace lpsim;

namespace {

// Where the scenario files live. The suites run with the project root as the
// working directory (CMake sets WORKING_DIRECTORY and scripts/run_tests.sh does
// the same), so a relative path is correct and a missing file is a real failure
// rather than a path problem to paper over.
const char* kScenarioDir = "scenarios";

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

std::vector<SymbolProfile> symbols() {
    SymbolProfile eur;
    eur.symbol = "EURUSD";
    eur.price.scale = 5;
    eur.price.tick_step = 1;
    eur.lot_step = 1000;
    eur.quote_stale_after_ms = 2000;
    SymbolProfile gbp = eur;
    gbp.symbol = "GBPUSD";
    return {eur, gbp};
}

ScenarioSpec load(const std::string& name) {
    const std::string path = std::string(kScenarioDir) + "/" + name;
    ConfLimits limits;
    ConfError err;
    std::string text;
    if (!read_conf_file(path, limits, &text, &err))
        TT_FAIL("cannot read " + path + ": " + err.message);
    ConfNode doc;
    if (!parse_conf(text, limits, &doc, &err))
        TT_FAIL("cannot parse " + path + " at line " + std::to_string(err.line) + ": " +
                err.message);
    ScenarioSpec spec;
    if (!parse_scenario(doc, &spec, &err))
        TT_FAIL("invalid scenario " + path + ": " + err.message);
    return spec;
}

// Run one scenario against a clean store and return the result.
ScenarioResult run(const ScenarioSpec& spec, const std::string& tag) {
    tt::TmpDir dir(tag);
    Instance inst;

    StoreConfig sc;
    sc.path = dir.db();
    sc.session_id = "LP-SESSION-1";
    sc.sender_comp_id = "SIMLP01";
    sc.target_comp_id = "TRADER01";
    if (inst.store.open(sc) != StoreError::None) TT_FAIL("cannot open the store");

    ActorConfig ac;
    ac.firm = "TRADER01";
    ac.session_id = sc.session_id;
    ac.symbols = symbols();
    ac.fill_trigger = spec.fill_trigger;
    ac.emit_pending_cancel = spec.emit_pending_cancel;
    ac.emit_pending_replace = spec.emit_pending_replace;
    inst.actor =
        std::make_unique<OrderActor>(ac, inst.store, inst.book, inst.sched, inst.clock);
    if (!inst.actor->recover()) TT_FAIL("recovery refused: " + inst.actor->halt_detail());

    SessionConfig secfg;
    secfg.session_id = sc.session_id;
    secfg.sender_comp_id = sc.sender_comp_id;
    secfg.target_comp_id = sc.target_comp_id;
    inst.session = std::make_unique<AcceptorSession>(secfg, inst.store, *inst.actor,
                                                     inst.clock, inst.transport);
    if (!inst.session->recover()) TT_FAIL("session recovery refused");

    ScenarioRunner runner(spec, inst.clock, *inst.actor, *inst.session, inst.book,
                          ac.symbols, inst.transport);
    ScenarioResult res = runner.run();

    inst.session.reset();
    inst.actor.reset();
    inst.store.close();
    return res;
}

// Run twice from clean stores and require identical canonical traces.
ScenarioResult run_twice(const std::string& file, const std::string& tag) {
    const ScenarioSpec spec = load(file);
    const ScenarioResult a = run(spec, tag + "-a");
    CHECK_MSG(a.ok, file + ": " + a.failure);
    const ScenarioResult b = run(spec, tag + "-b");
    CHECK_MSG(b.ok, file + " second run: " + b.failure);
    // H-11, M-05: the same seed and fixture must give the same logical log.
    CHECK_MSG(a.trace_hash == b.trace_hash,
              file + ": trace hash differs between identical runs");
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// The playbooks
// ---------------------------------------------------------------------------

SCENARIO("E-03b", "P0", "happy path: New, partial fill, final fill, exact AvgPx") {
    const ScenarioResult r = run_twice("happy_path.yaml", "pb-happy");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
    CHECK_EQ(r.refusals, static_cast<std::size_t>(0));
}

SCENARIO("PB-06", "P0", "cancel race, fill wins: CumQty 60000 and then Canceled") {
    // The worked example from design doc 14.2. The 20000 fill is due at 32 and
    // the cancel finalises at 35, so the fill wins.
    const ScenarioResult r = run_twice("cancel_fill_race.yaml", "pb-06a");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
}

SCENARIO("PB-06b", "P0", "cancel race, cancel wins: CumQty 40000 and the fill invalidated") {
    // The SAME timeline with the finalisation moved earlier. Running both is
    // what makes the outcome a decision rather than an accident (design doc
    // 11.3: two explicitly named fixtures).
    const ScenarioResult r = run_twice("cancel_wins_race.yaml", "pb-06b");
    CHECK_EQ(r.fills, static_cast<std::size_t>(1));
    // The late fill was refused, and the refusal was asserted rather than
    // tolerated.
    CHECK_EQ(r.refusals, static_cast<std::size_t>(1));
}

SCENARIO("PB-07", "P0", "the final fill wins and the cancel is refused as too late") {
    const ScenarioResult r = run_twice("full_fill_cancel_too_late.yaml", "pb-07");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
}

SCENARIO("PB-08", "P0", "a fill during a pending replace uses the old accepted parameters") {
    const ScenarioResult r = run_twice("replace_partial.yaml", "pb-08");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
}

SCENARIO("PB-09", "P0", "two orders competing for one slice never oversubscribe it") {
    const ScenarioResult r = run_twice("two_orders_one_slice.yaml", "pb-09");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
    CHECK_EQ(r.refusals, static_cast<std::size_t>(1));
}

SCENARIO("PB-18", "P0", "the acceptance transcript from the design doc runs end to end") {
    const ScenarioResult r = run_twice("demo_acceptance.yaml", "pb-18");
    CHECK_EQ(r.fills, static_cast<std::size_t>(2));
}

SCENARIO("H-01b", "P0", "swapping only the event order changes the outcome") {
    // The two race files differ in the order of two events and in nothing
    // else. If the outcome did NOT differ, the scheduler would not be deciding
    // the race and both fixtures would be testing the same thing.
    const ScenarioResult fill_wins = run(load("cancel_fill_race.yaml"), "h01-fill");
    const ScenarioResult cancel_wins = run(load("cancel_wins_race.yaml"), "h01-cancel");
    CHECK(fill_wins.ok);
    CHECK(cancel_wins.ok);
    CHECK_NE(fill_wins.fills, cancel_wins.fills);
    CHECK_NE(fill_wins.trace_hash, cancel_wins.trace_hash);
}

SCENARIO("M-03", "P0", "every scenario file in the directory parses and runs") {
    // A playbook that exists but is never executed is not evidence of
    // anything, so the suite walks the directory rather than naming files one
    // by one.
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(kScenarioDir, ec)) {
        if (entry.path().extension() == ".yaml")
            files.push_back(entry.path().filename().string());
    }
    CHECK_MSG(!ec, "cannot list the scenarios directory");
    CHECK(files.size() >= 7);

    for (const std::string& f : files) {
        const ScenarioSpec spec = load(f);
        // Every scenario must pin a seed, or its result is not reproducible.
        CHECK_MSG(spec.seed_present, f + " has no seed");
        CHECK_MSG(!spec.steps.empty(), f + " has no steps");
        const ScenarioResult r = run(spec, "walk-" + spec.scenario_id);
        CHECK_MSG(r.ok, f + ": " + r.failure);
    }
}

// ---------------------------------------------------------------------------
// Scenario and configuration validation: untrusted input
// ---------------------------------------------------------------------------

SCENARIO("M-12", "P1", "a scenario without a seed is refused") {
    const char* text =
        "scenario_id: no-seed\n"
        "clock: virtual\n"
        "events:\n"
        "  - {at_ms: 0, action: expect_logon}\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(parse_conf(text, limits, &doc, &err));
    ScenarioSpec spec;
    CHECK(!parse_scenario(doc, &spec, &err));
    CHECK(err.message.find("seed") != std::string::npos);
}

SCENARIO("L-09", "P0", "a scenario that nests beyond the bound is refused") {
    // An adversarial file cannot make the parser recurse without limit.
    std::string text = "scenario_id: deep\nseed: 1\n";
    std::string indent;
    for (int i = 0; i < 40; ++i) {
        indent += "  ";
        text += indent + "k" + std::to_string(i) + ":\n";
    }
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(!parse_conf(text, limits, &doc, &err));
    CHECK(!err.ok);
}

SCENARIO("L-09b", "P0", "an oversized document is refused before it is parsed") {
    ConfLimits limits;
    limits.max_file_bytes = 1024;
    const std::string huge(4096, 'x');
    ConfError err;
    ConfNode doc;
    CHECK(!parse_conf(huge, limits, &doc, &err));
    CHECK(err.message.find("size bound") != std::string::npos);
}

SCENARIO("L-09c", "P0", "a duplicate key is refused rather than silently resolved") {
    const char* text =
        "simulator:\n"
        "  heartbeat_seconds: 30\n"
        "  heartbeat_seconds: 1\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(!parse_conf(text, limits, &doc, &err));
    CHECK(err.message.find("duplicate key") != std::string::npos);
}

SCENARIO("M-20", "P0", "an unknown configuration key is an error, not something ignored") {
    // A silently ignored key means the operator believes a limit is set when it
    // is not.
    const char* text =
        "simulator:\n"
        "  sender_comp_id: SIMLP01\n"
        "  target_comp_id: TRADER01\n"
        "  max_outstandingg_orders: 10\n"
        "  store: {path: ./var/x.db}\n"
        "  symbols:\n"
        "    EURUSD: {tick_size: '0.00001', lot_step_base_units: 1000}\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(parse_conf(text, limits, &doc, &err));
    SimulatorConfig cfg;
    CHECK(!build_simulator_config(doc, &cfg, &err));
    CHECK(err.message.find("unknown key") != std::string::npos);
}

SCENARIO("L-06", "P0", "adversarial mode on a non-loopback bind fails startup") {
    const char* text =
        "simulator:\n"
        "  mode: adversarial\n"
        "  listen: 0.0.0.0:9898\n"
        "  sender_comp_id: SIMLP01\n"
        "  target_comp_id: TRADER01\n"
        "  store: {path: ./var/x.db}\n"
        "  symbols:\n"
        "    EURUSD: {tick_size: '0.00001', lot_step_base_units: 1000}\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(parse_conf(text, limits, &doc, &err));
    SimulatorConfig cfg;
    CHECK(!build_simulator_config(doc, &cfg, &err));
    CHECK(err.message.find("loopback") != std::string::npos);
}

SCENARIO("L-05", "P0", "wire faults on a non-loopback bind fail startup") {
    const char* text =
        "simulator:\n"
        "  listen: 192.168.1.10:9898\n"
        "  allow_unsafe_wire_faults: true\n"
        "  sender_comp_id: SIMLP01\n"
        "  target_comp_id: TRADER01\n"
        "  store: {path: ./var/x.db}\n"
        "  symbols:\n"
        "    EURUSD: {tick_size: '0.00001', lot_step_base_units: 1000}\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(parse_conf(text, limits, &doc, &err));
    SimulatorConfig cfg;
    CHECK(!build_simulator_config(doc, &cfg, &err));
    CHECK(err.message.find("loopback") != std::string::npos);

    // The same capability on loopback is permitted.
    const char* ok_text =
        "simulator:\n"
        "  listen: 127.0.0.1:9898\n"
        "  allow_unsafe_wire_faults: true\n"
        "  sender_comp_id: SIMLP01\n"
        "  target_comp_id: TRADER01\n"
        "  store: {path: ./var/x.db}\n"
        "  symbols:\n"
        "    EURUSD: {tick_size: '0.00001', lot_step_base_units: 1000}\n";
    ConfNode ok_doc;
    CHECK(parse_conf(ok_text, limits, &ok_doc, &err));
    SimulatorConfig ok_cfg;
    CHECK_MSG(build_simulator_config(ok_doc, &ok_cfg, &err), err.message);
}

SCENARIO("M-19b", "P1", "a TimeInForce the profile does not implement is refused in config") {
    // Accepting IOC in configuration without implementing it would be a false
    // claim of support (M-19).
    const char* text =
        "simulator:\n"
        "  sender_comp_id: SIMLP01\n"
        "  target_comp_id: TRADER01\n"
        "  store: {path: ./var/x.db}\n"
        "  symbols:\n"
        "    EURUSD:\n"
        "      tick_size: '0.00001'\n"
        "      lot_step_base_units: 1000\n"
        "      allowed_tif: [DAY, IOC]\n";
    ConfLimits limits;
    ConfError err;
    ConfNode doc;
    CHECK(parse_conf(text, limits, &doc, &err));
    SimulatorConfig cfg;
    CHECK(!build_simulator_config(doc, &cfg, &err));
    CHECK(err.message.find("not implemented") != std::string::npos);
}

SCENARIO("L-04", "P0", "a store path that escapes its directory is refused") {
    std::string why;
    CHECK(!safe_store_path("../../etc/passwd", &why));
    CHECK(!safe_store_path("/dev/sda", &why));
    CHECK(!safe_store_path("", &why));
    CHECK(safe_store_path("./var/lp.db", &why));
}

SCENARIO("A-13b", "P0", "a non-positive heartbeat or a blank CompID fails validation") {
    SimulatorConfig cfg;
    cfg.session.sender_comp_id = "SIMLP01";
    cfg.session.target_comp_id = "TRADER01";
    cfg.store.path = "./var/x.db";
    cfg.actor.symbols = symbols();
    ConfError err;
    CHECK_MSG(validate_simulator_config(cfg, &err), err.message);

    SimulatorConfig blank = cfg;
    blank.session.sender_comp_id.clear();
    CHECK(!validate_simulator_config(blank, &err));

    SimulatorConfig same = cfg;
    same.session.target_comp_id = same.session.sender_comp_id;
    CHECK(!validate_simulator_config(same, &err));

    SimulatorConfig hb = cfg;
    hb.session.heartbeat_seconds = 0;
    CHECK(!validate_simulator_config(hb, &err));

    SimulatorConfig caps = cfg;
    caps.actor.max_live_orders = 0;
    CHECK(!validate_simulator_config(caps, &err));

    SimulatorConfig creds = cfg;
    creds.session.require_credentials = true;
    creds.session.password.clear();
    CHECK(!validate_simulator_config(creds, &err));
}

SCENARIO("L-07", "P0", "a password never survives into a log line or an artifact") {
    // The audit layer redacts by tag rather than by trying to recognise a
    // secret, so a value that happens to look ordinary is still removed.
    const std::string frame =
        std::string("35=A|49=TRADER01|56=SIMLP01|553=trader|554=s3cr3t-pass|108=30|");
    const std::string clean = redact(frame);
    CHECK(clean.find("s3cr3t-pass") == std::string::npos);
    CHECK(clean.find("554=[REDACTED]") != std::string::npos);
    CHECK(clean.find("553=[REDACTED]") != std::string::npos);
    // Everything else survives, so the record stays useful.
    CHECK(clean.find("49=TRADER01") != std::string::npos);
    CHECK(clean.find("108=30") != std::string::npos);

    // And an audit record built around a secret is redacted too.
    AuditRecord rec;
    rec.session_id = "LP-SESSION-1";
    rec.decision_reason = "554=s3cr3t-pass";
    const std::string rendered = rec.render();
    CHECK(rendered.find("s3cr3t-pass") == std::string::npos);
}

SCENARIO("L-13", "P1", "metric labels are drawn from closed sets, not from order ids") {
    Metrics m;
    // A hundred distinct orders rejected for two distinct reasons must produce
    // two series, not a hundred.
    for (int i = 0; i < 100; ++i) {
        m.count_order_reject(BizReject::UnknownSymbol);
        m.count_order_reject(BizReject::BadQuantity);
    }
    CHECK_EQ(m.orders_rejected_total.size(), static_cast<std::size_t>(2));
    const std::string text = m.render();
    CHECK(text.find("reason=\"UnknownSymbol\"") != std::string::npos);
    CHECK(text.find("reason=\"BadQuantity\"") != std::string::npos);
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "playbooks"); }
