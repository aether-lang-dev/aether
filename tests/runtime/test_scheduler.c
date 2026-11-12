/**
 * Aether Runtime Scheduler Tests
 * Tests multicore scheduler correctness and performance
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   // sched_getaffinity, pthread_getaffinity_np, CPU_COUNT (#3106)
#endif
#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_actor_thread.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/aether_process_mem.h"
#include "../../runtime/utils/aether_cpu_detect.h"
#include <stdatomic.h>


#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#define get_time_ms() GetTickCount64()
#else
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>   /* mprotect, for the #3521 guard */
#define sleep_ms(ms) usleep((ms) * 1000)
static long long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

// ============================================================================
// Test Actor Types
// ============================================================================

typedef struct {
    // The scheduler casts this to ActorBase*, so the prefix is the macro
    // rather than a hand-copied list that silently drifts when a field lands.
    AETHER_ACTOR_BASE_FIELDS
    // Test-specific fields below
    atomic_int count;
    atomic_int last_value;
} CounterActor;

typedef struct {
    // The scheduler casts this to ActorBase*, so the prefix is the macro
    // rather than a hand-copied list that silently drifts when a field lands.
    AETHER_ACTOR_BASE_FIELDS
    // Test-specific fields below
    atomic_int received[1000];
    atomic_int count;
} OrderActor;

// ============================================================================
// Actor Step Functions
// ============================================================================

// Each takes one message per call, the step contract (docs/c-embedding.md):
// the scheduler counts every call as one message processed, so a step that
// drained its mailbox left scheduler_wait() counting messages as never
// processed. These did, and passed only while every step found one message
// waiting; delivered a run at a time (#3090), a mailbox holds several.

void counter_step(CounterActor* self) {
    Message msg;
    if (mailbox_receive(&self->mailbox, &msg)) {
        atomic_fetch_add(&self->count, 1);
        atomic_store(&self->last_value, msg.payload_int);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

void order_step(OrderActor* self) {
    Message msg;
    if (mailbox_receive(&self->mailbox, &msg)) {
        int idx = atomic_load(&self->count);
        if (idx < 1000) {
            atomic_store(&self->received[idx], msg.payload_int);
        }
        atomic_fetch_add(&self->count, 1);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// A pair that answers: the reply target travels in payload_ptr, so the step
// function sends back on the same scheduler it was woken by. This is the one
// case the deleted tests/runtime/test_scheduler_correctness.c covered that
// nothing here did; unlike that file, the actor prefix is the shared macro.
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int pings_received;
    atomic_int pongs_sent;
} PingPongActor;

#define PINGPONG_PING 1
#define PINGPONG_PONG 2

void pingpong_step(PingPongActor* self) {
    Message msg;
    if (mailbox_receive(&self->mailbox, &msg)) {
        if (msg.type == PINGPONG_PING) {
            atomic_fetch_add(&self->pings_received, 1);
            PingPongActor* sender = (PingPongActor*)msg.payload_ptr;
            if (sender) {
                /* Both actors live on the same core, so this runs on the
                 * thread that owns the target too: the local queue is the
                 * path the runtime itself takes for a same-core send. */
                Message pong = message_create_simple(PINGPONG_PONG, self->id, 0);
                scheduler_send_local((ActorBase*)sender, pong);
                atomic_fetch_add(&self->pongs_sent, 1);
            }
        } else {
            atomic_fetch_add(&self->pings_received, 1);
        }
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// ============================================================================
// Test Cases
// ============================================================================

void test_mailbox_basic(void) {
    Mailbox mbox;
    mailbox_init(&mbox);
    
    // Send messages
    for (int i = 0; i < 10; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        ASSERT_TRUE(mailbox_send(&mbox, msg) == 1);
    }
    
    ASSERT_EQ(10, mbox.count);
    
    // Receive messages
    for (int i = 0; i < 10; i++) {
        Message msg;
        ASSERT_TRUE(mailbox_receive(&mbox, &msg) == 1);
        ASSERT_EQ(i, msg.payload_int);
    }
    
    ASSERT_EQ(0, mbox.count);
}

void test_mailbox_overflow(void) {
    Mailbox mbox;
    mailbox_init(&mbox);
    
    // Fill mailbox to capacity
    int sent = 0;
    for (int i = 0; i < MAILBOX_SIZE + 10; i++) {
        Message msg = message_create_simple(1, 0, i);
        if (mailbox_send(&mbox, msg)) {
            sent++;
        }
    }
    
    ASSERT_EQ(MAILBOX_SIZE, sent);
    ASSERT_EQ(MAILBOX_SIZE, mbox.count);
}

void test_scheduler_init_cleanup(void) {
    scheduler_init(2);

    ASSERT_EQ(2, num_cores);
    ASSERT_NOT_NULL(atomic_load(&schedulers[0].actor_table));
    ASSERT_NOT_NULL(atomic_load(&schedulers[1].actor_table));
    ASSERT_EQ(0, atomic_load(&schedulers[0].actor_count));
    ASSERT_EQ(0, atomic_load(&schedulers[1].actor_count));

    // Cleanup
    scheduler_cleanup();
}

// #2297: a library's actors in a host that never initialized the scheduler
// (a C program, or an Aether program with no actors of its own). The first
// spawn initializes and starts it; a later scheduler_init() is a no-op that
// leaves the live actors' tables alone.
void test_scheduler_spawn_on_demand(void) {
    ASSERT_EQ(0, num_cores);

    CounterActor* a = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    CounterActor* b = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    // The scheduler initializes the ActorBase prefix only; the generated
    // spawn function initializes an actor's own fields.
    atomic_store(&a->count, 0);
    atomic_store(&a->last_value, -1);
    atomic_store(&b->count, 0);
    atomic_store(&b->last_value, -1);
    int cores = num_cores;
    ASSERT_TRUE(cores > 0);

    scheduler_init(1);
    ASSERT_EQ(cores, num_cores);
    int registered = 0;
    for (int i = 0; i < num_cores; i++) registered += atomic_load(&schedulers[i].actor_count);
    ASSERT_EQ(2, registered);

    for (int i = 0; i < 50; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote((ActorBase*)a, msg, -1);
        scheduler_send_remote((ActorBase*)b, msg, -1);
    }
    for (int i = 0; i < 200 && (atomic_load(&a->count) < 50 || atomic_load(&b->count) < 50); i++) {
        sleep_ms(10);
    }
    int a_count = atomic_load(&a->count);
    int b_count = atomic_load(&b->count);

    scheduler_shutdown();
    scheduler_release_actor((ActorBase*)a);
    scheduler_release_actor((ActorBase*)b);
    scheduler_cleanup();

    ASSERT_EQ(50, a_count);
    ASSERT_EQ(50, b_count);
    ASSERT_EQ(0, num_cores);
}

void test_scheduler_spawn_placement(void) {
    scheduler_init(4);
    ActorBase* actors[10];
    int placements[4] = {0};
    for (int i = 0; i < 8; i++) {
        actors[i] = scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                          sizeof(CounterActor));
        ASSERT_NOT_NULL(actors[i]);
        int core = atomic_load(&actors[i]->assigned_core);
        ASSERT_TRUE(core >= 0 && core < 4);
        placements[core]++;
    }
    // Explicit placement and scheduler-thread child locality still win
    // over balancing. No messages are sent, so placement cannot migrate.
    actors[8] = scheduler_spawn_actor(3, (void (*)(void*))counter_step,
                                      sizeof(CounterActor));
    ASSERT_NOT_NULL(actors[8]);
    aether_core_id_set(2);
    actors[9] = scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                       sizeof(CounterActor));
    aether_core_id_set(-1);
    ASSERT_NOT_NULL(actors[9]);
    int explicit_core = atomic_load(&actors[8]->assigned_core);
    int child_core = atomic_load(&actors[9]->assigned_core);
    scheduler_shutdown();
    for (int i = 0; i < 10; i++) scheduler_release_actor(actors[i]);
    scheduler_cleanup();
    for (int i = 0; i < 4; i++) ASSERT_EQ(2, placements[i]);
    ASSERT_EQ(3, explicit_core);
    ASSERT_EQ(2, child_core);
}

// #2485: generated actor structs are declared aligned(64), so every actor
// scheduler_spawn_actor hands out must sit on a 64-byte boundary, whatever
// its size. malloc gave 16, and the actors are freed through the matching
// aligned free, which scheduler_release_actor exercises here.
void test_scheduler_spawn_aligned(void) {
    scheduler_init(2);
    const size_t sizes[] = {
        sizeof(ActorBase), sizeof(ActorBase) + 8, sizeof(CounterActor),
        sizeof(ActorBase) + 24, sizeof(ActorBase) + 200, sizeof(ActorBase) + 1000,
        sizeof(ActorBase) + 5000, sizeof(ActorBase) + 70000,
    };
    enum { PER_SIZE = 8, NSIZES = sizeof(sizes) / sizeof(sizes[0]) };
    ActorBase* actors[NSIZES * PER_SIZE];
    int misaligned = 0, missing = 0;
    for (int s = 0; s < NSIZES; s++) {
        for (int k = 0; k < PER_SIZE; k++) {
            ActorBase* a = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizes[s]);
            actors[s * PER_SIZE + k] = a;
            if (!a) { missing++; continue; }
            if ((uintptr_t)a % AETHER_ACTOR_ALIGN != 0) misaligned++;
            // The whole requested size is usable.
            memset((char*)a + sizeof(ActorBase), 0x5A, sizes[s] - sizeof(ActorBase));
        }
    }
    scheduler_shutdown();
    for (int i = 0; i < NSIZES * PER_SIZE; i++) scheduler_release_actor(actors[i]);
    scheduler_cleanup();
    ASSERT_EQ(0, missing);
    ASSERT_EQ(0, misaligned);
}

// #2486: a core's actor table grows past MAX_ACTORS_PER_CORE while its
// scheduler thread scans it without the lock, with messages flowing.
// Afterwards every actor is in exactly one table, every slot at or past a
// table's count is NULL (the grown tail included), and the replaced tables
// are still chained for scheduler_cleanup() to free. (A second reader,
// aether_scheduler_poll walking the tables from another thread, went with
// main-thread mode, #3112: the cores run every actor.)

void test_scheduler_actor_table_growth(void) {
    enum { N = 2 * MAX_ACTORS_PER_CORE + 100 };
    scheduler_init(2);
    scheduler_start();
    CounterActor** actors = calloc(N, sizeof(CounterActor*));
    int* seen = calloc(N, sizeof(int));
    ASSERT_NOT_NULL(actors);
    ASSERT_NOT_NULL(seen);

    int spawned = 0, sent = 0;
    for (int i = 0; i < N; i++) {
        // Just before a grow, leave a freed block of the new table's size
        // full of garbage, the block malloc is likeliest to hand back: a
        // grow that does not clear the new table's tail then shows it.
        AetherActorTable* t = atomic_load(&schedulers[0].actor_table);
        if (atomic_load(&schedulers[0].actor_count) == t->capacity) {
            size_t grown = sizeof(AetherActorTable) +
                           (size_t)t->capacity * 2 * sizeof(_Atomic(ActorBase*));
            void* dirty = malloc(grown);
            if (dirty) { memset(dirty, 0xA5, grown); free(dirty); }
        }
        CounterActor* a = (CounterActor*)scheduler_spawn_actor(
            0, (void (*)(void*))counter_step, sizeof(CounterActor));
        if (!a) break;
        atomic_init(&a->count, 0);
        atomic_init(&a->last_value, i);
        actors[i] = a;
        spawned++;
        // A sparse trickle: core 0 keeps going idle, which is when it scans.
        if (i % 50 == 49) {
            int target = (i * 7919) % (i + 1);
            Message msg = {1, 0, target, NULL, {NULL, 0, 0}, NULL};
            scheduler_send_remote((ActorBase*)actors[target], msg, -1);
            sent++;
        }
    }

    long processed = 0;
    for (int w = 0; w < 1000; w++) {
        processed = 0;
        for (int i = 0; i < spawned; i++) processed += atomic_load(&actors[i]->count);
        if (processed >= sent) break;
        sleep_ms(5);
    }
    scheduler_shutdown();

    // Threads are joined: the tables are quiescent.
    int registered = 0, bad_live = 0, bad_tail = 0, foreign = 0, chain_ok = 1;
    int core0_capacity = 0;
    for (int c = 0; c < num_cores; c++) {
        AetherActorTable* t = atomic_load(&schedulers[c].actor_table);
        int count = atomic_load(&schedulers[c].actor_count);
        if (count > t->capacity) { chain_ok = 0; continue; }
        registered += count;
        for (int i = 0; i < count; i++) {
            CounterActor* a = (CounterActor*)atomic_load(&t->slots[i]);
            if (!a) { bad_live++; continue; }
            int idx = atomic_load(&a->last_value);
            if (idx < 0 || idx >= spawned || actors[idx] != a) { foreign++; continue; }
            seen[idx]++;
        }
        for (int i = count; i < t->capacity; i++) {
            if (atomic_load(&t->slots[i]) != NULL) bad_tail++;
        }
        // Each retired table is half the one that replaced it, down to the
        // initial size.
        for (AetherActorTable* r = t; r->retired; r = r->retired) {
            if (r->retired->capacity * 2 != r->capacity) chain_ok = 0;
        }
        AetherActorTable* oldest = t;
        while (oldest->retired) oldest = oldest->retired;
        if (oldest->capacity != MAX_ACTORS_PER_CORE) chain_ok = 0;
        if (c == 0) core0_capacity = t->capacity;
    }
    int duplicated = 0, lost = 0;
    for (int i = 0; i < spawned; i++) {
        if (seen[i] > 1) duplicated++;
        if (seen[i] == 0) lost++;
    }

    for (int i = 0; i < spawned; i++) scheduler_release_actor((ActorBase*)actors[i]);
    scheduler_cleanup();
    free(actors);
    free(seen);

    ASSERT_EQ(N, spawned);
    ASSERT_EQ(sent, processed);
    ASSERT_EQ(N, registered);
    ASSERT_EQ(0, bad_live);
    ASSERT_EQ(0, foreign);
    ASSERT_EQ(0, duplicated);
    ASSERT_EQ(0, lost);
    ASSERT_EQ(0, bad_tail);
    ASSERT_TRUE(chain_ok);
    ASSERT_TRUE(core0_capacity >= 2 * MAX_ACTORS_PER_CORE);
}

// #2509: actors are spawned and released in rounds while every core scans
// its table.
// Half release themselves from their own step, the other half are released by
// the main thread right after their step ran, while the core that ran it may
// still be touching them. A release takes the actor out of its table at once
// and frees it only once no reader can hold it (ASan in CI turns a read of a
// freed actor into a failure here). Afterwards every count is back where it
// started and every released actor has been freed.
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int self_release;
} ChurnActor;

static atomic_int g_churn_handled;

static void churn_step(void* self) {
    ChurnActor* actor = (ChurnActor*)self;
    Message msg;
    // One message per call, as a generated step takes them.
    if (mailbox_receive(&actor->mailbox, &msg)) {
        int self_release = actor->self_release;
        atomic_fetch_add(&g_churn_handled, 1);
        if (self_release) scheduler_release_actor((ActorBase*)actor);
    }
}

static int churn_registered(void) {
    int total = 0;
    for (int c = 0; c < num_cores; c++) total += atomic_load(&schedulers[c].actor_count);
    return total;
}

void test_scheduler_release_churn(void) {
    enum { ROUNDS = 40, PER_ROUND = 250 };
    scheduler_init(4);
    scheduler_start();
    int registered_before = churn_registered();
    int live_before = atomic_load(&g_aether_config.actor_count);

    atomic_store(&g_churn_handled, 0);
    ChurnActor* batch[PER_ROUND];
    int spawned = 0, handled_all = 1, max_registered = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int start = atomic_load(&g_churn_handled);
        int made = 0;
        for (int k = 0; k < PER_ROUND; k++) {
            ChurnActor* a = (ChurnActor*)scheduler_spawn_actor(
                -1, churn_step, sizeof(ChurnActor));
            batch[k] = a;
            if (!a) continue;
            a->self_release = (k % 2 == 0);
            made++;
            spawned++;
            Message msg = {1, 0, k, NULL, {NULL, 0, 0}, NULL};
            scheduler_send_remote((ActorBase*)a, msg, -1);
        }
        int r = churn_registered();
        if (r > max_registered) max_registered = r;
        int waited = 0;
        while (atomic_load(&g_churn_handled) - start < made && waited < 5000) {
            sleep_ms(1);
            waited++;
        }
        if (atomic_load(&g_churn_handled) - start < made) handled_all = 0;
        // The rest are released from here, just after their step: the core
        // that ran it may still be in that loop iteration.
        // (The even ones may be freed already: they are not touched here.)
        for (int k = 1; k < PER_ROUND; k += 2) {
            if (batch[k]) scheduler_release_actor((ActorBase*)batch[k]);
        }
    }

    // Every core passes its quiescent point at least every PARK_MS_MAX.
    int pending = scheduler_released_actors_pending();
    for (int w = 0; w < 5000 && pending > 0; w++) {
        sleep_ms(1);
        pending = scheduler_released_actors_pending();
    }
    int registered_after = churn_registered();
    int live_after = atomic_load(&g_aether_config.actor_count);

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(ROUNDS * PER_ROUND, spawned);
    ASSERT_TRUE(handled_all);
    ASSERT_EQ(ROUNDS * PER_ROUND, atomic_load(&g_churn_handled));
    ASSERT_EQ(registered_before, registered_after);
    ASSERT_EQ(live_before, live_after);
    ASSERT_EQ(0, pending);
    // Never more than one round's worth in the tables at once.
    ASSERT_TRUE(max_registered <= registered_before + PER_ROUND);
}

