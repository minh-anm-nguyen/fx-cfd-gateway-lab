// Named failpoints for the 20 crash windows in §23.1.
//
// A failpoint is a labelled position in the commit path. A test arms one with
// an action (fail the commit, report the outcome as unknown, or kill the
// process) and the production code reaches it through a single `reach()` call,
// so the crash windows are part of the engine's structure rather than something
// a test reconstructs by timing.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace risk {

// The window identifiers are the spec's, verbatim, so a failing test names the
// row of §23.1 it came from.
enum class Failpoint : std::uint8_t {
  None = 0,
  BeforeAuthz,                 // CW-01
  AfterAuthzBeforeIdempotency, // CW-02
  AfterSnapshotBeforeMath,     // CW-03
  AfterMathBeforeDomainLock,   // CW-04
  AfterLockBeforeBegin,        // CW-05
  AfterCommandInsert,          // CW-06
  AfterReservationInsert,      // CW-07
  AfterProjectionUpdate,       // CW-08
  AfterAuditInsert,            // CW-09
  AfterOutboxInsert,           // CW-10
  AfterRevisionCas,            // CW-11
  AfterDurableCommitBeforeAck, // CW-12
  AfterAllowBeforeClaim,       // CW-13
  AfterClaimBeforeSequence,    // CW-14
  AfterSequenceBeforeWrite,    // CW-15
  AfterSocketWriteBeforeAck,   // CW-16
  AfterInboundDurableBeforeApply, // CW-17
  AfterLedgerStageBeforeCommit,   // CW-18
  AfterApplyCommitBeforeCursorAck, // CW-19
  AfterReleaseCommitBeforeAck,     // CW-20
  Count,
};

std::string_view failpoint_name(Failpoint fp) noexcept;
std::string_view failpoint_window_id(Failpoint fp) noexcept;  // "CW-07"
bool parse_failpoint(std::string_view text, Failpoint& out) noexcept;

enum class FailAction : std::uint8_t {
  // The store reports a definitive commit failure; nothing is durable.
  CommitFail,
  // The store commits durably but the acknowledgement is lost. This is the
  // dangerous case: the caller must not treat it as a rollback (RI-24).
  CommitAckLost,
  // The store refuses to commit because the writer lost ownership.
  FenceReject,
  // Terminate the process immediately, leaving the journal exactly as it is on
  // disk. Used by the out-of-process crash harness.
  Abort,
  // Run an arbitrary hook (used to interleave a concurrent writer).
  Hook,
};

struct ArmedFailpoint {
  FailAction action = FailAction::CommitFail;
  // Number of times this failpoint still fires; 0 means disarmed.
  int remaining = 0;
  // Skip this many reaches before firing, so a test can target the Nth pass.
  int skip = 0;
  std::function<void()> hook;
};

// Process-wide registry. The engine holds one reference; tests arm and disarm
// through the same object so there is no hidden global state in the library.
class FailpointRegistry {
 public:
  void arm(Failpoint fp, FailAction action, int times = 1, int skip = 0);
  void arm_hook(Failpoint fp, std::function<void()> hook, int times = 1,
                int skip = 0);
  void disarm(Failpoint fp);
  void disarm_all();

  // Called from the engine. Returns the action to take, or std::nullopt when
  // the failpoint is not armed. Decrements the remaining count.
  struct Hit {
    FailAction action;
    std::function<void()> hook;
  };
  bool reach(Failpoint fp, Hit& hit);

  // Every failpoint the process has passed through, in order. The crash tests
  // assert on this trace to prove the window they targeted was really entered.
  std::vector<Failpoint> trace() const;
  void clear_trace();

  // Total reaches per failpoint, for coverage reporting.
  std::map<Failpoint, int> reach_counts() const;

 private:
  mutable std::mutex mu_;
  std::map<Failpoint, ArmedFailpoint> armed_;
  std::vector<Failpoint> trace_;
  std::map<Failpoint, int> counts_;
};

// Arms from a "<name>:<action>[:times[:skip]]" string, e.g.
// "CW-10:abort" or "AFTER_OUTBOX_INSERT:commit_fail:2:1".
// Returns false when the spec does not parse, so a typo in a crash harness
// cannot silently run the un-armed case and look like a pass.
bool arm_from_spec(FailpointRegistry& registry, std::string_view spec);

// Same, read from RISK_FAILPOINT. A missing variable is not an error.
void arm_from_environment(FailpointRegistry& registry);

}  // namespace risk
