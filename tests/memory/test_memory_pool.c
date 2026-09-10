#include "../runtime/test_harness.h"
#include <stdint.h>
#include "../../runtime/memory/aether_pool.h"
#include <string.h>

TEST_CATEGORY(pool_create_destroy, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(32, 10);
    ASSERT_NOT_NULL(pool);
    ASSERT_EQ(10, pool_get_capacity(pool));
    ASSERT_EQ(0, pool_get_used(pool));
    pool_destroy(pool);
}

TEST_CATEGORY(pool_alloc_single, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(32, 10);
    
    void* ptr = pool_alloc(pool);
    ASSERT_NOT_NULL(ptr);
    ASSERT_EQ(1, pool_get_used(pool));
    
    pool_destroy(pool);
}

TEST_CATEGORY(pool_alloc_free, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(32, 10);
    
    void* ptr = pool_alloc(pool);
    ASSERT_NOT_NULL(ptr);
    ASSERT_EQ(1, pool_get_used(pool));
    
    pool_free(pool, ptr);
    ASSERT_EQ(0, pool_get_used(pool));
    
    pool_destroy(pool);
}

TEST_CATEGORY(pool_reuse_freed, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(32, 10);
    
    void* ptr1 = pool_alloc(pool);
    pool_free(pool, ptr1);
    
    void* ptr2 = pool_alloc(pool);
    ASSERT_EQ(ptr1, ptr2);
    
    pool_destroy(pool);
}

TEST_CATEGORY(pool_exhaust, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(16, 3);
    
    void* p1 = pool_alloc(pool);
    void* p2 = pool_alloc(pool);
    void* p3 = pool_alloc(pool);
    void* p4 = pool_alloc(pool);
    
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);
    ASSERT_NULL(p4);
    
    ASSERT_EQ(3, pool_get_used(pool));
    
    pool_destroy(pool);
}

TEST_CATEGORY(pool_multiple_alloc_free, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(64, 5);
    
    void* ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = pool_alloc(pool);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    
    ASSERT_EQ(5, pool_get_used(pool));
    
    for (int i = 0; i < 5; i++) {
        pool_free(pool, ptrs[i]);
    }
    
    ASSERT_EQ(0, pool_get_used(pool));
    
    pool_destroy(pool);
}

TEST_CATEGORY(standard_pools_create, TEST_CATEGORY_MEMORY) {
    StandardPools* pools = standard_pools_create();
    ASSERT_NOT_NULL(pools);
    standard_pools_destroy(pools);
}

TEST_CATEGORY(standard_pools_alloc_8, TEST_CATEGORY_MEMORY) {
    StandardPools* pools = standard_pools_create();
    
    void* ptr = standard_pools_alloc(pools, 8);
    ASSERT_NOT_NULL(ptr);
    
    standard_pools_free(pools, ptr, 8);
    standard_pools_destroy(pools);
}

TEST_CATEGORY(standard_pools_alloc_various_sizes, TEST_CATEGORY_MEMORY) {
    StandardPools* pools = standard_pools_create();
    
    void* p8 = standard_pools_alloc(pools, 8);
    void* p16 = standard_pools_alloc(pools, 16);
    void* p32 = standard_pools_alloc(pools, 32);
    void* p64 = standard_pools_alloc(pools, 64);
    void* p128 = standard_pools_alloc(pools, 128);
    void* p256 = standard_pools_alloc(pools, 256);
    
    ASSERT_NOT_NULL(p8);
    ASSERT_NOT_NULL(p16);
    ASSERT_NOT_NULL(p32);
    ASSERT_NOT_NULL(p64);
    ASSERT_NOT_NULL(p128);
    ASSERT_NOT_NULL(p256);
    
    standard_pools_free(pools, p8, 8);
    standard_pools_free(pools, p16, 16);
    standard_pools_free(pools, p32, 32);
    standard_pools_free(pools, p64, 64);
    standard_pools_free(pools, p128, 128);
    standard_pools_free(pools, p256, 256);
    
    standard_pools_destroy(pools);
}

TEST_CATEGORY(standard_pools_size_rounding, TEST_CATEGORY_MEMORY) {
    StandardPools* pools = standard_pools_create();
    
    void* p5 = standard_pools_alloc(pools, 5);
    void* p12 = standard_pools_alloc(pools, 12);
    void* p25 = standard_pools_alloc(pools, 25);
    
    ASSERT_NOT_NULL(p5);
    ASSERT_NOT_NULL(p12);
    ASSERT_NOT_NULL(p25);
    
    standard_pools_free(pools, p5, 5);
    standard_pools_free(pools, p12, 12);
    standard_pools_free(pools, p25, 25);
    
    standard_pools_destroy(pools);
}

TEST_CATEGORY(standard_pools_too_large, TEST_CATEGORY_MEMORY) {
    StandardPools* pools = standard_pools_create();
    
    void* p512 = standard_pools_alloc(pools, 512);
    ASSERT_NULL(p512);
    
    standard_pools_destroy(pools);
}

TEST_CATEGORY(pool_data_integrity, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(sizeof(int), 10);
    
    int* nums[5];
    for (int i = 0; i < 5; i++) {
        nums[i] = (int*)pool_alloc(pool);
        *nums[i] = i * 10;
    }
    
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(i * 10, *nums[i]);
    }
    
    pool_destroy(pool);
}

/* Every slot is cast to FreeNode* while it is on the free list, so the stride
 * between slots has to keep them pointer-aligned. An object size that clears
 * the minimum but is not a multiple of that alignment used to put every odd
 * slot on a misaligned address: undefined behaviour that x86 and ARM64 absorb
 * and a strict target does not. 12 is the smallest such size. */
TEST_CATEGORY(pool_alignment_odd_object_size, TEST_CATEGORY_MEMORY) {
    MemoryPool* pool = pool_create(12, 8);
    ASSERT_TRUE(pool != NULL);

    void* slots[8];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        slots[i] = pool_alloc(pool);
        if (slots[i]) n++;
    }
    ASSERT_TRUE(n > 1);

    for (int i = 0; i < n; i++) {
        uintptr_t addr = (uintptr_t)slots[i];
        ASSERT_EQ(0, (int)(addr % sizeof(void*)));
    }

    pool_destroy(pool);
}

// Note: Tests are auto-registered via TEST_CATEGORY macro
void register_memory_pool_tests() {
    // Empty - tests registered by constructor
}
