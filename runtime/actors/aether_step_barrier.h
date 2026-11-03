#ifndef AETHER_STEP_BARRIER_H
#define AETHER_STEP_BARRIER_H

/* One step of an actor inside the panic barrier, shared by the multicore and
 * the cooperative scheduler (#2974).
 *
 * If the handler calls aether_panic() or takes a caught signal, the barrier
 * unwinds: the actor ends (its PANICKED bit, the panic's reason unless
 * something ended it first), the message its handler was running on and the
 * ask it held are dropped and failed, the scheduler finalizes its death
 * (drains what waits for it and tells its watchers), and the C death hook
 * fires. Otherwise, after the step: an ask the handler took and left
 * unanswered fails, a stop() or a kill that came while it ran is finalized,
 * and the scheduler's own after-step work runs.
 *
 * A step runs in its own context, whatever thread runs it and whatever
 * runs below it there (#3389). A step can run inside another: a send steps
 * an idle actor of the same core inline while the sender's handler is still
 * running, and a cooperative ask polls until its reply. The rule, for every
 * step and both of its ends:
 *
 *   - at entry, the step puts aside the per-thread context of the code it
 *     runs inside, and starts with none of it;
 *   - at exit, normal or by panic, it gives that context back as it was;
 *   - its panic, and its end, undo only what the step itself made;
 *   - what follows the handler is the step's too, and runs in its context
 *     as on the actor's own thread: the drop of the message a panicking
 *     handler ran on, the finalize of an actor the step ended (its drops
 *     are the actor's code), the death hook. Each runs in a containment
 *     frame of its own (aether_contain), so a panic there ends nothing but
 *     itself and cannot leave with the context put aside (#3403), and the
 *     context is given back once they are done (#3416);
 *   - no step runs an actor whose step is already on the stack, so none
 *     finalizes an actor whose handler is still running: the multicore
 *     scheduler steps an actor only under its step lock, and the
 *     cooperative one leaves an actor stepping out of its polls (#3391).
 *
 * The context, part by part:
 *   - the ask in hand (g_current_reply_slot) and the actor stepping
 *     (g_current_step_actor): a handler that sends, or asks, before it
 *     replies still answers its own asker. Once the handler is done there
 *     is neither, as on a core's thread after a step: what follows is no
 *     handler's;
 *   - the in-step flags (g_aether_in_actor_step, g_aether_current_actor_id):
 *     the outer handler keeps its signal recovery after an inner step;
 *   - a supervisor's restart claim, the reborn block and the spawn brackets
 *     (aether_supervision.c): a nested step's spawn takes no claimed block
 *     and is no factory's child, and its panic undoes no enclosing claim;
 *   - the sandbox levels in force on the thread, in this runtime copy and
 *     every other it shares the count with (aether_sandbox.c, #3393): a
 *     receiver stepped inside an enforce block is checked as it is on a
 *     thread of its own, against the levels it pushes itself and no other,
 *     its drops after the handler included (#3416), and the block's levels
 *     are in force again when the send returns;
 *   - the library calls (executions) running on the thread, with what they
 *     carry (aether_unwind.c, aether_exec.c, aether_host.c, #3392, #3396):
 *     an interruption on its way through them, the call's deadline and
 *     cancel, the stop a failed allocation notes for the call, and the
 *     caller info the host set for it. An actor stepped inside a call (a
 *     send's inline step, a cooperative ask's poll in an export) is not the
 *     call: its loop heads are not cut by the call's deadline, a try in it
 *     catches its own panics, its failed allocations are its own, and it
 *     reads no caller info, as on a core's thread, in its handler and in
 *     its drops after it. The call is cut at its own next safe point once
 *     the step returns;
 *   - the execution account in use (aether_resource_caps.c, #2954): an
 *     actor stepped inside code that has one in use is charged nothing to
 *     it, its drops included, as on a thread of its own; the account stays
 *     the thread's meanwhile, so no other thread takes it, and one the step
 *     left in use is let go.
 *
 * A plain step pays for none of the parts: one word says whether some
 * thread holds any of them, aether_caps_slow's step bits (the accounts
 * exist; a sandbox level may be in force where this copy's steps can see
 * it, #3419; the count of live restart claims and spawn captures,
 * aether_resource_caps.h), read once at
 * each end with one predicted branch, and only when it is set are the parts
 * looked at, out of line, each behind what says this thread holds it (for
 * the levels, the count of threads with one, through
 * aether_sandbox_in_force). The executions are behind two fields of the
 * thread's frames block, which the step's try frame is pushed on anyway
 * (their depth against the floor, and an interruption on its way); the ask
 * and the actor are two thread-local words, and the in-step flags are
 * restored from what the step read when it set them. Neither the locals
 * holding what was put aside nor the variables change between the
 * sigsetjmp and a longjmp back to it, so all of it is intact on the panic
 * path.
 *
 * The scheduler's parts are passed as functions it defines static: the
 * compiler sees them at the call and inlines the whole. The caller has
 * checked that the actor has a step and is not dead, and holds what it holds
 * to step it (a core's step lock). The step lock is not released here, on
 * either path (#2163): the caller took it and clears it.
 *
 * Where the target has no setjmp (wasi), AETHER_SIGSETJMP always takes the
 * first arm and a panic aborts the program (aether_panic.h). */

