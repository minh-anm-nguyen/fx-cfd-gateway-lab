// order_trace_replay -- runs a scenario file through the state machine and
// prints the canonical result, or verifies the file's own assertions.
//
// The design doc sketches this in YAML (section 20.4). The format here is a
// line-oriented equivalent so the tool has no parser dependency; the fields
// are the same ones. One line, one event, in commit order:
//
//   policy       <name>                     (strict_v1 only, for now)
//   new          <clordid> qty=<n> px=<p> [sym=<s>] [side=1|2] [acct=<a>]
//   dispatch     <clordid>
//   cancel       <clordid> orig=<clordid>
//   replace      <clordid> orig=<clordid> [qty=<n>] [px=<p>]
//   timeout      <clordid>
//   disconnect   <clordid>
//   reconcile    <clordid>
//   resolve      <clordid> status=<WORKING|CANCELED|FILLED|REJECTED> [evidence] [dual]
//   ack          <clordid> [oid=<id>]
//   fill         <execid> cl=<clordid> qty=<n> px=<p> [cum=<n>] [leaves=<n>] [status=<s>]
//   report       <kind> <clordid> [exec=<id>] [cum=<n>] [leaves=<n>] [status=<s>] [qty=<n>] [px=<p>]
//   crash        <W00..W15>                 arms a faultpoint for the next line
//   recover
//   assert       lifecycle=<L> cum=<n> leaves=<n> fills=<n> versions=<n> violations=0 ...
//
// Exit status is zero when every assertion held.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "order_core/decimal.hpp"
#include "order_core/engine.hpp"

using namespace order_core;

namespace {

struct Fields {
    std::map<std::string, std::string> kv;
    std::vector<std::string> positional;

    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = "") const {
        const auto it = kv.find(key);
        return it == kv.end() ? fallback : it->second;
    }
    [[nodiscard]] bool has(const std::string& key) const { return kv.count(key) != 0; }
    [[nodiscard]] std::string at(size_t i, const std::string& fallback = "") const {
        return i < positional.size() ? positional[i] : fallback;
    }
};