// #2517: an actor with its own thread (auto_process, run by
// aether_actor_thread) reads actors from the tables in its step: the ones
// its handler sends to. It used to publish no reclamation epoch, so the core
// could free an actor it had just sent to, released meanwhile, while the
// step still held it. It now reads under an epoch like a core: nothing
// released is reclaimed until it passes the top of its loop again, which it
// cannot while its step is still running. The relay's step sends to the
// target, which its core steps and which releases itself in that step, and
// then waits at a gate, so the hold is observed with the thread
// deterministically inside its step; the core's published epoch says when it
// has tried to reclaim.
//
// The relay sends as an actor thread does since #3246: through the core's
// queue (it is not its core's thread, so it steps nothing inline). This test
// called scheduler_send_local and took the target as stepped inline by the
// time the relay reported its send; the core steps it, a little later, and
// the check read 0 when it came first (#3314). It waits for that step now.
//
// Then the relay itself is released by an actor the core steps, while the
// relay's step is waiting on that very core: a release never blocks, the
// relay's own thread ends the actor once it leaves its loop. Last, an actor
// thread's actor releases itself from its own step.
//
// What the actor threads read lives off this function's stack, and every
// assertion comes after both threads are joined (#3314): a failed one
// returned while the relay's thread still polled a gate on this stack.
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* target;
    ActorBase* echo;
    atomic_int* gate;
    atomic_int stepped;
    atomic_int echoed;     // the core answered while this step waited on it
} RelayActor;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* victim;     // released from this step, which a core runs
    atomic_int count;
} EchoActor;

static atomic_int g_relay_gate;

static void echo_step(void* self) {
    EchoActor* a = (EchoActor*)self;
    Message msg;
    if (mailbox_receive(&a->mailbox, &msg)) {
        if (a->victim) scheduler_release_actor(a->victim);
        atomic_fetch_add(&a->count, 1);
    }
    atomic_store_explicit(&a->active, (a->mailbox.count > 0), memory_order_relaxed);
}

