#include "alloc_probe.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

namespace probe {
namespace {

std::atomic<std::uint64_t> g_alloc{0};
std::atomic<std::uint64_t> g_free{0};
std::atomic<std::uint64_t> g_bytes{0};

}  // namespace

std::uint64_t allocations() { return g_alloc.load(std::memory_order_relaxed); }
std::uint64_t deallocations() { return g_free.load(std::memory_order_relaxed); }
std::uint64_t bytes_allocated() { return g_bytes.load(std::memory_order_relaxed); }
std::uint64_t live_blocks() { return allocations() - deallocations(); }

void reset() {
    g_alloc.store(0, std::memory_order_relaxed);
    g_free.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
}

}  // namespace probe

// The replaceable global allocation functions. std::malloc is used rather than
// anything smarter: the point is to count, not to allocate better.
void* operator new(std::size_t n) {
    probe::g_alloc.fetch_add(1, std::memory_order_relaxed);
    probe::g_bytes.fetch_add(n, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) { return operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    probe::g_alloc.fetch_add(1, std::memory_order_relaxed);
    probe::g_bytes.fetch_add(n, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}

void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
}

void operator delete(void* p) noexcept {
    if (p != nullptr) probe::g_free.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }

// Aligned forms, needed because the pool's storage and the cache-aligned ring
// members request over-aligned allocations.
void* operator new(std::size_t n, std::align_val_t a) {
    probe::g_alloc.fetch_add(1, std::memory_order_relaxed);
    probe::g_bytes.fetch_add(n, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(a);
    // Round the size up to a multiple of the alignment: aligned_alloc requires
    // it, and MSVC-style _aligned_malloc does not but accepts it.
    const std::size_t size = ((n + align - 1) / align) * align;
#if defined(_WIN32)
    void* p = _aligned_malloc(size, align);
#else
    void* p = std::aligned_alloc(align, size);
#endif
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }

void operator delete(void* p, std::align_val_t) noexcept {
    if (p != nullptr) probe::g_free.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept {
    operator delete(p, a);
}
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept {
    operator delete(p, a);
}
