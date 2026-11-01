/* libaether.h — public C API for embedders of `--emit=lib` Aether
 * code.
 *
 * Stable C interface that host programs (Java/Python/Go embedders,
 * plugin systems, language gateways) link against to drive
 * Aether-generated `.so` / `.a` artifacts.
 *
 * Compatibility (#2957, docs/abi-policy.md): this surface is versioned by
 * AETHER_HOST_ABI below. Within a level it only grows: new functions,
 * new constants, new outcomes; a removal, a layout change of a struct
 * here or a change of meaning bumps the level. A library records the
 * level it was built at in its catalog (aether_lib_meta, host_abi; none
 * is level 0, Aether 0.856 and before). A deprecated name stays, accepted
 * and documented as such, until the next level removes it.
 *
 * Today this surface exposes the per-call resource caps from issue
 * #343 (memory ceiling + wall-clock deadline). Future additions
 * land as additive symbols — never break the existing names.
 *
 * The Aether build emits codegen-level calls into this surface
 * automatically when `--emit=lib` is set. Embedders that drive
 * those caps from outside (typical plugin host) include this
 * header and link `libaether.a` (or the platform shared variant).
 */

#ifndef LIBAETHER_H
#define LIBAETHER_H

#include <stdint.h>

/* The host ABI level of this header (#2957). */
#define AETHER_HOST_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol-visibility export. Builds with `-fvisibility=hidden`
 * still expose these as default-visibility public symbols. */
#if defined(__GNUC__) || defined(__clang__)
#  define AETHER_API __attribute__((visibility("default")))
#elif defined(_MSC_VER)
#  define AETHER_API __declspec(dllexport)
#else
#  define AETHER_API
#endif

/* Process-wide memory cap. After this call, every subsequent
 * cap-aware allocator (string, list, map, arena, pool) refuses
 * allocations that would push current usage past `bytes` and
 * returns NULL / its error-string convention. Pass `0` to
 * disable (unlimited).
 *
 * Already-accounted allocations are unaffected — only new
 * allocations are gated. Existing ones drain via their normal
 * free paths and decrement the counter.
 *
 * Inside an export, a refused allocation (or one the allocator could
 * not satisfy) also ends the call at its next safe point or its return,
 * with the outcome AETHER_MEMORY_CAP (or AETHER_OUT_OF_MEMORY) and the
 * zero value, its defers run (#2956); see aether_last_outcome().
 *
 * Thread-safe: relaxed atomic store. */
AETHER_API void aether_set_memory_cap(uint64_t bytes);

/* Per-thread wall-clock deadline: one instant, `deadline_at = now + ms`
 * when this is called, not a budget each call gets. After this call,
 * every safe point (every loop head in `--emit=lib` builds, and every
 * 4096 values a JSON parse reads) of every call on this thread checks
 * `now >= deadline_at`, until a fresh deadline or 0. Inside an export the
 * first one past the deadline ends the call (#2956): the export returns
 * its zero value with the outcome AETHER_DEADLINE, its defers and owned
 * locals released on the way, and a `try` in the script does not catch
 * it. Code that runs no safe point is not cut: straight-line code,
 * recursion without a loop, and C-backed std operations other than the
 * JSON parse (docs/emit-lib.md, "Per-call resource caps"). On a trip the
 * sticky flag flips, and `aether_deadline_tripped()` returns 1 for every
 * subsequent call until you arm a fresh deadline.
 *
 * Pass `0` to disable. Calling with a non-zero value clears the
 * sticky flag (a fresh deadline is a fresh chance). */
AETHER_API void aether_set_call_deadline(int64_t ms);

/* Returns 1 when the per-thread deadline has passed OR when
 * `__aether_abort_call` was explicitly invoked since the last
 * `aether_set_call_deadline`. Returns 0 otherwise.
 *
 * Embedders typically don't call this directly: aether_last_outcome()
 * says AETHER_DEADLINE for a call the deadline ended. The host's job is
 * to set the deadline before invoking the entry function. */
AETHER_API int aether_deadline_tripped(void);

