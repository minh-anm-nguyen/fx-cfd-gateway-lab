// P05 -- the pre-trade risk interface and the lab fake behind it.
//
// Design doc 6.4 is explicit that this fake "khong duoc trinh bay la risk
// engine thuc". It exists to exercise the admission policy:
//
//   * New and Replace need an APPROVE before anything is queued (R-I14);
//   * UNAVAILABLE fails closed for New and Replace, it never becomes an
//     implicit approval (OR-A-16);
//   * a Cancel reduces exposure, so it has its own path and may proceed while
//     risk is down, without bypassing store or session safety (OR-A-19);
//   * a verdict has an expiry, and a stale approval must be rechecked rather
//     than reused (OR-A-18);
//   * reservations are idempotent by command id, so a retried RPC cannot
//     reserve twice (OR-A-17) and a replayed release cannot double-release
//     (OR-K-15).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "router/clock.hpp"
#include "router/ids.hpp"

namespace router::p05 {

enum class Verdict { Approve, Reject, Unavailable };
std::string_view to_string(Verdict v);

struct RiskRequest {
    CommandId command_id;
    TenantId tenant;
    AccountId account;
    std::string wire_symbol;
    Side side = Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price_ticks = 0;
    bool increases_exposure = true;  // false for a cancel

    // The order lineage this command belongs to, immutable across replace.
    //
    // A real risk engine keys a reservation by the chain, not by the command:
    // that is what lets a replace resize the hold the New took instead of
    // taking a second one on top of it. Without this field the two look like
    // unrelated orders and the account is charged twice for one position
    // (EI-23). The lab fake ignores it, which is why it was easy to leave out.
    OrderChainId order_chain_id;

    // How to read `price_ticks`: the number of fraction digits the symbol is
    // quoted to. Passing the number beside the price is the difference between
    // a reader that knows the scale and one that assumes it, and EI-15 requires
    // a scale mismatch to be rejected rather than guessed through.
    int price_scale = 5;

    // The absolute quantity the chain will hold if this command is accepted.
    //
    // `qty` is a delta on the replace path and an absolute on the New path.
    // A risk engine that tracks exposure per chain needs the absolute, because
    // adding a delta to its own idea of the total is how two views drift
    // apart. Both are carried so neither caller has to reconstruct the other.
    std::int64_t total_units_after = 0;
};

struct RiskDecision {
    Verdict verdict = Verdict::Unavailable;
    std::string reservation_id;
    WallMicros valid_until = 0;
    std::string reason;
};

class IRiskAdmission {
public:
    virtual ~IRiskAdmission() = default;
    virtual RiskDecision reserve(const RiskRequest& req) = 0;
    // Idempotent by reservation id: calling it twice has the effect of once.
    virtual void release(const std::string& reservation_id) = 0;
    virtual bool held(const std::string& reservation_id) const = 0;

    // A cancel was admitted durably.
    //
    // A cancel reserves nothing and releases nothing (OR-A-19), so there is no
    // verdict to ask for and this is a notification rather than a request. Risk
    // still has to hear about it: EI-24 requires a pending cancel to keep
    // conservative headroom for the fills that can still arrive before the LP
    // acts on it, and no engine can hold headroom for a request it never saw.
    //
    // Defaulted to nothing so the lab fake, which has no exposure to hold, is
    // unaffected.
    virtual void note_cancel_requested(const TenantId& tenant, const AccountId& account,
                                       const OrderChainId& chain,
                                       const CommandId& command_id) {
        (void)tenant;
        (void)account;
        (void)chain;
        (void)command_id;
    }
};

class FakeRisk final : public IRiskAdmission {
public:
    explicit FakeRisk(const IClock& clock, std::uint64_t ttl_micros = 2000000)
        : clock_(clock), ttl_(ttl_micros) {}

    RiskDecision reserve(const RiskRequest& req) override;
    void release(const std::string& reservation_id) override;
    bool held(const std::string& reservation_id) const override;

    // Scripting.
    void set_verdict(Verdict v) { verdict_ = v; }
    void revoke(const std::string& reservation_id);

    // Counters the tests assert on.
    std::size_t reserve_calls() const { return reserve_calls_; }
    std::size_t distinct_reservations() const { return reservations_.size(); }
    std::size_t release_calls() const { return release_calls_; }
    std::size_t effective_releases() const { return effective_releases_; }

private:
    const IClock& clock_;
    std::uint64_t ttl_;
    Verdict verdict_ = Verdict::Approve;
    std::map<std::string, std::string> by_command_;  // command_id -> reservation_id
    std::map<std::string, bool> reservations_;       // reservation_id -> held
    std::size_t reserve_calls_ = 0;
    std::size_t release_calls_ = 0;
    std::size_t effective_releases_ = 0;
    std::uint64_t next_id_ = 1;
};

}  // namespace router::p05