#include <stdio.h>

#include "aether_panic.h"
#include "aether_frames.h"
#include "../scheduler/multicore_scheduler.h"
#include "../utils/aether_trace.h"
#include "../aether_resource_caps.h"

extern AETHER_TLS void* g_current_reply_slot;
extern AETHER_TLS void* g_current_step_actor;

/* The sandbox's part (aether_panic.c's, which aether_sandbox.c sets when it
 * first looks for the runtime copies): the threads with a level in force,
 * and each copy's hooks, by which this thread's levels are put aside and
 * given back (aether_sandbox_hooks, aether_panic.h). */
extern atomic_int* _Atomic aether_sandbox_in_force;
/* scheduler_init joins another copy's count through this (#3438): a step
 * reads only its own copy's word, and a copy that had looked nothing up
 * stepped plainly inside another copy's levels. */
extern AETHER_COPY_LOCAL void (*_Atomic aether_sandbox_join)(void);

/* What a step put aside, to give back when it ends. `hid` has a bit for each
 * part put aside (AETHER_STEP_HID_*, aether_frames.h, where aether_step_push
 * sets the executions'); the rest is read only under its bit. */
_Static_assert((AETHER_STEP_HID_EXECS & (AETHER_STEP_HID_SPAWN | AETHER_STEP_HID_ACCOUNT |
                                         AETHER_STEP_HID_SANDBOX)) == 0 &&
               (AETHER_STEP_HID_SPAWN & (AETHER_STEP_HID_ACCOUNT | AETHER_STEP_HID_SANDBOX)) == 0 &&
               (AETHER_STEP_HID_ACCOUNT & AETHER_STEP_HID_SANDBOX) == 0,
               "the step's part bits are distinct");
typedef struct {
    void* reply_slot;
    void* step_actor;
    int in_step;
    int actor_id;
    int hid;
    AetherSpawnAside spawn;
    /* The copies of the sandbox that put levels aside, and what each kept
     * (#3438): every copy registered with this barrier hides its own. */
    int sandbox_n;
    const AetherSandboxHooks* sandbox_hooks[AETHER_STEP_SANDBOX_COPIES];
    int sandbox[AETHER_STEP_SANDBOX_COPIES][2];
    AetherExecAside execs;
    struct AetherExecAccount* account;
} AetherStepAside;

#if defined(__GNUC__) || defined(__clang__)
#define AETHER_STEP_COLD __attribute__((cold, noinline))
#else
#define AETHER_STEP_COLD
#endif

/* The parts some thread holds, as the caps word's step bits say: each put
 * aside when this thread holds it. */
static AETHER_STEP_COLD void aether_step_put_aside_parts(AetherStepAside* s, unsigned word) {
    if ((word & AETHER_CAPS_SPAWN_MASK) && aether_spawn_put_aside(&s->spawn))
        s->hid |= AETHER_STEP_HID_SPAWN;
    if (word & AETHER_CAPS_SLOW_ACCOUNTS) {
        s->account = aether_account_step_aside();
        s->hid |= AETHER_STEP_HID_ACCOUNT;
    }
    if (word & AETHER_CAPS_SLOW_SANDBOX) {
        atomic_int* in_force = atomic_load_explicit(&aether_sandbox_in_force, memory_order_acquire);
        if (in_force != NULL && atomic_load_explicit(in_force, memory_order_relaxed) != 0) {
            int n = 0;
            for (int i = 0; i < AETHER_SANDBOX_HOOKS_MAX && n < AETHER_STEP_SANDBOX_COPIES; i++) {
                const AetherSandboxHooks* h = atomic_load_explicit(&aether_sandbox_hooks[i], memory_order_acquire);
                if (h && h->hide && h->show && h->hide(s->sandbox[n])) s->sandbox_hooks[n++] = h;
            }
            s->sandbox_n = n;
            if (n) s->hid |= AETHER_STEP_HID_SANDBOX;
        }
    }
}

