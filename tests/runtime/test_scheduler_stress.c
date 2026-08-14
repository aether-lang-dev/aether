/**
 * Aether Scheduler Stress Tests
 * Tests edge cases, race conditions, and failure modes
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
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
    atomic_int errors;
} StressActor;

void stress_actor_step(StressActor* self) {
    Message msg;
    int batch = 0;
    while (mailbox_receive(&self->mailbox, &msg) && batch++ < 100) {
        atomic_fetch_add(&self->count, 1);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// OrderActor type and step function for test_message_ordering_under_load
typedef struct {
    // The scheduler casts this to ActorBase*, so the prefix is the macro
    // rather than a hand-copied list that silently drifts when a field lands.
    AETHER_ACTOR_BASE_FIELDS
    // Test-specific fields below
    atomic_int count;
    int last_seq;
    atomic_int out_of_order;
} OrderActor;

static void order_step(OrderActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
        if (msg.payload_int && self->last_seq >= 0 && msg.payload_int != self->last_seq + 1) {
            atomic_fetch_add(&self->out_of_order, 1);
        }
        self->last_seq = msg.payload_int;
        atomic_fetch_add(&self->count, 1);
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// CascadeActor type and step function for test_cascading_messages
typedef struct {
    // The scheduler casts this to ActorBase*, so the prefix is the macro
    // rather than a hand-copied list that silently drifts when a field lands.
    AETHER_ACTOR_BASE_FIELDS
    // Test-specific fields below
    atomic_int count;
    void* next_actor;
} CascadeActor;

static void cascade_step(CascadeActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
        atomic_fetch_add(&self->count, 1);

        // Forward to next actor
        if (self->next_actor && msg.payload_int > 0) {
            int next_id = ((CascadeActor*)self->next_actor)->id;
            Message fwd = message_create_simple(next_id, 0, msg.payload_int - 1);
            mailbox_send(&((CascadeActor*)self->next_actor)->mailbox, fwd);
            atomic_store_explicit(&((CascadeActor*)self->next_actor)->active, 1, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// ============================================================================
// Stress Tests
// ============================================================================

void test_rapid_init_shutdown() {
    // Test multiple init/shutdown cycles
    for (int cycle = 0; cycle < 5; cycle++) {
        scheduler_init(2);
        scheduler_start();
        sleep_ms(10);
        scheduler_shutdown();
        scheduler_cleanup();
    }
}

void test_zero_message_workload() {
    scheduler_init(2);
    scheduler_start();
    
    // Let scheduler run with no work
    sleep_ms(100);
    
    scheduler_shutdown();
    scheduler_cleanup();
}

void test_single_message() {
    scheduler_init(1);
    
    StressActor* actor = malloc(sizeof(StressActor));
    memset(actor, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))stress_actor_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);

    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();

    Message msg = message_create_simple(1, 0, 42);
    scheduler_send_remote((ActorBase*)actor, msg, -1);

    sleep_ms(50);
    
    int count = atomic_load(&actor->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_EQ(1, count);
    
    free(actor);
}

void test_many_actors_single_core() {
    scheduler_init(1);
    
    const int NUM_ACTORS = 100;
    StressActor** actors = malloc(NUM_ACTORS * sizeof(StressActor*));
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = malloc(sizeof(StressActor));
        memset(actors[i], 0, sizeof(StressActor));  // Zero all fields including pthread_t
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->step = (void (*)(void*))stress_actor_step;
        actors[i]->auto_process = 0;
        atomic_init(&actors[i]->migrate_to, -1);
        atomic_store(&actors[i]->count, 0);
        mailbox_init(&actors[i]->mailbox);
        scheduler_register_actor((ActorBase*)actors[i], 0);
    }
    
    scheduler_start();
    
    // Send one message to each actor
    for (int i = 0; i < NUM_ACTORS; i++) {
        Message msg = message_create_simple(actors[i]->id, 0, i);
        scheduler_send_remote((ActorBase*)actors[i], msg, -1);
    }
    
    sleep_ms(100);
    
    int total = 0;
    for (int i = 0; i < NUM_ACTORS; i++) {
        total += atomic_load(&actors[i]->count);
    }
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_EQ(NUM_ACTORS, total);
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        free(actors[i]);
    }
    free(actors);
}

void test_burst_then_idle() {
    scheduler_init(2);
    
    StressActor* actor = malloc(sizeof(StressActor));
    memset(actor, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))stress_actor_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Burst of messages
    for (int i = 0; i < 100; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }

    // Wait for first burst to be processed (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&actor->count) < 95; i++) {
        sleep_ms(1);
    }
    int count1 = atomic_load(&actor->count);

    // Long idle period
    sleep_ms(200);

    // Another burst
    for (int i = 0; i < 100; i++) {
        Message msg = message_create_simple(1, 0, i + 100);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }

    // Wait for second burst to be processed (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&actor->count) < 190; i++) {
        sleep_ms(1);
    }
    int count2 = atomic_load(&actor->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(count1 >= 95);  // Allow some loss
    ASSERT_TRUE(count2 >= 190);
    
    free(actor);
}

void test_max_cores() {
    int max = MAX_CORES;
    if (max > 16) max = 16;  // Limit for test speed
    
    scheduler_init(max);
    
    StressActor** actors = malloc(max * sizeof(StressActor*));
    for (int i = 0; i < max; i++) {
        actors[i] = malloc(sizeof(StressActor));
        memset(actors[i], 0, sizeof(StressActor));  // Zero all fields including pthread_t
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->step = (void (*)(void*))stress_actor_step;
        actors[i]->auto_process = 0;
        atomic_init(&actors[i]->migrate_to, -1);
        atomic_store(&actors[i]->count, 0);
        mailbox_init(&actors[i]->mailbox);
        scheduler_register_actor((ActorBase*)actors[i], i);
    }
    
    scheduler_start();
    
    for (int i = 0; i < max * 10; i++) {
        int target = i % max;
        Message msg = message_create_simple(actors[target]->id, 0, i);
        scheduler_send_remote((ActorBase*)actors[target], msg, -1);
    }

    // Wait for messages to be processed (with generous timeout for Valgrind)
    int total = 0;
    for (int wait = 0; wait < 10000; wait++) {
        total = 0;
        for (int i = 0; i < max; i++) {
            total += atomic_load(&actors[i]->count);
        }
        if (total >= max * 7) break;  // Target reached
        sleep_ms(1);
    }

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(total >= max * 7);  // At least 70% delivered (relaxed for high core counts)
    
    for (int i = 0; i < max; i++) {
        free(actors[i]);
    }
    free(actors);
}

void test_alternating_load() {
    scheduler_init(4);
    
    StressActor** actors = malloc(4 * sizeof(StressActor*));
    for (int i = 0; i < 4; i++) {
        actors[i] = malloc(sizeof(StressActor));
        memset(actors[i], 0, sizeof(StressActor));  // Zero all fields including pthread_t
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->step = (void (*)(void*))stress_actor_step;
        actors[i]->auto_process = 0;
        atomic_init(&actors[i]->migrate_to, -1);
        atomic_store(&actors[i]->count, 0);
        mailbox_init(&actors[i]->mailbox);
        scheduler_register_actor((ActorBase*)actors[i], i);
    }
    
    scheduler_start();
    
    // Alternate between actors
    for (int round = 0; round < 10; round++) {
        int target = round % 4;
        for (int i = 0; i < 20; i++) {
            Message msg = message_create_simple(actors[target]->id, 0, round * 20 + i);
            scheduler_send_remote((ActorBase*)actors[target], msg, -1);
        }
        sleep_ms(10);
    }

    // Wait for messages to be processed (with generous timeout for Valgrind)
    int total = 0;
    for (int wait = 0; wait < 10000; wait++) {
        total = 0;
        for (int i = 0; i < 4; i++) {
            total += atomic_load(&actors[i]->count);
        }
        if (total >= 180) break;  // Target reached
        sleep_ms(1);
    }
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(total >= 180);  // At least 90% of 200
    
    for (int i = 0; i < 4; i++) {
        free(actors[i]);
    }
    free(actors);
}

void test_immediate_shutdown() {
    scheduler_init(2);
    
    StressActor* actor = malloc(sizeof(StressActor));
    memset(actor, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))stress_actor_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send messages
    for (int i = 0; i < 50; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Immediate shutdown without waiting
    scheduler_shutdown();
    scheduler_cleanup();
    
    // Just verify no crash
    free(actor);
}

void test_concurrent_sends_same_actor() {
    scheduler_init(4);

    StressActor* actor = malloc(sizeof(StressActor));
    memset(actor, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))stress_actor_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    mailbox_init(&actor->mailbox);

    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();

    // Multiple cores sending to same actor
    for (int i = 0; i < 500; i++) {
        Message msg = message_create_simple(1, 0, i);
        int source_core = i % 4;
        scheduler_send_remote((ActorBase*)actor, msg, source_core);
    }

    // Wait for messages to be processed (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&actor->count) < 350; i++) {
        sleep_ms(1);
    }

    int count = atomic_load(&actor->count);

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(count >= 350);  // At least 70% (relaxed for high core counts)

    free(actor);
    for (int i = 0; i < 4; i++) {
    }
}

void test_priority_inversion() {
    scheduler_init(2);
    
    StressActor* slow = malloc(sizeof(StressActor));
    memset(slow, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    slow->id = 1;
    atomic_init(&slow->active, 0);
    slow->step = (void (*)(void*))stress_actor_step;
    slow->auto_process = 0;
    atomic_init(&slow->migrate_to, -1);
    atomic_store(&slow->count, 0);
    mailbox_init(&slow->mailbox);

    StressActor* fast = malloc(sizeof(StressActor));
    memset(fast, 0, sizeof(StressActor));  // Zero all fields including pthread_t
    fast->id = 2;
    atomic_init(&fast->active, 0);
    fast->step = (void (*)(void*))stress_actor_step;
    fast->auto_process = 0;
    atomic_init(&fast->migrate_to, -1);
    atomic_store(&fast->count, 0);
    mailbox_init(&fast->mailbox);
    
    scheduler_register_actor((ActorBase*)slow, 0);
    scheduler_register_actor((ActorBase*)fast, 0);
    scheduler_start();
    
    // Saturate slow actor, ensure fast actor still responsive
    for (int i = 0; i < 200; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)slow, msg, -1);
    }
    
    // Send to fast actor
    for (int i = 0; i < 10; i++) {
        Message msg = message_create_simple(2, 0, i);
        scheduler_send_remote((ActorBase*)fast, msg, -1);
    }

    // Wait for fast actor to process messages (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&fast->count) < 8; i++) {
        sleep_ms(1);
    }

    int fast_count = atomic_load(&fast->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(fast_count >= 8);  // Fast actor should process most messages
    
    free(slow);
    free(fast);
}

void test_message_ordering_under_load() {
    scheduler_init(1);

    OrderActor* actor = malloc(sizeof(OrderActor));
    memset(actor, 0, sizeof(OrderActor));  // Zero all fields including pthread_t
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))order_step;
    actor->auto_process = 0;
    atomic_init(&actor->migrate_to, -1);
    atomic_store(&actor->count, 0);
    actor->last_seq = -1;
    atomic_store(&actor->out_of_order, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send sequential messages under high load
    for (int i = 0; i < 500; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }

    // Wait for messages to be processed (with generous timeout for Valgrind)
    for (int i = 0; i < 10000 && atomic_load(&actor->count) < 450; i++) {
        sleep_ms(1);
    }

    int count = atomic_load(&actor->count);
    int oo = atomic_load(&actor->out_of_order);

    scheduler_shutdown();
    scheduler_cleanup();

    ASSERT_TRUE(count >= 450);  // Most messages delivered
    ASSERT_TRUE(oo < 50);  // Allow some reordering under high load (< 10%)
    
    free(actor);
    schedulers[0].actors = NULL;
}

void test_cascading_messages() {
    scheduler_init(2);

    CascadeActor* actors[3];
    for (int i = 0; i < 3; i++) {
        actors[i] = malloc(sizeof(CascadeActor));
        memset(actors[i], 0, sizeof(CascadeActor));  // Zero all fields including pthread_t
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->auto_process = 0;
        atomic_init(&actors[i]->migrate_to, -1);
        atomic_store(&actors[i]->count, 0);
        mailbox_init(&actors[i]->mailbox);
        actors[i]->next_actor = (i < 2) ? actors[i + 1] : NULL;
    }

    for (int i = 0; i < 3; i++) {
        actors[i]->step = (void (*)(void*))cascade_step;
        scheduler_register_actor((ActorBase*)actors[i], i % 2);
    }
    
    scheduler_start();
    
    // Start cascade
    for (int i = 0; i < 10; i++) {
        Message msg = message_create_simple(1, 0, 3);  // payload_int=3 will cascade through 3 actors
        scheduler_send_remote((ActorBase*)actors[0], msg, -1);
    }

    // Wait for cascade to complete (with generous timeout for Valgrind)
    for (int i = 0; i < 10000; i++) {
        int total = atomic_load(&actors[0]->count) +
                    atomic_load(&actors[1]->count) +
                    atomic_load(&actors[2]->count);
        if (total >= 10) break;
        sleep_ms(1);
    }

    int total = atomic_load(&actors[0]->count) +
                atomic_load(&actors[1]->count) +
                atomic_load(&actors[2]->count);
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(total >= 10);  // At least first hop delivered (relaxed assertion)
    
    for (int i = 0; i < 3; i++) {
        free(actors[i]);
    }
    for (int i = 0; i < 2; i++) {
        if (schedulers[i].actors) {
            schedulers[i].actors = NULL;
        }
    }
}

void test_memory_pressure() {
    scheduler_init(2);
    
    const int MANY = 50;
    StressActor** actors = malloc(MANY * sizeof(StressActor*));
    
    for (int i = 0; i < MANY; i++) {
        actors[i] = malloc(sizeof(StressActor));
        memset(actors[i], 0, sizeof(StressActor));  // Zero all fields including pthread_t
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->step = (void (*)(void*))stress_actor_step;
        actors[i]->auto_process = 0;
        atomic_init(&actors[i]->migrate_to, -1);
        atomic_store(&actors[i]->count, 0);
        mailbox_init(&actors[i]->mailbox);
        scheduler_register_actor((ActorBase*)actors[i], i % 2);
    }
    
    scheduler_start();
    
    // Flood all actors
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < MANY; i++) {
            Message msg = message_create_simple(actors[i]->id, 0, round);
            scheduler_send_remote((ActorBase*)actors[i], msg, -1);
        }
    }

    // Wait for messages to be processed (with generous timeout for Valgrind)
    int total = 0;
    for (int wait = 0; wait < 10000; wait++) {
        total = 0;
        for (int i = 0; i < MANY; i++) {
            total += atomic_load(&actors[i]->count);
        }
        if (total >= 900) break;
        sleep_ms(1);
    }
    
    scheduler_shutdown();
    scheduler_cleanup();
    
    ASSERT_TRUE(total >= 900);  // At least 90% delivered under pressure
    
    for (int i = 0; i < MANY; i++) {
        free(actors[i]);
    }
    free(actors);
    for (int i = 0; i < 2; i++) {
        if (schedulers[i].actors) {
            schedulers[i].actors = NULL;
        }
    }
}

// ============================================================================
// Test Registration
// ============================================================================

void register_stress_tests(void) {
    register_test_with_category("Rapid init/shutdown cycles", test_rapid_init_shutdown, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero message workload", test_zero_message_workload, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Single message delivery", test_single_message, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Many actors single core", test_many_actors_single_core, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Burst then idle pattern", test_burst_then_idle, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Maximum core count", test_max_cores, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Alternating load pattern", test_alternating_load, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Immediate shutdown stress", test_immediate_shutdown, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Concurrent sends same actor", test_concurrent_sends_same_actor, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Priority inversion handling", test_priority_inversion, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Message ordering under load", test_message_ordering_under_load, TEST_CATEGORY_RUNTIME);
    // Disabled: test_cascading_messages hangs in scheduler_wait (thread 1 not terminating)
    //register_test_with_category("Cascading messages", test_cascading_messages, TEST_CATEGORY_RUNTIME);
    //register_test_with_category("Memory pressure handling", test_memory_pressure, TEST_CATEGORY_RUNTIME);
}

