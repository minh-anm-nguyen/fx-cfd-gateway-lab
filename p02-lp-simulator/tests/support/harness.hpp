// Test harness: a controllable transport, a scripted FIX client and a wired-up
// simulator instance.
//
// Reference: design doc 18.1 (test topology) and 18.2 (test layers).
//
// Two things here are load-bearing rather than convenience:
//
//   * FakeTransport can fail in specific, repeatable ways. A real socket will
//     not reliably give you a short write, a mid-frame reset or a peer that
//     stops reading, and those are exactly scenarios K-07, K-08 and K-14.
//   * ClientPeer builds frames with its own sequence counter and will build
//     deliberately malformed ones on request. It is a scripted raw peer, which
//     design doc 1.4 says some wire faults require, because a well-behaved FIX
//     library cannot be asked to emit a bad BodyLength.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lp/codec.hpp"
#include "lp/order_actor.hpp"
#include "lp/quote_book.hpp"
#include "lp/report_builder.hpp"
#include "lp/scheduler.hpp"
#include "lp/session.hpp"
#include "lp/sqlite_store.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

namespace tt {

using namespace lpsim;

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

class FakeTransport final : public ITransport {
public:
    std::size_t write(std::string_view bytes) override {
        if (!connected_) return 0;
        if (reject_writes_) return 0;
        if (short_write_bytes_ > 0) {
            // A partial write: the peer may have received any prefix. The
            // session must not treat the report as delivered (W7, K-08).
            const std::size_t n = std::min(short_write_bytes_, bytes.size());
            partial_.append(bytes.substr(0, n));
            short_write_bytes_ = 0;
            return n;
        }
        out_.append(bytes);
        ++frames_;
        return bytes.size();
    }

    void close() override { connected_ = false; }
    bool connected() const override { return connected_; }

    void reopen() {
        connected_ = true;
        reject_writes_ = false;
        short_write_bytes_ = 0;
    }

    // Controls.
    void stop_reading() { reject_writes_ = true; }   // K-14
    void resume_reading() { reject_writes_ = false; }
    void next_write_short(std::size_t n) { short_write_bytes_ = n; }  // W7

    const std::string& bytes() const { return out_; }
    const std::string& partial_bytes() const { return partial_; }
    void clear() {
        out_.clear();
        partial_.clear();
        frames_ = 0;
    }
    std::size_t write_calls() const { return frames_; }

    // Split the accumulated output into complete frames.
    std::vector<std::string> frames() const {
        std::vector<std::string> out;
        FixFramer f(1 << 20, 1 << 21);
        f.append(out_);
        while (true) {
            std::string_view fr;
            WireError e = WireError::None;
            if (!f.next(&fr, &e)) break;
            out.emplace_back(fr);
        }
        return out;
    }

private:
    std::string out_;
    std::string partial_;
    bool connected_ = true;
    bool reject_writes_ = false;
    std::size_t short_write_bytes_ = 0;
    std::size_t frames_ = 0;
};

// ---------------------------------------------------------------------------
// Scripted client
// ---------------------------------------------------------------------------

struct NewOrderArgs {
    std::string cl_ord_id = "O1";
    std::string symbol = "EURUSD";
    std::string side = std::string(side_code::Buy);
    std::string qty = "100000";
    std::string ord_type = std::string(ord_type::Limit);
    std::string price = "1.08500";
    std::string tif = std::string(tif_code::Day);
    std::string account;
    bool omit_cl_ord_id = false;
    bool omit_symbol = false;
    bool omit_side = false;
    bool omit_ord_type = false;
    bool omit_price = false;
    bool omit_qty = false;
    bool omit_transact_time = false;
    bool duplicate_cl_ord_id = false;  // B-18
    bool duplicate_qty = false;        // B-18
    std::string transact_time_override;
};

struct CancelArgs {
    std::string cl_ord_id = "C1";
    std::string orig_cl_ord_id = "O1";
    std::string symbol = "EURUSD";
    std::string side = std::string(side_code::Buy);
    std::string qty;  // optional on a cancel
    bool omit_orig = false;
};

struct ReplaceArgs {
    std::string cl_ord_id = "R1";
    std::string orig_cl_ord_id = "O1";
    std::string symbol = "EURUSD";
    std::string side = std::string(side_code::Buy);
    std::string qty = "100000";
    std::string ord_type = std::string(ord_type::Limit);
    std::string price = "1.08470";
    std::string tif = std::string(tif_code::Day);
    bool omit_orig = false;
};

class ClientPeer {
public:
    ClientPeer(std::string sender, std::string target, WallTime now)
        : sender_(std::move(sender)), target_(std::move(target)), now_(now) {}

