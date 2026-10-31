// FIX session layer: logon, identity, sequencing, resend and transport chaos.
//
// Matrix coverage: group A, group I, and the parts of group K that do not need
// a real socket.
//
// The invariant under constant test here is I-01: no application message
// reaches the order actor before the session is authenticated, synchronised and
// the frame is durably committed. Several cases assert it negatively - the
// order must NOT exist - which is the only way to show a guard is actually
// doing something.

#include <string>
#include <vector>

#include "../support/harness.hpp"

using namespace lpsim;
using namespace tt;

namespace {

// A started fixture that has NOT logged on yet.
struct Raw {
    LpFixture fx;
    std::vector<std::string> storage;

    explicit Raw(const std::string& tag, FixtureOptions opt = {}) : fx(tag, std::move(opt)) {
        CHECK(fx.start());
    }

    std::vector<DecodedMessage> sent() { return fx.sent_messages(&storage); }
    std::vector<DecodedMessage> of(std::string_view mt) {
        return fx.sent_of_type(mt, &storage);
    }
};

// A logged-on fixture with liquidity, for the resend and outbox cases.
struct Live {
    LpFixture fx;
    std::vector<std::string> storage;

    explicit Live(const std::string& tag, FixtureOptions opt = {}) : fx(tag, std::move(opt)) {
        CHECK(fx.start());
        CHECK(fx.logon());
        CHECK_EQ(fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 1, 600000),
                 QuoteError::Ok);
    }

    std::vector<DecodedMessage> sent() { return fx.sent_messages(&storage); }
    std::vector<DecodedMessage> of(std::string_view mt) {
        return fx.sent_of_type(mt, &storage);
    }
    const OrderChain* order(const std::string& id) {
        return fx.actor().find_by_cl_ord_id(id);
    }
};

