/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */

/* aether_sync.c — atomic i64 cell (issue #2082).
 *
 * Implementation notes:
 *
 *   * The cell is a single C11 `_Atomic(int64_t)`. That is the entire
 *     state — no lock. load is an acquire load; store a release store;
 *     add/sub/cas are acq_rel read-modify-writes.
 *
 *   * Memory ordering. The atomic frequently gates access to OTHER data
 *     (a refcount protecting the value it counts, a generation flag
 *     publishing a freshly-built buffer), so acquire/release is not a
 *     tuning knob: a thread that acquires the counter must observe every
 *     write the releasing thread made before it. Same pairing as
 *     std.snapshot's pointer.
 *
 *   * add/sub return the NEW (post-op) value, not the previous value.
 *     C's atomic_fetch_add returns the prior value; we add the delta to
 *     it so the Aether caller can write `if sync.atomic_sub(rc,1) == 0`
 *     for the refcount-hits-zero case without a second step across the
 *     language boundary. The fetch and the add are a single atomic RMW;
 *     only the returned number is adjusted, locally, from the atomically-
 *     fetched prior value.
 *
 *   * Cap accounting. The cell is allocated through aether_caps_malloc
 *     and freed through aether_caps_free, consistent with the process
 *     memory cap (the caps audit), exactly as std.snapshot's cell is.
 */

#include "aether_sync.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* The cell. A single atomic i64; nothing else is shared state. */
typedef struct aether_sync_atomic {
    _Atomic(int64_t) value;
} aether_sync_atomic;

/* A NULL cell reaching an operation is always a bug: the only way to get one is
 * an unchecked aether_sync_atomic_new() that returned NULL on cap-exceeded / OOM.
 * Silently returning a sentinel is dangerous here -- 0 from atomic_sub means
 * "reference count hit zero, reclaim now", so a swallowed NULL would free a live
 * value on the first release. Fail fast and loud instead, on whatever thread
 * (including a foreign std.http pool worker where the panic unwind may not be
 * installed), so the missing NULL check surfaces at the call site that has it. */
static void aether_sync_null_fatal(const char* op) {
    fprintf(stderr,
            "fatal: std.sync.%s called on a NULL cell -- atomic_new() returned "
            "NULL (memory cap exceeded / OOM) and the result was not checked "
            "before use.\n",
            op);
    fflush(stderr);
    abort();
}

void* aether_sync_atomic_new(int64_t initial) {
    aether_sync_atomic* c =
        (aether_sync_atomic*)aether_caps_malloc(sizeof *c);
    if (!c) {
        return NULL; /* cap-exceeded or OOM */
    }
    /* No other thread can see `c` yet (not returned), so a relaxed init
     * store is sufficient — acquire/release only matters once shared. */
    atomic_store_explicit(&c->value, initial, memory_order_relaxed);
    return c;
}

int64_t aether_sync_atomic_load(void* cell) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        aether_sync_null_fatal("atomic_load");
    }
    return atomic_load_explicit(&c->value, memory_order_acquire);
}

void aether_sync_atomic_store(void* cell, int64_t value) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        aether_sync_null_fatal("atomic_store");
    }
    atomic_store_explicit(&c->value, value, memory_order_release);
}

int64_t aether_sync_atomic_add(void* cell, int64_t delta) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        aether_sync_null_fatal("atomic_add");
    }
    /* fetch_add returns the PRIOR value; add delta locally to report the
     * new value. The RMW itself is a single atomic step. */
    int64_t prev =
        atomic_fetch_add_explicit(&c->value, delta, memory_order_acq_rel);
    return prev + delta;
}

int64_t aether_sync_atomic_sub(void* cell, int64_t delta) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        /* 0 means "refcount reached zero, reclaim" -- a swallowed NULL here
         * frees a live value on the first release. Fatal, never 0. */
        aether_sync_null_fatal("atomic_sub");
    }
    int64_t prev =
        atomic_fetch_sub_explicit(&c->value, delta, memory_order_acq_rel);
    return prev - delta;
}

int aether_sync_atomic_cas(void* cell, int64_t expected, int64_t desired) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        aether_sync_null_fatal("atomic_cas");
    }
    /* Strong CAS — no spurious failures — so the Aether-side retry loop
     * treats a 0 as a real concurrent change, not noise. `expected` is
     * passed by value; on failure the Aether caller re-loads to get the
     * fresh current value rather than relying on a written-back expected,
     * keeping the extern signature a plain (ptr,long,long)->int.
     *
     * Success is acq_rel, not plain release: a CAS that WINS the slot and
     * then reads the value it now guards (a lock taken, a refcount claimed)
     * must acquire, or it can observe writes made before the prior owner
     * published. release-only is right for a publish-only cell (std.snapshot)
     * but wrong for a lock/refcount. Failure is acquire so the reloaded value
     * for the next retry is a consistent view. */
    int64_t exp = expected;
    return atomic_compare_exchange_strong_explicit(
               &c->value, &exp, desired,
               memory_order_acq_rel, memory_order_acquire)
               ? 1
               : 0;
}

void aether_sync_atomic_free(void* cell) {
    aether_sync_atomic* c = (aether_sync_atomic*)cell;
    if (!c) {
        return; /* NULL-safe, mirrors libc free */
    }
    aether_caps_free(c, sizeof *c);
}
