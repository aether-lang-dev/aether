/* aether_resource_caps.h — runtime per-call resource caps (#343)
 *
 * Two caps for `--emit=lib` plugin-host scenarios where untrusted
 * Aether code runs inside a trusted process:
 *
 *   1. Process-wide memory cap. The atomic counter `g_mem_used`
 *      tracks current allocated bytes (NOT high-water-mark);
 *      `aether_caps_check_alloc(n)` refuses past the configured
 *      ceiling. Cap-aware allocators in std/string, std/collections,
 *      runtime/memory consult the counter before malloc/realloc and
 *      decrement via `aether_caps_account_free(n)` on free. Counter
 *      saturates at zero in release builds; debug builds assert on
 *      under-flow so accounting bugs surface early.
 *
 *   2. Per-thread wall-clock deadline. `aether_caps_set_deadline_ms`
 *      records `clock_gettime(CLOCK_MONOTONIC) + ms` in TLS;
 *      `aether_caps_deadline_tripped()` is the codegen-emitted
 *      tripwire that compares to NOW at every loop head and at the
 *      mailbox-receive in actor handlers. Deadline-tripped state is
 *      sticky — once `__aether_abort_call()` flips the TLS flag,
 *      every subsequent tripwire check returns 1 until the host
 *      explicitly clears the deadline by setting a new one.
 *
 * Public C surface (libaether.h) wraps the symbols below in
 * commit 2/3 (libaether_caps.c). Codegen emits direct calls to
 * `aether_caps_deadline_tripped` + `__aether_abort_call` only
 * when --emit=lib is active (commit 3/3).
 *
 * Fast path: when no cap is set, every check is one branch + one
 * relaxed atomic load. Hot-path cost ≈ 1 ns.
 */

#ifndef AETHER_RESOURCE_CAPS_H
#define AETHER_RESOURCE_CAPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if the alloc is permitted (and accounts `bytes` against
 * the counter); 0 if the cap would be exceeded. The caller does the
 * actual malloc/realloc; on alloc failure it must call
 * aether_caps_account_free(bytes) to roll back the accounting. */
int aether_caps_check_alloc(size_t bytes);

/* Decrement the accounting counter by `bytes`. Saturating —
 * decrementing more than was accumulated leaves the counter at 0
 * rather than wrapping (an underflow would falsely trip the cap on
 * subsequent allocations). Debug builds (NDEBUG unset) assert. */
void aether_caps_account_free(size_t bytes);

/* Set the process-wide memory cap. `bytes == 0` disables (unlimited).
 * Idempotent; takes effect immediately for subsequent
 * aether_caps_check_alloc calls. Existing accounted allocations are
 * unaffected — only new allocations are gated. */
void aether_caps_set_memory_cap(uint64_t bytes);

/* Arm the per-thread wall-clock deadline. `ms == 0` disables (no
 * deadline). Sets the TLS deadline timestamp to `now + ms` and
 * clears the sticky-tripped flag. */
void aether_caps_set_deadline_ms(int64_t ms);

/* Inline tripwire — codegen emits this at every loop head when
 * --emit=lib is active. Returns 1 if the deadline has passed OR
 * the sticky-tripped flag is set; 0 otherwise. Direct TLS read +
 * atomic load + monotonic clock — no PLT call, no allocation. */
/* Nonzero once any deadline has been armed or tripped (see the .c). */
extern int aether_caps_armed;
int aether_caps_deadline_tripped(void);

/* Set the sticky-tripped flag; called by codegen-emitted code on
 * tripwire-positive at a loop head, by the libaether wrapper to
 * surface the deadline state to the host, or by tests. After this
 * call every subsequent aether_caps_deadline_tripped returns 1
 * until aether_caps_set_deadline_ms is called again with a fresh
 * deadline (which clears the flag). */
void __aether_abort_call(void);

/* Snapshot of currently-accounted bytes: what the cap-aware allocators
 * above (collections, arenas, pools, the actor registry) hold, and only
 * those. heap.new, strings and closure environments are plain mallocs the
 * cap does not see, so this is not a measure of what the process holds;
 * for a leak check use aether_heap_in_use() (runtime/aether_process_mem.h,
 * std.mem's heap_in_use), which reads the allocator itself (#2310). */
uint64_t aether_caps_used_bytes(void);

/* ============================================================
 * Cap-aware allocator wrappers
 * ============================================================
 *
 * Drop-in replacements for malloc / calloc / realloc / free that
 * thread the cap counter through. Allocators that participate in
 * the cap (string, collections, arena, pool) use these instead of
 * libc primitives. The pointer returned has the SAME shape as a
 * libc malloc — no hidden header — so it can be passed to libc
 * free safely (cap accounting is lost in that case but heap
 * integrity is preserved). For full cap accounting, free with the
 * matching aether_caps_free passing the original byte count.
 */

/* malloc(bytes) gated by the cap. Returns NULL on cap-exceeded
 * OR libc OOM. The returned pointer is libc-compatible. */
void* aether_caps_malloc(size_t bytes);

/* calloc(n, size) — gated. Buffer is zero-initialised. */
void* aether_caps_calloc(size_t n, size_t size);

/* realloc that accounts the size delta. `old_bytes` is the size
 * passed at the prior alloc/realloc; pass 0 when growing from
 * NULL (equivalent to malloc). Returns NULL on cap exceeded; the
 * original pointer is not freed in that case. */
void* aether_caps_realloc(void* p, size_t old_bytes, size_t new_bytes);

/* free + account. `bytes` is the size originally requested at
 * malloc / calloc / realloc time. NULL is a no-op (mirrors libc
 * free). The pointer is libc-compatible: aether_caps_free p; libc
 * free p; both are heap-safe — only the cap counter differs. */
void  aether_caps_free(void* p, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif  /* AETHER_RESOURCE_CAPS_H */
