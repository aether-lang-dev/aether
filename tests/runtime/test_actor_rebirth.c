/**
 * Restart in place under concurrent senders (#2974).
 *
 * T panics every K messages; S, its supervisor on another core, gets the
 * Down and restarts T under the same ref (aether_actor_rebirth_begin, T's
 * "factory": the spawn of T's type takes T's block, its state is written,
 * scheduler_spawn_ready, aether_actor_rebirth_end), retrying through a
 * message to itself while a message sent before the death is still on its
 * way. Meanwhile three sender actors (one on T's core, one a step) and the
 * main thread each send T N numbered messages through the same ref.
 *
 * Every message is handled at most once and in its sender's order; handled
 * plus dropped (scheduler_dead_sends) is what was sent; no message is handled
 * by a half-made T; and no message a sender had finished sending when S
 * claimed T for the restart (the snapshot S takes then) is handled by the
 * new incarnation: it was queued at the death or sent while T was dead.
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define rb_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#include <sched.h>
#define rb_sleep_ms(ms) usleep((ms) * 1000)
#endif

#define RB_SENDERS 4          /* three actors and the main thread */
#define RB_N 1500
#define RB_K 150
#define RB_MAX_INC 64
#define RB_SPIN_NS 200000ULL

enum { RB_NUMBER = 1, RB_GO = 2, RB_WATCH = 3, RB_RETRY = 4 };

typedef struct {
    int type;
    int sender;
    int seq;
} RbMsg;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int ready;      // written by T's "state initializers"
    int count;      // messages this incarnation handled
    int residue;    // written by no initializer: 0 in each incarnation
} RbTarget;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* target;
    int sender;
} RbActor;

static ActorBase* g_target;
static atomic_int g_sent[RB_SENDERS];
static atomic_int g_last[RB_SENDERS];
static atomic_int g_handled;
static atomic_int g_disorder;
static atomic_int g_not_ready;
static atomic_int g_stale;          // handled by an incarnation it was sent before
static atomic_int g_restarts;
static atomic_int g_restart_errors;
static atomic_int g_senders_done;
static uint64_t g_send_until;       // the senders' deadline, set before they start
static int g_snapshot[RB_MAX_INC][RB_SENDERS];

static void rb_target_step(RbTarget* self) {
    aether_step_actor_set(self);
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    RbMsg m = *(RbMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);
    if (!self->ready) atomic_fetch_add(&g_not_ready, 1);
    int inc = atomic_load(&self->incarnation);
    if (inc > 0 && inc < RB_MAX_INC && m.seq <= g_snapshot[inc][m.sender])
        atomic_fetch_add(&g_stale, 1);
    if (m.seq <= atomic_load(&g_last[m.sender])) atomic_fetch_add(&g_disorder, 1);
    atomic_store(&g_last[m.sender], m.seq);
    atomic_fetch_add(&g_handled, 1);
    if (++self->count % RB_K == 0) aether_panic("rebirth test: planned failure");
}

static void rb_send_number(ActorBase* to, int sender, int seq) {
    RbMsg m = { RB_NUMBER, sender, seq };
    aether_send_message(to, &m, sizeof m);
}

static void rb_send(void* to, int type) {
    RbMsg m = { type, 0, 0 };
    aether_send_message(to, &m, sizeof m);
}

/* T's factory, as a generated spawn is: the spawn, its state, ready. */
static ActorBase* rb_spawn_target(int core) {
    RbTarget* t = (RbTarget*)scheduler_spawn_actor(core, (void (*)(void*))rb_target_step,
                                                   sizeof(RbTarget));
    t->ready = 1;
    t->count = 0;
    scheduler_spawn_ready((ActorBase*)t);
    return (ActorBase*)t;
}

static void rb_try_restart(RbActor* self) {
    int claimed = aether_actor_rebirth_begin(self->target);
    if (claimed == 0) {   // a message from before the death still on its way
        rb_send(self, RB_RETRY);
        return;
    }
    if (claimed < 0) {
        atomic_fetch_add(&g_restart_errors, 1);
        return;
    }
    // What each sender had finished sending when T was claimed.
    int next_inc = atomic_load(&self->target->incarnation) + 1;
    if (next_inc < RB_MAX_INC)
        for (int s = 0; s < RB_SENDERS; s++) g_snapshot[next_inc][s] = atomic_load(&g_sent[s]);
    ActorBase* again = rb_spawn_target(atomic_load(&self->target->assigned_core));
    if (again != self->target || aether_actor_rebirth_end(self->target) != 1) {
        atomic_fetch_add(&g_restart_errors, 1);
        return;
    }
    atomic_fetch_add(&g_restarts, 1);
    aether_actor_monitor(self->target);
}

