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
#include <stdatomic.h>

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
/* Nonzero once any deadline has been armed or tripped, or a call cancelled
 * (see the .c). Read it with a relaxed atomic load. */
extern _Atomic int aether_caps_armed;
int aether_caps_deadline_tripped(void);

/* Set the sticky-tripped flag; called by codegen-emitted code on
 * tripwire-positive at a loop head, by the libaether wrapper to
 * surface the deadline state to the host, or by tests. After this
 * call every subsequent aether_caps_deadline_tripped returns 1
 * until aether_caps_set_deadline_ms is called again with a fresh
 * deadline (which clears the flag). */
void __aether_abort_call(void);

/* The deadline in force on this thread, as an execution frame keeps it
 * (runtime/aether_exec.c, #2955): a nested execution gets its own and the
 * outer one's is put back when it returns. */
typedef struct {
    int64_t at_ns;     /* 0: none */
    int     tripped;   /* the sticky flag */
} AetherDeadlineState;

void aether_caps_deadline_save(AetherDeadlineState* out);
void aether_caps_deadline_restore(const AetherDeadlineState* s);
/* The state aether_caps_set_deadline_ms(ms) would make, without making it. */
AetherDeadlineState aether_caps_deadline_for_ms(int64_t ms);
/* The sticky flag alone: whether a tripwire saw the deadline pass (or
 * __aether_abort_call ran), without reading the clock. */
int aether_caps_deadline_flag(void);

/* Why the innermost execution should stop (runtime/aether_exec.h): its
 * deadline passed (AETHER_STOP_DEADLINE, the outcome AETHER_DEADLINE) or
 * it was cancelled (AETHER_STOP_CANCELLED, AETHER_CANCELLED); 0 for
 * neither. For callers that found aether_caps_armed set. */
#define AETHER_STOP_DEADLINE      2
#define AETHER_STOP_CANCELLED     3
#define AETHER_STOP_MEMORY_CAP    4   /* the outcome AETHER_MEMORY_CAP */
#define AETHER_STOP_OUT_OF_MEMORY 5   /* the outcome AETHER_OUT_OF_MEMORY */
int aether_exec_stop_reason(void);

/* An allocation its caller survives failed (it returns NULL, or an error),
 * for the reason `why` (AETHER_STOP_MEMORY_CAP or AETHER_STOP_OUT_OF_MEMORY).
 * Inside an execution the execution ends with that outcome at its next safe
 * point or its return (#2956); outside one nothing changes. The cap-aware
 * allocators below call it; so does any runtime allocation that reports
 * its failure instead of stopping the process. Allocates nothing. */
void aether_alloc_failed(int why);

/* ------------------------------------------------------------------
 * Execution accounts (#2954): budgets a host owns, for executions that
 * outlive one call. libaether.h has the public half; this is the runtime's.
 * ------------------------------------------------------------------ */
#ifndef AETHER_EXEC_ACCOUNT_DEFINED
#define AETHER_EXEC_ACCOUNT_DEFINED
typedef struct AetherExecAccount AetherExecAccount;
typedef struct {
    uint64_t memory_bytes;       /* the live bytes it may hold; 0: no limit of its own */
    int64_t  deadline_ms;        /* 0: no deadline of its own */
    int      deadline_counts;    /* AETHER_DEADLINE_ELAPSED or AETHER_DEADLINE_ACTIVE */
    AetherExecAccount* parent;   /* NULL, or the account its charges also go to */
} AetherExecLimits;
#define AETHER_DEADLINE_ELAPSED 0   /* wall time since the account was made */
#define AETHER_DEADLINE_ACTIVE  1   /* time spent in executions under it */
#endif

