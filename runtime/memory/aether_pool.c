#include "aether_pool.h"
#include "../aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

typedef struct FreeNode {
    struct FreeNode* next;
} FreeNode;

struct MemoryPool {
    void* memory;
    size_t object_size;
    int capacity;
    int used;
    FreeNode* free_list;
};

MemoryPool* pool_create(size_t object_size, int initial_count) {
    if (object_size < sizeof(FreeNode)) {
        object_size = sizeof(FreeNode);
    }
    /* CRITICAL: every slot is cast to FreeNode* while it sits on the free
     * list, so the stride between slots has to keep them pointer-aligned. A
     * size that clears the minimum but is not a multiple of the alignment (12,
     * say) put every odd slot on a misaligned address: undefined behaviour
     * that x86 and ARM64 absorb silently and a strict target does not.
     * Confirmed with -fsanitize=alignment before this rounding was added. */
    {
        size_t align = sizeof(void*);
        size_t rem = object_size % align;
        if (rem != 0) object_size += align - rem;
    }

    /* #343: cap-aware. Both the MemoryPool struct and the backing
     * memory block are accounted; pool_destroy frees both with
     * their known sizes. */
    MemoryPool* pool = (MemoryPool*)aether_caps_malloc(sizeof(MemoryPool));
    if (!pool) return NULL;

    pool->memory = aether_caps_malloc(object_size * (size_t)initial_count);
    if (!pool->memory) {
        aether_caps_free(pool, sizeof(MemoryPool));
        return NULL;
    }
    
    pool->object_size = object_size;
    pool->capacity = initial_count;
    pool->used = 0;
    pool->free_list = NULL;
    
    for (int i = 0; i < initial_count; i++) {
        FreeNode* node = (FreeNode*)((char*)pool->memory + i * object_size);
        node->next = pool->free_list;
        pool->free_list = node;
    }
    
    return pool;
}

void* pool_alloc(MemoryPool* pool) {
    if (!pool) return NULL;
    
    if (pool->free_list) {
        FreeNode* node = pool->free_list;
        pool->free_list = node->next;
        pool->used++;
        return (void*)node;
    }
    
    return NULL;
}

void pool_free(MemoryPool* pool, void* ptr) {
    if (!pool || !ptr) return;
    
    FreeNode* node = (FreeNode*)ptr;
    node->next = pool->free_list;
    pool->free_list = node;
    pool->used--;
}

int pool_get_capacity(MemoryPool* pool) {
    return pool ? pool->capacity : 0;
}

int pool_get_used(MemoryPool* pool) {
    return pool ? pool->used : 0;
}

void pool_destroy(MemoryPool* pool) {
    if (!pool) return;
    aether_caps_free(pool->memory, pool->object_size * (size_t)pool->capacity);
    aether_caps_free(pool, sizeof(MemoryPool));
}

StandardPools* standard_pools_create(void) {
    StandardPools* pools = (StandardPools*)aether_caps_malloc(sizeof(StandardPools));
    if (!pools) return NULL;
    
    pools->pool_8 = pool_create(8, 256);
    pools->pool_16 = pool_create(16, 128);
    pools->pool_32 = pool_create(32, 64);
    pools->pool_64 = pool_create(64, 32);
    pools->pool_128 = pool_create(128, 16);
    pools->pool_256 = pool_create(256, 8);
    
    if (!pools->pool_8 || !pools->pool_16 || !pools->pool_32 ||
        !pools->pool_64 || !pools->pool_128 || !pools->pool_256) {
        standard_pools_destroy(pools);
        return NULL;
    }
    
    return pools;
}

void* standard_pools_alloc(StandardPools* pools, size_t size) {
    if (!pools) return NULL;
    
    MemoryPool* pool = NULL;
    
    if (size <= 8) pool = pools->pool_8;
    else if (size <= 16) pool = pools->pool_16;
    else if (size <= 32) pool = pools->pool_32;
    else if (size <= 64) pool = pools->pool_64;
    else if (size <= 128) pool = pools->pool_128;
    else if (size <= 256) pool = pools->pool_256;
    else return NULL;
    
    return pool_alloc(pool);
}

void standard_pools_free(StandardPools* pools, void* ptr, size_t size) {
    if (!pools || !ptr) return;
    
    MemoryPool* pool = NULL;
    
    if (size <= 8) pool = pools->pool_8;
    else if (size <= 16) pool = pools->pool_16;
    else if (size <= 32) pool = pools->pool_32;
    else if (size <= 64) pool = pools->pool_64;
    else if (size <= 128) pool = pools->pool_128;
    else if (size <= 256) pool = pools->pool_256;
    
    if (pool) {
        pool_free(pool, ptr);
    }
}

void standard_pools_destroy(StandardPools* pools) {
    if (!pools) return;
    
    if (pools->pool_8) pool_destroy(pools->pool_8);
    if (pools->pool_16) pool_destroy(pools->pool_16);
    if (pools->pool_32) pool_destroy(pools->pool_32);
    if (pools->pool_64) pool_destroy(pools->pool_64);
    if (pools->pool_128) pool_destroy(pools->pool_128);
    if (pools->pool_256) pool_destroy(pools->pool_256);

    aether_caps_free(pools, sizeof(StandardPools));
}