static void relay_step(void* self) {
    RelayActor* a = (RelayActor*)self;
    Message msg;
    if (mailbox_receive(&a->mailbox, &msg)) {
        // Counted handled by the actor thread's loop, which steps it (#3464;
        // a core's count is its own thread's to write, #3541).
        Message fwd = {1, 0, 0, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote(a->target, fwd, -1);   // the core steps it
        atomic_store(&a->stepped, 1);
        for (int w = 0; w < 10000 && !atomic_load(a->gate); w++) sleep_ms(1);
        // The core steps echo, which releases this actor while this step is
        // still waiting on it.
        EchoActor* echo = (EchoActor*)a->echo;
        scheduler_send_remote(a->echo, fwd, -1);
        for (int w = 0; w < 5000 && atomic_load(&echo->count) < 1; w++) sleep_ms(1);
        if (atomic_load(&echo->count) == 1) atomic_store(&a->echoed, 1);
    }
}

static void quitter_step(void* self) {
    ChurnActor* a = (ChurnActor*)self;
    Message msg;
    if (mailbox_receive(&a->mailbox, &msg)) {
        atomic_fetch_add(&g_churn_handled, 1);   // counted handled by its thread's loop
        scheduler_release_actor((ActorBase*)a);   // from its own thread's step
    }
}

// Waits until core 0 has published an epoch past `seen`, so it has tried to
// reclaim since; 0 when it does not within 5 s.
static int core0_moved_past(uint64_t seen) {
    for (int w = 0; w < 5000; w++) {
        if (atomic_load(&schedulers[0].reclaim_epoch) > seen) return 1;
        sleep_ms(1);
    }
    return 0;
}

static int wait_no_pending(void) {
    int pending = scheduler_released_actors_pending();
    for (int w = 0; w < 5000 && pending > 0; w++) {
        sleep_ms(1);
        pending = scheduler_released_actors_pending();
    }
    return pending;
}

void test_scheduler_actor_thread_holds_released_actor(void) {
    scheduler_init(1);   // one core: every actor is the actor thread's core-mate
    scheduler_start();

    ChurnActor* target = (ChurnActor*)scheduler_spawn_actor(-1, churn_step, sizeof(ChurnActor));
    EchoActor* echo = (EchoActor*)scheduler_spawn_actor(-1, echo_step, sizeof(EchoActor));
    RelayActor* relay = (RelayActor*)scheduler_spawn_actor(-1, relay_step, sizeof(RelayActor));
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(echo);
    ASSERT_NOT_NULL(relay);
    target->self_release = 1;
    atomic_init(&echo->count, 0);
    atomic_store(&g_relay_gate, 0);
    relay->target = (ActorBase*)target;
    relay->echo = (ActorBase*)echo;
    relay->gate = &g_relay_gate;
    atomic_init(&relay->stepped, 0);
    atomic_init(&relay->echoed, 0);
    echo->victim = (ActorBase*)relay;
    relay->auto_process = 1;
    int relay_started = pthread_create(&relay->thread, NULL, aether_actor_thread, relay) == 0;

    atomic_store(&g_churn_handled, 0);
    Message msg = {1, 0, 0, NULL, {NULL, 0, 0}, NULL};
    if (relay_started) scheduler_send_remote((ActorBase*)relay, msg, -1);
    for (int w = 0; w < 5000 && !atomic_load(&relay->stepped); w++) sleep_ms(1);
    int stepped = atomic_load(&relay->stepped);
    // The target ran, on its core, and released itself (just after it
    // counted itself handled); the relay's step, still at the gate, holds it.
    for (int w = 0; w < 5000 && atomic_load(&g_churn_handled) < 1; w++) sleep_ms(1);
    int handled = atomic_load(&g_churn_handled);
    for (int w = 0; w < 5000 && scheduler_released_actors_pending() < 1; w++) sleep_ms(1);
    int pending_target = scheduler_released_actors_pending();

    // Further releases move the epoch on; the core publishes each move and
    // tries to reclaim. The target stays because the relay thread's epoch
    // is older than its release.
    int held = 1, moved = 1, spawned = 1;
    for (int round = 1; round <= 3; round++) {
        uint64_t seen = atomic_load(&schedulers[0].reclaim_epoch);
        ActorBase* extra = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
        if (!extra) { spawned = 0; break; }
        scheduler_release_actor(extra);
        if (!core0_moved_past(seen)) moved = 0;
        if (scheduler_released_actors_pending() != round + 1) held = 0;
    }

    // The relay's step goes on to wait on the core, which releases it
    // meanwhile; the release returns at once, the step finishes, the relay's
    // thread leaves its loop and ends the actor, and everything is reclaimed.
    atomic_store(&g_relay_gate, 1);
    for (int w = 0; w < 6000 && !atomic_load(&relay->echoed); w++) sleep_ms(1);
    int echoed = atomic_load(&relay->echoed);
    int pending_after_relay = wait_no_pending();
    if (relay_started) pthread_join(relay->thread, NULL);

    // An actor thread's actor releasing itself from its own step.
    ChurnActor* quitter = (ChurnActor*)scheduler_spawn_actor(-1, quitter_step, sizeof(ChurnActor));
    int quitter_started = 0;
    if (quitter) {
        quitter->auto_process = 1;
        quitter_started = pthread_create(&quitter->thread, NULL, aether_actor_thread, quitter) == 0;
        if (quitter_started) scheduler_send_remote((ActorBase*)quitter, msg, -1);
    }
    for (int w = 0; w < 5000 && atomic_load(&g_churn_handled) < 2; w++) sleep_ms(1);
    int handled_quitter = atomic_load(&g_churn_handled);
    int pending_after_quitter = wait_no_pending();
    if (quitter_started) pthread_join(quitter->thread, NULL);

    scheduler_shutdown();
    scheduler_release_actor((ActorBase*)echo);
    scheduler_cleanup();

    ASSERT_TRUE(relay_started);
    ASSERT_EQ(1, stepped);
    ASSERT_EQ(1, handled);
    ASSERT_EQ(1, pending_target);
    ASSERT_TRUE(spawned);
    ASSERT_TRUE(moved);
    ASSERT_TRUE(held);
    ASSERT_EQ(1, echoed);
    ASSERT_EQ(0, pending_after_relay);
    ASSERT_NOT_NULL(quitter);
    ASSERT_TRUE(quitter_started);
    ASSERT_EQ(2, handled_quitter);
    ASSERT_EQ(0, pending_after_quitter);
}

// #2517: a send to a released actor is defined. The release contract forbids
// it, but a raw ActorBase* cannot be revoked, so a released actor's block is
// kept, marked released, until a spawn of the same size takes it back: the
// late send reads the mark, drops the message and counts it. Every send
// entry is tried after the actor has been reclaimed, under the plain build.
void test_scheduler_late_send_after_release(void) {
    scheduler_init(2);
    scheduler_start();

    CounterActor* a = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    atomic_init(&a->count, 0);
    atomic_init(&a->last_value, 0);
    Message msg = {1, 0, 7, NULL, {NULL, 0, 0}, NULL};
    scheduler_send_remote((ActorBase*)a, msg, -1);
    for (int w = 0; w < 5000 && atomic_load(&a->count) < 1; w++) sleep_ms(1);
    ASSERT_EQ(1, atomic_load(&a->count));

    uint64_t dropped_before = scheduler_released_sends();
    scheduler_release_actor((ActorBase*)a);
    int pending = scheduler_released_actors_pending();
    for (int w = 0; w < 5000 && pending > 0; w++) {
        sleep_ms(1);
        pending = scheduler_released_actors_pending();
    }
    ASSERT_EQ(0, pending);   // reclaimed: the block is now a kept one

    // Late sends through each entry point. The block is kept, so reading it
    // is defined, and each is dropped.
    ASSERT_EQ(AETHER_ACTOR_RELEASED, atomic_load(&a->dead));
    scheduler_send_remote((ActorBase*)a, msg, -1);
    scheduler_send_batch_start();
    scheduler_send_batch_add((ActorBase*)a, msg);
    scheduler_send_batch_flush();
    struct { int id; intptr_t value; } typed = {1, 7};
    aether_send_message(a, &typed, sizeof typed);
    ASSERT_EQ(dropped_before + 3, scheduler_released_sends());
    ASSERT_EQ(1, atomic_load(&a->count));
    ASSERT_EQ(0, atomic_load(&a->mailbox.count));

    // The next spawn of the size takes the block back as a live actor.
    CounterActor* b = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(b == a);
    ASSERT_EQ(0, atomic_load(&b->dead));
    atomic_store(&b->count, 0);
    scheduler_send_remote((ActorBase*)b, msg, -1);
    for (int w = 0; w < 5000 && atomic_load(&b->count) < 1; w++) sleep_ms(1);
    ASSERT_EQ(1, atomic_load(&b->count));
    ASSERT_EQ(dropped_before + 3, scheduler_released_sends());

    scheduler_shutdown();
    scheduler_release_actor((ActorBase*)b);
    scheduler_cleanup();
}

// #3090: a released actor's block is not handed to a new actor while a
// message sent to it before the release is still in a core's queue. The
// scheduler is initialised but not started, so the messages wait there,
// counted in the actor's `inflight`; the actor is released and reclaimed,
// and an actor of the same size is spawned. Given the old block, it took
// those messages as its own once the cores ran, and their uncounts took its
// count below zero, which reads as a move in progress.
void test_scheduler_reuse_waits_for_messages_in_flight(void) {
    scheduler_init(2);
    // No spawn starts the cores: they start when asked.
    CounterActor* a = (CounterActor*)scheduler_spawn_actor(1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    atomic_init(&a->count, 0);
    for (int i = 0; i < 10; i++) {
        scheduler_send_remote((ActorBase*)a, message_create_simple(1, 0, i), -1);
    }
    int counted = atomic_load(&a->inflight);
    scheduler_release_actor((ActorBase*)a);
    int pending = scheduler_reclaim_released();

    CounterActor* b = (CounterActor*)scheduler_spawn_actor(1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    int fresh = b != NULL && b != a;
    if (b) atomic_init(&b->count, 0);
    scheduler_start();
    scheduler_wait();   // returns: the ten were dropped and credited
    int stolen = b ? atomic_load(&b->count) : -1;
    int b_inflight = b ? atomic_load(&b->inflight) : -1;
    int a_inflight = atomic_load(&a->inflight);

    scheduler_shutdown();
    if (b) scheduler_release_actor((ActorBase*)b);
    scheduler_cleanup();
    ASSERT_EQ(10, counted);
    ASSERT_EQ(0, pending);
    ASSERT_TRUE(fresh);        // the old block still had messages on their way
    ASSERT_EQ(0, stolen);      // none of them reached the new actor
    ASSERT_EQ(0, b_inflight);
    ASSERT_EQ(0, a_inflight);  // dropped against the released block, uncounted
}

void test_scheduler_basic_messaging(void) {
    scheduler_init(2);
    
    CounterActor* actor = malloc(sizeof(CounterActor));
    memset(actor, 0, sizeof(CounterActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))counter_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    atomic_store(&actor->last_value, -1);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send messages via remote queue (thread-safe)
    for (int i = 0; i < 100; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for processing
    for (int i = 0; i < 100 && atomic_load(&actor->count) < 100; i++) {
        sleep_ms(10);
    }
    
    int final_count = atomic_load(&actor->count);
    int final_last = atomic_load(&actor->last_value);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_EQ(100, final_count);
    ASSERT_EQ(99, final_last);
    
    free(actor);
    // Freed by scheduler_cleanup()
    // Freed by scheduler_cleanup()
}

void test_scheduler_high_throughput(void) {
    scheduler_init(2);
    
    CounterActor* actor = malloc(sizeof(CounterActor));
    memset(actor, 0, sizeof(CounterActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))counter_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    atomic_store(&actor->last_value, -1);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send many messages (reduced from 10000 to avoid overwhelming queue)
    const int TOTAL = 1000;
    long long start = get_time_ms();
    
    for (int i = 0; i < TOTAL; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for all messages to be processed (up to 10 seconds)
    for (int i = 0; i < 1000 && atomic_load(&actor->count) < TOTAL; i++) {
        sleep_ms(10);
    }
    
    long long end = get_time_ms();
    double elapsed = (end - start) / 1000.0;
    
    int final_count = atomic_load(&actor->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_EQ(TOTAL, final_count);
    
    // Report throughput
    if (elapsed > 0) {
        double throughput = final_count / elapsed;
        printf(" [%.0f msg/sec]", throughput);
    }
    
    free(actor);
    // Freed by scheduler_cleanup()
    // Freed by scheduler_cleanup()
}

void test_scheduler_message_ordering(void) {
    scheduler_init(2);
    
    OrderActor* actor = malloc(sizeof(OrderActor));
    memset(actor, 0, sizeof(OrderActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))order_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);

    for (int i = 0; i < 1000; i++) {
        atomic_store(&actor->received[i], -1);
    }
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send ordered messages
    for (int i = 0; i < 100; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for processing
    for (int i = 0; i < 100 && atomic_load(&actor->count) < 100; i++) {
        sleep_ms(10);
    }
    
    int final_count = atomic_load(&actor->count);
    ASSERT_EQ(100, final_count);
    
    // Check ordering
    int ordered = 1;
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&actor->received[i]) != i) {
            ordered = 0;
            break;
        }
    }
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(ordered);
    
    free(actor);
    // Freed by scheduler_cleanup()
    // Freed by scheduler_cleanup()
}

void test_scheduler_cross_core(void) {
    scheduler_init(4);
    
    CounterActor* actor0 = malloc(sizeof(CounterActor));
    memset(actor0, 0, sizeof(CounterActor));  // Zero all fields including pthread_t
    CounterActor* actor1 = malloc(sizeof(CounterActor));
    memset(actor1, 0, sizeof(CounterActor));  // Zero all fields including pthread_t

    actor0->id = 1;
    atomic_init(&actor0->active, 0);
    actor0->step = (void (*)(void*))counter_step;
    actor0->auto_process = 0;
    atomic_init(&actor0->migrate_to, -1);
    atomic_store(&actor0->count, 0);
    mailbox_init(&actor0->mailbox);

    actor1->id = 2;
    atomic_init(&actor1->active, 0);
    actor1->step = (void (*)(void*))counter_step;
    actor1->auto_process = 0;
    atomic_init(&actor1->migrate_to, -1);
    atomic_store(&actor1->count, 0);
    mailbox_init(&actor1->mailbox);
    
    scheduler_register_actor((ActorBase*)actor0, 0);
    scheduler_register_actor((ActorBase*)actor1, 1);
    scheduler_start();
    
    // Send messages to actor on different core
    for (int i = 0; i < 100; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor1, msg, 0);
    }

    // Wait for processing (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&actor1->count) < 100; i++) {
        sleep_ms(1);
    }
    
    int count0 = atomic_load(&actor0->count);
    int count1 = atomic_load(&actor1->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_EQ(0, count0);
    ASSERT_EQ(100, count1);
    
    free(actor0);
    free(actor1);
    for (int i = 0; i < 4; i++) {
        // Freed by scheduler_cleanup()
    }
}

void test_scheduler_exit_clean(void) {
    scheduler_init(2);
    scheduler_start();
    
    // Let threads run briefly
    sleep_ms(50);
    
    // Should exit cleanly without hanging
    long long start = get_time_ms();
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    long long end = get_time_ms();
    long long elapsed = end - start;
    
    // Should exit within 2000ms (generous for Valgrind which runs ~10x slower)
    ASSERT_TRUE(elapsed < 2000);
    
    // Freed by scheduler_cleanup()
    // Freed by scheduler_cleanup()
}

void test_scheduler_backpressure(void) {
    scheduler_init(2);
    
    CounterActor* actor = calloc(1, sizeof(CounterActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))counter_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send MORE messages than mailbox can hold to test backpressure
    for (int i = 0; i < 200; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for processing
    for (int i = 0; i < 200 && atomic_load(&actor->count) < 200; i++) {
        sleep_ms(10);
    }
    
    int final_count = atomic_load(&actor->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    // With backpressure, should process all or nearly all messages
    // Allow some loss (< 5%) in extreme scenarios
    ASSERT_TRUE(final_count >= 190);
    
    free(actor);
    // Freed by scheduler_cleanup()
    // Freed by scheduler_cleanup()
}

// ============================================================================
// Test Registration
// ============================================================================

void test_scheduler_bidirectional(void) {
    scheduler_init(2);

    PingPongActor* a = malloc(sizeof(PingPongActor));
    PingPongActor* b = malloc(sizeof(PingPongActor));
    memset(a, 0, sizeof(PingPongActor));
    memset(b, 0, sizeof(PingPongActor));

    a->id = 1;
    a->step = (void (*)(void*))pingpong_step;
    atomic_init(&a->migrate_to, -1);
    mailbox_init(&a->mailbox);

    b->id = 2;
    b->step = (void (*)(void*))pingpong_step;
    atomic_init(&b->migrate_to, -1);
    mailbox_init(&b->mailbox);

    scheduler_register_actor((ActorBase*)a, 0);
    scheduler_register_actor((ActorBase*)b, 0);
    scheduler_start();

    /* Through the scheduler, not into the mailbox: the mailbox is drained by
     * the scheduler thread, and pushing into it from here races that reader.
     * (Doing exactly that is what made the deleted test drop messages.) */
    const int pings = 50;
    for (int i = 0; i < pings; i++) {
        Message msg = message_create_simple(PINGPONG_PING, a->id, 0);
        msg.payload_ptr = a;                 /* answer here */
        scheduler_send_remote((ActorBase*)b, msg, -1);
    }

    for (int i = 0; i < 10000 && atomic_load(&a->pings_received) < pings; i++) {
        sleep_ms(1);
    }

    int b_got = atomic_load(&b->pings_received);
    int b_sent = atomic_load(&b->pongs_sent);
    int a_got = atomic_load(&a->pings_received);

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(pings, b_got);
    ASSERT_EQ(pings, b_sent);
    ASSERT_EQ(pings, a_got);

    free(a);
    free(b);
}

// A scheduler stopped with actors still alive (a host's aether_main_exit, a
// program that never releases its workers) frees the actors it spawned along
// with its tables, which were their last reference: run again, the first
// run's actors leaked (macOS leaks, emit_lib_keeps_main). An actor a caller
// registered is the caller's and is left alone.
static CounterActor g_caller_actor;

static void teardown_cycle(void) {
    scheduler_init(2);
    scheduler_start();
    const size_t sizes[] = { sizeof(CounterActor), sizeof(ActorBase) + 1000, sizeof(OrderActor) };
    for (int s = 0; s < 3; s++) {
        for (int k = 0; k < 4; k++) {
            ActorBase* a = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizes[s]);
            ASSERT_NOT_NULL(a);
            Message msg = message_create_simple(1, 0, k);
            scheduler_send_remote(a, msg, -1);
        }
    }
    scheduler_register_actor((ActorBase*)&g_caller_actor, -1);
    scheduler_wait();
    scheduler_shutdown();
    scheduler_cleanup();
}

// The heap's growth over two equal windows of five cycles, after a warm one
// (first-use allocations of the runtime). Meaningful where
// aether_heap_in_use_exact() holds, and there one window must not grow at
// all: a leak in every cycle grows both, a one-time change at most one.
// On Windows the walk counts the system's blocks too, and some of those
// change once in a while with nothing leaking, each time by itself: a
// critical section a system DLL keeps for the process (ucrtbase's, seen
// here) gets its 48-byte debug record the first time it is contended;
// ntdll keeps the records of a few deleted critical sections for reuse, so
// it holds 48 bytes more the first time more of them are in use at once
// than before; and the loader's worker threads, which aether_thread_epoch()
// does not count, free about 1 KB each when they leave, some 30 s after the
// last library load. A leak that takes a race grows only the windows the
// race is lost in, so a test that wants one caught makes its race happen in
// every cycle (test_scheduler_teardown_destroys_core_locks).
static int heap_steady_over_cycles(void (*cycle)(void), const char* what) {
    cycle();
    int64_t start = aether_heap_in_use();
    for (int round = 0; round < 5; round++) cycle();
    int64_t mid = aether_heap_in_use();
    for (int round = 0; round < 5; round++) cycle();
    int64_t end = aether_heap_in_use();
    if (!aether_heap_in_use_exact()) return 1;
    if (mid - start == 0 || end - mid == 0) return 1;
    fprintf(stderr, "    %s grew the heap: %lld then %lld bytes\n", what,
            (long long)(mid - start), (long long)(end - mid));
    return 0;
}

void test_scheduler_teardown_frees_live_actors(void) {
    memset(&g_caller_actor, 0, sizeof(g_caller_actor));
    mailbox_init(&g_caller_actor.mailbox);
    g_caller_actor.step = (void (*)(void*))counter_step;
    atomic_store(&g_caller_actor.last_value, 4242);

    int steady = heap_steady_over_cycles(teardown_cycle, "teardown cycles");

    // The caller's actor was run and never freed.
    ASSERT_EQ(0, g_caller_actor.scheduler_owned);
    ASSERT_EQ(4242, atomic_load(&g_caller_actor.last_value));
    ASSERT_TRUE(steady);
}

// A lifecycle's core locks end with it. scheduler_init() made each core's
// park and foreign-send locks again without the previous lifecycle's being
// destroyed, and on Windows a critical section that has been contended
// holds a heap block (its 48-byte debug record) until it is deleted, so
// every lifecycle in which a lock had been contended lost one. The test
// above saw that only when wakes happened to meet a core at its park in
// both of its windows: on a busy runner (240 then 480 bytes on the 0.813.0
// Windows release build) and now and then locally. Here the core's
// foreign-send lock is contended in every cycle: this thread holds it while
// another thread sends through it, and lets go once the sender is waiting.
// One core and one lock: ntdll keeps the records of a few deleted critical
// sections for reuse and holds one more each time more of them are in use
// at once than ever before (see heap_steady_over_cycles), and contending
// four cores' locks at once, with parks contended by chance on top, raised
// that count in both windows in 2 runs of 200.
typedef struct {
    ActorBase* target;
    atomic_int sending;
} LockContender;

static void* lock_contender(void* arg) {
    LockContender* c = (LockContender*)arg;
    atomic_store(&c->sending, 1);
    scheduler_send_remote(c->target, message_create_simple(1, 0, 1), -1);
    return NULL;
}

static long long monotonic_us(void) {
#ifdef _WIN32
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (long long)(n.QuadPart / f.QuadPart) * 1000000LL +
           (long long)(n.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

static void yield_cpu(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void contended_lock_cycle(void) {
    scheduler_init(1);
    scheduler_start();
    LockContender c;
    atomic_init(&c.sending, 0);
    c.target = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
    ASSERT_NOT_NULL(c.target);
    atomic_store(&((CounterActor*)c.target)->count, 0);   // spawn sets the ActorBase prefix only
    pthread_mutex_lock(&schedulers[0].foreign_lock);
    pthread_t sender;
    int started = pthread_create(&sender, NULL, lock_contender, &c) == 0;
    // The sender is on its way to the lock: it is let go of a millisecond
    // later, by when the sender has stopped spinning on it and waits.
    while (started && !atomic_load(&c.sending)) yield_cpu();
    long long until = monotonic_us() + 1000;
    while (started && monotonic_us() < until) yield_cpu();
    pthread_mutex_unlock(&schedulers[0].foreign_lock);
    if (started) pthread_join(sender, NULL);
    ASSERT_TRUE(started);
    scheduler_wait();
    ASSERT_EQ(1, atomic_load(&((CounterActor*)c.target)->count));
    scheduler_shutdown();
    scheduler_cleanup();
}

void test_scheduler_teardown_destroys_core_locks(void) {
    ASSERT_TRUE(heap_steady_over_cycles(contended_lock_cycle, "contended lock cycles"));
}

// An actor with its own thread (#2517) that is alive when the scheduler
// stops: teardown marks it released instead of freeing it under the thread,
// and the second of teardown and the thread's exit ends it; the next
// scheduler's teardown reclaims what was retired.
static void live_actor_thread_cycle(void) {
    scheduler_init(1);
    // An actor thread runs while its core does, so the core threads must be
    // up.
    scheduler_start();
    ASSERT_TRUE(atomic_load(&schedulers[0].running));
    CounterActor* a = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    atomic_store(&a->count, 0);   // spawn sets the ActorBase prefix only
    a->auto_process = 1;
    ASSERT_EQ(0, pthread_create(&a->thread, NULL, aether_actor_thread, a));
    Message msg = message_create_simple(1, 0, 7);
    scheduler_send_remote((ActorBase*)a, msg, -1);
    for (int w = 0; w < 5000 && atomic_load(&a->count) < 1; w++) sleep_ms(1);
    ASSERT_EQ(1, atomic_load(&a->count));

    pthread_t t = a->thread;
    scheduler_shutdown();
    // Teardown runs while the thread may be leaving its loop and ending the
    // actor, which deregisters it against the table teardown takes away.
    scheduler_cleanup();
    pthread_join(t, NULL);
}

void test_scheduler_teardown_with_live_actor_thread(void) {
    // Repeated, so the thread's exit lands before, during and after the
    // teardown of its table in some cycle.
    for (int cycle = 0; cycle < 20; cycle++) live_actor_thread_cycle();
    scheduler_init(1);       // the next teardown reclaims what the threads retired
    scheduler_cleanup();
}

// #2592: an actor thread with nothing to do sleeps instead of spinning for
// as long as the actor lives, and a message and its release each wake it.
// The park's timed wait is set to a minute, so a wake that never came shows
// as a stall of a minute rather than as a 32 ms delay; the waits below give
// up after 10 s. They are bounded by the clock: sleep_ms(1) is a timer tick,
// about 15 ms, on Windows, so a bound in iterations is not a bound in time.
static int wait_thread_parked(ActorBase* a) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (!atomic_load(&a->thread_parked)) {
        if ((long long)get_time_ms() > deadline) return 0;
        sleep_ms(1);
    }
    return 1;
}

static int wait_thread_gone(ActorBase* a) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (!(atomic_load(&a->dead) & AETHER_ACTOR_THREAD_GONE)) {
        if ((long long)get_time_ms() > deadline) return 0;
        sleep_ms(1);
    }
    return 1;
}

// Waits until *count reaches `want`, at most 10 s; returns the count.
static int wait_count(atomic_int* count, int want) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (atomic_load(count) < want && (long long)get_time_ms() <= deadline) sleep_ms(1);
    return atomic_load(count);
}

void test_scheduler_actor_thread_parks_when_idle(void) {
    scheduler_init(1);
    scheduler_set_actor_park_ms(60000);
    scheduler_start();
    CounterActor* a = (CounterActor*)scheduler_spawn_actor(-1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    atomic_store(&a->count, 0);
    a->auto_process = 1;
    ASSERT_EQ(0, pthread_create(&a->thread, NULL, aether_actor_thread, a));

    // Idle from the start: it spins a while, then sleeps; a message wakes it.
    for (int round = 1; round <= 3; round++) {
        ASSERT_TRUE(wait_thread_parked((ActorBase*)a));
        Message msg = message_create_simple(1, 0, round);
        scheduler_send_remote((ActorBase*)a, msg, -1);
        ASSERT_EQ(round, wait_count(&a->count, round));
        ASSERT_EQ(round, atomic_load(&a->last_value));
    }

    // Asleep again; the release wakes it, it leaves its loop and ends the
    // actor.
    ASSERT_TRUE(wait_thread_parked((ActorBase*)a));
    pthread_t t = a->thread;
    scheduler_release_actor((ActorBase*)a);
    ASSERT_TRUE(wait_thread_gone((ActorBase*)a));
    pthread_join(t, NULL);
    ASSERT_EQ(0, wait_no_pending());

    scheduler_set_actor_park_ms(0);   // back to the default
    scheduler_shutdown();
    scheduler_cleanup();
}

// #3102: a receive timeout armed on an actor that receives nothing fires, on
// its core and on its own thread. Only a message stepped an actor before, so
// on the scheduler threads the timeout never fired.
// The step tests the deadline as a generated one does, and counts a firing:
// the interval stays, the firing disarms the countdown, and only a message
// would arm it again (#3162), so each fires once.
static void timeout_step(void* self) {
    CounterActor* a = (CounterActor*)self;
    if (a->timeout_ns > 0 && a->last_activity_ns > 0 &&
        atomic_load_explicit(&a->mailbox.count, memory_order_relaxed) == 0 &&
        aether_now_ns() - a->last_activity_ns >= a->timeout_ns) {
        a->last_activity_ns = 0;
        atomic_fetch_add(&a->count, 1);
        return;
    }
    Message msg;
    if (mailbox_receive(&a->mailbox, &msg)) atomic_store(&a->last_value, msg.payload_int);
    atomic_store_explicit(&a->active, (a->mailbox.count > 0), memory_order_relaxed);
}

void test_scheduler_fires_receive_timeouts(void) {
    scheduler_init(2);
    scheduler_start();
    CounterActor* on_core = (CounterActor*)scheduler_spawn_actor(-1, timeout_step, sizeof(CounterActor));
    CounterActor* on_thread = (CounterActor*)scheduler_spawn_actor(-1, timeout_step, sizeof(CounterActor));
    ASSERT_NOT_NULL(on_core);
    ASSERT_NOT_NULL(on_thread);
    atomic_store(&on_core->count, 0);
    atomic_store(&on_thread->count, 0);
    on_thread->auto_process = 1;
    on_thread->timeout_ns = 20 * 1000000ULL;
    on_thread->last_activity_ns = aether_now_ns();
    ASSERT_EQ(0, pthread_create(&on_thread->thread, NULL, aether_actor_thread, on_thread));
    on_core->timeout_ns = 20 * 1000000ULL;
    on_core->last_activity_ns = aether_now_ns();
    // Armed by hand, outside a step: its core is told, as the generated
    // spawn tells it (#3540). An actor thread reads its own countdown.
    scheduler_timeout_armed((ActorBase*)on_core);

    int core_fired = wait_count(&on_core->count, 1);
    int thread_fired = wait_count(&on_thread->count, 1);

    pthread_t t = on_thread->thread;
    scheduler_release_actor((ActorBase*)on_thread);
    int gone = wait_thread_gone((ActorBase*)on_thread);
    pthread_join(t, NULL);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_EQ(1, core_fired);
    ASSERT_EQ(1, thread_fired);
    ASSERT_TRUE(gone);
}

// #3106: scheduler threads are counted and placed by the CPUs the process
// may run on, not by the machine's. The process is limited to its highest
// allowed CPU: thread i went to CPU i, from 0, outside that limit, and the
// count was the machine's. Linux checks each thread's binding, Windows its
// ideal processor (a hint, #3087); both check the count.
#if defined(__linux__) || defined(_WIN32)
void test_scheduler_places_threads_among_process_cpus(void) {
#if defined(__linux__)
    cpu_set_t saved;
    CPU_ZERO(&saved);
    ASSERT_EQ(0, sched_getaffinity(0, sizeof(saved), &saved));
    int only = -1;
    for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &saved)) only = c;
    ASSERT_TRUE(only >= 0);
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(only, &one);
    ASSERT_EQ(0, sched_setaffinity(0, sizeof(one), &one));
#else
    DWORD_PTR saved = 0, system_mask = 0;
    ASSERT_TRUE(GetProcessAffinityMask(GetCurrentProcess(), &saved, &system_mask) != 0);
    int only = -1;
    for (int c = 0; c < (int)(sizeof(DWORD_PTR) * 8); c++)
        if (saved & ((DWORD_PTR)1 << c)) only = c;
    ASSERT_TRUE(only >= 0);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)1 << only) != 0);
