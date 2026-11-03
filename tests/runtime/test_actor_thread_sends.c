/**
 * An actor with a thread of its own sends as a foreign thread (#3246).
 *
 * It ran with its core's id, so its sends went straight into that core's
 * mailboxes and into other cores' queues from that core, whose one producer
 * is the core's own thread. A (its own thread, core 0) and K (core 0) both
 * send to L (core 1), and A sends to K while core 0 delivers main's messages
 * to K: every message must arrive once. With two producers on those
 * structures the counts came out wrong, or a send spun forever.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/actors/aether_actor_thread.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define athread_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define athread_sleep_ms(ms) usleep((ms) * 1000)
#endif

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} AthreadMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to[2];   // each message handled sends one to each of these
    atomic_int got;
} AthreadActor;

static void athread_step(AthreadActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);
    for (int k = 0; k < 2; k++) {
        if (!self->to[k]) continue;
        AthreadMsg m = { 1, 0 };
        aether_send_message(self->to[k], &m, sizeof m);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static AthreadActor* athread_actor(int core) {
    AthreadActor* a = (AthreadActor*)scheduler_spawn_actor(core, (void (*)(void*))athread_step,
                                                           sizeof(AthreadActor));
    a->to[0] = a->to[1] = NULL;
    atomic_store(&a->got, 0);
    return a;
}

static void athread_send(AthreadActor* a) {
    AthreadMsg m = { 1, 0 };
    aether_send_message(a, &m, sizeof m);
}

static void test_actor_thread_sends_once(void) {
    const int N = 100000;
    scheduler_init(2);
    scheduler_start();
    AthreadActor* l = athread_actor(1);
    AthreadActor* k = athread_actor(0);
    AthreadActor* a = athread_actor(0);
    k->to[0] = (ActorBase*)l;
    a->to[0] = (ActorBase*)k;
    a->to[1] = (ActorBase*)l;
    a->auto_process = 1;
    int started = aether_actor_thread_start(&a->thread, a) == 0;

    for (int i = 0; i < N; i++) {
        athread_send(a);
        athread_send(k);
    }
    // K takes N from main and N from A, and sends L one for each; A sends L N.
    for (int w = 0; w < 20000 && atomic_load(&l->got) < 3 * N; w++) athread_sleep_ms(1);
    athread_sleep_ms(50);
    int a_got = atomic_load(&a->got), k_got = atomic_load(&k->got), l_got = atomic_load(&l->got);
    int k_inflight = atomic_load(&k->inflight), l_inflight = atomic_load(&l->inflight);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(started);
    ASSERT_EQ(N, a_got);
    ASSERT_EQ(2 * N, k_got);
    ASSERT_EQ(3 * N, l_got);
    ASSERT_EQ(0, k_inflight);
    ASSERT_EQ(0, l_inflight);
}

void register_actor_thread_sends_tests(void) {
    register_test_with_category("An actor thread's sends arrive once",
        test_actor_thread_sends_once, TEST_CATEGORY_RUNTIME);
}
