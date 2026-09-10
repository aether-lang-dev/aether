#include "aether_arena_optimized.h"
#include "../utils/aether_compiler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ALIGN_UP(n, align) (((n) + (align) - 1) & ~((align) - 1))
#define DEFAULT_BLOCK_SIZE_SMALL (64 * 1024)     // 64KB blocks for small
#define DEFAULT_BLOCK_SIZE_MEDIUM (512 * 1024)   // 512KB blocks for medium
#define DEFAULT_BLOCK_SIZE_LARGE (2 * 1024 * 1024) // 2MB blocks for large

static ArenaManager global_manager = {0};
static AETHER_TLS ThreadLocalArena* tl_arena = NULL;

// Create a new arena block
static ArenaBlock* arena_block_create(size_t size) {
    ArenaBlock* block = (ArenaBlock*)malloc(sizeof(ArenaBlock));
    if (!block) return NULL;
    
    block->memory = (char*)malloc(size);
    if (!block->memory) {
        free(block);
        return NULL;
    }
    
    block->size = size;
    block->used = 0;
    block->next = NULL;
    
    return block;
}

// Free arena blocks
static void arena_blocks_free(ArenaBlock* block) {
    while (block) {
        ArenaBlock* next = block->next;
        free(block->memory);
        free(block);
        block = next;
    }
}

// Initialize size class arena
static void size_class_arena_init(SizeClassArena* arena, size_t block_size) {
    arena->blocks = arena_block_create(block_size);
    arena->block_size = block_size;
    pthread_mutex_init(&arena->lock, NULL);
}

// Destroy size class arena
static void size_class_arena_destroy(SizeClassArena* arena) {
    arena_blocks_free(arena->blocks);
    pthread_mutex_destroy(&arena->lock);
}

// Fast bump allocation from size class arena
static void* size_class_alloc(SizeClassArena* arena, size_t bytes, size_t alignment) {
    if (!arena->blocks) {
        arena->blocks = arena_block_create(arena->block_size);
        if (!arena->blocks) return NULL;
    }
    
    ArenaBlock* block = arena->blocks;
    size_t aligned_used = ALIGN_UP(block->used, alignment);
    
    // Check if fits in current block
    if (aligned_used + bytes <= block->size) {
        void* ptr = block->memory + aligned_used;
        block->used = aligned_used + bytes;
        return ptr;
    }
    
    // Need new block
    size_t new_block_size = arena->block_size;
    if (bytes > new_block_size) {
        new_block_size = ALIGN_UP(bytes, arena->block_size);
    }
    
    ArenaBlock* new_block = arena_block_create(new_block_size);
    if (!new_block) return NULL;
    
    new_block->next = arena->blocks;
    arena->blocks = new_block;
    
    void* ptr = new_block->memory;
    new_block->used = bytes;
    
    return ptr;
}

// Thread-local arena destructor
static void thread_arena_destructor(void* arg) {
    ThreadLocalArena* arena = (ThreadLocalArena*)arg;
    if (!arena) return;
    
    size_class_arena_destroy(&arena->small);
    size_class_arena_destroy(&arena->medium);
    size_class_arena_destroy(&arena->large);
    free(arena);
}

// Get or create thread-local arena
static ThreadLocalArena* get_thread_arena(void) {
    if (tl_arena) return tl_arena;
    
    tl_arena = (ThreadLocalArena*)calloc(1, sizeof(ThreadLocalArena));
    if (!tl_arena) return NULL;
    
    size_class_arena_init(&tl_arena->small, DEFAULT_BLOCK_SIZE_SMALL);
    size_class_arena_init(&tl_arena->medium, DEFAULT_BLOCK_SIZE_MEDIUM);
    size_class_arena_init(&tl_arena->large, DEFAULT_BLOCK_SIZE_LARGE);
    
    tl_arena->allocated_bytes = 0;
    tl_arena->allocation_count = 0;
    
    // Set thread-specific data for cleanup
    pthread_setspecific(global_manager.thread_key, tl_arena);
    
    return tl_arena;
}

