// Deterministic fault injection.
//
// The 36 FI playbooks of design doc 25 and the 24 crash windows of doc 26 are
// only executable if the pipeline can be stopped at an exact boundary, so every
// boundary named in those tables has a failpoint. Doc 25 is also explicit that
// a playbook may not be marked PASS unless the failpoint is confirmed to have
// fired, which is why every point keeps a hit count that a test asserts on.
//
// Actions:
//   ReturnError  the instrumented operation reports a typed failure
//   AbruptExit   terminate immediately, no unwinding and no stdio flush. This
//                is the closest approximation to SIGKILL available on both
//                POSIX and Windows, and is used by the crash-window tests that
//                re-launch the process. It is not a power-loss test.
//   Delay        sleep for `param` milliseconds (consumer stall, DB stall)
//   Pause        block until another thread calls release(). This is the
//                deterministic barrier doc 26 asks for: "pause_at(failpoint)
//                bằng barrier deterministic", and it is what makes a window
//                like CW-04 (after slot write, before head release) reachable
//                without relying on a race.
//   Corrupt      hand the caller a corrupted value, used by LL-040 to prove the
//                engine halts on an impossible queue occupancy rather than
//                continuing with a false reading.
//
// Failpoints compile in only when LLX_ENABLE_FAILPOINTS is defined, so the
// measured build carries no branch on the hot path. Doc 22 requires them off
// for anything resembling production.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace llx::fp {

enum class Action : std::uint8_t {
    None,
    ReturnError,
    AbruptExit,
    Delay,
    Pause,
    Corrupt,
};

// Arm a failpoint. `occurrence` is 1-based: 1 fires on the first hit, which
// keeps a playbook's trigger unambiguous when the same boundary is crossed many
// times in a run.
void arm(std::string name, Action action, std::int64_t param = 0, int occurrence = 1);
void disarm(std::string_view name);
void disarm_all();

// Releases every thread parked on a Pause failpoint with this name, and leaves
// the point released so a later hit does not park again until it is re-armed.
void release(std::string_view name);

// How many times the point was reached, and how many times an armed action
// actually fired. A playbook asserts fired() > 0; doc 25 forbids a PASS
// without it.
std::uint64_t hits(std::string_view name);
std::uint64_t fired(std::string_view name);
void reset_counters();

// Value substituted by a Corrupt action.
std::int64_t corrupt_param(std::string_view name);

// Grammar, shared by the LLX_FAILPOINT environment variable and by the
// --failpoint argument the crash tests pass to a child process:
//   name=error | name=exit | name=delay:250 | name=pause | name=corrupt:42
//   name=exit@3        (fire on the third hit)
// A malformed specification aborts startup rather than letting a test score a
// pass against an uninstrumented binary.
void arm_from_spec(const std::string& spec);
void arm_from_env();

// Returns the action that fired, performing AbruptExit, Delay and Pause in
// place. The caller must honour ReturnError and Corrupt itself.
Action fire(std::string_view name);

// Boundary names. Kept in one place so a test and the engine cannot drift
// apart. The CW-xx references are the crash windows of doc 26 and the FI-xx
// references the playbooks of doc 25.
namespace point {

// Ring, doc 8 / CW-04..CW-06 / FI-01.
constexpr std::string_view kRingBeforeSlotWrite = "ring.before_slot_write";
constexpr std::string_view kRingAfterSlotWrite = "ring.after_slot_write";   // CW-04
constexpr std::string_view kRingAfterHeadRelease = "ring.after_head_release";  // CW-05
constexpr std::string_view kRingAfterConsumerAcquire = "ring.after_consumer_acquire";  // CW-06
constexpr std::string_view kRingBeforeTailRelease = "ring.before_tail_release";  // FI-01
constexpr std::string_view kRingCorruptOccupancy = "ring.corrupt_occupancy";  // LL-040

// Pool, doc 9 / CW-03, CW-24 / FI-05..FI-07.
constexpr std::string_view kPoolAfterLease = "pool.after_lease";  // CW-03
constexpr std::string_view kPoolBeforeRelease = "pool.before_release";
constexpr std::string_view kPoolAfterRelease = "pool.after_release";  // CW-24
constexpr std::string_view kPoolExhaust = "pool.exhaust";             // FI-05

// Owner, doc 19 / CW-07..CW-10.
constexpr std::string_view kOwnerAfterDequeue = "owner.after_dequeue";          // CW-07
constexpr std::string_view kOwnerAfterReduce = "owner.after_reduce";            // CW-08
constexpr std::string_view kOwnerAfterRiskCompute = "owner.after_risk_compute"; // CW-09
constexpr std::string_view kOwnerAfterRiskStage = "owner.after_risk_stage";     // CW-10
constexpr std::string_view kOwnerBeforeCommit = "owner.before_commit";
constexpr std::string_view kOwnerAfterCommitResult = "owner.after_commit_result";  // CW-18

// Store, doc 13 / CW-11..CW-18 / FI-20..FI-24.
constexpr std::string_view kStoreAfterBegin = "store.after_begin";                // CW-11
constexpr std::string_view kStoreAfterInbox = "store.after_inbox";                // CW-12
constexpr std::string_view kStoreAfterJournal = "store.after_journal";            // CW-13
constexpr std::string_view kStoreAfterProjection = "store.after_projection";      // CW-14
constexpr std::string_view kStoreAfterRiskRow = "store.after_risk_row";           // CW-15
constexpr std::string_view kStoreAfterOutbox = "store.after_outbox";              // CW-16
constexpr std::string_view kStoreCommitLostAck = "store.commit_lost_ack";         // CW-17
constexpr std::string_view kStoreCommitStall = "store.commit_stall";              // FI-21
constexpr std::string_view kStoreAfterCommit = "store.after_commit";

// Dispatch, doc 19 / CW-19..CW-22 / FI-25..FI-27.
constexpr std::string_view kDispatchAfterClaim = "dispatch.after_claim";          // CW-19
constexpr std::string_view kDispatchBeforeEncode = "dispatch.before_encode";      // CW-20
constexpr std::string_view kDispatchAfterEncode = "dispatch.after_encode";        // CW-21
constexpr std::string_view kDispatchPartialWrite = "dispatch.partial_write";      // CW-22
constexpr std::string_view kDispatchAfterWrite = "dispatch.after_write";

// Inbound reconciliation, doc 19 / CW-23.
constexpr std::string_view kInboundAfterParse = "inbound.after_parse";            // CW-23

// Telemetry and environment, doc 14 / FI-28..FI-33.
constexpr std::string_view kTelemetryBufferFull = "telemetry.buffer_full";
constexpr std::string_view kClockRewind = "clock.rewind";
constexpr std::string_view kAffinityPinFail = "affinity.pin_fail";

}  // namespace point

// The no-op form, used when failpoints are compiled out.
//
// A function rather than a bare `Action::None`: LLX_FP appears both as a
// statement and inside an expression, and expanding the statement form to a
// plain enumerator makes it a statement with no effect, which -Wunused-value
// reports once per call site. An inline function call is valid in both
// positions and compiles to nothing.
inline Action fire_disabled(std::string_view) noexcept { return Action::None; }

}  // namespace llx::fp

#ifdef LLX_ENABLE_FAILPOINTS
#define LLX_FP(name) (::llx::fp::fire(name))
#else
#define LLX_FP(name) (::llx::fp::fire_disabled(name))
#endif
