// Time source.
//
// Design doc G23 puts clock jumps and skew in the failure taxonomy, and
// section 8 note 8 is explicit that created_at is audit metadata and must
// never decide replay order. The engine therefore never reads the system
// clock directly: it reads an IClock, and the tests drive a manual one
// backwards to prove that ordering does not move with it (G01-18).
#pragma once

#include <cstdint>
#include <string>

namespace jrnl {

class IClock {
public:
    virtual ~IClock() = default;

    // Wall clock, milliseconds since the UNIX epoch. Used for audit rows and
    // lease expiry only.
    virtual std::int64_t now_ms() const = 0;

    // Monotonic milliseconds. Used for timeouts and lease arithmetic, because
    // a wall clock that steps backwards must not extend a lease.
    virtual std::int64_t steady_ms() const = 0;

    // ISO-8601 UTC rendering of now_ms(), for audit columns.
    std::string now_iso8601() const;
};

class SystemClock final : public IClock {
public:
    std::int64_t now_ms() const override;
    std::int64_t steady_ms() const override;
};

// Test clock. Both components move independently so a test can step the wall
// clock backwards while the monotonic one keeps going, which is the situation
// G23 asks about.
class ManualClock final : public IClock {
public:
    explicit ManualClock(std::int64_t start_ms = 1767225600000)  // 2026-01-01T00:00:00Z
        : wall_(start_ms), steady_(0) {}

    std::int64_t now_ms() const override { return wall_; }
    std::int64_t steady_ms() const override { return steady_; }

    void advance(std::int64_t ms) {
        wall_ += ms;
        steady_ += ms;
    }
    void set_wall(std::int64_t ms) { wall_ = ms; }
    void advance_steady(std::int64_t ms) { steady_ += ms; }

private:
    std::int64_t wall_;
    std::int64_t steady_;
};

}  // namespace jrnl
