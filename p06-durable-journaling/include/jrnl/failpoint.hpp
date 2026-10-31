// Fault injection points.
//
// Section 15 of the design doc names 24 crash windows and section 26.3 asks
// for storage errors injected "at every SQL statement and C++ exception
// boundary". A window is only a real test if the process can be stopped
// exactly inside it, so every durability boundary in the store carries a
// named point.
//
// Abrupt termination uses an immediate exit with no unwinding and no stdio
// flush. That is the closest approximation to SIGKILL available on both POSIX
// and Windows. It is NOT a power-loss test: section 17.1 is explicit that
// durability past the fsync contract needs hardware whose write cache is
// under test control, and that stays outside this lab.
//
// Failpoints compile in only when JRNL_ENABLE_FAILPOINTS is defined. They are
// armed either from a test or from the JRNL_FAILPOINT environment variable,
// so a crash test can hand an exact boundary to a child process.
#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace jrnl::fp {

enum class Action {
    None,
    ReturnError,  // the instrumented call returns a typed failure
    AbruptExit,   // terminate immediately, no unwinding, no flush
    Delay,        // sleep for param milliseconds
    Throw,        // raise an exception, to exercise the RAII rollback path
};

// Arm a failpoint. occurrence is 1-based: 1 fires on the first hit.
void arm(std::string name, Action action, int param = 0, int occurrence = 1);
void disarm(std::string_view name);
void disarm_all();

// Parse JRNL_FAILPOINT, of the form
//   name=error | name=exit | name=throw | name=delay:250 | name=exit@3
// A malformed specification aborts startup rather than leaving a test to
// score an uninstrumented run as a pass.
void arm_from_env();
void arm_from_spec(const std::string& spec);

// Returns the action that fired, performing AbruptExit, Throw and Delay in
// place. The caller must honour ReturnError by failing the operation.
Action fire(std::string_view name);

// How many times a point has been reached this process. Lets a test assert
// that a boundary was actually crossed rather than silently skipped.
int hits(std::string_view name);

// Thrown by Action::Throw.
struct InjectedFault : std::exception {
    std::string where;
    explicit InjectedFault(std::string w) : where(std::move(w)) {}
    const char* what() const noexcept override { return where.c_str(); }
};

// Boundary names. Kept in one place so the store, the crash suite and the
// shell scripts cannot drift apart. The trailing comment is the crash window
// from section 15 that the point makes reachable.
namespace point {

// --- Business transaction boundaries, W02..W12 -----------------------------
constexpr std::string_view kBeforeBegin = "tx.before_begin";                    // W02
constexpr std::string_view kAfterBegin = "tx.after_begin";                      // W03
constexpr std::string_view kAfterFenceCheck = "tx.after_fence_check";           // W04
constexpr std::string_view kAfterDedupInsert = "tx.after_dedup_insert";         // W05
constexpr std::string_view kAfterJournalAppend = "tx.after_journal_append";     // W06
constexpr std::string_view kAfterStreamHead = "tx.after_stream_head";           // W06
constexpr std::string_view kAfterOrderProjection = "tx.after_order_projection"; // W07
constexpr std::string_view kAfterRiskProjection = "tx.after_risk_projection";   // W08
constexpr std::string_view kAfterCommandIntent = "tx.after_command_intent";     // W09
constexpr std::string_view kAfterOutboxInsert = "tx.after_outbox_insert";       // W10
constexpr std::string_view kAfterManifest = "tx.after_manifest";                // W10
constexpr std::string_view kBeforeCommit = "tx.before_commit";                  // W10
constexpr std::string_view kDuringCommit = "tx.during_commit";                  // W11
constexpr std::string_view kAfterCommit = "tx.after_commit";                    // W12

// --- Outbox dispatch boundaries, W13..W16 ----------------------------------
constexpr std::string_view kAfterOutboxClaim = "outbox.after_claim";            // W13
constexpr std::string_view kBeforeStableSend = "outbox.before_stable_send";     // W13
constexpr std::string_view kAfterSeqPersisted = "p01.after_seq_persisted";      // W14
constexpr std::string_view kDuringSocketWrite = "p01.during_socket_write";      // W15
constexpr std::string_view kAfterSocketWrite = "p01.after_socket_write";        // W16
constexpr std::string_view kBeforeBindingRecorded = "outbox.before_binding";    // W16

// --- Inbound boundaries, W17..W21 ------------------------------------------
constexpr std::string_view kAfterRawReceiptPersisted = "p01.after_raw_receipt"; // W18
constexpr std::string_view kAfterInboxInsert = "inbox.after_insert";            // W19
constexpr std::string_view kAfterExecKeyInsert = "inbox.after_exec_key";        // W20
constexpr std::string_view kBeforeDeliveryAck = "inbox.before_delivery_ack";    // W21

// --- Snapshot boundaries, W22 ----------------------------------------------
constexpr std::string_view kAfterSnapshotInsert = "snapshot.after_insert";      // W22
constexpr std::string_view kBeforeSnapshotVerified = "snapshot.before_mark";    // W22

// --- Storage and recovery --------------------------------------------------
constexpr std::string_view kStorageWrite = "storage.write";
constexpr std::string_view kStorageRead = "storage.read";
constexpr std::string_view kReplayMidStream = "replay.mid_stream";
constexpr std::string_view kRecoveryAfterReplay = "recovery.after_replay";

}  // namespace point

}  // namespace jrnl::fp

#ifdef JRNL_ENABLE_FAILPOINTS
#define JRNL_FP(name) (::jrnl::fp::fire(name))
#else
#define JRNL_FP(name) (::jrnl::fp::Action::None)
#endif
