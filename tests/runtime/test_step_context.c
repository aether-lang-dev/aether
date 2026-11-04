/**
 * A step runs in its own context, and gives the enclosing step's back
 * (#3389; runtime/actors/aether_step_barrier.h has the rule).
 *
 * A send to an idle actor of the same core steps it inline, inside the
 * sender's handler. What the inner step changed of the thread's per-step
 * state stayed changed for the rest of the outer handler: the in-step flag
 * and the actor id the signal handler reads were 0 and -1 after it, so a
 * fault in the rest of the outer handler was not recovered as a panic.
 *
 * A step inside a library call is not the call either (#3392, #3396):
 * the third test steps an actor inside an execution, on a core's thread.
 * Nor is it charged to the execution account in use (#2954): the fourth.
 * And what a step runs after its handler, the drops of an actor that
 * panicked or ended in it, is the step's as its handler is (#3416), and an
 * end it runs goes on past a death hook that panics (#3417): the last two.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define sc_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sc_sleep_ms(ms) usleep((ms) * 1000)
#endif

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} ScMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* tell;          // sent a payload message (stepped inline) when set
    atomic_int got;
    atomic_int in_step_after; // g_aether_in_actor_step once the send returned
    atomic_int id_after;      // g_aether_current_actor_id then
    atomic_int done;
} ScActor;

static void sc_step(ScActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);
    if (self->tell) {
        ScMsg m = { 1, 0 };
        aether_send_message(self->tell, &m, sizeof m);
        atomic_store(&self->in_step_after, g_aether_in_actor_step);
        atomic_store(&self->id_after, g_aether_current_actor_id);
        atomic_store(&self->done, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static ScActor* sc_actor(int core) {
    ScActor* a = calloc(1, sizeof(ScActor));
    static int next_id = 9700;
    a->id = next_id++;
    a->step = (void (*)(void*))sc_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

static int sc_wait(atomic_int* v, int want, int limit_ms) {
    for (int w = 0; w < limit_ms && atomic_load(v) < want; w++) sc_sleep_ms(1);
    return atomic_load(v) >= want;
}

/* A on core 0 sends to B, idle on core 0: B is stepped inside A's
 * handler. After the send A is still the actor stepping. */
TEST_CATEGORY(step_context_in_step_flags_come_back, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    ScActor* b = sc_actor(0);
    ScActor* a = sc_actor(0);
    a->tell = (ActorBase*)b;
    scheduler_start();

    ScMsg m = { 1, 0 };
    aether_send_message(a, &m, sizeof m);
    int done = sc_wait(&a->done, 1, 6000);
    int stepped = sc_wait(&b->got, 1, 2000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_TRUE(stepped);
    ASSERT_EQ(1, atomic_load(&a->in_step_after));
    ASSERT_EQ(a->id, atomic_load(&a->id_after));
    free(a);
    free(b);
}

/* #3392, #3396: an actor stepped inside a library call is not the call.
 * On core 0's thread, X's handler sets a caller, arms a deadline that has
 * passed by then and enters an execution, as a host service calling an
 * export from a handler does; inside it, it sends to Y, idle on core 0, so
 * Y is stepped inline, inside the call. Y polls as a loop head does and
 * reads the caller. Before: the poll raised the call's deadline in Y, Y's
 * barrier caught it (Y died, its loop cut), the interruption stayed marked
 * for the call, and Y read "alice". Now Y runs as on a core's thread, and
 * the call is cut at its own next safe point. */
#include "../../runtime/aether_exec.h"
#include "../../runtime/aether_host.h"
#include <string.h>

extern void aether_set_call_deadline(int64_t ms);

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* tell;
    atomic_int polled;         // Y: what its poll returned, -1 before
    atomic_int no_caller;      // Y: it read no caller identity
    atomic_int interrupting;   // X: an interruption marked after the send
    atomic_int cut_at_own;     // X: the call ended at its own safe point
    atomic_int outcome;        // X: the call's outcome
    atomic_int done;
} ExActor;

static ExActor* g_ex_x;

static void ex_call_body(void) {
    ScMsg m = { 1, 0 };
    aether_send_message(g_ex_x->tell, &m, sizeof m);
    atomic_store(&g_ex_x->interrupting, aether_exec_interrupting());
    atomic_store(&g_ex_x->cut_at_own, 1);
    (void)aether_exec_poll();   // the call's own loop head: cut here
    atomic_store(&g_ex_x->cut_at_own, 0);
}

