#include "llx/reference_pipeline.hpp"

#include <algorithm>

namespace llx {

ReferencePipeline::ReferencePipeline(Config cfg) : cfg_(std::move(cfg)) {
    JournalStore::Options so;
    so.journal_path = cfg_.journal_path;
    so.sync_on_commit = cfg_.sync_on_commit;
    so.enforce_domain_sequence = cfg_.enforce_domain_sequence;
    store_ = std::make_unique<JournalStore>(so);
}

ReferencePipeline::~ReferencePipeline() = default;

std::size_t ReferencePipeline::backlog() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

AdmissionOutcome ReferencePipeline::offer(const InputEnvelope& ev) {
    ++stats_.offered;
    ++outcomes_.offered;
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() >= cfg_.max_backlog) {
        ++stats_.backlog_refused;
        ++outcomes_.rejected_overload;
        return AdmissionOutcome::RejectedOverload;
    }
    // One allocation per event. This is the cost the pooled path removes, and
    // counting it here makes the A/B honest about what changed.
    queue_.push_back(std::make_unique<InputEnvelope>(ev));
    ++stats_.heap_allocations;
    ++outcomes_.admitted;
    return AdmissionOutcome::AcceptedForProcessing;
}

std::size_t ReferencePipeline::step(std::size_t max) {
    std::size_t n = 0;
    while (n < max) {
        std::unique_ptr<InputEnvelope> slot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.empty()) break;
            slot = std::move(queue_.front());
            queue_.pop_front();
        }
        ++stats_.consumed;
        const InputEnvelope& ev = *slot;
        ++n;

        if (ev.type == EventType::SessionControl || ev.type == EventType::Tick) continue;

        // The same identity, dedup and causality rules as the engine. Anything
        // weaker here would make the comparison meaningless.
        if (!ev.event_id.is_null() && applied_.count(ev.event_id) != 0) {
            ++stats_.duplicates;
            ++outcomes_.duplicate_ignored;
            continue;
        }
        const bool dup_exec = (ev.exec_id != 0) && (seen_exec_.count(ev.exec_id) != 0);
        const Proposal p = reduce(store_->state(), ev, dup_exec);
        if (!p.accepted) {
            ++stats_.rejected;
            ++outcomes_.rejected_invalid;
            if (!ev.event_id.is_null()) applied_.insert(ev.event_id);
            continue;
        }
        if (p.no_effect) {
            ++outcomes_.duplicate_ignored;
            if (!ev.event_id.is_null()) applied_.insert(ev.event_id);
            continue;
        }

        InputEnvelope seq_ev = ev;
        std::uint64_t& next = next_seq_[ev.risk_domain];
        if (next == 0) next = store_->committed_domain_seq(ev.risk_domain);
        seq_ev.domain_seq = ++next;
        seq_ev.owner_epoch = store_->fencing_epoch();

        CommitRequest req;
        req.intent = to_intent(p, seq_ev);
        req.effect = p;
        CommitReceipt receipt;
        const CommitOutcome outcome = store_->commit(req, receipt);
        switch (outcome) {
            case CommitOutcome::Committed:
                ++stats_.committed;
                ++outcomes_.completed;
                if (!ev.event_id.is_null()) applied_.insert(ev.event_id);
                if (ev.exec_id != 0) seen_exec_.insert(ev.exec_id);
                break;
            case CommitOutcome::Unknown:
                ++stats_.unknown;
                ++outcomes_.held_unknown;
                break;
            case CommitOutcome::Rejected:
                ++stats_.rejected;
                ++outcomes_.rejected_invalid;
                // The sequence number was consumed by a commit that did not
                // happen, so it is given back; otherwise the next event for
                // this domain would be refused as out of order.
                --next;
                break;
        }
    }
    return n;
}

std::size_t ReferencePipeline::run_until_idle(std::size_t budget) {
    std::size_t total = 0;
    while (total < budget) {
        const std::size_t moved = step(64);
        if (moved == 0) break;
        total += moved;
    }
    return total;
}

}  // namespace llx
