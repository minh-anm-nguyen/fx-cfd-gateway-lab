// Deterministic fault injection for the sixteen crash windows.
//
// Design doc 13.2 numbers the windows W00..W15 and 20.6 insists a crash test
// may only pass if the faultpoint actually fired. So a FailPoint records that
// it fired, and the test harness asserts it did: a window that silently failed
// to trigger reports BLOCKED, never PASS.
#pragma once

#include <exception>
#include <string>

namespace order_core {

enum class CrashWindow {
    None,
    W00_BeforeParse,
    W01_AfterParseBeforeCorrelation,
    W02_AfterCorrelationBeforeDedup,
    W03_AfterDedupBeforeReduce,
    W04_AfterReduceBeforeTxn,
    W05_InTxnBeforeJournalInsert,
    W06_AfterDedupInsertBeforeState,
    W07_AfterStateBeforeOutbox,
    W08_AfterOutboxBeforeCommit,
    W09_AfterCommitBeforeInboxAck,
    W10_AfterCommitBeforeDispatch,
    W11_AfterDispatchBeforeMarkSent,
    W12_AmbiguousFlush,
    W13_DuringSnapshotCompaction,
    W14_DuringSchemaMigration,
    W15_DuringRecoveryReplay,
};

const char* to_string(CrashWindow w);

// Thrown where a process would have died. The in-memory engine state is
// abandoned by unwinding; whatever the store already committed survives,
// which is exactly the distinction the crash windows are about.
struct SimulatedCrash : std::exception {
    CrashWindow window = CrashWindow::None;
    std::string where;
    SimulatedCrash(CrashWindow w, std::string where_) : window(w), where(std::move(where_)) {}
    const char* what() const noexcept override { return "simulated crash"; }
};

// W12 is not a crash but an ambiguous answer: the flush neither clearly
// succeeded nor clearly failed. Publishing either way would be a guess.
struct AmbiguousCommit : std::exception {
    const char* what() const noexcept override { return "ambiguous commit outcome"; }
};

class FailPoint {
  public:
    void arm(CrashWindow w) {
        armed_ = w;
        fired_ = false;
    }
    void disarm() {
        armed_ = CrashWindow::None;
        fired_ = false;
    }
    [[nodiscard]] bool armed_at(CrashWindow w) const { return armed_ == w && !fired_; }
    [[nodiscard]] bool fired() const { return fired_; }
    [[nodiscard]] CrashWindow window() const { return armed_; }

    // Fires at most once, then stays quiet so recovery can make progress.
    void check(CrashWindow w, const std::string& where) {
        if (!armed_at(w)) return;
        fired_ = true;
        if (w == CrashWindow::W12_AmbiguousFlush) throw AmbiguousCommit{};
        throw SimulatedCrash{w, where};
    }

  private:
    CrashWindow armed_ = CrashWindow::None;
    bool fired_ = false;
};

}  // namespace order_core
