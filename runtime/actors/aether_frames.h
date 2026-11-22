/* aether_frames.h: the per-thread frames a panic unwinds to, shared by the
 * runtime files that keep them (aether_panic.c, aether_unwind.c,
 * aether_exec.c). Runtime-internal: generated code never names this
 * thread-local (a Windows DLL cannot import one); it reaches the frames only
 * through the functions in aether_panic.h and aether_exec.h.
 *
 * One block per thread, so a try frame or an execution frame costs one
 * thread-local lookup, not one per file: the frames, the executions among
 * them (#2955, #2956), and the depth the allocation journal tags entries
 * with, which is the frame depth itself. */

#ifndef AETHER_FRAMES_H
#define AETHER_FRAMES_H

#include <stdint.h>
#include "aether_panic.h"

/* What an execution keeps to give back when it returns (aether_exec.c). */
typedef struct {
    int64_t deadline_at_ns;    /* the deadline in force when it was entered */
    int     deadline_tripped;  /*   (a nested execution's only) */
    int64_t next_at_ns;        /* the deadline a host service it called set */
    int     next_tripped;      /*   for the executions entered from there */
    int     next_set;
    int     sandbox_depth;     /* this thread's sandbox levels at entry */
    int     has_view;          /* aether_host.c kept its caller info */
    int     stop;              /* why a C-backed std loop stopped early */
    uint64_t serial;           /* this thread's count of executions, its token */
    struct AetherExecAccount* account;  /* the account in use when it began (#2954) */
    int     account_timed;              /*   the ACTIVE accounts it began timing (#3409) */
    int     account_deadline;           /* it took its account's deadline */
} AetherExecRecord;

/* Where another thread asks for an execution of this one to stop
 * (aether_exec_cancel): the serial of the execution to cancel, 0 for none.
 * Made the first time a token is asked for on a thread and kept for the
 * process, so a token never points at freed memory. */
typedef struct AetherCancelCell {
    _Atomic uint64_t pending;
    struct AetherCancelCell* next;
    /* A cooperative build's task contexts (aether_exec.c): a context that is
     * detached gives its cell back, free, with the serial it reached, and the
     * next context attached takes it and starts counting above that, so a
     * token of the old context names no execution of the new one. */
    _Atomic int unowned;
    uint64_t serial_floor;
} AetherCancelCell;

#define AETHER_OUTCOME_TEXT_MAX 1024

struct AetherUnwindEntry;   /* aether_unwind.c's */
struct AetherCallerState;   /* aether_host.c's */
struct AetherCtxLocal;      /* aether_unwind.c's */

/* An ACTIVE account a thread is timing (aether_resource_caps.c, #3409): when
 * this thread began timing it. The first AETHER_ACCT_TIMED_INLINE are kept
 * in the block, the rest in a block that grows as needed. */
#define AETHER_ACCT_TIMED_INLINE 8
typedef struct {
    struct AetherExecAccount* a;
    int64_t since;
} AetherAcctTimed;

/* The execution state of one thread (of one task, on a cooperative build
 * with task contexts: docs/design/task-execution-contexts.md). Everything a
 * call into Aether reads or changes about how it runs, rather than what it
 * computes, is here: its try and execution frames, the executions' records
 * and outcome, the cancel cell, the account in use and the accounts being
 * timed, the deadline, the caller info, the allocation journal, the
 * teardown queue, the fn-pointer ownership mark and the last refusing
 * callback's reason. The heap parts (the journal, the teardown queue, the
 * timing list past its inline entries, the caller info, the callback
 * reason) are made on first use and given back by aether_frames_release
 * when the thread ends or the task's context is detached. */
typedef struct AetherFrames {
    AetherJmpFrame   frames[AETHER_PANIC_MAX_DEPTH];
    int              depth;    /* live frames; the innermost is frames[depth-1] */
    AetherExecRecord exec[AETHER_PANIC_MAX_DEPTH];
    int              exec_depth;
    /* The executions below are the code's an actor step runs inside
     * (#3392): put aside for the step, which runs outside every one of them
     * (aether_step_put_aside_execs below). Only those above it run here. */
    int              exec_floor;
    int              outcome;  /* the last execution's, aether_last_outcome() */
    char             outcome_text[AETHER_OUTCOME_TEXT_MAX];
    int              interrupting; /* the outcome an interruption on its way carries */
    uint64_t         serial;   /* executions entered on this thread */
    AetherCancelCell* cell;
    struct AetherExecAccount* account; /* in use on this thread (aether_exec_use, #2954) */
    /* The allocation journal (aether_unwind.c): its entries are tagged with
     * the frame depth they were made at. */
    struct AetherUnwindEntry* journal;
    int              journal_count;
    int              journal_capacity;
    /* The deadline of this thread's calls (aether_resource_caps.c):
     * monotonic-clock ns, 0 for none, and the sticky trip flag. */
    int64_t          deadline_at_ns;
    int              deadline_tripped;
    /* aether_test_fail_alloc_after's count on this thread. */
    int              alloc_fail_after;
    /* The ACTIVE accounts this thread is timing, innermost last. */
    AetherAcctTimed  timed_inline[AETHER_ACCT_TIMED_INLINE];
    AetherAcctTimed* timed_more;
    int              timed_more_cap;
    int              timed_n;
    /* The caller info (aether_host.c), made at the first aether_set_caller. */
    struct AetherCallerState* caller;
    /* Why this thread's last refusing callback refused (aether_callbacks.c),
     * made at the first one; NULL reads as "". */
    char*            callback_reason;
    /* The string a fn-pointer callee handed over (aether_fnptr_mark). */
    const void*      fnptr_owned;
    /* Actors torn down while a teardown runs (aether_teardown). */
    void**           teardown_queue;
    int              teardown_count;
    int              teardown_capacity;
    int              teardown_running;
    /* Context-local blocks (aether_context_local), newest first. */
    struct AetherCtxLocal* locals;
} AetherFrames;