    void set_now(WallTime t) { now_ = t; }
    std::uint64_t next_seq() const { return next_out_; }
    void set_next_seq(std::uint64_t s) { next_out_ = s; }

    std::string logon(int heartbeat = 30, bool reset = false,
                      const std::string& user = "", const std::string& pass = "") {
        OutboundFields f = header(msg_type::Logon);
        f.body.emplace_back(tag::EncryptMethod, "0");
        f.body.emplace_back(tag::HeartBtInt, std::to_string(heartbeat));
        if (reset) f.body.emplace_back(tag::ResetSeqNumFlag, "Y");
        if (!user.empty()) f.body.emplace_back(tag::Username, user);
        if (!pass.empty()) f.body.emplace_back(tag::Password, pass);
        return encode(f);
    }

    std::string heartbeat(const std::string& test_req_id = "") {
        OutboundFields f = header(msg_type::Heartbeat);
        if (!test_req_id.empty()) f.body.emplace_back(tag::TestReqID, test_req_id);
        return encode(f);
    }

    std::string test_request(const std::string& id) {
        OutboundFields f = header(msg_type::TestRequest);
        f.body.emplace_back(tag::TestReqID, id);
        return encode(f);
    }

    std::string resend_request(std::uint64_t begin, std::uint64_t end) {
        OutboundFields f = header(msg_type::ResendRequest);
        f.body.emplace_back(tag::BeginSeqNo, std::to_string(begin));
        f.body.emplace_back(tag::EndSeqNo, std::to_string(end));
        return encode(f);
    }

    std::string sequence_reset(std::uint64_t new_seq, bool gap_fill) {
        OutboundFields f = header(msg_type::SequenceReset);
        if (gap_fill) {
            f.poss_dup = true;
            f.orig_sending_time = format_utc_timestamp(now_);
            f.body.emplace_back(tag::GapFillFlag, "Y");
        }
        f.body.emplace_back(tag::NewSeqNo, std::to_string(new_seq));
        return encode(f);
    }

    std::string logout(const std::string& text = "bye") {
        OutboundFields f = header(msg_type::Logout);
        f.body.emplace_back(tag::Text, text);
        return encode(f);
    }

    std::string new_order(const NewOrderArgs& a) {
        OutboundFields f = header(msg_type::NewOrderSingle);
        if (!a.omit_cl_ord_id) f.body.emplace_back(tag::ClOrdID, a.cl_ord_id);
        if (a.duplicate_cl_ord_id) f.body.emplace_back(tag::ClOrdID, a.cl_ord_id + "-dup");
        if (!a.account.empty()) f.body.emplace_back(tag::Account, a.account);
        if (!a.omit_symbol) f.body.emplace_back(tag::Symbol, a.symbol);
        if (!a.omit_side) f.body.emplace_back(tag::Side, a.side);
        if (!a.omit_qty) f.body.emplace_back(tag::OrderQty, a.qty);
        if (a.duplicate_qty) f.body.emplace_back(tag::OrderQty, "1000");
        if (!a.omit_ord_type) f.body.emplace_back(tag::OrdType, a.ord_type);
        if (!a.omit_price) f.body.emplace_back(tag::Price, a.price);
        if (!a.tif.empty()) f.body.emplace_back(tag::TimeInForce, a.tif);
        if (!a.omit_transact_time) {
            f.body.emplace_back(tag::TransactTime, a.transact_time_override.empty()
                                                       ? format_utc_timestamp(now_)
                                                       : a.transact_time_override);
        }
        return encode(f);
    }

    std::string cancel(const CancelArgs& a) {
        OutboundFields f = header(msg_type::OrderCancelRequest);
        f.body.emplace_back(tag::ClOrdID, a.cl_ord_id);
        if (!a.omit_orig) f.body.emplace_back(tag::OrigClOrdID, a.orig_cl_ord_id);
        f.body.emplace_back(tag::Symbol, a.symbol);
        f.body.emplace_back(tag::Side, a.side);
        if (!a.qty.empty()) f.body.emplace_back(tag::OrderQty, a.qty);
        f.body.emplace_back(tag::TransactTime, format_utc_timestamp(now_));
        return encode(f);
    }

