#ifndef AETHER_SLICE_H
#define AETHER_SLICE_H

/* First-class slices (#1286): the C shape of an Aether `T[]`.
 *
 * A slice is a fat pointer, `{ ptr, len }`, that does NOT own the
 * elements it points at. The element type is static (the Aether
 * typechecker knows it); the generated C scales an index by sizeof(T)
 * itself, so one struct serves every element type.
 *
 * `len` is the element count. AETHER_SLICE_UNBOUNDED marks a VIEW whose
 * length the compiler could not know: a bare `ptr as T[]`, or a `T[]`
 * handed back by an extern C function. Such a view indexes without a
 * bounds check (exactly what `T[]` meant before slices carried a length)
 * and reports `.len` as -1. Sub-slicing it with an explicit end
 * (`v[0..n]`) yields a bounded slice.
 *
 * Every helper is `static inline`: the header is textually included by
 * each generated program, so a program that never touches a slice pays
 * nothing, and there is no runtime symbol to link. A failed check goes
 * through aether_panic(), the same unwind `panic("...")` uses, so a
 * `try` block can catch it and an actor step dies cleanly. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "actors/aether_panic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherSlice {
    void*    ptr;
    uint64_t len;
} AetherSlice;

#define AETHER_SLICE_UNBOUNDED UINT64_MAX

static inline AetherSlice aether_slice_make(void* ptr, uint64_t len) {
    AetherSlice s;
    s.ptr = ptr;
    s.len = len;
    return s;
}

/* A view over a raw pointer of unknown extent. */
static inline AetherSlice aether_slice_view(void* ptr) {
    return aether_slice_make(ptr, AETHER_SLICE_UNBOUNDED);
}

/* `make([]T, n)`: a fresh zeroed buffer of n elements, viewed as a slice.
 * The caller frees it with `free(s)` (the slice decays to its pointer). */
static inline AetherSlice aether_slice_alloc(int64_t count, size_t elem_size) {
    if (count < 0) {
        aether_panic("make([]T, n): negative element count");
    }
    void* p = calloc(count > 0 ? (size_t)count : 1, elem_size);
    if (!p) aether_panic("make([]T, n): out of memory");
    return aether_slice_make(p, (uint64_t)count);
}

/* `.len` — -1 for an unbounded view. */
static inline int64_t aether_slice_len(AetherSlice s) {
    return s.len == AETHER_SLICE_UNBOUNDED ? -1 : (int64_t)s.len;
}

static inline void aether_slice_fail_index(int64_t idx, uint64_t len,
                                           const char* file, int line) {
    static char msg[192];
    snprintf(msg, sizeof msg,
             "%s:%d: slice index %lld out of range for length %llu",
             file, line, (long long)idx, (unsigned long long)len);
    aether_panic(msg);
}

static inline void aether_slice_fail_range(int64_t lo, int64_t hi, uint64_t len,
                                           const char* file, int line) {
    static char msg[192];
    snprintf(msg, sizeof msg,
             "%s:%d: slice range %lld..%lld out of range for length %llu",
             file, line, (long long)lo, (long long)hi,
             (unsigned long long)len);
    aether_panic(msg);
}

/* `s[i]` — the address of element i, checked against len (an unbounded
 * view is not checked). Returns a byte pointer; the generated C casts it
 * to `T*` and dereferences, which keeps the result an lvalue for
 * `s[i] = v` and evaluates `s` exactly once. */
static inline void* aether_slice_at(AetherSlice s, int64_t i, size_t elem_size,
                                    const char* file, int line) {
    if (s.len != AETHER_SLICE_UNBOUNDED && (uint64_t)i >= s.len) {
        aether_slice_fail_index(i, s.len, file, line);
    }
    return (char*)s.ptr + (uint64_t)i * elem_size;
}

/* `s[lo..hi]` — the half-open sub-slice [lo, hi), sharing the backing
 * store. `hi` past the end, `lo > hi`, or a negative bound is a panic;
 * on an unbounded view only `lo <= hi` and `lo >= 0` can be checked. */
static inline AetherSlice aether_slice_sub(AetherSlice s, int64_t lo, int64_t hi,
                                           size_t elem_size,
                                           const char* file, int line) {
    if (lo < 0 || hi < lo ||
        (s.len != AETHER_SLICE_UNBOUNDED && (uint64_t)hi > s.len)) {
        aether_slice_fail_range(lo, hi, s.len, file, line);
    }
    return aether_slice_make((char*)s.ptr + (uint64_t)lo * elem_size,
                             (uint64_t)(hi - lo));
}

/* `s[lo..]` — from lo to the end. On an unbounded view the result stays
 * unbounded. */
static inline AetherSlice aether_slice_from(AetherSlice s, int64_t lo,
                                            size_t elem_size,
                                            const char* file, int line) {
    if (s.len == AETHER_SLICE_UNBOUNDED) {
        if (lo < 0) aether_slice_fail_range(lo, lo, s.len, file, line);
        return aether_slice_view((char*)s.ptr + (uint64_t)lo * elem_size);
    }
    return aether_slice_sub(s, lo, (int64_t)s.len, elem_size, file, line);
}

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SLICE_H */
