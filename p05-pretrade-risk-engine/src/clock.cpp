#include "risk/clock.hpp"

#include <chrono>

namespace risk {

MonoNanos SystemClock::mono_now() const {
  const auto t = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

UtcNanos SystemClock::utc_now() const {
  const auto t = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

VirtualClock::VirtualClock(MonoNanos mono, UtcNanos utc)
    : mono_(mono), utc_(utc) {}

MonoNanos VirtualClock::mono_now() const {
  return mono_.load(std::memory_order_acquire);
}

UtcNanos VirtualClock::utc_now() const {
  return utc_.load(std::memory_order_acquire);
}

void VirtualClock::advance_nanos(std::int64_t delta) {
  mono_.fetch_add(delta, std::memory_order_acq_rel);
  utc_.fetch_add(delta, std::memory_order_acq_rel);
}

void VirtualClock::advance_millis(std::int64_t delta_ms) {
  advance_nanos(millis_to_nanos(delta_ms));
}

void VirtualClock::set_utc(UtcNanos v) {
  utc_.store(v, std::memory_order_release);
}

}  // namespace risk
