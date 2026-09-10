/**
 * Zero-Copy Message Tests
 * Validates zero-copy message passing for large payloads
 */

#include "test_harness.h"
#include "../../runtime/actors/actor_state_machine.h"
#include <stdlib.h>
#include <string.h>

void test_zerocopy_message_creation(void) {
    // Test small message (no zero-copy)
    Message msg1 = message_create_simple(MSG_INCREMENT, 1, 42);
    ASSERT_EQ(msg1.type, MSG_INCREMENT);
    ASSERT_EQ(msg1.sender_id, 1);
    ASSERT_EQ(msg1.payload_int, 42);
    ASSERT_TRUE(msg1.zerocopy.data == NULL);
    ASSERT_EQ(msg1.zerocopy.size, 0);
    ASSERT_EQ(msg1.zerocopy.owned, 0);
    
    // Test large message (zero-copy)
    int large_size = 512;
    void* large_data = malloc(large_size);
    memset(large_data, 0xAB, large_size);
    
    Message msg2 = message_create_zerocopy(MSG_USER_START, 2, large_data, large_size);
    ASSERT_EQ(msg2.type, MSG_USER_START);
    ASSERT_EQ(msg2.sender_id, 2);
    ASSERT_TRUE(msg2.zerocopy.data == large_data);
    ASSERT_EQ(msg2.zerocopy.size, large_size);
    ASSERT_EQ(msg2.zerocopy.owned, 1);
    
    // Clean up
    message_free(&msg2);
}

void test_zerocopy_message_transfer(void) {
    int size = 1024;
    void* data = malloc(size);
    memset(data, 0xCD, size);
    
    Message src = message_create_zerocopy(MSG_USER_START + 1, 5, data, size);
    Message dest;
    memset(&dest, 0, sizeof(Message));
    
    // Transfer ownership
    message_transfer(&dest, &src);
    
    ASSERT_EQ(dest.type, MSG_USER_START + 1);
    ASSERT_EQ(dest.sender_id, 5);
    ASSERT_TRUE(dest.zerocopy.data == data);
    ASSERT_EQ(dest.zerocopy.size, size);
    ASSERT_EQ(dest.zerocopy.owned, 1);
    
    // Source should no longer own the data
    ASSERT_EQ(src.zerocopy.owned, 0);
    
    // Clean up
    message_free(&dest);
}

void test_zerocopy_mailbox_operations(void) {
    Mailbox mbox;
    mailbox_init(&mbox);
    
    // Send small messages
    for (int i = 0; i < 10; i++) {
        Message msg = message_create_simple(MSG_INCREMENT, i, i * 10);
        int result = mailbox_send(&mbox, msg);
        ASSERT_EQ(result, 1);
    }
    ASSERT_EQ(mbox.count, 10);
    
    // Send large zero-copy message
    void* data = malloc(512);
    memset(data, 0xEF, 512);
    Message zcmsg = message_create_zerocopy(MSG_USER_START + 10, 99, data, 512);
    int result = mailbox_send(&mbox, zcmsg);
    ASSERT_EQ(result, 1);
    ASSERT_EQ(mbox.count, 11);
    
    // Receive all messages
    Message received;
    int count = 0;
    while (mailbox_receive(&mbox, &received)) {
        count++;
        if (received.zerocopy.owned && received.zerocopy.data) {
            ASSERT_EQ(received.zerocopy.size, 512);
            message_free(&received);
        }
    }
    ASSERT_EQ(count, 11);
    ASSERT_EQ(mbox.count, 0);
}

void test_zerocopy_threshold(void) {
    // Messages below threshold should not allocate
    Message small = message_create_simple(MSG_INCREMENT, 1, 100);
    ASSERT_TRUE(small.zerocopy.data == NULL);
    ASSERT_EQ(small.zerocopy.size, 0);
    
    // Messages above threshold use zero-copy
    void* data = malloc(ZEROCOPY_THRESHOLD + 1);
    Message large = message_create_zerocopy(MSG_USER_START, 1, data, ZEROCOPY_THRESHOLD + 1);
    ASSERT_TRUE(large.zerocopy.data != NULL);
    ASSERT_EQ(large.zerocopy.size, ZEROCOPY_THRESHOLD + 1);
    ASSERT_EQ(large.zerocopy.owned, 1);
    
    message_free(&large);
}

void test_zerocopy_batch_operations(void) {
    Mailbox mbox;
    mailbox_init(&mbox);
    
    // Create batch of messages with mix of small and large
    Message messages[5];
    messages[0] = message_create_simple(MSG_INCREMENT, 1, 10);
    messages[1] = message_create_simple(MSG_INCREMENT, 1, 20);
    
    void* data1 = malloc(512);
    messages[2] = message_create_zerocopy(MSG_USER_START, 1, data1, 512);
    
    messages[3] = message_create_simple(MSG_INCREMENT, 1, 30);
    
    void* data2 = malloc(1024);
    messages[4] = message_create_zerocopy(MSG_USER_START + 1, 1, data2, 1024);
    
    // Send batch
    int sent = mailbox_send_batch(&mbox, messages, 5);
    ASSERT_EQ(sent, 5);
    ASSERT_EQ(mbox.count, 5);
    
    // Receive batch
    Message received[5];
    int recv_count = mailbox_receive_batch(&mbox, received, 5);
    ASSERT_EQ(recv_count, 5);
    ASSERT_EQ(mbox.count, 0);
    
    // Verify and clean up
    int zerocopy_count = 0;
    for (int i = 0; i < recv_count; i++) {
        if (received[i].zerocopy.owned && received[i].zerocopy.data) {
            zerocopy_count++;
            message_free(&received[i]);
        }
    }
    ASSERT_EQ(zerocopy_count, 2);
}

void test_zerocopy_message_free(void) {
    // Test freeing null/unowned message (should be safe)
    Message msg1 = message_create_simple(MSG_INCREMENT, 1, 42);
    message_free(&msg1);  // Should be no-op
    
    // Test freeing owned zero-copy message
    void* data = malloc(512);
    Message msg2 = message_create_zerocopy(MSG_USER_START, 1, data, 512);
    ASSERT_EQ(msg2.zerocopy.owned, 1);
    message_free(&msg2);
    ASSERT_EQ(msg2.zerocopy.owned, 0);
    ASSERT_TRUE(msg2.zerocopy.data == NULL);
}

void register_zerocopy_tests(void) {
    register_test_with_category("Zero-copy message creation", test_zerocopy_message_creation, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero-copy message transfer", test_zerocopy_message_transfer, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero-copy mailbox operations", test_zerocopy_mailbox_operations, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero-copy threshold behavior", test_zerocopy_threshold, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero-copy batch operations", test_zerocopy_batch_operations, TEST_CATEGORY_RUNTIME);
    register_test_with_category("Zero-copy message cleanup", test_zerocopy_message_free, TEST_CATEGORY_RUNTIME);
}