FixtureOptions creds() {
    FixtureOptions o;
    o.require_credentials = true;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// A: connection and identity
// ---------------------------------------------------------------------------

SCENARIO("A-01", "P0", "an order sent before Logon is not processed at all") {
    Raw R("a01");
    R.fx.connect();
    CHECK_EQ(R.fx.session().phase(), SessionPhase::LogonPending);

    const auto v = R.fx.feed(R.fx.peer().new_order(NewOrderArgs{}));
    CHECK_EQ(v.size(), static_cast<std::size_t>(1));
    CHECK_EQ(v[0], InboundVerdict::NotActive);

    // Invariant I-01: no chain, no inbox row, no sequence movement.
    CHECK(R.fx.actor().find_by_cl_ord_id("O1") == nullptr);
    CHECK_EQ(R.fx.session().next_in(), static_cast<std::uint64_t>(1));
    CHECK(!R.fx.transport().connected());
}

SCENARIO("A-02", "P0", "a correct Logon reaches ACTIVE and is acknowledged") {
    Raw R("a02");
    R.fx.connect();
    const auto v = R.fx.feed(R.fx.peer().logon(30));
    CHECK_EQ(v[0], InboundVerdict::Accepted);
    CHECK_EQ(R.fx.session().phase(), SessionPhase::Active);
    CHECK_EQ(R.fx.session().next_in(), static_cast<std::uint64_t>(2));

    const auto acks = R.of("A");
    CHECK_EQ(acks.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(acks[0], tag::HeartBtInt), std::string("30"));
    CHECK_EQ(acks[0].sender_comp_id, std::string("SIMLP01"));
    CHECK_EQ(acks[0].target_comp_id, std::string("TRADER01"));
}

SCENARIO("A-03", "P0", "a wrong SenderCompID is refused and the session stays closed") {
    Raw R("a03");
    R.fx.connect();
    const auto v = R.fx.feed(R.fx.peer().with_comp_ids("A", "IMPOSTOR", "SIMLP01"));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK_NE(R.fx.session().phase(), SessionPhase::Active);
    CHECK(!R.fx.transport().connected());
    // No sequence state was touched by an unauthenticated peer.
    CHECK_EQ(R.fx.session().next_in(), static_cast<std::uint64_t>(1));
}

SCENARIO("A-04", "P0", "a wrong TargetCompID is refused") {
    Raw R("a04");
    R.fx.connect();
    const auto v = R.fx.feed(R.fx.peer().with_comp_ids("A", "TRADER01", "SOMEONEELSE"));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK_NE(R.fx.session().phase(), SessionPhase::Active);
    CHECK_EQ(R.fx.session().next_in(), static_cast<std::uint64_t>(1));
}

SCENARIO("A-05b", "P0", "a FIX.4.2 Logon is refused without reaching the business layer") {
    Raw R("a05");
    R.fx.connect();
    const std::string bad = ClientPeer::with_bad_begin_string(R.fx.peer().logon(30));
    const auto v = R.fx.feed(bad);
    CHECK(!v.empty());
    // The framer stops it at the frame boundary, so it never decodes.
    CHECK(v[0] == InboundVerdict::RejectedWire || v[0] == InboundVerdict::RejectedSession);
    CHECK_NE(R.fx.session().phase(), SessionPhase::Active);
}

SCENARIO("A-06", "P0", "a wrong password is refused and nothing is leaked") {
    Raw R("a06", creds());
    R.fx.connect();
    const auto v = R.fx.feed(R.fx.peer().logon(30, false, "u", "wrong-password"));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK_NE(R.fx.session().phase(), SessionPhase::Active);

    // L-07: the refusal text must not echo the credential back.
    for (const DecodedMessage& m : R.sent()) {
        const std::string text = field(m, tag::Text);
        CHECK(text.find("wrong-password") == std::string::npos);
        CHECK(text.find("p") != 0 || text != std::string("p"));
    }
}

SCENARIO("A-06b", "P0", "the correct credentials are accepted") {
    Raw R("a06b", creds());
    R.fx.connect();
    const auto v = R.fx.feed(R.fx.peer().logon(30, false, "u", "p"));
    CHECK_EQ(v[0], InboundVerdict::Accepted);
    CHECK_EQ(R.fx.session().phase(), SessionPhase::Active);
}

SCENARIO("A-13", "P1", "a heartbeat interval outside the agreed range is refused") {
    Raw R("a13");
    R.fx.connect();
    // Zero would mean no liveness checking at all.
    const auto v = R.fx.feed(R.fx.peer().logon(0));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK_NE(R.fx.session().phase(), SessionPhase::Active);

    Raw S("a13b");
    S.fx.connect();
    CHECK_EQ(S.fx.feed(S.fx.peer().logon(100000))[0], InboundVerdict::RejectedSession);
}

SCENARIO("A-14", "P1", "a second Logon on one connection is refused and resets nothing") {
    Raw R("a14");
    R.fx.connect();
    CHECK_EQ(R.fx.feed(R.fx.peer().logon(30))[0], InboundVerdict::Accepted);
    const std::uint64_t next_in = R.fx.session().next_in();

    const auto v = R.fx.feed(R.fx.peer().logon(30));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    // The sequence state is not rewound by a protocol error.
    CHECK(R.fx.session().next_in() >= next_in);
}

SCENARIO("A-10", "P0", "an unapproved ResetSeqNumFlag is refused, not honoured") {
    Live L("a10");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::uint64_t next_in = L.fx.session().next_in();
    const std::uint64_t epoch = L.fx.session().epoch();

    L.fx.session().on_disconnect(L.fx.session().generation());
    L.fx.connect();
    L.fx.peer().set_next_seq(1);
    const auto v = L.fx.feed(L.fx.peer().logon(30, true));

    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    // ADR-09 and design doc 3.4: fail closed. Sequence state and the epoch are
    // unchanged, and the order ledger is untouched.
    CHECK_EQ(L.fx.session().epoch(), epoch);
    CHECK(L.fx.session().next_in() >= next_in);
    CHECK(L.order("O1") != nullptr);
}

SCENARIO("A-10b", "P0", "an approved reset moves sequences and keeps the order ledger") {
    FixtureOptions opt;
    opt.allow_reset_on_logon = true;
    Live L("a10b", opt);
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string order_id = L.order("O1")->order_id;

    L.fx.session().on_disconnect(L.fx.session().generation());
    L.fx.connect();
    L.fx.peer().set_next_seq(1);
    const auto v = L.fx.feed(L.fx.peer().logon(30, true));
    CHECK_EQ(v[0], InboundVerdict::Accepted);

    // Session numbers restart; business identity does not (C-07, I-15).
    CHECK_EQ(L.fx.session().epoch(), static_cast<std::uint64_t>(2));
    CHECK(L.order("O1") != nullptr);
    CHECK_EQ(L.order("O1")->order_id, order_id);
}

SCENARIO("A-11", "P0", "a Logon ahead of expectation synchronises before processing") {
    Raw R("a11");
    R.fx.connect();
    // The peer claims sequence 6 when 1 is expected.
    R.fx.peer().set_next_seq(6);
    const auto v = R.fx.feed(R.fx.peer().logon(30));
    CHECK_EQ(v[0], InboundVerdict::QueuedGap);
    CHECK_EQ(R.fx.session().phase(), SessionPhase::Synchronizing);

    // A ResendRequest was sent, and the session is NOT active.
    const auto rr = R.of("2");
    CHECK_EQ(rr.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(rr[0], tag::BeginSeqNo), std::string("1"));

    // H-19: an application message now is not delivered.
    const auto v2 = R.fx.feed(R.fx.peer().new_order(NewOrderArgs{}));
    CHECK(v2[0] == InboundVerdict::NotActive || v2[0] == InboundVerdict::QueuedGap);
    CHECK(R.fx.actor().find_by_cl_ord_id("O1") == nullptr);
}

SCENARIO("A-12", "P0", "a Logon below expectation is refused") {
    Live L("a12");
    // next_in is now 2 after the logon. Reconnect claiming sequence 1 again
    // without PossDup is unrecoverable at the session layer.
    L.fx.session().on_disconnect(L.fx.session().generation());
    L.fx.connect();
    L.fx.peer().set_next_seq(1);
    const auto v = L.fx.feed(L.fx.peer().logon(30));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK_NE(L.fx.session().phase(), SessionPhase::Active);
}

SCENARIO("A-07", "P0", "a disconnect keeps sequences, the epoch and the order ledger") {
    Live L("a07");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string order_id = L.order("O1")->order_id;
    const std::uint64_t next_in = L.fx.session().next_in();
    const std::uint64_t next_out = L.fx.session().next_out();
    const std::uint64_t epoch = L.fx.session().epoch();

    L.fx.transport().close();
    L.fx.session().on_disconnect(L.fx.session().generation());
    CHECK_EQ(L.fx.session().phase(), SessionPhase::Listening);

    // A TCP disconnect is not a session reset (design doc 3.4).
    CHECK_EQ(L.fx.session().next_in(), next_in);
    CHECK_EQ(L.fx.session().next_out(), next_out);
    CHECK_EQ(L.fx.session().epoch(), epoch);
    CHECK_EQ(L.order("O1")->order_id, order_id);
}

// A-08, two simultaneous sockets for one FIX identity, needs two real sockets
// against the listener and so cannot be expressed with this in-process
// transport. It lives in scripts/smoke_tcp.py, which CTest runs as test_smoke.
// What follows is the in-process half of the same concern: an older connection
// generation is fenced off and cannot act.
SCENARIO("A-19", "P0", "bytes from a superseded connection are ignored") {
    Live L("a19");
    const std::uint64_t old_generation = L.fx.session().generation();

    L.fx.session().on_disconnect(old_generation);
    L.fx.connect();  // a new generation
    CHECK_NE(L.fx.session().generation(), old_generation);

    // A delayed frame from the old socket must not advance anything (K-11).
    const auto v = L.fx.session().on_bytes(L.fx.peer().new_order(NewOrderArgs{}),
                                           old_generation);
    CHECK_EQ(v[0], InboundVerdict::Disconnected);
    CHECK(L.order("O1") == nullptr);
}

SCENARIO("A-19b", "P0", "a disconnect callback from an old generation does not change state") {
    Live L("a19b");
    const std::uint64_t old_generation = L.fx.session().generation();
    L.fx.session().on_disconnect(old_generation);
    L.fx.connect();
    const SessionPhase phase = L.fx.session().phase();

    // The stale callback arrives late; the live connection must survive it.
    L.fx.session().on_disconnect(old_generation);
    CHECK_EQ(L.fx.session().phase(), phase);
}

SCENARIO("A-16", "P1", "a Logout does not discard reports still in the outbox") {
    Live L("a16");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // Make the peer stop reading so the trade report cannot be delivered.
    L.fx.transport().stop_reading();
    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK(L.fx.store().count_outbox_pending() > 0);

    L.fx.feed(L.fx.peer().logout());

    // The fill and its report survive the logout.
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK(L.fx.store().count_outbox_pending() > 0);
}

SCENARIO("A-17", "P1", "an unanswered TestRequest ends the connection") {
    Live L("a17");
    L.fx.transport().clear();

    // Past the heartbeat interval with no inbound traffic: a TestRequest goes
    // out as a liveness probe.
    L.fx.clock().advance_steady(Millis(40000));
    L.fx.session().on_timer();
    const auto probes = L.of("1");
    CHECK_EQ(probes.size(), static_cast<std::size_t>(1));
    const std::string probe_id = field(probes[0], tag::TestReqID);
    CHECK(!probe_id.empty());

    // Still nothing back: the session gives up rather than trusting a socket
    // that has not errored (A-17, K-17).
    L.fx.clock().advance_steady(Millis(70000));
    L.fx.session().on_timer();
    CHECK_EQ(L.fx.session().phase(), SessionPhase::Listening);
}

SCENARIO("A-17b", "P1", "a Heartbeat echoing the TestReqID clears the probe") {
    Live L("a17b");
    L.fx.transport().clear();
    L.fx.clock().advance_steady(Millis(40000));
    L.fx.session().on_timer();
    const std::string probe_id = field(L.of("1")[0], tag::TestReqID);

    // Only the matching id resolves the probe.
    L.fx.feed(L.fx.peer().heartbeat("UNRELATED"));
    L.fx.clock().advance_steady(Millis(70000));
    L.fx.session().on_timer();
    CHECK_EQ(L.fx.session().phase(), SessionPhase::Listening);

    Live M("a17c");
    M.fx.transport().clear();
    M.fx.clock().advance_steady(Millis(40000));
    M.fx.session().on_timer();
    const std::string id2 = field(M.of("1")[0], tag::TestReqID);
    M.fx.feed(M.fx.peer().heartbeat(id2));
    M.fx.clock().advance_steady(Millis(1000));
    M.fx.session().on_timer();
    CHECK_EQ(M.fx.session().phase(), SessionPhase::Active);
}

SCENARIO("A-02b", "P0", "a TestRequest from the peer is answered with the same id") {
    Live L("a02b");
    L.fx.transport().clear();
    L.fx.feed(L.fx.peer().test_request("PING-1"));
    const auto hbs = L.of("0");
    CHECK_EQ(hbs.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(hbs[0], tag::TestReqID), std::string("PING-1"));
}

// ---------------------------------------------------------------------------
// Sequencing
// ---------------------------------------------------------------------------

SCENARIO("B-20", "P0", "an application message arriving out of order is not delivered") {
    Live L("b20");
    // Skip a sequence number: the frame is ahead of expectation.
    L.fx.peer().set_next_seq(L.fx.session().next_in() + 3);
    const auto v = L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    CHECK_EQ(v[0], InboundVerdict::QueuedGap);

    // Nothing reached the actor and the inbox did not advance.
    CHECK(L.order("O1") == nullptr);
    const auto rr = L.of("2");
    CHECK(!rr.empty());
}

SCENARIO("I-10", "P0", "a repeated low sequence without PossDup is a protocol error") {
    Live L("i10");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::uint64_t used = L.fx.session().next_in() - 1;

    // The same sequence number again, with no 43=Y to explain it.
    L.fx.peer().set_next_seq(used);
    NewOrderArgs other;
    other.cl_ord_id = "O2";
    const auto v = L.fx.feed(L.fx.peer().new_order(other));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK(L.order("O2") == nullptr);
}

SCENARIO("I-14", "P0", "a SequenceReset-GapFill advances expectations only") {
    Live L("i14");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string order_id = L.order("O1")->order_id;

    const std::uint64_t target = L.fx.session().next_in() + 5;
    const auto v = L.fx.feed(L.fx.peer().sequence_reset(target, true));
    CHECK_EQ(v[0], InboundVerdict::Accepted);
    CHECK_EQ(L.fx.session().next_in(), target);

    // Business de-duplication is untouched: O1 is still taken.
    CHECK_EQ(L.order("O1")->order_id, order_id);
    L.fx.peer().set_next_seq(target);
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
}

SCENARIO("I-14b", "P0", "an unsolicited SequenceReset-Reset is refused") {
    Live L("i14b");
    // Without GapFillFlag this is a reset by another name, and the profile does
    // not permit one.
    const auto v = L.fx.feed(L.fx.peer().sequence_reset(99, false));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
}

SCENARIO("I-02b", "P0", "a retransmitted cancel does not cancel twice") {
    Live L("i02b");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string cancel = L.fx.peer().cancel(CancelArgs{});
    L.fx.feed(cancel);
    CHECK_EQ(L.order("O1")->base, BaseState::Canceled);
    const std::size_t reports = L.sent().size();

    const auto v = L.fx.feed(ClientPeer::as_poss_dup(cancel, L.fx.clock().wall_now()));
    CHECK_EQ(v[0], InboundVerdict::IgnoredPossDup);
    CHECK_EQ(L.sent().size(), reports);
}

SCENARIO("I-05", "P0", "a retransmitted replace does not create a second version") {
    Live L("i05");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    ReplaceArgs r;
    r.qty = "120000";
    r.price = "1.08500";
    const std::string replace = L.fx.peer().replace(r);
    L.fx.feed(replace);
    const std::uint64_t version = L.order("R1")->version;

    const auto v = L.fx.feed(ClientPeer::as_poss_dup(replace, L.fx.clock().wall_now()));
    CHECK_EQ(v[0], InboundVerdict::IgnoredPossDup);
    CHECK_EQ(L.order("R1")->version, version);
    CHECK_EQ(L.order("R1")->effective_qty, static_cast<Units>(120000));
}

// ---------------------------------------------------------------------------
// I: resend and the outbox
// ---------------------------------------------------------------------------

SCENARIO("I-06b", "P0", "a resend replays the original sequence and ExecID with 43=Y") {
    Live L("i06");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    // Capture the original trade report.
    std::vector<std::string> storage;
    const auto before = L.fx.sent_of_type("8", &storage);
    CHECK(before.size() >= 2);
    const DecodedMessage original_trade = before.back();
    const std::uint64_t original_seq = original_trade.seq_num;
    const std::string original_exec = field(original_trade, tag::ExecID);
    CHECK(!original_exec.empty());

    L.fx.transport().clear();
    L.fx.feed(L.fx.peer().resend_request(original_seq, original_seq));

    const auto replays = L.of("8");
    CHECK_EQ(replays.size(), static_cast<std::size_t>(1));
    // The same sequence number, the same ExecID, flagged as a duplicate with
    // the original SendingTime preserved (I-05, I-12).
    CHECK_EQ(replays[0].seq_num, original_seq);
    CHECK_EQ(field(replays[0], tag::ExecID), original_exec);
    CHECK(replays[0].poss_dup);
    CHECK(!field(replays[0], tag::OrigSendingTime).empty());
    // And no new fill was produced by replaying a trade report.
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
}

SCENARIO("I-07", "P0", "a resend over admin sequences gap-fills instead of inventing a report") {
    Live L("i07");
    // The logon ack occupies sequence 1 and is admin traffic, which is never
    // retransmitted verbatim.
    L.fx.transport().clear();
    L.fx.feed(L.fx.peer().resend_request(1, 1));

    const auto fills = L.of("4");
    CHECK_EQ(fills.size(), static_cast<std::size_t>(1));
    CHECK_EQ(field(fills[0], tag::GapFillFlag), std::string("Y"));
    // Critically, NO ExecutionReport was fabricated to cover the gap.
    CHECK(L.of("8").empty());
}

SCENARIO("I-01", "P0", "a report lost on the wire is recovered by a resend, not re-executed") {
    Live L("i01");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // The peer stops reading, so the trade report never arrives.
    L.fx.transport().stop_reading();
    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));

    // Reading resumes and the pending report is dispatched. The ledger did not
    // change while the peer was unreachable.
    L.fx.transport().resume_reading();
    L.fx.session().dispatch_outbox();
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));

    const auto reports = L.of("8");
    bool saw_trade = false;
    for (const DecodedMessage& m : reports) {
        if (field(m, tag::ExecType) == "F") saw_trade = true;
    }
    CHECK(saw_trade);
    CHECK_EQ(L.fx.store().count_outbox_pending(), static_cast<std::size_t>(0));
}

