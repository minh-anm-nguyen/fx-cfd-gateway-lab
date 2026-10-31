// Crash windows W1..W10: the process really dies, at a named cutpoint.
//
// Matrix coverage: J-01 to J-10, W8, PB-01, PB-02, PB-16.
//
// This suite re-executes its own binary as a child process. That is not
// convenience: scenario M-15 says a crash test whose cutpoint cannot be proven
// is INVALID rather than PASS, and the only way to prove the cutpoint is to let
// a failpoint terminate a real process and then read the marker it wrote on its
// way out. An in-process simulation would run destructors and flush buffers,
// which is precisely what a kill does not do.
//
// The shape of every case is the same:
//
//   1. a child seeds the store with an accepted order and a quote;
//   2. a child attempts a fill with one failpoint armed, and dies there;
//   3. the parent checks the marker names the cutpoint it asked for;
//   4. a child reopens the same store and reports what survived;
//   5. the parent asserts the survival is what the window demands.
//
// What is NOT claimed: survival of physical power loss. SQLite with
// synchronous=FULL guarantees what the filesystem and the drive honour, and
// design doc 10.1 and 10.5 record that limit rather than testing past it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lp/failpoint.hpp"
#include "lp/order_actor.hpp"
#include "lp/session.hpp"
#include "lp/sqlite_store.hpp"
#include "../support/harness.hpp"

using namespace lpsim;

namespace {

const char* kSeedClOrdId = "O1";
const char* kFillEventId = "CRASH-FILL";

std::string g_self;  // argv[0], for re-execution

std::vector<SymbolProfile> symbols() {
    SymbolProfile eur;
    eur.symbol = "EURUSD";
    eur.price.scale = 5;
    eur.price.tick_step = 1;
    eur.lot_step = 1000;
    eur.quote_stale_after_ms = 600000;
    return {eur};
}

// One simulator instance against a fixed database path.
struct Child {
    VirtualClock clock;
    SqliteStore store;
    QuoteBook book;
    Scheduler sched;
    tt::FakeTransport transport;
    std::unique_ptr<OrderActor> actor;
    std::unique_ptr<AcceptorSession> session;
    std::unique_ptr<tt::ClientPeer> peer;

    Child() : sched(100000) {}

    bool open(const std::string& db) {
        StoreConfig sc;
        sc.path = db;
        sc.session_id = "LP-SESSION-1";
        sc.sender_comp_id = "SIMLP01";
        sc.target_comp_id = "TRADER01";
        if (store.open(sc) != StoreError::None) return false;

        ActorConfig ac;
        ac.firm = "TRADER01";
        ac.session_id = sc.session_id;
        ac.symbols = symbols();
        ac.fill_trigger = FillTrigger::Scripted;
        actor = std::make_unique<OrderActor>(ac, store, book, sched, clock);
        if (!actor->recover()) return false;

        SessionConfig secfg;
        secfg.session_id = sc.session_id;
        secfg.sender_comp_id = sc.sender_comp_id;
        secfg.target_comp_id = sc.target_comp_id;
        session = std::make_unique<AcceptorSession>(secfg, store, *actor, clock, transport);
        if (!session->recover()) return false;

        peer = std::make_unique<tt::ClientPeer>("TRADER01", "SIMLP01", clock.wall_now());
        peer->set_next_seq(session->next_in());
        return true;
    }

    bool logon() {
        transport.reopen();
        session->on_connected(1);
        peer->set_now(clock.wall_now());
        session->on_bytes(peer->logon(30), 1);
        return session->phase() == SessionPhase::Active;
    }
};

// key=value result file, so the parent reads facts rather than parsing prose.
void write_result(const std::string& path, const std::map<std::string, std::string>& kv) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto& p : kv) out << p.first << "=" << p.second << "\n";
}

