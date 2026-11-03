/**
 * An ask's message carries its own reply slot, and nothing else does
 * (#3387, #3395).
 *
 * The ask used to hand its slot to aether_send_message through a
 * thread-local, set around the send. Two things went wrong with that:
 *
 * - The send steps its target inline when the target is an idle actor of
 *   the same core, and the target's handler runs inside the send. Every
 *   payload message that handler sent read the thread-local too, so its
 *   receiver got the asker's slot. It replied into it, or failed it, and the
 *   slot was let go twice: a use after free in the asker (#3395).
 * - Inside an execution, a payload copy that finds no memory ends the call
 *   by unwinding from inside the send. The unwind skipped the line that
 *   cleared the thread-local and, on a core's thread, the lines that undo
 *   the core's asking state. Every later payload send on that thread
 *   carried the stale slot, and no actor was moved onto the core again
 *   (#3387).
 *
 * The ask now builds its own message with the slot in it. A copy that finds
 * no memory fails the ask (AETHER_ASK_NO_MEMORY), and the call then ends
 * with AETHER_OUT_OF_MEMORY at its return, with nothing left set.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../runtime/aether_exec.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define am_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define am_sleep_ms(ms) usleep((ms) * 1000)
#endif


typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} AmMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int value;               // what it replies
    ActorBase* ask;          // asked on its first message, when set
    ActorBase* tell;         // sent a payload message before it replies, when set
    int ask_fails;           // the ask is made inside an execution, its copy refused
    atomic_int got;          // messages handled
    atomic_int slotted;      // messages that came with a reply slot
    atomic_int result;       // what the ask returned, or its status when it failed
    atomic_int outcome;      // the execution's outcome (ask_fails)
    atomic_int asking_after; // the core's asking count once the ask returned
    atomic_int done;
} AmActor;

/* Larger than the bytes a message carries its payload in (#3534), so the
 * ask copies it to the heap, where the copy can be refused. */
typedef struct {
    int type;
    int value;
    char pad[AETHER_MSG_INLINE_BYTES];
} AmBigMsg;

static AmActor* g_failing;
static void ask_refused(void) {
    AmActor* self = g_failing;
    AmBigMsg q = { 1, 0, { 0 } };
    int status = 0;
    aether_test_fail_alloc_after(1);
    void* r = scheduler_ask_message_status(self->ask, &q, sizeof q, 3000, &status);
    free(r);
    atomic_store(&self->result, status);
}

static void am_step(AmActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_reply_slot_set(msg._reply_slot);
    if (msg._reply_slot) atomic_fetch_add(&self->slotted, 1);
    aether_message_free_payload(&msg);
    int first = atomic_fetch_add(&self->got, 1) == 0;

    if (self->ask && first) {
        if (self->ask_fails) {
            g_failing = self;
            AetherJmpFrame* x = aether_exec_enter();
            if (AETHER_SIGSETJMP(x->buf, 1)) {
                aether_exec_failed();
            } else {
                ask_refused();
                aether_exec_leave();
            }
            atomic_store(&self->outcome, aether_last_outcome());
            int core = atomic_load(&self->assigned_core);
            atomic_store(&self->asking_after, atomic_load(&schedulers[core].asking));
        } else {
            AmMsg q = { 1, 0 };
            int* r = (int*)scheduler_ask_message(self->ask, &q, sizeof q, 3000);
            atomic_store(&self->result, r ? *r : -1000);
            free(r);
        }
    }
    if (self->tell) {
        AmMsg m = { 1, 0 };
        aether_send_message(self->tell, &m, sizeof m);
    }
    int value = self->value;
    scheduler_reply((ActorBase*)self, &value, sizeof value);   // no asker: nothing
    if (self->ask && first) atomic_store(&self->done, 1);
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static AmActor* am_actor(int core, int value) {
    AmActor* a = calloc(1, sizeof(AmActor));
    static int next_id = 9500;
    a->id = next_id++;
    a->value = value;
    a->step = (void (*)(void*))am_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

static void am_send(AmActor* a) {
    AmMsg m = { 1, 0 };
    aether_send_message(a, &m, sizeof m);
}

static int am_wait(atomic_int* v, int want, int limit_ms) {
    for (int w = 0; w < limit_ms && atomic_load(v) < want; w++) am_sleep_ms(1);
    return atomic_load(v) >= want;
}

/* #3395: A asks B, both on core 0, B idle, so A's send steps B inline. B
 * tells C (core 0, idle: stepped inline in turn) and then replies 7. C's
 * message carried A's slot: C replied 42 into it, B's reply let the slot
 * go a second time, and A read freed memory. */
TEST_CATEGORY(ask_slot_not_on_inline_handlers_sends, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    AmActor* c = am_actor(0, 42);
    AmActor* b = am_actor(0, 7);
    AmActor* a = am_actor(0, 1);
    b->tell = (ActorBase*)c;
    a->ask = (ActorBase*)b;
    scheduler_start();

    am_send(a);
    int done = am_wait(&a->done, 1, 6000);
    int told = am_wait(&c->got, 1, 2000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_EQ(7, atomic_load(&a->result));   // B's reply, not C's
    ASSERT_TRUE(told);
    ASSERT_EQ(0, atomic_load(&c->slotted));  // C's message was B's plain send
    ASSERT_EQ(1, atomic_load(&b->slotted));  // the ask's own
    free(a);
    free(b);
    free(c);
}

/* #3387: inside an execution on core 0's thread, an ask whose payload copy
 * finds no memory. The ask fails with AETHER_ASK_NO_MEMORY, the call ends
 * with AETHER_OUT_OF_MEMORY, the core is not left asking, and the payload
 * message the same handler sends next (to D) carries no slot. */
TEST_CATEGORY(ask_copy_failure_leaves_nothing_set, TEST_CATEGORY_RUNTIME) {
    scheduler_init(1);
    AmActor* d = am_actor(0, 3);
    AmActor* b = am_actor(0, 7);
    AmActor* a = am_actor(0, 1);
    a->ask = (ActorBase*)b;
    a->ask_fails = 1;
    a->tell = (ActorBase*)d;
    scheduler_start();

    am_send(a);
    int done = am_wait(&a->done, 1, 6000);
    int told = am_wait(&d->got, 1, 2000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(done);
    ASSERT_EQ(AETHER_ASK_NO_MEMORY, atomic_load(&a->result));
    ASSERT_EQ(AETHER_OUT_OF_MEMORY, atomic_load(&a->outcome));
    ASSERT_EQ(0, atomic_load(&a->asking_after));
    ASSERT_EQ(0, atomic_load(&b->got));       // nothing was sent
    ASSERT_TRUE(told);
    ASSERT_EQ(0, atomic_load(&d->slotted));
    free(a);
    free(b);
    free(d);
}