SCENARIO("I-08b", "P0", "a resend over missing application history halts the session") {
    Live L("i08");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    L.fx.inject_fill(id, 40000, "1.08490");

    std::vector<std::string> storage;
    const auto reports = L.fx.sent_of_type("8", &storage);
    const std::uint64_t trade_seq = reports.back().seq_num;

    // Corrupt the journal so the frame for that sequence is unusable.
    CHECK_EQ(L.fx.store().test_corrupt_outbound_frame(trade_seq, "not-a-fix-frame"),
             StoreError::None);

    L.fx.feed(L.fx.peer().resend_request(trade_seq, trade_seq));
    // PB-13: never gap-fill over a trade. The session halts for the operator.
    CHECK_EQ(L.fx.session().phase(), SessionPhase::Halted);
    CHECK_EQ(L.fx.session().halt_reason(), HaltReason::MissingOutboundHistory);
}

SCENARIO("I-09b", "P0", "repeated resend requests are bounded") {
    Live L("i09");
    SessionConfig probe;
    // The configured bound is 50 per connection in the fixture profile; well
    // past it the session stops serving rather than looping forever.
    for (int i = 0; i < 60; ++i) {
        L.fx.feed(L.fx.peer().resend_request(1, 1));
        if (!L.fx.transport().connected()) break;
    }
    CHECK(!L.fx.transport().connected());
}