struct AetherExecAccount {
    _Atomic uint64_t used;            /* live bytes charged to it (its children's too) */
    uint64_t memory_bytes;
    int64_t  deadline_at_ns;          /* ELAPSED: when it passes; 0: none */
    int64_t  active_budget_ns;        /* ACTIVE: what it may spend; 0: none */
    _Atomic int64_t active_spent_ns;
    int      deadline_counts;
    _Atomic int tripped;              /* AETHER_ACCOUNT_TRIPPED_* bits */
    AetherExecAccount* parent;
    _Atomic uintptr_t thread;         /* the thread it is in use on, 0 for none */
    const void* owner;                /* the accounts that made it: only they take it */
    AetherExecAccount* next_live;     /* every live account, for freeing one */
    /* What still names it (#3407), under the accounts' lock: the live list
     * until the host frees it, the thread it is in use on, and each
     * execution that began under it. The block goes with the last. */
    int      refs;
    _Atomic int dead;                 /* the host freed it: charged, used and linked no more */
};
#ifndef AETHER_ACCOUNT_TRIPPED_DEADLINE
#define AETHER_ACCOUNT_TRIPPED_DEADLINE 1
#define AETHER_ACCOUNT_TRIPPED_MEMORY   2
#endif

/* The public half (libaether.h). */
AetherExecAccount* aether_exec_account_new(const AetherExecLimits* limits);
void     aether_exec_account_free(AetherExecAccount* account);
int      aether_exec_use(AetherExecAccount* account, AetherExecAccount** previous);
uint64_t aether_exec_account_used(const AetherExecAccount* account);
int      aether_exec_account_tripped(const AetherExecAccount* account);

/* std.host's: an account from scalars. */
AetherExecAccount* aether_exec_account_make(uint64_t memory_bytes, int64_t deadline_ms, int counts,
                                            AetherExecAccount* parent);

/* The allocation fast path's word (aether_resource_caps.c): 0 when nothing
 * is to be asked; AETHER_CAPS_SLOW_ACCOUNTS while an account exists in the
 * process. Scalars, read with relaxed loads by the execution frames. */
extern _Atomic unsigned aether_caps_slow;
#define AETHER_CAPS_SLOW_ACCOUNTS 2u
/* The same word says what an actor step must put aside
 * (aether_step_barrier.h), so a plain step reads this one word and takes one
 * predicted branch: the accounts bit above; AETHER_CAPS_SLOW_SANDBOX, set for
 * good once a sandbox level may be in force where this copy's steps can see
 * it: the first thread with a level in this copy's count, or this copy
 * sharing another copy's count (aether_sandbox.c; a mere check of a getenv or
 * a file read sets nothing, #3419); and, from bit 8 up, the live restart
 * claims and spawn captures (aether_supervision.c counts them here,
 * AETHER_CAPS_SPAWN_ONE each). The
 * allocation fast path asks about its own bits only (AETHER_CAPS_ALLOC_BITS:
 * the cap, the accounts, the test hook). */
#define AETHER_CAPS_SLOW_SANDBOX  8u
#define AETHER_CAPS_SPAWN_ONE     (1u << 8)
#define AETHER_CAPS_SPAWN_MASK    (~0xFFu)
#define AETHER_CAPS_ALLOC_BITS    7u
#define AETHER_CAPS_STEP_BITS     (AETHER_CAPS_SLOW_ACCOUNTS | AETHER_CAPS_SLOW_SANDBOX | AETHER_CAPS_SPAWN_MASK)
/* Some restart claim or spawn capture is live, on any thread. A claim
 * counts itself (seq_cst) before it reads its child's inflight count; a
 * sender that counted its message in flight (seq_cst) and then finds none
 * here cannot race a claim, which will see the message and wait for it. */
static inline int aether_spawn_watched(void) {
    return (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_SPAWN_MASK) != 0;
}
/* Set once this copy has found the accounts it shares (the rendezvous). */
extern _Atomic int aether_accounts_found;
void aether_accounts_register(void);

