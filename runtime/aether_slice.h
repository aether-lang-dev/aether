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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "actors/aether_panic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherSlice {
    void*    ptr;
    uint64_t len;
    /* #3025: AETHER_SLICE_READONLY, or 0. Beside `len` and read only where
     * an element is stored, so a read costs what it did. */
    uint32_t flags;
} AetherSlice;

#define AETHER_SLICE_UNBOUNDED UINT64_MAX

/* #3025: a view of memory the program may read and not write: the bytes of
 * a string (`string.bytes(s)`), a literal's in read-only data or a counted
 * string's that others share. A store through it panics, by name, where an
 * element store is bounds-checked; every view made from it (a sub-slice, a
 * pattern's tail, a copy) carries the mark. */
#define AETHER_SLICE_READONLY 1u

/* #3072: a view of elements another slice or an array holds: a sub-slice of
 * a bounded slice (or one at an offset of any view), a list pattern's tail,
 * an array viewed as a slice. It owns nothing (R1, R2), so `free` of it is
 * a named panic, as `free` of a read-only view is: the allocator would take
 * a pointer it never handed out, or one the owner still holds. A view of a
 * raw pointer bounded at its start, `(p as T[])[0..n]`, is how a program
 * holds a buffer it owns, and is not marked. */
#define AETHER_SLICE_BORROWED 2u

/* #3159: the buffer `make([]T, n)` handed out. Its elements are places: a
 * store into one takes the value (#2921), so `free` gives back what each
 * holds before the buffer goes. A view of a raw pointer is not marked: its
 * elements are C's, and a store there takes nothing. */
#define AETHER_SLICE_MADE 4u

/* `append(s, v)`: a `make` buffer `append` has grown. Its capacity is not
 * stored: it is the smallest power of two, at least 4, that holds its
 * length (aether_slice_grown_cap), so `append` doubles the buffer when the
 * length reaches one and writes in place otherwise, amortised O(1), with
 * the slice still `{ ptr, len, flags }`. A buffer that is not marked holds
 * exactly its length. A sub-slice, a pattern's tail or an array view is
 * BORROWED, so the mark never describes a length it was not grown to. */
#define AETHER_SLICE_GROWN 8u

/* `const void*`: a view is made over an array of strings too, `const char*
 * xs[N]`. Taken as `void*`, cl.exe read the elements' `const` as the array's
 * and refused the call (C4090, an error under the MSVC check, #2799); the
 * view itself is untyped either way. */
static inline AetherSlice aether_slice_make(const void* ptr, uint64_t len) {
    AetherSlice s;
    s.ptr = (void*)ptr;
    s.len = len;
    s.flags = 0;
    return s;
}

/* #3025: `s` as a read-only view: the same elements, a store through it a
 * panic. */
static inline AetherSlice aether_slice_readonly(AetherSlice s) {
    s.flags |= AETHER_SLICE_READONLY;
    return s;
}

/* #3072: `s` as a view of elements something else holds (an array viewed
 * as a slice, a list pattern's tail): `free` of it panics. */
static inline AetherSlice aether_slice_borrowed(AetherSlice s) {
    s.flags |= AETHER_SLICE_BORROWED;
    return s;
}

/* A view of `s`'s memory at `ptr`, `len` long, with `s`'s marks: a
 * sub-slice, a list pattern's tail. */
static inline AetherSlice aether_slice_derive(AetherSlice s, const void* ptr, uint64_t len) {
    AetherSlice d = aether_slice_make(ptr, len);
    d.flags = s.flags;
    return d;
}

/* A view over a raw pointer of unknown extent. */
static inline AetherSlice aether_slice_view(void* ptr) {
    return aether_slice_make(ptr, AETHER_SLICE_UNBOUNDED);
}

/* `make([]T, n)`: a fresh zeroed buffer of n elements, viewed as a slice.
 * The caller frees it with `free(s)` (aether_slice_free). */
static inline AetherSlice aether_slice_alloc(int64_t count, size_t elem_size) {
    if (count < 0) {
        aether_panic("make([]T, n): negative element count");
    }
    void* p = calloc(count > 0 ? (size_t)count : 1, elem_size);
    if (!p) aether_out_of_memory("make([]T, n): out of memory");
    AetherSlice s = aether_slice_make(p, (uint64_t)count);
    s.flags = AETHER_SLICE_MADE;
    return s;
}

/* `.len` — -1 for an unbounded view. */
static inline int64_t aether_slice_len(AetherSlice s) {
    return s.len == AETHER_SLICE_UNBOUNDED ? -1 : (int64_t)s.len;
}

