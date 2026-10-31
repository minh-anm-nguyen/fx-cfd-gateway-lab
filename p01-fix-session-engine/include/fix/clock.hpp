// Clock abstraction. Durations and timeouts are measured on a monotonic clock;
// wall-clock time is used only for SendingTime(52) and log timestamps
// (design doc section 10). Tests drive both independently through VirtualClock
// so that NTP steps (H-01, H-02, D-12) can be simulated without disturbing
// liveness detection.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace fixse {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;
using Millis = std::chrono::milliseconds;

class IClock {
public:
    virtual ~IClock() = default;
    // Monotonic: never goes backwards, unaffected by wall-clock adjustments.
    virtual SteadyTime steady_now() const = 0;
    // UTC wall clock, used for SendingTime(52) and audit timestamps only.
    virtual WallTime wall_now() const = 0;
};

class SystemClock final : public IClock {
public:
    SteadyTime steady_now() const override { return std::chrono::steady_clock::now(); }
    WallTime wall_now() const override { return std::chrono::system_clock::now(); }
};

// Deterministic clock for tests: steady and wall time advance independently.
class VirtualClock final : public IClock {
public:
    VirtualClock()
        : steady_(SteadyTime{} + std::chrono::hours(1)),
          // 2026-10-30T05:00:00Z, a fixed epoch so golden fixtures stay stable.
          wall_(WallTime{} + std::chrono::seconds(1793336400)) {}

    SteadyTime steady_now() const override { return steady_; }
    WallTime wall_now() const override { return wall_; }

    // Advance both clocks, the normal passage of time.
    void advance(Millis d) {
        steady_ += d;
        wall_ += d;
    }
    // Advance monotonic time only (CPU starvation / scheduler delay, D-11).
    void advance_steady(Millis d) { steady_ += d; }
    // Step the wall clock without touching monotonic time (NTP jump, H-01/H-02).
    void jump_wall(std::chrono::seconds d) { wall_ += d; }

private:
    SteadyTime steady_;
    WallTime wall_;
};

// Format as a FIX UTCTimestamp with millisecond precision: YYYYMMDD-HH:MM:SS.sss
std::string format_utc_timestamp(WallTime tp);

// Strict UTCTimestamp validation: calendar-correct including leap years, with
// 0, 3, 6 or 9 fractional digits (B-23, H-04, H-05). Returns false and leaves
// *out untouched on any deviation; never silently coerces.
bool parse_utc_timestamp(std::string_view s, WallTime* out);

}  // namespace fixse
