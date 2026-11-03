/**
 * A bounded mailbox (#2974): `@mailbox(bound: N)` accepts at most N messages
 * for an actor that its step has not taken yet, wherever they wait (a core's
 * queue, a sender's deferred sends, its overflow, its mailbox), and refuses
 * the rest without blocking the sender: scheduler_try_send says so, a plain
 * send is dropped and counted in full_drops, an ask fails at once. Every
 * place comes back as the step takes a message (aether_mailbox_took, which
 * the generated step calls) or a drop releases one.
 *
 * The actors are registered on the cores the cases need, as in
 * test_ask_relief.c: D's core waits in an ask, so a relief takes D's
 * messages into its mailbox and overflow (#3241) and moves it to core 1.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_actor_thread.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define bound_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define bound_sleep_ms(ms) usleep((ms) * 1000)
#endif

enum { BD_NUMBER = 1, BD_ASK = 2, BD_HOLD = 3, BD_EMIT = 4, BD_COUNT = 5 };

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} BdMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to;          // where an EMIT sends its numbers, plain
    ActorBase* ask;         // asked with a HOLD on an ASK
    ActorBase* watch;       // what a HOLD waits on
    int want;               // a HOLD waits until `watch` has handled this many
    int hold_first;         // the first number waits for `release_first`
    atomic_int release_first;
    atomic_int in_first;    // set while the first number is handled
    atomic_int got;         // numbers handled
    atomic_int last;        // the last number handled
    atomic_int disorder;    // numbers not above the one before
    atomic_int first_core;  // the core the first number was handled on
    atomic_int asked;       // a HOLD came
    atomic_int done;        // an ASK or an EMIT finished
} BdActor;

static int bd_wait(atomic_int* v, int want, int limit_ms) {
    uint64_t until = aether_now_ns() + (uint64_t)limit_ms * 1000000ULL;
    while (atomic_load(v) < want && aether_now_ns() < until) bound_sleep_ms(1);
    return atomic_load(v) >= want;
}

static BdMsg* bd_payload(int type, int value) {
    BdMsg* m = malloc(sizeof *m);
    m->type = type;
    m->value = value;
    return m;
}

static Message bd_message(int type, int value) {
    Message msg;
    memset(&msg, 0, sizeof msg);
    msg.type = type;
    msg.payload_ptr = bd_payload(type, value);
    return msg;
}

static void bd_send(void* to, int type, int value) {
    BdMsg m = { type, value };
    aether_send_message(to, &m, sizeof m);
}

static void bd_step(BdActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    // What the generated step of a bounded actor does as it takes one.
    if (atomic_load(&self->mailbox_bound)) aether_mailbox_took((ActorBase*)self, msg.type);
    aether_reply_slot_set(msg._reply_slot);
    BdMsg m = *(BdMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);

    if (m.type == BD_NUMBER) {
        if (atomic_load(&self->got) == 0) {
            atomic_store(&self->first_core, aether_core_id_get());
            if (self->hold_first) {
                atomic_store(&self->in_first, 1);
                bd_wait(&self->release_first, 1, 8000);
            }
        }
        if (m.value <= atomic_load(&self->last)) atomic_fetch_add(&self->disorder, 1);
        atomic_store(&self->last, m.value);
        atomic_fetch_add(&self->got, 1);
    } else if (m.type == BD_EMIT) {
        for (int k = 1; k <= m.value; k++) bd_send(self->to, BD_NUMBER, 100000 + k);
        atomic_store(&self->done, 1);
    } else if (m.type == BD_ASK) {
        BdMsg q = { BD_HOLD, 0 };
        int* r = (int*)scheduler_ask_message(self->ask, &q, sizeof q, 10000);
        free(r);
        atomic_store(&self->done, 1);
    } else if (m.type == BD_HOLD) {
        atomic_store(&self->asked, 1);
        BdActor* w = (BdActor*)self->watch;
        uint64_t until = aether_now_ns() + 8000ULL * 1000000ULL;
        while (atomic_load(&w->got) < self->want && !atomic_load(&w->dead) &&
               aether_now_ns() < until)
            bound_sleep_ms(1);
        int value = 1;
        scheduler_reply((ActorBase*)self, &value, sizeof value);
    } else if (m.type == BD_COUNT) {
        int value = atomic_load(&self->got);
        scheduler_reply((ActorBase*)self, &value, sizeof value);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static BdActor* bd_actor(int core, int bound) {
    BdActor* a = calloc(1, sizeof(BdActor));
    static int next_id = 9600;
    a->id = next_id++;
    a->step = (void (*)(void*))bd_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_init(&a->first_core, -1);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    atomic_store(&a->mailbox_bound, bound);   // as the generated spawn sets it
    return a;
}

/* Main sends D numbers with scheduler_try_send while D's core waits in an
 * ask: what is accepted waits in core 0's queue, then in D's mailbox and
 * overflow when the relief moves D, with its first number, to core 1, where
 * it holds that number. Main sends more once it holds. Nothing else is
 * taken meanwhile, so exactly the bound is pending, the held one besides:
 * 51 accepted in all, whenever the relief came. E, an actor with a thread of
 * its own, sends D 30 plain while D holds: all dropped and counted. Main's
 * ask fails at once. Let go, D handles the 51 in order, and every place has
 * come back.
 *
 * Three cores, as in test_ask_relief.c: the relief moves D to an idle core,
 * preferring a parked one, and with core 2 in B's long step that is core 1
 * alone. E runs on no core, so wherever D goes it cannot hold E up (E on a
 * fourth core was where D went when core 1 was awake: E never ran). */
