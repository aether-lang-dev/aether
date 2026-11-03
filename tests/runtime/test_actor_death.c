/**
 * The end of an actor (#2974, #3298).
 *
 * An actor ends once: a panic, a kill (actors.kill), a stop (actors.stop) or
 * a release. Its death is finalized on the thread that owns its mailbox while
 * it steps nothing: what waits for it is dropped and counted
 * (scheduler_dead_sends), and the actors that monitor it get a Down. So a
 * Down never comes while its handler still runs, and comes after the last
 * messages it sent the watcher; an ask its killed handler is waiting in ends
 * at once; and an actor with a thread of its own that panics with messages
 * still in its inbox gives them up, so the shutdown's wait for every message
 * sent ends (#3298).
 *
 * The cases order their events with flags and with a sender's own FIFO,
 * never with sleeps: a probe sent after a kill is handled after a Down the
 * kill itself would have sent.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/actors/aether_actor_thread.h"
#include "../../runtime/actors/aether_actor_inbox.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define death_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define death_sleep_ms(ms) usleep((ms) * 1000)
#endif

enum {
    D_PING = 1,     // counted
    D_HOLD = 2,     // waits for `release`, after setting `entered`
    D_WATCH = 3,    // monitors `watch`
    D_PROBE = 4,    // records the Downs seen so far
    D_ASK = 5,      // asks `ask` with a HOLD and records the status
    D_LAST = 6,     // a message the target sends its watcher before it dies
    D_BOOM = 7      // sends `to` a LAST, then panics
};

typedef struct {
    int type;
    int value;
} DeathMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* monitored;
    ActorBase* ask;
    ActorBase* to;
    int panic_first;          // the first message panics
    atomic_int got;
    atomic_int entered;
    atomic_int release;
    atomic_int downs;
    atomic_int probed;        // downs when the PROBE came
    atomic_int probe_done;
    atomic_int ask_status;
    atomic_int ask_done;
    atomic_int ask_saw_release;
    atomic_int lasts;
    atomic_int last_before_down;
    char down_reason[64];
} DeathActor;

static int death_wait(atomic_int* v, int want, int limit_ms) {
    uint64_t until = aether_now_ns() + (uint64_t)limit_ms * 1000000ULL;
    while (atomic_load(v) < want && aether_now_ns() < until) death_sleep_ms(1);
    return atomic_load(v) >= want;
}

static void death_hold(DeathActor* self) {
    atomic_store(&self->entered, 1);
    uint64_t until = aether_now_ns() + 5000ULL * 1000000ULL;
    while (!atomic_load(&self->release) && aether_now_ns() < until) death_sleep_ms(1);
}

static void death_send(void* to, int type, int value) {
    DeathMsg m = { type, value };
    aether_send_message(to, &m, sizeof m);
}

static void death_step(DeathActor* self) {
    aether_step_actor_set(self);   // as a generated step does: `self` for actors.*
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    if (msg.type == AETHER_MSG_DOWN) {
        if (aether_down_accept((ActorBase*)self, &msg)) {
            AetherDownMsg* d = (AetherDownMsg*)aether_message_payload(&msg);
            const char* text = aether_panic_reason_text(d->reason);
            strncpy(self->down_reason, text, sizeof self->down_reason - 1);
            atomic_fetch_add(&self->downs, 1);
        }
        aether_message_drop((ActorBase*)self, &msg);
        return;
    }
    DeathMsg m = *(DeathMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);
    if (self->panic_first) aether_panic("first message");
    switch (m.type) {
    case D_HOLD: {
        death_hold(self);
        int value = 1;
        scheduler_reply((ActorBase*)self, &value, sizeof value);
        break;
    }
    case D_WATCH:
        aether_actor_monitor(self->monitored);
        break;
    case D_PROBE:
        atomic_store(&self->probed, atomic_load(&self->downs));
        atomic_store(&self->probe_done, 1);
        break;
    case D_ASK: {
        DeathMsg q = { D_HOLD, 0 };
        int status = -1;
        int* r = (int*)scheduler_ask_message_status(self->ask, &q, sizeof q, 5000, &status);
        free(r);
        atomic_store(&self->ask_saw_release, atomic_load(&((DeathActor*)self->ask)->release));
        atomic_store(&self->ask_status, status);
        atomic_store(&self->ask_done, 1);
        break;
    }
    case D_LAST:
        if (atomic_load(&self->downs) == 0) atomic_store(&self->last_before_down, 1);
        atomic_fetch_add(&self->lasts, 1);
        break;
    case D_BOOM:
        death_send(self->to, D_LAST, 0);
        aether_panic("boom");
        break;
    default:
        break;
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static DeathActor* death_actor(int core) {
    DeathActor* a = (DeathActor*)scheduler_spawn_actor(core, (void (*)(void*))death_step,
                                                       sizeof(DeathActor));
    memset((char*)a + sizeof(ActorBase), 0, sizeof(DeathActor) - sizeof(ActorBase));
    return a;
}

/* A kill of an actor in the middle of a handler: no Down while the handler
 * runs (a probe the killer sends the watcher after the kill sees none), one
 * Down, "killed", once it returns; what was queued for it is dropped and
 * counted. */
