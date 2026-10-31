// Outbox dispatch: the P04/P01 boundary.
//
// Design doc 13 and 19.6. The dispatcher is the only thing that puts an
// original business command on the wire, and it may do so only from a durable
// outbox record under the current fencing epoch. Everything else here follows
// from one rule, LI-18 and ADR-09: an ambiguous send is not a failed send.
//
// Concretely, after an attempt whose outcome is unknown:
//   * the original is NOT reissued, and certainly not with a new ClOrdID. The
//     LP may already be working it; a second original would be a duplicate
//     order, which is the worst outcome available (doc 13 "forbidden
//     shortcuts");
//   * the record moves to Ambiguous and the reconciliation path owns it;
//   * a timeout never converts to success or to failure. Doc 13: "timeout
//     không phải permission để tạo New với ClOrdID mới."
//
// The dispatcher also enforces exactly-one original per ClOrdID across the
// whole run, which is the oracle group 19 of the catalog tests against.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "llx/store.hpp"

namespace llx {

enum class SendResult : std::uint8_t {
    Acked,    // the write completed locally; the LP has not necessarily seen it
    Failed,   // the write definitively did not happen
    Unknown,  // partial write, timeout or disconnect mid-write (CW-22)
};

std::string_view to_string(SendResult r);

// The session boundary. P01 owns the real one; this is the port P07 talks to.
struct IFixSink {
    virtual SendResult send(const std::string& frame) = 0;
    virtual ~IFixSink() = default;
};

// Test sink. Records what was sent and can be told to fail or to go ambiguous
// on a chosen attempt, which is how the send-ambiguity cases are driven without
// a network.
class RecordingSink final : public IFixSink {
public:
    SendResult send(const std::string& frame) override;

    // Make attempt number `n` (1-based) return `r`.
    void program(std::uint64_t n, SendResult r) { programmed_[n] = r; }
    void program_all(SendResult r) { default_ = r; }

    const std::vector<std::string>& frames() const { return frames_; }
    std::uint64_t attempts() const { return attempts_; }
    void clear() {
        frames_.clear();
        attempts_ = 0;
        programmed_.clear();
        default_ = SendResult::Acked;
    }

private:
    std::vector<std::string> frames_;
    std::map<std::uint64_t, SendResult> programmed_;
    SendResult default_ = SendResult::Acked;
    std::uint64_t attempts_ = 0;
};

struct DispatchStats {
    std::uint64_t claimed = 0;
    std::uint64_t sent = 0;
    std::uint64_t failed = 0;
    std::uint64_t ambiguous = 0;
    std::uint64_t refused_stale_epoch = 0;
    std::uint64_t refused_ambiguous_on_restart = 0;
    // The invariant counter: a second original for the same ClOrdID would
    // increment this, and any non-zero value is a hard release block (LI-16).
    std::uint64_t duplicate_original_attempts = 0;
};

class Dispatcher {
public:
    Dispatcher(IAtomicBusinessStore& store, IFixSink& sink) : store_(store), sink_(sink) {}

    // Drains up to `max` eligible outbox records. Returns how many were
    // dispatched. Anything already Ambiguous is left alone: it belongs to
    // reconciliation, not to the send path.
    std::size_t pump(std::uint32_t owner_epoch, std::size_t max = 64);

    // Records that a reconciliation established the outcome of an ambiguous
    // send. This is the only route out of Ambiguous, and it requires evidence
    // (an ExecutionReport, or an idempotency-key lookup that proves the state).
    bool reconcile(std::uint64_t outbox_id, bool lp_has_order);

    // Outbox rows still waiting for evidence. While this is non-empty the
    // engine stays in RECON_ONLY for that domain (doc 21).
    std::vector<OutboxRecord> ambiguous_records() const;

    const DispatchStats& stats() const { return stats_; }

    // FIX 4.4-shaped text, enough to be identity-bearing and comparable between
    // the baseline and the optimized pipeline. It is not a conformant encoder:
    // P01 owns the wire format, and doc 1.3 forbids P07 from reimplementing it.
    static std::string encode(const OutboxRecord& ob);

private:
    IAtomicBusinessStore& store_;
    IFixSink& sink_;
    // ClOrdIDs for which an original has already been put on the wire.
    std::map<OrderId, std::uint32_t> originals_;
    DispatchStats stats_{};
};

}  // namespace llx
