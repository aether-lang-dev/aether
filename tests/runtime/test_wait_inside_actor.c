/* scheduler_wait() (wait_for_idle()) on a thread that runs actors (#3463).
 *
 * An actor that panics ends on its core's thread, and the death hook runs
 * there, after the step, where the in-step flag is off and the step's
 * message is not yet counted handled: a hook that waited for the actors
 * waited for that core, which ran it, forever, and the program's own wait
 * after it never returned. The wait now panics on such a thread: the hook's
 * own frame takes it, the end of the actor goes on, and the program's wait
 * returns. */
#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_panic.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    int type;
    int value;
} WiMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int got;
} WiActor;

static void wi_step(WiActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_message_free_payload(&msg);
    atomic_fetch_add(&self->got, 1);
    aether_panic("the actor ends");
}

static atomic_int g_hook_entered;
static atomic_int g_hook_waited;

static void wi_hook(int actor_id, const char* reason) {
    (void)actor_id;
    (void)reason;
    atomic_fetch_add(&g_hook_entered, 1);
    scheduler_wait();
    atomic_fetch_add(&g_hook_waited, 1);
}

TEST_CATEGORY(wait_in_a_death_hook_on_a_core_panics, TEST_CATEGORY_RUNTIME) {
    atomic_store(&g_hook_entered, 0);
    atomic_store(&g_hook_waited, 0);
    scheduler_init(1);
    WiActor* a = calloc(1, sizeof(WiActor));
    a->id = 9700;
    a->step = (void (*)(void*))wi_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, 0);
    aether_set_on_actor_death(wi_hook);
    scheduler_start();

    WiMsg m = { 1, 0 };
    aether_send_message(a, &m, sizeof m);
    /* This thread runs no actor: it waits, and returns once the step's
     * message is counted handled, which is after the hook ran. */
    scheduler_wait();
    int entered = atomic_load(&g_hook_entered) == 1;
    scheduler_shutdown();
    scheduler_cleanup();
    aether_set_on_actor_death(NULL);

    ASSERT_TRUE(entered);
    ASSERT_EQ(1, atomic_load(&a->got));
    ASSERT_EQ(0, atomic_load(&g_hook_waited));   /* its wait panicked, in its frame */
    free(a);
}
