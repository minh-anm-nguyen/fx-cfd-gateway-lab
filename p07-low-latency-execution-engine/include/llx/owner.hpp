// The single-owner actor.
//
// Design doc 7, 13 and 19. One thread owns the ordering and the hot state of a
// risk domain, and this class is that thread's body. The responsibilities, each
// tied to the invariant that forces it:
//
//   dedup          one accepted event_id produces exactly one effect, however
//                  many times a duplicate wake or a session replay presents it
//                  (LI-16, LL-016, doc 19.3)
//   fencing        an event or a commit receipt carrying an epoch older than
//                  the current one is refused before it can touch anything
//                  (LI-14, LI-47, LL-015)
//   causality      domain_seq is assigned here and the store enforces it, so a
//                  Cancel can never commit ahead of the fill that causally
//                  precedes it, whatever priority it was given (LI-13, LI-36,
//                  LL-011, doc 7 worked example)
//   staging        the reducer runs against committed state and produces a
//                  proposal. Nothing derived from it is observable until the
//                  store commits (LI-15, doc 13)
//   the send gate  dispatch eligibility comes from the committed outbox row and
//                  from nowhere else (LI-16)
//   unknown        a COMMIT_UNKNOWN holds. It is not retried, not reissued
//                  under a new id, and not reinterpreted as a failure
//                  (LI-17, LI-18)
//
// Commit modes. Doc 13 recommends a serialized per-domain worker for v1 and
// this class offers exactly two shapes:
//
//   Synchronous   the owner commits inline. Simplest and the default. L2
//                 (compute) and L3 (durable) are still measured separately,
//                 because doc 13 requires it: moving the commit off the owner
//                 thread does not shorten the time to durable, it only stops
//                 the owner blocking, and conflating the two would hide that.
//   AsyncWorker   the owner stages into a bounded ring that the persistence
//                 thread drains. At most one commit is in flight per risk
//                 domain; a second event for a busy domain is refused with
//                 RetryLaterNoAdmission rather than queued internally, because
//                 an internal queue is the invisible backlog LI-22 forbids.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "llx/backpressure.hpp"
#include "llx/clock.hpp"
#include "llx/domain.hpp"
#include "llx/event.hpp"
#include "llx/store.hpp"
#include "llx/telemetry.hpp"

namespace llx {

// Doc 18 contract.
struct IEngineOwner {
    virtual AdmissionOutcome on_event(const InputEnvelope&) noexcept = 0;
    virtual void on_commit_result(const CommitReceipt&) noexcept = 0;
    virtual ServiceMode mode() const noexcept = 0;
    virtual ~IEngineOwner() = default;
};

enum class CommitMode : std::uint8_t { Synchronous, AsyncWorker };

struct OwnerStats {
    std::uint64_t events_seen = 0;
    std::uint64_t accepted = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t stale_epoch = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t rejected_business = 0;
    std::uint64_t quarantined = 0;
    std::uint64_t no_effect = 0;
    std::uint64_t committed = 0;
    std::uint64_t commit_rejected = 0;
    std::uint64_t commit_unknown = 0;
    std::uint64_t held_for_reconciliation = 0;
    std::uint64_t receipts_reordered = 0;
    std::uint64_t receipts_stale = 0;
    std::uint64_t session_control = 0;
    // Non-zero means the owner observed something it has no safe response to.
    std::uint64_t integrity_violations = 0;
};

class OwnerActor final : public IEngineOwner {
public:
    struct Config {
        CommitMode commit_mode = CommitMode::Synchronous;
        std::uint32_t owner_epoch = 1;
        // Doc 6: shard by a stable risk-domain key, never per order, because a
        // shared firm limit has to be serialized by one authority.
        std::uint32_t shard_count = 1;
        std::uint32_t shard_index = 0;
        bool measure_spans = true;
    };

    OwnerActor(Config cfg, IAtomicBusinessStore& store, ServiceModeController& modes,
               ILatencySink& telemetry);

    AdmissionOutcome on_event(const InputEnvelope& ev) noexcept override;
    void on_commit_result(const CommitReceipt& r) noexcept override;
    ServiceMode mode() const noexcept override { return modes_.mode(); }