#endif

    int recommended = cpu_recommend_cores();

    // Two actors, one per core.
    scheduler_init(2);
    CounterActor* a = (CounterActor*)scheduler_spawn_actor(0, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    CounterActor* b = (CounterActor*)scheduler_spawn_actor(1, (void (*)(void*))counter_step,
                                                           sizeof(CounterActor));
    scheduler_start();
    int placed[2] = {-1, -1};
    int bound[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        placed[i] = scheduler_core_cpu(i);
#if defined(__linux__)
        cpu_set_t got;
        CPU_ZERO(&got);
        if (pthread_getaffinity_np(schedulers[i].thread, sizeof(got), &got) == 0)
            bound[i] = (CPU_COUNT(&got) == 1 && CPU_ISSET(only, &got)) ? only : -2;
#else
        PROCESSOR_NUMBER pn;
        if (GetThreadIdealProcessorEx((HANDLE)schedulers[i].thread, &pn))
            bound[i] = (int)pn.Number;
#endif
    }
    (void)a;
    (void)b;
    scheduler_shutdown();
    scheduler_cleanup();

#if defined(__linux__)
    sched_setaffinity(0, sizeof(saved), &saved);
#else
    SetProcessAffinityMask(GetCurrentProcess(), saved);
#endif
    ASSERT_EQ(1, recommended);
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(only, placed[i]);
        ASSERT_EQ(only, bound[i]);
    }
}
#endif