static void rb_actor_step(RbActor* self) {
    aether_step_actor_set(self);
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    if (msg.type == AETHER_MSG_DOWN) {
        int ok = aether_down_accept((ActorBase*)self, &msg);
        aether_message_drop((ActorBase*)self, &msg);
        if (ok) rb_try_restart(self);
        return;
    }
    RbMsg m = *(RbMsg*)aether_message_payload(&msg);
    aether_message_free_payload(&msg);
    if (m.type == RB_WATCH) {
        aether_actor_monitor(self->target);
    } else if (m.type == RB_RETRY) {
        rb_try_restart(self);
    } else if (m.type == RB_GO) {
        // One number a step, then on: the core runs T and the others in
        // between, so the deaths and restarts fall among the sends.
        int seq = atomic_load(&g_sent[self->sender]) + 1;
        rb_send_number(self->target, self->sender, seq);
        atomic_store(&g_sent[self->sender], seq);
        // On past N until T has been restarted three times, or until a
        // deadline. A restart waits for the messages counted in flight for T
        // when it died, and one held in a core's overflow buffer arrives
        // only when that core's thread runs: a time slice of the OS on a busy
        // runner. The bound is in time for that reason (#3606): it was 40 N
        // sends, and a send to a dead T costs next to nothing, so the 60,000
        // were spent within one such wait and the senders stopped before
        // the restarts came (on 2 CPUs, 1 restart after 3.5 million claims).
        if (seq < RB_N || (atomic_load(&g_restarts) < 3 && aether_now_ns() < g_send_until))
            rb_send(self, RB_GO);
        else
            atomic_fetch_add(&g_senders_done, 1);
    }
}

static RbActor* rb_actor(int core, int sender) {
    RbActor* a = (RbActor*)scheduler_spawn_actor(core, (void (*)(void*))rb_actor_step, sizeof(RbActor));
    a->target = g_target;
    a->sender = sender;
    scheduler_spawn_ready((ActorBase*)a);
    return a;
}

static void test_rebirth_under_senders(void) {
    for (int s = 0; s < RB_SENDERS; s++) {
        atomic_store(&g_sent[s], 0);
        atomic_store(&g_last[s], 0);
    }
    atomic_store(&g_handled, 0);
    atomic_store(&g_disorder, 0);
    atomic_store(&g_not_ready, 0);
    atomic_store(&g_stale, 0);
    atomic_store(&g_restarts, 0);
    atomic_store(&g_restart_errors, 0);
    atomic_store(&g_senders_done, 0);
    memset(g_snapshot, 0, sizeof g_snapshot);

    scheduler_init(4);
    scheduler_start();
    uint64_t dead0 = scheduler_dead_sends();
    g_target = rb_spawn_target(0);
    RbActor* sup = rb_actor(3, 0);   // a core of its own: it restarts T while the others send
    rb_send(sup, RB_WATCH);
    scheduler_wait();
    RbActor* senders[3];
    for (int s = 0; s < 3; s++) senders[s] = rb_actor(s, s);
    g_send_until = aether_now_ns() + 10000ULL * 1000000ULL;
    for (int s = 0; s < 3; s++) rb_send(senders[s], RB_GO);
    for (int seq = 1; seq <= RB_N; seq++) {   // the main thread, sender 3
        rb_send_number(g_target, 3, seq);
        atomic_store(&g_sent[3], seq);
    }
    uint64_t until = aether_now_ns() + 20000ULL * 1000000ULL;
    while (atomic_load(&g_senders_done) < 3 && aether_now_ns() < until) rb_sleep_ms(1);
    scheduler_wait();
    int handled = atomic_load(&g_handled);
    int dropped = (int)(scheduler_dead_sends() - dead0);
    int restarts = atomic_load(&g_restarts);
    int inc = aether_actor_incarnation(g_target);
    int alive = (atomic_load(&g_target->dead) & AETHER_ACTOR_ENDED) == 0;
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(3, atomic_load(&g_senders_done));
    int sent = 0;
    for (int s = 0; s < RB_SENDERS; s++) sent += atomic_load(&g_sent[s]);
    ASSERT_EQ(sent, handled + dropped);
    ASSERT_EQ(0, atomic_load(&g_disorder));
    ASSERT_EQ(0, atomic_load(&g_not_ready));
    ASSERT_EQ(0, atomic_load(&g_stale));
    ASSERT_EQ(0, atomic_load(&g_restart_errors));
    ASSERT_TRUE(restarts >= 3);
    ASSERT_EQ(restarts, inc);
    ASSERT_EQ(handled / RB_K, restarts);   // one restart per planned failure
    ASSERT_TRUE(alive);
}