static void test_mailbox_bound_across_overflow(void) {
    scheduler_init(3);
    BdActor* d = bd_actor(0, 50);
    BdActor* a = bd_actor(0, 0);
    BdActor* b = bd_actor(2, 0);
    a->ask = (ActorBase*)b;
    b->watch = (ActorBase*)d;
    b->want = 51;
    d->hold_first = 1;
    uint64_t full_before = scheduler_full_drops();
    scheduler_start();
    BdActor* e = (BdActor*)scheduler_spawn_actor(-1, (void (*)(void*))bd_step, sizeof(BdActor));
    int e_started = 0;
    if (e) {
        memset((char*)e + sizeof(ActorBase), 0, sizeof(BdActor) - sizeof(ActorBase));
        atomic_store(&e->first_core, -1);
        e->to = (ActorBase*)d;
        e->auto_process = 1;
        e_started = pthread_create(&e->thread, NULL, aether_actor_thread, e) == 0;
    }

    bd_send(a, BD_ASK, 0);
    int asked = bd_wait(&b->asked, 1, 3000);
    int accepted = 0, full = 0, other = 0;
    for (int i = 1; i <= 100; i++) {
        const char* why = scheduler_try_send((ActorBase*)d, bd_message(BD_NUMBER, i));
        if (!why[0]) accepted++;
        else if (strcmp(why, "the mailbox is full") == 0) full++;
        else other++;
    }
    int holding = bd_wait(&d->in_first, 1, 5000);
    for (int i = 101; i <= 200; i++) {
        const char* why = scheduler_try_send((ActorBase*)d, bd_message(BD_NUMBER, i));
        if (!why[0]) accepted++;
        else if (strcmp(why, "the mailbox is full") == 0) full++;
        else other++;
    }
    int pending_held = atomic_load(&d->mailbox_pending);
    int emitted = 0;
    if (e_started) {
        bd_send(e, BD_EMIT, 30);
        emitted = bd_wait(&e->done, 1, 5000);
    }
    BdMsg q = { BD_COUNT, 0 };
    int status = -1;
    void* r = scheduler_ask_message_status((ActorBase*)d, &q, sizeof q, 5000, &status);
    free(r);
    atomic_store(&d->release_first, 1);
    int done = bd_wait(&a->done, 1, 12000);
    int handled = bd_wait(&d->got, 51, 5000);
    scheduler_wait();
    int pending_after = atomic_load(&d->mailbox_pending);
    if (e_started) {
        scheduler_release_actor((ActorBase*)e);   // its thread ends it
        pthread_join(e->thread, NULL);
    }
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(e_started);
    ASSERT_TRUE(asked);
    ASSERT_TRUE(holding);
    ASSERT_TRUE(emitted);
    ASSERT_TRUE(done);
    ASSERT_TRUE(handled);
    ASSERT_EQ(51, accepted);
    ASSERT_EQ(149, full);
    ASSERT_EQ(0, other);
    ASSERT_EQ(50, pending_held);
    ASSERT_EQ(AETHER_ASK_FULL, status);
    ASSERT_EQ(30, (int)atomic_load(&d->full_drops));   // E's, not the ask
    ASSERT_EQ(30, (int)(scheduler_full_drops() - full_before));
    ASSERT_EQ(1, atomic_load(&d->first_core));          // moved with its overflow
    ASSERT_EQ(51, atomic_load(&d->got));
    ASSERT_EQ(0, atomic_load(&d->disorder));
    ASSERT_EQ(0, pending_after);
    free(a);
    free(b);
    free(d);
}

/* D holds its first number with 10 accepted behind it, and is killed: its
 * death drops the 10, and a plain send to it after is a dead send, not a
 * full one. Each drop gives its place back, so none is left pending. */
static void test_mailbox_bound_drops_give_back(void) {
    scheduler_init(2);
    BdActor* d = bd_actor(0, 10);
    d->hold_first = 1;
    uint64_t dead_before = scheduler_dead_sends();
    scheduler_start();

    bd_send(d, BD_NUMBER, 1);
    int holding = bd_wait(&d->in_first, 1, 3000);
    int accepted = 0;
    for (int i = 2; i <= 20; i++)
        if (!scheduler_try_send((ActorBase*)d, bd_message(BD_NUMBER, i))[0]) accepted++;
    int pending_full = atomic_load(&d->mailbox_pending);
    scheduler_actor_end((ActorBase*)d, "killed", AETHER_ACTOR_KILLED);
    atomic_store(&d->release_first, 1);
    scheduler_wait();
    bd_send(d, BD_NUMBER, 99);
    scheduler_wait();
    int pending_after = atomic_load(&d->mailbox_pending);
    const char* why = scheduler_try_send((ActorBase*)d, bd_message(BD_NUMBER, 100));
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(holding);
    ASSERT_EQ(10, accepted);
    ASSERT_EQ(10, pending_full);
    ASSERT_EQ(1, atomic_load(&d->got));
    ASSERT_EQ(11, (int)(scheduler_dead_sends() - dead_before));   // the 10 and the late one
    ASSERT_EQ(0, (int)atomic_load(&d->full_drops));
    ASSERT_EQ(0, pending_after);
    ASSERT_TRUE(strcmp(why, "the actor is dead") == 0);
    free(d);
}

void register_mailbox_bound_tests(void) {
    register_test_with_category("Mailbox bound: exact across the queues and the overflow",
        test_mailbox_bound_across_overflow, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Mailbox bound: drops give their places back",
        test_mailbox_bound_drops_give_back, TEST_CATEGORY_RUNTIME);
}
