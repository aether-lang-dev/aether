#ifndef AETHER_COUNT_H
#define AETHER_COUNT_H

/* A reference count another thread may change at the same time: a shared
 * string's (#2919), a string sequence cell's. Increments need no ordering;
 * a decrement orders this thread's reads of the object before another's
 * free of it. Plain where the target has no threads. */

#include "../config/aether_optimization_config.h"  /* AETHER_HAS_THREADS */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

static inline void aether_count_inc(int* p) {
#if !AETHER_HAS_THREADS
    (*p)++;
#elif defined(_MSC_VER) && !defined(__clang__)
    _InterlockedIncrement((volatile long*)p);
#else
    __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
#endif
}

/* Sets `bits` in the count, returning it as it was: two threads setting a
 * flag bit at once both see whether the other came first (#3568, a slice
 * buffer's published bit). */
static inline int aether_count_or(int* p, int bits) {
#if !AETHER_HAS_THREADS
    int old = *p;
    *p = old | bits;
    return old;
#elif defined(_MSC_VER) && !defined(__clang__)
    return (int)_InterlockedOr((volatile long*)p, (long)bits);
#else
    return __atomic_fetch_or(p, bits, __ATOMIC_ACQ_REL);
#endif
}

/* The count as it is now, for a decision another thread's change of it may
 * race: whether it is shared (the flag bit), whether this is the last
 * reference. Every read of a count that can be shared goes through here;
 * a plain read of one raced the other threads' atomic changes.
 * Acquire, so a count of one read here comes after every other holder's
 * release of it (aether_count_dec), as a buffer mutated in place needs. */
static inline int aether_count_load(const int* p) {
#if !AETHER_HAS_THREADS
    return *p;
#elif defined(_MSC_VER) && !defined(__clang__)
#  if defined(_M_ARM64)
    return (int)__ldar32((unsigned __int32 volatile*)p);
#  else
    /* x86 and x64: a load is an acquire; the barrier keeps the compiler
     * from moving later reads above it. */
    int v = *(const volatile int*)p;
    _ReadWriteBarrier();
    return v;
#  endif
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}

/* Returns the count before the decrement. */
static inline int aether_count_dec(int* p) {
#if !AETHER_HAS_THREADS
    return (*p)--;
#elif defined(_MSC_VER) && !defined(__clang__)
    return (int)_InterlockedDecrement((volatile long*)p) + 1;
#else
    return __atomic_fetch_sub(p, 1, __ATOMIC_ACQ_REL);
#endif
}

#endif