// Initialize arena manager
void arena_manager_init(int max_threads) {
    if (global_manager.thread_arenas) {
        return;  // Already initialized
    }
    
    global_manager.max_threads = max_threads;
    global_manager.thread_arenas = (ThreadLocalArena*)calloc(max_threads, sizeof(ThreadLocalArena));
    
    // Create thread-local key for cleanup
    pthread_key_create(&global_manager.thread_key, thread_arena_destructor);
}

void arena_manager_shutdown(void) {
    if (!global_manager.thread_arenas) return;
    
    pthread_key_delete(global_manager.thread_key);
    free(global_manager.thread_arenas);
    global_manager.thread_arenas = NULL;
}

// Fast path allocation with automatic size class selection
void* arena_alloc_fast(size_t bytes) {
    return arena_alloc_fast_aligned(bytes, 8);
}

void* arena_alloc_fast_aligned(size_t bytes, size_t alignment) {
    if (bytes == 0) return NULL;
    
    ThreadLocalArena* arena = get_thread_arena();
    if (!arena) {
        // Fallback to malloc if arena initialization fails
        return malloc(bytes);
    }
    
    void* ptr;
    
    // Select size class
    if (bytes <= SIZE_CLASS_SMALL) {
        ptr = size_class_alloc(&arena->small, bytes, alignment);
    } else if (bytes <= SIZE_CLASS_MEDIUM) {
        ptr = size_class_alloc(&arena->medium, bytes, alignment);
    } else {
        ptr = size_class_alloc(&arena->large, bytes, alignment);
    }
    
    if (ptr) {
        arena->allocated_bytes += bytes;
        arena->allocation_count++;
    }
    
    return ptr;
}

void arena_get_thread_stats(uint64_t* allocated_bytes, uint64_t* allocation_count) {
    ThreadLocalArena* arena = tl_arena;
    
    if (arena) {
        if (allocated_bytes) *allocated_bytes = arena->allocated_bytes;
        if (allocation_count) *allocation_count = arena->allocation_count;
    } else {
        if (allocated_bytes) *allocated_bytes = 0;
        if (allocation_count) *allocation_count = 0;
    }
}

void arena_reset_thread(void) {
    ThreadLocalArena* arena = tl_arena;
    if (!arena) return;
    
    // Reset each size class by resetting block usage
    if (arena->small.blocks) {
        arena->small.blocks->used = 0;
        // Free extra blocks
        if (arena->small.blocks->next) {
            arena_blocks_free(arena->small.blocks->next);
            arena->small.blocks->next = NULL;
        }
    }
    
    if (arena->medium.blocks) {
        arena->medium.blocks->used = 0;
        if (arena->medium.blocks->next) {
            arena_blocks_free(arena->medium.blocks->next);
            arena->medium.blocks->next = NULL;
        }
    }
    
    if (arena->large.blocks) {
        arena->large.blocks->used = 0;
        if (arena->large.blocks->next) {
            arena_blocks_free(arena->large.blocks->next);
            arena->large.blocks->next = NULL;
        }
    }
    
    arena->allocated_bytes = 0;
    arena->allocation_count = 0;
}

void arena_get_global_stats(ArenaStats* stats) {
    if (!stats) return;
    
    memset(stats, 0, sizeof(ArenaStats));
    
    // This would need to iterate all thread arenas in a real implementation
    // For now, just return current thread stats
    ThreadLocalArena* arena = tl_arena;
    if (arena) {
        stats->total_allocated = arena->allocated_bytes;
        
        // Count allocations by size class (approximate)
        if (arena->small.blocks) {
            ArenaBlock* b = arena->small.blocks;
            while (b) {
                stats->small_allocations += b->used;
                b = b->next;
            }
        }
        
        if (arena->medium.blocks) {
            ArenaBlock* b = arena->medium.blocks;
            while (b) {
                stats->medium_allocations += b->used;
                b = b->next;
            }
        }
        
        if (arena->large.blocks) {
            ArenaBlock* b = arena->large.blocks;
            while (b) {
                stats->large_allocations += b->used;
                b = b->next;
            }
        }
    }
}

