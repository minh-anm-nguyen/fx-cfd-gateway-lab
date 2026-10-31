// Domain state and the pure reducers that produce it.
//
// Section 5.1 of the design doc puts order transition logic in P03 and
// exposure logic in P05, with P06 owning only durability. This header is the
// P06 side of the OrderReducerV1 and RiskMutationV1 contracts from section 6:
// a pure function from (state, event) to state, with no I/O, no clock and no
// randomness, so that replaying a stream reproduces the projection exactly
// (JI-11).
//
// The reducers live behind a version registry because JI-37 forbids replaying
// historical events with today's algorithm. An event records the reducer
// version that produced it and replay looks that version up; a reducer that
// has been retired must still be available or the stream cannot be verified.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jrnl/canon.hpp"
#include "jrnl/hash.hpp"
#include "jrnl/ids.hpp"
#include "jrnl/result.hpp"

namespace jrnl {

// ---------------------------------------------------------------------------
// Order aggregate
// ---------------------------------------------------------------------------

enum class OrdStatus {
    None = 0,
    PendingNew,
    New,
    PartiallyFilled,
    Filled,
    PendingCancel,
    Canceled,
    PendingReplace,
    Replaced,
    Rejected,
    Expired,
};

std::string_view to_string(OrdStatus s);
bool parse_ord_status(std::string_view s, OrdStatus* out);

// A status from which no further execution can arrive. JI-10 is the reason
// PendingCancel and PendingReplace are deliberately absent from this list.
bool is_terminal(OrdStatus s);

struct OrderState {
    bool exists = false;
    OrderChainId chain;
    AccountId account;
    ClOrdId cl_ord_id;
    ClOrdId orig_cl_ord_id;
    std::string symbol;
    std::string side;  // "BUY" or "SELL"

    std::int64_t order_qty = 0;
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t limit_px = 0;      // minor units
    std::int64_t exec_value = 0;    // sum(last_qty * last_px), for average price
    std::int64_t fill_count = 0;
    // Tracked only by p03.v2. A field that one reducer version records and
    // another does not is exactly the situation JI-37 is about, and keeping a
    // real one here makes the version-pinning tests mean something.
    std::int64_t last_px = 0;

    OrdStatus status = OrdStatus::None;
    bool pending_cancel = false;
    bool pending_replace = false;
    ClOrdId pending_cl_ord_id;
    std::int64_t pending_qty = 0;
    std::int64_t pending_px = 0;

    CommandId origin_command;
    RiskDomainId risk_domain;

    bool operator==(const OrderState& o) const;

    // What a FIX counterparty would call the order right now. The pending
    // flags are kept separately from status so that a rejected cancel can
    // restore the previous status without having had to remember it.
    OrdStatus effective_status() const {
        if (pending_replace) return OrdStatus::PendingReplace;
        if (pending_cancel) return OrdStatus::PendingCancel;
        return status;
    }
};

// ---------------------------------------------------------------------------
// Risk aggregate
// ---------------------------------------------------------------------------

enum class ReservationState { Held = 0, Released, Consumed };

std::string_view to_string(ReservationState s);
bool parse_reservation_state(std::string_view s, ReservationState* out);

struct Reservation {
    std::int64_t amount = 0;
    ReservationState state = ReservationState::Held;
    AccountId account;
    OrderChainId chain;

    bool operator==(const Reservation& o) const {
        return amount == o.amount && state == o.state && account == o.account &&
               chain == o.chain;
    }
};

struct RiskDomainState {
    bool exists = false;
    RiskDomainId domain;
    std::int64_t limit = 0;     // firm or account ceiling for this domain
    std::int64_t reserved = 0;  // sum of Held reservations
    std::int64_t exposure = 0;  // sum of Consumed amounts
    std::uint64_t revision = 0;
    std::map<ReservationId, Reservation> reservations;

    // What the limit is measured against. JI-09 forbids letting the sum of
    // held and consumed exceed the limit at any committed point.
    std::int64_t utilised() const { return reserved + exposure; }
    std::int64_t headroom() const { return limit - utilised(); }