/* #2932: a failed check panics with its message built whole, a long source
 * path in it included, as a counted string the catch that receives it owns.
 * Declared as the generated program declares them; std/string defines both. */
void* aether_interp_string(const char* fmt, va_list ap);
void string_release(const char* s);

static inline void aether_slice_release_msg(const void* p) {
    string_release((const char*)p);
}

static inline void aether_slice_panicf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    void* msg = aether_interp_string(fmt, ap);
    va_end(ap);
    if (!msg) aether_panic("slice bounds check failed (out of memory for its message)");
    aether_panic_owned((const char*)msg, aether_slice_release_msg);
}

static inline void aether_slice_fail_index(int64_t idx, uint64_t len,
                                           const char* file, int line) {
    aether_slice_panicf("%s:%d: slice index %lld out of range for length %llu",
                        file, line, (long long)idx, (unsigned long long)len);
}

/* #3113: an index into a fixed-size array `T[N]` outside `0 ..< N`, the
 * slice's panic with the array's length. */
static inline void aether_array_fail_index(int64_t idx, uint64_t len,
                                           const char* file, int line) {
    aether_slice_panicf("%s:%d: array index %lld out of range for length %llu",
                        file, line, (long long)idx, (unsigned long long)len);
}

/* #3479: `s[i]` on a string outside `0 ..< length`, the slice's panic with
 * the string's length. */
static inline void aether_string_fail_index(int64_t idx, uint64_t len,
                                            const char* file, int line) {
    aether_slice_panicf("%s:%d: string index %lld out of range for length %llu",
                        file, line, (long long)idx, (unsigned long long)len);
}

/* #3113: `a[i]` on a `T[N]`: `i`, checked against N, for the generated C
 * to index with (`a[aether_array_index(i, N, ...)]`), so the element stays
 * an lvalue and `i` is evaluated once. A negative index is out of range.
 * With a constant N and a loop bounded by it the C compiler drops the
 * test. */
static inline int64_t aether_array_index(int64_t i, uint64_t len,
                                         const char* file, int line) {
    if ((uint64_t)i >= len) aether_array_fail_index(i, len, file, line);
    return i;
}

