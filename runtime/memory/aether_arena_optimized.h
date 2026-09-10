#ifndef AETHER_ARENA_OPTIMIZED_H
#define AETHER_ARENA_OPTIMIZED_H

#include <stddef.h>
#include <stdint.h>
#include "../utils/aether_thread.h"

// Size classes for better memory allocation
#define SIZE_CLASS_SMALL 128      // < 128 bytes
#define SIZE_CLASS_MEDIUM 4096    // 128 bytes to 4KB
// > 4KB goes to large allocations

// Arena block for size class
typedef struct ArenaBlock {
    char* memory;
    size_t size;
    size_t used;
    struct ArenaBlock* next;
} ArenaBlock;

// Size class arena (separate arena for each size class)
typedef struct {
    ArenaBlock* blocks;
    size_t block_size;
    pthread_mutex_t lock;  // Only for shared arenas
} SizeClassArena;

// Thread-local arena set (one per thread)
typedef struct {
    SizeClassArena small;     // For allocations < 128 bytes
    SizeClassArena medium;    // For allocations 128B - 4KB
    SizeClassArena large;     // For allocations > 4KB
    uint64_t allocated_bytes;
    uint64_t allocation_count;
} ThreadLocalArena;

// Global arena manager
typedef struct {
    ThreadLocalArena* thread_arenas;
    int max_threads;
    pthread_key_t thread_key;
} ArenaManager;

// Initialization
void arena_manager_init(int max_threads);
void arena_manager_shutdown(void);

// Fast path allocation (thread-local, no locks)
void* arena_alloc_fast(size_t bytes);
void* arena_alloc_fast_aligned(size_t bytes, size_t alignment);

// Get current thread's arena statistics
void arena_get_thread_stats(uint64_t* allocated_bytes, uint64_t* allocation_count);

// Reset thread-local arena (for per-request allocations)
void arena_reset_thread(void);

// Get total memory statistics
typedef struct {
    uint64_t total_allocated;
    uint64_t small_allocations;
    uint64_t medium_allocations;
    uint64_t large_allocations;
} ArenaStats;

void arena_get_global_stats(ArenaStats* stats);

#endif // AETHER_ARENA_OPTIMIZED_H

