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
