// The lab fixture.
//
// Section 23 of the design doc is strict about the oracle: "do not treat a
// live process as a PASS", and the independent LP ledger must not share
// reducers with the system under test. LpOracle below therefore keeps its
// own books in plain arithmetic, with no reference to OrderState or the
// risk reducer, so that "the engine says one fill and the counterparty says
// one fill" is two independent statements rather than one restated.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "jrnl/config.hpp"
#include "jrnl/outbox.hpp"
#include "jrnl/recovery.hpp"
#include "jrnl/replay.hpp"
#include "jrnl/retention.hpp"
#include "jrnl/service.hpp"
#include "jrnl/snapshot.hpp"
#include "jrnl/stable_send.hpp"
#include "jrnl/store.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

namespace lab {

using namespace jrnl;

// An independent record of what actually went out on the wire and what the
// counterparty did with it. Deliberately dumb: counters and sets.
class LpOracle {
public:
    struct Wire {
        SessionKey session;
        std::uint64_t seq = 0;
        std::string payload;
    };

    // Called from the session's write path.
    bool accept(const SessionKey& session, std::uint64_t seq, const std::string& payload) {
        writes_.push_back(Wire{session, seq, payload});
        ++per_payload_[payload];
        distinct_sequences_[session].insert(seq);
        return !refuse_writes_;
    }

    // How many times these exact bytes were put on the wire. The release
    // condition for JI-19 is that an original NewOrderSingle appears once.
    std::size_t wire_count(const std::string& payload) const {
        const auto it = per_payload_.find(payload);
        return it == per_payload_.end() ? 0 : it->second;
    }
    std::size_t total_writes() const { return writes_.size(); }
    std::size_t distinct_sequences(const SessionKey& s) const {
        const auto it = distinct_sequences_.find(s);
        return it == distinct_sequences_.end() ? 0 : it->second.size();
    }
    const std::vector<Wire>& writes() const { return writes_; }
    void refuse_writes(bool v) { refuse_writes_ = v; }

    // The counterparty's own trade ledger, keyed by execution identity.
    void record_trade(const ExecId& exec_id, std::int64_t qty, std::int64_t px) {
        auto& t = trades_[exec_id];
        ++t.reports;
        t.qty = qty;
        t.px = px;
    }
    // "economic_trade_count(E100) == 1" from the scenario DSL in section
    // 26.1: a trade exists once no matter how many times it was reported.
    std::size_t economic_trade_count(const ExecId& exec_id) const {
        return trades_.count(exec_id);
    }
    std::size_t report_count(const ExecId& exec_id) const {
        const auto it = trades_.find(exec_id);
        return it == trades_.end() ? 0 : it->second.reports;
    }
    std::int64_t traded_quantity() const {
        std::int64_t total = 0;
        for (const auto& e : trades_) total += e.second.qty;
        return total;
    }
    std::size_t distinct_trades() const { return trades_.size(); }

private:
    struct Trade {
        std::size_t reports = 0;
        std::int64_t qty = 0;
        std::int64_t px = 0;
    };
    std::vector<Wire> writes_;
    std::map<std::string, std::size_t> per_payload_;
    std::map<SessionKey, std::set<std::uint64_t>> distinct_sequences_;
    std::map<ExecId, Trade> trades_;
    bool refuse_writes_ = false;
};

// Everything one scenario needs, wired together the way the deployment
// diagram in section 5 wires it.
class Lab {
public:
    static constexpr const char* kTenant = "t1";
    static constexpr const char* kAccount = "A1";
    static constexpr const char* kDomain = "D1";
    static constexpr const char* kSession = "S1";
    static constexpr const char* kLp = "LP1";

    explicit Lab(const std::string& label, std::int64_t limit = 1000000)
        : dir_(label), clock_(1767225600000) {
        open(limit);
    }

    // Reopens the same files, as a restarted process would. Everything
    // in-memory is discarded; only what was committed comes back.
    void restart(std::int64_t limit = -1) {
        service_.reset();
        dispatcher_.reset();
        supervisor_.reset();
        snapshots_.reset();
        link_.reset();
        store_.reset();
        open(limit, /*reset_limit=*/false);
    }

    SqliteAtomicStore& store() { return *store_; }
    StableSendStore& link() { return *link_; }
    GatewayService& service() { return *service_; }
    OutboxDispatcher& dispatcher() { return *dispatcher_; }
    SnapshotStore& snapshots() { return *snapshots_; }
    RecoverySupervisor& supervisor() { return *supervisor_; }
    LpOracle& lp() { return lp_; }
    ManualClock& clock() { return clock_; }
    Metrics& metrics() { return metrics_; }
    const tt::TempDir& dir() const { return dir_; }
    std::string db_path() const { return dir_.file("journal.db"); }
    std::string link_path() const { return dir_.file("session.db"); }

