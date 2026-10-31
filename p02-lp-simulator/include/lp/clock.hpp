// Clock abstractions.
//
// Design doc 8.2 and 11.3 require three distinct notions of time, never mixed:
//
//   * virtual time (microseconds) is the deterministic scheduler ordinate. It
//     is the first component of the canonical event key
//     (virtual_time_us, priority_class, insertion_seq).
//   * monotonic time measures elapsed intervals (heartbeats, liveness). It is
//     unaffected by a wall-clock step, which is what makes H-13 and H-14
//     testable.
//   * UTC wall-clock time populates SendingTime(52) and TransactTime(60) and
//     audit records only.
//
// A test that compares a virtual-clock latency histogram against a wall-clock
// one is invalid by construction (M-13), so the clock mode is part of the
// run manifest rather than an implicit property of the process.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace lpsim {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;
using Millis = std::chrono::milliseconds;
using Micros = std::chrono::microseconds;

// Deterministic scheduler ordinate. Monotonically non-decreasing within a run.
using VirtualTimeUs = std::int64_t;

enum class ClockMode { Virtual, Wall };

std::string_view to_string(ClockMode m);

class IClock {
public:
    virtual ~IClock() = default;
    // Monotonic: never goes backwards, unaffected by wall-clock adjustments.
    virtual SteadyTime steady_now() const = 0;
    // UTC wall clock, used for SendingTime(52), TransactTime(60) and audit.
    virtual WallTime wall_now() const = 0;
    // Scheduler ordinate. For SystemClock this is derived from the monotonic
    // clock so that one code path serves both modes.
    virtual VirtualTimeUs virtual_now_us() const = 0;
    virtual ClockMode mode() const = 0;
};

class SystemClock final : public IClock {
public:
    SteadyTime steady_now() const override { return std::chrono::steady_clock::now(); }
    WallTime wall_now() const override { return std::chrono::system_clock::now(); }
    VirtualTimeUs virtual_now_us() const override {
        return std::chrono::duration_cast<Micros>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    ClockMode mode() const override { return ClockMode::Wall; }
};

// Deterministic clock for tests and scripted scenarios: virtual, monotonic and
// wall time advance independently so that each failure mode can be reproduced
// in isolation.
class VirtualClock final : public IClock {
public:
    VirtualClock()
        : steady_(SteadyTime{} + std::chrono::hours(1)),
          // 2026-10-30T05:00:00Z, a fixed epoch so golden fixtures stay stable.
          wall_(WallTime{} + std::chrono::seconds(1793336400)),
          virtual_us_(0) {}

    SteadyTime steady_now() const override { return steady_; }
    WallTime wall_now() const override { return wall_; }
    VirtualTimeUs virtual_now_us() const override { return virtual_us_; }
    ClockMode mode() const override { return ClockMode::Virtual; }

    // The ordinary passage of time: all three clocks move together.
    void advance(Millis d) {
        steady_ += d;
        wall_ += d;
        virtual_us_ += std::chrono::duration_cast<Micros>(d).count();
    }

    // Advance the scheduler ordinate only. Used by the scheduler itself when it
    // steps to the due time of the next event.
    void set_virtual_us(VirtualTimeUs t) {
        if (t > virtual_us_) {
            const VirtualTimeUs delta = t - virtual_us_;
            virtual_us_ = t;
            steady_ += Micros(delta);
            wall_ += Micros(delta);
        }
    }

    // Advance monotonic time only: CPU starvation, scheduler delay (H-16).
    void advance_steady(Millis d) { steady_ += d; }

    // Step the wall clock without touching monotonic time: an NTP jump
    // (H-13 backwards, H-14 forwards). Business-day rollover is a separate
    // explicit event, never a side effect of this call (design doc 12.2).
    void jump_wall(std::chrono::seconds d) { wall_ += d; }

private:
    SteadyTime steady_;
    WallTime wall_;
    VirtualTimeUs virtual_us_;
};

// Format as a FIX UTCTimestamp with millisecond precision:
// YYYYMMDD-HH:MM:SS.sss
std::string format_utc_timestamp(WallTime tp);

// Strict UTCTimestamp validation: calendar-correct including leap years, with
// 0, 3, 6 or 9 fractional digits. Returns false and leaves *out untouched on
// any deviation; it never silently coerces a bad value into a plausible one,
// because that would turn C-17 and C-18 into false passes.
bool parse_utc_timestamp(std::string_view s, WallTime* out);

// Business day in YYYYMMDD form, derived from a UTC wall-clock instant. The
// lab profile pins the business-day timezone to UTC (design doc 14.1); a real
// FX trading calendar is explicitly out of scope.
std::string business_day_utc(WallTime tp);

}  // namespace lpsim