std::map<std::string, std::string> read_result(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// -------------------------------------------------------------------------
// Child entry points
// -------------------------------------------------------------------------

int child_seed(const std::string& db, const std::string& result) {
    Child c;
    if (!c.open(db)) return 10;
    if (!c.logon()) return 11;

    // A quote with enough liquidity for the fill the next stage attempts.
    QuoteSlice q;
    q.symbol = "EURUSD";
    q.version = 1;
    q.bid_ticks = 108480;
    q.ask_ticks = 108490;
    q.bid_available = 80000;
    q.ask_available = 40000;
    q.valid_from_ms = 0;
    q.expires_at_ms = 600000;
    q.source_fixture_id = "crash";
    ScheduledEvent qe;
    qe.event_id = "QV-1";
    qe.due_us = 0;
    qe.kind = EventKind::QuoteUpdate;
    qe.quote = q;
    if (c.actor->schedule(qe) != EnqueueResult::Ok) return 12;
    c.actor->run_due(0);

    tt::NewOrderArgs a;
    a.cl_ord_id = kSeedClOrdId;
    c.peer->set_now(c.clock.wall_now());
    c.session->on_bytes(c.peer->new_order(a), 1);

    const OrderChain* chain = c.actor->find_by_cl_ord_id(kSeedClOrdId);
    if (chain == nullptr) return 13;
    c.session->dispatch_outbox();

    write_result(result, {{"order_id", chain->order_id},
                          {"next_in", std::to_string(c.session->next_in())},
                          {"next_out", std::to_string(c.session->next_out())},
                          {"outbox_pending", std::to_string(c.store.count_outbox_pending())}});
    c.store.close();
    return 0;
}

// Attempt a 40000 fill, arming the cutpoint at the exact moment under test.
//
// The failpoint is armed HERE rather than from the environment at startup, and
// the difference matters. Several of these points are hit more than once per
// process: tx_b.* fires for the commit that persists the scheduled event as
// well as for the fill's own decision, and socket.* fires for the logon
// acknowledgement before any report. Arming from the environment therefore
// killed the process at the wrong hit and the window under test never ran.
// Arming after the setup is complete makes occurrence one the right one.
int child_fill(const std::string& db, const std::string& order_id,
               const std::string& failpoint, const std::string& marker) {
    Child c;
    if (!c.open(db)) return 10;
    if (!c.logon()) return 11;

    ScheduledEvent ev;
    ev.event_id = kFillEventId;
    ev.due_us = c.clock.virtual_now_us();
    ev.kind = EventKind::InjectFill;
    ev.order_id = order_id;
    ev.qty = 40000;
    ev.px_ticks = 108490;
    // schedule() persists the event before it can run (design doc 11.3). That
    // commit is setup, not the window, so it happens before the arming.
    if (c.actor->schedule(ev) != EnqueueResult::Ok) return 14;

    if (!failpoint.empty()) {
        if (!marker.empty()) fp::set_marker_path(marker);
        fp::arm(failpoint, fp::Action::AbruptExit);
    }

    c.actor->run_due(ev.due_us);
    c.session->dispatch_outbox();
    c.store.close();
    return 0;
}

// Feed a NewOrderSingle, for the TX-A windows W1 and W2.
int child_inbound(const std::string& db) {
    Child c;
    if (!c.open(db)) return 10;
    if (!c.logon()) return 11;
    tt::NewOrderArgs a;
    a.cl_ord_id = "O2";
    c.peer->set_now(c.clock.wall_now());
    c.session->on_bytes(c.peer->new_order(a), 1);
    c.session->dispatch_outbox();
    c.store.close();
    return 0;
}

int child_inspect(const std::string& db, const std::string& order_id,
                  const std::string& result) {
    SqliteStore store;
    StoreConfig sc;
    sc.path = db;
    sc.session_id = "LP-SESSION-1";
    sc.sender_comp_id = "SIMLP01";
    sc.target_comp_id = "TRADER01";
    if (store.open(sc) != StoreError::None) {
        write_result(result, {{"open", "failed"}});
        return 0;
    }
    const RecoveryReport r = store.recover_and_verify();

    std::map<std::string, std::string> kv;
    kv["open"] = "ok";
    kv["recovered"] = r.ok ? "yes" : "no";
    kv["halt"] = std::string(to_string(r.halt));
    kv["detail"] = r.detail;
    kv["unclean"] = r.unclean_shutdown ? "yes" : "no";
    kv["fills"] = std::to_string(r.fills.size());
    kv["next_out"] = std::to_string(r.session.next_out);
    kv["next_in"] = std::to_string(r.session.next_in);
    kv["pending_inbox"] = std::to_string(r.pending_inbox);
    kv["outbox_pending"] = std::to_string(r.outbox.size());
    kv["chains"] = std::to_string(r.chains.size());

    Units cum = 0;
    Ticks avg = 0;
    for (const OrderChain& c : r.chains) {
        if (c.order_id == order_id) {
            cum = c.cum_qty;
            avg = c.avg_px_ticks;
        }
    }
    kv["cum_qty"] = format_qty(cum);
    kv["avg_px"] = format_qty(avg);

    Units ask = 0;
    for (const QuoteSlice& q : r.quotes) {
        if (q.symbol == "EURUSD") ask = q.ask_available;
    }
    kv["ask_available"] = format_qty(ask);

    // Was the fill event marked executed, and did the fill get a sequence?
    bool fill_executed = false;
    for (const std::string& id : r.executed_event_ids) {
        if (id == kFillEventId) fill_executed = true;
    }
    kv["fill_event_executed"] = fill_executed ? "yes" : "no";

    std::string assigned;
    for (const OutboxEntry& e : r.outbox) {
        if (e.type == ReportType::ExecTrade && e.outbound_seq > 0)
            assigned = std::to_string(e.outbound_seq);
    }
    kv["trade_seq"] = assigned;

    // The exec id of the single fill, so the parent can prove it is stable.
    if (!r.fills.empty()) kv["exec_id"] = r.fills.front().exec_id;

    write_result(result, kv);
    store.close();
    return 0;
}

// -------------------------------------------------------------------------
// Parent helpers
// -------------------------------------------------------------------------

int run_child(const std::string& args, const std::string& failpoint,
              const std::string& marker) {
    std::string cmd = "\"" + g_self + "\" " + args;
    // The failpoint and its marker travel in the environment, the way the
    // design doc's LPSIM_FAILPOINT contract describes.
#ifdef _WIN32
    std::string prefix = "set \"LPSIM_FAILPOINT=" + failpoint + "\" && set \"" +
                         "LPSIM_FAILPOINT_MARKER=" + marker + "\" && ";
    cmd = "cmd /C \"" + prefix + cmd + "\"";
#else
    cmd = "LPSIM_FAILPOINT='" + failpoint + "' LPSIM_FAILPOINT_MARKER='" + marker + "' " +
          cmd;
#endif
    const int rc = std::system(cmd.c_str());
    return rc;
}

struct Window {
    const char* id;          // matrix id
    const char* name;        // the failpoint
    const char* description;
};

// Run the standard three-stage crash cycle for one business-path cutpoint.
std::map<std::string, std::string> crash_cycle(const std::string& tag,
                                               const std::string& failpoint,
                                               bool expect_death,
                                               std::string* marker_out) {
    tt::TmpDir dir(tag, /*keep=*/false);
    const std::string db = dir.db();
    const std::string seed_result = dir.file("seed.txt");
    const std::string inspect_result = dir.file("inspect.txt");
    const std::string marker = dir.file("marker.txt");

    // Stage 1: seed, with no failpoint armed.
    const int seed_rc = run_child("--child-seed \"" + db + "\" \"" + seed_result + "\"", "",
                                  "");
    CHECK_MSG(seed_rc == 0, "seed child exited " + std::to_string(seed_rc));
    const auto seed = read_result(seed_result);
    const std::string order_id = seed.count("order_id") ? seed.at("order_id") : std::string();
    CHECK(!order_id.empty());

    // Stage 2: the fill, with the cutpoint armed to terminate the process.
    const int fill_rc = run_child(
        "--child-fill \"" + db + "\" " + order_id + " " + failpoint + " \"" + marker + "\"",
        "", "");
    if (expect_death) {
        // 137 is 128 + SIGKILL, which is what the failpoint exits with.
        CHECK_MSG(fill_rc != 0, "the child was expected to die at " + failpoint +
                                    " but exited 0");
        // M-15: the marker proves WHERE it died. Without this the run is not
        // evidence of anything.
        *marker_out = read_text(marker);
        CHECK_MSG(*marker_out == failpoint,
                  "marker says '" + *marker_out + "', expected '" + failpoint + "'");
    } else {
        CHECK_MSG(fill_rc == 0, "the child exited " + std::to_string(fill_rc));
    }

    // Stage 3: reopen and report.
    const int inspect_rc = run_child(
        "--child-inspect \"" + db + "\" " + order_id + " \"" + inspect_result + "\"", "", "");
    CHECK_MSG(inspect_rc == 0, "inspect child exited " + std::to_string(inspect_rc));
    auto result = read_result(inspect_result);
    result["order_id"] = order_id;
    return result;
}

// The store reopened and verified itself. Says nothing about HOW the previous
// process ended, because not every case here ends in a kill: the W1 case halts
// and shuts down in an orderly way, and the PB-16 reference run never dies.
void require_consistent(const std::map<std::string, std::string>& r) {
    CHECK_EQ(r.at("open"), std::string("ok"));
    CHECK_MSG(r.at("recovered") == "yes",
              "recovery refused: " + r.at("halt") + " " + r.at("detail"));
}

// The previous process really was killed: it never wrote the clean-shutdown
// marker, which is how an orderly stop is told apart from a crash (J-13).
void require_crashed(const std::map<std::string, std::string>& r) {
    require_consistent(r);
    CHECK_EQ(r.at("unclean"), std::string("yes"));
}

}  // namespace

