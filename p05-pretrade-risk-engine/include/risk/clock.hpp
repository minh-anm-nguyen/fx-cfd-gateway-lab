// Clock abstraction with a separate monotonic and wall-clock reading.
//
// §9.1 is explicit that freshness must not be judged from wall-clock time
// alone: a NTP step or a deliberate clock rollback would make an expired quote
// look fresh. Every TTL in the engine is therefore measured on the monotonic
// axis, while the wall clock is recorded only for audit. A rollback of the wall
// clock is itself reported (RI-12 / Reason::ClockRollback) rather than ignored.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace risk {

// Nanoseconds since an unspecified monotonic origin.
using MonoNanos = std::int64_t;
// Nanoseconds since the Unix epoch, UTC.
using UtcNanos = std::int64_t;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual MonoNanos mono_now() const = 0;
  virtual UtcNanos utc_now() const = 0;
};

// Real clock. Monotonic readings come from steady_clock, wall from system_clock.
class SystemClock final : public Clock {
 public:
  MonoNanos mono_now() const override;
  UtcNanos utc_now() const override;
};

// Deterministic clock for tests and for the scenario DSL. Both axes advance
// only when the test says so, which is what makes TTL-boundary cases (PB-04)
// reproducible instead of flaky.
class VirtualClock final : public Clock {
 public:
  explicit VirtualClock(MonoNanos mono = 1'000'000'000,
                        UtcNanos utc = 1'700'000'000'000'000'000);

  MonoNanos mono_now() const override;
  UtcNanos utc_now() const override;

  void advance_nanos(std::int64_t delta);
  void advance_millis(std::int64_t delta_ms);

  // Moves the wall clock only, leaving the monotonic axis alone. Used to prove
  // that freshness decisions do not depend on the wall clock (PB-28).
  void set_utc(UtcNanos v);

 private:
  mutable std::atomic<MonoNanos> mono_;
  mutable std::atomic<UtcNanos> utc_;
};

constexpr std::int64_t kNanosPerMilli = 1'000'000;

constexpr std::int64_t millis_to_nanos(std::int64_t ms) {
  return ms * kNanosPerMilli;
}

}  // namespace risk
