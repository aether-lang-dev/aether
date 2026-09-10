// Test suite for lock-free mailbox implementation
// Tests correctness, performance, and thread safety

#include "test_harness.h"
#include "../../runtime/actors/lockfree_mailbox.h"
#include "../../runtime/actors/actor_state_machine.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>

#define TEST_COUNT 10000
#define THREAD_COUNT 2

// Test 1: Basic send/receive
TEST_CATEGORY(lockfree_mailbox_basic_operations, TEST_CATEGORY_RUNTIME) {
    LockFreeMailbox mbox;
    lockfree_mailbox_init(&mbox);
    
    Message msg = message_create_simple(1, 0, 42);
    Message recv_msg;
    
    // Test send
    ASSERT_TRUE(lockfree_mailbox_send(&mbox, msg) == 1);
    
    // Test receive
    ASSERT_TRUE(lockfree_mailbox_receive(&mbox, &recv_msg) == 1);
    ASSERT_EQ(1, recv_msg.type);
    ASSERT_EQ(42, recv_msg.payload_int);
    
    // Test empty
    ASSERT_TRUE(lockfree_mailbox_receive(&mbox, &recv_msg) == 0);

}

// Test 2: Fill and drain
TEST_CATEGORY(lockfree_mailbox_capacity, TEST_CATEGORY_RUNTIME) {
    LockFreeMailbox mbox;
    lockfree_mailbox_init(&mbox);
    
    Message msg = message_create_simple(1, 0, 0);
    
    // Fill to capacity (63 messages, size is 64)
    int sent = 0;
    for (int i = 0; i < 100; i++) {
        msg.payload_int = i;
        if (lockfree_mailbox_send(&mbox, msg)) {
            sent++;
        } else {
            break;
        }
    }
    
    ASSERT_EQ(63, sent);  // One slot always empty in SPSC
    
    // Drain all
    Message recv_msg;
    int received = 0;
    for (int i = 0; i < 100; i++) {
        if (lockfree_mailbox_receive(&mbox, &recv_msg)) {
            ASSERT_EQ(i, recv_msg.payload_int);
            received++;
        } else {
            break;
        }
    }
    
    ASSERT_EQ(sent, received);
    ASSERT_TRUE(lockfree_mailbox_is_empty(&mbox));
}

// Test 3: Batch operations
TEST_CATEGORY(lockfree_mailbox_batch_operations, TEST_CATEGORY_RUNTIME) {
    LockFreeMailbox mbox;
    lockfree_mailbox_init(&mbox);
    
    Message msgs[16];
    for (int i = 0; i < 16; i++) {
        msgs[i] = message_create_simple(1, 0, i);
    }
    
    // Batch send
    int sent = lockfree_mailbox_send_batch(&mbox, msgs, 16);
    ASSERT_EQ(16, sent);
    
    // Batch receive
    Message recv_msgs[16];
    int received = lockfree_mailbox_receive_batch(&mbox, recv_msgs, 16);
    ASSERT_EQ(16, received);
    
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(i, recv_msgs[i].payload_int);
    }
}

// Thread safety test data
typedef struct {
    LockFreeMailbox* mbox;
    int id;
    int* counter;
} ThreadData;

void* producer_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    Message msg = message_create_simple(1, data->id, 0);
    
    for (int i = 0; i < TEST_COUNT / THREAD_COUNT; i++) {
        msg.payload_int = i;
        while (!lockfree_mailbox_send(data->mbox, msg)) {
            sched_yield();  // Allow consumer to drain under instrumentation
        }
    }
    
    return NULL;
}

void* consumer_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    Message msg;
    int count = 0;
    
    while (count < TEST_COUNT / THREAD_COUNT) {
        if (lockfree_mailbox_receive(data->mbox, &msg)) {
            (*data->counter)++;
            count++;
        } else {
            sched_yield();  // Allow producer to fill under instrumentation
        }
    }
    
    return NULL;
}

TEST_CATEGORY(lockfree_mailbox_thread_safety, TEST_CATEGORY_RUNTIME) {
    // SPSC = Single Producer Single Consumer
    // Test with ONE producer and ONE consumer thread
    LockFreeMailbox mbox;
    lockfree_mailbox_init(&mbox);
    
    int counter = 0;
    pthread_t producer;
    pthread_t consumer;
    ThreadData producer_data = {&mbox, 0, &counter};
    ThreadData consumer_data = {&mbox, 1, &counter};
    
    // Start single producer
    pthread_create(&producer, NULL, producer_thread, &producer_data);
    
    // Start single consumer
    pthread_create(&consumer, NULL, consumer_thread, &consumer_data);
    
    // Wait for completion
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    
    ASSERT_EQ(TEST_COUNT / THREAD_COUNT, counter);
}

// Note: Tests are auto-registered via TEST_CATEGORY macro
void register_lockfree_mailbox_tests(void) {
    // Empty - tests registered by constructor
}
