/* aether_exec.h: an execution, one call from a host into a library export
 * until it returns (#2955, #2956).
 *
 * Every aether_<name> export stub runs its function inside an execution
 * frame:
 *
 *     AetherJmpFrame* x = aether_exec_enter();
 *     if (AETHER_SIGSETJMP(x->buf, 1)) { aether_exec_failed(); return zero; }
 *     r = f(args);
 *     if (aether_exec_leave_outcome() != 0) { release r; return zero; }
 *     return r;
 *
 * The frame is a panic frame, so a panic the export does not catch ends
 * the execution instead of the process, and it keeps what a nested
 * execution may change on this thread (the caller info a host service
 * sets, the deadline, the sandbox levels), putting it back however the
 * execution ends. Each execution leaves an outcome the caller reads with
 * aether_last_outcome() (libaether.h): a zero value with an outcome other
 * than AETHER_OK is not a result. A call that returned but ended other than
 * AETHER_OK (a stop noted after its last safe point) returns its zero value
 * too: the stub releases what it would have returned (#3388). */

#ifndef AETHER_EXEC_H
#define AETHER_EXEC_H

#include <stdint.h>
#include "actors/aether_panic.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AETHER_OK
#define AETHER_OK             0
#define AETHER_PANICKED       1
#define AETHER_DEADLINE       2
#define AETHER_CANCELLED      3
#define AETHER_MEMORY_CAP     4
#define AETHER_OUT_OF_MEMORY  5
#define AETHER_INCOMPATIBLE   6   /* made for another runtime (#2957) */
#endif

/* The deepest executions nest on one thread: each is a panic frame. */
#define AETHER_EXEC_MAX_DEPTH AETHER_PANIC_MAX_DEPTH

AetherJmpFrame* aether_exec_enter(void);
/* The execution ends: its outcome (AETHER_OK, or the stop noted for it),
 * which aether_last_outcome() then reads. aether_exec_leave is the same
 * without the result, for stubs generated before #3388. */
int aether_exec_leave_outcome(void);
void aether_exec_leave(void);
void aether_exec_failed(void);

#if AETHER_EXEC_BUILTIN_SJLJ
/* The stub's form where the compiler has __builtin_setjmp (aether_panic.h):
 * the context saved in the stub's own `void* buf[5]`, before entering, and
 * the function that jumps back to it, defined beside the stub so that the
 * same compiler and flags lay out the buffer and read it.
 *
 *     static void jump(void** b) { __builtin_longjmp(b, 1); }
 *     void* b[5];
 *     if (__builtin_setjmp(b)) { aether_exec_failed(); return zero; }
 *     aether_exec_enter_at(b, jump);
 *     r = f(args);
 *     aether_exec_leave();
 */
void aether_exec_enter_at(void** buf, void (*jump)(void** buf));
#endif

/* Executions running on this thread (0 outside any). */
int aether_exec_depth(void);
/* The executions below this depth are the ones an actor step runs inside
 * (#3392): not the running code's. 0 outside any step that put some aside. */
int aether_exec_floor(void);

/* aether_host.c keeps a copy of the caller info the execution at `depth`
 * was entered with (aether_host.h): it is put back when that one returns. */
void aether_exec_mark_view(int depth);

/* A deadline a host service sets while an execution runs: for the
 * executions it enters from there, until the running one returns. */
void aether_exec_set_next_deadline(int64_t ms);

/* The stub of an export that can neither panic, allocate, loop nor call
 * anything has no frame: it records the OK outcome only. */
void aether_exec_ok(void);

/* A safe point (#2956): every loop head of --emit=lib
 * code calls this once aether_caps_armed is set. Inside an execution whose
 * deadline passed or that was cancelled, it ends the execution (it does
 * not return); outside any execution it returns whether the deadline
 * passed, and the loop breaks, as before. */
int aether_exec_poll(void);

/* For a C-backed std operation that runs long on its input: 1 when the
 * running execution should stop, so the operation returns early, cleanly
 * (an error, its own memory freed); the next safe point, or the export's
 * return, ends the execution with the outcome. */
int aether_exec_should_stop(void);

/* A user `try` caught an interruption: it re-raises it after its own
 * cleanup, instead of running its handler. In aether_unwind.c and
 * aether_panic.c, which a program with a `try` links anyway, so it does not
 * link this file's. */
int aether_exec_interrupting(void);
void aether_exec_reraise(void);
/* A containment frame caught an interruption of the call it runs inside
 * (#3404, aether_contain): it is not the frame's to end. It is taken off
 * its way and noted as the call's stop, as a failed allocation's is: the
 * call ends with it at its next safe point, or when it returns. */
void aether_exec_defer_interruption(void);

/* The reason text of an interruption's outcome (aether_panic.c). */
const char* aether_exec_reason(int outcome);

#ifndef AETHER_EXEC_TOKEN_DEFINED
#define AETHER_EXEC_TOKEN_DEFINED
typedef struct AetherExecToken { void* cell; uint64_t serial; } AetherExecToken;
#endif
AetherExecToken aether_exec_token(void);
int aether_exec_cancel(AetherExecToken token);

int aether_last_outcome(void);
const char* aether_last_outcome_text(void);

/* The outcome of a call into a library with a runtime of its own, as the
 * importer's runtime records it, so std.host's last_outcome() sees it. */
void aether_exec_note_outcome(int outcome, const char* text);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_EXEC_H */
