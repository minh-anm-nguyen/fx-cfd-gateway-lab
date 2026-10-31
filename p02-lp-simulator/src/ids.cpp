#include "lp/ids.hpp"

#include <cstdio>
#include <limits>

namespace lpsim {

std::string IdAllocator::format(std::uint64_t counter) const {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%0*llu", pad_,
                                static_cast<unsigned long long>(counter));
    return prefix_ + std::string(buf, static_cast<std::size_t>(n < 0 ? 0 : n));
}

std::string IdAllocator::allocate() {
    const std::string id = format(next_);
    ++next_;
    return id;
}

bool IdAllocator::adopt_highwater(std::uint64_t observed_max_counter) {
    if (observed_max_counter == 0) return true;  // nothing has been issued yet
    if (observed_max_counter >= next_) {
        // Identifiers exist that the persisted counter does not cover. The
        // counter is repaired from the ledger so that forensic tooling can see
        // the correct value, and the caller is told to fail closed.
        next_ = observed_max_counter + 1;
        return false;
    }
    return true;
}

std::optional<std::uint64_t> IdAllocator::parse_counter(std::string_view id) const {
    if (id.size() <= prefix_.size()) return std::nullopt;
    if (id.compare(0, prefix_.size(), prefix_) != 0) return std::nullopt;

    const std::string_view digits = id.substr(prefix_.size());
    if (digits.empty()) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        if (v > (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(c - '0')) / 10)
            return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

IdAllocator make_order_id_allocator(std::uint64_t next) {
    return IdAllocator("SIM-", next, 4);
}

IdAllocator make_exec_id_allocator(std::uint64_t next) {
    return IdAllocator("EX-", next, 6);
}

IdAllocator make_fill_id_allocator(std::uint64_t next) {
    return IdAllocator("FL-", next, 6);
}

IdAllocator make_event_id_allocator(std::uint64_t next) {
    return IdAllocator("EV-", next, 6);
}

}  // namespace lpsim
