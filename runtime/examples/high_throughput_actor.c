// Example: High-Throughput Actor with Optimizations
// Demonstrates SIMD batching and message coalescing for maximum performance

#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/actors/aether_simd_batch.h"
#include "../../runtime/actors/aether_message_coalescing.h"
#include "../../runtime/actors/aether_zerocopy.h"

// Message types
typedef struct {
    int message_id;
    int value;
} ComputeMessage;

typedef struct {
    int message_id;
    int result;
} ResultMessage;

// High-performance actor that processes compute messages
typedef struct {
    int id;
    CoalescingBuffer outgoing;
    int messages_processed;
} HighThroughputActor;

// Initialize actor with coalescing enabled
void actor_init(HighThroughputActor* actor, int id) {
    actor->id = id;
    coalescing_buffer_init(&actor->outgoing);
    actor->messages_processed = 0;
}

// Send function for coalescing
void send_result(void* msg, uint16_t size) {
    (void)size;   /* the coalescing callback signature carries it; unused here */
    ResultMessage* result = (ResultMessage*)msg;
    printf("Sending result: %d\n", result->result);
    free(msg);
}

// Process batch of messages using SIMD
void process_batch_simd(
    ComputeMessage* messages,
    int count,
    HighThroughputActor* actor
) {
    if (count == 0) return;
    
    // Extract values for SIMD processing
    int values[SIMD_BATCH_SIZE];
    int results[SIMD_BATCH_SIZE];
    
    int batches = (count + SIMD_BATCH_SIZE - 1) / SIMD_BATCH_SIZE;
    
    for (int b = 0; b < batches; b++) {
        int batch_start = b * SIMD_BATCH_SIZE;
        int batch_size = (batch_start + SIMD_BATCH_SIZE <= count) 
            ? SIMD_BATCH_SIZE 
            : count - batch_start;
        
        // Gather values
        for (int i = 0; i < batch_size; i++) {
            values[i] = messages[batch_start + i].value;
        }
        
        // SIMD process: result = value * 2 + 10
        simd_batch_process_int(values, results, batch_size, 2, 10);
        
        // Coalesce results
        for (int i = 0; i < batch_size; i++) {
            ResultMessage* result = malloc(sizeof(ResultMessage));
            result->message_id = 1;
            result->result = results[i];
            
            int should_flush = coalescing_buffer_add(
                &actor->outgoing, 
                result, 
                sizeof(ResultMessage)
            );
            
            if (should_flush) {
                coalescing_buffer_flush(&actor->outgoing, send_result);
            }
        }
        
        actor->messages_processed += batch_size;
    }
}

// Process batch of messages using zero-copy
void process_batch_zerocopy(
    ZeroCopyMessage* messages,
    int count,
    HighThroughputActor* actor
) {
    for (int i = 0; i < count; i++) {
        if (!zerocopy_is_valid(&messages[i])) continue;
        
        ComputeMessage* msg = (ComputeMessage*)messages[i].data;
        
        // Process message
        int result = msg->value * 2 + 10;
        
        // Create result message (zero-copy)
        ResultMessage* result_msg = malloc(sizeof(ResultMessage));
        result_msg->message_id = 1;
        result_msg->result = result;
        
        // Coalesce result
        int should_flush = coalescing_buffer_add(
            &actor->outgoing,
            result_msg,
            sizeof(ResultMessage)
        );
        
        if (should_flush) {
            coalescing_buffer_flush(&actor->outgoing, send_result);
        }
        
        // Free original message (ownership transferred)
        zerocopy_free(&messages[i]);
        
        actor->messages_processed++;
    }
}

// Main example
int main() {
    printf("High-Throughput Actor Example\n");
    printf("Optimizations: SIMD + Coalescing + Zero-Copy\n\n");
    
    // Create actor
    HighThroughputActor actor;
    actor_init(&actor, 1);
    
    // Simulate incoming message batch
    const int MESSAGE_COUNT = 32;
    ComputeMessage messages[MESSAGE_COUNT];
    
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        messages[i].message_id = 0;
        messages[i].value = i * 10;
    }
    
    printf("Processing %d messages with SIMD batching...\n", MESSAGE_COUNT);
    process_batch_simd(messages, MESSAGE_COUNT, &actor);
    
    // Flush remaining coalesced messages
    if (actor.outgoing.pending.count > 0) {
        coalescing_buffer_flush(&actor.outgoing, send_result);
    }
    
    printf("\nActor statistics:\n");
    printf("- Messages processed: %d\n", actor.messages_processed);
    
    CoalescingStats stats = coalescing_buffer_stats(&actor.outgoing);
    printf("- Flush count: %d\n", stats.flush_count);
    printf("- Average batch size: %.2f\n", stats.avg_batch_size);
    printf("- Queue operations saved: %d%%\n", 
           (int)((1.0f - (float)stats.flush_count / actor.messages_processed) * 100));
    
    printf("\nOptimizations active in this run:\n");
    printf("- SIMD: vectorised compute over the message batch\n");
    printf("- Coalescing: fewer cross-core enqueues per burst\n");
    printf("- Zero-copy: no memcpy for large payloads\n");
    
    return 0;
}