// #3087: a batch of sends wakes the cores it lands on. A loop of sends in
// main() goes through the batch path (scheduler_send_batch_*), which put each
// core's share on that core's queue and told no core: a parked core slept on
// the batch until its timed wait ran out, up to 32 ms. Here the cores' timed
// wait is a minute, so a wake that never came is a stall of a minute, which
// the waits below give up on after 10 s: a bound no slow machine comes near,
// where one of milliseconds could not tell a missed wake from a busy CI
// runner. Each round waits until both cores are parked, then sends one
// message to each of four actors, two on each core.
static int wait_core_parked(int core) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (!atomic_load(&schedulers[core].parked)) {
        if ((long long)get_time_ms() > deadline) return 0;
        sleep_ms(1);
    }
    return 1;
}

void test_scheduler_batch_wakes_parked_cores(void) {
    scheduler_init(2);
    CounterActor* actors[4];
    for (int i = 0; i < 4; i++) {
        actors[i] = (CounterActor*)scheduler_spawn_actor(i % 2, (void (*)(void*))counter_step,
                                                         sizeof(CounterActor));
        ASSERT_NOT_NULL(actors[i]);
        atomic_store(&actors[i]->count, 0);
    }
    scheduler_set_core_park_ms(60000);
    scheduler_start();

    // Checked after the teardown, so a failure leaves no core parked for a
    // minute under the tests that follow.
    int parked = 1, rounds_done = 0, unstepped = -1;
    for (int round = 1; round <= 3; round++) {
        parked = wait_core_parked(0) && wait_core_parked(1);
        if (!parked) break;
        scheduler_send_batch_start();
        for (int i = 0; i < 4; i++) {
            scheduler_send_batch_add((ActorBase*)actors[i], message_create_simple(1, 0, round));
        }
        scheduler_send_batch_flush();
        for (int i = 0; i < 4 && unstepped < 0; i++) {
            if (wait_count(&actors[i]->count, round) != round) unstepped = i;
        }
        if (unstepped >= 0) break;
        rounds_done = round;
    }

    scheduler_set_core_park_ms(0);   // back to the default
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_TRUE(parked);
    ASSERT_EQ(-1, unstepped);   // no actor waited on a core left asleep
    ASSERT_EQ(3, rounds_done);
}

// #3611: a core with messages of its own still to deliver does not park. A
// step on core 1 sends OVERFLOW_BURST messages to an actor on core 0 while
// core 0 is held in another step: QUEUE_SIZE of them fill core 0's queue from
// core 1, and the rest wait in core 1's overflow buffer, which only core 1's
// thread flushes. Core 1 then has nothing to step and idles past its spin.
// It parked so, and the messages waited out the park: up to 32 ms each time,
// and here, with the cores' park a minute as in #3087's test, a minute. Once
// core 0 is let go, every message must arrive within 10 s.
#define OVERFLOW_BURST (QUEUE_SIZE * 3)

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int in_step;
    atomic_int release;
} HoldActor;

static void hold_step(HoldActor* h) {
    Message msg;
    if (mailbox_receive(&h->mailbox, &msg)) {
        atomic_store(&h->in_step, 1);
        for (int w = 0; w < 10000 && !atomic_load(&h->release); w++) sleep_ms(1);
    }
    atomic_store_explicit(&h->active, h->mailbox.count > 0, memory_order_relaxed);
}

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* to;
    atomic_int sent;
} SpillActor;

static void spill_step(SpillActor* b) {
    Message msg;
    if (mailbox_receive(&b->mailbox, &msg)) {
        int me = atomic_load_explicit(&b->assigned_core, memory_order_relaxed);
        for (int i = 1; i <= OVERFLOW_BURST; i++)
            scheduler_send_remote(b->to, message_create_simple(1, 0, i), me);
        atomic_store(&b->sent, 1);
    }
    atomic_store_explicit(&b->active, b->mailbox.count > 0, memory_order_relaxed);
}

static int wait_flag(atomic_int* flag) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (!atomic_load(flag)) {
        if ((long long)get_time_ms() > deadline) return 0;
        sleep_ms(1);
    }
    return 1;
}

void test_scheduler_core_with_overflow_does_not_park(void) {
    scheduler_init(2);
    HoldActor* hold = (HoldActor*)scheduler_spawn_actor(0, (void (*)(void*))hold_step,
                                                        sizeof(HoldActor));
    CounterActor* sink = (CounterActor*)scheduler_spawn_actor(0, (void (*)(void*))counter_step,
                                                              sizeof(CounterActor));
    SpillActor* burst = (SpillActor*)scheduler_spawn_actor(1, (void (*)(void*))spill_step,
                                                           sizeof(SpillActor));
    CounterActor* waker = (CounterActor*)scheduler_spawn_actor(1, (void (*)(void*))counter_step,
                                                               sizeof(CounterActor));
    ASSERT_NOT_NULL(hold);
    ASSERT_NOT_NULL(sink);
    ASSERT_NOT_NULL(burst);
    ASSERT_NOT_NULL(waker);
    atomic_store(&hold->in_step, 0);
    atomic_store(&hold->release, 0);
    atomic_store(&sink->count, 0);
    atomic_store(&waker->count, 0);
    burst->to = (ActorBase*)sink;
    atomic_store(&burst->sent, 0);
    scheduler_set_core_park_ms(60000);
    scheduler_start();

    scheduler_send_remote((ActorBase*)hold, message_create_simple(1, 0, 0), -1);
    int held = wait_flag(&hold->in_step);
    scheduler_send_remote((ActorBase*)burst, message_create_simple(1, 0, 0), -1);
    int sent = held && wait_flag(&burst->sent);
    // Core 1 idles past its 10,000 spinning turns, where it parked: its
    // count of idle turns stops there, or goes on past it.
    long long deadline = (long long)get_time_ms() + 10000;
    while (sent && !atomic_load(&schedulers[1].parked) &&
           atomic_load(&schedulers[1].idle_cycles) < 30000 &&
           (long long)get_time_ms() <= deadline)
        sleep_ms(1);
    atomic_store(&hold->release, 1);
    int got = sent ? wait_count(&sink->count, OVERFLOW_BURST) : -1;

    // Checked after the teardown, so a failure leaves no core parked for a
    // minute under the tests that follow: a message for core 1 wakes it
    // (sched_wake), and the teardown does not wait out its park.
    scheduler_set_core_park_ms(0);
    scheduler_send_remote((ActorBase*)waker, message_create_simple(1, 0, 0), -1);
    wait_count(&waker->count, 1);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_TRUE(held);
    ASSERT_TRUE(sent);
    ASSERT_EQ(OVERFLOW_BURST, got);
}

// #2598: an actor thread's inbox takes any number of senders at once and
// keeps each one's order. The actor's first step holds it at a gate while
// three threads send the first half of their 400 messages: the inbox's ring
// fills and the rest waits in its list. The gate opens and the threads send
// their second halves while the actor works through that backlog, so the
// ring has room again while older messages still wait in the list: each new
// one must go behind them. Then a core-mate's step sends 400 from the core
// thread, and the actor 400 to itself. All 2,002 must come out, each sender's
// in the order it sent them. The single-producer queue this replaced lost
// messages here: past its 63 slots, past the 32 its thread moved into the
// mailbox, and whenever two senders wrote the same slot.
#define ORDER_SENDERS 5    // three threads, the core-mate, the actor itself
#define ORDER_EACH 400

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int count;
    atomic_int out_of_order;
    atomic_int next[ORDER_SENDERS];   // each sender's next sequence number
} OrderedActor;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* target;
    atomic_int done;
} BurstActor;

static atomic_int g_order_gate;
static atomic_int g_order_halves;   // sender threads past their first half

static void ordered_step(void* self) {
    OrderedActor* a = (OrderedActor*)self;
    Message msg;
    if (!mailbox_receive(&a->mailbox, &msg)) return;
    if (msg.type == 1) {
        atomic_fetch_add(&a->count, 1);   // counted first: the test waits on it
        while (!atomic_load(&g_order_gate)) sleep_ms(1);
        return;
    } else if (msg.type == 2) {
        int from = msg.sender_id;
        if (from < 0 || from >= ORDER_SENDERS ||
            msg.payload_int != atomic_load(&a->next[from])) {
            atomic_fetch_add(&a->out_of_order, 1);
        } else {
            atomic_fetch_add(&a->next[from], 1);
        }
    } else if (msg.type == 3) {
        // From its own thread, to itself.
        for (int i = 0; i < ORDER_EACH; i++) {
            scheduler_send_local((ActorBase*)a, message_create_simple(2, 4, i));
        }
    }
    atomic_fetch_add(&a->count, 1);
}

static void burst_step(void* self) {
    BurstActor* b = (BurstActor*)self;
    Message msg;
    if (mailbox_receive(&b->mailbox, &msg)) {
        // On the core thread, to an actor of the same core.
        for (int i = 0; i < ORDER_EACH; i++) {
            scheduler_send_local(b->target, message_create_simple(2, 3, i));
        }
        atomic_store(&b->done, 1);
    }
    atomic_store_explicit(&b->active, (b->mailbox.count > 0), memory_order_relaxed);
}

typedef struct {
    ActorBase* target;
    int sender;
} OrderSender;

