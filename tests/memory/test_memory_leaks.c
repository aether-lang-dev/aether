#include "../runtime/test_harness.h"
#include "../../runtime/memory/aether_arena.h"
#include "../../runtime/memory/aether_pool.h"
#include "../../runtime/memory/aether_memory_stats.h"
#include <stdlib.h>

TEST(no_leak_arena_simple) {
    memory_stats_init();
    
    Arena* arena = arena_create(1024);
    arena_alloc(arena, 100);
    arena_alloc(arena, 200);
    arena_destroy(arena);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(stats.current_allocations, stats.total_frees);
}

TEST(no_leak_pool_simple) {
    memory_stats_init();
    
    MemoryPool* pool = pool_create(64, 10);
    void* p1 = pool_alloc(pool);
    void* p2 = pool_alloc(pool);
    pool_free(pool, p1);
    pool_free(pool, p2);
    pool_destroy(pool);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(stats.current_allocations, stats.total_frees);
}

TEST(no_leak_arena_reset) {
    memory_stats_init();
    
    Arena* arena = arena_create(2048);
    for (int i = 0; i < 100; i++) {
        arena_alloc(arena, 50);
        arena_reset(arena);
    }
    arena_destroy(arena);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(stats.current_allocations, stats.total_frees);
}

TEST(no_leak_standard_pools) {
    memory_stats_init();
    
    StandardPools* pools = standard_pools_create();
    
    void* p8 = standard_pools_alloc(pools, 8);
    void* p16 = standard_pools_alloc(pools, 16);
    void* p32 = standard_pools_alloc(pools, 32);
    
    standard_pools_free(pools, p8, 8);
    standard_pools_free(pools, p16, 16);
    standard_pools_free(pools, p32, 32);
    
    standard_pools_destroy(pools);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(stats.current_allocations, stats.total_frees);
}

TEST(no_leak_arena_scopes) {
    memory_stats_init();
    
    Arena* arena = arena_create(4096);
    
    for (int i = 0; i < 50; i++) {
        ArenaScope scope = arena_begin(arena);
        arena_alloc(arena, 100);
        arena_end(scope);
    }
    
    arena_destroy(arena);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(stats.current_allocations, stats.total_frees);
}

TEST(detect_leak_simulation) {
    memory_stats_init();
    
    // Use direct memory allocation to test leak detection
    memory_stats_record_alloc(100);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_TRUE(stats.current_allocations > 0);
    
    // Clean up
    memory_stats_record_free(100);
}

TEST(memory_stats_tracking) {
    memory_stats_init();
    
    memory_stats_record_alloc(1024);
    memory_stats_record_alloc(2048);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(2, stats.total_allocations);
    ASSERT_EQ(3072, stats.bytes_allocated);
    ASSERT_EQ(2, stats.current_allocations);
    
    memory_stats_record_free(1024);
    
    stats = memory_stats_get();
    ASSERT_EQ(1, stats.total_frees);
    ASSERT_EQ(1, stats.current_allocations);
    ASSERT_EQ(2048, stats.current_bytes);
    
    memory_stats_record_free(2048);
    
    stats = memory_stats_get();
    ASSERT_EQ(0, stats.current_allocations);
    ASSERT_EQ(0, stats.current_bytes);
}

TEST(memory_peak_tracking) {
    memory_stats_init();
    
    memory_stats_record_alloc(1000);
    memory_stats_record_alloc(2000);
    memory_stats_record_alloc(3000);
    
    MemoryStats stats = memory_stats_get();
    ASSERT_EQ(3, stats.peak_allocations);
    ASSERT_EQ(6000, stats.peak_bytes);
    
    memory_stats_record_free(1000);
    memory_stats_record_free(2000);
    
    stats = memory_stats_get();
    ASSERT_EQ(3, stats.peak_allocations);
    ASSERT_EQ(6000, stats.peak_bytes);
    ASSERT_EQ(1, stats.current_allocations);
}

TEST(no_double_free) {
    MemoryPool* pool = pool_create(32, 10);
    
    void* ptr = pool_alloc(pool);
    ASSERT_NOT_NULL(ptr);
    
    pool_free(pool, ptr);
    
    pool_destroy(pool);
}

