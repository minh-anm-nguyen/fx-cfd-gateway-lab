// Monotonic identifier allocators.
//
// Reference: design doc 6.2. OrderID, ExecID, fill_id and outbox event_id are
// all LP-generated and all must be unique for the lifetime of the business
// store, independently of the FIX session epoch (invariant I-05, I-08, I-15).
//
// The counters live in the durable store, which creates two failure modes the
// matrix names explicitly:
//
//   C-14  a crash between reserving an OrderID and committing the order must
//         not let the next startup hand out the same OrderID;
//   J-15  a restored snapshot whose counter sits BELOW an ID already present in
//         the ledger must be detected, not quietly reused.
//
// adopt_highwater() is how startup recovery closes both: it is given the
// largest counter observed in the ledger and refuses to move the allocator
// backwards, so a rolled-back counter surfaces as a HALT rather than as a
// duplicate ExecID discovered later by a client.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lpsim {

class IdAllocator {
public:
    IdAllocator(std::string prefix, std::uint64_t next, int pad_width)
        : prefix_(std::move(prefix)), next_(next), pad_(pad_width) {}

    // Format an identifier for a counter value without consuming it.
    std::string format(std::uint64_t counter) const;

    // Consume the next counter.
    std::string allocate();

    std::uint64_t next_counter() const { return next_; }

    // Set the counter when loading persisted state. Moving it forward is
    // always allowed; this is the only way it ever moves.
    void set_next(std::uint64_t n) { next_ = n; }

    // Raise the counter so that it sits strictly above every identifier
    // already issued. Returns false when the persisted counter was BELOW the
    // observed high-water mark, which means identifiers were issued that the
    // counter no longer accounts for: the caller must HALT (J-15, J-16).
    bool adopt_highwater(std::uint64_t observed_max_counter);

    // Recover the counter from an identifier this allocator produced, so that
    // recovery can derive a high-water mark from the ledger. Returns nullopt
    // when the identifier does not belong to this allocator.
    std::optional<std::uint64_t> parse_counter(std::string_view id) const;

    const std::string& prefix() const { return prefix_; }

private:
    std::string prefix_;
    std::uint64_t next_ = 1;
    int pad_ = 0;
};

// The identifier shapes used across the project. They match the worked example
// in design doc 7.4 so that golden fixtures stay readable.
IdAllocator make_order_id_allocator(std::uint64_t next = 1001);
IdAllocator make_exec_id_allocator(std::uint64_t next = 1);
IdAllocator make_fill_id_allocator(std::uint64_t next = 1);
IdAllocator make_event_id_allocator(std::uint64_t next = 1);

}  // namespace lpsim
