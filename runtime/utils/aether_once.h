#ifndef AETHER_ONCE_H
#define AETHER_ONCE_H

/* A lazy initialisation that any thread may reach first (#3680).
 *
 *   static aether_once_t g_once = AETHER_ONCE_INIT;
 *   if (aether_once_begin(&g_once)) {
 *       ... compute the value, fill the table ...
 *       aether_once_done(&g_once);
 *   }
 *   ... use it ...
 *
 * aether_once_begin returns 1 to the one thread that is to run the
 * initialisation, which then calls aether_once_done; every other thread,
 * and every later call, gets 0, once the initialisation is done: a thread
 * that comes while it runs waits for it. The done store is a release and
 * every load that sees it an acquire, so what the initialisation wrote is
 * seen by whoever reads the result.
 *
 * A plain flag (`if (done) return; ...; done = 1;`) raced: two threads both
 * ran the initialisation, one wrote what the other read, and a reader that
 * saw the flag could see the values it guards before they were written.
 *
 * The runtime's own pthread shim has no pthread_once (aether_thread.h), so
 * this is the once. Not reentrant: an initialisation that reached its own
 * once would wait for itself. In a build without threads or atomics it is a
 * plain flag. */

#include "../config/aether_optimization_config.h"   /* AETHER_HAS_THREADS, AETHER_HAS_ATOMICS */
#include "aether_compiler.h"                        /* AETHER_CPU_PAUSE */

#define AETHER_ONCE_INIT 0

#if AETHER_HAS_THREADS && AETHER_HAS_ATOMICS
#include <stdatomic.h>

typedef atomic_int aether_once_t;   /* 0 not begun, 1 running, 2 done */

static inline int aether_once_begin(aether_once_t* o) {
    if (atomic_load_explicit(o, memory_order_acquire) == 2) return 0;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(o, &expected, 1, memory_order_acquire,
                                                memory_order_acquire))
        return 1;
    while (atomic_load_explicit(o, memory_order_acquire) != 2) AETHER_CPU_PAUSE();
    return 0;
}

static inline void aether_once_done(aether_once_t* o) {
    atomic_store_explicit(o, 2, memory_order_release);
}

/* Whether the initialisation is done, for a reader that does not run it:
 * an acquire, as begin's is. */
static inline int aether_once_is_done(aether_once_t* o) {
    return atomic_load_explicit(o, memory_order_acquire) == 2;
}

/* For a teardown that may initialise again later, with no other thread
 * left to race it. */
static inline void aether_once_reset(aether_once_t* o) {
    atomic_store_explicit(o, 0, memory_order_relaxed);
}

#else

typedef int aether_once_t;

static inline int aether_once_begin(aether_once_t* o) {
    if (*o) return 0;
    *o = 1;
    return 1;
}

static inline void aether_once_done(aether_once_t* o) { *o = 2; }
static inline int aether_once_is_done(aether_once_t* o) { return *o == 2; }
static inline void aether_once_reset(aether_once_t* o) { *o = 0; }

#endif

#endif /* AETHER_ONCE_H */
