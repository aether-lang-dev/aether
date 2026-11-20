/* Copyright (c) 2026 Aether Developers. */
#include "aether_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/utils/aether_aligned.h"

static void* sys_alloc(void* data, size_t size) {
    (void)data;
    if (size == 0) size = 1;
    return malloc(size);
}
static void* sys_realloc(void* data, void* ptr, size_t old_size, size_t new_size) {
    (void)data; (void)old_size;
    if (new_size == 0) { free(ptr); return NULL; }
    return realloc(ptr, new_size);
}
static void sys_free(void* data, void* ptr, size_t size) {
    (void)data; (void)size;
    free(ptr);
}

static AetherAllocator g_system = { sys_alloc, sys_realloc, sys_free, NULL };

AetherAllocator* aether_allocator_system(void) { return &g_system; }

void* aether_allocator_alloc(AetherAllocator* a, size_t size) {
    if (!a || !a->alloc_fn) return NULL;
    return a->alloc_fn(a->data, size);
}

void* aether_allocator_realloc(AetherAllocator* a, void* ptr,
                               size_t old_size, size_t new_size) {
    if (!a) return NULL;
    if (a->realloc_fn) return a->realloc_fn(a->data, ptr, old_size, new_size);
    if (new_size == 0) { aether_allocator_free(a, ptr, old_size); return NULL; }
    void* dst = aether_allocator_alloc(a, new_size);
    if (!dst) return NULL;
    if (ptr) {
        size_t n = old_size < new_size ? old_size : new_size;
        memcpy(dst, ptr, n);
        aether_allocator_free(a, ptr, old_size);
    }
    return dst;
}

void aether_allocator_free(AetherAllocator* a, void* ptr, size_t size) {
    if (!a || !ptr || !a->free_fn) return;
    a->free_fn(a->data, ptr, size);
}

/* #3778: a block at `align`, a power of two and a multiple of
 * sizeof(void*). The system allocator has the platform's aligned
 * allocator; any other (an arena, a tracking wrapper, a host's) is asked
 * for `size + align - 1 + sizeof(void*)` bytes, the block is the first
 * aligned address past a pointer's room, and the pointer before it holds
 * what the allocator gave, for the release. */
static int align_ok(size_t align) {
    return align >= sizeof(void*) && (align & (align - 1)) == 0;
}

static size_t aligned_extra(size_t align) {
    return align - 1 + sizeof(void*);
}

void* aether_allocator_alloc_aligned(AetherAllocator* a, size_t size, size_t align) {
    if (!a || !align_ok(align)) return NULL;
    if (a == &g_system) return aether_aligned_heap_alloc(size, align);
    if (!a->alloc_fn || size > SIZE_MAX - aligned_extra(align)) return NULL;
    char* got = (char*)a->alloc_fn(a->data, size + aligned_extra(align));
    if (!got) return NULL;
    uintptr_t at = ((uintptr_t)got + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1);
    ((void**)at)[-1] = got;
    return (void*)at;
}

void aether_allocator_free_aligned(AetherAllocator* a, void* block, size_t size, size_t align) {
    if (!a || !block || !align_ok(align)) return;
    if (a == &g_system) {
        aether_aligned_heap_free(block);
        return;
    }
    if (!a->free_fn) return;
    a->free_fn(a->data, ((void**)block)[-1], size + aligned_extra(align));
}

/* aether_allocator_arena lives in runtime/memory/aether_arena.c: it must
 * link with the arena, which the bootstrap compiler does not carry while it
 * does link this file via STD_SRC. Referencing the arena here would break
 * the compiler link. */
