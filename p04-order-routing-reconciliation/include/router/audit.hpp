// Append-only audit with a hash chain, plus the redaction rules.
//
// Design doc 17.1 asks for three separate things and this header carries all of
// them: an immutable record of every admission, hold and operator action
// (R-I22); detectable tampering (OR-Q-18); and no secret or raw PII in anything
// that gets persisted or logged (OR-Q-05, OR-Q-06, OR-Q-15).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "router/clock.hpp"
#include "router/hash.hpp"
#include "router/ids.hpp"

namespace router {

enum class AuditKind {
    CommandAdmitted,
    CommandRejected,
    CommandDuplicate,
    RouteDecided,
    OriginalSendAssigned,
    SendAmbiguous,
    InboundApplied,
    InboundDeduplicated,
    InboundQuarantined,
    HoldOpened,
    HoldResolved,
    ReconOpened,
    ReconProbeSent,
    ReconProbeResult,
    ReconResolved,
    OperatorAction,
    OperatorDenied,
    ConfigChanged,
    IntegrityFailure,
    StateHalted,
    StateResumed,
};

std::string_view to_string(AuditKind k);

struct AuditRecord {
    std::uint64_t seq = 0;
    AuditKind kind = AuditKind::CommandAdmitted;
    WallMicros wall = 0;
    MonoMicros mono = 0;
    std::string actor;       // who caused it, "system" for internal transitions
    std::string subject;     // command_id, chain_id or case_id
    std::string reason;      // enum name, never free-form peer text
    std::string detail;      // already sanitised
    Hash evidence;           // digest of the raw frame or payload this refers to
    Hash prev_hash;
    Hash self_hash;
};

// Roles used by the operator workflow (design doc 17.1, OR-Q-07..Q-09).
enum class Role { Viewer, Trader, Operator, Admin };

struct Principal {
    ActorId actor;
    TenantId tenant;
    Role role = Role::Viewer;
};

class AuditLog {
public:
    // Appends and chains. Returns the record, or nothing when the audit store is
    // unavailable: the caller must then fail closed rather than proceed (OR-Q-10).
    std::optional<AuditRecord> append(AuditRecord rec);

    // Recomputes every self_hash and link. False means someone edited history.
    bool verify() const;
    // Verifies only records at or after `from`, taking the link from the record
    // before it. Records are append only and immutable once written, so a
    // caller that has already verified a prefix does not pay for it again on
    // every step. The full verify() is what the integrity gate and OR-Q-18 use.
    bool verify_from(std::size_t from) const;

    const std::vector<AuditRecord>& records() const { return records_; }
    std::size_t size() const { return records_.size(); }

    // Count of records of one kind, used heavily by the matrix assertions.
    std::size_t count(AuditKind k) const;
    std::vector<AuditRecord> with_subject(const std::string& subject) const;

    void set_available(bool v) { available_ = v; }
    bool available() const { return available_; }

    // Test-only: corrupts one record so OR-Q-18 can prove detection works.
    bool tamper_for_test(std::size_t index, const std::string& new_detail);

private:
    static Hash compute(const AuditRecord& rec);

    std::vector<AuditRecord> records_;
    bool available_ = true;
};

// ---------------------------------------------------------------------------
// Sanitisation.

// Replaces control characters and anything non-ASCII, and truncates. Used on
// every peer-supplied string before it reaches a log line or an audit record so
// that tag 58 cannot forge a log entry (OR-Q-05).
std::string sanitize_text(std::string_view raw, std::size_t max_len = 200);

// Rewrites the value of any credential-bearing FIX tag to "***". Applied before
// a raw frame is stored or printed (OR-Q-06).
std::string redact_fix_frame(std::string_view frame);

// Printable rendering of a frame with SOH shown as a vertical bar.
std::string wire_text(std::string_view frame);

// True when the principal may perform the action. Deliberately explicit rather
// than a bitmask so each decision reads as policy.
bool may_submit_orders(const Principal& p);
bool may_force_resolve(const Principal& p);
bool may_change_config(const Principal& p);
bool may_view_raw_frames(const Principal& p);

}  // namespace router
