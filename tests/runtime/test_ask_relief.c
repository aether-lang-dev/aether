/**
 * A core whose thread waits in an ask keeps its work moving (#3197).
 *
 * An ask made in a handler blocks that core's scheduler thread until the
 * reply. Its queued messages were not delivered and its other actors not
 * stepped meanwhile, so an actor elsewhere that, answering, asked one of
 * them waited out its timeout, and so did every actor of the core. The
 * waiting thread now relieves its core: it delivers what is queued for its
 * actors and moves those with work to a core that runs them (the core
 * waiting on that actor, or an idle one). An actor with more queued than its
 * mailbox holds moves too: what its mailbox cannot take goes, in order, into
 * an overflow of its own that moves with it, and every later delivery goes
 * behind that overflow (#3241).
 *
 * The actors are registered on the cores the cases need, which the
 * scheduler keeps: no core idles long enough to steal, and the only send
 * from one core to another outside an ask (E's numbers to D, in the
 * overflow cases) gathers D on the core the relief takes it to.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_actor_inbox.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define relief_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define relief_sleep_ms(ms) usleep((ms) * 1000)
#endif

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} ReliefMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* ask;        // asked on every message when set
    ActorBase* watch;      // its count is read when the ask returns
    int pause_ms;          // slept before replying
    ActorBase* hold_for;   // the reply waits until this has handled hold_want
    int hold_want;
    atomic_int got;        // messages handled
    atomic_int result;     // what the ask returned
    atomic_int watched;    // `watch`'s count when it did
    atomic_long took_ms;   // how long it took
    atomic_int done;
} ReliefActor;

static void relief_step(ReliefActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);

    int value = 42;
    if (self->ask) {
        ReliefMsg q = { 1, 0 };
        uint64_t t0 = aether_now_ns();
        int* r = (int*)scheduler_ask_message(self->ask, &q, sizeof q, 3000);
        atomic_store(&self->took_ms, (long)((aether_now_ns() - t0) / 1000000ULL));
        value = r ? *r + 1 : -1000;
        free(r);
        if (self->watch) atomic_store(&self->watched, atomic_load(&((ReliefActor*)self->watch)->got));
        atomic_store(&self->result, value);
        atomic_store(&self->done, 1);
    }
    if (self->pause_ms) relief_sleep_ms(self->pause_ms);
    /* #3483: the reply waits for an event, not a pause a slow runner
     * outlasts: until `hold_for` has handled `hold_want`, or 2 s, inside
     * the asker's 3 s timeout so a relief that fails shows as a short count. */
    if (self->hold_for) {
        uint64_t until = aether_now_ns() + 2000ULL * 1000000ULL;
        while (atomic_load(&((ReliefActor*)self->hold_for)->got) < self->hold_want &&
               aether_now_ns() < until)
            relief_sleep_ms(1);
    }
    scheduler_reply((ActorBase*)self, &value, sizeof value);   // no asker: nothing
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static ReliefActor* relief_actor(int core) {
    ReliefActor* a = calloc(1, sizeof(ReliefActor));
    static int next_id = 9000;
    a->id = next_id++;
    a->step = (void (*)(void*))relief_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

static void relief_send(ReliefActor* a) {
    ReliefMsg m = { 1, 0 };
    aether_send_message(a, &m, sizeof m);
}

static int relief_wait_done(ReliefActor* a, int limit_ms) {
    for (int w = 0; w < limit_ms && !atomic_load(&a->done); w++) relief_sleep_ms(1);
    return atomic_load(&a->done);
}

/* The chain of the issue: A on core 0 asks B on core 1, and B, answering,
 * asks C, which is on core 0 too. C's message waited in core 0's queue
 * while core 0 waited for B: both asks timed out. */
static void test_ask_relief_chain_back_to_waiting_core(void) {
    scheduler_init(2);
    ReliefActor* c = relief_actor(0);
    ReliefActor* b = relief_actor(1);
    ReliefActor* a = relief_actor(0);
    b->ask = (ActorBase*)c;
    a->ask = (ActorBase*)b;
    scheduler_start();

    relief_send(a);
    int done = relief_wait_done(a, 6000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_EQ(44, atomic_load(&a->result));   // C 42, B 43, A 44
    ASSERT_TRUE(atomic_load(&a->took_ms) < 1000);
    free(a);
    free(b);
    free(c);
}

/* An actor of a core waiting in an ask runs meanwhile: D, on A's core 0, is
 * sent 20 messages (they fit its mailbox, #3241) once A waits for B, and B,
 * on core 2, holds its reply until D has handled them (or 2 s pass). They
 * waited for A's reply. #3483: B paused 400 ms instead, and on a slow runner
 * (macOS ASan, Valgrind) the pause ended with 11 of the 20 handled. B is on
 * core 2, as in the overflow cases below, so core 1 is the relief's choice
 * for D: on core 1 B's long step was a core D could be moved behind, and B
 * waited for D while D waited for B. */
static void test_ask_relief_other_actor_runs(void) {
    scheduler_init(3);
    ReliefActor* b = relief_actor(2);
    ReliefActor* d = relief_actor(0);
    ReliefActor* a = relief_actor(0);
    b->hold_for = (ActorBase*)d;
    b->hold_want = 20;
    a->ask = (ActorBase*)b;
    a->watch = (ActorBase*)d;
    scheduler_start();

    relief_send(a);
    /* B has A's ask, so A is waiting for it. */
    uint64_t until = aether_now_ns() + 3000ULL * 1000000ULL;
    while (atomic_load(&b->got) < 1 && aether_now_ns() < until) relief_sleep_ms(1);
    int asked = atomic_load(&b->got) >= 1;
    for (int i = 0; i < 20; i++) relief_send(d);
    int done = relief_wait_done(a, 6000);
    scheduler_shutdown();
    scheduler_cleanup();

    /* Read before the actors are freed, so a failed check leaks nothing. */
    int result = atomic_load(&a->result);
    int watched = atomic_load(&a->watched);
    int got = atomic_load(&d->got);
    free(a);
    free(b);
    free(d);
    ASSERT_TRUE(asked);
    ASSERT_TRUE(done);
    ASSERT_EQ(43, result);
    ASSERT_EQ(20, watched);
    ASSERT_EQ(20, got);
}

/* An ask to an actor of the same core whose message is still in the batch
 * the core was delivering: P and T get one message each in one batch while B
 * holds core 0, and P's handler asks T. The ask's message waited behind T's
 * batched one, which nothing placed, to the timeout. */
static void test_ask_relief_target_held_in_batch(void) {
    scheduler_init(2);
    ReliefActor* b = relief_actor(0);
    ReliefActor* p = relief_actor(0);
    ReliefActor* t = relief_actor(0);
    b->pause_ms = 150;
    p->ask = (ActorBase*)t;
    scheduler_start();

    relief_send(b);          // core 0 is busy for 150 ms
    relief_sleep_ms(30);
    relief_send(p);
    relief_send(t);          // behind P's, in the same batch
    int done = relief_wait_done(p, 6000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_EQ(43, atomic_load(&p->result));
    ASSERT_TRUE(atomic_load(&p->took_ms) < 1000);
    ASSERT_EQ(2, atomic_load(&t->got));
    free(b);
    free(p);
    free(t);
}

/* ---------------------------------------------------------------------------
 * An actor with more messages queued than its mailbox holds (#3241)
 *
 * A on core 0 asks B on core 2, and B holds its reply until D, on A's core,
 * has handled what it is sent (or died). D is sent more numbered messages
 * than its mailbox holds (32) while core 0 waits. The relief puts the 32 in
 * its mailbox and the rest, in order, in its overflow, and moves D, with
 * both, to core 1, which runs it. D used to stay on core 0, a message of it
 * still queued there, until the ask ended: B held its reply until its own
 * wait ran out, and D had handled nothing when A's ask returned.
 *
 * Core 1 is the relief's choice: the waiting core and B's, in a long step,
 * are not, and of the others it takes the lowest. What D is sent once it
 * runs there goes behind its overflow, from either delivery path: a local
 * send (E, on core 1, answering D's first number inline) and a message
 * through core 1's queues (main's, while D's first number holds it).
 * ------------------------------------------------------------------------- */

enum { OV_NUMBER = 1, OV_EMIT = 2, OV_ASK = 3, OV_HOLD = 4 };

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to;          // where an EMIT sends its numbers
    ActorBase* poke;        // sent an EMIT of `poke_count` on the first number
    int poke_count;
    ActorBase* ask;         // asked with a HOLD on an ASK
    ActorBase* watch;       // what a HOLD waits on, read when the ask returns
    int want;               // a HOLD waits until `watch` has this many
    int hold_first;         // the first number waits for `release_first`
    int panic_first;        // the first number panics
    int next;               // the last number an EMIT sent
    atomic_int release_first;
    atomic_int in_first;    // set while the first number is handled
    atomic_int got;         // numbers handled
    atomic_int last;        // the last number handled
    atomic_int disorder;    // numbers that did not follow the last
    atomic_int first_core;  // the core the first number was handled on
    atomic_int asked;       // a HOLD came
    atomic_int watched;     // `watch`'s count when the ask returned
    atomic_int watched_dead;
    atomic_int done;
} OvActor;

/* Bounds by the clock: a 1 ms sleep can last a 15.6 ms tick on Windows. */
static int ov_wait(atomic_int* v, int want, int limit_ms) {
    uint64_t until = aether_now_ns() + (uint64_t)limit_ms * 1000000ULL;
    while (atomic_load(v) < want && aether_now_ns() < until) relief_sleep_ms(1);
    return atomic_load(v) >= want;
}

static void ov_send(void* to, int type, int value) {
    ReliefMsg m = { type, value };
    aether_send_message(to, &m, sizeof m);
}

static void ov_step(OvActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    ReliefMsg m = *(ReliefMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);

    if (m.type == OV_NUMBER) {
        if (atomic_load(&self->got) == 0) {
            atomic_store(&self->first_core, aether_core_id_get());
            if (self->panic_first) aether_panic("first number");
            if (self->hold_first) {
                atomic_store(&self->in_first, 1);
                ov_wait(&self->release_first, 1, 5000);
            }
            if (self->poke) ov_send(self->poke, OV_EMIT, self->poke_count);
        }
        if (m.value != atomic_load(&self->last) + 1) atomic_fetch_add(&self->disorder, 1);
        atomic_store(&self->last, m.value);
        atomic_fetch_add(&self->got, 1);
    } else if (m.type == OV_EMIT) {
        for (int k = 0; k < m.value; k++) ov_send(self->to, OV_NUMBER, ++self->next);
    } else if (m.type == OV_ASK) {
        ReliefMsg q = { OV_HOLD, 0 };
        int* r = (int*)scheduler_ask_message(self->ask, &q, sizeof q, 6000);
        free(r);
        OvActor* w = (OvActor*)self->watch;
        atomic_store(&self->watched, atomic_load(&w->got));
        atomic_store(&self->watched_dead, atomic_load(&w->dead) != 0);
        atomic_store(&self->done, 1);
    } else if (m.type == OV_HOLD) {
        atomic_store(&self->asked, 1);
        OvActor* w = (OvActor*)self->watch;
        uint64_t until = aether_now_ns() + 3000ULL * 1000000ULL;
        while (atomic_load(&w->got) < self->want && !atomic_load(&w->dead) &&
               aether_now_ns() < until)
            relief_sleep_ms(1);
        int value = 1;
        scheduler_reply((ActorBase*)self, &value, sizeof value);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static OvActor* ov_actor(int core) {
    OvActor* a = calloc(1, sizeof(OvActor));
    static int next_id = 9200;
    a->id = next_id++;
    a->step = (void (*)(void*))ov_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_init(&a->first_core, -1);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

/* What D's overflow still holds, read with D dead: none once it is drained. */
static int ov_overflow_left(OvActor* d) {
    ActorInbox* ib = atomic_load(&d->inbox);
    return ib ? atomic_load(&ib->count) : 0;
}

/* E sends D 100 numbers while core 0 waits: D moves with them, and the 10
 * that E sends from core 1 as D handles its first go behind its overflow. */
static void test_ask_relief_overflow_moves_actor(void) {
    scheduler_init(3);
    OvActor* d = ov_actor(0);
    OvActor* a = ov_actor(0);
    OvActor* e = ov_actor(1);
    OvActor* b = ov_actor(2);
    a->ask = (ActorBase*)b;
    a->watch = (ActorBase*)d;
    b->watch = (ActorBase*)d;
    b->want = 110;
    d->poke = (ActorBase*)e;
    d->poke_count = 10;
    e->to = (ActorBase*)d;
    scheduler_start();

    ov_send(a, OV_ASK, 0);
    int asked = ov_wait(&b->asked, 1, 3000);
    ov_send(e, OV_EMIT, 100);          // more than D's mailbox holds
    int done = ov_wait(&a->done, 1, 12000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(asked);
    ASSERT_TRUE(done);
    ASSERT_EQ(110, atomic_load(&a->watched));   // all of it, during the wait
    ASSERT_EQ(1, atomic_load(&d->first_core));
    ASSERT_EQ(110, atomic_load(&d->got));
    ASSERT_EQ(110, atomic_load(&d->last));
    ASSERT_EQ(0, atomic_load(&d->disorder));
    free(a);
    free(b);
    free(d);
    free(e);
}

/* Main sends D 100 numbers while core 0 waits, and 50 more once D, moved,
 * holds its first on core 1: they reach core 1 through its queue and go
 * behind the overflow. */
static void test_ask_relief_overflow_queued_behind(void) {
    scheduler_init(3);
    OvActor* d = ov_actor(0);
    OvActor* a = ov_actor(0);
    OvActor* b = ov_actor(2);
    a->ask = (ActorBase*)b;
    a->watch = (ActorBase*)d;
    b->watch = (ActorBase*)d;
    b->want = 150;
    d->hold_first = 1;
    scheduler_start();

    ov_send(a, OV_ASK, 0);
    int asked = ov_wait(&b->asked, 1, 3000);
    for (int i = 1; i <= 100; i++) ov_send(d, OV_NUMBER, i);
    int holding = ov_wait(&d->in_first, 1, 3000);
    for (int i = 101; i <= 150; i++) ov_send(d, OV_NUMBER, i);
    atomic_store(&d->release_first, 1);
    int done = ov_wait(&a->done, 1, 12000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(asked);
    ASSERT_TRUE(holding);
    ASSERT_TRUE(done);
    ASSERT_EQ(150, atomic_load(&a->watched));
    ASSERT_EQ(1, atomic_load(&d->first_core));
    ASSERT_EQ(150, atomic_load(&d->got));
    ASSERT_EQ(150, atomic_load(&d->last));
    ASSERT_EQ(0, atomic_load(&d->disorder));
    free(a);
    free(b);
    free(d);
}

/* D, moved with 68 messages in its overflow, panics on its first: what the
 * overflow holds is dropped with it and credited as handled, or the
 * shutdown's wait for every message sent would never end. */
static void test_ask_relief_overflow_panic_drains(void) {
    scheduler_init(3);
    OvActor* d = ov_actor(0);
    OvActor* a = ov_actor(0);
    OvActor* b = ov_actor(2);
    a->ask = (ActorBase*)b;
    a->watch = (ActorBase*)d;
    b->watch = (ActorBase*)d;
    b->want = 100;
    d->panic_first = 1;
    scheduler_start();

    ov_send(a, OV_ASK, 0);
    int asked = ov_wait(&b->asked, 1, 3000);
    for (int i = 1; i <= 100; i++) ov_send(d, OV_NUMBER, i);
    int done = ov_wait(&a->done, 1, 12000);
    int left = ov_overflow_left(d);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(asked);
    ASSERT_TRUE(done);
    ASSERT_EQ(1, atomic_load(&a->watched_dead));   // it died during the wait
    ASSERT_EQ(1, atomic_load(&d->first_core));
    ASSERT_EQ(0, atomic_load(&d->got));
    ASSERT_EQ(0, left);
    free(a);
    free(b);
    free(d);
}

void register_ask_relief_tests(void) {
    register_test_with_category("Ask relief: an actor with more queued than its mailbox moves",
        test_ask_relief_overflow_moves_actor, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ask relief: messages that come later go behind the overflow",
        test_ask_relief_overflow_queued_behind, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ask relief: a panic drops what the overflow holds",
        test_ask_relief_overflow_panic_drains, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ask relief: a same-core target held in the batch",
        test_ask_relief_target_held_in_batch, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ask relief: a chain back to the waiting core",
        test_ask_relief_chain_back_to_waiting_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ask relief: the waiting core's other actors run",
        test_ask_relief_other_actor_runs, TEST_CATEGORY_RUNTIME);
}