// ---------------------------------------------------------------------------
// W3 and W10: inside the business transaction
// ---------------------------------------------------------------------------

SCENARIO("J-03b", "P0", "W3: dying inside TX-B rolls the whole decision back") {
    std::string marker;
    const auto r = crash_cycle("w3", std::string(fp::point::kTxBBeforeCommit), true, &marker);
    require_crashed(r);

    // No fill, no quote consumption, no report. Either all of TX-B landed or
    // none of it did.
    CHECK_EQ(r.at("fills"), std::string("0"));
    CHECK_EQ(r.at("cum_qty"), std::string("0"));
    CHECK_EQ(r.at("ask_available"), std::string("40000"));
    CHECK_EQ(r.at("fill_event_executed"), std::string("no"));
}

SCENARIO("J-10b", "P0", "W10: dying between consuming the quote and writing the fill") {
    std::string marker;
    const auto r =
        crash_cycle("w10", std::string(fp::point::kTxBAfterQuoteConsume), true, &marker);
    require_crashed(r);

    // This cutpoint is inside the same transaction as W3, so it must be
    // indistinguishable from outside: no orphan consumed liquidity.
    CHECK_EQ(r.at("fills"), std::string("0"));
    CHECK_EQ(r.at("cum_qty"), std::string("0"));
    CHECK_EQ(r.at("ask_available"), std::string("40000"));
}