/* The claim, one step at a time: not before the death is finalized, not
 * while a message of the dead actor is counted in flight (here one counted
 * by hand, as a sender between its count and its delivery holds it); sends
 * while it is dead are dropped and counted, and the ones after the factory
 * returns reach the new incarnation, whose state is its own. */
static void test_rebirth_waits_for_in_flight(void) {
    scheduler_init(2);
    scheduler_start();
    ActorBase* t = rb_spawn_target(0);
    ((RbTarget*)t)->residue = 7;   // the old incarnation's state
    aether_actor_kill(t);
    uint64_t until = aether_now_ns() + 3000ULL * 1000000ULL;
    while (!(atomic_load(&t->exit_flags) & AETHER_EXIT_FINALIZED) && aether_now_ns() < until)
        rb_sleep_ms(1);
    int alive_claim = -2;
    {
        ActorBase* other = rb_spawn_target(1);
        alive_claim = aether_actor_rebirth_begin(other);   // alive: -1
    }
    uint64_t dead0 = scheduler_dead_sends();
    for (int i = 1; i <= 3; i++) rb_send_number(t, 3, i);
    scheduler_wait();
    int dropped = (int)(scheduler_dead_sends() - dead0);

    inflight_enter(t, 1);
    int while_in_flight = aether_actor_rebirth_begin(t);
    inflight_leave(t, 1);
    int claimed = aether_actor_rebirth_begin(t);
    ActorBase* again = rb_spawn_target(0);
    int residue_after = ((RbTarget*)again)->residue;
    int ended = aether_actor_rebirth_end(t);
    atomic_store(&g_last[3], 0);
    atomic_store(&g_handled, 0);
    for (int i = 1; i <= 2; i++) rb_send_number(t, 3, i);
    scheduler_wait();
    int handled = atomic_load(&g_handled);
    int inc = aether_actor_incarnation(t);
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(-1, alive_claim);
    ASSERT_EQ(3, dropped);
    ASSERT_EQ(0, while_in_flight);
    ASSERT_EQ(1, claimed);
    ASSERT_TRUE(again == t);
    ASSERT_EQ(0, residue_after);
    ASSERT_EQ(1, ended);
    ASSERT_EQ(2, handled);
    ASSERT_EQ(1, inc);
}

/* The death notice list survives a restart. A monitor or a link of the dead
 * child, from another actor's handler, takes the child's list and locks it
 * to read the death it told; the restart runs on the supervisor's thread
 * meanwhile. It freed the list, and such a caller locked freed memory and
 * read a freed reason. Now the list stays, emptied under its lock, and
 * tells the new incarnation's death in turn. */