static void* order_sender_thread(void* arg) {
    OrderSender* s = (OrderSender*)arg;
    for (int i = 0; i < ORDER_EACH; i++) {
        if (i == ORDER_EACH / 2) {
            atomic_fetch_add(&g_order_halves, 1);
            while (!atomic_load(&g_order_gate)) sleep_ms(1);
        }
        scheduler_send_remote(s->target, message_create_simple(2, s->sender, i), -1);
    }
    return NULL;
}

void test_scheduler_actor_thread_inbox_keeps_order(void) {
    scheduler_init(1);
    scheduler_start();
    BurstActor* burst = (BurstActor*)scheduler_spawn_actor(-1, burst_step, sizeof(BurstActor));
    ASSERT_NOT_NULL(burst);
    atomic_init(&burst->done, 0);
    OrderedActor* a = (OrderedActor*)scheduler_spawn_actor(-1, ordered_step, sizeof(OrderedActor));
    ASSERT_NOT_NULL(a);
    atomic_init(&a->count, 0);
    atomic_init(&a->out_of_order, 0);
    for (int i = 0; i < ORDER_SENDERS; i++) atomic_init(&a->next[i], 0);
    burst->target = (ActorBase*)a;
    atomic_store(&g_order_gate, 0);
    a->auto_process = 1;
    ASSERT_EQ(0, pthread_create(&a->thread, NULL, aether_actor_thread, a));

    scheduler_send_remote((ActorBase*)a, message_create_simple(1, 0, 0), -1);
    ASSERT_EQ(1, wait_count(&a->count, 1));   // its thread waits at the gate

    atomic_store(&g_order_halves, 0);
    pthread_t senders[3];
    OrderSender args[3];
    for (int i = 0; i < 3; i++) {
        args[i].target = (ActorBase*)a;
        args[i].sender = i;
        ASSERT_EQ(0, pthread_create(&senders[i], NULL, order_sender_thread, &args[i]));
    }
    ASSERT_EQ(3, wait_count(&g_order_halves, 3));
    atomic_store(&g_order_gate, 1);
    for (int i = 0; i < 3; i++) pthread_join(senders[i], NULL);
    scheduler_send_remote((ActorBase*)burst, message_create_simple(1, 0, 0), -1);
    ASSERT_EQ(1, wait_count(&burst->done, 1));
    scheduler_send_remote((ActorBase*)a, message_create_simple(3, 0, 0), -1);

    int total = 1 + 4 * ORDER_EACH + 1 + ORDER_EACH;
    ASSERT_EQ(total, wait_count(&a->count, total));
    ASSERT_EQ(0, atomic_load(&a->out_of_order));
    for (int i = 0; i < ORDER_SENDERS; i++) ASSERT_EQ(ORDER_EACH, atomic_load(&a->next[i]));
    scheduler_wait();   // returns: every message counted as sent was processed

    pthread_t t = a->thread;
    scheduler_release_actor((ActorBase*)a);
    ASSERT_TRUE(wait_thread_gone((ActorBase*)a));
    pthread_join(t, NULL);
    scheduler_shutdown();
    scheduler_cleanup();
}

// Make the `len` bytes at `page` (page aligned) read-only, or writable again.
static int protect_pages(void* page, size_t len, int read_only) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(page, len, read_only ? PAGE_READONLY : PAGE_READWRITE, &old) ? 0 : -1;
#else
    return mprotect(page, len, read_only ? PROT_READ : (PROT_READ | PROT_WRITE));
#endif
}

static size_t system_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

// #3521: scheduler_stop woke the cores by storing each foreign queue's tail
// back into itself, for an MWAIT on that word that no core makes, and it did
// so without foreign_lock: a send from a thread the scheduler does not own,
// landing between the load and the store, had the tail moved back under it.
// stop no longer writes that queue at all. The page holding each core's
// foreign queue tail is read-only across the call, so a write there faults.
// With two cores and nothing sent, nothing else on the page is written: it
// holds only queues of senders that do not exist and the empty foreign queue.
void test_scheduler_stop_leaves_foreign_queue_alone(void) {
    scheduler_init(2);
    scheduler_start();

    size_t page = system_page_size();
    void* pages[2];
    int guarded = 1;
    for (int i = 0; i < 2; i++) {
        uintptr_t tail = (uintptr_t)&schedulers[i].from_queues[MAX_CORES].tail;
        uintptr_t start = tail & ~(uintptr_t)(page - 1);
        pages[i] = (void*)start;
        // A page large enough to reach a field stop does write (`running`,
        // past the queues) cannot be guarded this way.
        if (start < (uintptr_t)&schedulers[i].from_queues[2] ||
            start + page > (uintptr_t)&schedulers[i].foreign_lock) guarded = 0;
    }
    const char* why = "page of this size reaches past the queues";
    if (guarded) {
        // OpenBSD refuses mprotect on a program's .data and .bss (EPERM),
        // where `schedulers` lives: there the call runs unguarded.
        int done = 0;
        while (done < 2 && protect_pages(pages[done], page, 1) == 0) done++;
        if (done < 2) {
            while (done > 0) ASSERT_EQ(0, protect_pages(pages[--done], page, 0));
            guarded = 0;
            why = "the system refuses mprotect on this page";
        }
    }
    scheduler_stop();
    if (guarded) {
        for (int i = 0; i < 2; i++) ASSERT_EQ(0, protect_pages(pages[i], page, 0));
    } else {
        printf("    (%zu-byte page not guarded: %s)\n", page, why);
    }

    scheduler_shutdown();
    scheduler_cleanup();
}

// A scheduler_send_local from a thread that is not the actor's core thread
// (the main thread of a C host) wrote straight into the actor's mailbox while
// the core thread delivered into it too: two producers on a single producer
// ring, so messages were lost or delivered twice. The target counts each
// sender's sequence; the main thread and an actor of the target's core send
// at once.
#define LS_EACH 100000

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int got;
    atomic_int out_of_order;
    int next[2];
} LsTarget;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* target;
    atomic_int done;
} LsSender;

static void ls_target_step(void* self) {
    LsTarget* t = (LsTarget*)self;
    Message msg;
    if (!mailbox_receive(&t->mailbox, &msg)) return;
    int from = msg.sender_id;
    if (from < 0 || from > 1 || msg.payload_int != t->next[from]) {
        atomic_fetch_add(&t->out_of_order, 1);
        if (from >= 0 && from <= 1) t->next[from] = (int)msg.payload_int + 1;
    } else {
        t->next[from]++;
    }
    atomic_fetch_add(&t->got, 1);
}

static void ls_sender_step(void* self) {
    LsSender* s = (LsSender*)self;
    Message msg;
    if (!mailbox_receive(&s->mailbox, &msg)) return;
    // On the target's core thread: the same-core path.
    for (int i = 0; i < LS_EACH; i++)
        scheduler_send_local(s->target, message_create_simple(2, 1, i));
    atomic_store(&s->done, 1);
}

void test_scheduler_send_local_from_foreign_thread(void) {
    scheduler_init(1);
    scheduler_start();
    LsTarget* t = (LsTarget*)scheduler_spawn_actor(0, ls_target_step, sizeof(LsTarget));
    LsSender* s = (LsSender*)scheduler_spawn_actor(0, ls_sender_step, sizeof(LsSender));
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(s);
    atomic_init(&t->got, 0);
    atomic_init(&t->out_of_order, 0);
    t->next[0] = t->next[1] = 0;
    s->target = (ActorBase*)t;
    atomic_init(&s->done, 0);

    scheduler_send_remote((ActorBase*)s, message_create_simple(1, 0, 0), -1);
    for (int i = 0; i < LS_EACH; i++)
        scheduler_send_local((ActorBase*)t, message_create_simple(2, 0, i));
    ASSERT_EQ(1, wait_count(&s->done, 1));
    int got = wait_count(&t->got, 2 * LS_EACH);
    scheduler_wait();
    int late = atomic_load(&t->got) - 2 * LS_EACH;   // delivered twice
    int bad = atomic_load(&t->out_of_order);
    int next0 = t->next[0], next1 = t->next[1];
    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_EQ(2 * LS_EACH, got);
    ASSERT_EQ(0, late);
    ASSERT_EQ(0, bad);
    ASSERT_EQ(LS_EACH, next0);
    ASSERT_EQ(LS_EACH, next1);
}

// ---------------------------------------------------------------------------
// The ready list and the timer heap (#3540): a core looks at the actors that
// have work, not at every actor it has. Each test sets the cores' park to a
// minute, so an actor the core is never brought back to shows as a stall of
// the wait (10 s) rather than as a park's delay.
// ---------------------------------------------------------------------------

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int handled;
    atomic_int empty_steps;   // stepped with nothing to take
    atomic_int out_of_order;
    int chain;                // a Next(n) below it sends Next(n + 1) to itself
    int next[8];              // each sender's next sequence number
} RlActor;

enum { RL_NEXT = 1, RL_SEQ = 2 };

static void rl_step(void* self) {
    RlActor* a = (RlActor*)self;
    Message msg;
    if (!mailbox_receive(&a->mailbox, &msg)) {
        atomic_fetch_add(&a->empty_steps, 1);
        return;
    }
    if (msg.type == RL_NEXT) {
        // From its own step: never stepped inline, so the step leaves the
        // message in the mailbox, and the core's ready list brings it back.
        if (msg.payload_int + 1 < a->chain)
            scheduler_send_local((ActorBase*)a, message_create_simple(RL_NEXT, 0, msg.payload_int + 1));
    } else if (msg.type == RL_SEQ) {
        int from = msg.sender_id;
        if (from < 0 || from >= 8 || msg.payload_int != a->next[from]) {
            atomic_fetch_add(&a->out_of_order, 1);
            if (from >= 0 && from < 8) a->next[from] = (int)msg.payload_int + 1;
        } else {
            a->next[from]++;
        }
    }
    atomic_fetch_add(&a->handled, 1);
}

static RlActor* rl_spawn(int core) {
    RlActor* a = (RlActor*)scheduler_spawn_actor(core, rl_step, sizeof(RlActor));
    if (!a) return NULL;
    atomic_init(&a->handled, 0);
    atomic_init(&a->empty_steps, 0);
    atomic_init(&a->out_of_order, 0);
    a->chain = 0;
    for (int i = 0; i < 8; i++) a->next[i] = 0;
    return a;
}

// An idle actor that receives after the core has been parked a long while,
// among many idle actors, is stepped through all of its work: a chain of
// 1,000 messages it sends itself, 64 steps at most per look, so the core
// comes back to it through its ready list some 16 times.
void test_ready_list_steps_actor_after_long_idle(void) {
    scheduler_init(1);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    for (int i = 0; i < 10000; i++) ASSERT_NOT_NULL(rl_spawn(0));
    RlActor* x = rl_spawn(0);
    ASSERT_NOT_NULL(x);
    x->chain = 1000;
    int got[2];
    for (int round = 0; round < 2; round++) {
        sleep_ms(200);   // the core parks
        scheduler_send_remote((ActorBase*)x, message_create_simple(RL_NEXT, 0, 0), -1);
        got[round] = wait_count(&x->handled, 1000 * (round + 1));
    }
    int empty = atomic_load(&x->empty_steps);
    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_EQ(1000, got[0]);
    ASSERT_EQ(2000, got[1]);
    ASSERT_EQ(0, empty);
}

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int fired;
    atomic_llong fired_at_ns;
} RlTimed;

static void rl_timed_step(void* self) {
    RlTimed* a = (RlTimed*)self;
    if (a->timeout_ns > 0 && a->last_activity_ns > 0 &&
        atomic_load_explicit(&a->mailbox.count, memory_order_relaxed) == 0 &&
        aether_now_ns() - a->last_activity_ns >= a->timeout_ns) {
        a->last_activity_ns = 0;   // fired: only a message arms it again
        atomic_store(&a->fired_at_ns, (long long)aether_now_ns());
        atomic_fetch_add(&a->fired, 1);
        return;
    }
    Message msg;
    (void)mailbox_receive(&a->mailbox, &msg);
}

