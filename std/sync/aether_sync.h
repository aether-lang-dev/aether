/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */
#ifndef AETHER_SYNC_H
#define AETHER_SYNC_H

/* std.sync — a single atomic 64-bit integer cell (issue #2082).
 *
 * `.ae` has an atomic *pointer* swap (std.snapshot) but no atomic
 * *integer* — so there is no way to build a reference count or a small
 * lock-free retire ring in pure Aether, which is exactly what a
 * pool-owned copy-on-write structure needs to reclaim its displaced
 * values safely (see std.snapshot's reclamation contract). This closes
 * that gap with the smallest primitive that does the job: one atomic
 * i64 with load / store / fetch-style add & sub / compare-and-swap.
 *
 *   c = sync.atomic_new(0)          // heap atomic i64, init value
 *   v = sync.atomic_load(c)         // acquire load
 *       sync.atomic_store(c, v)     // release store
 *   n = sync.atomic_add(c, d)       // add d, return the NEW value
 *   n = sync.atomic_sub(c, d)       // subtract d, return the NEW value
 *   ok= sync.atomic_cas(c, exp, nw) // 1 if swapped exp->nw, else 0
 *       sync.atomic_free(c)         // free the cell
 *
 * WHY add/sub RETURN THE NEW VALUE (not the previous one, as C's
 * fetch_add does): the dominant use is a reference count where the
 * caller must act exactly when the count reaches 0. Returning the
 * post-operation value makes `if sync.atomic_sub(rc, 1) == 0 { reclaim }`
 * correct with no extra arithmetic and no room to fumble the fetch-then-
 * adjust step across the language boundary.
 *
 * MEMORY ORDERING. load is acquire, store is release, add/sub/cas are
 * acq_rel on success — so an atomic that gates access to other data
 * (a refcount protecting a value, a flag publishing a buffer) carries
 * the usual publish/subscribe guarantees: whatever a thread wrote before
 * it released the counter is visible to a thread that acquires it. This
 * is the same discipline std.snapshot uses for its pointer.
 *
 * The value is a signed 64-bit integer, matching Aether's `int`/`long`
 * lowering, so a refcount, a generation number, or a pointer punned
 * through `mem.long_to_ptr` all fit. The cell is cap-accounted via
 * aether_caps_malloc, consistent with the process memory cap.
 *
 * SCOPE. This is the low-level plumbing primitive, deliberately minimal —
 * it is NOT a mutex and NOT a general lock. It exists so pool-owned,
 * C-style shared state (the shape std/http/proxy/aether_proxy_lb.c
 * already uses in C) can be expressed in `.ae` without dropping to C. It
 * is not an invitation to replace the actor model with locks.
 */

#include "../../runtime/aether_resource_caps.h" /* aether_caps_malloc / aether_caps_free */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate an atomic i64 cell initialised to `initial`. Cap-accounted
 * via aether_caps_malloc. Returns the cell handle (opaque `void*`), or
 * NULL on allocation failure / cap-exceeded. */
void* aether_sync_atomic_new(int64_t initial);

/* Every operation below is FATAL (abort) on a NULL cell, not a silent no-op.
 * The only source of a NULL cell is an unchecked atomic_new() that returned
 * NULL on cap-exceeded / OOM, and a swallowed NULL is dangerous: 0 from
 * atomic_sub means "refcount reached zero, reclaim", so silently returning 0
 * would free a live value. Check atomic_new()'s result. (free() is the sole
 * exception -- NULL there is a no-op, mirroring libc free.) */

/* Acquire load of the current value. Fatal on a NULL cell. */
int64_t aether_sync_atomic_load(void* cell);

/* Release store of `value`. Fatal on a NULL cell. */
void aether_sync_atomic_store(void* cell, int64_t value);

/* Atomically add `delta` and return the NEW (post-add) value. acq_rel.
 * Fatal on a NULL cell. Use a negative delta, or atomic_sub, to decrement. */
int64_t aether_sync_atomic_add(void* cell, int64_t delta);

/* Atomically subtract `delta` and return the NEW (post-sub) value.
 * acq_rel. Fatal on a NULL cell. The canonical refcount-release call:
 * `if (aether_sync_atomic_sub(rc, 1) == 0) reclaim();`. */
int64_t aether_sync_atomic_sub(void* cell, int64_t delta);

/* Compare-and-swap: if the cell holds `expected`, replace it with
 * `desired` and return 1; otherwise leave it unchanged and return 0.
 * Strong (no spurious failures), so an Aether-side retry loop treats a
 * 0 as a real concurrent change. Success AND failure are acquire-inclusive
 * (acq_rel on success, acquire on failure): a CAS that wins the slot then
 * reads the data it guards must acquire, or it can miss the prior owner's
 * writes -- release-only would be right only for a publish-only cell.
 * Fatal on a NULL cell. */
int aether_sync_atomic_cas(void* cell, int64_t expected, int64_t desired);

/* Free the cell. NULL is a no-op (mirrors libc free). */
void aether_sync_atomic_free(void* cell);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SYNC_H */