SCENARIO("J-04b", "P0", "W4: dying after TX-B keeps the fill and its pending report") {
    std::string marker;
    const auto r = crash_cycle("w4", std::string(fp::point::kTxBAfterCommit), true, &marker);
    require_crashed(r);

    // PB-01: exactly one fill, the quote debited exactly once, and the report
    // still owed to the client.
    CHECK_EQ(r.at("fills"), std::string("1"));
    CHECK_EQ(r.at("cum_qty"), std::string("40000"));
    CHECK_EQ(r.at("avg_px"), std::string("108490"));
    CHECK_EQ(r.at("ask_available"), std::string("0"));
    CHECK_EQ(r.at("fill_event_executed"), std::string("yes"));
    CHECK(std::stoi(r.at("outbox_pending")) >= 1);
}

SCENARIO("J-05b", "P0", "W5: dying before send_once leaves the report unsequenced") {
    std::string marker;
    const auto r = crash_cycle("w5", std::string(fp::point::kTxCBeforeCommit), true, &marker);
    require_crashed(r);

    // The business effect is durable; no outbound sequence number was issued,
    // so the retry is free to assign the first one.
    CHECK_EQ(r.at("fills"), std::string("1"));
    CHECK_EQ(r.at("cum_qty"), std::string("40000"));
    CHECK_EQ(r.at("trade_seq"), std::string(""));
}

SCENARIO("J-06b", "P0", "W6: dying after send_once keeps the sequence binding") {
    std::string marker;
    const auto r = crash_cycle("w6", std::string(fp::point::kTxCAfterCommit), true, &marker);
    require_crashed(r);

    // PB-02: the report is permanently bound to one sequence number, and the
    // frame is journaled, so a resend replays the original rather than minting
    // a new one.
    CHECK_EQ(r.at("fills"), std::string("1"));
    CHECK_EQ(r.at("cum_qty"), std::string("40000"));
    CHECK(!r.at("trade_seq").empty());
    const int seq = std::stoi(r.at("trade_seq"));
    const int next_out = std::stoi(r.at("next_out"));
    // NextNumOut moved past the assigned number exactly once.
    CHECK(next_out > seq);
}