SCENARIO("I-19", "P0", "reports committed while the peer is away are delivered on return") {
    Live L("i19");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // Disconnect, then commit a fill. The report has nowhere to go yet.
    L.fx.transport().close();
    L.fx.session().on_disconnect(L.fx.session().generation());
    L.fx.inject_fill(id, 40000, "1.08490");
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK(L.fx.store().count_outbox_pending() > 0);

    // Reconnect and log on: the durable outbox drains.
    L.fx.connect();
    CHECK_EQ(L.fx.feed(L.fx.peer().logon(30))[0], InboundVerdict::Accepted);
    L.fx.session().dispatch_outbox();
    CHECK_EQ(L.fx.store().count_outbox_pending(), static_cast<std::size_t>(0));
    // Exactly one fill, reported once as an original.
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));
}

SCENARIO("K-02", "P0", "a short write leaves the report pending with its sequence intact") {
    Live L("k02");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;

    // W7: the next write is truncated, so the peer may have received anything.
    L.fx.transport().next_write_short(10);
    L.fx.inject_fill(id, 40000, "1.08490");

    // The fill is committed, and the report whose write was truncated keeps the
    // sequence number it was already bound to.
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(40000));
    std::vector<OutboxEntry> pending;
    CHECK_EQ(L.fx.store().pending_outbox(&pending), StoreError::None);
    std::map<std::string, std::uint64_t> assigned_before;
    for (const OutboxEntry& e : pending) {
        if (e.send_state == SendState::SeqAssigned && e.outbound_seq > 0)
            assigned_before[e.event_id] = e.outbound_seq;
    }
    CHECK(!assigned_before.empty());

    // Retrying re-sends those rows under their ORIGINAL sequence numbers. The
    // counter may still advance here, because a report that had not been
    // assigned one yet gets its first - which is correct, and is why the check
    // is per event rather than on the counter (I-12, I-15).
    L.fx.session().dispatch_outbox();
    CHECK_EQ(L.fx.store().pending_outbox(&pending), StoreError::None);
    for (const OutboxEntry& e : pending) {
        const auto it = assigned_before.find(e.event_id);
        if (it != assigned_before.end()) CHECK_EQ(e.outbound_seq, it->second);
    }
    CHECK_EQ(L.fx.actor().fill_count_for(id), static_cast<std::size_t>(1));

    // And no original message ever shares a sequence number with another.
    check_outbound_seq_unique(L.fx);
}

