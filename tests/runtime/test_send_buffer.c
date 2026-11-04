// Test suite for the thread-local send buffer (runtime/actors/aether_send_buffer.c).
//
// Coverage motivation: gcov on `make ci-coverage` reported
// runtime/actors/aether_send_buffer.c at 0%: the public surface
// (`send_buffer_flush`, `send_buffered`, `send_buffer_force_flush`) is
// called only from runtime/examples/*.c bench programs, and the codegen
// never emits these calls. So the batched-send path was a CI dead zone.
//
// The buffer's own bookkeeping is checked on the test thread without a
// scheduler (nothing is flushed there). What a flush sends is checked under
// a running scheduler, from an actor's handler on a core (where real callers
// buffer) or from the test thread: since #3292 a flush sends every message it
// holds, through scheduler_send_many, as single sends would go. It used to
// keep what its same-core path could not place, and the buffer then grew past
// its end; to drop the part of a cross-core batch a full queue had no room
// for; and, on a target change with messages kept, to send them to the new
// target.

#include "test_harness.h"
#include "../../runtime/actors/aether_send_buffer.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define buf_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define buf_sleep_ms(ms) usleep((ms) * 1000)
#endif

// A plain ActorBase for the bookkeeping tests, never flushed to.
static ActorBase* make_idle_actor(void) {
    ActorBase* actor = calloc(1, sizeof(ActorBase));
    if (!actor) return NULL;
    actor->id = 1;
    mailbox_init(&actor->mailbox);
    return actor;
}

// The test thread's buffer, empty, with no target.
static void reset_send_buffer(void) {
    SendBuffer* b = send_buffer_here();
    b->target = NULL;
    b->count = 0;
}

// Test 1: send_buffer_flush is a no-op when the buffer is empty.
TEST_CATEGORY(send_buffer_flush_empty_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer();
    send_buffer_flush();
    ASSERT_EQ(0, send_buffer_here()->count);
    ASSERT_TRUE(send_buffer_here()->target == NULL);
}

// Test 2: send_buffer_flush is a no-op when target is NULL even with
// count > 0 (defensive: the API never leaves the buffer so).
TEST_CATEGORY(send_buffer_flush_null_target_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer();
    SendBuffer* b = send_buffer_here();
    b->target = NULL;
    b->count = 3;
    b->buffer[0] = message_create_simple(1, 0, 100);
    send_buffer_flush();
    ASSERT_EQ(3, b->count);
    reset_send_buffer();
}

// Test 3: send_buffered accumulates messages without flushing while the
// target is the same and count is below SEND_BUFFER_SIZE.
TEST_CATEGORY(send_buffered_accumulates, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer();
    ActorBase* actor = make_idle_actor();
    ASSERT_NOT_NULL(actor);

    for (int i = 0; i < 10; i++) {
        send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, i));
    }

    ASSERT_EQ(10, send_buffer_here()->count);
    ASSERT_EQ(10, send_buffer_pending());
    ASSERT_TRUE(send_buffer_here()->target == actor);
    ASSERT_EQ(0, atomic_load(&actor->active));  // not flushed → not activated

    reset_send_buffer();  // nothing was sent: the messages own nothing
    free(actor);
}

// Test 4: send_buffer_force_flush is a no-op when buffer is empty.
TEST_CATEGORY(send_buffer_force_flush_empty_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer();
    send_buffer_force_flush();
    ASSERT_EQ(0, send_buffer_here()->count);
    ASSERT_TRUE(send_buffer_here()->target == NULL);
}

// Test 5: send_buffer_pending reports the current count.
TEST_CATEGORY(send_buffer_pending_reports_count, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer();
    ActorBase* actor = make_idle_actor();
    ASSERT_NOT_NULL(actor);

    ASSERT_EQ(0, send_buffer_pending());
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 1));
    ASSERT_EQ(1, send_buffer_pending());
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 2));
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 3));
    ASSERT_EQ(3, send_buffer_pending());

    reset_send_buffer();
    free(actor);
}

// ---------------------------------------------------------------------------
// Flushes under a running scheduler (#3292)
//
// S, on a core, is sent a GO and buffers `count` numbered messages (in
// payload_int) to `to`, `to2` taking every other run of RUN when set, then
// force-flushes. A receiver checks that its numbers come one after another.
// ---------------------------------------------------------------------------

enum { BUF_NUMBER = 1, BUF_GO = 2 };
#define BUF_RUN 100

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    // as a sender
    ActorBase* to;
    ActorBase* to2;
    int count;
    int release_to;          // sets `to`'s release once it has sent
    atomic_int wrong_core;   // the buffer's core id changed under it
    atomic_int sent;
    // as a receiver
    int hold;                // the first number waits for `release`
    atomic_int release;
    atomic_int got;
    atomic_int last;
    atomic_int disorder;
} BufActor;

static void buf_step(BufActor* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    aether_message_free_payload(&msg);
    if (msg.type == BUF_GO) {
        int core = send_buffer_here()->core_id;
        int next[2] = { 0, 0 };
        for (int i = 0; i < self->count; i++) {
            int second = self->to2 && (i / BUF_RUN) % 2 == 1;
            ActorBase* to = second ? self->to2 : self->to;
            send_buffered((struct ActorBase*)to,
                          message_create_simple(BUF_NUMBER, 0, ++next[second]));
            if (send_buffer_here()->core_id != core) atomic_fetch_add(&self->wrong_core, 1);
        }
        send_buffer_force_flush();
        if (self->release_to) atomic_store(&((BufActor*)self->to)->release, 1);
        atomic_store(&self->sent, 1);
    } else if (msg.type == BUF_NUMBER) {
        if (self->hold && atomic_load(&self->got) == 0) {
            uint64_t until = aether_now_ns() + 5000ULL * 1000000ULL;
            while (!atomic_load(&self->release) && aether_now_ns() < until) buf_sleep_ms(1);
        }
        if (msg.payload_int != atomic_load(&self->last) + 1) atomic_fetch_add(&self->disorder, 1);
        atomic_store(&self->last, (int)msg.payload_int);
        atomic_fetch_add(&self->got, 1);
    }
    atomic_store_explicit(&self->active, self->mailbox.count > 0, memory_order_relaxed);
}