    std::string replace(const ReplaceArgs& a) {
        OutboundFields f = header(msg_type::OrderCancelReplaceRequest);
        f.body.emplace_back(tag::ClOrdID, a.cl_ord_id);
        if (!a.omit_orig) f.body.emplace_back(tag::OrigClOrdID, a.orig_cl_ord_id);
        f.body.emplace_back(tag::Symbol, a.symbol);
        f.body.emplace_back(tag::Side, a.side);
        f.body.emplace_back(tag::OrderQty, a.qty);
        f.body.emplace_back(tag::OrdType, a.ord_type);
        f.body.emplace_back(tag::Price, a.price);
        f.body.emplace_back(tag::TimeInForce, a.tif);
        f.body.emplace_back(tag::TransactTime, format_utc_timestamp(now_));
        return encode(f);
    }

    // An arbitrary application message type, for B-17.
    std::string app_message(std::string_view mt, const CanonicalBody& body) {
        OutboundFields f = header(mt);
        for (const auto& kv : body) f.body.emplace_back(kv.first, kv.second);
        return encode(f);
    }

    // Re-send a previously built frame as a session retransmission: same
    // sequence number, 43=Y, 122 set. Used for C-02 and I-02.
    static std::string as_poss_dup(const std::string& original, WallTime now) {
        std::string out;
        rebuild_as_retransmission(original, now, &out);
        return out;
    }

    // Re-send with a FRESH sequence number and 97=Y: an application resend,
    // which is a different mechanism from a session retransmission (C-03).
    std::string as_poss_resend(const std::string& original) {
        DecodedMessage m;
        DecodeOptions opt;
        opt.max_value_bytes = 4096;
        if (decode(original, opt, &m) != WireError::None) return std::string();
        OutboundFields f = header(m.msg_type);
        f.body.emplace_back(tag::PossResend, "Y");
        for (const Field& fl : m.fields) {
            switch (fl.tag) {
                case tag::BeginString:
                case tag::BodyLength:
                case tag::MsgType:
                case tag::MsgSeqNum:
                case tag::SenderCompID:
                case tag::TargetCompID:
                case tag::SendingTime:
                case tag::OrigSendingTime:
                case tag::PossDupFlag:
                case tag::PossResend:
                case tag::CheckSum:
                    break;
                default:
                    f.body.emplace_back(fl.tag, std::string(fl.value));
            }
        }
        return encode(f);
    }

    // Deliberately corrupt helpers. These are the scripted raw-peer cases: a
    // conforming encoder cannot produce them.
    static std::string with_bad_body_length(const std::string& frame) {
        // Rewrite tag 9 to a value that no longer lands on the trailer.
        const std::size_t b = frame.find("9=");
        const std::size_t e = frame.find(kSOH, b);
        if (b == std::string::npos || e == std::string::npos) return frame;
        std::string out = frame;
        out.replace(b, e - b, "9=7");
        return out;
    }

    static std::string with_bad_checksum(const std::string& frame) {
        std::string out = frame;
        const std::size_t p = out.rfind("10=");
        if (p == std::string::npos) return out;
        // Flip the declared checksum to a value that cannot be correct.
        out[p + 3] = out[p + 3] == '0' ? '1' : '0';
        return out;
    }

    static std::string with_bad_begin_string(const std::string& frame) {
        std::string out = frame;
        const std::size_t p = out.find("8=FIX.4.4");
        if (p == std::string::npos) return out;
        out.replace(p, 9, "8=FIX.4.2");
        // BodyLength and CheckSum are unchanged on purpose: the version check
        // must fire before either is consulted.
        return out;
    }

    std::string with_seq(const std::string& original, std::uint64_t seq) {
        DecodedMessage m;
        DecodeOptions opt;
        opt.max_value_bytes = 4096;
        if (decode(original, opt, &m) != WireError::None) return original;
        OutboundFields f;
        f.msg_type = std::string(m.msg_type);
        f.seq_num = seq;
        f.sender_comp_id = sender_;
        f.target_comp_id = target_;
        f.sending_time = now_;
        for (const Field& fl : m.fields) {
            switch (fl.tag) {
                case tag::BeginString:
                case tag::BodyLength:
                case tag::MsgType:
                case tag::MsgSeqNum:
                case tag::SenderCompID:
                case tag::TargetCompID:
                case tag::SendingTime:
                case tag::CheckSum:
                    break;
                default:
                    f.body.emplace_back(fl.tag, std::string(fl.value));
            }
        }
        return lpsim::encode(f);
    }