SCENARIO("K-14", "P0", "a peer that stops reading does not change business state") {
    Live L("k14");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string id = L.order("O1")->order_id;
    const std::size_t fills_before = L.fx.actor().fill_count();

    L.fx.transport().stop_reading();
    // Dispatch repeatedly against a blocked peer.
    for (int i = 0; i < 20; ++i) L.fx.session().dispatch_outbox();

    // No fill was invented and nothing was dropped; the outbox simply waits.
    CHECK_EQ(L.fx.actor().fill_count(), fills_before);
    CHECK_EQ(L.order("O1")->cum_qty, static_cast<Units>(0));
    std::string why;
    CHECK(L.fx.actor().check_invariants(&why));
}

SCENARIO("K-15", "P0", "a flood of orders while the peer is blocked stays bounded") {
    FixtureOptions opt;
    opt.max_pending_outbox = 5;
    Live L("k15", opt);
    L.fx.transport().stop_reading();

    for (int i = 1; i <= 50; ++i) {
        NewOrderArgs a;
        a.cl_ord_id = "O" + std::to_string(i);
        a.qty = "1000";
        L.fx.feed(L.fx.peer().new_order(a));
    }
    // Admission stopped before the backlog could grow without bound (L-03).
    CHECK(L.fx.store().count_outbox_pending() <= 6);
    std::string why;
    CHECK(L.fx.actor().check_invariants(&why));
}