static inline void aether_step_put_aside(AetherStepAside* s) {
    s->reply_slot = g_current_reply_slot;
    s->step_actor = g_current_step_actor;
    g_current_reply_slot = NULL;
    s->in_step = g_aether_in_actor_step;
    s->actor_id = s->in_step ? g_aether_current_actor_id : -1;
    s->hid = 0;
    /* One word, one predicted branch: 0 on a plain step. */
    unsigned word = atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_STEP_BITS;
    if (unlikely(word != 0)) aether_step_put_aside_parts(s, word);
}

static AETHER_STEP_COLD void aether_step_give_back_parts(const AetherStepAside* s) {
    if (s->hid & AETHER_STEP_HID_EXECS) aether_step_give_back_execs(&s->execs);
    if (s->hid & AETHER_STEP_HID_SANDBOX)   /* the last to hide gives back first */
        for (int i = s->sandbox_n - 1; i >= 0; i--) s->sandbox_hooks[i]->show(s->sandbox[i]);
    /* Accounts made while the step ran, and one it left in use, count too. */
    if ((s->hid & AETHER_STEP_HID_ACCOUNT) ||
        (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_SLOW_ACCOUNTS))
        aether_account_step_back((s->hid & AETHER_STEP_HID_ACCOUNT) ? s->account : NULL);
    if (s->hid & AETHER_STEP_HID_SPAWN) aether_spawn_give_back(&s->spawn);
}

static inline void aether_step_give_back(const AetherStepAside* s) {
    /* What it put aside, and an account made while it ran: one word again. */
    if (unlikely((unsigned)s->hid |
                 (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_SLOW_ACCOUNTS)))
        aether_step_give_back_parts(s);
    g_aether_in_actor_step = s->in_step;
    g_aether_current_actor_id = s->actor_id;
    g_current_reply_slot = s->reply_slot;
    g_current_step_actor = s->step_actor;
}

/* The finalize of an actor its step ended, in the step's context and in a
 * containment frame of its own (#3416). Its drops are the actor's code, each
 * contained already with the death hook a drop's panic is reported to
 * (aether_message_drop_at_end, #3417), so nothing it runs leaves it; the
 * frame stands for a panic a later change would let out, which is then
 * reported on stderr and never leaves with the context put aside. */
typedef struct {
    ActorBase* actor;
    void (*finalize)(ActorBase*);
} AetherStepFinalize;

static inline void aether_step_finalize_body(void* p) {
    AetherStepFinalize* c = (AetherStepFinalize*)p;
    c->finalize(c->actor);
}

static inline void aether_step_finalize(ActorBase* actor, void (*finalize)(ActorBase*)) {
    AetherStepFinalize c = { actor, finalize };
    const char* r = NULL;
    void (*rel)(const void*) = NULL;
    if (aether_contain(aether_step_finalize_body, &c, &r, &rel) == 1) {
        fprintf(stderr, "aether: the end of actor %d panicked: %s\n", actor->id,
                aether_panic_reason_text(r));
        if (rel) rel(r);
    }
}

