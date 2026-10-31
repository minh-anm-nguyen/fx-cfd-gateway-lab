// Time, split into the two kinds the design doc insists on keeping apart.
//
// R-I23: durations (timeouts, backoff, recon budgets) use a monotonic clock, and
// UTC wall time is only ever used for FIX TransactTime and SendingTime. OR-Q-13
// moves the wall clock minutes forward and OR-Q-14 jumps it an hour backwards;
// both must leave retry and timeout behaviour untouched, which is only possible
// if no code path derives a duration from the wall clock.
#pragma once

#include <cstdint>
#include <string>

namespace router {

// Monotonic microseconds since router start. Never moves backwards.
using MonoMicros = std::uint64_t;

// Microseconds since the UNIX epoch, UTC. May jump in either direction.
using WallMicros = std::int64_t;

class IClock {
public:
    virtual ~IClock() = default;
    virtual MonoMicros mono_now() const = 0;
    virtual WallMicros wall_now() const = 0;
};

// The only clock the tests use. Nothing sleeps: a scenario advances time
// explicitly, which is what makes the fault schedules in design doc 22.1
// reproducible instead of race-prone.
class VirtualClock final : public IClock {
public:
    explicit VirtualClock(WallMicros wall_start = kLabEpochUtc) : wall_(wall_start) {}

    MonoMicros mono_now() const override { return mono_; }
    WallMicros wall_now() const override { return wall_; }

    // Advances both clocks, which is what the passage of real time looks like.
    void advance(std::uint64_t micros) {
        mono_ += micros;
        wall_ += static_cast<WallMicros>(micros);
    }

    // Moves wall time only, modelling an NTP step or an operator mistake. The
    // monotonic clock deliberately does not follow (OR-Q-14).
    void skew_wall(WallMicros micros) { wall_ += micros; }

    // 2026-10-30T08:00:00.000Z, the fixture time used by the playbooks in 21.0.
    // 20756 days after the UNIX epoch, plus eight hours.
    static constexpr WallMicros kLabEpochUtc = (20756LL * 86400LL + 8LL * 3600LL) * 1000000LL;

private:
    MonoMicros mono_ = 0;
    WallMicros wall_ = 0;
};

// FIX UTCTimestamp with milliseconds: "20261030-08:00:00.000".
std::string format_fix_utc(WallMicros wall);

// Parses the same form back. Returns false for anything else, including the
// seconds-only variant, because the lab profile pins millisecond precision.
bool parse_fix_utc(const std::string& text, WallMicros& out);

}  // namespace router