SCENARIO("J-07", "P0", "W7: dying mid-write does not issue a second sequence") {
    std::string marker;
    const auto r =
        crash_cycle("w7", std::string(fp::point::kSocketAfterPartialWrite), true, &marker);
    require_crashed(r);

    // The peer may have received any prefix. The local state is unambiguous:
    // one fill, one sequence number, the frame journaled.
    CHECK_EQ(r.at("fills"), std::string("1"));
    CHECK_EQ(r.at("cum_qty"), std::string("40000"));
    CHECK(!r.at("trade_seq").empty());
}

SCENARIO("J-08", "P0", "W8: dying before the dispatch acknowledgement keeps one sequence") {
    std::string marker;
    const auto r = crash_cycle("w8", std::string(fp::point::kTxDBeforeCommit), true, &marker);
    require_crashed(r);

    // The report may already be at the peer. The row stays pending and is
    // re-sent under its ORIGINAL sequence number; no second original is ever
    // issued for the same event (I-15).
    CHECK_EQ(r.at("fills"), std::string("1"));
    CHECK(!r.at("trade_seq").empty());
    CHECK(std::stoi(r.at("outbox_pending")) >= 1);
}

SCENARIO("J-09", "P0", "W9: dying while marking a scheduled event does not run it twice") {
    std::string marker;
    const auto r =
        crash_cycle("w9", std::string(fp::point::kSchedulerMarkExecuted), true, &marker);
    require_crashed(r);

    // The event's durable row and the fill it produced are in the same
    // transaction, so the in-memory mark is not what makes it safe: the fill
    // either exists with the event marked, or neither exists.
    const int fills = std::stoi(r.at("fills"));
    CHECK(fills == 0 || fills == 1);
    if (fills == 1) {
        CHECK_EQ(r.at("cum_qty"), std::string("40000"));
        CHECK_EQ(r.at("fill_event_executed"), std::string("yes"));
        CHECK_EQ(r.at("ask_available"), std::string("0"));
    } else {
        CHECK_EQ(r.at("cum_qty"), std::string("0"));
        CHECK_EQ(r.at("ask_available"), std::string("40000"));
    }
}

SCENARIO("J-02b", "P0", "W2: dying after the inbound commit leaves the request undelivered") {
    tt::TmpDir dir("w2");
    const std::string db = dir.db();
    const std::string seed_result = dir.file("seed.txt");
    const std::string inspect_result = dir.file("inspect.txt");
    const std::string marker = dir.file("marker.txt");

    CHECK_EQ(run_child("--child-seed \"" + db + "\" \"" + seed_result + "\"", "", ""), 0);
    const auto seed = read_result(seed_result);
    const std::string order_id = seed.at("order_id");

    // W2: the inbound identity is durable, the business decision is not.
    const int rc = run_child("--child-inbound \"" + db + "\"",
                             std::string(fp::point::kTxAAfterCommit) + "=exit", marker);
    CHECK(rc != 0);
    CHECK_EQ(read_text(marker), std::string(fp::point::kTxAAfterCommit));

    CHECK_EQ(run_child("--child-inspect \"" + db + "\" " + order_id + " \"" +
                           inspect_result + "\"",
                       "", ""),
             0);
    const auto r = read_result(inspect_result);
    require_crashed(r);

    // O2 never became a chain, and the frame is still pending delivery so the
    // restart will process it exactly once.
    CHECK_EQ(r.at("chains"), std::string("1"));
    CHECK(std::stoi(r.at("pending_inbox")) >= 1);
}

SCENARIO("J-01b", "P0", "W1: dying before the inbound commit leaves no trace") {
    tt::TmpDir dir("w1");
    const std::string db = dir.db();
    const std::string seed_result = dir.file("seed.txt");
    const std::string inspect_result = dir.file("inspect.txt");
    const std::string marker = dir.file("marker.txt");

    CHECK_EQ(run_child("--child-seed \"" + db + "\" \"" + seed_result + "\"", "", ""), 0);
    const std::string order_id = read_result(seed_result).at("order_id");

    // The failpoint at TX-A returns an error rather than exiting, because the
    // session must HALT rather than process a message it could not record.
    const int rc = run_child("--child-inbound \"" + db + "\"",
                             std::string(fp::point::kTxABeforeCommit) + "=error", marker);
    (void)rc;

    CHECK_EQ(run_child("--child-inspect \"" + db + "\" " + order_id + " \"" +
                           inspect_result + "\"",
                       "", ""),
             0);
    const auto r = read_result(inspect_result);
    require_consistent(r);

    // No second chain and no inbox row: the peer will resend and it will be
    // processed exactly once.
    CHECK_EQ(r.at("chains"), std::string("1"));
    CHECK_EQ(r.at("fills"), std::string("0"));
}

