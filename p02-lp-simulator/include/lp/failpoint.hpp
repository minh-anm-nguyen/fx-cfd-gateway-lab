// Fault injection points.
//
// Reference: design doc 10.3, which enumerates ten crash windows W1..W10, and
// 11.2, which requires every trigger to be bounded and audited.
//
// Scenario M-15 is the reason these exist as named points rather than as ad-hoc
// sleeps in the test: a crash test whose cutpoint cannot be proven is scored
// INVALID, not PASS. Each firing records a marker so the harness can assert the
// process really died where the playbook said it would.
//
// Abrupt termination uses an immediate exit with no unwinding and no stdio
// flush, the closest approximation to SIGKILL available on both POSIX and
// Windows. It is NOT a power-loss test: durability beyond the fsync contract
// needs real hardware and is out of lab scope (design doc 10.5).
//
// Failpoints compile in only when LPSIM_ENABLE_FAILPOINTS is defined, and are
// armed either programmatically or from the LPSIM_FAILPOINT environment
// variable so that a child process can be killed at an exact boundary.
#pragma once

#include <string>
#include <string_view>

namespace lpsim::fp {

enum class Action {
    None,
    ReturnError,  // the instrumented call returns a typed failure
    AbruptExit,   // terminate immediately: no unwinding, no flush
    Delay,        // sleep for param milliseconds
};

// Arm a failpoint. occurrence is 1-based: 1 fires on the first hit. A bounded
// occurrence count is what keeps H-17 from becoming an infinite kill loop.
void arm(std::string name, Action action, int param = 0, int occurrence = 1);
void disarm_all();

// Parse LPSIM_FAILPOINT, of the form
//   name=error | name=exit | name=delay:250 | name=exit@3
// A malformed specification aborts startup rather than running an
// uninstrumented binary that the harness would then score as a pass.
void arm_from_env();

// Returns the action that fired, performing AbruptExit and Delay in place.
// The caller must honour ReturnError by failing the operation.
Action fire(std::string_view name);

// Where the marker file is written just before an AbruptExit, so the harness
// can prove the cutpoint (M-15). Empty disables marker writing.
void set_marker_path(std::string path);

// Name of the last failpoint that fired in this process, for in-process tests.
std::string_view last_fired();

// The ten crash windows of design doc 10.3, plus the socket boundary. Names are
// defined once here so that a playbook and the code cannot drift apart.
namespace point {
// W1: before the inbound FIX session commit. No business effect may exist.
constexpr std::string_view kTxABeforeCommit = "tx_a.before_commit";
// W2: inbound committed, business command not yet dispatched.
constexpr std::string_view kTxAAfterCommit = "tx_a.after_commit";
// W3: inside the business transaction, before COMMIT. Everything rolls back.
constexpr std::string_view kTxBBeforeCommit = "tx_b.before_commit";
// W10: between deducting quote liquidity and writing the fill row. Both live in
// TX-B, so this must be indistinguishable from W3 to an outside observer.
constexpr std::string_view kTxBAfterQuoteConsume = "tx_b.after_quote_consume";
// W4: business state and outbox committed, report not yet dispatched.
constexpr std::string_view kTxBAfterCommit = "tx_b.after_commit";
// W5: before send_once assigns an outbound sequence number.
constexpr std::string_view kTxCBeforeCommit = "tx_c.before_commit";
// W6: sequence assigned and frame journaled, nothing written to the socket.
constexpr std::string_view kTxCAfterCommit = "tx_c.after_commit";
// W7: mid-write, with an unknown number of bytes delivered.
constexpr std::string_view kSocketAfterPartialWrite = "socket.after_partial_write";
// W8: the peer has the report, the local dispatch bookkeeping is unfinished.
constexpr std::string_view kTxDBeforeCommit = "tx_d.before_commit";
// W9: while marking a scheduled event as executed.
constexpr std::string_view kSchedulerMarkExecuted = "scheduler.mark_executed";

// Additional store boundaries used by the ENOSPC and corruption playbooks.
constexpr std::string_view kStoreBeginTxn = "store.begin_txn";
constexpr std::string_view kStoreCommit = "store.commit";
constexpr std::string_view kSocketBeforeWrite = "socket.before_write";
}  // namespace point

}  // namespace lpsim::fp

#ifdef LPSIM_ENABLE_FAILPOINTS
#define LPSIM_FP(name) (::lpsim::fp::fire(name))
#else
#define LPSIM_FP(name) (::lpsim::fp::Action::None)
#endif
