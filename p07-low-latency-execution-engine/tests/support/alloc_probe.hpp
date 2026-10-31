// Global allocation counter.
//
// FR-03 and LI-11 say the hot path performs no dynamic allocation once the
// engine is warm, and doc 2 says the oracle is "malloc tracing = 0 when the
// workload is supported". An assertion about allocation that is not measured is
// not evidence, so this replaces the global operator new and delete and counts
// them.
//
// Scope, stated because it bounds what the number means:
//   * it counts the replaceable global operators. It does not see a direct
//     malloc, an mmap, or an allocation inside a library that provides its own
//     operator new. For this project that is sufficient: every container and
//     smart pointer in the measured path goes through the global operators.
//   * the counters are not atomic per allocation by default -- they are
//     relaxed atomics, which is accurate enough for "is this zero" and cheap
//     enough not to distort the thing being measured.
//   * the probe is linked only into the suites that assert on it, so it cannot
//     perturb the others.
#pragma once

#include <cstddef>
#include <cstdint>

namespace probe {

std::uint64_t allocations();
std::uint64_t deallocations();
std::uint64_t bytes_allocated();
std::uint64_t live_blocks();

// Arms the counters. Everything before this -- construction, prewarm, fixture
// building -- is outside the measured region by definition.
void reset();

// RAII window. `allocations()` inside it is what the test asserts on.
class Window {
public:
    Window() : start_alloc_(allocations()), start_free_(deallocations()) {}
    std::uint64_t allocations_since() const { return allocations() - start_alloc_; }
    std::uint64_t deallocations_since() const { return deallocations() - start_free_; }

private:
    std::uint64_t start_alloc_;
    std::uint64_t start_free_;
};

}  // namespace probe