    std::string with_comp_ids(const std::string& mt, const std::string& sender,
                              const std::string& target) {
        OutboundFields f;
        f.msg_type = mt;
        f.seq_num = next_out_++;
        f.sender_comp_id = sender;
        f.target_comp_id = target;
        f.sending_time = now_;
        f.body.emplace_back(tag::EncryptMethod, "0");
        f.body.emplace_back(tag::HeartBtInt, "30");
        return lpsim::encode(f);
    }

private:
    OutboundFields header(std::string_view mt) {
        OutboundFields f;
        f.msg_type = std::string(mt);
        f.seq_num = next_out_++;
        f.sender_comp_id = sender_;
        f.target_comp_id = target_;
        f.sending_time = now_;
        return f;
    }

    static std::string encode(const OutboundFields& f) { return lpsim::encode(f); }

    std::string sender_;
    std::string target_;
    WallTime now_;
    std::uint64_t next_out_ = 1;
};

// ---------------------------------------------------------------------------
// A wired-up simulator
// ---------------------------------------------------------------------------

inline SymbolProfile eurusd() {
    SymbolProfile p;
    p.symbol = "EURUSD";
    p.price.scale = 5;
    p.price.tick_step = 1;
    p.lot_step = 1000;
    p.quote_stale_after_ms = 2000;
    return p;
}

inline SymbolProfile gbpusd() {
    SymbolProfile p = eurusd();
    p.symbol = "GBPUSD";
    return p;
}

struct FixtureOptions {
    FillTrigger fill_trigger = FillTrigger::Scripted;
    bool emit_pending_cancel = false;
    bool emit_pending_replace = false;
    std::size_t max_live_orders = 100000;
    std::size_t max_pending_outbox = 100000;
    std::size_t max_inbox = 10000;
    bool require_credentials = false;
    bool allow_reset_on_logon = false;
    std::vector<SymbolProfile> symbols{eurusd()};
    ValidatorPolicy validator;
};

class LpFixture {
public:
    explicit LpFixture(const std::string& tag, FixtureOptions opt = {})
        : dir_(tag), opt_(std::move(opt)) {}

    // Open the store and bring the actor and session up. Returns false when
    // recovery refused to continue.
    bool start() {
        StoreConfig sc;
        sc.path = dir_.db();
        sc.session_id = "LP-SESSION-1";
        sc.sender_comp_id = "SIMLP01";
        sc.target_comp_id = "TRADER01";
        sc.max_pending_outbox = opt_.max_pending_outbox;
        sc.max_inbox = opt_.max_inbox;
        sc.max_live_orders = opt_.max_live_orders;
        if (store_.open(sc) != StoreError::None) return false;

        ActorConfig ac;
        ac.firm = "TRADER01";
        ac.session_id = sc.session_id;
        ac.symbols = opt_.symbols;
        ac.validator = opt_.validator;
        ac.fill_trigger = opt_.fill_trigger;
        ac.emit_pending_cancel = opt_.emit_pending_cancel;
        ac.emit_pending_replace = opt_.emit_pending_replace;
        ac.max_live_orders = opt_.max_live_orders;
        ac.max_pending_outbox = opt_.max_pending_outbox;

        actor_ = std::make_unique<OrderActor>(ac, store_, book_, sched_, clock_);
        if (!actor_->recover()) return false;

        SessionConfig secfg;
        secfg.session_id = sc.session_id;
        secfg.sender_comp_id = sc.sender_comp_id;
        secfg.target_comp_id = sc.target_comp_id;
        secfg.require_credentials = opt_.require_credentials;
        secfg.allow_reset_on_logon = opt_.allow_reset_on_logon;
        secfg.username = "u";
        secfg.password = "p";
        session_ = std::make_unique<AcceptorSession>(secfg, store_, *actor_, clock_,
                                                     transport_);
        if (!session_->recover()) return false;

        peer_ = std::make_unique<ClientPeer>("TRADER01", "SIMLP01", clock_.wall_now());
        return true;
    }

    // Close everything and bring it back up against the same database, the way
    // a restart after a crash does.
    bool restart() {
        session_.reset();
        actor_.reset();
        store_.close();
        book_ = QuoteBook();
        sched_.clear();
        transport_.reopen();
        transport_.clear();
        return start();
    }

    void connect() {
        transport_.reopen();
        session_->on_connected(++generation_);
    }