Fields parse_line(const std::string& line) {
    Fields f;
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos) {
            f.positional.push_back(token);
        } else {
            f.kv[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
    return f;
}

CrashWindow window_from_name(const std::string& name) {
    static const std::map<std::string, CrashWindow> kByName = {
        {"W00", CrashWindow::W00_BeforeParse},
        {"W01", CrashWindow::W01_AfterParseBeforeCorrelation},
        {"W02", CrashWindow::W02_AfterCorrelationBeforeDedup},
        {"W03", CrashWindow::W03_AfterDedupBeforeReduce},
        {"W04", CrashWindow::W04_AfterReduceBeforeTxn},
        {"W05", CrashWindow::W05_InTxnBeforeJournalInsert},
        {"W06", CrashWindow::W06_AfterDedupInsertBeforeState},
        {"W07", CrashWindow::W07_AfterStateBeforeOutbox},
        {"W08", CrashWindow::W08_AfterOutboxBeforeCommit},
        {"W09", CrashWindow::W09_AfterCommitBeforeInboxAck},
        {"W10", CrashWindow::W10_AfterCommitBeforeDispatch},
        {"W11", CrashWindow::W11_AfterDispatchBeforeMarkSent},
        {"W12", CrashWindow::W12_AmbiguousFlush},
        {"W13", CrashWindow::W13_DuringSnapshotCompaction},
        {"W14", CrashWindow::W14_DuringSchemaMigration},
        {"W15", CrashWindow::W15_DuringRecoveryReplay},
    };
    const auto it = kByName.find(name);
    return it == kByName.end() ? CrashWindow::None : it->second;
}

class Replayer {
  public:
    Replayer() : engine_(store_, default_lab_policy()) {}

    bool run(std::istream& in, const std::string& name) {
        std::string line;
        size_t line_no = 0;
        while (std::getline(in, line)) {
            ++line_no;
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            const Fields f = parse_line(line);
            if (f.positional.empty() && f.kv.empty()) continue;
            if (!step(f, line_no)) {
                std::printf("%s:%zu: unrecognised line\n", name.c_str(), line_no);
                return false;
            }
        }
        return failures_ == 0;
    }

    void print_state() {
        for (const auto& [key, chain] : chains_) {
            const OrderAggregate* a = engine_.find(OrderId{chain});
            if (a == nullptr) continue;
            std::printf("chain %s (%llu)\n", key.c_str(), static_cast<unsigned long long>(chain));
            std::printf("  lifecycle    %s\n", to_string(a->lifecycle()));
            std::printf("  accepted     %s qty=%lld px=%lld ticks (v%zu)\n",
                        a->accepted_cl_ord_id().c_str(),
                        static_cast<long long>(a->accepted_qty_units()),
                        static_cast<long long>(a->accepted_price_ticks()),
                        a->accepted_versions.size());
            std::printf("  cum/leaves   %lld / %lld\n", static_cast<long long>(a->cum_qty_units),
                        static_cast<long long>(a->leaves_qty_units()));
            int64_t avg = 0;
            if (a->avg_px_ticks_rounded(avg)) {
                std::printf("  avg px       %s (%lld ticks)\n",
                            ticks_to_text(avg, 1, 100000).c_str(), static_cast<long long>(avg));
            }
            std::printf("  fills        %zu\n", a->ledger.size());
            for (const FillLedgerEntry& fill : a->ledger) {
                std::printf("    %-10s %8lld @ %lld ticks  (v%u, %s)\n", fill.exec_id.c_str(),
                            static_cast<long long>(fill.qty_units),
                            static_cast<long long>(fill.price_ticks), fill.accepted_version_seen,
                            fill.cl_ord_id.c_str());
            }
            std::printf("  pending      %s\n",
                        a->pending.has_value()
                            ? (std::string(to_string(a->pending->kind)) + " " +
                               a->pending->cl_ord_id + " (" + to_string(a->pending->phase) + ")")
                                  .c_str()
                            : "none");
            std::printf("  quarantined  %s\n", a->quarantined ? "yes" : "no");
            std::printf("  reconcile    %s\n", a->reconciliation_required ? "required" : "no");
            for (const QuarantineIncident& i : a->incidents) {
                std::printf("    incident   %s %s\n", i.reason_code.c_str(), i.detail.c_str());
            }
            std::printf("  revision     %llu\n", static_cast<unsigned long long>(a->revision));
            std::printf("  state hash   %llu\n",
                        static_cast<unsigned long long>(a->state_hash()));
            const std::vector<InvariantViolation> v = check_invariants(*a, engine_.policy());
            std::printf("  violations   %zu%s\n", v.size(),
                        v.empty() ? "" : (" -- " + describe(v)).c_str());
        }
        std::printf("effects dispatched %zu, duplicates suppressed %zu\n",
                    engine_.sink().dispatched().size(), engine_.sink().suppressed());
    }

    [[nodiscard]] size_t failures() const { return failures_; }

  private:
    void remember(const std::string& cl, OrderId id) {
        if (id.valid()) chains_[cl] = id.value;
    }
    [[nodiscard]] OrderId last_chain() const {
        return chains_.empty() ? OrderId{} : OrderId{chains_.rbegin()->second};
    }

    PeerReport base_report(const Fields& f) {
        PeerReport r;
        r.msg_type = '8';
        r.account = f.get("acct", "ACC1");
        r.symbol = f.get("sym", "EURUSD");
        r.side_raw = f.get("side", "1");
        r.peer_id = f.get("peer", "LP1");
        r.session_id = f.get("session", "S1");
        r.has_source_cursor = true;
        r.source_seq = ++source_seq_;
        return r;
    }

    void arm_if_requested() {
        if (armed_ == CrashWindow::None) return;
        store_.failpoint().arm(armed_);
        armed_ = CrashWindow::None;
    }

    EngineResult deliver(const OrderEvent& e) {
        arm_if_requested();
        try {
            return engine_.submit(e);
        } catch (const SimulatedCrash& c) {
            std::printf("  [crash] %s at %s\n", to_string(c.window), c.where.c_str());
            crashed_ = true;
            EngineResult r;
            r.decision = Decision::RetryCommit;
            return r;
        } catch (const AmbiguousCommit&) {
            std::printf("  [crash] W12 ambiguous commit\n");
            EngineResult r;
            r.decision = Decision::RetryCommit;
            return r;
        }
    }

    bool step(const Fields& f, size_t line_no);
    bool verify(const Fields& f, size_t line_no);

    MemoryStore store_;
    Engine engine_;
    std::map<std::string, uint64_t> chains_;
    uint64_t source_seq_ = 0;
    CrashWindow armed_ = CrashWindow::None;
    bool crashed_ = false;
    size_t failures_ = 0;
};

bool Replayer::step(const Fields& f, size_t line_no) {
    const std::string verb = f.at(0);

    if (verb == "policy") {
        return true;  // strict_v1 is the only profile this build has
    }
    if (verb == "new") {
        LocalCommand c;
        c.kind = CommandKind::New;
        c.account = f.get("acct", "ACC1");
        c.symbol = f.get("sym", "EURUSD");
        c.side_raw = f.get("side", "1");
        c.cl_ord_id = f.at(1);
        c.qty_text = f.get("qty");
        c.price_text = f.get("px");
        const EngineResult r = deliver(OrderEvent{c});
        std::printf("  new %-8s -> %s %s\n", c.cl_ord_id.c_str(), to_string(r.decision),
                    to_string(r.reason));
        remember(c.cl_ord_id, r.id);
        return true;
    }
    if (verb == "cancel" || verb == "replace") {
        LocalCommand c;
        c.kind = verb == "cancel" ? CommandKind::Cancel : CommandKind::Replace;
        c.account = f.get("acct", "ACC1");
        c.symbol = f.get("sym", "EURUSD");
        c.side_raw = f.get("side", "1");
        c.cl_ord_id = f.at(1);
        c.orig_cl_ord_id = f.get("orig");
        c.qty_text = f.get("qty");
        c.price_text = f.get("px");
        const EngineResult r = deliver(OrderEvent{c});
        std::printf("  %-7s %-8s -> %s %s\n", verb.c_str(), c.cl_ord_id.c_str(),
                    to_string(r.decision), to_string(r.reason));
        return true;
    }
    if (verb == "dispatch" || verb == "timeout" || verb == "disconnect" || verb == "reconcile" ||
        verb == "resolve" || verb == "override") {
        SystemEvent s;
        s.target_cl_ord_id = f.at(1);
        if (verb == "dispatch") s.kind = SystemKind::Dispatched;
        if (verb == "timeout") s.kind = SystemKind::AckTimeout;
        if (verb == "disconnect") s.kind = SystemKind::TransportDisconnected;
        if (verb == "reconcile") s.kind = SystemKind::ReconcileStarted;
        if (verb == "resolve") s.kind = SystemKind::ReconcileResolved;
        if (verb == "override") s.kind = SystemKind::ManualOverride;
        s.resolved_status = f.get("status");
        s.evidence_provided = f.has("evidence");
        s.dual_control_signed = f.has("dual");
        s.reconciliation_generation = std::stoull(f.get("gen", "0"));
        const EngineResult r = deliver(OrderEvent{s});
        std::printf("  %-10s %-8s -> %s %s\n", verb.c_str(), s.target_cl_ord_id.c_str(),
                    to_string(r.decision), to_string(r.reason));
        return true;
    }
    if (verb == "ack") {
        PeerReport r = base_report(f);
        r.exec_type = exec_type::kNew;
        r.ord_status = ord_status::kNew;
        r.cl_ord_id = f.at(1);
        r.exec_id = f.get("exec", "E-ACK-" + r.cl_ord_id);
        r.order_id = f.get("oid", "LPO-" + r.cl_ord_id);
        const EngineResult res = deliver(OrderEvent{r});
        std::printf("  ack %-8s -> %s %s\n", r.cl_ord_id.c_str(), to_string(res.decision),
                    to_string(res.reason));
        remember(r.cl_ord_id, res.id);
        return true;
    }
    if (verb == "fill") {
        PeerReport r = base_report(f);
        r.exec_type = exec_type::kTrade;
        r.ord_status = f.get("status", ord_status::kPartiallyFilled);
        r.exec_id = f.at(1);
        r.cl_ord_id = f.get("cl");
        r.last_qty = f.get("qty");
        r.last_px = f.get("px");
        if (f.has("cum")) r.cum_qty = f.get("cum");
        if (f.has("leaves")) r.leaves_qty = f.get("leaves");
        if (f.has("oqty")) r.order_qty = f.get("oqty");
        const EngineResult res = deliver(OrderEvent{r});
        std::printf("  fill %-8s -> %s %s %s\n", r.exec_id.c_str(), to_string(res.decision),
                    to_string(res.reason), res.detail.c_str());
        return true;
    }
    if (verb == "report") {
        const std::string kind = f.at(1);
        PeerReport r = base_report(f);
        r.cl_ord_id = f.at(2);
        r.exec_id = f.get("exec", "E-" + kind + "-" + r.cl_ord_id);
        if (f.has("orig")) r.orig_cl_ord_id = f.get("orig");
        if (f.has("cum")) r.cum_qty = f.get("cum");
        if (f.has("leaves")) r.leaves_qty = f.get("leaves");
        if (f.has("qty")) r.order_qty = f.get("qty");
        if (f.has("px")) r.price = f.get("px");
        if (f.has("ref")) r.exec_ref_id = f.get("ref");
        if (f.has("gen")) r.reconciliation_generation = std::stoull(f.get("gen"));

        if (kind == "pending_cancel") {
            r.exec_type = exec_type::kPendingCancel;
            r.ord_status = ord_status::kPendingCancel;
        } else if (kind == "canceled") {
            r.exec_type = exec_type::kCanceled;
            r.ord_status = ord_status::kCanceled;
            if (!r.leaves_qty.has_value()) r.leaves_qty = "0";
        } else if (kind == "pending_replace") {
            r.exec_type = exec_type::kPendingReplace;
            r.ord_status = ord_status::kPendingReplace;
        } else if (kind == "replaced") {
            r.exec_type = exec_type::kReplaced;
            r.ord_status = ord_status::kNew;
        } else if (kind == "rejected") {
            r.exec_type = exec_type::kRejected;
            r.ord_status = ord_status::kRejected;
        } else if (kind == "expired") {
            r.exec_type = exec_type::kExpired;
            r.ord_status = ord_status::kExpired;
        } else if (kind == "done_for_day") {
            r.exec_type = exec_type::kDoneForDay;
            r.ord_status = ord_status::kDoneForDay;
        } else if (kind == "status") {
            r.exec_type = exec_type::kOrderStatus;
            r.order_id = f.get("oid", "LPO-" + r.cl_ord_id);
        } else if (kind == "cancel_reject") {
            r.msg_type = '9';
            r.cxl_rej_response_to = f.get("to", "1");
            r.cxl_rej_reason = f.get("reason", "0");
        } else if (kind == "trade_correct") {
            r.exec_type = exec_type::kTradeCorrect;
        } else if (kind == "trade_cancel") {
            r.exec_type = exec_type::kTradeCancel;
        } else {
            return false;
        }
        if (f.has("status_code")) r.ord_status = f.get("status_code");
        const EngineResult res = deliver(OrderEvent{r});
        std::printf("  %-14s %-8s -> %s %s %s\n", kind.c_str(), r.cl_ord_id.c_str(),
                    to_string(res.decision), to_string(res.reason), res.detail.c_str());
        return true;
    }
    if (verb == "crash") {
        armed_ = window_from_name(f.at(1));
        if (armed_ == CrashWindow::None) return false;
        std::printf("  [arm] %s\n", to_string(armed_));
        return true;
    }
    if (verb == "recover") {
        const RecoveryReport rep = engine_.recover();
        std::printf("  [recover] %s replayed=%zu chains=%zu trading=%s %s\n",
                    to_string(rep.status), rep.records_replayed, rep.chains_recovered,
                    rep.trading_allowed ? "allowed" : "blocked", rep.detail.c_str());
        return true;
    }
    if (verb == "drain") {
        const size_t n = engine_.pump_all();
        std::printf("  [drain] %zu event(s)\n", n);
        return true;
    }
    if (verb == "assert") {
        return verify(f, line_no);
    }
    return false;
}

bool Replayer::verify(const Fields& f, size_t line_no) {
    const OrderId id = f.has("chain") ? OrderId{chains_[f.get("chain")]} : last_chain();
    const OrderAggregate* a = engine_.find(id);
    if (a == nullptr) {
        std::printf("  [FAIL] line %zu: no chain to assert against\n", line_no);
        ++failures_;
        return true;
    }

    const auto fail = [&](const std::string& what, const std::string& want,
                          const std::string& got) {
        std::printf("  [FAIL] line %zu: %s expected %s, got %s\n", line_no, what.c_str(),
                    want.c_str(), got.c_str());
        ++failures_;
    };

    for (const auto& [key, want] : f.kv) {
        if (key == "lifecycle") {
            const std::string got = to_string(a->lifecycle());
            if (got != want) fail("lifecycle", want, got);
        } else if (key == "cum") {
            const std::string got = std::to_string(a->cum_qty_units);
            if (got != want) fail("cum", want, got);
        } else if (key == "leaves") {
            const std::string got = std::to_string(a->leaves_qty_units());
            if (got != want) fail("leaves", want, got);
        } else if (key == "qty") {
            const std::string got = std::to_string(a->accepted_qty_units());
            if (got != want) fail("accepted qty", want, got);
        } else if (key == "fills") {
            const std::string got = std::to_string(a->ledger.size());
            if (got != want) fail("fill count", want, got);
        } else if (key == "versions") {
            const std::string got = std::to_string(a->accepted_versions.size());
            if (got != want) fail("version count", want, got);
        } else if (key == "accepted") {
            if (a->accepted_cl_ord_id() != want) {
                fail("accepted ClOrdID", want, a->accepted_cl_ord_id());
            }
        } else if (key == "pending") {
            const std::string got =
                a->pending.has_value() ? a->pending->cl_ord_id : std::string("none");
            if (got != want) fail("pending", want, got);
        } else if (key == "quarantined") {
            const std::string got = a->quarantined ? "yes" : "no";
            if (got != want) fail("quarantined", want, got);
        } else if (key == "reconcile") {
            const std::string got = a->reconciliation_required ? "yes" : "no";
            if (got != want) fail("reconcile", want, got);
        } else if (key == "violations") {
            const std::vector<InvariantViolation> v = check_invariants(*a, engine_.policy());
            const std::string got = std::to_string(v.size());
            if (got != want) fail("invariant violations (" + describe(v) + ")", want, got);
        } else if (key == "chain") {
            // selector, not an assertion
        } else {
            std::printf("  [FAIL] line %zu: unknown assertion '%s'\n", line_no, key.c_str());
            ++failures_;
        }
    }
    if (failures_ == 0) std::printf("  [ok] assertions held\n");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quiet") {
            quiet = true;
        } else if (a == "--case" && i + 1 < argc) {
            path = std::string("tests/fixtures/traces/") + argv[++i] + ".trace";
        } else if (a.rfind("--", 0) == 0) {
            if (a.rfind("--seed", 0) == 0) continue;  // accepted and ignored: replay is exact
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::printf("usage: order_trace_replay [--case <id> | <file.trace>] [--quiet]\n");
        return 2;
    }

    std::ifstream in(path);
    if (!in) {
        std::printf("cannot open %s\n", path.c_str());
        return 2;
    }
    std::printf("replaying %s\n", path.c_str());
    Replayer r;
    const bool ok = r.run(in, path);
    if (!quiet) r.print_state();
    std::printf("%s: %s (%zu assertion failure(s))\n", path.c_str(), ok ? "PASS" : "FAIL",
                r.failures());
    return ok ? 0 : 1;
}
