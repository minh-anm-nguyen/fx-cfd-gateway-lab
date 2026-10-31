// Scenario fixture and event builders.
//
// The what-if catalog has 340 entries, so constructing an event has to be one
// readable line. These builders default every field to the lab profile
// (EURUSD, ACC1, buy, peer LP1) and let a case override exactly the field it
// is about.
//
// The oracle at the bottom is deliberately not the aggregate's own ledger: it
// rebuilds CumQty and notional by walking the committed journal, so a bug in
// the ledger vector or in the cached totals cannot hide behind itself (design
// doc 20.1, layer one).
#pragma once

#include <set>
#include <string>

#include "order_core/decimal.hpp"
#include "order_core/engine.hpp"
#include "tiny_test.hpp"

namespace lab {

using namespace order_core;

// --- command builders ------------------------------------------------------

inline LocalCommand cmd_new(std::string cl, std::string qty = "100000",
                            std::string px = "1.08500", std::string sym = "EURUSD",
                            std::string side = "1", std::string acct = "ACC1") {
    LocalCommand c;
    c.kind = CommandKind::New;
    c.firm = "FIRM";
    c.business_day = "2026-10-30";
    c.account = std::move(acct);
    c.symbol = std::move(sym);
    c.side_raw = std::move(side);
    c.cl_ord_id = std::move(cl);
    c.qty_text = std::move(qty);
    c.price_text = std::move(px);
    return c;
}

inline LocalCommand cmd_cancel(std::string cl, std::string orig) {
    LocalCommand c = cmd_new(std::move(cl), "", "");
    c.kind = CommandKind::Cancel;
    c.orig_cl_ord_id = std::move(orig);
    return c;
}

inline LocalCommand cmd_replace(std::string cl, std::string orig, std::string qty = "",
                                std::string px = "") {
    LocalCommand c = cmd_new(std::move(cl), std::move(qty), std::move(px));
    c.kind = CommandKind::Replace;
    c.orig_cl_ord_id = std::move(orig);
    return c;
}

inline SystemEvent sys(SystemKind k, std::string target) {
    SystemEvent s;
    s.kind = k;
    s.target_cl_ord_id = std::move(target);
    return s;
}

// --- fluent report builder -------------------------------------------------

struct R {
    PeerReport r;

    R(char msg_type, const char* exec_type, const char* ord_status) {
        r.msg_type = msg_type;
        r.exec_type = exec_type;
        r.ord_status = ord_status;
        r.account = "ACC1";
        r.symbol = "EURUSD";
        r.side_raw = "1";
        r.peer_id = "LP1";
        r.session_id = "S1";
        r.has_source_cursor = true;
    }

