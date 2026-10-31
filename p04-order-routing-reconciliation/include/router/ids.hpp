// Strongly typed identifiers and the fixed-point numeric model.
//
// Design doc 7.1 lists nine distinct ID namespaces that must never be mixed up
// (a MsgSeqNum is not an execution identity, an ExecID is not a receipt key).
// Plain strings would let any of them be passed where another is expected, so
// every namespace gets its own type and the compiler enforces 7.1 for us.
//
// Quantities and prices are integers only (design doc 9.2): a price is a count
// of symbol ticks, a quantity is a count of base units. No double ever touches
// an order.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace router {

// ---------------------------------------------------------------------------
// String-shaped identifiers.

template <typename Tag>
class StrongId {
public:
    StrongId() = default;
    // One converting constructor only. Offering both std::string and
    // std::string_view would make every StrongId("literal") ambiguous.
    explicit StrongId(std::string_view v) : value_(v) {}

    const std::string& str() const noexcept { return value_; }
    bool empty() const noexcept { return value_.empty(); }

    friend bool operator==(const StrongId& a, const StrongId& b) { return a.value_ == b.value_; }
    friend bool operator!=(const StrongId& a, const StrongId& b) { return a.value_ != b.value_; }
    friend bool operator<(const StrongId& a, const StrongId& b) { return a.value_ < b.value_; }

private:
    std::string value_;
};

using TenantId = StrongId<struct TenantTag>;
using ActorId = StrongId<struct ActorTag>;
using AccountId = StrongId<struct AccountTag>;
using LpId = StrongId<struct LpTag>;
using CommandId = StrongId<struct CommandTag>;        // (tenant, client_command_id) canonical form
using OrderChainId = StrongId<struct OrderChainTag>;  // immutable across replace
using ClientOrderId = StrongId<struct ClOrdIdTag>;    // FIX 11, fresh per D/F/G/H request
using OrderId = StrongId<struct OrderIdTag>;          // FIX 37, assigned by the LP
using ExecutionId = StrongId<struct ExecIdTag>;       // FIX 17, LP execution identity
using ReconCaseId = StrongId<struct ReconCaseTag>;
using QueryId = StrongId<struct QueryTag>;            // FIX 790 OrdStatusReqID
using Symbol = StrongId<struct SymbolTag>;

// Numeric identifiers kept distinct from the business IDs above.
using MsgSeqNum = std::uint64_t;        // FIX 34, owned by P01, session ordering only
using DurableReceiptId = std::uint64_t; // P01 inbox key
using SessionEpoch = std::uint64_t;
using OutboxId = std::uint64_t;
using StateVersion = std::uint64_t;

// ---------------------------------------------------------------------------
// Fixed-point money/size.

// A count of base currency units (EUR/USD qty 100000 == 100000 units).
struct QuantityTicks {
    std::int64_t v = 0;
    friend bool operator==(QuantityTicks a, QuantityTicks b) { return a.v == b.v; }
    friend bool operator!=(QuantityTicks a, QuantityTicks b) { return a.v != b.v; }
    friend bool operator<(QuantityTicks a, QuantityTicks b) { return a.v < b.v; }
    friend bool operator<=(QuantityTicks a, QuantityTicks b) { return a.v <= b.v; }
    friend bool operator>(QuantityTicks a, QuantityTicks b) { return a.v > b.v; }
    friend bool operator>=(QuantityTicks a, QuantityTicks b) { return a.v >= b.v; }
};

// A count of symbol ticks (EUR/USD tick 0.00001 => 1.08500 is 108500).
struct PriceTicks {
    std::int64_t v = 0;
    friend bool operator==(PriceTicks a, PriceTicks b) { return a.v == b.v; }
    friend bool operator!=(PriceTicks a, PriceTicks b) { return a.v != b.v; }
    friend bool operator<(PriceTicks a, PriceTicks b) { return a.v < b.v; }
    friend bool operator>(PriceTicks a, PriceTicks b) { return a.v > b.v; }
};

// ---------------------------------------------------------------------------
// FIX application enumerations used by this project. Values mirror FIX 4.4; the
// mapping tables live in fix44.cpp so a wrong wire code cannot silently become
// a valid business meaning (design doc 11.2).

enum class Side { Buy, Sell };
enum class OrdType { Limit, Market, Stop, StopLimit };
enum class TimeInForce { Day, GoodTillCancel, ImmediateOrCancel, FillOrKill };

// ExecType(150). Only the codes in scope for v1 plus the ones that must be
// recognised in order to be quarantined rather than misread.
enum class ExecType {
    New,            // 0
    DoneForDay,     // 3
    Canceled,       // 4
    Replaced,       // 5
    PendingCancel,  // 6
    Stopped,        // 7
    Rejected,       // 8
    Suspended,      // 9
    PendingNew,     // A
    Calculated,     // B
    Expired,        // C
    Restated,       // D  -> quarantine in v1
    PendingReplace, // E
    Trade,          // F
    TradeCorrect,   // G  -> quarantine in v1
    TradeCancel,    // H  -> quarantine in v1
    OrderStatus,    // I  -> snapshot only, never a fill
};

// OrdStatus(39).
enum class OrdStatus {
    New,              // 0
    PartiallyFilled,  // 1
    Filled,           // 2
    DoneForDay,       // 3
    Canceled,         // 4
    PendingCancel,    // 6
    Stopped,          // 7
    Rejected,         // 8
    Suspended,        // 9
    PendingNew,       // A
    Calculated,       // B
    Expired,          // C
    AcceptedForBidding,  // D
    PendingReplace,   // E
};

// CxlRejResponseTo(434).
enum class CxlRejResponseTo { OrderCancelRequest, OrderCancelReplaceRequest };

std::string_view to_string(Side s);
std::string_view to_string(OrdType t);
std::string_view to_string(TimeInForce t);
std::string_view to_string(ExecType t);
std::string_view to_string(OrdStatus s);
std::string_view to_string(CxlRejResponseTo r);

}  // namespace router

// Hash support so the identifiers can key the ledgers in store.hpp.
namespace std {
template <typename Tag>
struct hash<router::StrongId<Tag>> {
    size_t operator()(const router::StrongId<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.str());
    }
};
}  // namespace std
