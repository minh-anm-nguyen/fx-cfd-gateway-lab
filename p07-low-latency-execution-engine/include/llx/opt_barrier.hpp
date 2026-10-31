// Optimization barriers for measurement code.
//
// Design doc 15.3 lists dead-code elimination as the first benchmarking trap:
// a loop whose result is never observed can be deleted outright, and the
// resulting "0 ns" is then reported as an improvement. Every measured loop in
// this project therefore either checks a final checksum or passes its result
// through do_not_optimize().
//
// These are the standard inline-asm barriers. On a compiler without GNU inline
// asm the fallback writes through a volatile pointer, which is weaker (it does
// not model a general memory clobber) but still prevents the common case of the
// whole computation being discarded.
#pragma once

#include <cstddef>

namespace llx {

#if defined(__GNUC__) || defined(__clang__)

// Forces `value` to be materialized as if it escaped into unknown code.
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

template <typename T>
inline void do_not_optimize_mut(T& value) {
    asm volatile("" : "+r,m"(value) : : "memory");
}

// Tells the compiler that all reachable memory may have been written, so stores
// made before the barrier cannot be sunk past it.
inline void clobber_memory() { asm volatile("" : : : "memory"); }

#else

template <typename T>
inline void do_not_optimize(T const& value) {
    volatile const void* sink = static_cast<const void*>(&value);
    (void)sink;
}

template <typename T>
inline void do_not_optimize_mut(T& value) {
    volatile void* sink = static_cast<void*>(&value);
    (void)sink;
}

inline void clobber_memory() {}

#endif

}  // namespace llx
