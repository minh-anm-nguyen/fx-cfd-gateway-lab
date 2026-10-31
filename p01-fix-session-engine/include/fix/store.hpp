// Durable session store interface.
//
// Reference: design doc 9 (durability, crash consistency, startup recovery).
// Two operations carry the whole crash-consistency argument of this project:
//
//   commit_outbound : atomically appends the original outbound frame to history
//                     AND advances NextNumOut. If it does not commit, the frame
//                     must never reach the socket (invariant I-06).
//   commit_inbound  : atomically records the inbox row (application messages
//                     only) AND advances NextNumIn. If it does not commit, the
//                     message must not be treated as processed (invariant I-07).
//
// Everything else exists to detect a store that is lying: integrity checksums,
// cross-checks between state and history, and an exclusive ownership lease.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fix/result.hpp"

namespace fixse {

struct SessionKey {
    std::string begin_string = "FIX.4.4";
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string qualifier;

    std::string id() const {
        return begin_string + ":" + sender_comp_id + ":" + target_comp_id + ":" + qualifier;
    }
    bool operator==(const SessionKey&) const = default;
};

struct PersistedState {
    uint64_t epoch = 1;
    // NextNumIn / NextNumOut as defined in design doc 8.1. Both are 1-based and
    // must never decrease within an epoch (invariant I-02).
    uint64_t next_in = 1;
    uint64_t next_out = 1;
    uint64_t last_good_connection_generation = 0;
    uint64_t version = 0;  // optimistic concurrency counter, bumped per commit
};

struct OutboundRecord {
    uint64_t seq = 0;
    std::string msg_type;
    std::string payload;  // the exact original bytes that went on the wire
    std::string original_sending_time;
};

enum class DeliveryState { Pending, Delivered };

struct InboundRecord {
    uint64_t seq = 0;
    std::string msg_type;
    std::string payload;
    DeliveryState delivery_state = DeliveryState::Pending;
};

// Audit event types written to recovery_audit. These are the records an
// operator reads after an incident, so they are explicit rather than free text.
enum class AuditEvent {
    StoreOpened,
    OwnershipAcquired,
    OwnershipRefused,
    UncleanShutdownDetected,
    StateTransition,
    GapDetected,
    ResendRequested,
    ResendServed,
    GapFillSent,
    GapFillReceived,
    GapFillQuarantined,
    DuplicateIgnored,
    SequenceResetRefused,
    CoordinatedResetApplied,
    IntegrityFailure,
    Faulted,
    LogonRejected,
    TestRequestTimeout,
    InboxOverflow,
    ResendStormBlocked,
};

std::string_view to_string(AuditEvent e);

struct AuditRecord {
    AuditEvent event = AuditEvent::StateTransition;
    uint64_t epoch = 0;
    uint64_t seq_from = 0;
    uint64_t seq_to = 0;
    std::string reason;
};

// Result of the startup consistency scan (design doc 9.4).
struct ConsistencyReport {
    bool ok = false;
    uint64_t max_outbound_seq = 0;
    uint64_t max_inbox_seq = 0;
    size_t pending_inbox_rows = 0;
    bool integrity_checksum_valid = false;
    bool clean_shutdown = false;
    std::string problem;
};

class ISessionStore {
public:
    virtual ~ISessionStore() = default;

    // Load persisted state. A store that has never seen this session returns
    // the initial state (epoch 1, next_in 1, next_out 1) and records it.
    virtual StoreError load_state(PersistedState* out) = 0;

    // Atomic: history row for the ORIGINAL frame plus the new NextNumOut.
    virtual StoreError commit_outbound(const OutboundRecord& rec, uint64_t next_out) = 0;

    // Atomic: optional inbox row plus the new NextNumIn. Pass nullptr for a
    // session-level message, which advances sequencing without becoming an
    // application event.
    virtual StoreError commit_inbound(const InboundRecord* rec, uint64_t next_in) = 0;

    // Mark an inbox row delivered after the sink acknowledged it. A crash
    // between delivery and this call means at-least-once delivery, which the
    // sink must tolerate (invariant I-11, scenario G-07).
    virtual StoreError mark_delivered(uint64_t seq) = 0;

    virtual StoreError load_outbound_range(uint64_t from_seq, uint64_t to_seq,
                                           std::vector<OutboundRecord>* out) = 0;
    virtual StoreError load_pending_inbox(size_t max_rows, std::vector<InboundRecord>* out) = 0;

    // Look up a single inbox row. Used to compare a retransmitted message
    // against what was already committed for that sequence: same sequence with
    // different content is an integrity problem, not a duplicate (E-20).
    virtual StoreError load_inbox_record(uint64_t seq, InboundRecord* out, bool* found) = 0;
    virtual StoreError append_audit(const AuditRecord& rec) = 0;
    virtual StoreError record_connection_generation(uint64_t generation) = 0;

    // A coordinated sequence reset: a new epoch with NextNumIn and NextNumOut
    // back to 1. Requires two distinct approver identities and is always
    // audited (design doc 11.2, scenarios J-15, K-11, PB-12).
    virtual StoreError coordinated_reset(const std::string& approver_a,
                                         const std::string& approver_b,
                                         const std::string& reason) = 0;

    virtual ConsistencyReport verify_consistency() = 0;
    virtual bool owns_session() const = 0;
    // Flag a clean exit so the next startup can distinguish a crash from a
    // planned shutdown (scenario G-22).
    virtual void mark_clean_shutdown() = 0;
    virtual size_t audit_count() const = 0;
};

}  // namespace fixse