    // The health the controller evaluates. The owner contributes the parts only
    // it can see: unresolved unknown outcomes, ownership doubt, integrity.
    //
    // Reads atomics only, so it is safe from any thread. The underlying maps
    // and sets belong to the owner thread and must not be touched from
    // anywhere else -- a std::map walked while the owner inserts into it is
    // heap corruption, which is what an intermittent 0xC0000374 in the
    // threaded runtime turned out to be.
    void contribute_health(HealthSnapshot& h) const;

    std::uint32_t epoch() const { return cfg_.owner_epoch; }

    // Promotes this owner to a new epoch, fencing the previous one. A receipt
    // or an event from the old epoch is refused from here on (doc 19.5).
    void promote_to_epoch(std::uint32_t epoch);

    // True when this owner is responsible for the domain, given the shard map.
    bool owns_domain(DomainId d) const {
        return cfg_.shard_count <= 1 || (d % cfg_.shard_count) == cfg_.shard_index;
    }

    const OwnerStats& stats() const { return stats_; }

    // Events whose commit is in flight, by domain. Used by the engine to size
    // the persistence budget and by tests to prove the cap is honoured.
    std::size_t inflight() const { return inflight_.size(); }

    // Set when a reconciliation has resolved everything outstanding. Only the
    // operator path calls this; a timer never does (doc 21).
    void clear_unknowns() {
        unresolved_unknown_.clear();
        publish_health();
    }
    std::size_t unresolved_unknown() const { return unresolved_unknown_.size(); }

    // Staged work waiting for the persistence worker, in AsyncWorker mode.
    struct StagedCommit {
        CommitRequest request;
        InputEnvelope source;
        std::uint64_t staged_ns = 0;
    };
    // Called by the persistence thread to take the next staged commit. Returns
    // false when nothing is staged.
    bool take_staged(StagedCommit& out);

private:
    AdmissionOutcome process(const InputEnvelope& ev);
    // Copies the facts the mode controller needs into the atomics above.
    void publish_health();
    void finish_commit(const CommitReceipt& r, const InputEnvelope& src,
                       std::uint64_t commit_begin_ns);
    void sample(Span s, std::uint64_t a, std::uint64_t b, const EventId& id,
                std::uint8_t outcome);

    Config cfg_;
    IAtomicBusinessStore& store_;
    ServiceModeController& modes_;
    ILatencySink& telemetry_;

    // Applied identities. Bounded in a real deployment by the journal's inbox
    // horizon; here it is the full set, which is what makes the dedup oracle
    // exact in a test. Doc 20 counts it against the memory budget.
    std::set<EventId> applied_;
    std::set<ExecId> seen_exec_;
    // Next domain_seq to assign, per risk domain.
    std::map<DomainId, std::uint64_t> next_seq_;
    // Highest domain_seq whose commit result has been applied, per domain.
    std::map<DomainId, std::uint64_t> applied_seq_;
    // Receipts that arrived ahead of their causal predecessor. Held, not
    // applied: doc 19.5 requires the progression to stop rather than reorder
    // the business effect.
    std::map<DomainId, std::map<std::uint64_t, CommitReceipt>> reorder_;
    // Commits in flight, keyed by domain. At most one per domain.
    std::map<DomainId, StagedCommit> inflight_;
    // Outcomes with no evidence. While non-empty the engine is RECON_ONLY.
    std::set<std::uint64_t> unresolved_unknown_;
    // Staged but not yet taken by the persistence worker.
    std::vector<StagedCommit> staged_;
    bool integrity_violation_ = false;
    bool ownership_uncertain_ = false;
    // Published copies of the three facts the mode controller needs. Written
    // by the owner thread whenever the corresponding structure changes, read
    // by whichever thread evaluates health. The structures themselves stay
    // single-owner.
    std::atomic<std::size_t> published_unknown_{0};
    std::atomic<std::size_t> published_inflight_{0};
    std::atomic<bool> published_integrity_{false};
    std::atomic<bool> published_ownership_doubt_{false};
    OwnerStats stats_{};
};

}  // namespace llx
