// The single-owner order actor.
//
// One owner per chain, one event at a time, in a total order fixed by the
// durable inbox. Everything the reducer cannot know lives here: which chain an
// event belongs to, whether an identity is already taken, whether the mailbox
// has room, and whether the commit succeeded.
//
// The sequence is the one in design doc 5.2, and the crash windows are checked
// between its steps so a test can cut the process at each boundary:
//
//   append to durable inbox        (W00 before parse)
//   validate / correlate           (W01, W02)
//   semantic dedup                 (W03)
//   pure reduce                    (W04)
//   atomic commit                  (W05..W08, W12 inside the store)
//   ack the inbox                  (W09)
//   dispatch the outbox            (W10, W11)
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "order_core/store.hpp"

namespace order_core {

struct EngineResult {
    Decision decision = Decision::RejectLocal;
    Reason reason = Reason::None;
    std::string detail;
    OrderId id;
    uint64_t revision = 0;
    CommitStatus commit_status = CommitStatus::Committed;
    bool committed = false;
    std::vector<InvariantViolation> violations;
    std::vector<EffectIntent> intents;

    [[nodiscard]] bool ok() const { return decision == Decision::Applied && committed; }
};

// Audit record emitted per event, per design doc 16.1.
struct AuditRecord {
    uint64_t trace_id = 0;
    uint64_t chain_id = 0;
    uint64_t source_inbox_id = 0;
    std::string source_session_id;
    uint64_t source_seq = 0;
    std::string fix_exec_id;
    std::string fix_cl_ord_id;
    uint64_t old_revision = 0;
    uint64_t new_revision = 0;
    std::string decision;
    std::string reason;
    uint32_t policy_version = 0;
    uint64_t reconciliation_generation = 0;
    uint64_t before_hash = 0;
    uint64_t after_hash = 0;
    std::string detail;

    [[nodiscard]] std::string text() const;
};

// Normalized event in, state committed out. Deliberately not comparable with
// a receive-to-route figure: there is no socket, no framer and no router in
// this measurement, and the design doc insists the two are reported under
// different names (what-if Q-08).
struct LatencySummary {
    size_t samples = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t max_ns = 0;
    [[nodiscard]] const char* what_is_measured() const {
        return "normalized event -> state commit (excludes TCP receive, FIX decode and routing)";
    }
};

struct EngineMetrics {
    uint64_t events_ingested = 0;
    uint64_t applied = 0;
    uint64_t duplicates = 0;
    uint64_t conflicts = 0;
    uint64_t quarantined = 0;
    uint64_t rejected_local = 0;
    uint64_t held = 0;
    uint64_t unknown_outcome = 0;
    uint64_t commit_failures = 0;
    uint64_t commit_conflicts = 0;
    uint64_t ambiguous_commits = 0;
    uint64_t backpressure_rejections = 0;
    uint64_t orphan_reports = 0;
    uint64_t retry_commits = 0;
    uint64_t queue_high_water = 0;
    uint64_t effects_dispatched = 0;
    uint64_t effects_suppressed_duplicate = 0;
};

// An idempotent outbound port. Real issue-once across a network and a database
// is P01/P04/P06's problem (open question 10 in the design doc); this records
// what was handed out so a test can see whether a replayed effect would have
// become a second order.
class RecordingEffectSink {
  public:
    void dispatch(const OutboxRow& row);
    [[nodiscard]] const std::vector<OutboxRow>& dispatched() const { return dispatched_; }
    [[nodiscard]] size_t count_key(const std::string& key) const;
    [[nodiscard]] size_t unique_keys() const { return seen_.size(); }
    [[nodiscard]] size_t suppressed() const { return suppressed_; }
    void clear() {
        dispatched_.clear();
        seen_.clear();
        suppressed_ = 0;
    }

  private:
    std::vector<OutboxRow> dispatched_;
    std::map<std::string, size_t> seen_;
    size_t suppressed_ = 0;
};

enum class RecoveryStatus {
    Clean,            // replay reproduced every recorded state hash
    HashDivergence,   // a replayed chain disagreed with the journal: do not trade
    CorruptJournal,   // a record failed verification
    SnapshotRejected, // snapshot unusable; recovered from the journal instead
    SchemaMismatch,   // journal or snapshot written by an incompatible version
    Halted,           // recovery refused to continue
};
const char* to_string(RecoveryStatus s);

struct RecoveryReport {
    RecoveryStatus status = RecoveryStatus::Clean;
    size_t records_replayed = 0;
    size_t chains_recovered = 0;
    uint64_t first_divergent_event = 0;
    std::string detail;
    bool trading_allowed = false;
};

class Engine {
  public:
    Engine(MemoryStore& store, Policy policy);