    bool operator==(const RiskDomainState& o) const;
};

// ---------------------------------------------------------------------------
// Event names
// ---------------------------------------------------------------------------

namespace ev {
// Order stream
constexpr std::string_view kOrderAccepted = "OrderAccepted";
constexpr std::string_view kOrderRejected = "OrderRejected";
constexpr std::string_view kFillApplied = "FillApplied";
constexpr std::string_view kCancelRequested = "CancelRequested";
constexpr std::string_view kCancelAccepted = "CancelAccepted";
constexpr std::string_view kCancelRejected = "CancelRejected";
constexpr std::string_view kReplaceRequested = "ReplaceRequested";
constexpr std::string_view kReplaceAccepted = "ReplaceAccepted";
constexpr std::string_view kReplaceRejected = "ReplaceRejected";
constexpr std::string_view kOrderExpired = "OrderExpired";
constexpr std::string_view kOrderCorrected = "OrderCorrected";

// Risk stream
constexpr std::string_view kRiskLimitSet = "RiskLimitSet";
constexpr std::string_view kRiskReserved = "RiskReserved";
constexpr std::string_view kRiskReleased = "RiskReleased";
constexpr std::string_view kRiskConsumed = "RiskConsumed";
}  // namespace ev

// ---------------------------------------------------------------------------
// Reducers
// ---------------------------------------------------------------------------

struct ReduceError {
    Incident incident = Incident::None;
    std::string detail;
};

// Reducer version strings. The number is part of the hash preimage, so
// changing a reducer's behaviour without changing its version would make a
// replay of old events disagree with history and is caught by the verifier.
namespace reducer_version {
constexpr std::string_view kOrderV1 = "p03.v1";
constexpr std::string_view kOrderV2 = "p03.v2";  // tracks last traded price
constexpr std::string_view kRiskV1 = "p05.v1";
}  // namespace reducer_version

class IOrderReducer {
public:
    virtual ~IOrderReducer() = default;
    virtual std::string_view version() const = 0;
    // Returns false and fills err when the event contradicts the state. A
    // contradictory execution report is quarantined, never coerced (JI-08).
    virtual bool apply(OrderState* state, std::string_view event_type,
                       const CanonValue& payload, ReduceError* err) const = 0;
    // Canonical rendering of the state, which is what the state hash covers.
    virtual CanonValue to_canon(const OrderState& state) const = 0;
    virtual bool from_canon(const CanonValue& v, OrderState* out) const = 0;
};

class IRiskReducer {
public:
    virtual ~IRiskReducer() = default;
    virtual std::string_view version() const = 0;
    virtual bool apply(RiskDomainState* state, std::string_view event_type,
                       const CanonValue& payload, ReduceError* err) const = 0;
    virtual CanonValue to_canon(const RiskDomainState& state) const = 0;
    virtual bool from_canon(const CanonValue& v, RiskDomainState* out) const = 0;
};

// Version registry. Replay asks for the exact version recorded on the event;
// an unknown version is an incident, not a reason to fall back to the newest
// implementation (JI-37, G18).
class ReducerRegistry {
public:
    ReducerRegistry();

    const IOrderReducer* order(std::string_view version) const;
    const IRiskReducer* risk(std::string_view version) const;

    // Version the engine stamps on newly produced events.
    std::string_view current_order_version() const { return current_order_; }
    std::string_view current_risk_version() const { return current_risk_; }
    void set_current_order_version(std::string_view v) { current_order_ = std::string(v); }

    static const ReducerRegistry& instance();

private:
    std::map<std::string, std::shared_ptr<IOrderReducer>, std::less<>> orders_;
    std::map<std::string, std::shared_ptr<IRiskReducer>, std::less<>> risks_;
    std::string current_order_{reducer_version::kOrderV1};
    std::string current_risk_{reducer_version::kRiskV1};
};

// State hash over the canonical rendering. Used for the chain and for
// projection-versus-replay comparison.
Digest state_digest(const CanonValue& canon_state);

}  // namespace jrnl