SCENARIO("PB-16b", "P0", "a crash at any window leaves the same ledger as no crash") {
    // The reference: the fill with nothing armed.
    std::string marker;
    const auto reference = crash_cycle("ref", "no.such.failpoint", false, &marker);
    require_consistent(reference);
    CHECK_EQ(reference.at("fills"), std::string("1"));
    CHECK_EQ(reference.at("cum_qty"), std::string("40000"));
    const std::string reference_exec = reference.count("exec_id") ? reference.at("exec_id")
                                                                  : std::string();
    CHECK(!reference_exec.empty());

    // Every window at or after the TX-B commit must agree with it on the
    // LOGICAL ledger. Wire timings and retransmission counts may differ, which
    // is exactly what PB-16 permits and what it does not.
    const char* after_commit[] = {fp::point::kTxBAfterCommit.data(),
                                  fp::point::kTxCBeforeCommit.data(),
                                  fp::point::kTxCAfterCommit.data(),
                                  fp::point::kTxDBeforeCommit.data()};
    int i = 0;
    for (const char* point : after_commit) {
        const auto r = crash_cycle("pb16-" + std::to_string(i++), point, true, &marker);
        require_crashed(r);
        CHECK_MSG(r.at("fills") == reference.at("fills"),
                  std::string(point) + ": fill count differs from the reference run");
        CHECK_MSG(r.at("cum_qty") == reference.at("cum_qty"),
                  std::string(point) + ": CumQty differs from the reference run");
        CHECK_MSG(r.at("avg_px") == reference.at("avg_px"),
                  std::string(point) + ": AvgPx differs from the reference run");
        CHECK_MSG(r.at("ask_available") == reference.at("ask_available"),
                  std::string(point) + ": quote inventory differs from the reference run");
        // The ExecID is stable: the same logical fill keeps its identity no
        // matter where the process died (I-05).
        CHECK_MSG(r.count("exec_id") && r.at("exec_id") == reference_exec,
                  std::string(point) + ": the fill's ExecID is not the reference one");
    }
}

SCENARIO("M-15", "P0", "a crash test proves its own cutpoint") {
    // The guard on this whole suite: if the marker did not name the armed
    // failpoint, the run says nothing about that window and must not be scored
    // as a pass. Here the claim is checked directly for two different points.
    std::string marker_b, marker_c;
    crash_cycle("m15-b", std::string(fp::point::kTxBAfterCommit), true, &marker_b);
    crash_cycle("m15-c", std::string(fp::point::kTxCAfterCommit), true, &marker_c);
    CHECK_EQ(marker_b, std::string(fp::point::kTxBAfterCommit));
    CHECK_EQ(marker_c, std::string(fp::point::kTxCAfterCommit));
    CHECK_NE(marker_b, marker_c);
}

SCENARIO_BLOCKED("J-18c", "P0",
                 "survival of physical power loss with a drive that misreports fsync",
                 "Out of reach in process and in this lab. SQLite with synchronous=FULL "
                 "guarantees what the filesystem and the drive honour; proving more needs "
                 "real hardware and instrumented storage. Design doc 10.1 and 10.5 record "
                 "the limitation, and these suites therefore test process death only.")

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    g_self = argv[0];
    fp::arm_from_env();

    // Child dispatch, before the test registry is consulted.
    if (argc >= 2 && std::strcmp(argv[1], "--child-seed") == 0 && argc >= 4)
        return child_seed(argv[2], argv[3]);
    if (argc >= 2 && std::strcmp(argv[1], "--child-fill") == 0 && argc >= 4)
        return child_fill(argv[2], argv[3], argc >= 5 ? argv[4] : "",
                          argc >= 6 ? argv[5] : "");
    if (argc >= 2 && std::strcmp(argv[1], "--child-inbound") == 0 && argc >= 3)
        return child_inbound(argv[2]);
    if (argc >= 2 && std::strcmp(argv[1], "--child-inspect") == 0 && argc >= 5)
        return child_inspect(argv[2], argv[3], argv[4]);

    return tt::run_all(argc, argv, "crash");
}