/* The heap parts of `F` given back (the thread ended, or its task's
 * context is detached): no execution or frame may be open in it. */
void aether_frames_release(AetherFrames* F);

/* The calling thread's heap parts are given back when it ends: called by a
 * file that gives its block a heap part, the first time it does. */
void aether_frames_mark_thread(void);

#if defined(AETHER_NO_THREADING)
/* Task contexts (aether_unwind.c, aether_exec.c; libaether.h has the host
 * API and docs/design/task-execution-contexts.md the contract). */
struct AetherExecContext;
struct AetherExecContext* aether_context_builtin(void);
AetherFrames* aether_context_frames(struct AetherExecContext* ctx);
size_t aether_context_size(void);
/* 1 when the calling task may run actors: no port is installed, or it holds
 * the built-in context. */
int aether_context_owns_actors(void);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void aether_context_wrong_task(const char* what);
/* Wait a little for a lock another task holds: the port's relax, or a pause. */
void aether_context_relax(void);
/* The actors' own entry points stop a task that does not own them. */
#define AETHER_ACTORS_TASK_CHECK(what) \
    do { if (__builtin_expect(!aether_context_owns_actors(), 0)) aether_context_wrong_task(what); } while (0)
#endif

/* This thread's block. The block is a `static` thread-local of
 * aether_unwind.c, the one file that owns it (with the try frames' push and
 * pop and the journal, the paths every step and every try takes); every
 * other file reaches it through this function and never names the
 * thread-local.
 *
 * The rule for the runtime's thread-locals: an extern thread-local is a
 * scalar written whole; an aggregate stays static to its file, reached by
 * accessor. Generated code goes further and names none (a Windows DLL cannot
 * import a thread-local). An extern aggregate was miscompiled by GCC 16.2
 * for x86_64-w64-mingw32 at -O2: two adjacent stores into it, merged into
 * one, were addressed through the variable's non-thread address and landed
 * in the thread-local template, not the calling thread's block. A scalar
 * read or written whole (an int, a pointer: current_core_id,
 * g_current_step_actor) has no adjacent store to merge with. The send buffer
 * (aether_send_buffer.c) follows the same rule. */
AetherFrames* aether_frames_here(void);

/* What an actor step puts aside of the executions it runs inside (#3392,
 * #3396; the rule is aether_step_barrier.h's): the floor below which the
 * executions are not the step's, an interruption on its way through them,
 * and, through aether_exec.c, the call's deadline and its caller info. */
typedef struct {
    int     exec_floor;
    int     interrupting;
    int     execs_part;     /* aether_exec.c took its part (the hook was set) */
    int64_t deadline_at_ns;
    int     deadline_tripped;
    void*   caller;         /* aether_host.c's: the setting, put away */
} AetherExecAside;

/* Set by aether_exec.c at its first execution: puts aside (hide 1) or gives
 * back (hide 0) the deadline and the caller info, which live in files only
 * a library call links. */
extern void (*_Atomic aether_step_execs_hook)(AetherExecAside* s, int hide);

/* An actor step begins and ends (aether_step_barrier.h): the executions
 * running on this thread are put aside, AETHER_STEP_HID_EXECS set in *hid
 * when there were any (or an interruption on its way), and given back. The
 * step's try frame is pushed with them, in one look at this thread's block.
 * The step's other parts have the other bits of *hid. */
#define AETHER_STEP_HID_EXECS   1
#define AETHER_STEP_HID_SPAWN   2
#define AETHER_STEP_HID_ACCOUNT 4
#define AETHER_STEP_HID_SANDBOX 8
AetherJmpFrame* aether_step_push(AetherExecAside* s, int* hid);
void aether_step_give_back_execs(const AetherExecAside* s);

#endif /* AETHER_FRAMES_H */