// A receive timeout fires once its deadline passes, with 20,000 idle actors
// on its core and the core parked for up to a minute: the park sleeps no
// longer than the earliest countdown in the core's timer heap. And a
// released actor whose countdown is still in the heap is reclaimed: its core
// takes it out of the heap when the reclaim asks it to.
void test_ready_list_timeout_fires_on_time_among_idle_actors(void) {
    scheduler_init(1);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    for (int i = 0; i < 20000; i++) ASSERT_NOT_NULL(rl_spawn(0));
    RlTimed* t = (RlTimed*)scheduler_spawn_actor(0, rl_timed_step, sizeof(RlTimed));
    RlTimed* late = (RlTimed*)scheduler_spawn_actor(0, rl_timed_step, sizeof(RlTimed));
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(late);
    atomic_init(&t->fired, 0);
    atomic_init(&late->fired, 0);
    sleep_ms(100);   // the core parks
    uint64_t armed = aether_now_ns();
    t->timeout_ns = 50 * 1000000ULL;
    t->last_activity_ns = armed;
    scheduler_timeout_armed((ActorBase*)t);
    late->timeout_ns = 60000 * 1000000ULL;   // a minute: still in the heap below
    late->last_activity_ns = armed;
    scheduler_timeout_armed((ActorBase*)late);
    int fired = wait_count(&t->fired, 1);
    long long after_ms = (atomic_load(&t->fired_at_ns) - (long long)armed) / 1000000LL;

    sleep_ms(20);
    int in_heap = atomic_load(&late->timer_slot) != 0;
    scheduler_release_actor((ActorBase*)late);
    int left = scheduler_reclaim_released();

    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_EQ(1, fired);
    ASSERT_TRUE(after_ms >= 50);
    // On time: a Windows timed wait ends on its 15.6 ms tick, and a loaded
    // runner is slow; a park that missed the deadline sleeps a minute.
    ASSERT_TRUE(after_ms < 2000);
    ASSERT_TRUE(in_heap);
    ASSERT_EQ(0, left);
}

typedef struct {
    ActorBase* target;
    int sender;
    int each;
} RlSenderArgs;

static void* rl_sender_thread(void* arg) {
    RlSenderArgs* s = (RlSenderArgs*)arg;
    for (int i = 0; i < s->each; i++) {
        scheduler_send_remote(s->target, message_create_simple(RL_SEQ, s->sender, i), -1);
        if ((i & 255) == 0) sleep_ms(1);   // the target's mailbox runs empty between bursts
    }
    return NULL;
}

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* target;
    int sender;
    int each;
} RlForwarder;

// On a core thread: to another core's target, which asks it to move here.
static void rl_forwarder_step(void* self) {
    RlForwarder* f = (RlForwarder*)self;
    Message msg;
    if (!mailbox_receive(&f->mailbox, &msg)) return;
    int base = (int)msg.payload_int;
    for (int i = 0; i < f->each; i++)
        scheduler_send_remote(f->target, message_create_simple(RL_SEQ, f->sender, base + i),
                              aether_core_id_get());
}

// A mailbox that runs empty and fills again under senders on other cores and
// on threads no core owns, while the target is asked to move from core to
// core (each sender's core asks for it, #3090), loses no wakeup: every
// message is handled, once, in its sender's order, though a core parks for a
// minute when it is not woken.
void test_ready_list_no_lost_wakeup_under_concurrent_senders(void) {
    scheduler_init(4);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    RlActor* t = rl_spawn(0);
    ASSERT_NOT_NULL(t);
    RlForwarder* fw[3];
    for (int k = 0; k < 3; k++) {
        fw[k] = (RlForwarder*)scheduler_spawn_actor(k + 1, rl_forwarder_step, sizeof(RlForwarder));
        ASSERT_NOT_NULL(fw[k]);
        fw[k]->target = (ActorBase*)t;
        fw[k]->sender = k;
        fw[k]->each = 50;
    }
    const int rounds = 100, each = 2000;
    pthread_t th[2];
    RlSenderArgs args[2];
    for (int k = 0; k < 2; k++) {
        args[k].target = (ActorBase*)t;
        args[k].sender = 3 + k;
        args[k].each = each;
        ASSERT_EQ(0, pthread_create(&th[k], NULL, rl_sender_thread, &args[k]));
    }
    int expected = 2 * each;
    for (int r = 0; r < rounds; r++) {
        for (int k = 0; k < 3; k++)
            scheduler_send_remote((ActorBase*)fw[k], message_create_simple(1, 0, r * 50), -1);
        expected += 3 * 50;
        if (r % 10 == 0) sleep_ms(5);   // the cores park
    }
    for (int k = 0; k < 2; k++) pthread_join(th[k], NULL);
    int got = wait_count(&t->handled, expected);
    scheduler_wait();
    int total = atomic_load(&t->handled);
    int bad = atomic_load(&t->out_of_order);
    int empty = atomic_load(&t->empty_steps);
    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_EQ(expected, got);
    ASSERT_EQ(expected, total);   // none twice
    ASSERT_EQ(0, bad);
    ASSERT_EQ(0, empty);
}

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    ActorBase* targets[16];
    int ntargets;
} RlFanout;

// On core 1: one message to each target, all of core 0. Each target is
// asked to move here (#3090), and its core moves it before it runs, with
// the message in its mailbox.
static void rl_fanout_step(void* self) {
    RlFanout* f = (RlFanout*)self;
    Message msg;
    if (!mailbox_receive(&f->mailbox, &msg)) return;
    for (int i = 0; i < f->ntargets; i++)
        scheduler_send_remote(f->targets[i], message_create_simple(RL_SEQ, 0, msg.payload_int),
                              aether_core_id_get());
}

// An actor moved to another core with a message in its mailbox, and nothing
// sent to it after, is stepped there: the move brings the new core to it
// through its ready list. Nothing else would: no message comes through the
// new core's queues for it, and the old core no longer looks at it.
void test_ready_list_moved_actor_stepped_on_new_core(void) {
    scheduler_init(2);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    RlFanout* f = (RlFanout*)scheduler_spawn_actor(1, rl_fanout_step, sizeof(RlFanout));
    ASSERT_NOT_NULL(f);
    RlActor* t[16];
    f->ntargets = 16;
    for (int i = 0; i < 16; i++) {
        t[i] = rl_spawn(0);
        ASSERT_NOT_NULL(t[i]);
        f->targets[i] = (ActorBase*)t[i];
    }
    sleep_ms(100);   // both cores park
    scheduler_send_remote((ActorBase*)f, message_create_simple(1, 0, 0), -1);
    int handled = 0, moved = 0;
    for (int i = 0; i < 16; i++) {
        handled += wait_count(&t[i]->handled, 1);
        moved += atomic_load(&t[i]->assigned_core) == 1;
    }
    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_EQ(16, handled);
    ASSERT_TRUE(moved > 0);   // the path under test was taken
}

static atomic_int g_rl_hammer_stop;

static void* rl_hammer_thread(void* arg) {
    ActorBase* a = (ActorBase*)arg;
    while (!atomic_load(&g_rl_hammer_stop)) scheduler_actor_revived(a);
    return NULL;
}

// A push for an actor already in a ready list adds no entry: a thousand
// pushes before the core runs leave one, and pushes from two threads the
// whole time the actor takes 20,000 messages step none of them twice and
// step it with an empty mailbox never.
void test_ready_list_duplicates_do_not_double_step(void) {
    scheduler_init(1);
    scheduler_set_core_park_ms(60000);
    RlActor* a = rl_spawn(0);
    ASSERT_NOT_NULL(a);
    for (int i = 0; i < 1000; i++) scheduler_actor_revived((ActorBase*)a);
    int entries_before = atomic_load(&schedulers[0].ready_in.count);
    int queued_before = atomic_load(&a->ready_queued);
    scheduler_start();

    atomic_store(&g_rl_hammer_stop, 0);
    pthread_t th[2];
    for (int k = 0; k < 2; k++) ASSERT_EQ(0, pthread_create(&th[k], NULL, rl_hammer_thread, a));
    const int count = 20000;
    for (int i = 0; i < count; i++) {
        scheduler_send_remote((ActorBase*)a, message_create_simple(RL_SEQ, 0, i), -1);
        if ((i & 1023) == 0) sleep_ms(1);
    }
    int got = wait_count(&a->handled, count);
    atomic_store(&g_rl_hammer_stop, 1);
    for (int k = 0; k < 2; k++) pthread_join(th[k], NULL);
    scheduler_wait();
    sleep_ms(50);   // the core takes the last push
    int total = atomic_load(&a->handled);
    int bad = atomic_load(&a->out_of_order);
    int empty = atomic_load(&a->empty_steps);
    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    // Joined: the core's lists are readable here.
    int left_local = schedulers[0].ready_count;
    int left_in = atomic_load(&schedulers[0].ready_in.count);
    int queued_after = atomic_load(&a->ready_queued);
    scheduler_cleanup();
    ASSERT_EQ(1, entries_before);
    ASSERT_EQ(1, queued_before);
    ASSERT_EQ(count, got);
    ASSERT_EQ(count, total);
    ASSERT_EQ(0, bad);
    ASSERT_EQ(0, empty);
    ASSERT_EQ(0, left_local);
    ASSERT_EQ(0, left_in);
    ASSERT_EQ(0, queued_after);
}

// #3541: a loop that steps one actor many times yields to the core's queues
// once a message for another actor comes in, now on a hint word that senders
// set rather than on every queue's head and tail. An actor ticks itself, a
// millisecond a tick; a Look from the main thread to a second actor of its
// core is handled within a few ticks, not after the loop's 64 (a message to
// the ticking actor itself would end the loop anyway: counted in flight, it
// sends the actor's next tick behind it, #3093).
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int ticks;
    atomic_int stop;
} TickerActor;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    TickerActor* ticker;
    atomic_int seen_at;   // the ticker's ticks when Look was handled, -1 before
} WatcherActor;

static void ticker_step(void* self) {
    TickerActor* t = (TickerActor*)self;
    Message msg;
    if (!mailbox_receive(&t->mailbox, &msg)) return;
    if (atomic_load(&t->stop)) return;
    atomic_fetch_add(&t->ticks, 1);
    sleep_ms(1);
    scheduler_send_local((ActorBase*)t, message_create_simple(1, 0, 0));
}

static void watcher_step(void* self) {
    WatcherActor* w = (WatcherActor*)self;
    Message msg;
    if (!mailbox_receive(&w->mailbox, &msg)) return;
    atomic_store(&w->seen_at, atomic_load(&w->ticker->ticks));
    atomic_store(&w->ticker->stop, 1);
}

void test_scheduler_stepping_loop_yields_to_queues(void) {
    scheduler_init(1);
    scheduler_start();
    TickerActor* t = (TickerActor*)scheduler_spawn_actor(0, ticker_step, sizeof(TickerActor));
    WatcherActor* w = (WatcherActor*)scheduler_spawn_actor(0, watcher_step, sizeof(WatcherActor));
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(w);
    atomic_init(&t->ticks, 0);
    atomic_init(&t->stop, 0);
    w->ticker = t;
    atomic_init(&w->seen_at, -1);
    scheduler_send_remote((ActorBase*)t, message_create_simple(1, 0, 0), -1);
    int ticked = wait_count(&t->ticks, 10);
    int at_send = atomic_load(&t->ticks);
    scheduler_send_remote((ActorBase*)w, message_create_simple(2, 0, 0), -1);
    long long deadline = (long long)get_time_ms() + 10000;
    while (atomic_load(&w->seen_at) < 0 && (long long)get_time_ms() <= deadline) sleep_ms(1);
    int seen_at = atomic_load(&w->seen_at);
    atomic_store(&t->stop, 1);
    scheduler_shutdown();
    scheduler_cleanup();
    ASSERT_TRUE(ticked >= 10);
    ASSERT_TRUE(seen_at >= at_send);
    // The tick under way when Look came, and slack for a slow timer: far
    // below the loop's 64.
    ASSERT_TRUE(seen_at - at_send <= 8);
}

// #3673: a caller's actor carried over scheduler restarts, with a thread no
// core owns sending to it all along. Between a restart's scheduler_init and
// the actor's registration, the sends go to the core its last lifecycle left
// in assigned_core, and that core delivers them; the registration then gave
// the actor the least loaded core, and both cores wrote its mailbox. Each
// message carries a sequence; every one must arrive once, in order.
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int got;
    atomic_int out_of_order;
    int next;
} CarriedActor;

static atomic_int g_carry_stop;
static atomic_int g_carry_sending;
static atomic_int g_carry_paused;
static atomic_int g_carry_sent;   // sends the scheduler took (a refused one is not)

static void carried_step(void* self) {
    CarriedActor* a = (CarriedActor*)self;
    Message msg;
    if (!mailbox_receive(&a->mailbox, &msg)) return;
    if (msg.payload_int != a->next) atomic_fetch_add(&a->out_of_order, 1);
    a->next = (int)msg.payload_int + 1;
    atomic_fetch_add(&a->got, 1);
}