SCENARIO("K-06b", "P0", "a hundred coalesced orders are each processed once") {
    Live L("k06");
    std::string blob;
    for (int i = 1; i <= 100; ++i) {
        NewOrderArgs a;
        a.cl_ord_id = "O" + std::to_string(i);
        a.qty = "1000";
        blob += L.fx.peer().new_order(a);
    }
    L.fx.feed(blob);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(100));

    // One chain per request, no duplicates and no losses.
    for (int i = 1; i <= 100; ++i) {
        CHECK(L.order("O" + std::to_string(i)) != nullptr);
    }
}

SCENARIO("K-07b", "P0", "an order split across many reads is processed once") {
    Live L("k07");
    const std::string frame = L.fx.peer().new_order(NewOrderArgs{});
    for (std::size_t i = 0; i < frame.size(); ++i) {
        L.fx.feed(frame.substr(i, 1));
    }
    CHECK(L.order("O1") != nullptr);
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
}

SCENARIO("B-16b", "P0", "a corrupt checksum closes the connection without business effect") {
    Live L("b16");
    const std::string bad = ClientPeer::with_bad_checksum(
        L.fx.peer().new_order(NewOrderArgs{}));
    const auto v = L.fx.feed(bad);
    CHECK(!v.empty());
    CHECK_EQ(v.back(), InboundVerdict::RejectedWire);
    CHECK(L.order("O1") == nullptr);
    CHECK(!L.fx.transport().connected());
}