    FenceToken token() const { return token_; }

    // Brings the supervisor up and takes the fence. Most scenarios start
    // here; the ones that are about recovery call verify_and_replay
    // themselves with the mode they care about.
    RecoveryReport start(RecoveryMode mode = RecoveryMode::ProcessRestart) {
        const RecoveryReport r = supervisor_->verify_and_replay(mode);
        token_ = supervisor_->token(kDomain);
        return r;
    }

    // Convenience builders. Every field a scenario does not care about gets
    // a stable default, so a test reads as the one thing it is varying.
    NewOrderRequest new_order(const std::string& cmd, const std::string& chain,
                              std::int64_t qty, std::int64_t px = 10000) const {
        NewOrderRequest r;
        r.tenant = kTenant;
        r.account = kAccount;
        r.command_id = cmd;
        r.chain = chain;
        r.cl_ord_id = cmd + "-cl";
        r.symbol = "EURUSD";
        r.side = "BUY";
        r.order_qty = qty;
        r.limit_px = px;
        r.risk_domain = kDomain;
        r.session_key = kSession;
        r.fix_payload = "35=D|11=" + r.cl_ord_id + "|38=" + std::to_string(qty) +
                        "|44=" + std::to_string(px);
        return r;
    }

    CancelRequest cancel(const std::string& cmd, const std::string& chain) const {
        CancelRequest r;
        r.tenant = kTenant;
        r.account = kAccount;
        r.command_id = cmd;
        r.chain = chain;
        r.cl_ord_id = cmd + "-cl";
        r.risk_domain = kDomain;
        r.session_key = kSession;
        r.fix_payload = "35=F|11=" + r.cl_ord_id;
        return r;
    }

    ReplaceRequest replace(const std::string& cmd, const std::string& chain, std::int64_t qty,
                           std::int64_t px = 10000) const {
        ReplaceRequest r;
        r.tenant = kTenant;
        r.account = kAccount;
        r.command_id = cmd;
        r.chain = chain;
        r.cl_ord_id = cmd + "-cl";
        r.order_qty = qty;
        r.limit_px = px;
        r.risk_domain = kDomain;
        r.session_key = kSession;
        r.fix_payload = "35=G|11=" + r.cl_ord_id + "|38=" + std::to_string(qty);
        return r;
    }

    ExecutionReport fill(const std::string& chain, const std::string& exec_id,
                         std::int64_t qty, std::int64_t px = 10000,
                         const std::string& originating = "") {
        ExecutionReport r;
        r.tenant = kTenant;
        r.session_key = kSession;
        r.session_epoch = 1;
        r.receipt_id = ++receipt_seq_;
        r.lp_namespace = kLp;
        r.exec_id = exec_id;
        r.exec_type = "TRADE";
        r.chain = chain;
        r.account = kAccount;
        r.risk_domain = kDomain;
        r.last_qty = qty;
        r.last_px = px;
        r.originating_command = originating;
        r.raw = "35=8|17=" + exec_id + "|150=F|32=" + std::to_string(qty) +
                "|31=" + std::to_string(px) + "|r=" + std::to_string(r.receipt_id);
        return r;
    }

    ExecutionReport terminal(const std::string& chain, const std::string& type,
                             const std::string& originating = "") {
        ExecutionReport r;
        r.tenant = kTenant;
        r.session_key = kSession;
        r.session_epoch = 1;
        r.receipt_id = ++receipt_seq_;
        r.lp_namespace = kLp;
        r.exec_type = type;
        r.chain = chain;
        r.account = kAccount;
        r.risk_domain = kDomain;
        r.originating_command = originating;
        r.raw = "35=8|150=" + type + "|r=" + std::to_string(r.receipt_id);
        return r;
    }

    // Puts the counterparty's own ledger in step with a report the test is
    // about to deliver, so the independent oracle has something to disagree
    // with if the engine gets it wrong.
    void lp_executes(const ExecutionReport& r) {
        if (r.exec_type == "TRADE") lp_.record_trade(r.exec_id, r.last_qty, r.last_px);
    }

    // Deliver a report through P01's durable inbox, the way the real path
    // goes: persist raw, hand over, apply, acknowledge.
    CommitOutcome deliver(const ExecutionReport& r, bool ack = true) {
        RawReceipt raw;
        raw.session_key = r.session_key;
        raw.session_epoch = r.session_epoch;
        raw.receipt_id = r.receipt_id;
        raw.raw = r.raw;
        raw.raw_hash = sha256(r.raw);
        link_->persist_raw(raw);
        lp_executes(r);
        const CommitOutcome out = service_->apply_report(r, token_);
        if (ack && out.durable()) service_->ack_delivery(r);
        return out;
    }

