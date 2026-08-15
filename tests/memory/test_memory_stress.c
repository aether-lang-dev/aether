#include "../runtime/test_harness.h"
#include "../../runtime/memory/aether_arena.h"
#include "../../runtime/memory/aether_pool.h"
#include <stdlib.h>
#include <string.h>

TEST(arena_stress_allocations) {
    Arena* arena = arena_create(4096);
    
    for (int i = 0; i < 10000; i++) {
        void* ptr = arena_alloc(arena, 32);
        ASSERT_NOT_NULL(ptr);
        memset(ptr, i % 256, 32);
    }
    
    size_t used = arena_get_used(arena);
    ASSERT_TRUE(used >= 320000);
    
    arena_destroy(arena);
}

TEST(arena_stress_reset_reuse) {
    Arena* arena = arena_create(8192);
    
    for (int iteration = 0; iteration < 1000; iteration++) {
        for (int i = 0; i < 100; i++) {
            void* ptr = arena_alloc(arena, 64);
            ASSERT_NOT_NULL(ptr);
            memset(ptr, 0xFF, 64);
        }
        arena_reset(arena);
    }
    
    ASSERT_EQ(0, arena_get_used(arena));
    arena_destroy(arena);
}

TEST(pool_stress_allocations) {
    MemoryPool* pool = pool_create(64, 1000);
    
    void* ptrs[1000];
    for (int i = 0; i < 1000; i++) {
        ptrs[i] = pool_alloc(pool);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], i % 256, 64);
    }
    
    ASSERT_EQ(1000, pool_get_used(pool));
    
    for (int i = 0; i < 1000; i++) {
        pool_free(pool, ptrs[i]);
    }
    
    ASSERT_EQ(0, pool_get_used(pool));
    pool_destroy(pool);
}

TEST(pool_stress_alloc_free_cycles) {
    MemoryPool* pool = pool_create(32, 100);
    
    for (int cycle = 0; cycle < 1000; cycle++) {
        void* ptrs[100];
        for (int i = 0; i < 100; i++) {
            ptrs[i] = pool_alloc(pool);
            ASSERT_NOT_NULL(ptrs[i]);
        }
        
        for (int i = 0; i < 100; i++) {
            pool_free(pool, ptrs[i]);
        }
    }
    
    ASSERT_EQ(0, pool_get_used(pool));
    pool_destroy(pool);
}

TEST(standard_pools_stress) {
    StandardPools* pools = standard_pools_create();
    
    void* ptrs[1000];
    size_t sizes[1000];
    
    for (int i = 0; i < 1000; i++) {
        sizes[i] = (i % 6) == 0 ? 8 :
                   (i % 6) == 1 ? 16 :
                   (i % 6) == 2 ? 32 :
                   (i % 6) == 3 ? 64 :
                   (i % 6) == 4 ? 128 : 256;
        ptrs[i] = standard_pools_alloc(pools, sizes[i]);
        if (ptrs[i]) {
            memset(ptrs[i], 0xAA, sizes[i]);
        }
    }
    
    for (int i = 0; i < 1000; i++) {
        if (ptrs[i]) {
            standard_pools_free(pools, ptrs[i], sizes[i]);
        }
    }
    
    standard_pools_destroy(pools);
}

TEST(arena_scopes_stress) {
    Arena* arena = arena_create(16384);
    
    for (int i = 0; i < 100; i++) {
        ArenaScope outer = arena_begin(arena);
        arena_alloc(arena, 1024);
        
        for (int j = 0; j < 10; j++) {
            ArenaScope inner = arena_begin(arena);
            arena_alloc(arena, 512);
            arena_end(inner);
        }
        
        arena_end(outer);
    }
    
    ASSERT_EQ(0, arena_get_used(arena));
    arena_destroy(arena);
}

TEST(arena_large_allocations) {
    Arena* arena = arena_create(1024);
    
    void* p1 = arena_alloc(arena, 1024 * 1024);
    ASSERT_NOT_NULL(p1);
    memset(p1, 0x55, 1024 * 1024);
    
    void* p2 = arena_alloc(arena, 512 * 1024);
    ASSERT_NOT_NULL(p2);
    memset(p2, 0xAA, 512 * 1024);
    
    arena_destroy(arena);
}

TEST(memory_no_leaks_arena) {
    for (int i = 0; i < 100; i++) {
        Arena* arena = arena_create(4096);
        for (int j = 0; j < 100; j++) {
            arena_alloc(arena, 32);
        }
        arena_destroy(arena);
    }
}

TEST(memory_no_leaks_pool) {
    for (int i = 0; i < 100; i++) {
        MemoryPool* pool = pool_create(64, 50);
        void* ptrs[50];
        for (int j = 0; j < 50; j++) {
            ptrs[j] = pool_alloc(pool);
        }
        for (int j = 0; j < 50; j++) {
            pool_free(pool, ptrs[j]);
        }
        pool_destroy(pool);
    }
}

TEST(arena_alignment_stress) {
    Arena* arena = arena_create(8192);
    
    for (int i = 0; i < 1000; i++) {
        void* ptr = arena_alloc(arena, i % 128 + 1);
        ASSERT_NOT_NULL(ptr);
        
        size_t addr = (size_t)ptr;
        ASSERT_EQ(0, addr % 8);
    }
    
    arena_destroy(arena);
}