static inline void aether_slice_fail_range(int64_t lo, int64_t hi, uint64_t len,
                                           const char* file, int line) {
    aether_slice_panicf("%s:%d: slice range %lld..%lld out of range for length %llu",
                        file, line, (long long)lo, (long long)hi,
                        (unsigned long long)len);
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

static inline void aether_slice_fail_readonly(const char* file, int line) {
    aether_slice_panicf("%s:%d: a write through a read-only view of a string", file, line);
}

/* #3025: `s[i] = v`, `s[i] += v`, `s[i]++`: the element's address for a
 * store, which a read-only view (`string.bytes(s)`) refuses with a named
 * panic, checked beside the bounds. A read goes through aether_slice_at
 * and tests nothing more than it did. */
static inline void* aether_slice_at_w(AetherSlice s, int64_t i, size_t elem_size,
                                      const char* file, int line) {
    if (s.flags & AETHER_SLICE_READONLY) aether_slice_fail_readonly(file, line);
    return aether_slice_at(s, i, elem_size, file, line);
}

/* #3029: `s[i]` in a function's view body, where the function's entry has
 * tested once that `s` is an unbounded view: the element's address with no
 * test, which is what aether_slice_at does for a view after its test of
 * the sentinel. Without that test in the loop the compiler can vectorize. */
static inline void* aether_slice_view_at(AetherSlice s, int64_t i, size_t elem_size) {
    return (char*)s.ptr + (uint64_t)i * elem_size;
}

/* #3029: a store there, which still refuses a read-only view. */
static inline void* aether_slice_view_at_w(AetherSlice s, int64_t i, size_t elem_size,
                                           const char* file, int line) {
    if (s.flags & AETHER_SLICE_READONLY) aether_slice_fail_readonly(file, line);
    return (char*)s.ptr + (uint64_t)i * elem_size;
}

/* The address `lo` elements into `p`. An empty slice can have a NULL
 * store, and NULL + 0 is undefined in C (UBSan reports it), so offset 0
 * is `p` itself. */
static inline void* aether_slice_offset(void* p, int64_t lo, size_t elem_size) {
    return lo ? (char*)p + (uint64_t)lo * elem_size : p;
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
    AetherSlice d = aether_slice_derive(s, aether_slice_offset(s.ptr, lo, elem_size),
                                        (uint64_t)(hi - lo));
    /* #3072: a view of a bounded slice's elements, or of any view's from
     * an offset, owns nothing. */
    if (s.len != AETHER_SLICE_UNBOUNDED || lo != 0) d.flags |= AETHER_SLICE_BORROWED;
    return d;
}

/* `s[lo..]` — from lo to the end. On an unbounded view the result stays
 * unbounded. */
static inline AetherSlice aether_slice_from(AetherSlice s, int64_t lo,
                                            size_t elem_size,
                                            const char* file, int line) {
    if (s.len == AETHER_SLICE_UNBOUNDED) {
        if (lo < 0) aether_slice_fail_range(lo, lo, s.len, file, line);
        AetherSlice d = aether_slice_derive(s, aether_slice_offset(s.ptr, lo, elem_size),
                                            AETHER_SLICE_UNBOUNDED);
        if (lo != 0) d.flags |= AETHER_SLICE_BORROWED;   /* #3072 */
        return d;
    }
    return aether_slice_sub(s, lo, (int64_t)s.len, elem_size, file, line);
}

/* The capacity of a buffer marked AETHER_SLICE_GROWN of length `len`. */
static inline uint64_t aether_slice_grown_cap(uint64_t len) {
    uint64_t c = 4;
    while (c < len) c <<= 1;
    return c;
}

/* `append(s, v)`, its first half: `s` one element longer, the new last
 * element zeroed for the generated code to store `v` into (a store that
 * takes the value, as `s[i] = v` does). `s` must be a buffer the program
 * owns, which `free(s)` would release: one `make` or `append` handed out,
 * or the empty slice (`[]`, `null`), which starts a buffer. When the buffer
 * is full it is reallocated to twice its capacity: the old storage goes, as
 * `free` would release it, and its elements move to the new storage with
 * what they hold. A view (a sub-slice, an array, a string's bytes, C
 * memory) owns nothing, so growing it is a named panic, as freeing it is. */
static inline AetherSlice aether_slice_grow1(AetherSlice s, size_t elem_size,
                                             const char* file, int line) {
    if (s.len == AETHER_SLICE_UNBOUNDED)
        aether_slice_panicf("%s:%d: append to a view of unknown length, which owns nothing",
                            file, line);
    if (s.flags & AETHER_SLICE_READONLY)
        aether_slice_panicf("%s:%d: append to a read-only view of a string, which owns nothing",
                            file, line);
    if (s.flags & AETHER_SLICE_BORROWED)
        aether_slice_panicf("%s:%d: append to a view of elements another slice or an array "
                            "holds, which owns nothing", file, line);
    int empty = s.ptr == NULL && s.len == 0;
    if (!empty && !(s.flags & AETHER_SLICE_MADE))
        aether_slice_panicf("%s:%d: append to a view of memory the program did not make "
                            "(make([]T, n) or append), which it cannot grow", file, line);
    uint64_t len = empty ? 0 : s.len;
    int grown = !empty && (s.flags & AETHER_SLICE_GROWN);
    if (!grown || len == aether_slice_grown_cap(len)) {
        uint64_t cap = aether_slice_grown_cap(len + 1);
        if (cap > SIZE_MAX / (elem_size ? elem_size : 1))
            aether_slice_panicf("%s:%d: append: out of memory", file, line);
        void* p = realloc(empty ? NULL : s.ptr, (size_t)cap * elem_size);
        if (!p) aether_slice_panicf("%s:%d: append: out of memory", file, line);
        s.ptr = p;
        s.flags = AETHER_SLICE_MADE | AETHER_SLICE_GROWN;
    }
    memset((char*)s.ptr + len * elem_size, 0, elem_size);
    s.len = len + 1;
    return s;
}

/* ---- Slice ownership (docs/memory-management.md, Slices) ----
 *
 * A `make` buffer, or one `append` or `push` grew, has one owner: a local
 * (or a `@keep` parameter) that holds it, an element of a slice of slices,
 * or a place the program frees by hand (a field, a module-level `var`).
 * The owning local is dropped at every exit of its scope, unless the buffer
 * was moved out of it first: a move reads the slice and leaves the place
 * empty (aether_slice_take), so the drop that follows finds nothing. A copy
 * read out of a place that does not own it (a parameter, a field, an
 * element, a module-level `var`) is marked BORROWED (aether_slice_lend), so
 * whatever holds the copy never frees it. */

/* Does `s` own the buffer it views: one `make` handed out or `append`
 * grew, not a view of one? */
static inline int aether_slice_owned(AetherSlice s) {
    return (s.flags & AETHER_SLICE_MADE) &&
           !(s.flags & (AETHER_SLICE_READONLY | AETHER_SLICE_BORROWED)) &&
           s.len != AETHER_SLICE_UNBOUNDED;
}

/* A move: the slice at `p`, and `p` left empty. */
static inline AetherSlice aether_slice_take(AetherSlice* p) {
    AetherSlice s = *p;
    p->ptr = NULL;
    p->len = 0;
    p->flags = 0;
    return s;
}

/* A copy of a slice some other place owns: a view of it, which never frees
 * it. The empty slice stays itself, so a place given one may start a buffer
 * of its own (`push`, `append`). */
static inline AetherSlice aether_slice_lend(AetherSlice s) {
    if (s.ptr != NULL || s.len != 0) s.flags |= AETHER_SLICE_BORROWED;
    return s;
}

/* The drop of an owning place: the buffer it owns, if it owns one, is
 * released, its elements first (`drop`, each element's own release, or
 * NULL when they own nothing), and the place is left empty. A view, the
 * empty slice and a moved-from place release nothing. */
static inline void aether_slice_drop(AetherSlice* p, size_t elem_size,
                                     void (*drop)(const void*)) {
    AetherSlice s = aether_slice_take(p);
    if (!aether_slice_owned(s)) return;
    if (drop) {
        for (uint64_t i = 0; i < s.len; i++) drop((const char*)s.ptr + i * elem_size);
    }
    free(s.ptr);
}

/* aether_slice_drop of a slice whose elements own nothing, at `p`: the
 * element drop of a slice of slices of numbers, and an unwind drop. */
static inline void aether_slice_drop_plain(const void* p) {
    aether_slice_drop((AetherSlice*)(void*)p, 0, NULL);
}

/* A store into an owning place (`xs = v` for a local that owns, `rows[i] =
 * v` for an element of a slice of slices): what the place owned is
 * dropped, and `v` is what to store. `v` may not view the buffer the place
 * owns (`xs = xs[1..]`, or a function handing back the slice it was lent):
 * the store would free what `v` views, so that is a named panic. */
static inline AetherSlice aether_slice_set(AetherSlice* p, AetherSlice v, size_t elem_size,
                                           void (*drop)(const void*), const char* file,
                                           int line) {
    AetherSlice old = *p;
    if (aether_slice_owned(old) && v.ptr != NULL) {
        uint64_t cap = (old.flags & AETHER_SLICE_GROWN) ? aether_slice_grown_cap(old.len)
                                                         : old.len;
        const char* lo = (const char*)old.ptr;
        const char* at = (const char*)v.ptr;
        if (at >= lo && at < lo + cap * elem_size)
            aether_slice_panicf("%s:%d: a slice re-bound to a view of the buffer it owned, "
                                "which the store frees: copy the elements out first",
                                file, line);
    }
    aether_slice_drop(p, elem_size, drop);
    return v;
}

/* #3072: `free(s)`: the buffer `make([]T, n)` handed out, or a pointer the
 * program owns viewed from its start. A view owns nothing (R1, R2): `free`
 * of a read-only one (string.bytes) or of one of another slice's or an
 * array's elements is a named panic, where the allocator was given a
 * pointer it never handed out (heap corruption) or one its owner still
 * holds (a double free later). */
static inline void aether_slice_free(AetherSlice s, const char* file, int line) {
    if (s.flags & AETHER_SLICE_READONLY)
        aether_slice_panicf("%s:%d: free of a read-only view of a string, which owns nothing",
                            file, line);
    if (s.flags & AETHER_SLICE_BORROWED)
        aether_slice_panicf("%s:%d: free of a view of elements another slice or an array "
                            "holds, which owns nothing", file, line);
    free(s.ptr);
}

/* #3159: `free(s)` of a slice whose element type owns something (a string,
 * a closure, a struct or composite that holds one): a `make` buffer's
 * elements each give back what they hold (`drop`, the place's own release,
 * which empties it), then the buffer goes, as a struct's destroy releases
 * its fields. Before, every string stored into the buffer leaked. A view is
 * refused before anything is released: its elements are another's. */
static inline void aether_slice_free_elems(AetherSlice s, size_t elem_size,
                                           void (*drop)(const void*),
                                           const char* file, int line) {
    if (s.flags & (AETHER_SLICE_READONLY | AETHER_SLICE_BORROWED)) {
        aether_slice_free(s, file, line);
        return;
    }
    if ((s.flags & AETHER_SLICE_MADE) && s.len != AETHER_SLICE_UNBOUNDED) {
        for (uint64_t i = 0; i < s.len; i++) drop((const char*)s.ptr + i * elem_size);
    }
    free(s.ptr);
}

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SLICE_H */
