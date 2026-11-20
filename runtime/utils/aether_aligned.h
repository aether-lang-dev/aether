/* Heap memory at an alignment malloc does not promise (#3774, #3778).
 *
 * malloc gives 16 bytes (8 on some 32-bit targets), too little for a type
 * declared aligned(64) and for a record that is to own a cache line. Each
 * platform's aligned allocator has a free of its own: _aligned_free on
 * Windows, plain free after posix_memalign elsewhere, so a block from here
 * goes back through aether_aligned_heap_free and nothing else.
 *
 * Inline, so the runtime (aether_numa.c), std.alloc and std.net use the one
 * definition without a symbol to link: std.alloc's file is linked into the
 * compiler too, which carries no runtime. */
#ifndef AETHER_ALIGNED_HEAP_H
#define AETHER_ALIGNED_HEAP_H

#include <stddef.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif

/* `align` a power of two and a multiple of sizeof(void*); NULL when it is
 * not, or when the memory cannot be had. A `size` of 0 still gives a block. */
static inline void* aether_aligned_heap_alloc(size_t size, size_t align) {
    if (align < sizeof(void*) || (align & (align - 1)) != 0) return NULL;
    if (size == 0) size = 1;
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void* ptr = NULL;
    return posix_memalign(&ptr, align, size) == 0 ? ptr : NULL;
#endif
}

static inline void aether_aligned_heap_free(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

#endif
