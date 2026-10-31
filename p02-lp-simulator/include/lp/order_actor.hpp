// The LP order actor: the single writer of business state.
//
// Reference: design doc 5.1 (state ownership), 5.2 (critical write path), 9
// (message flows and race semantics) and 13.1 (IOrderActor).
//
// Invariant I-02 says one canonical order chain is mutated by one actor in the
// total order of the event log. That is enforced structurally here: every
// mutation path ends in exactly one commit_business() call carrying the whole
// effect of one decision, and nothing in this class writes a socket. The
// reports it produces are rows in the durable outbox; the session layer picks
// them up afterwards through send_once().
//
// Policy decisions pinned in this class, each because the design doc leaves the
// choice to the profile and the matrix then asserts the choice:
//
//   * A duplicate business request produces NO new report. The original report
//     is already in the outbound journal, and FIX already has a mechanism for a
//     client that lost it: ResendRequest. Minting a fresh report for every
//     resend would make a resend storm unbounded and would put two live
//     ExecIDs behind one decision. C-02, C-03, C-10 and PB-04 assert the
//     "no second order, no second fill" half of this; K-02 and PB-03 cover
//     recovering the lost report through the session layer.
//   * A request whose ClOrdID was used before with different semantics is
//     refused and the existing chain is untouched (C-04..C-06, PB-05).
//   * OrigClOrdID must name the LAST accepted version. Pointing at a superseded
//     version is refused rather than resolved, because guessing which version
//     the client meant is how the wrong order gets cancelled (G-11, F-08).
//   * Replace below CumQty is refused. FIX permits other readings by bilateral
//     agreement; this profile picks the strict one and says so (G-04, ADR-10).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lp/clock.hpp"
#include "lp/domain.hpp"
#include "lp/ids.hpp"
#include "lp/matcher.hpp"
#include "lp/quote_book.hpp"
#include "lp/report_builder.hpp"
#include "lp/scheduler.hpp"
#include "lp/store.hpp"
#include "lp/validator.hpp"

namespace lpsim {

// Design doc 8.1: exactly one of these is active. Enabling both would let a
// fixture auto-fill on New and then inject the same fill again.
enum class FillTrigger { Scripted, AutoOnNewOrQuote };

std::string_view to_string(FillTrigger t);

struct ActorConfig {
    std::string firm = "TRADER01";  // the counterparty firm this profile serves
    std::string session_id = "LP-SESSION-1";
    std::vector<SymbolProfile> symbols;
    ValidatorPolicy validator;

    FillTrigger fill_trigger = FillTrigger::Scripted;

    // When set, a cancel or replace is acknowledged with 150=6 / 150=E and
    // only completes when the scenario finalises it. That two-step form is what
    // makes the pending-versus-fill races of F-09, F-10, G-13 and G-14
    // expressible; the one-step form is the ordinary case (F-01).
    bool emit_pending_cancel = false;
    bool emit_pending_replace = false;

    std::size_t max_live_orders = 100000;

    // Admission stops before the outbox backlog can grow past this, so the
    // actor never produces a fill it cannot durably report (L-03, PB-14).
    std::size_t max_pending_outbox = 100000;
};

enum class ActorState { Initializing, Ready, Halted };

std::string_view to_string(ActorState s);

// What one command or scheduled event did, for the harness and the audit log.
struct ActorOutcome {
    bool accepted = false;
    BizReject reject = BizReject::None;
    NoTrade no_trade = NoTrade::None;
    StoreError store = StoreError::None;
    BuildError build = BuildError::Ok;
    bool duplicate_suppressed = false;
    std::string order_id;
    std::vector<std::string> emitted_event_ids;
    std::string detail;
};

class OrderActor {
public:
    OrderActor(ActorConfig cfg, IBusinessStore& store, QuoteBook& book, Scheduler& sched,
               IClock& clock);

    // Load persisted state. Returns false when recovery found something it will
    // not trade through, in which case state() is Halted and halt_reason() says
    // why (design doc 10.4 step 7).
    bool recover();

    ActorState state() const { return state_; }
    HaltReason halt_reason() const { return halt_; }
    const std::string& halt_detail() const { return halt_detail_; }

    // The one entry point for validated business commands, called from the
    // durable inbox in sequence order.
    ActorOutcome on_command(const AppCommand& cmd);

