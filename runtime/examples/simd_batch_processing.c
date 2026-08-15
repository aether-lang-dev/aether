// Example: SIMD Batch Message Processing
// Demonstrates AVX2-vectorised batch processing over a message buffer,
// with an automatic scalar fallback on CPUs without AVX2.

#include <stdio.h>
#include <stdlib.h>
#include "../utils/aether_simd_vectorized.h"

// Message structure
typedef struct {
    int32_t type;
    int32_t sender;
    int32_t payload;
} SimpleMessage;

int main() {
    printf("===========================================\n");
    printf("  SIMD Batch Processing Example\n");
    printf("===========================================\n\n");
    
    // Initialize SIMD (detects AVX2)
    aether_simd_init();
    printf("\n");
    
    // Create 1024 test messages
    const int count = 1024;
    SimpleMessage* messages = malloc(count * sizeof(SimpleMessage));
    void** msg_ptrs = malloc(count * sizeof(void*));
    int32_t* msg_ids = malloc(count * sizeof(int32_t));
    
    // Initialize messages (10 different types)
    for (int i = 0; i < count; i++) {
        messages[i].type = i % 10;
        messages[i].sender = i;
        messages[i].payload = i * 2;
        msg_ptrs[i] = &messages[i];
    }
    
    printf("Processing %d messages...\n\n", count);
    
    // Extract message IDs using SIMD (8 at once)
    extract_message_ids_avx2((const void**)msg_ptrs, msg_ids, count);
    
    printf("Extracted message IDs:\n");
    printf("First 16: ");
    for (int i = 0; i < 16; i++) {
        printf("%d ", msg_ids[i]);
    }
    printf("\n\n");
    
    // Filter messages by type using SIMD
    int* indices = malloc(count * sizeof(int));
    int32_t target_type = 5;
    int matched = filter_messages_by_type_avx2(msg_ids, count, target_type, indices);
    
    printf("Messages of type %d: %d matches\n", target_type, matched);
    printf("Indices: ");
    for (int i = 0; i < (matched < 10 ? matched : 10); i++) {
        printf("%d ", indices[i]);
    }
    if (matched > 10) printf("...");
    printf("\n\n");
    
    // Batch counter increment using SIMD
    int32_t* counters = calloc(count, sizeof(int32_t));
    int32_t* increments = malloc(count * sizeof(int32_t));
    for (int i = 0; i < count; i++) {
        increments[i] = 1;
    }
    
    increment_counters_avx2(counters, increments, count);
    
    printf("After SIMD increment:\n");
    printf("Counters[0-15]: ");
    for (int i = 0; i < 16; i++) {
        printf("%d ", counters[i]);
    }
    printf("\n\n");
    
    printf("===========================================\n");
    printf("Key Takeaways:\n");
    printf("- SIMD processes 8 values simultaneously (AVX2 256-bit lanes)\n");
    printf("- Scalar fallback runs automatically when AVX2 is unavailable\n");
    printf("- Well-suited to compute-heavy batch operations over uniform data\n");
    printf("- Run your own workload through this example to measure locally\n");
    printf("===========================================\n");
    
    free(messages);
    free(msg_ptrs);
    free(msg_ids);
    free(indices);
    free(counters);
    free(increments);
    
    return 0;
}