    // Puts the event in the durable inbox and processes it. Returns the
    // decision for that event. A SimulatedCrash propagates to the caller: that
    // is the process dying.
    EngineResult submit(const OrderEvent& event);

    // Ingest without processing, so a test can fill the mailbox.
    bool ingest(const OrderEvent& event);
    // Process one pending inbox row. Returns false when there is nothing to do.
    bool pump(EngineResult* out = nullptr);
    size_t pump_all();

    // Rebuilds every index and every aggregate from the store. Re-derives
    // state by replaying journaled events through the same reducer and
    // compares the result with the hash each record recorded, so a divergence
    // blocks trading instead of being discovered later (what-if Q-06).
    RecoveryReport recover();

    // Hands committed outbox rows to the sink. Called automatically after a
    // successful commit, and again by recovery for rows a crash left pending.
    size_t dispatch_outbox();

    [[nodiscard]] const OrderAggregate* find(OrderId id) const;
    [[nodiscard]] const OrderAggregate* find_by_cl_ord_id(const std::string& firm,
                                                          const std::string& day,
                                                          const std::string& cl_ord_id) const;
    [[nodiscard]] size_t chain_count() const { return chains_.size(); }
    [[nodiscard]] const Policy& policy() const { return policy_; }

    // Hot reload. The new configuration is validated first, and a change to
    // the identity namespace is refused while dedup rows exist under the old
    // one unless the caller can show the index was migrated (what-if P-04).
    // Accepted versions keep the scale and policy version they were admitted
    // under, whatever the new config says (what-if P-01, P-02, D-11).
    ConfigError reload_policy(Policy p, bool dedup_index_migrated = false);
    [[nodiscard]] const EngineMetrics& metrics() const { return metrics_; }
    [[nodiscard]] const std::vector<AuditRecord>& audit() const { return audit_; }
    [[nodiscard]] LatencySummary event_to_commit_latency() const;
    RecordingEffectSink& sink() { return sink_; }
    MemoryStore& store() { return store_; }
    [[nodiscard]] bool trading_allowed() const { return trading_allowed_; }
    void set_trading_allowed(bool v) { trading_allowed_ = v; }

    // Simulates a second owner writing the same chain at a stale revision, to
    // show the revision CAS refuses it (what-if N-02, J-? split brain).
    CommitResult commit_as_second_writer(OrderId id, uint64_t expected_revision,
                                         const OrderEvent& event);

    [[nodiscard]] size_t held_count(OrderId id) const;
    [[nodiscard]] size_t total_held() const;

  private:
    struct Correlation {
        OrderId id;
        bool create_new = false;
        Decision decision = Decision::Applied;
        Reason reason = Reason::None;
        std::string detail;
    };

    Correlation correlate(const OrderEvent& event);
    [[nodiscard]] bool cl_ord_id_unresolved_elsewhere(const LocalCommand& cmd) const;
    EngineResult process(const OrderEvent& event, uint64_t inbox_seq);
    EngineResult finish(const OrderEvent& event, OrderId id, const Reduction& reduction,
                        uint64_t inbox_seq, uint64_t before_hash);
    void index_chain(OrderId id, const OrderAggregate& a);
    void retry_held(OrderId id);
    [[nodiscard]] bool consumed(uint64_t inbox_seq) const;
    void record_audit(const OrderEvent& event, OrderId id, const Reduction& reduction,
                      uint64_t inbox_seq, uint64_t before_hash, uint64_t after_hash);

    MemoryStore& store_;
    Policy policy_;
    std::map<uint64_t, OrderAggregate> chains_;
    std::map<std::string, uint64_t> request_index_;  // RequestKey::text() -> chain
    // ClOrdID alone -> the chains that use it. A peer report carries no firm
    // or business day, so this is the index correlation actually has to use.
    // More than one chain under a ClOrdID makes the report ambiguous, which is
    // a quarantine rather than an arbitrary pick.
    std::map<std::string, std::vector<uint64_t>> cl_ord_id_index_;
    std::map<std::string, uint64_t> alias_index_;    // peer|order_id -> chain
    std::map<uint64_t, std::vector<OrderEvent>> held_;
    std::vector<AuditRecord> audit_;
    EngineMetrics metrics_;
    RecordingEffectSink sink_;
    uint64_t next_order_id_ = 1;
    uint64_t next_event_index_ = 1;
    uint64_t next_trace_id_ = 1;
    // Bounded reservoir: metrics must not grow without a limit either.
    std::vector<uint64_t> latency_ns_;
    static constexpr size_t kMaxLatencySamples = 65536;
    bool trading_allowed_ = true;
    bool retrying_held_ = false;
};

}  // namespace order_core
