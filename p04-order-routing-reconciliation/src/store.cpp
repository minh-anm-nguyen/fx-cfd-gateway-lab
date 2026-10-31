#include "router/store.hpp"

#include <algorithm>

namespace router {

std::string_view to_string(CommandKind k) {
    switch (k) {
        case CommandKind::New: return "New";
        case CommandKind::Cancel: return "Cancel";
        case CommandKind::Replace: return "Replace";
        case CommandKind::StatusQuery: return "StatusQuery";
    }
    return "Unknown";
}

std::string_view to_string(DeliveryState s) {
    switch (s) {
        case DeliveryState::Received: return "RECEIVED";
        case DeliveryState::RejectedLocal: return "REJECTED_LOCAL";
        case DeliveryState::AcceptedDurable: return "ACCEPTED_DURABLE";
        case DeliveryState::Queued: return "QUEUED";
        case DeliveryState::Dispatching: return "DISPATCHING";
        case DeliveryState::OriginalSeqBound: return "ORIGINAL_SEQ_BOUND";
        case DeliveryState::AwaitBusiness: return "AWAIT_BUSINESS";
        case DeliveryState::UnknownTransport: return "UNKNOWN_TRANSPORT";
        case DeliveryState::Reconciling: return "RECONCILING";
        case DeliveryState::LpAcked: return "LP_ACKED";
        case DeliveryState::LpRejected: return "LP_REJECTED";
        case DeliveryState::ManualHold: return "MANUAL_HOLD";
    }
    return "UNKNOWN";
}

std::string_view to_string(BusinessOutcome o) {
    switch (o) {
        case BusinessOutcome::Unknown: return "UNKNOWN";
        case BusinessOutcome::Acknowledged: return "ACKNOWLEDGED";
        case BusinessOutcome::Rejected: return "REJECTED";
        case BusinessOutcome::Canceled: return "CANCELED";
        case BusinessOutcome::Replaced: return "REPLACED";
        case BusinessOutcome::Filled: return "FILLED";
    }
    return "UNKNOWN";
}

std::string_view to_string(OutboxState s) {
    switch (s) {
        case OutboxState::Pending: return "Pending";
        case OutboxState::Claimed: return "Claimed";
        case OutboxState::Sent: return "Sent";
        case OutboxState::Done: return "Done";
        case OutboxState::Failed: return "Failed";
        case OutboxState::Held: return "Held";
    }
    return "Unknown";
}

std::string_view to_string(ReconState s) {
    switch (s) {
        case ReconState::Open: return "OPEN";
        case ReconState::StatusQueryPending: return "STATUS_QUERY_PENDING";
        case ReconState::StatusReceived: return "STATUS_RECEIVED";
        case ReconState::HistoryRequired: return "HISTORY_REQUIRED";
        case ReconState::Reconciled: return "RECONCILED";
        case ReconState::ManualHold: return "MANUAL_HOLD";
        case ReconState::Quarantined: return "QUARANTINED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Txn

void Txn::put_intent(CommandIntent v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.intents[v.key()] = v; }, nullptr});
}

namespace {

bool is_pending(OutboxState s) {
    return s == OutboxState::Pending || s == OutboxState::Claimed;
}

}  // namespace

void Txn::put_outbox(OutboxRow v) {
    ops_.push_back(Op{[v](StoreImage& img) {
                          auto it = img.outbox.find(v.id);
                          const bool was_pending =
                              it != img.outbox.end() && is_pending(it->second.state);
                          const bool now_pending = is_pending(v.state);
                          if (!was_pending && now_pending) ++img.pending_outbox_count;
                          if (was_pending && !now_pending) --img.pending_outbox_count;
                          img.outbox[v.id] = v;
                          if (v.id >= img.next_outbox_id) img.next_outbox_id = v.id + 1;
                      },
                      nullptr});
}

void Txn::drop_outbox(OutboxId id) {
    ops_.push_back(Op{[id](StoreImage& img) {
                          auto it = img.outbox.find(id);
                          if (it == img.outbox.end()) return;
                          if (is_pending(it->second.state)) --img.pending_outbox_count;
                          img.outbox.erase(it);
                      },
                      nullptr});
}