    std::vector<InboundVerdict> feed(const std::string& frame) {
        peer_->set_now(clock_.wall_now());
        return session_->on_bytes(frame, generation_);
    }

    // Connect and complete a logon handshake.
    bool logon(int hb = 30) {
        connect();
        peer_->set_now(clock_.wall_now());
        const auto v = feed(peer_->logon(hb, false,
                                         opt_.require_credentials ? "u" : "",
                                         opt_.require_credentials ? "p" : ""));
        return !v.empty() && v[0] == InboundVerdict::Accepted &&
               session_->phase() == SessionPhase::Active;
    }

    // Install a quote through the committed path, so inventory and version
    // history are durable exactly as a scenario would make them.
    QuoteError set_quote(const std::string& symbol, const char* bid, const char* ask,
                         Units bid_avail, Units ask_avail, std::uint64_t version,
                         std::int64_t ttl_ms = 2000) {
        QuoteSlice q;
        q.symbol = symbol;
        q.version = version;
        PriceFormat fmt = opt_.symbols.front().price;
        for (const SymbolProfile& p : opt_.symbols) {
            if (p.symbol == symbol) fmt = p.price;
        }
        if (parse_price_ticks(bid, fmt, &q.bid_ticks) != NumParse::Ok)
            return QuoteError::NotPositivePrice;
        if (parse_price_ticks(ask, fmt, &q.ask_ticks) != NumParse::Ok)
            return QuoteError::NotPositivePrice;
        q.bid_available = bid_avail;
        q.ask_available = ask_avail;
        q.valid_from_ms = now_ms();
        q.expires_at_ms = now_ms() + ttl_ms;
        q.source_fixture_id = "harness";

        ScheduledEvent ev;
        ev.event_id = "QV-" + symbol + "-" + std::to_string(version);
        ev.due_us = clock_.virtual_now_us();
        ev.kind = EventKind::QuoteUpdate;
        ev.quote = q;
        actor_->schedule(ev);
        actor_->run_due(clock_.virtual_now_us());
        const QuoteSlice* cur = book_.current(symbol);
        return (cur != nullptr && cur->version == version) ? QuoteError::Ok
                                                           : QuoteError::DuplicateVersion;
    }

    // Schedule an injected fill and run it at the current virtual time.
    //
    // expect_schedule_ok is true for almost every case: a refused SCHEDULE is
    // not a refused fill, and letting one masquerade as the other would make a
    // case pass because its event never ran. The duplicate-identity case
    // (E-05) passes false, because being refused at the scheduler is precisely
    // the behaviour it asserts.
    ActorOutcome inject_fill(const std::string& order_id, Units qty, const char* price,
                             const std::string& event_id = "",
                             bool expect_schedule_ok = true) {
        ScheduledEvent ev;
        ev.event_id = event_id.empty()
                          ? ("IF-" + order_id + "-" + std::to_string(++fill_counter_))
                          : event_id;
        ev.due_us = clock_.virtual_now_us();
        ev.kind = EventKind::InjectFill;
        ev.order_id = order_id;
        ev.qty = qty;
        PriceFormat fmt = opt_.symbols.front().price;
        parse_price_ticks(price, fmt, &ev.px_ticks);
        // A refused schedule must not look like a refused fill: the event would
        // simply never run and the case would pass for the wrong reason.
        const EnqueueResult sched = actor_->schedule(ev);
        if (expect_schedule_ok && sched != EnqueueResult::Ok)
            TT_FAIL(std::string("scheduling the fill was refused: ") +
                    std::string(to_string(sched)));
        last_schedule_ = sched;
        auto results = actor_->run_due(clock_.virtual_now_us());
        session_->dispatch_outbox();
        return results.empty() ? ActorOutcome{} : results.back();
    }

    // Reports the session actually put on the wire, decoded.
    std::vector<DecodedMessage> sent_messages(std::vector<std::string>* storage) const {
        *storage = transport_.frames();
        std::vector<DecodedMessage> out;
        for (const std::string& f : *storage) {
            DecodedMessage m;
            DecodeOptions o;
            o.max_value_bytes = 4096;
            if (decode(f, o, &m) == WireError::None) out.push_back(m);
        }
        return out;
    }

    // All reports of a given MsgType, in order.
    std::vector<DecodedMessage> sent_of_type(std::string_view mt,
                                             std::vector<std::string>* storage) const {
        std::vector<DecodedMessage> out;
        for (const DecodedMessage& m : sent_messages(storage)) {
            if (m.msg_type == mt) out.push_back(m);
        }
        return out;
    }

