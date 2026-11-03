/**
 * A sender's messages keep their order when the sender moves (#3240).
 *
 * A core receives through one queue per sending core and drains them in
 * index order. S on core 2 sends R, on core 0, five numbered messages while
 * core 0 is busy, so they wait in core 0's queue from core 2. S is then
 * asked to move to core 1 (as a gather or a steal asks), and sends five
 * more. Moved at once, its next five waited in core 0's queue from core 1,
 * which core 0 drains first: R saw 6 before 1. A core now moves an actor
 * only when nothing sent from it is still on its way.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define order_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define order_sleep_ms(ms) usleep((ms) * 1000)
#endif

enum { ORDER_BLOCK = 1, ORDER_EMIT = 2, ORDER_NUMBER = 3 };

typedef struct {
    int type;   // the message id comes first, as in every typed payload
    int value;
} OrderMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to;         // where an EMIT sends the next number
    int next;              // the last number sent
    atomic_int sent;
    atomic_int got;        // numbers received
    atomic_int last;       // the last number received
    atomic_int disorder;   // numbers that did not follow the last
} OrderActor;

static void order_step(OrderActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    OrderMsg m = *(OrderMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);
    if (m.type == ORDER_BLOCK) {
        order_sleep_ms(m.value);
    } else if (m.type == ORDER_EMIT) {
        OrderMsg out = { ORDER_NUMBER, ++self->next };
        aether_send_message(self->to, &out, sizeof out);
        atomic_fetch_add(&self->sent, 1);
    } else {
        if (m.value != atomic_load(&self->last) + 1) atomic_fetch_add(&self->disorder, 1);
        atomic_store(&self->last, m.value);
        atomic_fetch_add(&self->got, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static OrderActor* order_actor(int core) {
    OrderActor* a = calloc(1, sizeof(OrderActor));
    static int next_id = 9100;
    a->id = next_id++;
    a->step = (void (*)(void*))order_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

static void order_send(OrderActor* a, int type, int value) {
    OrderMsg m = { type, value };
    aether_send_message(a, &m, sizeof m);
}

static int order_wait(atomic_int* v, int want, int limit_ms) {
    for (int w = 0; w < limit_ms && atomic_load(v) < want; w++) order_sleep_ms(1);
    return atomic_load(v) >= want;
}

static void test_sender_move_keeps_order(void) {
    scheduler_init(3);
    OrderActor* r = order_actor(0);
    OrderActor* s = order_actor(2);
    s->to = (ActorBase*)r;
    scheduler_start();

    order_send(r, ORDER_BLOCK, 300);   // core 0 is busy for 300 ms
    order_sleep_ms(20);
    for (int i = 0; i < 5; i++) order_send(s, ORDER_EMIT, 0);
    int first = order_wait(&s->sent, 5, 2000);
    // Asked to move to core 1 while its five wait in core 0's queue.
    atomic_store(&s->migrate_to, 1);
    order_sleep_ms(50);
    for (int i = 0; i < 5; i++) order_send(s, ORDER_EMIT, 0);
    int all = order_wait(&r->got, 10, 3000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(first);
    ASSERT_TRUE(all);
    ASSERT_EQ(0, atomic_load(&r->disorder));
    ASSERT_EQ(10, atomic_load(&r->last));
    free(r);
    free(s);
}

void register_sender_move_order_tests(void) {
    register_test_with_category("Moves keep a sender's order",
        test_sender_move_keeps_order, TEST_CATEGORY_RUNTIME);
}