SCENARIO("B-15c", "P0", "a corrupt BodyLength closes the connection without business effect") {
    Live L("b15");
    const std::string bad = ClientPeer::with_bad_body_length(
        L.fx.peer().new_order(NewOrderArgs{}));
    const auto v = L.fx.feed(bad);
    CHECK(!v.empty());
    CHECK_EQ(v.back(), InboundVerdict::RejectedWire);
    CHECK(L.order("O1") == nullptr);
}

SCENARIO("K-18", "P0", "repeated reconnections do not duplicate orders or leak sequences") {
    Live L("k18");
    L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    const std::string order_id = L.order("O1")->order_id;

    for (int i = 0; i < 50; ++i) {
        L.fx.transport().close();
        L.fx.session().on_disconnect(L.fx.session().generation());
        L.fx.connect();
        const auto v = L.fx.feed(L.fx.peer().logon(30));
        CHECK_EQ(v[0], InboundVerdict::Accepted);
    }
    // One order, one chain, monotonic sequence state.
    CHECK_EQ(L.fx.actor().live_order_count(), static_cast<std::size_t>(1));
    CHECK_EQ(L.order("O1")->order_id, order_id);
    CHECK(L.fx.session().next_in() > 1);
    std::string why;
    CHECK(L.fx.actor().check_invariants(&why));
}

SCENARIO("K-16", "P1", "a SendingTime far outside tolerance is refused") {
    Live L("k16");
    // Move the peer's clock without moving the simulator's.
    L.fx.peer().set_now(L.fx.clock().wall_now() + std::chrono::hours(3));
    const auto v = L.fx.feed(L.fx.peer().new_order(NewOrderArgs{}));
    CHECK_EQ(v[0], InboundVerdict::RejectedSession);
    CHECK(L.order("O1") == nullptr);
}

SCENARIO_NOT_IMPLEMENTED(
    "K-19", "P1", "TLS handshake handling",
    "The v1 profile is plaintext loopback only (design doc 12.2 and config "
    "tls_enabled=false). There is no TLS code path, so there is nothing to test; "
    "the design doc requires that a TLS profile never fall back to plaintext "
    "silently, which is a property of code that does not exist yet.")

SCENARIO_NOT_IMPLEMENTED(
    "A-20", "P2", "multiple client firms",
    "v1 serves one counterparty firm (design doc 1.4). Per-firm isolation is "
    "designed and the firm is carried on every chain, but a second firm profile "
    "is not built.")

int main(int argc, char** argv) { return tt::run_all(argc, argv, "session"); }
