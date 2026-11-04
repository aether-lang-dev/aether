/**
 * A flush of a core's overflow is not entered again from a step it makes
 * (#3247).
 *
 * S sends T, on its own core, more messages in one handler than T's mailbox
 * and the core's own queue hold, so the rest goes to the core's overflow.
 * Flushing it, the core delivers to T and steps T at once; T's handler sends
 * U, also of the core, which is stepped inline, and the inline send flushed
 * the overflow again, from inside the first flush. That delivered entries
 * the outer flush had already delivered: T saw messages twice. Every message
 * must arrive once.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define reentry_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define reentry_sleep_ms(ms) usleep((ms) * 1000)
#endif

enum { REENTRY_GO = 1, REENTRY_ITEM = 2 };

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} ReentryMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to;      // where each message handled sends one (GO: `burst` of them)
    int burst;
    atomic_int got;
} ReentryActor;

static void reentry_step(ReentryActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    ReentryMsg m = *(ReentryMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);
    int n = m.type == REENTRY_GO ? self->burst : 1;
    for (int i = 0; self->to && i < n; i++) {
        ReentryMsg out = { REENTRY_ITEM, i };
        aether_send_message(self->to, &out, sizeof out);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static ReentryActor* reentry_actor(int core) {
    ReentryActor* a = (ReentryActor*)scheduler_spawn_actor(core, (void (*)(void*))reentry_step,
                                                           sizeof(ReentryActor));
    a->to = NULL;
    a->burst = 0;
    atomic_store(&a->got, 0);
    return a;
}

static void test_overflow_flush_not_reentered(void) {
    const int N = 3000;
    scheduler_init(2);
    scheduler_start();
    ReentryActor* u = reentry_actor(0);
    ReentryActor* t = reentry_actor(0);
    ReentryActor* s = reentry_actor(0);
    t->to = (ActorBase*)u;
    s->to = (ActorBase*)t;
    s->burst = N;

    ReentryMsg go = { REENTRY_GO, 0 };
    aether_send_message(s, &go, sizeof go);
    for (int w = 0; w < 10000 && atomic_load(&u->got) < N; w++) reentry_sleep_ms(1);
    reentry_sleep_ms(50);
    int t_got = atomic_load(&t->got), u_got = atomic_load(&u->got);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(N, t_got);
    ASSERT_EQ(N, u_got);
}

void register_overflow_reentry_tests(void) {
    register_test_with_category("An overflow flush is not entered again from its steps",
        test_overflow_flush_not_reentered, TEST_CATEGORY_RUNTIME);
}