static void ex_step(ExActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    aether_message_free_payload(&msg);
    if (self->tell) {
        g_ex_x = self;
        aether_set_caller("alice", NULL, NULL, 0, 0);
        aether_set_call_deadline(1);
        sc_sleep_ms(5);
        AetherJmpFrame* x = aether_exec_enter();
        if (AETHER_SIGSETJMP(x->buf, 1)) {
            aether_exec_failed();
        } else {
            ex_call_body();
            aether_exec_leave();
        }
        atomic_store(&self->outcome, aether_last_outcome());
        aether_set_call_deadline(0);
        aether_clear_caller();
        atomic_store(&self->done, 1);
    } else {
        atomic_store(&self->polled, aether_exec_poll());
        atomic_store(&self->no_caller, strcmp(aether_caller_identity(), "") == 0);
        atomic_store(&self->done, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static ExActor* ex_actor(int core) {
    ExActor* a = calloc(1, sizeof(ExActor));
    static int next_id = 9800;
    a->id = next_id++;
    a->step = (void (*)(void*))ex_step;
    atomic_init(&a->polled, -1);
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

TEST_CATEGORY(step_context_runs_outside_the_call, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    ExActor* y = ex_actor(0);
    ExActor* x = ex_actor(0);
    x->tell = (ActorBase*)y;
    scheduler_start();

    ScMsg m = { 1, 0 };
    aether_send_message(x, &m, sizeof m);
    int done = sc_wait(&x->done, 1, 6000);
    int stepped = sc_wait(&y->done, 1, 2000);
    int y_dead = atomic_load(&y->dead) != 0;
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_TRUE(stepped);
    ASSERT_EQ(0, atomic_load(&y->polled));        // not cut by the call's deadline
    ASSERT_FALSE(y_dead);
    ASSERT_EQ(1, atomic_load(&y->no_caller));     // no caller info of the call
    ASSERT_EQ(0, atomic_load(&x->interrupting));  // nothing left on its way
    ASSERT_EQ(1, atomic_load(&x->cut_at_own));    // the call cut at its own loop head
    ASSERT_EQ(AETHER_DEADLINE, atomic_load(&x->outcome));
    free(x);
    free(y);
}

/* #2954: an actor stepped while an execution account is in use is charged
 * nothing to it. On core 0's thread, P's handler puts an account in use
 * and sends to Q, idle on core 0, so Q is stepped inline; Q keeps a
 * 4000-byte cap-aware block. After the send P allocates 100 bytes of its
 * own, still under the account. The account holds P's 100, not Q's 4000,
 * and is in use again after Q's step. */
#include "../../runtime/aether_resource_caps.h"

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* tell;
    void* kept;                  // Q: its block
    atomic_long held;            // P: what the account holds at the end
    atomic_int in_use_after;     // P: the account was in use after the send
    atomic_int done;
} AcActor;

static void ac_step(AcActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    aether_message_free_payload(&msg);
    if (self->tell) {
        AetherExecLimits lim = { 0, 0, AETHER_DEADLINE_ELAPSED, NULL };
        AetherExecAccount* a = aether_exec_account_new(&lim);
        aether_exec_use(a, NULL);
        ScMsg m = { 1, 0 };
        aether_send_message(self->tell, &m, sizeof m);
        AetherExecAccount* now = NULL;
        void* mine = aether_caps_malloc(100);
        aether_exec_use(NULL, &now);
        atomic_store(&self->in_use_after, now == a);
        atomic_store(&self->held, (long)aether_exec_account_used(a));
        aether_caps_free(mine, 100);
        aether_exec_account_free(a);
        atomic_store(&self->done, 1);
    } else {
        self->kept = aether_caps_malloc(4000);
        atomic_store(&self->done, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static AcActor* ac_actor(int core) {
    AcActor* a = calloc(1, sizeof(AcActor));
    static int next_id = 9900;
    a->id = next_id++;
    a->step = (void (*)(void*))ac_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

TEST_CATEGORY(step_context_account_put_aside, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    AcActor* q = ac_actor(0);
    AcActor* p = ac_actor(0);
    p->tell = (ActorBase*)q;
    scheduler_start();

    ScMsg m = { 1, 0 };
    aether_send_message(p, &m, sizeof m);
    int done = sc_wait(&p->done, 1, 6000);
    int stepped = sc_wait(&q->done, 1, 2000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_TRUE(stepped);
    ASSERT_EQ(100, (int)atomic_load(&p->held));    // P's own, not Q's 4000
    ASSERT_EQ(1, atomic_load(&p->in_use_after));
    aether_caps_free(q->kept, 4000);
    free(p);
    free(q);
}

/* #3416: what a step runs after its handler is the step's, inline too. On
 * core 0's thread, D's handler puts an account in use, arms a deadline that
 * has passed by then and enters an execution, as a host service calling an
 * export from a handler does; inside it, it sends to E and to F, idle on
 * core 0, so each is stepped inline, inside the call. E queues a message to
 * itself and panics: its barrier drops the message E ran on and, finalizing
 * E, the one queued. F queues one and stops: the finalize after its handler
 * drops it. Each drop is the receiver's code (its release_message): it keeps
 * a 4000-byte cap-aware block, then polls as a loop head does. Before, the
 * drops ran in D's context, given back first: each block was charged to D's
 * account, and the poll raised D's deadline in the drop. Now they run in the
 * receiver's step, as on a core's thread: nothing charged, nothing cut, and
 * D's call is cut at its own next safe point. */
enum { DC_CALLER = 0, DC_PANICS = 1, DC_STOPS = 2 };

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int how;
    ActorBase* tell[2];
    atomic_long held;          // D: what its account holds after the sends
    atomic_int outcome;        // D: its call's outcome
    atomic_int done;
    AetherMsgInline step_copy; // the payload a handler runs on, as a generated actor's (#3534)
} DcActor;

static atomic_int g_dc_began;  // drops begun
static atomic_int g_dc_ended;  // drops that ran to their end (not cut)
static void* g_dc_kept[4];
static atomic_int g_dc_kept_n;

static void dc_release(int type, void* payload) {
    (void)type;
    (void)payload;
    atomic_fetch_add(&g_dc_began, 1);
    int i = atomic_fetch_add(&g_dc_kept_n, 1);
    void* b = aether_caps_malloc(4000);
    if (i < 4) g_dc_kept[i] = b;
    (void)aether_exec_poll();   // a loop head: the call's deadline cut it here
    atomic_fetch_add(&g_dc_ended, 1);
}

static void dc_step(DcActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    if (self->how == DC_CALLER) {
        aether_message_free_payload(&msg);
        AetherExecLimits lim = { 0, 0, AETHER_DEADLINE_ELAPSED, NULL };
        AetherExecAccount* a = aether_exec_account_new(&lim);
        aether_exec_use(a, NULL);
        aether_set_call_deadline(1);
        sc_sleep_ms(5);
        AetherJmpFrame* x = aether_exec_enter();
        if (AETHER_SIGSETJMP(x->buf, 1)) {
            aether_exec_failed();
        } else {
            ScMsg m = { 1, 0 };
            aether_send_message(self->tell[0], &m, sizeof m);
            aether_send_message(self->tell[1], &m, sizeof m);
            atomic_store(&self->held, (long)aether_exec_account_used(a));
            (void)aether_exec_poll();   // the call's own loop head: cut here
            aether_exec_leave();
        }
        atomic_store(&self->outcome, aether_last_outcome());
        aether_set_call_deadline(0);
        aether_exec_use(NULL, NULL);
        aether_exec_account_free(a);
        atomic_store(&self->done, 1);
    } else {
        ScMsg m = { 1, 0 };
        aether_send_message(self, &m, sizeof m);   // queued: its finalize drops it
        atomic_store(&self->done, 1);
        if (self->how == DC_PANICS) {
            /* The message the handler runs on, as a handler records it. */
            self->step_type = msg.type;
            self->step_payload = aether_message_take_payload((ActorBase*)self, &msg, &self->step_copy);
            aether_panic("the receiver panicked");
        }
        aether_message_free_payload(&msg);
        aether_actor_claim_end((ActorBase*)self, "normal", AETHER_ACTOR_STOPPED);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static DcActor* dc_actor(int core, int how) {
    DcActor* a = calloc(1, sizeof(DcActor));
    static int next_id = 9950;
    a->id = next_id++;
    a->how = how;
    a->step = (void (*)(void*))dc_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    /* After the registration, which leaves a caller's actor none. */
    if (how != DC_CALLER) a->release_message = dc_release;
    return a;
}

TEST_CATEGORY(step_context_drops_after_the_handler, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    DcActor* e = dc_actor(0, DC_PANICS);
    DcActor* f = dc_actor(0, DC_STOPS);
    DcActor* d = dc_actor(0, DC_CALLER);
    d->tell[0] = (ActorBase*)e;
    d->tell[1] = (ActorBase*)f;
    scheduler_start();

    ScMsg m = { 1, 0 };
    aether_send_message(d, &m, sizeof m);
    int done = sc_wait(&d->done, 1, 6000);
    int stepped = sc_wait(&e->done, 1, 2000) && sc_wait(&f->done, 1, 2000);
    int dropped = sc_wait(&g_dc_began, 3, 2000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_TRUE(stepped);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(3, atomic_load(&g_dc_began));  // E's held message and queued one, F's queued one
    ASSERT_EQ(3, atomic_load(&g_dc_ended));  // none cut by D's deadline
    ASSERT_EQ(0, (int)atomic_load(&d->held));  // none charged to D's account
    ASSERT_EQ(AETHER_DEADLINE, atomic_load(&d->outcome));  // D cut at its own loop head
    for (int i = 0; i < atomic_load(&g_dc_kept_n) && i < 4; i++) aether_caps_free(g_dc_kept[i], 4000);
    free(d);
    free(e);
    free(f);
}

/* #3417: an actor's end goes on past a death hook that panics. On core 0's
 * thread, P's handler sends to Q, idle on core 0, so Q is stepped inline; Q
 * queues a message to itself and stops, and W monitors Q. Q's finalize drops
 * the queued message: its release panics, the frame the drop runs in
 * catches it and reports it to the death hook, and the hook panics too. The
 * hook ran bare there, so its panic left the finalize halfway: the message
 * it had drained was never credited (scheduler_shutdown, which waits for
 * every message sent to be handled or dropped, hung, and the runner ended
 * the test at its limit) and W never got its Down. The hook runs in a frame
 * of its own now: the end goes on, and P's handler after the send. */
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* tell;            // P: Q
    int watcher;                // W
    atomic_int after;           // P: the send returned to its handler
    atomic_int in_step_after;   // P: g_aether_in_actor_step then
    atomic_int downs;           // W: the Downs it took
    atomic_int done;
} HkActor;

static atomic_int g_hk_hook_calls;

static void hk_release_panics(int type, void* payload) {
    (void)type;
    (void)payload;
    aether_panic("the release panicked");
}

static void hk_hook_panics(int actor_id, const char* reason) {
    (void)actor_id;
    (void)reason;
    if (atomic_fetch_add(&g_hk_hook_calls, 1) == 0) aether_panic("the death hook panicked");
}

static void hk_step(HkActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    if (msg.type == AETHER_MSG_DOWN) {
        if (aether_down_accept((ActorBase*)self, &msg)) atomic_fetch_add(&self->downs, 1);
        aether_message_drop((ActorBase*)self, &msg);
    } else {
        aether_message_free_payload(&msg);
        ScMsg m = { 1, 0 };
        if (self->tell) {
            aether_send_message(self->tell, &m, sizeof m);
            atomic_store(&self->in_step_after, g_aether_in_actor_step);
            atomic_store(&self->after, 1);
        } else if (!self->watcher) {
            aether_send_message(self, &m, sizeof m);   // queued: its finalize drops it
            aether_actor_claim_end((ActorBase*)self, "normal", AETHER_ACTOR_STOPPED);
        }
        atomic_store(&self->done, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static HkActor* hk_actor(int core, ActorBase* tell, int watcher) {
    HkActor* a = calloc(1, sizeof(HkActor));
    static int next_id = 9970;
    a->id = next_id++;
    a->tell = tell;
    a->watcher = watcher;
    a->step = (void (*)(void*))hk_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    /* After the registration, which leaves a caller's actor none. */
    if (!tell && !watcher) a->release_message = hk_release_panics;
    return a;
}

TEST_CATEGORY(step_context_end_past_a_panicking_hook, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    HkActor* q = hk_actor(0, NULL, 0);
    HkActor* p = hk_actor(0, (ActorBase*)q, 0);
    HkActor* w = hk_actor(0, NULL, 1);
    aether_step_actor_set(w);   // as from W's handler: W monitors Q
    aether_actor_monitor(q);
    aether_step_actor_set(NULL);
    atomic_store(&g_hk_hook_calls, 0);
    aether_set_on_actor_death(hk_hook_panics);
    uint64_t dead0 = scheduler_dead_sends();
    scheduler_start();

    ScMsg m = { 1, 0 };
    aether_send_message(p, &m, sizeof m);
    int done = sc_wait(&p->done, 1, 6000);
    int down = sc_wait(&w->downs, 1, 6000);
    int p_dead = atomic_load(&p->dead) != 0;
    scheduler_shutdown();   // waits for every message sent: Q's dropped one is credited
    uint64_t dead = scheduler_dead_sends() - dead0;
    scheduler_cleanup();
    aether_set_on_actor_death(NULL);

    ASSERT_TRUE(done);
    ASSERT_EQ(1, atomic_load(&q->done));
    ASSERT_EQ(1, atomic_load(&p->after));          // P's handler went on after the send
    ASSERT_EQ(1, atomic_load(&p->in_step_after));  // in its own step
    ASSERT_FALSE(p_dead);
    ASSERT_TRUE(down);                              // W was told of Q's end
    ASSERT_EQ(1, atomic_load(&w->downs));
    ASSERT_EQ(1, (int)dead);                        // Q's queued message, dropped and counted
    ASSERT_EQ(1, atomic_load(&g_hk_hook_calls));    // the release's panic, reported once
    free(p);
    free(q);
    free(w);
}