void Txn::put_chain(p03::ChainState v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.chains[v.chain_id.str()] = v; }, nullptr});
}

void Txn::put_recon(ReconCase v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.recon_cases[v.case_id.str()] = v; }, nullptr});
}

void Txn::add_applied_receipt(AppliedReceipt v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.applied_receipts[v.key()] = v; }, nullptr});
}

void Txn::put_exec_key(BusinessExecKey v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.exec_keys[v.key()] = v; }, nullptr});
}

void Txn::add_quarantine(QuarantineEntry v) {
    ops_.push_back(Op{[v](StoreImage& img) mutable {
                          v.id = img.next_quarantine_id++;
                          img.quarantine.push_back(v);
                          if (!v.resolved) ++img.unresolved_quarantine_count;
                      },
                      nullptr});
}

void Txn::resolve_quarantine(std::uint64_t id, const std::string& note) {
    ops_.push_back(Op{[id, note](StoreImage& img) {
                          for (QuarantineEntry& q : img.quarantine) {
                              if (q.id != id) continue;
                              if (!q.resolved && img.unresolved_quarantine_count > 0)
                                  --img.unresolved_quarantine_count;
                              q.resolved = true;
                              if (!note.empty()) q.note += " | " + note;
                              return;
                          }
                      },
                      nullptr});
}

void Txn::add_audit(AuditRecord v) {
    ops_.push_back(Op{[v](StoreImage& img) { img.audit.append(v); },
                      [](const StoreImage& img) -> std::optional<StoreError> {
                          // R-I15 and OR-Q-10: if the audit cannot be written, the
                          // whole transaction must fail rather than proceed
                          // unaudited.
                          if (!img.audit.available()) return StoreError::Unavailable;
                          return std::nullopt;
                      }});
}

void Txn::add_journal(JournalRecord v) {
    ops_.push_back(Op{[v](StoreImage& img) mutable {
                          v.seq = img.next_journal_seq++;
                          img.journal.push_back(v);
                      },
                      nullptr});
}

void Txn::reserve_cl_ord_id(const LpId& lp, const ClientOrderId& id,
                            const std::string& intent_key) {
    const std::string key = lp.str() + "|" + id.str();
    ops_.push_back(
        Op{[key, intent_key](StoreImage& img) { img.cl_ord_index[key] = intent_key; },
           [key, intent_key](const StoreImage& img) -> std::optional<StoreError> {
               auto it = img.cl_ord_index.find(key);
               if (it != img.cl_ord_index.end() && it->second != intent_key)
                   return StoreError::Conflict;
               return std::nullopt;
           }});
}

Result<std::uint64_t, StoreError> Txn::commit() {
    using R = Result<std::uint64_t, StoreError>;
    StoreImage& img = store_->image_;

    // Fencing first: a router instance that lost its lease must not write, even
    // if everything else about the transaction is valid.
    if (!store_->owns()) return R::err(StoreError::NotOwner);
    if (!img.available) return R::err(StoreError::Unavailable);
    if (img.corrupt) return R::err(StoreError::Corrupt);

    FaultKind kind = FaultKind::Crash;
    if (store_->fp_.should_fire(fp::kStoreFull, kind)) {
        if (kind == FaultKind::Crash) throw CrashMarker(fp::kStoreFull);
        return R::err(StoreError::Full);
    }
    if (img.full) return R::err(StoreError::Full);
    if (store_->fp_.should_fire(fp::kStoreIoError, kind)) {
        if (kind == FaultKind::Crash) throw CrashMarker(fp::kStoreIoError);
        return R::err(StoreError::IoError);
    }
    if (store_->fp_.should_fire(fp::kAuditWriteFail, kind)) {
        if (kind == FaultKind::Crash) throw CrashMarker(fp::kAuditWriteFail);
        return R::err(StoreError::Unavailable);
    }

    for (const Op& op : ops_) {
        if (!op.precheck) continue;
        if (auto e = op.precheck(img)) return R::err(*e);
    }

    // CW-02 and CW-13: dying here must leave nothing behind. Because nothing has
    // been written to the image yet, that is automatic rather than a promise.
    store_->fp_.check(fp::kInsideIntentCommit);

    for (const Op& op : ops_) op.apply(img);
    const std::uint64_t seq = img.next_journal_seq;

    // CW-19: the write landed but the caller never heard. The store reports
    // CommitAckLost, and the caller must re-read authoritative keys rather than
    // assume a rollback (OR-O-19).
    if (store_->fp_.should_fire(fp::kCommitAckLost, kind)) {
        if (kind == FaultKind::Crash) throw CrashMarker(fp::kCommitAckLost);
        return R::err(StoreError::CommitAckLost);
    }
    // CW-03 and CW-14: dying after the commit. The data is on disk already.
    store_->fp_.check(fp::kAfterApplyCommit);
    return R::ok(seq);
}

