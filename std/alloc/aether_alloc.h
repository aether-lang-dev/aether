/* Copyright (c) 2026 Aether Developers. */
#ifndef AETHER_ALLOC_H
#define AETHER_ALLOC_H

#include <stddef.h>

/* std.alloc (#1045): an explicit, swappable allocator handle. */
typedef struct AetherAllocator {
    void* (*alloc_fn)(void* data, size_t size);
    void* (*realloc_fn)(void* data, void* ptr, size_t old_size, size_t new_size);
    void  (*free_fn)(void* data, void* ptr, size_t size);
    void* data;
} AetherAllocator;

AetherAllocator* aether_allocator_system(void);

void* aether_allocator_alloc(AetherAllocator* a, size_t size);
void* aether_allocator_realloc(AetherAllocator* a, void* ptr,
                               size_t old_size, size_t new_size);
void  aether_allocator_free(AetherAllocator* a, void* ptr, size_t size);

/* #3778: `size` bytes at `align` (a power of two, at least sizeof(void*));
 * NULL for another alignment or when the memory cannot be had. Released
 * with aether_allocator_free_aligned and the same size and alignment, never
 * aether_allocator_free: the block starts inside what the allocator gave. */
void* aether_allocator_alloc_aligned(AetherAllocator* a, size_t size, size_t align);
void  aether_allocator_free_aligned(AetherAllocator* a, void* block, size_t size, size_t align);

struct Arena;
AetherAllocator* aether_allocator_arena(struct Arena* arena);
void aether_allocator_arena_destroy(AetherAllocator* a);

#endif