    // How the most recent inject_fill was received by the scheduler.
    EnqueueResult last_schedule() const { return last_schedule_; }

    std::int64_t now_ms() const { return clock_.virtual_now_us() / 1000; }
    void advance(int ms) { clock_.advance(Millis(ms)); }

    TmpDir& dir() { return dir_; }
    VirtualClock& clock() { return clock_; }
    SqliteStore& store() { return store_; }
    QuoteBook& book() { return book_; }
    Scheduler& sched() { return sched_; }
    OrderActor& actor() { return *actor_; }
    AcceptorSession& session() { return *session_; }
    ClientPeer& peer() { return *peer_; }
    FakeTransport& transport() { return transport_; }

private:
    TmpDir dir_;
    FixtureOptions opt_;
    VirtualClock clock_;
    SqliteStore store_;
    QuoteBook book_;
    Scheduler sched_;
    FakeTransport transport_;
    std::unique_ptr<OrderActor> actor_;
    std::unique_ptr<AcceptorSession> session_;
    std::unique_ptr<ClientPeer> peer_;
    std::uint64_t generation_ = 0;
    int fill_counter_ = 0;
    EnqueueResult last_schedule_ = EnqueueResult::Ok;
};

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

// Field of a decoded message, or the empty string.
inline std::string field(const DecodedMessage& m, int tag_num) {
    const auto v = m.find(tag_num);
    return v ? std::string(*v) : std::string();
}

// Every ExecutionReport and OrderCancelReject the session sent must satisfy the
// independent conformance oracle. Calling this at the end of a test is what
// stops a test from passing on the ledger while the wire is wrong (M-11).
inline void check_all_sent_conformant(LpFixture& fx) {
    std::vector<std::string> storage;
    for (const DecodedMessage& m : fx.sent_messages(&storage)) {
        if (m.msg_type != msg_type::ExecutionReport &&
            m.msg_type != msg_type::OrderCancelReject)
            continue;
        CanonicalBody body;
        for (const Field& f : m.fields) {
            switch (f.tag) {
                case tag::BeginString:
                case tag::BodyLength:
                case tag::MsgType:
                case tag::MsgSeqNum:
                case tag::SenderCompID:
                case tag::TargetCompID:
                case tag::SendingTime:
                case tag::OrigSendingTime:
                case tag::PossDupFlag:
                case tag::CheckSum:
                    break;
                default:
                    body.emplace_back(f.tag, std::string(f.value));
            }
        }
        const BuildError e = check_report_conformance(m.msg_type, body);
        if (e != BuildError::Ok) {
            TT_FAIL(std::string("non-conformant outbound report: ") +
                    std::string(to_string(e)) + " in " + wire_text(m.raw));
        }
    }
}

// ExecIDs must be unique across everything the session sent (I-05).
inline void check_exec_ids_unique(LpFixture& fx) {
    std::vector<std::string> storage;
    std::map<std::string, int> seen;
    for (const DecodedMessage& m : fx.sent_messages(&storage)) {
        if (m.msg_type != msg_type::ExecutionReport) continue;
        if (m.poss_dup) continue;  // a retransmission legitimately repeats one
        const std::string id = field(m, tag::ExecID);
        if (id.empty()) continue;
        if (++seen[id] > 1) TT_FAIL("duplicate ExecID on the wire: " + id);
    }
}

// No two original messages may share an outbound sequence number (I-15).
inline void check_outbound_seq_unique(LpFixture& fx) {
    std::vector<std::string> storage;
    std::map<std::uint64_t, int> seen;
    for (const DecodedMessage& m : fx.sent_messages(&storage)) {
        if (m.poss_dup) continue;  // replays reuse the original number by design
        if (++seen[m.seq_num] > 1)
            TT_FAIL("outbound sequence number reused: " + std::to_string(m.seq_num));
    }
}

inline void check_invariants(LpFixture& fx) {
    std::string why;
    if (!fx.actor().check_invariants(&why)) TT_FAIL("invariant violated: " + why);
}

// The full post-condition bundle every business test ends with.
inline void check_all(LpFixture& fx) {
    check_invariants(fx);
    check_all_sent_conformant(fx);
    check_exec_ids_unique(fx);
    check_outbound_seq_unique(fx);
}

}  // namespace tt