    R& id(std::string v) { r.exec_id = std::move(v); return *this; }
    R& cl(std::string v) { r.cl_ord_id = std::move(v); return *this; }
    R& orig(std::string v) { r.orig_cl_ord_id = std::move(v); return *this; }
    R& oid(std::string v) { r.order_id = std::move(v); return *this; }
    R& no_oid() { r.order_id.clear(); return *this; }
    R& lastq(std::string v) { r.last_qty = std::move(v); return *this; }
    R& lastpx(std::string v) { r.last_px = std::move(v); return *this; }
    R& cum(std::string v) { r.cum_qty = std::move(v); return *this; }
    R& leaves(std::string v) { r.leaves_qty = std::move(v); return *this; }
    R& oqty(std::string v) { r.order_qty = std::move(v); return *this; }
    R& px(std::string v) { r.price = std::move(v); return *this; }
    R& avgpx(std::string v) { r.avg_px = std::move(v); return *this; }
    R& acct(std::string v) { r.account = std::move(v); return *this; }
    R& sym(std::string v) { r.symbol = std::move(v); return *this; }
    R& side(std::string v) { r.side_raw = std::move(v); return *this; }
    R& no_side() { r.side_raw.clear(); return *this; }
    R& ref(std::string v) { r.exec_ref_id = std::move(v); return *this; }
    R& rejto(std::string v) { r.cxl_rej_response_to = std::move(v); return *this; }
    R& rejreason(std::string v) { r.cxl_rej_reason = std::move(v); return *this; }
    R& text(std::string v) { r.text = std::move(v); return *this; }
    R& seq(uint64_t v) { r.source_seq = v; return *this; }
    R& epoch(uint64_t v) { r.session_epoch = v; return *this; }
    R& session(std::string v) { r.session_id = std::move(v); return *this; }
    R& peer(std::string v) { r.peer_id = std::move(v); return *this; }
    R& resend(bool v = true) { r.poss_resend = v; return *this; }
    R& dup(bool v = true) { r.poss_dup = v; return *this; }
    R& gen(uint64_t v) { r.reconciliation_generation = v; return *this; }
    R& no_cursor() { r.has_source_cursor = false; return *this; }
    R& status(std::string v) { r.ord_status = std::move(v); return *this; }
    R& etype(std::string v) { r.exec_type = std::move(v); return *this; }
    R& sending(std::string v) { r.sending_time = std::move(v); return *this; }
};

// 35=8 shapes. Each names the FIX 4.4 ExecType and the OrdStatus it normally
// carries; a case that is about an inconsistent pair overrides one of them.
inline R new_ack(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-ACK-" + cl;
    // The acknowledgement is where OrderID(37) first appears, so it is derived
    // per order: one shared default would make every chain collide on it.
    const std::string oid = "LPO-" + cl;
    return R('8', exec_type::kNew, ord_status::kNew)
        .id(std::move(exec_id))
        .cl(std::move(cl))
        .oid(oid);
}
inline R rejected(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-REJ-" + cl;
    return R('8', exec_type::kRejected, ord_status::kRejected)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R trade(std::string exec_id, std::string cl) {
    return R('8', exec_type::kTrade, ord_status::kPartiallyFilled)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R pending_cancel(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-PC-" + cl;
    return R('8', exec_type::kPendingCancel, ord_status::kPendingCancel)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R canceled(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-CXL-" + cl;
    return R('8', exec_type::kCanceled, ord_status::kCanceled)
        .id(std::move(exec_id))
        .cl(std::move(cl))
        .leaves("0");
}
inline R pending_replace(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-PR-" + cl;
    return R('8', exec_type::kPendingReplace, ord_status::kPendingReplace)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R replaced(std::string cl, std::string orig, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-RPL-" + cl;
    return R('8', exec_type::kReplaced, ord_status::kNew)
        .id(std::move(exec_id))
        .cl(std::move(cl))
        .orig(std::move(orig));
}
inline R expired(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-EXP-" + cl;
    return R('8', exec_type::kExpired, ord_status::kExpired).id(std::move(exec_id)).cl(std::move(cl));
}
inline R done_for_day(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-DFD-" + cl;
    return R('8', exec_type::kDoneForDay, ord_status::kDoneForDay)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R status_snapshot(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-ST-" + cl;
    // A status response is only "complete" evidence if it identifies the order
    // on the counterparty's side too.
    const std::string oid = "LPO-" + cl;
    return R('8', exec_type::kOrderStatus, ord_status::kNew)
        .id(std::move(exec_id))
        .cl(std::move(cl))
        .oid(oid);
}
inline R trade_correct(std::string exec_id, std::string cl) {
    return R('8', exec_type::kTradeCorrect, ord_status::kPartiallyFilled)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R trade_cancel(std::string exec_id, std::string cl) {
    return R('8', exec_type::kTradeCancel, ord_status::kPartiallyFilled)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}
inline R restated(std::string exec_id, std::string cl) {
    return R('8', exec_type::kRestated, ord_status::kNew).id(std::move(exec_id)).cl(std::move(cl));
}
inline R pending_new(std::string cl, std::string exec_id = "") {
    if (exec_id.empty()) exec_id = "E-PN-" + cl;
    return R('8', exec_type::kPendingNew, ord_status::kPendingNew)
        .id(std::move(exec_id))
        .cl(std::move(cl));
}

// 35=9 OrderCancelReject. Tag 434 is mandatory: '1' answers a cancel, '2' a
// replace. It carries no ExecID.
inline R cancel_reject(std::string cl, std::string orig, std::string response_to = "1") {
    R r('9', "", "");
    r.cl(std::move(cl)).orig(std::move(orig)).rejto(std::move(response_to));
    return r;
}

// --- fixture ---------------------------------------------------------------

class Lab {
  public:
    explicit Lab(Policy p = default_lab_policy()) : pol_(std::move(p)), eng_(store_, pol_) {}

    EngineResult submit(const LocalCommand& c) { return eng_.submit(OrderEvent{c}); }
    EngineResult submit(const SystemEvent& s) { return eng_.submit(OrderEvent{s}); }
    EngineResult submit(const PeerReport& r) {
        PeerReport copy = r;
        if (copy.source_seq == 0) copy.source_seq = ++src_seq_;
        return eng_.submit(OrderEvent{copy});
    }
    EngineResult submit(const R& b) { return submit(b.r); }

    // Drives a new order all the way to Working: intent, durable dispatch,
    // acknowledgement. Returns the chain.
    OrderId working(std::string cl = "O1", std::string qty = "100000",
                    std::string px = "1.08500", std::string sym = "EURUSD") {
        const EngineResult a = submit(cmd_new(cl, qty, px, sym));
        CHECK_MSG(a.ok(), "setup: New rejected: " + a.detail);
        CHECK(submit(sys(SystemKind::Dispatched, cl)).ok());
        const EngineResult b = submit(new_ack(cl).sym(sym));
        CHECK_MSG(b.ok(), "setup: New Ack rejected: " + b.detail);
        return a.id;
    }

    // Working, then one partial fill of `qty` at `price`.
    OrderId part_filled(std::string cl = "O1", std::string order_qty = "100000",
                        std::string fill_qty = "40000", std::string fill_px = "1.08490") {
        const OrderId id = working(cl, order_qty);
        int64_t oq = 0, fq = 0;
        CHECK(parse_units(order_qty, oq) == DecimalError::None);
        CHECK(parse_units(fill_qty, fq) == DecimalError::None);
        const EngineResult t = submit(trade("X1", cl)
                                          .lastq(fill_qty)
                                          .lastpx(fill_px)
                                          .cum(std::to_string(fq))
                                          .leaves(std::to_string(oq - fq)));
        CHECK_MSG(t.ok(), "setup: fill rejected: " + t.detail);
        return id;
    }

    [[nodiscard]] const OrderAggregate& state(OrderId id) const {
        const OrderAggregate* a = eng_.find(id);
        CHECK_MSG(a != nullptr, "chain " + std::to_string(id.value) + " is not resident");
        return *a;
    }
    [[nodiscard]] Lifecycle lc(OrderId id) const { return state(id).lifecycle(); }

    Engine& engine() { return eng_; }
    MemoryStore& store() { return store_; }
    // The engine owns the live policy; a reload has to be visible here.
    const Policy& policy() const { return eng_.policy(); }
    void set_policy(Policy p) { pol_ = std::move(p); }

  private:
    Policy pol_;
    MemoryStore store_;
    Engine eng_;
    uint64_t src_seq_ = 0;
};

// --- independent oracle ----------------------------------------------------

// Rebuilt from the committed journal, not from the aggregate. Walks the events
// that were actually applied and re-derives the quantities from their raw
// text, so it shares no cached field and no ledger vector with the system
// under test.
struct JournalOracle {
    int64_t cum = 0;
    Int128 notional = 0;
    std::set<std::string> exec_ids;
    size_t applied_trades = 0;
    size_t quarantines = 0;
};

inline JournalOracle oracle(const MemoryStore& store, OrderId id) {
    JournalOracle o;
    for (const JournalRecord& rec : store.journal()) {
        if (!(rec.id == id)) continue;
        if (rec.decision == Decision::Quarantined) {
            ++o.quarantines;
            continue;
        }
        if (rec.decision != Decision::Applied) continue;
        const auto* r = std::get_if<PeerReport>(&rec.event);
        if (r == nullptr || r->exec_type != exec_type::kTrade) continue;
        if (!r->last_qty.has_value() || !r->last_px.has_value()) continue;
        int64_t q = 0;
        int64_t p = 0;
        if (parse_units(*r->last_qty, q) != DecimalError::None) continue;
        // EURUSD and the other lab symbols are all decimal ticks; the oracle
        // reads the scale from the policy the same way the reducer does, but
        // accumulates independently.
        if (parse_ticks(*r->last_px, 1, 100000, p) != DecimalError::None) {
            if (parse_ticks(*r->last_px, 1, 1000, p) != DecimalError::None) continue;
        }
        o.exec_ids.insert(r->exec_id);
        o.cum += q;
        o.notional += static_cast<Int128>(q) * p;
        ++o.applied_trades;
    }
    return o;
}

// Shorthand used all over the suite.
inline void expect(const EngineResult& r, Decision d, Reason reason) {
    CHECK_MSG(r.decision == d, std::string("decision was ") + to_string(r.decision) + " (" +
                                   to_string(r.reason) + ": " + r.detail + "), expected " +
                                   to_string(d));
    CHECK_MSG(r.reason == reason, std::string("reason was ") + to_string(r.reason) + " (" +
                                      r.detail + "), expected " + to_string(reason));
}

inline void expect_decision(const EngineResult& r, Decision d) {
    CHECK_MSG(r.decision == d, std::string("decision was ") + to_string(r.decision) + " (" +
                                   to_string(r.reason) + ": " + r.detail + "), expected " +
                                   to_string(d));
}

inline void expect_quarantine(const EngineResult& r, Reason reason) {
    expect(r, Decision::Quarantined, reason);
}

inline void expect_reject(const EngineResult& r, Reason reason) {
    expect(r, Decision::RejectLocal, reason);
}

}  // namespace lab