static void test_rebirth_keeps_watch_list(void) {
    scheduler_init(2);
    scheduler_start();
    ActorBase* t = rb_spawn_target(0);
    aether_actor_kill(t);
    uint64_t until = aether_now_ns() + 3000ULL * 1000000ULL;
    while (!(atomic_load(&t->exit_flags) & AETHER_EXIT_FINALIZED) && aether_now_ns() < until)
        rb_sleep_ms(1);
    // The core that finalized it lets go of its step lock just after; the
    // claim takes it, so the core is let finish first (it counts the kill's
    // request done once it has).
    scheduler_wait();
    struct AetherWatch* before = atomic_load(&t->death_watch);
    const char* why = aether_actor_death_reason(t);
    int killed = why && strcmp(why, "killed") == 0;
    int claimed = aether_actor_rebirth_begin(t);
    ActorBase* again = rb_spawn_target(0);
    int ended = aether_actor_rebirth_end(t);
    struct AetherWatch* after = atomic_load(&t->death_watch);
    const char* why_after = aether_actor_death_reason(t);
    aether_actor_kill(t);
    until = aether_now_ns() + 3000ULL * 1000000ULL;
    while (!(atomic_load(&t->exit_flags) & AETHER_EXIT_FINALIZED) && aether_now_ns() < until)
        rb_sleep_ms(1);
    const char* why_again = aether_actor_death_reason(t);
    int killed_again = why_again && strcmp(why_again, "killed") == 0;
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(before != NULL);
    ASSERT_TRUE(killed);
    ASSERT_EQ(1, claimed);
    ASSERT_TRUE(again == t);
    ASSERT_EQ(1, ended);
    ASSERT_TRUE(after == before);        // the same list: a holder still holds it
    ASSERT_TRUE(why_after == NULL);      // emptied for the new incarnation
    ASSERT_TRUE(killed_again);           // which tells its own death through it
}

/* Holds the core that steps it until `release`. */
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int in_step;
    atomic_int release;
} RbBlocker;

static void rb_blocker_step(RbBlocker* b) {
    Message msg;
    if (mailbox_receive(&b->mailbox, &msg)) {
        aether_message_free_payload(&msg);
        atomic_store(&b->in_step, 1);
        for (int w = 0; w < 10000 && !atomic_load(&b->release); w++) rb_sleep_ms(1);
    }
    atomic_store_explicit(&b->active, b->mailbox.count > 0, memory_order_relaxed);
}

/* Sends the blocker its message from a thread of its own: a send from the
 * main thread would flush the main thread's pending batch first. */
static void* rb_go_thread(void* b) {
    rb_send(b, RB_GO);
    return NULL;
}

/* A main-thread batch (a fan-out loop's sends) flushed while a restart
 * claim is live. Its message, added while the child was alive, is counted
 * only at the flush, after the claim read the child's inflight count, so
 * nothing waits for it: the flush reads the child's death again (it does
 * while a claim is live) and drops it, counted. The child's core is held
 * meanwhile, so a message the flush let through would reach the child only
 * after the restart, and the new incarnation would handle a message sent
 * before it existed. */
static void test_rebirth_batched_send_during_claim(void) {
    scheduler_init(2);
    scheduler_start();
    ActorBase* t = rb_spawn_target(1);
    RbBlocker* b = (RbBlocker*)scheduler_spawn_actor(1, (void (*)(void*))rb_blocker_step,
                                                     sizeof(RbBlocker));
    atomic_init(&b->in_step, 0);
    atomic_init(&b->release, 0);
    scheduler_spawn_ready((ActorBase*)b);

    scheduler_send_batch_start();
    RbMsg m = { RB_NUMBER, 0, 1 };
    aether_send_message_batch(t, &m, sizeof m);   // t alive: held in the batch
    aether_actor_kill(t);
    int claimed = 0;
    for (int w = 0; w < 3000 && (claimed = aether_actor_rebirth_begin(t)) == 0; w++)
        rb_sleep_ms(1);
    pthread_t go;
    int go_started = pthread_create(&go, NULL, rb_go_thread, b) == 0;
    if (go_started) pthread_join(go, NULL);
    for (int w = 0; w < 3000 && !atomic_load(&b->in_step); w++) rb_sleep_ms(1);
    int held = atomic_load(&b->in_step);

    uint64_t dead0 = scheduler_dead_sends();
    scheduler_send_batch_flush();
    int dropped = (int)(scheduler_dead_sends() - dead0);
    ActorBase* again = claimed == 1 ? rb_spawn_target(1) : NULL;
    int ended = claimed == 1 ? aether_actor_rebirth_end(t) : 0;
    atomic_store(&b->release, 1);
    scheduler_wait();
    int handled = ((RbTarget*)t)->count;
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(held);
    ASSERT_EQ(1, claimed);
    ASSERT_EQ(1, dropped);
    ASSERT_TRUE(again == t);
    ASSERT_EQ(1, ended);
    ASSERT_EQ(0, handled);   // the new incarnation never saw it
}

