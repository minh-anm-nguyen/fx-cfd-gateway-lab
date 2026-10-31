// Deterministic fault injection.
//
// Design doc 15.1 lists twenty crash windows and 22.1 requires step-based
// failpoints rather than sleeps. A failpoint is a named spot in the code that
// a test can arm; when the code reaches an armed point it either throws the
// crash marker or returns a degraded result, always on a counted schedule so
// the same seed reproduces the same interleaving.
#pragma once

#include <exception>
#include <string>
#include <unordered_map>

namespace router {

// Thrown by an armed Crash failpoint. Only the lab harness catches it: inside
// the router it unwinds like a process death would, which is the point.
struct CrashMarker : std::exception {
    std::string point;
    explicit CrashMarker(std::string p) : point(std::move(p)) {}
    const char* what() const noexcept override { return point.c_str(); }
};

// The named points. The CW-xx comments map each one to the crash window table in
// design doc 15.1 so a test can cite the window it is covering.
namespace fp {
inline constexpr const char* kBeforeIntentCommit = "before_intent_commit";      // CW-01
inline constexpr const char* kInsideIntentCommit = "inside_intent_commit";      // CW-02
inline constexpr const char* kAfterIntentCommit = "after_intent_commit";        // CW-03
inline constexpr const char* kAfterOutboxClaim = "after_outbox_claim";          // CW-04
inline constexpr const char* kDropSendOnceReply = "drop_send_once_reply";       // CW-05
inline constexpr const char* kBeforeSendBinding = "before_send_binding";        // CW-06
inline constexpr const char* kAfterSendBinding = "after_send_binding";          // CW-07
inline constexpr const char* kPartialWireWrite = "partial_wire_write";          // CW-08
inline constexpr const char* kAfterWireWrite = "after_wire_write";              // CW-09
inline constexpr const char* kLpAfterAccept = "lp_after_accept";                // CW-10
inline constexpr const char* kAfterInboxCommit = "after_inbox_commit";          // CW-11
inline constexpr const char* kAfterNormalize = "after_normalize";               // CW-12
inline constexpr const char* kInsideApplyTxn = "inside_apply_txn";              // CW-13
inline constexpr const char* kAfterApplyCommit = "after_apply_commit";          // CW-14
inline constexpr const char* kBeforeRiskEffect = "before_risk_effect";          // CW-15
inline constexpr const char* kDuringStatusDispatch = "during_status_dispatch";   // CW-16
inline constexpr const char* kAfterStatusReply = "after_status_reply";          // CW-17
inline constexpr const char* kDuringSnapshotPrune = "during_snapshot_prune";     // CW-18
inline constexpr const char* kCommitAckLost = "commit_ack_lost";                // CW-19
inline constexpr const char* kStoreFull = "store_full";                         // OR-O-20
inline constexpr const char* kStoreIoError = "store_io_error";
inline constexpr const char* kAuditWriteFail = "audit_write_fail";              // OR-Q-10
inline constexpr const char* kAllocFail = "alloc_fail";                         // OR-P-13
}  // namespace fp

enum class FaultKind {
    Crash,    // throw CrashMarker: the process is gone at this instruction
    Error,    // return a typed failure from the surrounding operation
    Silent,   // perform the work but lose the reply (ambiguity, not failure)
};

class FailpointRegistry {
public:
    struct Arm {
        FaultKind kind = FaultKind::Crash;
        int skip = 0;    // let this many hits pass first
        int times = 1;   // then fire this many times, -1 for always
        int fired = 0;
        int hits = 0;
    };

    void arm(const std::string& point, FaultKind kind, int skip = 0, int times = 1);
    void disarm(const std::string& point);
    void clear();

    // Returns the kind to apply, or nullopt when the point is not firing. Always
    // counts the hit, so a test can assert a point was reached at all.
    bool should_fire(const std::string& point, FaultKind& kind);

    // Convenience for the common case: throws CrashMarker when armed as Crash.
    void check(const std::string& point);

    int hits(const std::string& point) const;
    int fired(const std::string& point) const;

private:
    std::unordered_map<std::string, Arm> arms_;
    std::unordered_map<std::string, int> hits_;
};

}  // namespace router
