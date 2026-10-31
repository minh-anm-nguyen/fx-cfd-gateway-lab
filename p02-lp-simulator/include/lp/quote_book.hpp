// Synthetic quote book with bounded liquidity inventory.
//
// Reference: design doc 8.1 (deterministic LP fill model). This is NOT an
// exchange order book: it models one counterparty quoting a single price level
// per side with a finite, explicitly restocked inventory, which is what makes
// partial fills intentional rather than emergent.
//
// Two rules decided here rather than left implicit, because the what-if matrix
// asserts them directly:
//
//   * Expiry is EXCLUSIVE. A quote is usable while now_ms < expires_at_ms, so
//     at now_ms == expires_at_ms it is already stale (D-07).
//   * Versions are strictly monotonic per symbol. Re-sending a version with
//     identical content is an idempotent no-op; re-sending it with different
//     content is a conflict, never a rewind (D-13, D-14).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "lp/domain.hpp"
#include "lp/fixed_point.hpp"

namespace lpsim {

enum class QuoteError {
    Ok = 0,
    UnknownSymbol,      // not in the configured symbol set
    CrossedBook,        // bid >= ask                                    D-08
    NotPositivePrice,   // a zero or negative side                       D-09
    NegativeQuantity,   // negative available quantity                   D-09
    QuantityOverflow,   // available quantity beyond the domain          L-10
    DuplicateVersion,   // same version, different content               D-13
    NonMonotonic,       // an older version after a newer one            D-14
    VersionMismatch,    // consume() against a version that is not current  D-12
    InsufficientLiquidity,
};

std::string_view to_string(QuoteError e);

struct QuoteSlice {
    std::string symbol;
    std::uint64_t version = 0;
    Ticks bid_ticks = 0;
    Ticks ask_ticks = 0;
    Units bid_available = 0;
    Units ask_available = 0;
    std::int64_t valid_from_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::string source_fixture_id;

    // Identity of the liquidity slice a fill consumes, recorded in the fill
    // ledger so that "each slice is consumed at most once" is checkable after
    // the fact (invariant I-12).
    std::string slice_id(Side taker_side) const;

    // Usable while now_ms is strictly before expires_at_ms.
    bool usable_at(std::int64_t now_ms) const {
        return now_ms >= valid_from_ms && now_ms < expires_at_ms;
    }

    Ticks price_for(Side taker_side) const {
        // A buyer takes the ask, a seller hits the bid.
        return taker_side == Side::Buy ? ask_ticks : bid_ticks;
    }

    Units available_for(Side taker_side) const {
        return taker_side == Side::Buy ? ask_available : bid_available;
    }
};

class QuoteBook {
public:
    // Install or replace the current quote for a symbol. The symbol must be
    // known; an update is validated in full before any state changes, so a
    // rejected fixture cannot leave a half-applied quote (D-09, L-10).
    QuoteError apply_update(const QuoteSlice& q);

    // Current quote, or nullptr when the symbol has never been quoted.
    const QuoteSlice* current(std::string_view symbol) const;

    // Deduct liquidity. The caller passes the version it pinned when the event
    // was scheduled: a quote update that landed in between makes this fail
    // rather than silently fill at a different price (D-12).
    QuoteError consume(std::string_view symbol, std::uint64_t version, Side taker_side,
                       Units qty);

    // Total units still available, used by metrics and the invariant checker.
    Units inventory_units(std::string_view symbol, Side taker_side) const;

    // Symbols this book will accept updates for.
    void register_symbol(std::string_view symbol) { known_.emplace(std::string(symbol), true); }
    bool knows(std::string_view symbol) const;

    // Snapshot and restore, used by the durable store on recovery. Liquidity is
    // never silently reset by a reconnect: only an explicit committed update
    // creates capacity (D-17, D-18).
    std::vector<QuoteSlice> snapshot() const;
    void restore(const std::vector<QuoteSlice>& slices);

    // Every version ever applied, by symbol, so that a repeat of an old
    // version can be told apart from a conflicting one.
    struct VersionRecord {
        std::uint64_t version = 0;
        Ticks bid_ticks = 0;
        Ticks ask_ticks = 0;
        Units bid_available = 0;
        Units ask_available = 0;
    };

private:
    std::map<std::string, bool> known_;
    std::map<std::string, QuoteSlice> current_;
    std::map<std::string, std::vector<VersionRecord>> history_;
};

}  // namespace lpsim
