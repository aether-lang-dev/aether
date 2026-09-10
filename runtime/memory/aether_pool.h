#ifndef AETHER_POOL_H
#define AETHER_POOL_H

#include <stddef.h>

typedef struct MemoryPool MemoryPool;

MemoryPool* pool_create(size_t object_size, int initial_count);
void* pool_alloc(MemoryPool* pool);
void pool_free(MemoryPool* pool, void* ptr);
int pool_get_capacity(MemoryPool* pool);
int pool_get_used(MemoryPool* pool);
void pool_destroy(MemoryPool* pool);

typedef struct {
    MemoryPool* pool_8;
    MemoryPool* pool_16;
    MemoryPool* pool_32;
    MemoryPool* pool_64;
    MemoryPool* pool_128;
    MemoryPool* pool_256;
} StandardPools;

StandardPools* standard_pools_create(void);
void* standard_pools_alloc(StandardPools* pools, size_t size);
void standard_pools_free(StandardPools* pools, void* ptr, size_t size);
void standard_pools_destroy(StandardPools* pools);

#endif

