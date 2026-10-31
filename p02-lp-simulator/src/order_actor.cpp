#include "lp/order_actor.hpp"

#include <algorithm>

namespace lpsim {

std::string_view to_string(FillTrigger t) {
    switch (t) {
        case FillTrigger::Scripted: return "scripted";
        case FillTrigger::AutoOnNewOrQuote: return "auto_on_new_or_quote";
    }
    return "?";
}

std::string_view to_string(ActorState s) {
    switch (s) {
        case ActorState::Initializing: return "INITIALIZING";
        case ActorState::Ready: return "READY";
        case ActorState::Halted: return "HALTED";
    }
    return "?";
}

OrderActor::OrderActor(ActorConfig cfg, IBusinessStore& store, QuoteBook& book,
                       Scheduler& sched, IClock& clock)
    : cfg_(std::move(cfg)),
      store_(store),
      book_(book),
      sched_(sched),
      clock_(clock),
      validator_(cfg_.symbols, cfg_.validator),
      order_ids_(make_order_id_allocator()),
      exec_ids_(make_exec_id_allocator()),
      fill_ids_(make_fill_id_allocator()),
      event_ids_(make_event_id_allocator()) {
    business_day_ = business_day_utc(clock_.wall_now());
    for (const SymbolProfile& s : cfg_.symbols) book_.register_symbol(s.symbol);
}

std::string OrderActor::now_utc() const { return format_utc_timestamp(clock_.wall_now()); }

std::int64_t OrderActor::now_ms() const { return clock_.virtual_now_us() / 1000; }

void OrderActor::halt(HaltReason r, std::string detail) {
    state_ = ActorState::Halted;
    halt_ = r;
    halt_detail_ = std::move(detail);
}

bool OrderActor::recover() {
    const RecoveryReport rep = store_.recover_and_verify();
    if (!rep.ok) {
        halt(rep.halt, rep.detail);
        return false;
    }

    chains_.clear();
    registry_.clear();
    accepted_index_.clear();
    consumed_by_slice_.clear();
    fills_by_order_.clear();
    fill_count_ = rep.fills.size();

    for (const OrderChain& c : rep.chains) chains_[c.order_id] = c;
    for (const RegistryRow& r : rep.registry) {
        registry_[to_string(r.key)] = r;
        // Only an ACCEPTED request ever became a version of a chain, so only
        // those populate the version index that OrigClOrdID is resolved
        // against (G-11, F-08).
        if (r.outcome == RequestOutcome::Accepted && !r.order_id.empty())
            accepted_index_[r.key.cl_ord_id] = r.order_id;
    }
    for (const Fill& f : rep.fills) {
        consumed_by_slice_[f.liquidity_slice] += f.qty;
        ++fills_by_order_[f.order_id];
    }

    book_.restore(rep.quotes);
    sched_.restore(rep.pending_events, rep.executed_event_ids);

    order_ids_.set_next(rep.next_order_counter);
    exec_ids_.set_next(rep.next_exec_counter);
    fill_ids_.set_next(rep.next_fill_counter);
    event_ids_.set_next(rep.next_event_counter);

    std::string why;
    if (!check_invariants(&why)) {
        halt(HaltReason::InvariantViolated, why);
        return false;
    }

    state_ = ActorState::Ready;
    return true;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

const RegistryRow* OrderActor::lookup_request(const BusinessKey& key) const {
    const auto it = registry_.find(to_string(key));
    return it == registry_.end() ? nullptr : &it->second;
}

const SymbolProfile* OrderActor::symbol_of(std::string_view s) const {
    return validator_.find_symbol(s);
}

PriceFormat OrderActor::price_format(std::string_view symbol) const {
    const SymbolProfile* p = symbol_of(symbol);
    return p != nullptr ? p->price : PriceFormat{};
}

const OrderChain* OrderActor::find_order(std::string_view order_id) const {
    const auto it = chains_.find(std::string(order_id));
    return it == chains_.end() ? nullptr : &it->second;
}

const OrderChain* OrderActor::find_by_cl_ord_id(std::string_view cl_ord_id) const {
    const auto it = accepted_index_.find(std::string(cl_ord_id));
    if (it == accepted_index_.end()) return nullptr;
    return find_order(it->second);
}

std::vector<const OrderChain*> OrderActor::chains() const {
    std::vector<const OrderChain*> out;
    out.reserve(chains_.size());
    for (const auto& kv : chains_) out.push_back(&kv.second);
    return out;
}

std::size_t OrderActor::fill_count_for(std::string_view order_id) const {
    const auto it = fills_by_order_.find(std::string(order_id));
    return it == fills_by_order_.end() ? 0 : it->second;
}

std::size_t OrderActor::live_order_count() const {
    std::size_t n = 0;
    for (const auto& kv : chains_) {
        if (is_active(kv.second.base)) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Report assembly
// ---------------------------------------------------------------------------

bool OrderActor::push_report(ReportRequest req, ReportType type, BusinessTxn* txn,
                             ActorOutcome* out) {
    req.type = type;
    CanonicalBody body;
    const BuildError be = build_report(req, &body);
    if (be != BuildError::Ok) {
        // A malformed report is refused BEFORE the outbox commit, so nothing
        // the client could misread ever becomes durable (PB-17, E-15..E-18).
        out->build = be;
        out->detail = std::string("report builder refused: ") + std::string(to_string(be));
        return false;
    }

    OutboxEntry e;
    e.event_id = event_ids_.allocate();
    e.order_id = req.chain != nullptr ? req.chain->order_id : std::string();
    e.type = type;
    e.body = std::move(body);
    e.exec_id = req.exec_id;
    e.send_state = SendState::Pending;
    e.created_at_utc = now_utc();
    out->emitted_event_ids.push_back(e.event_id);
    txn->outbox.push_back(std::move(e));
    return true;
}

bool OrderActor::push_reject(const AppCommand& cmd, BizReject why, const OrderChain* chain,
                             bool unknown_order, BusinessTxn* txn, ActorOutcome* out) {
    ReportRequest req;
    req.chain = chain;
    req.price = price_format(cmd.symbol.empty() && chain != nullptr ? chain->symbol
                                                                   : cmd.symbol);
    req.cl_ord_id = cmd.key.cl_ord_id;
    req.orig_cl_ord_id = cmd.orig_cl_ord_id;
    req.reject = why;
    req.unknown_order = unknown_order;

    if (cmd.type == RequestType::New) {
        // A refused New is an ExecutionReport with ExecType=8 (design doc 7.2),
        // describing the refused request rather than any existing chain.
        req.exec_id = exec_ids_.allocate();
        req.symbol = cmd.symbol;
        req.side = cmd.side;
        req.order_qty = cmd.qty;
        if (!push_report(std::move(req), ReportType::ExecRejected, txn, out)) return false;
    } else {
        // A refused Cancel or Replace is 35=9, never 35=8 (F-16).
        req.ref_msg_type = std::string(cmd.type == RequestType::Replace
                                           ? msg_type::OrderCancelReplaceRequest
                                           : msg_type::OrderCancelRequest);
        if (!push_report(std::move(req), ReportType::CancelReject, txn, out)) return false;
    }

    // Only a request that actually carried a ClOrdID can be recorded: without
    // one there is no business key to record it under, and inventing a key
    // would make a later legitimate request look like a duplicate.
    if (cmd.has_cl_ord_id && !cmd.key.cl_ord_id.empty()) {
        RegistryRow row;
        row.key = cmd.key;
        row.type = cmd.type;
        row.canonical_hash = cmd.canonical_hash;
        // A refusal is NOT recorded against the chain it was refused against:
        // doing so would make the registry claim the request belonged to that
        // order (C-13).
        row.order_id = (why == BizReject::ConflictingRequest || chain == nullptr)
                           ? std::string()
                           : chain->order_id;
        row.outcome = RequestOutcome::Rejected;
        row.reject = why;
        txn->registry.push_back(row);
    }

    out->reject = why;
    out->accepted = false;
    return true;
}

ActorOutcome OrderActor::reject_command(const AppCommand& cmd, BizReject why) {
    ActorOutcome out;
    if (state_ != ActorState::Ready) {
        out.detail = "actor is not ready";
        out.reject = BizReject::NotActive;
        return out;
    }
    BusinessTxn txn;
    std::vector<OrderChain> none;

    // The refusal answers the request against whatever chain the request named,
    // when one can be resolved: F-17 requires a cancel reject to show the
    // current state of the real order.
    const OrderChain* target = nullptr;
    if (cmd.type != RequestType::New && !cmd.orig_cl_ord_id.empty()) {
        const auto it = accepted_index_.find(cmd.orig_cl_ord_id);
        if (it != accepted_index_.end()) target = find_order(it->second);
    }
    if (!push_reject(cmd, why, target, target == nullptr, &txn, &out)) return out;
    commit(txn, none, &out);
    return out;
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

bool OrderActor::commit(BusinessTxn& txn, const std::vector<OrderChain>& updated,
                        ActorOutcome* out) {
    txn.chains = updated;
    txn.next_order_counter = order_ids_.next_counter();
    txn.next_exec_counter = exec_ids_.next_counter();
    txn.next_fill_counter = fill_ids_.next_counter();
    txn.next_event_counter = event_ids_.next_counter();

    const StoreError se = store_.commit_business(txn);
    if (se != StoreError::None) {
        out->store = se;
        out->accepted = false;
        out->detail = std::string("commit refused: ") + std::string(to_string(se));
        // Fail closed: no report, no fill, no state change. A store that cannot
        // commit must not leave the actor believing it did (J-11, PB-11).
        halt(HaltReason::StoreCommitFailed, out->detail);
        return false;
    }

    // Mirror the committed decision into memory, in the same order the store
    // applied it.
    for (const QuoteSlice& q : txn.quote_updates) book_.apply_update(q);
    for (const QuoteConsumption& qc : txn.quote_consumptions) {
        book_.consume(qc.symbol, qc.version, qc.taker_side, qc.qty);
    }
    for (const OrderChain& c : txn.chains) chains_[c.order_id] = c;
    for (const RegistryRow& r : txn.registry) {
        registry_[to_string(r.key)] = r;
        if (r.outcome == RequestOutcome::Accepted && !r.order_id.empty())
            accepted_index_[r.key.cl_ord_id] = r.order_id;
    }
    for (const Fill& f : txn.fills) {
        consumed_by_slice_[f.liquidity_slice] += f.qty;
        ++fills_by_order_[f.order_id];
        ++fill_count_;
    }
    for (const std::string& id : txn.executed_event_ids) sched_.mark_executed(id);
    for (const ScheduledEvent& e : txn.new_events) sched_.enqueue(e);

    std::string why;
    if (!check_invariants(&why)) {
        halt(HaltReason::InvariantViolated, why);
        out->detail = why;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

ActorOutcome OrderActor::on_command(const AppCommand& cmd) {
    ActorOutcome out;
    if (state_ != ActorState::Ready) {
        // A halted actor produces nothing: no reject, no report, no fill. The
        // operator resolves it (I-19).
        out.detail = "actor is not ready";
        out.reject = BizReject::NotActive;
        return out;
    }
    switch (cmd.type) {
        case RequestType::New: return handle_new(cmd);
        case RequestType::Cancel: return handle_cancel(cmd);
        case RequestType::Replace: return handle_replace(cmd);
    }
    return out;
}

ActorOutcome OrderActor::handle_new(const AppCommand& cmd) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    // --- business idempotency ---------------------------------------------
    if (const RegistryRow* prior = lookup_request(cmd.key)) {
        if (prior->canonical_hash == cmd.canonical_hash) {
            // The same request again, by any route: a FIX retransmission the
            // inbox missed, or an application resend with a fresh sequence
            // number. No second chain, no second fill, no new report.
            out.duplicate_suppressed = true;
            out.accepted = prior->outcome == RequestOutcome::Accepted;
            out.order_id = prior->order_id;
            out.detail = "duplicate request suppressed";
            return out;
        }
        // Same ClOrdID, different meaning. The existing chain is not touched.
        const OrderChain* existing =
            prior->order_id.empty() ? nullptr : find_order(prior->order_id);
        if (!push_reject(cmd, BizReject::ConflictingRequest, existing, existing == nullptr,
                         &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    // --- capacity ----------------------------------------------------------
    if (live_order_count() >= cfg_.max_live_orders) {
        if (!push_reject(cmd, BizReject::OrderCapReached, nullptr, true, &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }
    if (store_.count_outbox_pending() >= cfg_.max_pending_outbox) {
        if (!push_reject(cmd, BizReject::AdmissionStopped, nullptr, true, &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    // --- accept ------------------------------------------------------------
    OrderChain c;
    c.order_id = order_ids_.allocate();
    c.firm = cmd.key.firm;
    c.business_day = cmd.key.business_day;
    c.original_cl_ord_id = cmd.key.cl_ord_id;
    c.current_cl_ord_id = cmd.key.cl_ord_id;
    c.symbol = cmd.symbol;
    c.side = cmd.side;
    c.ord_type = cmd.ord_type;
    c.tif = cmd.tif;
    c.original_qty = cmd.qty;
    c.effective_qty = cmd.qty;
    c.limit_ticks = cmd.limit_ticks;
    c.cum_qty = 0;
    c.leaves_qty = cmd.qty;
    c.avg_px_ticks = 0;
    c.base = BaseState::New;
    c.version = 1;
    c.accepted_at_utc = now_utc();

    RegistryRow row;
    row.key = cmd.key;
    row.type = RequestType::New;
    row.canonical_hash = cmd.canonical_hash;
    row.order_id = c.order_id;
    row.outcome = RequestOutcome::Accepted;
    txn.registry.push_back(row);

    {
        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = c.current_cl_ord_id;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecNew, &txn, &out)) return out;
    }

    // --- optional immediate match -----------------------------------------
    // The New acknowledgement and any resulting trade are separate reports with
    // separate ExecIDs, committed together (design doc 7.3, 9.1).
    if (cfg_.fill_trigger == FillTrigger::AutoOnNewOrQuote) {
        const MatchPlan plan = plan_auto_match(c, book_, now_ms());
        if (plan.traded) {
            if (!apply_fill(&c, plan, &txn, &out)) return out;
        } else {
            out.no_trade = plan.reason;
        }
    }

    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::handle_cancel(const AppCommand& cmd) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    if (const RegistryRow* prior = lookup_request(cmd.key)) {
        if (prior->canonical_hash == cmd.canonical_hash) {
            out.duplicate_suppressed = true;
            out.accepted = prior->outcome == RequestOutcome::Accepted;
            out.order_id = prior->order_id;
            out.detail = "duplicate cancel request suppressed";
            return out;
        }
        // Same request id aimed at a different order: refused, nothing mutated
        // (F-07).
        const OrderChain* existing =
            prior->order_id.empty() ? nullptr : find_order(prior->order_id);
        if (!push_reject(cmd, BizReject::ConflictingRequest, existing, existing == nullptr,
                         &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    // --- resolve the target ------------------------------------------------
    const auto idx = accepted_index_.find(cmd.orig_cl_ord_id);
    if (idx == accepted_index_.end()) {
        if (!push_reject(cmd, BizReject::UnknownOrder, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    const OrderChain* found = find_order(idx->second);
    if (found == nullptr) {
        if (!push_reject(cmd, BizReject::UnknownOrder, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    // Firm isolation: one firm cannot reach another firm's chain even with the
    // right identifier (F-20).
    if (found->firm != cmd.key.firm) {
        if (!push_reject(cmd, BizReject::CrossFirm, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    if (found->current_cl_ord_id != cmd.orig_cl_ord_id) {
        if (!push_reject(cmd, BizReject::StaleOrigClOrdId, found, false, &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    if (is_terminal(c.base)) {
        // 35=9 carrying the CURRENT status, which is the whole point of F-04
        // and F-17: the client learns what actually happened.
        if (!push_reject(cmd, BizReject::TooLate, &c, false, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    if (c.pending != Pending::None) {
        if (!push_reject(cmd, BizReject::AlreadyPending, &c, false, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }

    RegistryRow row;
    row.key = cmd.key;
    row.type = RequestType::Cancel;
    row.canonical_hash = cmd.canonical_hash;
    row.order_id = c.order_id;

    if (cfg_.emit_pending_cancel) {
        c.pending = Pending::Cancel;
        c.pending_cl_ord_id = cmd.key.cl_ord_id;
        ++c.version;
        row.outcome = RequestOutcome::Pending;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = cmd.key.cl_ord_id;
        req.orig_cl_ord_id = cmd.orig_cl_ord_id;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecPendingCancel, &txn, &out))
            return out;
    } else {
        c.base = BaseState::Canceled;
        c.pending = Pending::None;
        c.leaves_qty = 0;  // invariant I-04, even after a partial fill
        c.current_cl_ord_id = cmd.key.cl_ord_id;
        c.terminal_at_utc = now_utc();
        ++c.version;
        row.outcome = RequestOutcome::Accepted;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = cmd.key.cl_ord_id;
        req.orig_cl_ord_id = cmd.orig_cl_ord_id;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecCanceled, &txn, &out)) return out;
    }

    txn.registry.push_back(row);
    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::handle_replace(const AppCommand& cmd) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    if (const RegistryRow* prior = lookup_request(cmd.key)) {
        if (prior->canonical_hash == cmd.canonical_hash) {
            out.duplicate_suppressed = true;
            out.accepted = prior->outcome == RequestOutcome::Accepted;
            out.order_id = prior->order_id;
            out.detail = "duplicate replace request suppressed";
            return out;
        }
        const OrderChain* existing =
            prior->order_id.empty() ? nullptr : find_order(prior->order_id);
        if (!push_reject(cmd, BizReject::ConflictingRequest, existing, existing == nullptr,
                         &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    const auto idx = accepted_index_.find(cmd.orig_cl_ord_id);
    if (idx == accepted_index_.end()) {
        if (!push_reject(cmd, BizReject::UnknownOrder, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    const OrderChain* found = find_order(idx->second);
    if (found == nullptr) {
        if (!push_reject(cmd, BizReject::UnknownOrder, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    if (found->firm != cmd.key.firm) {
        if (!push_reject(cmd, BizReject::CrossFirm, nullptr, true, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    if (found->current_cl_ord_id != cmd.orig_cl_ord_id) {
        // Strict chaining: optimistic chaining against a version that has not
        // been accepted yet is refused, not queued (G-11, G-12).
        if (!push_reject(cmd, BizReject::StaleOrigClOrdId, found, false, &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    if (is_terminal(c.base)) {
        if (!push_reject(cmd, BizReject::TooLate, &c, false, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    if (c.pending != Pending::None) {
        if (!push_reject(cmd, BizReject::AlreadyPending, &c, false, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }
    // Symbol and side are immutable attributes of a chain (G-19).
    if (cmd.symbol != c.symbol || cmd.side != c.side) {
        if (!push_reject(cmd, BizReject::ImmutableAttribute, &c, false, &txn, &out))
            return out;
        commit(txn, updated, &out);
        return out;
    }
    // Strict lab profile: an OrderQty below what already traded is refused
    // rather than reinterpreted as "stop executing" (G-04, ADR-10).
    if (cmd.qty < c.cum_qty) {
        if (!push_reject(cmd, BizReject::QtyBelowCumQty, &c, false, &txn, &out)) return out;
        commit(txn, updated, &out);
        return out;
    }

    RegistryRow row;
    row.key = cmd.key;
    row.type = RequestType::Replace;
    row.canonical_hash = cmd.canonical_hash;
    row.order_id = c.order_id;

    if (cfg_.emit_pending_replace) {
        // The old version stays effective until the replace is accepted, so a
        // fill in between reports the OLD parameters (G-13, G-18, PB-08).
        c.pending = Pending::Replace;
        c.pending_cl_ord_id = cmd.key.cl_ord_id;
        c.pending_qty = cmd.qty;
        c.pending_limit_ticks = cmd.limit_ticks;
        ++c.version;
        row.outcome = RequestOutcome::Pending;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = cmd.key.cl_ord_id;
        req.orig_cl_ord_id = cmd.orig_cl_ord_id;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecPendingReplace, &txn, &out))
            return out;
    } else {
        c.effective_qty = cmd.qty;
        c.limit_ticks = cmd.limit_ticks;
        c.current_cl_ord_id = cmd.key.cl_ord_id;
        ++c.version;
        // CumQty is NOT reset by a replace (design doc 6.3).
        if (c.cum_qty >= c.effective_qty) {
            c.base = BaseState::Filled;
            c.leaves_qty = 0;
            c.terminal_at_utc = now_utc();
        } else {
            c.base = c.cum_qty > 0 ? BaseState::PartiallyFilled : BaseState::New;
            c.leaves_qty = c.effective_qty - c.cum_qty;
        }
        row.outcome = RequestOutcome::Accepted;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = cmd.key.cl_ord_id;
        req.orig_cl_ord_id = cmd.orig_cl_ord_id;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecReplaced, &txn, &out)) return out;
    }

    txn.registry.push_back(row);
    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::on_unsupported(std::string_view ref_msg_type,
                                        std::uint64_t ref_seq_num, std::string_view ref_id) {
    ActorOutcome out;
    if (state_ != ActorState::Ready) {
        out.detail = "actor is not ready";
        return out;
    }
    BusinessTxn txn;
    ReportRequest req;
    req.ref_msg_type = std::string(ref_msg_type);
    req.ref_seq_num = ref_seq_num;
    req.ref_id = std::string(ref_id);
    req.reject = BizReject::UnsupportedMsgType;
    if (!push_report(std::move(req), ReportType::BusinessReject, &txn, &out)) return out;
    out.reject = BizReject::UnsupportedMsgType;
    std::vector<OrderChain> none;
    commit(txn, none, &out);
    return out;
}

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

bool OrderActor::apply_fill(OrderChain* chain, const MatchPlan& plan, BusinessTxn* txn,
                            ActorOutcome* out) {
    Units new_cum = 0;
    if (!checked_add(chain->cum_qty, plan.qty, &new_cum)) {
        out->detail = "CumQty overflow";
        return false;
    }
    // Belt and braces over the matcher: an overfill must be impossible at the
    // point of mutation too, not only at the point of planning (I-03).
    if (new_cum > chain->effective_qty) {
        out->detail = "fill would overfill the order";
        out->no_trade = NoTrade::QtyExceedsLeaves;
        return false;
    }

    // AvgPx is recomputed from the accumulated notional rather than nudged, so
    // it matches what recovery will derive from the ledger (E-04, E-19).
    NotionalAccumulator acc;
    if (chain->cum_qty > 0) {
        if (!acc.add(chain->cum_qty, chain->avg_px_ticks)) {
            out->detail = "notional overflow rebuilding AvgPx";
            return false;
        }
    }
    if (!acc.add(plan.qty, plan.px_ticks)) {
        out->detail = "notional overflow adding fill";
        out->no_trade = NoTrade::Overflow;
        return false;
    }

    Fill f;
    f.fill_id = fill_ids_.allocate();
    f.exec_id = exec_ids_.allocate();
    f.order_id = chain->order_id;
    f.quote_version = plan.quote_version;
    f.liquidity_slice = plan.slice_id;
    f.qty = plan.qty;
    f.px_ticks = plan.px_ticks;
    f.created_at_utc = now_utc();

    chain->cum_qty = new_cum;
    chain->avg_px_ticks = acc.avg_px_ticks();
    if (chain->cum_qty >= chain->effective_qty) {
        chain->base = BaseState::Filled;
        chain->leaves_qty = 0;
        chain->terminal_at_utc = now_utc();
    } else {
        chain->base = BaseState::PartiallyFilled;
        chain->leaves_qty = chain->effective_qty - chain->cum_qty;
    }
    ++chain->version;

    QuoteConsumption qc;
    qc.symbol = chain->symbol;
    qc.version = plan.quote_version;
    qc.taker_side = chain->side;
    qc.qty = plan.qty;
    txn->quote_consumptions.push_back(qc);

    ReportRequest req;
    req.chain = chain;
    req.price = price_format(chain->symbol);
    // A trade reports the accepted version's ClOrdID. While a replace is
    // pending the accepted version is still the old one (G-18).
    req.cl_ord_id = chain->current_cl_ord_id;
    req.exec_id = f.exec_id;
    req.last_qty = plan.qty;
    req.last_px = plan.px_ticks;

    const std::size_t before = txn->outbox.size();
    if (!push_report(std::move(req), ReportType::ExecTrade, txn, out)) return false;
    // The outbox row created for this trade is the fill's event identity, which
    // is what ties one fill to exactly one report (I-05, I-06).
    f.event_id = txn->outbox[before].event_id;
    txn->fills.push_back(std::move(f));
    return true;
}

// ---------------------------------------------------------------------------
// Scheduled events
// ---------------------------------------------------------------------------

EnqueueResult OrderActor::schedule(ScheduledEvent e) {
    if (e.event_id.empty()) e.event_id = event_ids_.allocate();

    // Design doc 11.3: the planned event identity is persisted BEFORE it can
    // execute. Without the durable row there is nothing for the executing
    // transaction to mark, so a restart would requeue an event that had already
    // run and produce a second fill (W9, H-10).
    const EnqueueResult r = sched_.enqueue(e);
    if (r != EnqueueResult::Ok) return r;

    BusinessTxn txn;
    txn.new_events.push_back(e);
    txn.next_event_counter = event_ids_.next_counter();
    if (store_.commit_business(txn) != StoreError::None) {
        // The event is not durable, so it must not stay queued either: an event
        // that runs once from memory and once more after a restart is exactly
        // the double effect W9 is about.
        sched_.remove(e.event_id);
        return EnqueueResult::CapacityReached;
    }
    return EnqueueResult::Ok;
}

std::vector<ActorOutcome> OrderActor::run_due(VirtualTimeUs upto_us) {
    std::vector<ActorOutcome> results;
    ScheduledEvent ev;
    while (state_ == ActorState::Ready && sched_.pop_due(upto_us, &ev)) {
        switch (ev.kind) {
            case EventKind::QuoteUpdate: results.push_back(do_quote_update(ev)); break;
            case EventKind::InjectFill: results.push_back(do_inject_fill(ev)); break;
            case EventKind::AutoMatch: results.push_back(do_auto_match(ev)); break;
            case EventKind::FinalizeCancel: results.push_back(do_finalize_cancel(ev)); break;
            case EventKind::FinalizeReplace: results.push_back(do_finalize_replace(ev)); break;
            case EventKind::Expire: results.push_back(do_expire(ev)); break;
            case EventKind::FaultTrigger: {
                ActorOutcome o;
                o.accepted = true;
                o.detail = "fault trigger " + ev.fault_name;
                sched_.mark_executed(ev.event_id);
                results.push_back(o);
                break;
            }
        }
    }
    return results;
}

ActorOutcome OrderActor::do_quote_update(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    // Validate against the in-memory book first so a bad fixture is refused
    // before any row is written (D-08, D-09, D-13, D-14).
    QuoteBook probe;
    probe.register_symbol(ev.quote.symbol);
    for (const QuoteSlice& s : book_.snapshot()) {
        if (s.symbol == ev.quote.symbol) probe.restore({s});
    }
    const QuoteError qe = probe.apply_update(ev.quote);
    if (qe != QuoteError::Ok) {
        out.detail = std::string("quote refused: ") + std::string(to_string(qe));
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }
    // An identical replay of a version is accepted as a no-op by the book, but
    // must not be written again.
    const QuoteSlice* cur = book_.current(ev.quote.symbol);
    if (cur != nullptr && cur->version == ev.quote.version) {
        out.accepted = true;
        out.detail = "quote version already current";
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    txn.quote_updates.push_back(ev.quote);
    txn.executed_event_ids.push_back(ev.event_id);

    // Under the auto policy a fresh quote is a matching trigger. Orders are
    // visited in order_id order, a documented deterministic priority rather
    // than whatever the container happens to yield (D-11, H-07).
    if (cfg_.fill_trigger == FillTrigger::AutoOnNewOrQuote) {
        QuoteBook after;
        after.register_symbol(ev.quote.symbol);
        after.restore({ev.quote});
        for (auto& kv : chains_) {
            OrderChain c = kv.second;
            if (c.symbol != ev.quote.symbol || is_terminal(c.base)) continue;
            const MatchPlan plan = plan_auto_match(c, after, now_ms());
            if (!plan.traded) continue;
            if (!apply_fill(&c, plan, &txn, &out)) return out;
            after.consume(c.symbol, plan.quote_version, c.side, plan.qty);
            updated.push_back(c);
        }
    }

    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::do_inject_fill(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    const OrderChain* found = find_order(ev.order_id);
    if (found == nullptr) {
        // A fill aimed at an order that does not exist, for example one
        // scheduled before the New was accepted (E-12).
        out.no_trade = NoTrade::OrderTerminal;
        out.detail = "no such order: " + ev.order_id;
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    const MatchPlan plan = plan_injected_fill(c, book_, ev.qty, ev.px_ticks, now_ms());
    if (!plan.traded) {
        // A refused injection is recorded and changes nothing: the scenario
        // cannot bypass an invariant by asking (E-07..E-11, D-05, D-06).
        out.no_trade = plan.reason;
        out.detail = std::string("fill refused: ") + std::string(to_string(plan.reason));
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    if (!apply_fill(&c, plan, &txn, &out)) {
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }
    txn.executed_event_ids.push_back(ev.event_id);
    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::do_auto_match(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    const OrderChain* found = find_order(ev.order_id);
    if (found == nullptr) {
        out.detail = "no such order: " + ev.order_id;
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }
    OrderChain c = *found;
    const MatchPlan plan = plan_auto_match(c, book_, now_ms());
    if (!plan.traded) {
        out.no_trade = plan.reason;
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }
    if (!apply_fill(&c, plan, &txn, &out)) {
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }
    txn.executed_event_ids.push_back(ev.event_id);
    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::do_finalize_cancel(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    const OrderChain* found = find_order(ev.order_id);
    if (found == nullptr || found->pending != Pending::Cancel ||
        found->pending_cl_ord_id != ev.request_id) {
        out.detail = "no pending cancel to finalise";
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    const std::string request_id = c.pending_cl_ord_id;
    const std::string prior_version = c.current_cl_ord_id;

    RegistryRow row;
    row.key = BusinessKey{c.firm, c.business_day, request_id};
    row.type = RequestType::Cancel;
    row.order_id = c.order_id;
    if (const RegistryRow* prior = lookup_request(row.key)) row.canonical_hash = prior->canonical_hash;

    if (is_terminal(c.base)) {
        // The order finished while the cancel was pending: the request is too
        // late and is refused with the current status. No Canceled report is
        // emitted after a Filled one (F-10, PB-07).
        c.pending = Pending::None;
        c.pending_cl_ord_id.clear();
        ++c.version;
        row.outcome = RequestOutcome::Rejected;
        row.reject = BizReject::TooLate;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = request_id;
        req.orig_cl_ord_id = prior_version;
        req.reject = BizReject::TooLate;
        req.ref_msg_type = std::string(msg_type::OrderCancelRequest);
        if (!push_report(std::move(req), ReportType::CancelReject, &txn, &out)) return out;
        out.reject = BizReject::TooLate;
    } else {
        c.base = BaseState::Canceled;
        c.pending = Pending::None;
        c.pending_cl_ord_id.clear();
        c.leaves_qty = 0;
        c.current_cl_ord_id = request_id;
        c.terminal_at_utc = now_utc();
        ++c.version;
        row.outcome = RequestOutcome::Accepted;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = request_id;
        req.orig_cl_ord_id = prior_version;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecCanceled, &txn, &out)) return out;
        out.accepted = true;
    }

    txn.registry.push_back(row);
    txn.executed_event_ids.push_back(ev.event_id);
    updated.push_back(c);
    out.order_id = c.order_id;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::do_finalize_replace(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    const OrderChain* found = find_order(ev.order_id);
    if (found == nullptr || found->pending != Pending::Replace ||
        found->pending_cl_ord_id != ev.request_id) {
        out.detail = "no pending replace to finalise";
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    const std::string request_id = c.pending_cl_ord_id;
    const std::string prior_version = c.current_cl_ord_id;

    RegistryRow row;
    row.key = BusinessKey{c.firm, c.business_day, request_id};
    row.type = RequestType::Replace;
    row.order_id = c.order_id;
    if (const RegistryRow* prior = lookup_request(row.key)) row.canonical_hash = prior->canonical_hash;

    const bool too_late = is_terminal(c.base);
    // A replace that would put OrderQty below what has already traded is
    // refused at finalisation too: fills between the request and its
    // acceptance can make a once-valid request invalid (G-04, G-14).
    const bool below_cum = !too_late && c.pending_qty < c.cum_qty;

    if (too_late || below_cum) {
        c.pending = Pending::None;
        c.pending_cl_ord_id.clear();
        c.pending_qty = 0;
        c.pending_limit_ticks = 0;
        ++c.version;
        const BizReject why = too_late ? BizReject::TooLate : BizReject::QtyBelowCumQty;
        row.outcome = RequestOutcome::Rejected;
        row.reject = why;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = request_id;
        req.orig_cl_ord_id = prior_version;
        req.reject = why;
        req.ref_msg_type = std::string(msg_type::OrderCancelReplaceRequest);
        if (!push_report(std::move(req), ReportType::CancelReject, &txn, &out)) return out;
        out.reject = why;
    } else {
        c.effective_qty = c.pending_qty;
        c.limit_ticks = c.pending_limit_ticks;
        c.current_cl_ord_id = request_id;
        c.pending = Pending::None;
        c.pending_cl_ord_id.clear();
        c.pending_qty = 0;
        c.pending_limit_ticks = 0;
        ++c.version;
        if (c.cum_qty >= c.effective_qty) {
            c.base = BaseState::Filled;
            c.leaves_qty = 0;
            c.terminal_at_utc = now_utc();
        } else {
            c.base = c.cum_qty > 0 ? BaseState::PartiallyFilled : BaseState::New;
            c.leaves_qty = c.effective_qty - c.cum_qty;
        }
        row.outcome = RequestOutcome::Accepted;

        ReportRequest req;
        req.chain = &c;
        req.price = price_format(c.symbol);
        req.cl_ord_id = request_id;
        req.orig_cl_ord_id = prior_version;
        req.exec_id = exec_ids_.allocate();
        if (!push_report(std::move(req), ReportType::ExecReplaced, &txn, &out)) return out;
        out.accepted = true;
    }

    txn.registry.push_back(row);
    txn.executed_event_ids.push_back(ev.event_id);
    updated.push_back(c);
    out.order_id = c.order_id;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

ActorOutcome OrderActor::do_expire(const ScheduledEvent& ev) {
    ActorOutcome out;
    BusinessTxn txn;
    std::vector<OrderChain> updated;

    const OrderChain* found = find_order(ev.order_id);
    if (found == nullptr || is_terminal(found->base)) {
        // Already terminal: exactly one terminal outcome per chain, so an
        // expiry that lost the race does nothing (H-04, H-05, H-20).
        out.detail = "expiry not applicable";
        txn.executed_event_ids.push_back(ev.event_id);
        commit(txn, updated, &out);
        return out;
    }

    OrderChain c = *found;
    c.base = BaseState::Expired;
    c.pending = Pending::None;
    c.pending_cl_ord_id.clear();
    c.leaves_qty = 0;
    c.terminal_at_utc = now_utc();
    ++c.version;

    ReportRequest req;
    req.chain = &c;
    req.price = price_format(c.symbol);
    req.cl_ord_id = c.current_cl_ord_id;
    req.exec_id = exec_ids_.allocate();
    if (!push_report(std::move(req), ReportType::ExecExpired, &txn, &out)) return out;

    txn.executed_event_ids.push_back(ev.event_id);
    updated.push_back(c);
    out.order_id = c.order_id;
    out.accepted = true;
    if (!commit(txn, updated, &out)) out.accepted = false;
    return out;
}

// ---------------------------------------------------------------------------
// Invariants
// ---------------------------------------------------------------------------

bool OrderActor::check_invariants(std::string* why) const {
    for (const auto& kv : chains_) {
        const OrderChain& c = kv.second;
        // I-03: no overfill, and quantities stay in range.
        if (c.cum_qty < 0 || c.effective_qty < 0) {
            *why = "negative quantity on " + c.order_id;
            return false;
        }
        if (c.cum_qty > c.effective_qty) {
            *why = "I-03 violated: CumQty exceeds OrderQty on " + c.order_id;
            return false;
        }
        // I-04: LeavesQty is zero once terminal and OrderQty-CumQty otherwise.
        if (c.leaves_qty != c.expected_leaves()) {
            *why = "I-04 violated: LeavesQty inconsistent on " + c.order_id;
            return false;
        }
        if (c.avg_px_ticks < 0) {
            *why = "negative AvgPx on " + c.order_id;
            return false;
        }
        if (c.cum_qty == 0 && c.avg_px_ticks != 0) {
            *why = "AvgPx set with no fills on " + c.order_id;
            return false;
        }
        // A terminal chain MAY still carry an in-flight request: that is
        // exactly the PB-07 window, where the order fills completely while a
        // cancel is pending and the cancel is only refused when it finalises.
        // What must hold at every instant is that the chain reports its
        // terminal status rather than the pending one, so a client is never
        // told a finished order is still live (F-10, status precedence).
        if (is_terminal(c.base)) {
            const std::string_view reported = ord_status_for(c.base, c.pending);
            if (reported != ord_status::Filled && reported != ord_status::Canceled &&
                reported != ord_status::Expired && reported != ord_status::Rejected) {
                *why = "terminal chain " + c.order_id + " reports non-terminal status " +
                       std::string(reported);
                return false;
            }
        }
    }

    // I-12: no liquidity slice is oversubscribed. The book holds what remains,
    // and the ledger holds what was taken; together they must not exceed what
    // the slice ever offered, which the book enforces by refusing to go
    // negative. What is checked here is that nothing went negative anyway.
    for (const SymbolProfile& s : cfg_.symbols) {
        if (book_.inventory_units(s.symbol, Side::Buy) < 0 ||
            book_.inventory_units(s.symbol, Side::Sell) < 0) {
            *why = "I-12 violated: negative inventory on " + s.symbol;
            return false;
        }
    }
    for (const auto& kv : consumed_by_slice_) {
        if (kv.second < 0) {
            *why = "I-12 violated: negative consumption on slice " + kv.first;
            return false;
        }
    }
    return true;
}

}  // namespace lpsim