// ---------------------------------------------------------------------------
// Store

Store::Store(StoreImage& image, FailpointRegistry& fp, const IClock& clock,
             p03::IOrderReducer* reducer)
    : image_(image), fp_(fp), clock_(clock), reducer_(reducer) {}

Result<std::uint64_t, StoreError> Store::acquire_ownership() {
    using R = Result<std::uint64_t, StoreError>;
    if (!image_.available) return R::err(StoreError::Unavailable);
    if (image_.schema_version != 1) return R::err(StoreError::SchemaMismatch);
    fence_ = ++image_.owner_fence;
    return R::ok(fence_);
}

bool Store::owns() const { return fence_ != 0 && fence_ == image_.owner_fence; }

Result<bool, StoreError> Store::check_health() const {
    using R = Result<bool, StoreError>;
    if (!image_.available) return R::err(StoreError::Unavailable);
    if (image_.corrupt) return R::err(StoreError::Corrupt);
    if (image_.schema_version != 1) return R::err(StoreError::SchemaMismatch);
    // The audit chain is part of integrity: a store whose history was edited is
    // not a store we may resume from (OR-Q-18).
    if (!image_.audit.verify()) return R::err(StoreError::Corrupt);
    // An outbox row whose intent is missing means the two tables disagree.
    for (const auto& [id, row] : image_.outbox) {
        (void)id;
        const std::string key = row.tenant.str() + "|" + row.command_id.str();
        if (image_.intents.find(key) == image_.intents.end()) return R::err(StoreError::Corrupt);
        // OR-E-19: a row with no payload hash cannot be issued safely.
        if (row.payload_hash.is_zero()) return R::err(StoreError::Corrupt);
    }
    return R::ok(true);
}

const CommandIntent* Store::find_intent(const TenantId& t, const CommandId& c) const {
    auto it = image_.intents.find(t.str() + "|" + c.str());
    return it == image_.intents.end() ? nullptr : &it->second;
}

const CommandIntent* Store::find_intent_by_cl_ord_id(const LpId& lp,
                                                     const ClientOrderId& id) const {
    auto it = image_.cl_ord_index.find(lp.str() + "|" + id.str());
    if (it == image_.cl_ord_index.end()) return nullptr;
    auto jt = image_.intents.find(it->second);
    return jt == image_.intents.end() ? nullptr : &jt->second;
}

const p03::ChainState* Store::find_chain(const OrderChainId& id) const {
    auto it = image_.chains.find(id.str());
    return it == image_.chains.end() ? nullptr : &it->second;
}

const BusinessExecKey* Store::find_exec_key(const std::string& lp_ns,
                                            const std::string& exec_id) const {
    auto it = image_.exec_keys.find(lp_ns + "|" + exec_id);
    return it == image_.exec_keys.end() ? nullptr : &it->second;
}

bool Store::receipt_applied(const std::string& session_key, SessionEpoch epoch,
                            DurableReceiptId receipt) const {
    const std::string key =
        session_key + "|" + std::to_string(epoch) + "|" + std::to_string(receipt);
    return image_.applied_receipts.find(key) != image_.applied_receipts.end();
}

const ReconCase* Store::find_recon(const ReconCaseId& id) const {
    auto it = image_.recon_cases.find(id.str());
    return it == image_.recon_cases.end() ? nullptr : &it->second;
}