/* A reborn actor's receive timeout (an `after`) is in its core's timer heap
 * once the restart returns (#3540). The restart tells the core of the
 * countdown while the actor is still dead (scheduler_spawn_ready), then
 * clears the death bits and tells it again (scheduler_actor_revived). A core
 * that read the death for the first entry, and took the entry after the
 * second push had found it there and added none, went by the death it had
 * read: the countdown left the heap and nothing put it back, so the `after`
 * never fired. First whole restarts, each followed by the countdown in the
 * heap; then the death and the revival alone, as the restart makes them,
 * without the finalize and the claim between, so the core meets the two
 * pushes some hundred thousand times, a varied moment apart. */
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int ready;
} RbTimed;

static void rb_timed_step(RbTimed* self) {
    Message msg;
    (void)mailbox_receive(&self->mailbox, &msg);
}

static void rb_arm(ActorBase* t) {
    t->timeout_ns = 60000ULL * 1000000ULL;   // a minute: in the heap, never due here
    t->last_activity_ns = aether_now_ns();
}

static ActorBase* rb_spawn_timed(int core) {
    RbTimed* t = (RbTimed*)scheduler_spawn_actor(core, (void (*)(void*))rb_timed_step,
                                                 sizeof(RbTimed));
    rb_arm((ActorBase*)t);
    t->ready = 1;
    scheduler_spawn_ready((ActorBase*)t);
    return (ActorBase*)t;
}

static int rb_wait_until(ActorBase* a, int finalized, int ms) {
    uint64_t start = aether_now_ns();
    uint64_t until = start + (uint64_t)ms * 1000000ULL;
    for (;;) {
        int ok = finalized ? (atomic_load(&a->exit_flags) & AETHER_EXIT_FINALIZED) != 0
                           : atomic_load(&a->ready_queued) == 0 && atomic_load(&a->timer_slot) != 0;
        if (ok) return 1;
        uint64_t now = aether_now_ns();
        if (now > until) return 0;
        if (now - start > RB_SPIN_NS) sched_yield();
    }
}

static void test_rebirth_keeps_receive_timeout(void) {
    scheduler_init(1);
    scheduler_start();
    ActorBase* t = rb_spawn_timed(0);
    int armed_first = rb_wait_until(t, 0, 3000);
    int restarts = 0, errors = 0, lost = 0;
    for (; restarts < 200; restarts++) {
        aether_actor_kill(t);
        if (!rb_wait_until(t, 1, 3000)) { errors++; break; }
        int claimed = 0;
        for (int w = 0; w < 1000000 && (claimed = aether_actor_rebirth_begin(t)) == 0; w++) {
        }
        if (claimed != 1) { errors++; break; }
        ActorBase* again = rb_spawn_timed(0);
        if (again != t || aether_actor_rebirth_end(t) != 1) { errors++; break; }
        if (!rb_wait_until(t, 0, 1000)) { lost++; break; }
    }
    int revivals = 0;
    uint64_t until = aether_now_ns() + 2000ULL * 1000000ULL;
    for (; !lost && !errors && revivals < 400000 && aether_now_ns() < until; revivals++) {
        atomic_fetch_or(&t->dead, AETHER_ACTOR_KILLED);
        rb_arm(t);
        scheduler_timeout_armed(t);   // told while dead, as scheduler_spawn_ready does
        for (volatile int k = 0; k < (revivals * 37) % 4000; k++) {
        }
        atomic_fetch_and(&t->dead, ~AETHER_ACTOR_KILLED);
        scheduler_actor_revived(t);
        if (!rb_wait_until(t, 0, 200)) lost++;
    }
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(armed_first);
    ASSERT_EQ(0, errors);
    if (lost) printf("    (the countdown was lost after %d restarts, %d revivals)\n", restarts, revivals);
    ASSERT_EQ(0, lost);
    ASSERT_EQ(200, restarts);
    ASSERT_TRUE(revivals > 1000);
}

void register_actor_rebirth_tests(void) {
    register_test_with_category("Rebirth: a reborn actor's receive timeout stays in its core's heap",
        test_rebirth_keeps_receive_timeout, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Rebirth: a batch flushed during a claim drops what it sends the child",
        test_rebirth_batched_send_during_claim, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Rebirth: the death notice list survives a restart",
        test_rebirth_keeps_watch_list, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Rebirth: the claim waits for messages in flight",
        test_rebirth_waits_for_in_flight, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Rebirth: a restart in place under concurrent senders",
        test_rebirth_under_senders, TEST_CATEGORY_RUNTIME);
}