static void test_kill_mid_handler(void) {
    scheduler_init(2);
    scheduler_start();
    DeathActor* t = death_actor(0);
    DeathActor* w = death_actor(1);
    w->monitored = (ActorBase*)t;
    death_send(w, D_WATCH, 0);
    int watching = death_wait(&w->got, 1, 3000);
    uint64_t dead_before = scheduler_dead_sends();

    death_send(t, D_HOLD, 0);
    int entered = death_wait(&t->entered, 1, 3000);
    for (int i = 0; i < 5; i++) death_send(t, D_PING, i);   // queued behind the HOLD
    aether_actor_kill(t);
    death_send(w, D_PROBE, 0);
    int probed = death_wait(&w->probe_done, 1, 3000);
    int downs_while_running = atomic_load(&w->probed);
    atomic_store(&t->release, 1);
    int down = death_wait(&w->downs, 1, 3000);
    scheduler_wait();
    uint64_t dropped = scheduler_dead_sends() - dead_before;
    int t_got = atomic_load(&t->got);
    char reason[64];
    memcpy(reason, w->down_reason, sizeof reason);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(watching);
    ASSERT_TRUE(entered);
    ASSERT_TRUE(probed);
    ASSERT_EQ(0, downs_while_running);
    ASSERT_TRUE(down);
    ASSERT_TRUE(strcmp(reason, "killed") == 0);
    ASSERT_EQ(1, t_got);          // only the HOLD
    ASSERT_EQ(5, (int)dropped);   // the five queued behind it
}

/* An actor killed while its handler waits in an ask: the ask ends at once,
 * with AETHER_ASK_KILLED, before the asked actor answers. */
static void test_kill_ends_ask(void) {
    scheduler_init(2);
    scheduler_start();
    DeathActor* b = death_actor(1);
    DeathActor* a = death_actor(0);
    a->ask = (ActorBase*)b;
    death_send(a, D_ASK, 0);
    int asked = death_wait(&b->entered, 1, 3000);
    aether_actor_kill(a);
    int returned = death_wait(&a->ask_done, 1, 3000);
    int status = atomic_load(&a->ask_status);
    int saw_release = atomic_load(&a->ask_saw_release);
    atomic_store(&b->release, 1);
    scheduler_wait();
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(asked);
    ASSERT_TRUE(returned);
    ASSERT_EQ(AETHER_ASK_KILLED, status);
    ASSERT_EQ(0, saw_release);
}

/* What a dying actor sent its watcher before it panicked arrives before its
 * Down, which carries the panic's reason. */
static void test_down_after_last_messages(void) {
    scheduler_init(2);
    scheduler_start();
    DeathActor* t = death_actor(0);
    DeathActor* w = death_actor(1);
    w->monitored = (ActorBase*)t;
    t->to = (ActorBase*)w;
    death_send(w, D_WATCH, 0);
    int watching = death_wait(&w->got, 1, 3000);
    death_send(t, D_BOOM, 0);
    int down = death_wait(&w->downs, 1, 3000);
    scheduler_wait();
    int before = atomic_load(&w->last_before_down);
    char reason[64];
    memcpy(reason, w->down_reason, sizeof reason);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(watching);
    ASSERT_TRUE(down);
    ASSERT_EQ(1, before);
    ASSERT_TRUE(strcmp(reason, "boom") == 0);
}

/* #3399: a kill after scheduler_init and before scheduler_start (a C host
 * that starts later, or never) is finalized at once: no core thread is
 * there to take a request. It was posted to the cores as if they ran,
 * since init already says initialized, so the death waited for a start:
 * the actor stayed unfinalized, and an actor linked to it lived on. The
 * watcher's Down is sent at the kill and handled once the cores run. */
static void test_kill_before_start(void) {
    scheduler_init(2);
    DeathActor* t = death_actor(0);
    DeathActor* w = death_actor(1);
    DeathActor* l = death_actor(0);   // linked to t, does not trap exits
    aether_step_actor_set(w);         // as from w's handler
    aether_actor_monitor(t);
    aether_step_actor_set(l);
    aether_actor_link(t);
    aether_step_actor_set(NULL);
    aether_actor_kill(t);
    int t_final = (atomic_load(&t->exit_flags) & AETHER_EXIT_FINALIZED) != 0;
    int l_ended = (atomic_load(&l->dead) & AETHER_ACTOR_KILLED) &&
                  (atomic_load(&l->exit_flags) & AETHER_EXIT_FINALIZED);
    scheduler_start();
    int down = death_wait(&w->downs, 1, 3000);
    scheduler_wait();
    char reason[64];
    memcpy(reason, w->down_reason, sizeof reason);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(t_final);
    ASSERT_TRUE(l_ended);
    ASSERT_TRUE(down);
    ASSERT_TRUE(strcmp(reason, "killed") == 0);
}