static inline void aether_step_barrier(ActorBase* actor,
                                       void (*finalize)(ActorBase*),
                                       void (*fail_slot)(void* slot, int why),
                                       void (*after_step)(ActorBase*)) {
    AetherStepAside aside;
    aether_step_put_aside(&aside);

    /* The try frame, with the executions running on the thread put aside
     * in the same look at its block (aether_unwind.c). */
    AetherJmpFrame* f = aether_step_push(&aside.execs, &aside.hid);
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        g_aether_in_actor_step = 1;
        g_aether_current_actor_id = actor->id;
        AETHER_TRACE_EVENT(AE_TRACE_STEP_BEGIN, actor->id, -1, -1);
        actor->step(actor);
        AETHER_TRACE_EVENT(AE_TRACE_STEP_END, actor->id, -1, -1);
        g_aether_in_actor_step = 0;
        g_aether_current_actor_id = -1;
        aether_try_pop();
        /* The handler is done; what follows is still the step's, in its
         * context, and gives the context back last (#3416). */
        void* unanswered = g_current_reply_slot;
        g_current_reply_slot = NULL;
        /* A handler that took an ask and returned without replying: no
         * reply can come, since a reply answers the ask its own step took.
         * Its asker learns that now, not at its timeout. */
        if (unanswered) fail_slot(unanswered, AETHER_ASK_NO_REPLY);
        /* Ended while the handler ran (#2974): its stop(), or a kill or a
         * linked actor's death from elsewhere, takes effect now that the
         * handler has returned, so no Down is sent while it runs. Its drops
         * are the actor's code, run as on its own thread, where no handler
         * is running; the finalize is contained, so nothing leaves it with
         * the context put aside. A release ends it by its own path. */
        int ended = atomic_load_explicit(&actor->dead, memory_order_acquire);
        if (unlikely(ended != 0)) {
            if (!(ended & AETHER_ACTOR_RELEASED)) {
                g_current_step_actor = NULL;
                aether_step_finalize(actor, finalize);
            }
        } else if (after_step) {
            after_step(actor);
        }
        aether_step_give_back(&aside);
        return;
    }

    // Unwound via panic or signal.
    const char* reason = f->reason;
    void (*reason_release)(const void*) = f->reason_release;
    g_aether_in_actor_step = 0;
    g_aether_current_actor_id = -1;
    aether_try_pop();

    // A bit, not a store: a release may be setting its own bit meanwhile.
    // The reason is the panic's unless something ended the actor first.
    aether_actor_claim_end(actor, aether_panic_reason_text(reason), AETHER_ACTOR_PANICKED);
    atomic_store_explicit(&actor->active, 0, memory_order_release);

    /* What the step made of the thread's own is taken, before the context
     * it ran inside is given back: the message its handler was running on,
     * the ask it was handling, and a restart in place it had begun, which is
     * undone (#2974), with the spawn brackets it opened: its own, never an
     * enclosing step's, which were put aside (#3389). */
    Message held = {0};
    if (actor->step_payload) {
        held.type = actor->step_type;
        if (actor->step_inline) {
            /* #3534: the actor's own copy of a payload that travelled in
             * its message: copied back into one, never freed. */
            memcpy(aether_message_inline_bytes(&held), actor->step_payload,
                   AETHER_MSG_INLINE_BYTES);
            held.zerocopy.owned = AETHER_MSG_INLINE;
        } else {
            held.payload_ptr = actor->step_payload;
        }
        actor->step_payload = NULL;
    }
    void* unanswered = g_current_reply_slot;
    g_current_reply_slot = NULL;
    aether_spawn_step_panicked();
    /* The step is over, and this frame is gone. What follows runs program
     * code (drop functions, the host's death hook) that may panic: the
     * actor's, so in the step's context, as on its own thread, where no
     * handler is running (#3416); each piece in a containment frame of its
     * own, so none ends anything but itself or leaves with the context put
     * aside (#3403). The context is given back after them. */
    g_current_step_actor = NULL;

    /* The message (#3142): the release that follows the handler did not
     * run, so what the message owns and its payload go here, as a dropped
     * message's do; a drop function that panics is reported as the handler's
     * panic is. */
    if (held.payload_ptr || aether_message_is_inline(&held)) {
        const char* why = NULL;
        void (*why_release)(const void*) = NULL;
        if (aether_message_drop_contained(actor, &held, &why, &why_release)) {
            aether_fire_death_hook_contained(actor->id, why);
            if (why_release) why_release(why);
        }
    }

    /* The ask, if it had not replied (#3151): its asker learns now that no
     * reply is coming, instead of at its timeout. */
    if (unanswered) fail_slot(unanswered, AETHER_ASK_DIED);

    // What waits for it is dropped and credited, and its watchers told.
    aether_step_finalize(actor, finalize);

    aether_fire_death_hook_contained(actor->id, reason);
    /* A heap-built panic message (aether_panic_owned) belongs to whoever
     * caught it, here this barrier. The hook has seen it; release it. */
    if (reason_release) reason_release(reason);
    aether_step_give_back(&aside);
}

#endif /* AETHER_STEP_BARRIER_H */
