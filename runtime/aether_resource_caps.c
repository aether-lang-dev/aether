/* aether_resource_caps.c — runtime per-call resource caps (#343).
 *
 * See aether_resource_caps.h for the API contract. This file owns
 * the atomics, the TLS slots, and the clock arithmetic; everything
 * else (cap-aware allocators, codegen tripwire, public libaether
 * forwarders) calls in via the header.
 */

#include "aether_resource_caps.h"
#include "utils/aether_thread.h"
#include "utils/aether_compiler.h"

#include <stdatomic.h>
#include <assert.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#endif

/* Process-wide. _Atomic for the lock-free fast path — every alloc
 * site touches g_mem_used; contention is real but bounded by the
 * compare-and-add pattern. */
static _Atomic uint64_t g_mem_used = 0;
static _Atomic uint64_t g_mem_cap  = 0;   /* 0 = unlimited */

/* Per-thread. The deadline is monotonic-clock nanoseconds; 0 means
 * "no deadline armed". The tripped flag is sticky once set, until
 * aether_caps_set_deadline_ms is called again. */
static AETHER_TLS_SHARED int64_t g_deadline_at_ns = 0;
static AETHER_TLS_SHARED int     g_tripped        = 0;

/* Monotonic nanoseconds since an arbitrary epoch; only differences
 * matter. Delegates to aether_now_ns so Windows goes through the
 * overflow-free QueryPerformanceCounter split in the thread shim. */
static int64_t now_ns(void) {
    return (int64_t)aether_now_ns();
}

int aether_caps_check_alloc(size_t bytes) {
    /* Fast path: no cap set. One relaxed load + branch. */
    uint64_t cap = atomic_load_explicit(&g_mem_cap, memory_order_relaxed);
    if (cap == 0) {
        /* Still account the alloc so the counter reflects current
         * usage even when no cap is active — keeps test harnesses
         * and future cap-set transitions consistent. */
        atomic_fetch_add_explicit(&g_mem_used, (uint64_t)bytes,
                                  memory_order_relaxed);
        return 1;
    }

    /* Cap-set path: CAS-add to keep current+bytes <= cap. Loop
     * because another thread may bump the counter between our load
     * and our CAS — re-read and retry. */
    uint64_t cur = atomic_load_explicit(&g_mem_used, memory_order_relaxed);
    for (;;) {
        if (cur + (uint64_t)bytes > cap || cur + (uint64_t)bytes < cur) {
            return 0;  /* would exceed cap or wraps */
        }
        if (atomic_compare_exchange_weak_explicit(
                &g_mem_used, &cur, cur + (uint64_t)bytes,
                memory_order_relaxed, memory_order_relaxed)) {
            return 1;
        }
        /* CAS failed; cur is now the latest observed value. Retry. */
    }
}

void aether_caps_account_free(size_t bytes) {
    /* Saturating decrement: never wrap below 0. Underflow would
     * falsely trip the cap on subsequent allocations. */
    uint64_t cur = atomic_load_explicit(&g_mem_used, memory_order_relaxed);
    for (;;) {
        uint64_t next = (cur >= (uint64_t)bytes) ? (cur - (uint64_t)bytes) : 0;
#ifndef NDEBUG
        assert(cur >= (uint64_t)bytes && "alloc/free accounting underflow");
#endif
        if (atomic_compare_exchange_weak_explicit(
                &g_mem_used, &cur, next,
                memory_order_relaxed, memory_order_relaxed)) {
            return;
        }
    }
}

void aether_caps_set_memory_cap(uint64_t bytes) {
    atomic_store_explicit(&g_mem_cap, bytes, memory_order_relaxed);
}

/* Nonzero once any thread has armed or tripped a deadline. The codegen's
 * loop-head check reads this first, so code that never uses deadlines pays
 * one load instead of a call and two TLS reads. It is never cleared: another
 * thread may still be armed, and the full check below stays exact. */
int aether_caps_armed = 0;

void aether_caps_set_deadline_ms(int64_t ms) {
    if (ms > 0) aether_caps_armed = 1;
    if (ms <= 0) {
        g_deadline_at_ns = 0;
    } else {
        g_deadline_at_ns = now_ns() + ms * 1000000LL;
    }
    /* Reset the sticky flag — a fresh deadline is a fresh chance. */
    g_tripped = 0;
}

int aether_caps_deadline_tripped(void) {
    if (g_tripped) return 1;
    if (g_deadline_at_ns == 0) return 0;
    if (now_ns() >= g_deadline_at_ns) {
        aether_caps_armed = 1;
        g_tripped = 1;
        return 1;
    }
    return 0;
}

void __aether_abort_call(void) {
    aether_caps_armed = 1;
    g_tripped = 1;
}

uint64_t aether_caps_used_bytes(void) {
    return atomic_load_explicit(&g_mem_used, memory_order_relaxed);
}

#include <stdlib.h>
#include <string.h>

/* Libc-compatible wrappers: the returned pointer has no hidden
 * header, so libc free() is safe (loses cap accounting but no
 * heap corruption). For full cap accounting, callers free with
 * aether_caps_free(p, bytes) passing the original size. */

void* aether_caps_malloc(size_t bytes) {
    if (!aether_caps_check_alloc(bytes)) return NULL;
    void* p = malloc(bytes);
    if (!p) {
        aether_caps_account_free(bytes);
        return NULL;
    }
    return p;
}

void* aether_caps_calloc(size_t n, size_t size) {
    if (size != 0 && n > (size_t)-1 / size) return NULL;
    size_t bytes = n * size;
    if (!aether_caps_check_alloc(bytes)) return NULL;
    void* p = calloc(n, size);
    if (!p) {
        aether_caps_account_free(bytes);
        return NULL;
    }
    return p;
}

void* aether_caps_realloc(void* p, size_t old_bytes, size_t new_bytes) {
    if (new_bytes > old_bytes) {
        size_t delta = new_bytes - old_bytes;
        if (!aether_caps_check_alloc(delta)) return NULL;
    }
    void* np = realloc(p, new_bytes);
    if (!np) {
        if (new_bytes > old_bytes) {
            aether_caps_account_free(new_bytes - old_bytes);
        }
        return NULL;
    }
    if (new_bytes < old_bytes) {
        aether_caps_account_free(old_bytes - new_bytes);
    }
    return np;
}

void aether_caps_free(void* p, size_t bytes) {
    if (!p) return;
    free(p);
    aether_caps_account_free(bytes);
}