    std::size_t open_orders() const {
        Stmt s;
        if (const_cast<Db&>(store_->db()).prepare(
                "SELECT COUNT(*) FROM orders WHERE tenant_id=?1 AND leaves_qty>0;", &s) !=
            StoreError::None) {
            return 0;
        }
        s.bind_text(1, kTenant);
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) return 0;
        return static_cast<std::size_t>(s.col_i64(0));
    }

    std::size_t row_count(const std::string& table) const {
        Stmt s;
        if (const_cast<Db&>(store_->db())
                .prepare("SELECT COUNT(*) FROM " + table + " WHERE tenant_id=?1;", &s) !=
            StoreError::None) {
            return 0;
        }
        s.bind_text(1, kTenant);
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) return 0;
        return static_cast<std::size_t>(s.col_i64(0));
    }

    std::int64_t scalar(const std::string& sql) const {
        Stmt s;
        if (const_cast<Db&>(store_->db()).prepare(sql, &s) != StoreError::None) return -1;
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) return -1;
        return s.col_i64(0);
    }

    OrderState order(const std::string& chain) const {
        OrderState o;
        store_->load_order(kTenant, chain, &o);
        return o;
    }

    RiskDomainState risk() const {
        RiskDomainState r;
        store_->load_risk(kTenant, kDomain, &r);
        return r;
    }

private:
    void open(std::int64_t limit, bool reset_limit = true) {
        SqliteAtomicStore::Options sopt;
        sopt.path = db_path();
        sopt.clock = &clock_;
        std::string why;
        const StoreError e = SqliteAtomicStore::open(sopt, &store_, &why);
        if (e != StoreError::None) {
            throw tt::AssertionFailure("cannot open store: " + why + " (" +
                                       std::string(to_string(e)) + ")");
        }

        StableSendStore::Options lopt;
        lopt.path = link_path();
        lopt.clock = &clock_;
        lopt.wire = [this](const SessionKey& s, std::uint64_t seq, const std::string& p) {
            return lp_.accept(s, seq, p);
        };
        if (StableSendStore::open(lopt, &link_, &why) != StoreError::None) {
            throw tt::AssertionFailure("cannot open session store: " + why);
        }

        RecoverySupervisor::Options ropt;
        ropt.tenant = kTenant;
        ropt.owner = "gateway-lab";
        ropt.domains = {kDomain};
        ropt.metrics = &metrics_;
        supervisor_ = std::make_unique<RecoverySupervisor>(*store_, ropt);

        GatewayService::Options gopt;
        gopt.gate = supervisor_.get();
        gopt.link = link_.get();
        gopt.metrics = &metrics_;
        service_ = std::make_unique<GatewayService>(*store_, gopt);

        OutboxDispatcher::Options dopt;
        dopt.owner = "dispatcher-lab";
        dispatcher_ = std::make_unique<OutboxDispatcher>(*store_, *link_, dopt);

        snapshots_ = std::make_unique<SnapshotStore>(*store_);

        // A fresh lab needs a risk domain before anything can reserve. On a
        // restart the limit is already in the journal, so it is not reset.
        if (reset_limit && limit > 0) {
            FenceToken t = 0;
            store_->acquire_fence(kTenant, kDomain, "bootstrap", 60000, &t);
            const CommitOutcome o = service_->set_risk_limit(kTenant, kDomain, limit, t);
            if (o.kind != CommitKind::Committed) {
                throw tt::AssertionFailure("cannot set the lab risk limit: " + o.detail);
            }
        }
    }

    tt::TempDir dir_;
    ManualClock clock_;
    Metrics metrics_;
    LpOracle lp_;
    std::unique_ptr<SqliteAtomicStore> store_;
    std::unique_ptr<StableSendStore> link_;
    std::unique_ptr<RecoverySupervisor> supervisor_;
    std::unique_ptr<GatewayService> service_;
    std::unique_ptr<OutboxDispatcher> dispatcher_;
    std::unique_ptr<SnapshotStore> snapshots_;
    FenceToken token_ = 0;
    std::uint64_t receipt_seq_ = 100;
};

// Every scenario asserts this before it finishes. Section 23's DB diff
// oracle: whatever the test did, the journal must still verify and the
// projections must still equal what replaying it produces.
inline void expect_consistent(Lab& l, const char* where) {
    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    if (!v.ok()) {
        std::string msg = std::string(where) + ": integrity verification failed";
        for (const std::string& d : v.details) msg += "\n          - " + d;
        TT_FAIL(msg);
    }
}

}  // namespace lab