/* How the last call into an export on this thread ended (#2956). Every
 * aether_<name> export runs inside an execution frame: a panic the export
 * does not catch ends the call, not the process, and the export returns
 * the zero value of its result (NULL for a string, which the release
 * accepts). A zero value with an outcome other than AETHER_OK is not a
 * result; release only what an AETHER_OK call returned. That holds for a
 * call that ran to its return with a stop noted after its last safe point
 * (an allocation it survived, a JSON parse that stopped, the deadline's
 * flag) too: it returns the zero value, and the library releases what it
 * would have returned, a string or a struct's or closure's parts (#3388).
 * A `ptr` result owns nothing, so what it pointed to is not released, as on
 * a panic. The library is usable again at once; nothing done before the
 * failure is rolled back.
 *
 * New outcomes may be added within a host ABI level: a host treats any
 * outcome it does not know as a failure, as it does AETHER_PANICKED.
 * AETHER_INCOMPATIBLE (#2957) means the library was made for another
 * runtime than the one it found (a --shared-runtime library, or generated C
 * compiled into a host, against a runtime of another version): none of its
 * code ran, every call returns its zero value with it, and
 * aether_last_outcome_text() names both runtimes.
 *
 * Nested calls (an export calls a host service, which calls an export)
 * each have their own outcome, and the outer call's caller info,
 * deadline and sandbox grants are back when the inner one returns, on
 * every exit. The frame costs a context save per call (about 5.7 ns on
 * Windows MinGW for an export that calls a function; none for one that
 * can neither panic, allocate, loop nor call anything): docs/emit-lib.md,
 * "Failures and nested calls at the boundary". */
#ifndef AETHER_OK
#define AETHER_OK             0   /* returned normally */
#define AETHER_PANICKED       1   /* a panic the export did not catch */
#define AETHER_DEADLINE       2   /* its deadline passed (see above) */
#define AETHER_CANCELLED      3   /* cancelled from another thread */
#define AETHER_MEMORY_CAP     4   /* an allocation went past the memory cap */
#define AETHER_OUT_OF_MEMORY  5   /* the allocator could not satisfy one */
#define AETHER_INCOMPATIBLE   6   /* the library cannot run on the runtime it found (#2957) */
#endif
AETHER_API int aether_last_outcome(void);

/* The last outcome's reason: "" for AETHER_OK, the panic's message (cut at
 * 1023 bytes) or a fixed description otherwise. Borrowed: valid until the
 * next call into the library on this thread. */
AETHER_API const char* aether_last_outcome_text(void);

/* Cancellation from any thread (#2956). aether_exec_token() names the call
 * running on the calling thread or, outside one, the next call that thread
 * makes; pass it to another thread, whose aether_exec_cancel(token) ends
 * that call at its next safe point (a loop head of the
 * library's code, or every few thousand units of a long std operation)
 * with AETHER_CANCELLED. A call nested inside the cancelled one ends first,
 * then the cancelled one once the host code between them returns. A token
 * stays valid for the process; cancelling a call that already ended does
 * nothing. Returns 0, or -1 for a token that names nothing. A deadline set
 * with aether_set_call_deadline ends a call the same way, with
 * AETHER_DEADLINE: the export returns its zero value, its defers and owned
 * locals released, and a `try` in it does not catch the interruption. */
#ifndef AETHER_EXEC_TOKEN_DEFINED
#define AETHER_EXEC_TOKEN_DEFINED
typedef struct AetherExecToken { void* cell; uint64_t serial; } AetherExecToken;
#endif
AETHER_API AetherExecToken aether_exec_token(void);
AETHER_API int aether_exec_cancel(AetherExecToken token);