static BufActor* buf_actor(int core) {
    BufActor* a = calloc(1, sizeof(BufActor));
    static int next_id = 9300;
    a->id = next_id++;
    a->step = (void (*)(void*))buf_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_init(&a->reply_slot, NULL);
    atomic_flag_clear_explicit(&a->step_lock, memory_order_relaxed);
    mailbox_init(&a->mailbox);
    scheduler_register_actor((ActorBase*)a, core);
    return a;
}

static int buf_wait(atomic_int* v, int want, int limit_ms) {
    uint64_t until = aether_now_ns() + (uint64_t)limit_ms * 1000000ULL;
    while (atomic_load(v) < want && aether_now_ns() < until) buf_sleep_ms(1);
    return atomic_load(v) >= want;
}

static void buf_go(BufActor* s) {
    aether_send_message(s, &(int){ BUF_GO }, sizeof(int));
}

// Same core: S buffers 1000 to T, which its core cannot step while S's
// handler runs. A flush placed 32 in T's mailbox and kept the rest, the
// next placed none, and the buffer was written past its end.
TEST_CATEGORY(send_buffer_flush_same_core_sends_all, TEST_CATEGORY_RUNTIME) {
    scheduler_init(2);
    BufActor* t = buf_actor(1);
    BufActor* s = buf_actor(1);
    s->to = (ActorBase*)t;
    s->count = 1000;
    scheduler_start();

    buf_go(s);
    int sent = buf_wait(&s->sent, 1, 5000);
    int all = buf_wait(&t->got, 1000, 5000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(sent);
    ASSERT_TRUE(all);
    ASSERT_EQ(0, atomic_load(&s->wrong_core));
    ASSERT_EQ(1000, atomic_load(&t->got));
    ASSERT_EQ(1000, atomic_load(&t->last));
    ASSERT_EQ(0, atomic_load(&t->disorder));
    free(s);
    free(t);
}

// Another core: S buffers 3000 to T, whose core holds T's first number
// until S has sent, so the queue between the cores fills. A flush enqueued
// what fitted and dropped the rest, still counted in flight.
TEST_CATEGORY(send_buffer_flush_cross_core_keeps_overflow, TEST_CATEGORY_RUNTIME) {
    scheduler_init(2);
    BufActor* t = buf_actor(1);
    BufActor* s = buf_actor(0);
    t->hold = 1;
    s->to = (ActorBase*)t;
    s->count = 3000;
    s->release_to = 1;
    scheduler_start();

    buf_go(s);
    int sent = buf_wait(&s->sent, 1, 5000);
    int all = buf_wait(&t->got, 3000, 8000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(sent);
    ASSERT_TRUE(all);
    ASSERT_EQ(3000, atomic_load(&t->got));
    ASSERT_EQ(3000, atomic_load(&t->last));
    ASSERT_EQ(0, atomic_load(&t->disorder));
    free(s);
    free(t);
}

// A target change: S alternates runs of 100 between T1 and T2, all on S's
// core. Messages a flush kept went to the next target.
TEST_CATEGORY(send_buffer_flush_on_target_change, TEST_CATEGORY_RUNTIME) {
    scheduler_init(2);
    BufActor* t1 = buf_actor(1);
    BufActor* t2 = buf_actor(1);
    BufActor* s = buf_actor(1);
    s->to = (ActorBase*)t1;
    s->to2 = (ActorBase*)t2;
    s->count = 6 * BUF_RUN;
    scheduler_start();

    buf_go(s);
    int sent = buf_wait(&s->sent, 1, 5000);
    int all = buf_wait(&t1->got, 3 * BUF_RUN, 5000) && buf_wait(&t2->got, 3 * BUF_RUN, 5000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(sent);
    ASSERT_TRUE(all);
    ASSERT_EQ(3 * BUF_RUN, atomic_load(&t1->got));
    ASSERT_EQ(3 * BUF_RUN, atomic_load(&t2->got));
    ASSERT_EQ(0, atomic_load(&t1->disorder));
    ASSERT_EQ(0, atomic_load(&t2->disorder));
    free(s);
    free(t1);
    free(t2);
}

// From a thread that is no core's (here the test's own): the flush sends
// as the thread's single sends would, and a force flush leaves the buffer
// empty with no target. Coverage of that path only: the code before #3292
// passed it too.
TEST_CATEGORY(send_buffer_flush_from_foreign_thread, TEST_CATEGORY_RUNTIME) {
    scheduler_init(2);
    BufActor* t = buf_actor(0);
    scheduler_start();

    reset_send_buffer();
    for (int i = 1; i <= 700; i++)
        send_buffered((struct ActorBase*)t, message_create_simple(BUF_NUMBER, 0, i));
    send_buffer_force_flush();
    int pending = send_buffer_pending();
    void* target = send_buffer_here()->target;
    int all = buf_wait(&t->got, 700, 5000);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(0, pending);
    ASSERT_TRUE(target == NULL);
    ASSERT_TRUE(all);
    ASSERT_EQ(700, atomic_load(&t->last));
    ASSERT_EQ(0, atomic_load(&t->disorder));
    free(t);
}

void register_send_buffer_tests(void) {
    // Empty - tests auto-register via TEST_CATEGORY constructor on POSIX.
}
