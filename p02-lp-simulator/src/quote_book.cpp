#include "lp/quote_book.hpp"

#include <limits>

namespace lpsim {

std::string_view to_string(QuoteError e) {
    switch (e) {
        case QuoteError::Ok: return "Ok";
        case QuoteError::UnknownSymbol: return "UnknownSymbol";
        case QuoteError::CrossedBook: return "CrossedBook";
        case QuoteError::NotPositivePrice: return "NotPositivePrice";
        case QuoteError::NegativeQuantity: return "NegativeQuantity";
        case QuoteError::QuantityOverflow: return "QuantityOverflow";
        case QuoteError::DuplicateVersion: return "DuplicateVersion";
        case QuoteError::NonMonotonic: return "NonMonotonic";
        case QuoteError::VersionMismatch: return "VersionMismatch";
        case QuoteError::InsufficientLiquidity: return "InsufficientLiquidity";
    }
    return "Unknown";
}

namespace {
// A single quote side may not offer more than this. The cap exists so that a
// crafted fixture cannot create a quantity that later overflows the weighted
// notional sum (L-10, D-19).
constexpr Units kMaxAvailableUnits = 1000000000000LL;  // 1e12 base units
}  // namespace

std::string QuoteSlice::slice_id(Side taker_side) const {
    return symbol + ":" + std::to_string(version) + ":" +
           (taker_side == Side::Buy ? "ASK" : "BID");
}

bool QuoteBook::knows(std::string_view symbol) const {
    return known_.find(std::string(symbol)) != known_.end();
}

QuoteError QuoteBook::apply_update(const QuoteSlice& q) {
    if (!knows(q.symbol)) return QuoteError::UnknownSymbol;

    // Validate everything before touching state.
    if (q.bid_ticks <= 0 || q.ask_ticks <= 0) return QuoteError::NotPositivePrice;
    if (q.bid_available < 0 || q.ask_available < 0) return QuoteError::NegativeQuantity;
    if (q.bid_available > kMaxAvailableUnits || q.ask_available > kMaxAvailableUnits)
        return QuoteError::QuantityOverflow;
    // A crossed or locked book would let a buy and a sell both fill at a better
    // price than either counterparty quoted, so it is refused outright (D-08).
    if (q.bid_ticks >= q.ask_ticks) return QuoteError::CrossedBook;
    if (q.version == 0) return QuoteError::NonMonotonic;
    if (q.expires_at_ms <= q.valid_from_ms) return QuoteError::NonMonotonic;

    auto& hist = history_[q.symbol];
    for (const VersionRecord& r : hist) {
        if (r.version != q.version) continue;
        // Same version seen before: identical content is an idempotent replay,
        // anything else is a conflict (D-13).
        const bool same = r.bid_ticks == q.bid_ticks && r.ask_ticks == q.ask_ticks &&
                          r.bid_available == q.bid_available &&
                          r.ask_available == q.ask_available;
        return same ? QuoteError::Ok : QuoteError::DuplicateVersion;
    }

    auto cur = current_.find(q.symbol);
    if (cur != current_.end() && q.version <= cur->second.version)
        return QuoteError::NonMonotonic;  // D-14

    hist.push_back(VersionRecord{q.version, q.bid_ticks, q.ask_ticks, q.bid_available,
                                 q.ask_available});
    current_[q.symbol] = q;
    return QuoteError::Ok;
}

const QuoteSlice* QuoteBook::current(std::string_view symbol) const {
    const auto it = current_.find(std::string(symbol));
    return it == current_.end() ? nullptr : &it->second;
}

QuoteError QuoteBook::consume(std::string_view symbol, std::uint64_t version, Side taker_side,
                              Units qty) {
    if (qty <= 0) return QuoteError::NegativeQuantity;
    const auto it = current_.find(std::string(symbol));
    if (it == current_.end()) return QuoteError::UnknownSymbol;
    QuoteSlice& q = it->second;
    if (q.version != version) return QuoteError::VersionMismatch;

    Units& avail = (taker_side == Side::Buy) ? q.ask_available : q.bid_available;
    if (qty > avail) return QuoteError::InsufficientLiquidity;
    avail -= qty;
    return QuoteError::Ok;
}

Units QuoteBook::inventory_units(std::string_view symbol, Side taker_side) const {
    const QuoteSlice* q = current(symbol);
    if (q == nullptr) return 0;
    return q->available_for(taker_side);
}

std::vector<QuoteSlice> QuoteBook::snapshot() const {
    std::vector<QuoteSlice> out;
    out.reserve(current_.size());
    for (const auto& kv : current_) out.push_back(kv.second);
    return out;
}

void QuoteBook::restore(const std::vector<QuoteSlice>& slices) {
    for (const QuoteSlice& q : slices) {
        known_[q.symbol] = true;
        current_[q.symbol] = q;
        auto& hist = history_[q.symbol];
        bool seen = false;
        for (const VersionRecord& r : hist) {
            if (r.version == q.version) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            // The restored record carries the REMAINING availability, which is
            // what makes a restart unable to resurrect consumed liquidity
            // (D-17, D-18).
            hist.push_back(VersionRecord{q.version, q.bid_ticks, q.ask_ticks, q.bid_available,
                                         q.ask_available});
        }
    }
}

}  // namespace lpsim