/* Execution accounts (#2954): a memory allowance and a deadline that belong
 * to one logical execution, a script that yields to its host and resumes
 * later while others run on the same thread in between, not to the thread
 * or the call.
 *
 * aether_exec_account_new makes one: NULL when out of memory, or when
 * limits->parent is no live account of these accounts (one the host freed,
 * or one a copy that does not share them made: refused rather than
 * dropped, #3412, so a child is never wider than its parent);
 * aether_exec_use puts it in use on the
 * calling thread (NULL: none) and gives back the one it replaces, which the
 * host puts back when it switches scripts. While an account is in use:
 *   - every cap-aware allocation (strings, collections, JSON, the std
 *     buffers) is charged to it and to its parent chain; one that would take
 *     any of them past its memory_bytes is refused, the call ends at its next
 *     safe point with AETHER_MEMORY_CAP, and aether_exec_account_tripped has
 *     AETHER_ACCOUNT_TRIPPED_MEMORY;
 *   - an export's call has the account's deadline, the earliest on its
 *     chain and the call deadline's if that is earlier: ELAPSED counts wall
 *     time from aether_exec_account_new, so time suspended counts; ACTIVE
 *     counts only the time spent in calls under the account, once however
 *     calls nest (#3409). Past it the
 *     call ends with AETHER_DEADLINE, and the account has
 *     AETHER_ACCOUNT_TRIPPED_DEADLINE, which no later call clears.
 * A block's bytes stay charged to the account that allocated it until the
 * block is freed, wherever that happens: in a call under another account, or
 * in the host outside any call. A parent can only narrow: what a child
 * holds counts against its parent too. The process cap
 * (aether_set_memory_cap) stays the outer ceiling.
 *
 * Threads: an account is in use on one thread at a time; aether_exec_use
 * returns -1 for one in use on another thread. It may be used on another
 * thread later. Allocations made on other threads (an actor's handler) are
 * not charged to it, nor those of an actor stepped on this thread while it
 * is in use (a send's inline step). aether_exec_account_free detaches its
 * blocks, which then count only against the process cap (its ancestors are
 * credited with what they were charged for them), and puts its children
 * under its own parent (#3408).
 * Nothing is charged and nothing more is paid while no account exists.
 *
 * Ownership: an account is the host's, from new to free; the runtime keeps
 * no reference to one after free, and none to `limits` after new: freed
 * while in use on the calling thread, it is in use no more; in use on
 * another thread, nothing more is charged to it there, and that thread
 * lets go of it at its next allocation, call or aether_exec_use; an
 * execution that began under it puts no account back when it returns
 * (#3407). `previous` is written (when not NULL) and not kept.
 *
 * Runtime copies: a library built without --shared-runtime has a runtime of
 * its own, and the accounts are still one per process. Every copy works
 * through the accounts of the first loaded image that has them (the same
 * rule as the sandbox's levels), found once, at its first account call
 * or execution: one lock, one table, each thread's account in use in one
 * place, so an account is charged whichever copy allocates and is refused
 * on another thread whichever copy asks. The copy that owns them must stay
 * loaded while any other runs: unloading it first is not supported; any
 * other copy may be unloaded, and leaves them as it goes (#3411). Each copy
 * is found by a record in its image, so a program's own copy is found
 * though its executable exports nothing (Windows, Linux without -rdynamic;
 * #3445). Records are read on Windows, macOS, Linux and FreeBSD, NetBSD and
 * OpenBSD; elsewhere (Cygwin, illumos, DragonFly) copies look each other up
 * by name in the global scope, and where two do not find each other, a copy
 * refuses (-1) to put in use an account the other made.
 * Copies of different
 * layouts (another accounts version) never share: a copy that finds the
 * other's says so once on stderr, its aether_exec_account_new returns NULL
 * and its aether_exec_use -1 for an account, its allocations are charged to
 * none, and it reads nothing of the other's but the version and sizes. */
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
#ifndef AETHER_ACCOUNT_TRIPPED_DEADLINE
#define AETHER_ACCOUNT_TRIPPED_DEADLINE 1
#define AETHER_ACCOUNT_TRIPPED_MEMORY   2
#endif
AETHER_API AetherExecAccount* aether_exec_account_new(const AetherExecLimits* limits);
AETHER_API void     aether_exec_account_free(AetherExecAccount* account);
AETHER_API int      aether_exec_use(AetherExecAccount* account, AetherExecAccount** previous);
AETHER_API uint64_t aether_exec_account_used(const AetherExecAccount* account);
AETHER_API int      aether_exec_account_tripped(const AetherExecAccount* account);

#ifdef __cplusplus
}
#endif

#endif  /* LIBAETHER_H */
