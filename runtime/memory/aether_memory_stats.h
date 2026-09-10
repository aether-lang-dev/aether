#ifndef AETHER_MEMORY_STATS_H
#define AETHER_MEMORY_STATS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t current_allocations;
    uint64_t peak_allocations;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t allocation_failures;
} MemoryStats;

void memory_stats_init(void);
void memory_stats_record_alloc(size_t bytes);
void memory_stats_record_free(size_t bytes);
void memory_stats_record_failure(void);
MemoryStats memory_stats_get(void);
void memory_stats_print(void);
void memory_stats_reset(void);

#ifdef AETHER_MEMORY_TRACKING
    #define TRACK_ALLOC(bytes) memory_stats_record_alloc(bytes)
    #define TRACK_FREE(bytes) memory_stats_record_free(bytes)
    #define TRACK_FAILURE() memory_stats_record_failure()
#else
    #define TRACK_ALLOC(bytes)
    #define TRACK_FREE(bytes)
    #define TRACK_FAILURE()
#endif

#endif

