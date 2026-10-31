// Scenario DSL: a scripted, reproducible timeline.
//
// Reference: design doc 14.2 (example scenario) and 11.3 (deterministic
// scheduling).
//
// A scenario is the executable form of a playbook. What it controls is WHEN
// things are attempted; what it cannot do is make an invalid thing valid. An
// inject_fill still goes through the full matcher gate, so a scenario that asks
// for an overfill gets a refusal recorded, not an overfill (design doc 14.2).
//
// Reproducibility is the point (H-11, M-05, M-12): the run manifest carries the
// seed, the config hash, the fixture hash and the canonical trace hash, and two
// runs of the same scenario against a clean store must produce the same trace
// hash. A scenario that does not pin its seed is rejected rather than run with
// an arbitrary one.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "lp/config.hpp"
#include "lp/order_actor.hpp"
#include "lp/session.hpp"

namespace lpsim {

enum class ScenarioAction {
    ExpectLogon,
    ExpectNew,
    ExpectCancel,
    ExpectReplace,
    InjectQuote,
    InjectFill,
    FinalizeCancel,
    FinalizeReplace,
    Expire,
    Disconnect,
    Reconnect,
    ResendRequest,
    Advance,
    Assert,
};

std::string_view to_string(ScenarioAction a);

struct ScenarioStep {
    std::int64_t at_ms = 0;
    ScenarioAction action = ScenarioAction::Advance;

    // Order and request identity.
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string order_ref;  // the ClOrdID a fill or finalisation targets

    std::string symbol = "EURUSD";
    std::string side = "BUY";
    std::string qty;
    std::string price;

    // Quote payload.
    std::string bid;
    std::string ask;
    std::string bid_available;
    std::string ask_available;
    std::uint64_t quote_version = 0;
    std::int64_t ttl_ms = 2000;

    // Resend range.
    std::uint64_t begin_seq = 0;
    std::uint64_t end_seq = 0;

    // Whether this step is expected to be refused. A step marked expect_reject
    // that succeeds is a FAILURE: design doc 14.2 requires a deliberately
    // invalid injection to assert the refusal rather than quietly pass.
    bool expect_reject = false;

    std::string assertion;  // for ScenarioAction::Assert
    int source_line = 0;
};

struct ScenarioSpec {
    std::string scenario_id;
    std::uint64_t seed = 0;
    bool seed_present = false;
    ClockMode clock = ClockMode::Virtual;
    FillTrigger fill_trigger = FillTrigger::Scripted;
    bool emit_pending_cancel = false;
    bool emit_pending_replace = false;

    std::vector<ScenarioStep> steps;
    std::vector<std::string> assertions;

    // Bounds applied while parsing, so a crafted scenario cannot run away
    // (L-09, H-18).
    std::size_t max_steps = 10000;
};

bool parse_scenario(const ConfNode& doc, ScenarioSpec* out, ConfError* err);

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

struct ScenarioResult {
    bool ok = false;
    std::string failure;              // empty when ok
    std::vector<std::string> trace;   // canonical event log
    std::uint64_t trace_hash = 0;
    std::size_t fills = 0;
    std::size_t reports = 0;
    std::size_t refusals = 0;         // steps refused as expected
    std::map<std::string, std::string> order_ids;  // ClOrdID -> OrderID
};

// A transport that records what the session wrote. The runner drives the
// session in-process: it does not open a socket, because a scenario is about
// event ordering and a real socket would add timing the virtual clock is there
// to remove.
class RecordingTransport final : public ITransport {
public:
    std::size_t write(std::string_view bytes) override {
        if (!connected_) return 0;
        out_.append(bytes);
        return bytes.size();
    }
    void close() override { connected_ = false; }
    bool connected() const override { return connected_; }
    void reopen() { connected_ = true; }

    const std::string& bytes() const { return out_; }
    void clear() { out_.clear(); }
    std::vector<std::string> frames() const;

private:
    std::string out_;
    bool connected_ = true;
};

// Runs a parsed scenario against a simulator instance the caller owns, so a
// test can inspect the store afterwards and the runner application can simply
// report. Nothing here bypasses the actor: every order arrives as FIX bytes
// through the session, and every fill goes through the matcher gate.
class ScenarioRunner {
public:
    ScenarioRunner(const ScenarioSpec& spec, VirtualClock& clock, OrderActor& actor,
                   AcceptorSession& session, QuoteBook& book,
                   std::vector<SymbolProfile> symbols, RecordingTransport& transport);

    ScenarioResult run();

private:
    void note(std::string line);
    std::string build_logon();
    std::string build_new(const ScenarioStep& s);
    std::string build_cancel(const ScenarioStep& s);
    std::string build_replace(const ScenarioStep& s);
    std::string build_resend(const ScenarioStep& s);
    const SymbolProfile* symbol_of(std::string_view s) const;
    bool resolve_order(const std::string& ref, std::string* order_id, std::string* why);

    const ScenarioSpec& spec_;
    VirtualClock& clock_;
    OrderActor& actor_;
    AcceptorSession& session_;
    QuoteBook& book_;
    std::vector<SymbolProfile> symbols_;
    RecordingTransport& transport_;

    std::uint64_t client_seq_ = 1;
    std::uint64_t generation_ = 0;
    int event_counter_ = 0;
    std::map<std::string, std::string> order_ids_;  // ClOrdID -> OrderID
    std::vector<std::string> trace_;
};

// Evaluate one assertion against live state, for example
// "CumQty(O1) == 60000" or "OrdStatus(O1) == CANCELED".
// Returns false with *why populated when the assertion does not hold, and also
// when it cannot be parsed: an assertion that silently evaluates to true
// because it was misspelled is worse than no assertion.
bool evaluate_assertion(std::string_view text, const OrderActor& actor,
                        const std::map<std::string, std::string>& order_ids,
                        std::string* why);

// Canonical trace hash over the logical event log. Session timings and
// retransmission counts are deliberately excluded: design doc PB-16 says the
// wire may differ across a crash run while the logical ledger may not.
std::uint64_t trace_hash(const std::vector<std::string>& trace);

}  // namespace lpsim