    // Refuse a command the validator already rejected.
    //
    // This is a SEPARATE entry point from on_command on purpose. Routing a
    // failed validation back through on_command would re-enter the accept path
    // and the reason would be lost, which is how an order with an unknown
    // symbol or no price ends up accepted (B-05, B-10, B-11, C-16, C-17).
    ActorOutcome reject_command(const AppCommand& cmd, BizReject why);

    // An application message this profile does not implement: answered with
    // 35=j rather than a session reject (B-17).
    ActorOutcome on_unsupported(std::string_view ref_msg_type, std::uint64_t ref_seq_num,
                                std::string_view ref_id);

    // Run every event due at or before upto_us, in canonical order.
    std::vector<ActorOutcome> run_due(VirtualTimeUs upto_us);

    EnqueueResult schedule(ScheduledEvent e);

    // Read-only views for the harness, the oracle and metrics.
    const OrderChain* find_order(std::string_view order_id) const;
    const OrderChain* find_by_cl_ord_id(std::string_view cl_ord_id) const;
    std::vector<const OrderChain*> chains() const;
    std::size_t live_order_count() const;
    std::size_t fill_count() const { return fill_count_; }
    // Distinct ledger fills on one chain. Scenario assertions need this per
    // order, not in aggregate: "count_distinct_fills(O1) == 2" is a statement
    // about O1 (PB-06).
    std::size_t fill_count_for(std::string_view order_id) const;

    // Invariants I-03, I-04, I-06 and I-12 re-checked against in-memory state.
    // Called after every commit; a failure halts the actor.
    bool check_invariants(std::string* why) const;

    const AppValidator& validator() const { return validator_; }

private:
    // Each handler returns a fully formed decision; commit() is the only writer.
    ActorOutcome handle_new(const AppCommand& cmd);
    ActorOutcome handle_cancel(const AppCommand& cmd);
    ActorOutcome handle_replace(const AppCommand& cmd);

    ActorOutcome do_inject_fill(const ScheduledEvent& ev);
    ActorOutcome do_auto_match(const ScheduledEvent& ev);
    ActorOutcome do_quote_update(const ScheduledEvent& ev);
    ActorOutcome do_finalize_cancel(const ScheduledEvent& ev);
    ActorOutcome do_finalize_replace(const ScheduledEvent& ev);
    ActorOutcome do_expire(const ScheduledEvent& ev);

    // Shared helpers -------------------------------------------------------
    const RegistryRow* lookup_request(const BusinessKey& key) const;
    const SymbolProfile* symbol_of(std::string_view s) const;
    PriceFormat price_format(std::string_view symbol) const;

    // Build a reject answer for a New (35=8) or a Cancel/Replace (35=9).
    bool push_reject(const AppCommand& cmd, BizReject why, const OrderChain* chain,
                     bool unknown_order, BusinessTxn* txn, ActorOutcome* out);
    bool push_report(ReportRequest req, ReportType type, BusinessTxn* txn,
                     ActorOutcome* out);

    // Apply a fill to a chain copy and append the trade report.
    bool apply_fill(OrderChain* chain, const MatchPlan& plan, BusinessTxn* txn,
                    ActorOutcome* out);

    // Commit one decision and mirror it into memory. On failure the in-memory
    // state is left untouched, which is what makes a rolled-back transaction
    // invisible to the next command (J-03, J-11).
    bool commit(BusinessTxn& txn, const std::vector<OrderChain>& updated,
                ActorOutcome* out);

    void halt(HaltReason r, std::string detail);
    std::string now_utc() const;
    std::int64_t now_ms() const;

    ActorConfig cfg_;
    IBusinessStore& store_;
    QuoteBook& book_;
    Scheduler& sched_;
    IClock& clock_;
    AppValidator validator_;

    IdAllocator order_ids_;
    IdAllocator exec_ids_;
    IdAllocator fill_ids_;
    IdAllocator event_ids_;

    ActorState state_ = ActorState::Initializing;
    HaltReason halt_ = HaltReason::None;
    std::string halt_detail_;

    std::map<std::string, OrderChain> chains_;          // by order_id
    std::map<std::string, RegistryRow> registry_;       // by business key string
    std::map<std::string, std::string> accepted_index_;  // cl_ord_id -> order_id
    std::map<std::string, Units> consumed_by_slice_;     // slice_id -> units (I-12)
    std::map<std::string, std::size_t> fills_by_order_;   // order_id -> ledger rows
    std::size_t fill_count_ = 0;
    std::string business_day_;
};

}  // namespace lpsim
