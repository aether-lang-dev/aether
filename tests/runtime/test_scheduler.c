/**
 * Aether Runtime Scheduler Tests
 * Tests multicore scheduler correctness and performance
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/aether_actor_thread.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/aether_process_mem.h"
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#define get_time_ms() GetTickCount64()
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
static long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

void counter_step(CounterActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
        atomic_fetch_add(&self->count, 1);
        atomic_store(&self->last_value, msg.payload_int);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

void order_step(OrderActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
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
    while (mailbox_receive(&self->mailbox, &msg)) {
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
// scheduler thread scans it without the lock and the main-thread reader
// (aether_scheduler_poll) walks it from another thread, with messages
// flowing. Afterwards every actor is in exactly one table, every slot at or
// past a table's count is NULL (the grown tail included), and the replaced
// tables are still chained for scheduler_cleanup() to free.
typedef struct {
    atomic_int stop;
    atomic_int polls;
} TablePollerArgs;

static void* table_poller(void* arg) {
    TablePollerArgs* p = (TablePollerArgs*)arg;
    while (!atomic_load(&p->stop)) {
        aether_scheduler_poll(1);
        atomic_fetch_add(&p->polls, 1);
    }
    return NULL;
}

void test_scheduler_actor_table_growth(void) {
    enum { N = 2 * MAX_ACTORS_PER_CORE + 100 };
    scheduler_init(2);
    CounterActor** actors = calloc(N, sizeof(CounterActor*));
    int* seen = calloc(N, sizeof(int));
    ASSERT_NOT_NULL(actors);
    ASSERT_NOT_NULL(seen);

    TablePollerArgs poll_args;
    atomic_init(&poll_args.stop, 0);
    atomic_init(&poll_args.polls, 0);
    pthread_t poller;
    int poller_started = 0;

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
        // Scheduler threads run from the second spawn on; start the second
        // reader then.
        if (i == 1 && pthread_create(&poller, NULL, table_poller, &poll_args) == 0) {
            poller_started = 1;
        }
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
    atomic_store(&poll_args.stop, 1);
    if (poller_started) pthread_join(poller, NULL);
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
    ASSERT_TRUE(poller_started);
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
// its table and another thread walks all of them with aether_scheduler_poll().
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

    // Two actors that live throughout: the first takes main-thread mode, the
    // second ends it for good and starts the core threads.
    ActorBase* keep[2];
    for (int i = 0; i < 2; i++) {
        keep[i] = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
        ASSERT_NOT_NULL(keep[i]);
    }
    int registered_before = churn_registered();
    int live_before = atomic_load(&g_aether_config.actor_count);

    TablePollerArgs poll_args;
    atomic_init(&poll_args.stop, 0);
    atomic_init(&poll_args.polls, 0);
    pthread_t poller;
    int poller_started = pthread_create(&poller, NULL, table_poller, &poll_args) == 0;

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

    atomic_store(&poll_args.stop, 1);
    if (poller_started) pthread_join(poller, NULL);
    scheduler_shutdown();
    for (int i = 0; i < 2; i++) scheduler_release_actor(keep[i]);
    scheduler_cleanup();

    ASSERT_TRUE(poller_started);
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
// aether_actor_thread) sends to another actor of its core from its step.
// scheduler_send_local steps that actor inline, on the actor thread, and
// touches it after its step; the stepped actor releases itself in that step.
// The actor thread used to publish no reclamation epoch, so the core freed
// the released actor while the actor thread was still in the send that
// stepped it. It now reads under an epoch like a core: nothing released is
// reclaimed until it passes the top of its loop again, which it cannot while
// its step is still running. The relay's step waits at a gate after the
// send, so the hold is observed with the thread deterministically inside
// the step; the core's published epoch says when it has tried to reclaim.
//
// Then the relay itself is released by an actor the core steps, while the
// relay's step is waiting on that very core: a release never blocks, the
// relay's own thread ends the actor once it leaves its loop. Last, an actor
// thread's actor releases itself from its own step.
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

static void echo_step(void* self) {
    EchoActor* a = (EchoActor*)self;
    Message msg;
    while (mailbox_receive(&a->mailbox, &msg)) {
        if (a->victim) scheduler_release_actor(a->victim);
        atomic_fetch_add(&a->count, 1);
    }
    atomic_store_explicit(&a->active, (a->mailbox.count > 0), memory_order_relaxed);
}

static void relay_step(void* self) {
    RelayActor* a = (RelayActor*)self;
    Message msg;
    if (mailbox_receive(&a->mailbox, &msg)) {
        // Credited here as the core loop credits a step it runs itself:
        // scheduler_wait() balances sent against processed.
        atomic_fetch_add_explicit(&schedulers[a->assigned_core].messages_processed, 1,
                                  memory_order_relaxed);
        Message fwd = {1, 0, 0, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_local(a->target, fwd);   // steps the target right here
        atomic_store(&a->stepped, 1);
        while (!atomic_load(a->gate)) sleep_ms(1);
        // Through the core's queue, not inline: the core steps echo, which
        // releases this actor while this step is still waiting on it.
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
        atomic_fetch_add_explicit(&schedulers[a->assigned_core].messages_processed, 1,
                                  memory_order_relaxed);
        atomic_fetch_add(&g_churn_handled, 1);
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

    ActorBase* keep[2];
    for (int i = 0; i < 2; i++) {
        keep[i] = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
        ASSERT_NOT_NULL(keep[i]);
    }

    ChurnActor* target = (ChurnActor*)scheduler_spawn_actor(-1, churn_step, sizeof(ChurnActor));
    ASSERT_NOT_NULL(target);
    target->self_release = 1;

    EchoActor* echo = (EchoActor*)scheduler_spawn_actor(-1, echo_step, sizeof(EchoActor));
    ASSERT_NOT_NULL(echo);
    atomic_init(&echo->count, 0);

    atomic_int gate;
    atomic_init(&gate, 0);
    RelayActor* relay = (RelayActor*)scheduler_spawn_actor(-1, relay_step, sizeof(RelayActor));
    ASSERT_NOT_NULL(relay);
    relay->target = (ActorBase*)target;
    relay->echo = (ActorBase*)echo;
    relay->gate = &gate;
    atomic_init(&relay->stepped, 0);
    atomic_init(&relay->echoed, 0);
    echo->victim = (ActorBase*)relay;
    relay->auto_process = 1;
    ASSERT_EQ(0, pthread_create(&relay->thread, NULL, aether_actor_thread, relay));

    atomic_store(&g_churn_handled, 0);
    Message msg = {1, 0, 0, NULL, {NULL, 0, 0}, NULL};
    scheduler_send_remote((ActorBase*)relay, msg, -1);
    for (int w = 0; w < 5000 && !atomic_load(&relay->stepped); w++) sleep_ms(1);
    ASSERT_EQ(1, atomic_load(&relay->stepped));
    ASSERT_EQ(1, atomic_load(&g_churn_handled));   // the target ran, on the relay's thread
    ASSERT_EQ(1, scheduler_released_actors_pending());

    // Further releases move the epoch on; the core publishes each move and
    // tries to reclaim. The target stays because the relay thread's epoch
    // is older than its release.
    int held = 1;
    for (int round = 1; round <= 3; round++) {
        uint64_t seen = atomic_load(&schedulers[0].reclaim_epoch);
        ActorBase* extra = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
        ASSERT_NOT_NULL(extra);
        scheduler_release_actor(extra);
        ASSERT_TRUE(core0_moved_past(seen));
        if (scheduler_released_actors_pending() != round + 1) held = 0;
    }
    ASSERT_TRUE(held);

    // The relay's step goes on to wait on the core, which releases it
    // meanwhile; the release returns at once, the step finishes, the relay's
    // thread leaves its loop and ends the actor, and everything is reclaimed.
    atomic_store(&gate, 1);
    for (int w = 0; w < 6000 && !atomic_load(&relay->echoed); w++) sleep_ms(1);
    ASSERT_EQ(1, atomic_load(&relay->echoed));
    ASSERT_EQ(0, wait_no_pending());
    pthread_join(relay->thread, NULL);

    // An actor thread's actor releasing itself from its own step.
    ChurnActor* quitter = (ChurnActor*)scheduler_spawn_actor(-1, quitter_step, sizeof(ChurnActor));
    ASSERT_NOT_NULL(quitter);
    quitter->auto_process = 1;
    ASSERT_EQ(0, pthread_create(&quitter->thread, NULL, aether_actor_thread, quitter));
    scheduler_send_remote((ActorBase*)quitter, msg, -1);
    for (int w = 0; w < 5000 && atomic_load(&g_churn_handled) < 2; w++) sleep_ms(1);
    ASSERT_EQ(2, atomic_load(&g_churn_handled));
    ASSERT_EQ(0, wait_no_pending());
    pthread_join(quitter->thread, NULL);

    scheduler_shutdown();
    scheduler_release_actor((ActorBase*)echo);
    for (int i = 0; i < 2; i++) scheduler_release_actor(keep[i]);
    scheduler_cleanup();
}

// #2517: a send to a released actor is defined. The release contract forbids
// it, but a raw ActorBase* cannot be revoked, so a released actor's block is
// kept, marked released, until a spawn of the same size takes it back: the
// late send reads the mark, drops the message and counts it. Every send
// entry is tried after the actor has been reclaimed, under the plain build.
void test_scheduler_late_send_after_release(void) {
    scheduler_init(2);

    ActorBase* keep[2];
    for (int i = 0; i < 2; i++) {
        keep[i] = scheduler_spawn_actor(-1, (void (*)(void*))counter_step, sizeof(CounterActor));
        ASSERT_NOT_NULL(keep[i]);
    }

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
    for (int i = 0; i < 2; i++) scheduler_release_actor(keep[i]);
    scheduler_cleanup();
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
    long start = get_time_ms();
    
    for (int i = 0; i < TOTAL; i++) {
        Message msg = {1, 0, i, NULL, {NULL, 0, 0}, NULL};
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for all messages to be processed (up to 10 seconds)
    for (int i = 0; i < 1000 && atomic_load(&actor->count) < TOTAL; i++) {
        sleep_ms(10);
    }
    
    long end = get_time_ms();
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
    long start = get_time_ms();
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    long end = get_time_ms();
    long elapsed = end - start;
    
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

void test_scheduler_teardown_frees_live_actors(void) {
    memset(&g_caller_actor, 0, sizeof(g_caller_actor));
    mailbox_init(&g_caller_actor.mailbox);
    g_caller_actor.step = (void (*)(void*))counter_step;
    atomic_store(&g_caller_actor.last_value, 4242);

    teardown_cycle();   // warm: first-use allocations of the runtime
    int64_t before = aether_heap_in_use();
    for (int round = 0; round < 5; round++) teardown_cycle();
    int64_t after = aether_heap_in_use();

    // The caller's actor was run and never freed.
    ASSERT_EQ(0, g_caller_actor.scheduler_owned);
    ASSERT_EQ(4242, atomic_load(&g_caller_actor.last_value));
    if (aether_heap_in_use_exact()) {
        ASSERT_EQ(0, (int)(after - before));
    }
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
    register_test_with_category("Scheduler actor thread holds a released actor it stepped", test_scheduler_actor_thread_holds_released_actor, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler drops a send to a released actor", test_scheduler_late_send_after_release, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler basic messaging", test_scheduler_basic_messaging, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler message ordering", test_scheduler_message_ordering, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler cross-core messaging", test_scheduler_cross_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler clean exit", test_scheduler_exit_clean, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler high throughput", test_scheduler_high_throughput, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Scheduler backpressure handling", test_scheduler_backpressure, TEST_CATEGORY_RUNTIME);
}