/* #3400: a panic that leaves a link's drain leaves the thread's later
 * links working. Where a death is finalized as soon as it is seen (here the
 * threaded scheduler, stopped), the ends links pass on are made from a
 * queue; a panic that jumped past it left the thread marked as ending, so
 * every end a link passed on there afterwards was queued and never made.
 * a's death ends b through b's link and b's ends c; c holds a message whose
 * release panics out of the drain; then x's death must still end y.
 *
 * No real panic leaves a drain now: a dropped message's release runs in a
 * frame of its own (#3401), and so does the death hook it reports a panic
 * to (#3417), which was this test's way out until then. The drain still
 * journals its queue (link_end_unwind) for a panic a later change would let
 * through, and the release here stands for one: it raises its panic for
 * the test's own frame, past the frame its drop runs in. A bare longjmp to
 * the test's frame would not do: it would leave that frame pushed above the
 * test's and skip the unwind of the test's depth, the journal's part. So
 * the release leaves the frames above the test's as a path without them
 * would never have entered them, and panics: the unwind then runs the
 * drain's journal entry and jumps to the test's frame, as an escaped panic
 * does. The drop frees the payload after the release, which this one never
 * returns to, so it frees it itself. */
static int g_release_escapes = 0;
static AetherJmpFrame* g_escape_to = NULL;

static void release_that_escapes(int type, void* payload) {
    (void)type;
    if (g_release_escapes) {
        g_release_escapes = 0;
        free(payload);
        while (aether_current_frame() && aether_current_frame() != g_escape_to) aether_try_pop();
        aether_panic("a release's panic got out of its frame");
    }
}

static void test_link_end_after_panic(void) {
    scheduler_init(1);
    DeathActor* a = death_actor(0);
    DeathActor* b = death_actor(0);
    DeathActor* c = death_actor(0);
    DeathActor* x = death_actor(0);
    DeathActor* y = death_actor(0);
    aether_step_actor_set(b);   // as from b's handler: b links a
    aether_actor_link(a);
    aether_step_actor_set(c);
    aether_actor_link(b);
    aether_step_actor_set(y);
    aether_actor_link(x);
    aether_step_actor_set(NULL);
    scheduler_shutdown();
    c->release_message = release_that_escapes;
    Message m;
    memset(&m, 0, sizeof m);
    m.type = 9;
    m.payload_ptr = malloc(16);
    int queued = mailbox_send(&c->mailbox, m);
    g_release_escapes = 1;
    const char* volatile caught = NULL;
    AetherJmpFrame* f = aether_try_push();
    g_escape_to = f;
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_actor_kill(a);
    } else {
        caught = f->reason;
    }
    aether_try_pop();
    g_escape_to = NULL;
    int escaped = caught && strcmp(aether_panic_reason_text(caught), "a release's panic got out of its frame") == 0;
    int b_ended = (atomic_load(&b->dead) & AETHER_ACTOR_KILLED) != 0;
    aether_actor_kill(x);
    int y_ended = (atomic_load(&y->dead) & AETHER_ACTOR_KILLED) &&
                  (atomic_load(&y->exit_flags) & AETHER_EXIT_FINALIZED);
    scheduler_cleanup();

    ASSERT_TRUE(queued);
    ASSERT_TRUE(escaped);
    ASSERT_TRUE(b_ended);
    ASSERT_TRUE(y_ended);
}

/* #3298: an actor with a thread of its own panics on its first message with
 * 200 more in its inbox. Its thread took 32 into its mailbox; the rest waited
 * in the inbox, never credited, and the shutdown's wait for every message
 * sent never ended. */
static void test_actor_thread_panic_drains_inbox(void) {
    scheduler_init(2);
    scheduler_start();
    DeathActor* a = death_actor(0);
    a->panic_first = 1;
    a->auto_process = 1;
    for (int i = 0; i < 200; i++) death_send(a, D_PING, i);
    int started = aether_actor_thread_start(&a->thread, a) == 0;
    uint64_t until = aether_now_ns() + 3000ULL * 1000000ULL;
    while (!(atomic_load(&a->dead) & AETHER_ACTOR_THREAD_GONE) && aether_now_ns() < until)
        death_sleep_ms(1);
    ActorInbox* ib = atomic_load(&a->inbox);
    int left = ib ? atomic_load(&ib->count) : 0;
    int gone = (atomic_load(&a->dead) & AETHER_ACTOR_THREAD_GONE) != 0;
    scheduler_shutdown();   // waits for every message sent to be handled or dropped
    scheduler_cleanup();

    ASSERT_TRUE(started);
    ASSERT_TRUE(gone);
    ASSERT_EQ(0, left);
}

void register_actor_death_tests(void) {
    register_test_with_category("Death: a kill mid-handler, Down after it returns",
        test_kill_mid_handler, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Death: a killed asker's ask ends at once",
        test_kill_ends_ask, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Death: a Down comes after the last messages",
        test_down_after_last_messages, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Death: an actor thread's panic drops its inbox (#3298)",
        test_actor_thread_panic_drains_inbox, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Death: a kill before scheduler_start is finalized at once (#3399)",
        test_kill_before_start, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Death: a panic out of a link's end leaves later links working (#3400)",
        test_link_end_after_panic, TEST_CATEGORY_RUNTIME);
}