std::vector<const OutboxRow*> Store::pending_outbox() const {
    std::vector<const OutboxRow*> out;
    for (const auto& [id, row] : image_.outbox) {
        (void)id;
        if (row.state == OutboxState::Pending || row.state == OutboxState::Claimed)
            out.push_back(&row);
    }
    // Priority across chains, insertion order within equal priority: design doc
    // 16.1 wants cancels first without breaking per-chain ordering.
    std::stable_sort(out.begin(), out.end(), [](const OutboxRow* a, const OutboxRow* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->id < b->id;
    });
    return out;
}

std::vector<const ReconCase*> Store::open_recon_cases() const {
    std::vector<const ReconCase*> out;
    for (const auto& [k, c] : image_.recon_cases) {
        (void)k;
        if (c.state != ReconState::Reconciled) out.push_back(&c);
    }
    return out;
}

std::size_t Store::quarantine_depth() const { return image_.unresolved_quarantine_count; }

std::string Store::peek_chain_id() const {
    return "ord-" + std::to_string(image_.next_chain_seq);
}

std::string Store::peek_cl_ord_suffix() const {
    return std::to_string(image_.next_cl_ord_seq);
}

Result<bool, StoreError> Store::prune_journal(std::size_t keep_last) {
    using R = Result<bool, StoreError>;
    if (!owns()) return R::err(StoreError::NotOwner);
    // CW-18: dying mid-prune must leave a journal that still reconstructs the
    // state. The snapshot (the chains map) is already durable, so dropping a
    // prefix is safe; the crash below happens before any erase.
    fp_.check(fp::kDuringSnapshotPrune);
    if (image_.journal.size() <= keep_last) return R::ok(true);
    const std::size_t drop = image_.journal.size() - keep_last;
    JournalRecord marker;
    marker.kind = JournalRecord::Kind::Pruned;
    marker.seq = image_.next_journal_seq++;
    std::vector<JournalRecord> kept;
    kept.push_back(marker);
    for (std::size_t i = drop; i < image_.journal.size(); ++i)
        kept.push_back(image_.journal[i]);
    image_.journal = std::move(kept);
    return R::ok(true);
}

// ---------------------------------------------------------------------------

std::map<std::string, p03::ChainState> replay_chains_from_journal(const StoreImage& image,
                                                                  p03::IOrderReducer* reducer) {
    p03::IOrderReducer& red = reducer == nullptr ? p03::default_order_reducer() : *reducer;
    std::map<std::string, p03::ChainState> chains;
    bool pruned = false;
    for (const JournalRecord& rec : image.journal) {
        switch (rec.kind) {
            case JournalRecord::Kind::Pruned:
                pruned = true;
                break;
            case JournalRecord::Kind::IntentCommitted: {
                if (!rec.intent) break;
                const CommandIntent& in = *rec.intent;
                if (in.kind == CommandKind::New) {
                    chains[in.chain_id.str()] = red.create(
                        in.chain_id, in.tenant, in.account, in.lp, in.wire_symbol, in.side,
                        in.price_scale, in.cl_ord_id, in.qty, in.price_ticks);
                } else if (in.kind == CommandKind::Cancel) {
                    auto it = chains.find(in.chain_id.str());
                    if (it != chains.end())
                        red.note_cancel_requested(it->second, in.cl_ord_id);
                } else if (in.kind == CommandKind::Replace) {
                    auto it = chains.find(in.chain_id.str());
                    if (it != chains.end())
                        red.note_replace_requested(
                            it->second, in.cl_ord_id, in.qty, in.price_ticks);
                }
                break;
            }
            case JournalRecord::Kind::ReportApplied: {
                if (!rec.report) break;
                auto it = chains.find(rec.report->chain_id.str());
                if (it == chains.end()) break;
                red.apply(it->second, *rec.report);
                break;
            }
            case JournalRecord::Kind::ChainMutated:
                if (rec.chain_after) chains[rec.chain_after->chain_id.str()] = *rec.chain_after;
                break;
            case JournalRecord::Kind::ReconMutated:
                break;
        }
    }
    // After a prune the journal alone cannot rebuild history; the caller must
    // start from the snapshot instead. Signalled by returning what the snapshot
    // holds for the chains the journal no longer covers.
    if (pruned) {
        for (const auto& [id, live] : image.chains)
            if (chains.find(id) == chains.end()) chains[id] = live;
    }
    return chains;
}

}  // namespace router