static void* carry_sender(void* arg) {
    ActorBase* a = (ActorBase*)arg;
    int seq = 0;
    while (!atomic_load(&g_carry_stop)) {
        if (!atomic_load(&g_carry_sending)) {
            atomic_store(&g_carry_paused, 1);
            continue;
        }
        atomic_store(&g_carry_paused, 0);
        uint64_t refused = scheduler_stopped_sends();
        scheduler_send_remote(a, message_create_simple(1, 0, seq), -1);
        // A send refused while the scheduler was down is no message: the
        // sequence goes on from it.
        if (scheduler_stopped_sends() == refused) {
            seq++;
            atomic_fetch_add(&g_carry_sent, 1);
        }
    }
    return NULL;
}

static void carry_pause(void) {
    atomic_store(&g_carry_sending, 0);
    while (!atomic_load(&g_carry_paused)) {
    }
}

void test_scheduler_restart_hands_carried_actor_over(void) {
    CarriedActor* a = (CarriedActor*)calloc(1, sizeof(CarriedActor));
    ASSERT_NOT_NULL(a);
    mailbox_init(&a->mailbox);
    a->step = carried_step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    atomic_store(&g_carry_stop, 0);
    atomic_store(&g_carry_paused, 0);
    atomic_store(&g_carry_sent, 0);
    scheduler_init(2);
    scheduler_start();
    scheduler_register_actor((ActorBase*)a, -1);
    atomic_store(&g_carry_sending, 1);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, carry_sender, a));
    int lost = 0;
    for (int round = 0; round < 20 && !lost; round++) {
        sleep_ms(2);
        carry_pause();
        scheduler_shutdown();
        if (atomic_load(&a->got) != atomic_load(&g_carry_sent)) lost = 1;
        atomic_store(&g_carry_paused, 0);
        atomic_store(&g_carry_sending, 1);
        // Messages sent from here until the registration go to the core
        // the actor had.
        scheduler_init(2);
        scheduler_start();
        sleep_ms(1);
        scheduler_register_actor((ActorBase*)a, -1);
    }
    sleep_ms(2);
    carry_pause();
    scheduler_shutdown();
    int got = atomic_load(&a->got), sent = atomic_load(&g_carry_sent);
    int bad = atomic_load(&a->out_of_order);
    atomic_store(&g_carry_stop, 1);
    atomic_store(&g_carry_sending, 1);
    pthread_join(t, NULL);
    scheduler_cleanup();
    free(a);
    ASSERT_EQ(0, lost);
    ASSERT_EQ(sent, got);
    ASSERT_EQ(0, bad);
}

// An actor ended while scheduler_shutdown() is stopping the cores (the
// scheduler's state no longer "running", its threads not joined yet) has its
// death finalized by its core, once its step returns, not by the thread that
// ended it while the step still runs. A receive timeout's arm is still
// stepped then (the shutdown does not wait for countdowns, #3219): it holds
// at a gate while the main thread kills the actor.
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    atomic_int in_arm;
    atomic_int finalized_in_arm;   // the death was finalized while the arm ran
} GatedTimed;

static atomic_int g_arm_gate;

static void gated_timed_step(void* self) {
    GatedTimed* a = (GatedTimed*)self;
    if (a->timeout_ns > 0 && a->last_activity_ns > 0 &&
        atomic_load_explicit(&a->mailbox.count, memory_order_relaxed) == 0 &&
        aether_now_ns() - a->last_activity_ns >= a->timeout_ns) {
        a->last_activity_ns = 0;
        atomic_store(&a->in_arm, 1);
        for (int w = 0; w < 10000 && !atomic_load(&g_arm_gate); w++) sleep_ms(1);
        if (atomic_load_explicit(&a->exit_flags, memory_order_acquire) & AETHER_EXIT_FINALIZED)
            atomic_store(&a->finalized_in_arm, 1);
        atomic_store(&a->in_arm, 0);
        return;
    }
    Message msg;
    (void)mailbox_receive(&a->mailbox, &msg);
}

static void* shutdown_thread(void* arg) {
    (void)arg;
    scheduler_shutdown();
    return NULL;
}

void test_scheduler_end_during_shutdown_finalized_by_core(void) {
    scheduler_init(2);
    scheduler_start();
    GatedTimed* a = (GatedTimed*)scheduler_spawn_actor(1, gated_timed_step, sizeof(GatedTimed));
    CounterActor* probe = (CounterActor*)scheduler_spawn_actor(0, (void (*)(void*))counter_step,
                                                               sizeof(CounterActor));
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(probe);
    atomic_init(&a->in_arm, 0);
    atomic_init(&a->finalized_in_arm, 0);
    atomic_store(&g_arm_gate, 0);
    a->timeout_ns = 5 * 1000000ULL;
    a->last_activity_ns = aether_now_ns();
    scheduler_timeout_armed((ActorBase*)a);
    ASSERT_EQ(1, wait_count(&a->in_arm, 1));

    // The shutdown waits for no countdown: it stops the scheduler and then
    // joins core 1, which is in the arm.
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, shutdown_thread, NULL));
    // Stopping: a send from this thread is refused from then on.
    uint64_t refused = scheduler_stopped_sends();
    long long deadline = (long long)get_time_ms() + 10000;
    while (scheduler_stopped_sends() == refused && (long long)get_time_ms() <= deadline) {
        scheduler_send_remote((ActorBase*)probe, message_create_simple(1, 0, 0), -1);
        sleep_ms(1);
    }
    int stopping = scheduler_stopped_sends() != refused;

    scheduler_actor_end((ActorBase*)a, "killed", AETHER_ACTOR_KILLED);
    int finalized_at_end = (atomic_load(&a->exit_flags) & AETHER_EXIT_FINALIZED) != 0;
    atomic_store(&g_arm_gate, 1);
    pthread_join(t, NULL);
    int finalized_in_arm = atomic_load(&a->finalized_in_arm);
    // And it was finalized: by the core, on its way out of its loop.
    int finalized_after = (atomic_load(&a->exit_flags) & AETHER_EXIT_FINALIZED) != 0;
    scheduler_cleanup();

    ASSERT_TRUE(stopping);
    ASSERT_EQ(0, finalized_at_end);
    ASSERT_EQ(0, finalized_in_arm);
    ASSERT_EQ(1, finalized_after);
}

// A caller's actor (zeroed, its mailbox and step set, as the header asks).
static void* caller_actor(size_t size, void (*step)(void*)) {
    ActorBase* a = (ActorBase*)calloc(1, size);
    if (!a) return NULL;
    mailbox_init(&a->mailbox);
    a->step = step;
    atomic_init(&a->assigned_core, -1);
    atomic_init(&a->migrate_to, -1);
    return a;
}

static void arm_countdown(ActorBase* a, int ms) {
    a->timeout_ns = (uint64_t)ms * 1000000ULL;
    a->last_activity_ns = aether_now_ns();
    scheduler_timeout_armed(a);
}

static int wait_in_heap(ActorBase* a) {
    long long deadline = (long long)get_time_ms() + 10000;
    while (atomic_load(&a->timer_slot) == 0 && (long long)get_time_ms() <= deadline) sleep_ms(1);
    return atomic_load(&a->timer_slot) != 0;
}

// A caller's countdown is the scheduler's to fire once the actor is
// registered, whenever it was armed (#3540). The scan found such an actor in
// its table on its next pass; the ready list finds an actor only when told:
// one carried over a restart lost its countdown with the last lifecycle's
// heap, and one armed before its registration was told when it was on no
// core, and the push went nowhere.
void test_ready_list_registration_finds_armed_countdown(void) {
    RlTimed* carried = (RlTimed*)caller_actor(sizeof(RlTimed), rl_timed_step);
    RlTimed* early = (RlTimed*)caller_actor(sizeof(RlTimed), rl_timed_step);
    ASSERT_NOT_NULL(carried);
    ASSERT_NOT_NULL(early);
    atomic_init(&carried->fired, 0);
    atomic_init(&early->fired, 0);
    scheduler_init(2);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    scheduler_register_actor((ActorBase*)carried, 0);
    // Armed in this lifecycle, due in the next.
    arm_countdown((ActorBase*)carried, 300);
    int in_heap = wait_in_heap((ActorBase*)carried);
    scheduler_shutdown();
    int fired_before = atomic_load(&carried->fired);

    scheduler_init(2);
    scheduler_set_core_park_ms(60000);
    scheduler_start();
    arm_countdown((ActorBase*)early, 50);   // before its registration
    scheduler_register_actor((ActorBase*)carried, -1);
    scheduler_register_actor((ActorBase*)early, -1);
    int carried_fired = wait_count(&carried->fired, 1);
    int early_fired = wait_count(&early->fired, 1);
    scheduler_set_core_park_ms(0);
    scheduler_shutdown();
    scheduler_cleanup();
    free(carried);
    free(early);
    ASSERT_TRUE(in_heap);
    ASSERT_EQ(0, fired_before);
    ASSERT_EQ(1, carried_fired);
    ASSERT_EQ(1, early_fired);
}

void register_scheduler_tests(void) {
    register_test_with_category("Mailbox basic operations", test_mailbox_basic, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler bidirectional ping-pong", test_scheduler_bidirectional, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Mailbox overflow handling", test_mailbox_overflow, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler init/cleanup", test_scheduler_init_cleanup, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler spawn on demand", test_scheduler_spawn_on_demand, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler spawn placement", test_scheduler_spawn_placement, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler spawns 64-byte-aligned actors", test_scheduler_spawn_aligned, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler actor table grows under readers", test_scheduler_actor_table_growth, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler frees released actors once no reader holds them", test_scheduler_release_churn, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler teardown frees the live actors it spawned", test_scheduler_teardown_frees_live_actors, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler stop never writes a foreign queue", test_scheduler_stop_leaves_foreign_queue_alone, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler teardown destroys the core locks", test_scheduler_teardown_destroys_core_locks, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler teardown with a live actor thread", test_scheduler_teardown_with_live_actor_thread, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler actor thread holds a released actor it sent to", test_scheduler_actor_thread_holds_released_actor, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler actor thread parks when idle", test_scheduler_actor_thread_parks_when_idle, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler fires receive timeouts on a core and an actor thread", test_scheduler_fires_receive_timeouts, TEST_CATEGORY_RUNTIME);
#if defined(__linux__) || defined(_WIN32)
    register_test_with_category("Scheduler counts and places its threads by the process's CPUs", test_scheduler_places_threads_among_process_cpus, TEST_CATEGORY_RUNTIME);
#endif
    register_test_with_category("Scheduler batch send wakes parked cores", test_scheduler_batch_wakes_parked_cores, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler core with messages to deliver does not park", test_scheduler_core_with_overflow_does_not_park, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler actor thread inbox keeps every sender's order", test_scheduler_actor_thread_inbox_keeps_order, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler drops a send to a released actor", test_scheduler_late_send_after_release, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler reuses no block with messages on their way", test_scheduler_reuse_waits_for_messages_in_flight, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler basic messaging", test_scheduler_basic_messaging, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler message ordering", test_scheduler_message_ordering, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler cross-core messaging", test_scheduler_cross_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler clean exit", test_scheduler_exit_clean, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler high throughput", test_scheduler_high_throughput, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler backpressure handling", test_scheduler_backpressure, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler send_local from a foreign thread loses nothing", test_scheduler_send_local_from_foreign_thread, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: an actor is stepped after a long idle", test_ready_list_steps_actor_after_long_idle, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: a timeout fires on time among idle actors", test_ready_list_timeout_fires_on_time_among_idle_actors, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: no lost wakeup under concurrent senders", test_ready_list_no_lost_wakeup_under_concurrent_senders, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: duplicates do not double-step", test_ready_list_duplicates_do_not_double_step, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: a moved actor is stepped on its new core", test_ready_list_moved_actor_stepped_on_new_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler stepping loop yields to its queues", test_scheduler_stepping_loop_yields_to_queues, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler restart hands a carried actor over", test_scheduler_restart_hands_carried_actor_over, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler end during shutdown is finalized by the core", test_scheduler_end_during_shutdown_finalized_by_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Ready list: a registration finds an armed countdown", test_ready_list_registration_finds_armed_countdown, TEST_CATEGORY_RUNTIME);
}
