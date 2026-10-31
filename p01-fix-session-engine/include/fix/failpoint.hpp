// Fault injection points.
//
// Design doc 17.3 layer 5 requires failpoints immediately before and after each
// durability boundary: BEGIN, INSERT outbound, UPDATE next_out, COMMIT,
// socket write, INSERT inbox, UPDATE next_in, sink delivery and delivery ack.
// A failpoint can return an error, terminate the process abruptly, or delay.
//
// Abrupt termination uses an immediate exit with no unwinding and no stdio
// flush, which is the closest approximation to SIGKILL available on both
// POSIX and Windows. It is NOT a power-loss test: durability beyond the fsync
// contract (G-09) needs a real machine and is out of lab scope.
//
// Failpoints are compiled in only when FIXSE_ENABLE_FAILPOINTS is defined, and
// are armed either programmatically from a test or from the FIXSE_FAILPOINT
// environment variable so that a child process can be crashed at an exact
// boundary.
#pragma once

#include <string>
#include <string_view>

namespace fixse::fp {

enum class Action {
    None,
    ReturnError,  // the instrumented call returns a typed failure
    AbruptExit,   // terminate immediately, no unwinding, no flush
    Delay,        // sleep for param milliseconds
};

// Arm a failpoint. occurrence is 1-based: 1 fires on the first hit.
void arm(std::string name, Action action, int param = 0, int occurrence = 1);
void disarm_all();

// Parse FIXSE_FAILPOINT, which has the form
//   name=error | name=exit | name=delay:250 | name=exit@3
// A malformed specification aborts startup rather than running an uninstrumented
// binary that a test would then score as a pass.
void arm_from_env();

// Arm from a specification string, the same grammar as FIXSE_FAILPOINT. Used
// when a crash test passes the failpoint to a child process as an argument.
void arm_from_spec(const std::string& spec);

// Returns the action that fired, performing AbruptExit and Delay in place.
// The caller must honour ReturnError by failing the operation.
Action fire(std::string_view name);

// Names used by the engine, kept in one place so tests and sources cannot
// drift apart.
namespace point {
constexpr std::string_view kStoreOutboundBeforeBegin = "store.outbound.before_begin";
constexpr std::string_view kStoreOutboundAfterInsert = "store.outbound.after_insert";
constexpr std::string_view kStoreOutboundAfterUpdate = "store.outbound.after_update";
constexpr std::string_view kStoreOutboundBeforeCommit = "store.outbound.before_commit";
constexpr std::string_view kStoreOutboundAfterCommit = "store.outbound.after_commit";
constexpr std::string_view kStoreInboundBeforeBegin = "store.inbound.before_begin";
constexpr std::string_view kStoreInboundAfterInsert = "store.inbound.after_insert";
constexpr std::string_view kStoreInboundBeforeCommit = "store.inbound.before_commit";
constexpr std::string_view kStoreInboundAfterCommit = "store.inbound.after_commit";
constexpr std::string_view kStoreMarkDelivered = "store.mark_delivered";
constexpr std::string_view kSocketBeforeWrite = "socket.before_write";
constexpr std::string_view kSocketAfterPartialWrite = "socket.after_partial_write";
constexpr std::string_view kSocketAfterWrite = "socket.after_write";
constexpr std::string_view kSinkBeforeDeliver = "sink.before_deliver";
constexpr std::string_view kSinkAfterDeliver = "sink.after_deliver";
}  // namespace point

}  // namespace fixse::fp

#ifdef FIXSE_ENABLE_FAILPOINTS
#define FIXSE_FP(name) (::fixse::fp::fire(name))
#else
#define FIXSE_FP(name) (::fixse::fp::Action::None)
#endif