/* The account in use on this thread (aether_exec_use), NULL for none. */
AetherExecAccount* aether_account_here(void);
/* An execution begins: the account in use, held for it until it ends, so a
 * free meanwhile leaves the execution naming no freed block (#3407). */
AetherExecAccount* aether_account_hold(void);
/* An execution ends: the account in use when it began (the one held) is in
 * use again, unless the host freed it meanwhile, and is let go. */
void aether_account_restore(AetherExecAccount* account);
/* A cooperative build's task context is detached or its task ended
 * (aether_exec.c): the account in use in `frames` is let go, from whichever
 * task. Its executions have all returned. */
struct AetherFrames;
void aether_accounts_frames_ended(struct AetherFrames* frames);
/* An actor step begins and ends (aether_step_barrier.h): the account in use
 * is put aside for the step, still this thread's, and is in use again after
 * it; one the step left in use is let go. */
AetherExecAccount* aether_account_step_aside(void);
void aether_account_step_back(AetherExecAccount* was);
/* An execution is entered now under `a` (held for it): `*at` the deadline
 * in force for it, the earliest of `a`'s and its ancestors', as absolute
 * monotonic ns (0: none); `*tripped` is set when one already passed. The
 * ACTIVE accounts on the chain no execution open on this thread times yet
 * are timed from now (#3409); `*timed` says how many, for its end. -1 when
 * one could not be timed for want of memory: the execution must then end
 * at once (AETHER_OUT_OF_MEMORY), never run with a budget unenforced. */
int aether_account_exec_begin(AetherExecAccount* a, int64_t* at, int* tripped, int* timed);
/* That execution ends, its deadline tripped or not: the accounts it began
 * timing are charged the time spent since (once, however executions nest),
 * and the trip is recorded on `a` and its ancestors. */
void aether_account_exec_end(AetherExecAccount* a, int timed, int tripped);
/* Monotonic ns, the clock the deadlines use. */
int64_t aether_caps_now_ns(void);

/* Test hook (runtime C tests and C hosts of tests only; generated code never
 * arms it): the n-th checked allocation from now on this thread fails as if
 * the allocator were out of memory, once; 0 turns it off. The checked ones
 * are the cap-aware allocators, a send's payload copy, every allocation
 * the generated code makes (_aether_alloc and _aether_alloc_or_null), and
 * the growth of a thread's list of timed ACTIVE accounts (#3409). */
void aether_test_fail_alloc_after(int n);
/* Whether the hook refuses this allocation (it counts it); read the flag
 * first, as the generated code does: nonzero once a test armed the hook. */
extern _Atomic int aether_alloc_test_armed;
int aether_alloc_test_refuses(void);

/* For a C-backed std operation that runs long on its input: 1 when the
 * running execution should stop, so the operation returns early, cleanly
 * (an error, its own memory freed); the next safe point, or the export's
 * return, ends the execution with the outcome. One load while nothing is
 * armed; 0 outside any execution. */
int aether_exec_should_stop(void);

/* Snapshot of currently-accounted bytes: what the cap-aware allocators
 * above (collections, arenas, pools, the actor registry) hold, and only
 * those. heap.new, strings and closure environments are plain mallocs the
 * cap does not see, so this is not a measure of what the process holds;
 * for a leak check use aether_heap_in_use() (runtime/aether_process_mem.h,
 * std.mem's heap_in_use), which reads the allocator itself (#2310). */
uint64_t aether_caps_used_bytes(void);

/* Test hook (runtime C tests only, #3829): 1 when the calling thread counts
 * on a slot of its own, 0 when it shares the one every thread past the owned
 * slots shares. A thread that has not counted yet takes its slot first. */
int aether_caps_thread_owns_slot(void);

/* Test hook (runtime C tests only, #3829): takes (on != 0) or releases the
 * lock every fold of the slots takes, as a cap being set holds it. A thread
 * ending while it is held must not wait for it. */
void aether_caps_test_hold_folds(int on);

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
