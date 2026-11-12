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
#include <stdatomic.h>
#include "actors/aether_panic.h"
#include "utils/aether_count.h"
#include "utils/aether_compiler.h"   /* AETHER_CPU_PAUSE, a slice var's lock */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherSlice {
    void*    ptr;
    uint64_t len;
    /* #3025: the marks below, in bits 0..7. Beside `len` and read only where
     * an element is stored, so a read costs what it did. #3568: bits 8..31
     * hold bits 32..55 of the view's offset into its buffer (`off`). */
    uint32_t flags;
    /* #3568: for a slice of a counted buffer (AETHER_SLICE_MADE), bits 0..31
     * of the byte offset of `ptr` from the buffer's first element, by which
     * the buffer's header is found (aether_slice_buf). 56 bits reach past
     * any address space, so every view of a buffer finds it. It sits in the
     * padding after `flags`: the struct keeps its size and the offsets of
     * `ptr`, `len` and `flags`. */
    uint32_t off;
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
 * elements are C's, and a store there takes nothing.
 *
 * #3568: the mark of a counted buffer, and of every view of one (a sub-slice
 * or a copy keeps it). Such a buffer carries a header before its first
 * element (AetherSliceBuf) with a count of the places that hold it, and is
 * released by the last of them. A slice without the mark (a view of an
 * array, of a string's bytes, of C memory, a `T[]` an extern returns, one a
 * host builds) is not counted, and nothing is read before its `ptr`. */
#define AETHER_SLICE_MADE 4u

/* `append(s, v)`: a `make` buffer `append` has grown. Its capacity is not
 * stored: it is the smallest power of two, at least 4, that holds its
 * length (aether_slice_grown_cap), so `append` doubles the buffer when the
 * length reaches one and writes in place otherwise, amortised O(1), with
 * the slice still `{ ptr, len, flags }`. A buffer that is not marked holds
 * exactly its length. A sub-slice, a pattern's tail or an array view is
 * BORROWED, so the mark never describes a length it was not grown to. */
#define AETHER_SLICE_GROWN 8u

/* #3570: a view of a counted string's characters (string.bytes(s)), which
 * holds one reference to the string: the 56 bits that carry a counted
 * buffer's offset (`off` and flags' bits 8..31) carry the string's header,
 * an AetherString*, instead. A string's characters may sit in a block of
 * their own (an adopted buffer), so no offset from `ptr` finds the header.
 * Always READONLY. */
#define AETHER_SLICE_STRING 16u

/* #3568: the bits of `flags` that are marks; the rest carry the offset. */
#define AETHER_SLICE_MARKS 0xFFu

/* ---- Counted buffers (#3568) ----
 *
 * A `make` or `append` buffer is counted (R1): every place that holds it, the
 * local that made it or a view of it in a local, a field, an element, a
 * closure, a cell or a message, holds one reference, and the last release
 * gives back what each element holds and frees it. A view therefore keeps
 * the buffer it views for as long as it is held, wherever it goes.
 *
 * The header sits AETHER_SLICE_HEAD bytes before the first element (which
 * keeps the elements' 16-byte alignment). `ops` gives an element's drop and
 * copy, for the element type the buffer was made with; NULL when elements
 * own nothing. The count is plain until the buffer is published (a module
 * `var`, a closure, a message, an actor's state: other threads may hold it),
 * then atomic, as a string's is (#2919). */
typedef struct AetherSliceOps {
    void (*drop)(const void* elem);   /* give back what the element holds, empty it */
    void (*copy)(void* elem);         /* take a reference to what it holds, in place */
    void (*publish)(void* elem);      /* make what it holds count atomically (#3568) */
} AetherSliceOps;

typedef struct AetherSliceBuf {
    int      refs;        /* the count; AETHER_SLICE_SHARED_BIT once published */
    uint32_t reserved;
    uint64_t len;         /* the elements the buffer holds */
    uint64_t elem_size;
    const AetherSliceOps* ops;
} AetherSliceBuf;

#define AETHER_SLICE_HEAD 32
#define AETHER_SLICE_SHARED_BIT 0x40000000
#define AETHER_SLICE_COUNT_MASK 0x3FFFFFFF

/* The byte offset of a counted slice's `ptr` from its buffer's first
 * element. */
static inline uint64_t aether_slice_off(AetherSlice s) {
    return ((uint64_t)(s.flags >> 8) << 32) | (uint64_t)s.off;
}

static inline AetherSlice aether_slice_with_off(AetherSlice s, uint64_t off) {
    s.off = (uint32_t)off;
    s.flags = (s.flags & AETHER_SLICE_MARKS) | ((uint32_t)(off >> 32) << 8);
    return s;
}

/* The header of the counted buffer `s` views (AETHER_SLICE_MADE). */
static inline AetherSliceBuf* aether_slice_buf(AetherSlice s) {
    return (AetherSliceBuf*)(void*)((char*)s.ptr - aether_slice_off(s) - AETHER_SLICE_HEAD);
}

/* `const void*`: a view is made over an array of strings too, `const char*
 * xs[N]`. Taken as `void*`, cl.exe read the elements' `const` as the array's
 * and refused the call (C4090, an error under the MSVC check, #2799); the
 * view itself is untyped either way. */
static inline AetherSlice aether_slice_make(const void* ptr, uint64_t len) {
    AetherSlice s;
    s.ptr = (void*)ptr;
    s.len = len;
    s.flags = 0;
    s.off = 0;
    return s;
}

/* #3570: the string a view of a counted string's characters holds, and its
 * count, as the generated program declares them; std/string defines them.
 * aether_string_counted says whether `s` is a counted string that is not
 * pinned (a literal the compiler made static is never freed). */
void string_retain(const char* s);
void string_release(const char* s);
void aether_string_publish(const void* s);
int aether_string_counted(const void* s);

/* The string header a view marked AETHER_SLICE_STRING holds. */
static inline const char* aether_slice_string_of(AetherSlice s) {
    return (const char*)(uintptr_t)aether_slice_off(s);
}

/* A reference to the buffer `s` views, for a place that holds `s`: to the
 * string it views, for a view of a string's characters. An uncounted slice
 * takes none. */
static inline void aether_slice_retain(AetherSlice s) {
    if (s.flags & AETHER_SLICE_STRING) {
        string_retain(aether_slice_string_of(s));
        return;
    }
    if (!(s.flags & AETHER_SLICE_MADE)) return;
    AetherSliceBuf* h = aether_slice_buf(s);
    int rc = aether_count_load(&h->refs);
    if (rc & AETHER_SLICE_SHARED_BIT) aether_count_inc(&h->refs);
    else h->refs = rc + 1;
}

/* The buffer given back: each element's hold first, then the storage. */
static inline void aether_slice_buf_free(AetherSliceBuf* h) {
    if (h->ops && h->ops->drop) {
        char* e = (char*)h + AETHER_SLICE_HEAD;
        for (uint64_t i = 0; i < h->len; i++) h->ops->drop(e + i * h->elem_size);
    }
    free(h);
}

/* The reference a place held to the buffer `s` views, given back; the last
 * frees the buffer. An uncounted slice gives back nothing. */
static inline void aether_slice_unref(AetherSlice s) {
    if (s.flags & AETHER_SLICE_STRING) {
        string_release(aether_slice_string_of(s));
        return;
    }
    if (!(s.flags & AETHER_SLICE_MADE)) return;
    AetherSliceBuf* h = aether_slice_buf(s);
    int rc = aether_count_load(&h->refs);
    if (rc & AETHER_SLICE_SHARED_BIT) {
        if ((aether_count_dec(&h->refs) & AETHER_SLICE_COUNT_MASK) == 1) aether_slice_buf_free(h);
        return;
    }
    h->refs = rc - 1;
    if (rc <= 1) aether_slice_buf_free(h);
}

/* `s` is reachable from other threads from here on (a module `var`, a
 * closure, a message, an actor's state): its count is atomic, and so is the
 * count of everything its elements hold (a row of a slice of slices, a
 * struct element's slices and strings), which another thread reaches
 * through it. The bit is set atomically, and the elements are published by
 * whoever set it, once: a buffer already published was walked then, and an
 * element stored into it since was published by its store (#3568). */
static inline void aether_slice_publish(AetherSlice s) {
    if (s.flags & AETHER_SLICE_STRING) {
        aether_string_publish(aether_slice_string_of(s));
        return;
    }
    if (!(s.flags & AETHER_SLICE_MADE)) return;
    AetherSliceBuf* h = aether_slice_buf(s);
    if (aether_count_load(&h->refs) & AETHER_SLICE_SHARED_BIT) return;
    if (aether_count_or(&h->refs, AETHER_SLICE_SHARED_BIT) & AETHER_SLICE_SHARED_BIT) return;
    if (h->ops && h->ops->publish) {
        char* e = (char*)h + AETHER_SLICE_HEAD;
        for (uint64_t i = 0; i < h->len; i++) h->ops->publish(e + i * h->elem_size);
    }
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

/* A view of `s`'s memory at `ptr` (at or past `s.ptr`), `len` long, with
 * `s`'s marks: a sub-slice, a list pattern's tail. A view of a counted
 * buffer keeps its offset into it (#3568). */
static inline AetherSlice aether_slice_derive(AetherSlice s, const void* ptr, uint64_t len) {
    AetherSlice d = aether_slice_make(ptr, len);
    d.flags = s.flags & AETHER_SLICE_MARKS;
    if (s.flags & AETHER_SLICE_MADE)
        d = aether_slice_with_off(d, aether_slice_off(s) +
                                         (uint64_t)((const char*)ptr - (const char*)s.ptr));
    else if (s.flags & AETHER_SLICE_STRING)   /* #3570: the same string */
        d = aether_slice_with_off(d, aether_slice_off(s));
    return d;
}

/* A view over a raw pointer of unknown extent. */
static inline AetherSlice aether_slice_view(void* ptr) {
    return aether_slice_make(ptr, AETHER_SLICE_UNBOUNDED);
}

/* `make([]T, n)`: a fresh zeroed counted buffer of n elements (#3568), its
 * one reference the caller's. `ops` is the element type's drop and copy, or
 * NULL when elements own nothing. */
static inline AetherSlice aether_slice_alloc(int64_t count, size_t elem_size,
                                             const AetherSliceOps* ops) {
    if (count < 0) {
        aether_panic("make([]T, n): negative element count");
    }
    if (elem_size && (uint64_t)count > (SIZE_MAX - AETHER_SLICE_HEAD) / elem_size)
        aether_out_of_memory("make([]T, n): out of memory");
    /* At least one byte past the header: the slice's `ptr` then points into
     * the block, not one past its end, so a leak checker that follows the
     * pointers a program holds (LeakSanitizer) finds an empty buffer a
     * module `var` or a row still holds. */
    size_t body = (size_t)count * elem_size;
    AetherSliceBuf* h = (AetherSliceBuf*)calloc(1, AETHER_SLICE_HEAD + (body ? body : 1));
    if (!h) aether_out_of_memory("make([]T, n): out of memory");
    h->refs = 1;
    h->len = (uint64_t)count;
    h->elem_size = elem_size;
    h->ops = ops;
    AetherSlice s = aether_slice_make((char*)h + AETHER_SLICE_HEAD, (uint64_t)count);
    s.flags = AETHER_SLICE_MADE;
    return s;
}

/* #3570: `v`, a view of the characters of the counted string `header`, as
 * a view that holds the string: read-only, marked AETHER_SLICE_STRING, the
 * header in its 56 offset bits, and one reference to the string taken. A
 * user-space pointer fits in 56 bits on every target Aether builds for; one
 * that does not gets the characters copied into a counted buffer of its
 * own instead, still read-only, so no view is ever made that cannot find
 * what it holds. */
static inline AetherSlice aether_slice_string_view(AetherSlice v, const void* header) {
    v.flags |= AETHER_SLICE_READONLY;
    uint64_t h = (uint64_t)(uintptr_t)header;
    if (h >> 56) {
        AetherSlice c = aether_slice_alloc((int64_t)v.len, 1, NULL);
        if (v.len) memcpy(c.ptr, v.ptr, (size_t)v.len);
        c.flags |= AETHER_SLICE_READONLY;
        return c;
    }
    string_retain((const char*)header);
    v.flags |= AETHER_SLICE_STRING;
    return aether_slice_with_off(v, h);
}

/* Does `s` hold what it views (a counted buffer or string), or view nothing?
 * A return's temporary string is released only then (#3570): an uncounted
 * view may point into it. */
static inline int aether_slice_holds_or_empty(AetherSlice s) {
    return (s.flags & (AETHER_SLICE_MADE | AETHER_SLICE_STRING)) != 0 || s.ptr == NULL;
}

/* `string.bytes(s)` (#3025, #3570): `v`, the bounded view of `s`'s
 * characters, read-only, and holding `s` when `s` is a counted string. A
 * literal and a plain C string are not counted: the view holds nothing, as
 * before. */
static inline AetherSlice aether_string_view(AetherSlice v, const void* s) {
    if (!s || v.ptr == NULL || !aether_string_counted(s)) return aether_slice_readonly(v);
    return aether_slice_string_view(v, s);
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

/* #3479: `s[i]` on a string outside `0 ..= length` (`length` is its
 * terminator, 0), the slice's panic with the string's length. */
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

/* Why `append` cannot grow `s`, or NULL: a view of unknown length, a
 * read-only one, a view of part of a buffer or of an array, memory the
 * program did not make. */
static inline const char* aether_slice_grow_refusal(AetherSlice s) {
    if (s.len == AETHER_SLICE_UNBOUNDED)
        return "append to a view of unknown length, which owns nothing";
    if (s.flags & AETHER_SLICE_READONLY)
        return "append to a read-only view of a string, which owns nothing";
    if (s.flags & AETHER_SLICE_BORROWED)
        return "append to a view of elements another slice or an array holds, which owns nothing";
    if (s.ptr == NULL && s.len == 0) return NULL;
    if (!(s.flags & AETHER_SLICE_MADE))
        return "append to a view of memory the program did not make (make([]T, n) or append), "
               "which it cannot grow";
    AetherSliceBuf* h = aether_slice_buf(s);
    if (aether_slice_off(s) != 0 || s.len != h->len)
        return "append to a view of elements another slice or an array holds, which owns nothing";
    return NULL;
}

/* `append(s, v)`, its first half: `s` one element longer, the new last
 * element zeroed for the generated code to store `v` into (a store that
 * takes the value, as `s[i] = v` does). `s` must hold the whole buffer: one
 * `make` or `append` handed out, a copy of one, or the empty slice (`[]`,
 * `null`), which starts a buffer of element type `ops`. Held by `s` alone,
 * the buffer grows in place, reallocated to twice its capacity when it is
 * full. Held by another place as well (a view, a copy, #3568), it is left as
 * it is to them, and `s` goes on in storage of its own, its elements copied,
 * each holding what it held: the reallocation used to free the storage under
 * a view, and a write in place would show in the other holder. A view of
 * part of a buffer (a sub-slice), an array, a string's bytes or C memory is
 * no buffer's own slice, so growing it is a named panic. */
static inline AetherSlice aether_slice_grow1(AetherSlice s, size_t elem_size,
                                             const AetherSliceOps* ops, int taken,
                                             const char* file, int line) {
    /* `taken`: `s` holds a reference `append` was given (a slice moved out
     * of its place, a call's fresh result), which a refusal gives back
     * before it panics (#3568: it was lost with the panic). */
    const char* why = aether_slice_grow_refusal(s);
    if (why) {
        if (taken) aether_slice_unref(s);
        aether_slice_panicf("%s:%d: %s", file, line, why);
    }
    int empty = s.ptr == NULL && s.len == 0;
    AetherSliceBuf* h = empty ? NULL : aether_slice_buf(s);
    uint64_t len = empty ? 0 : s.len;
    int shared = h && (aether_count_load(&h->refs) & AETHER_SLICE_COUNT_MASK) != 1;
    int grown = !empty && (s.flags & AETHER_SLICE_GROWN);
    if (shared || !grown || len == aether_slice_grown_cap(len)) {
        uint64_t cap = aether_slice_grown_cap(len + 1);
        size_t bytes = AETHER_SLICE_HEAD + (size_t)cap * elem_size;
        AetherSliceBuf* n = NULL;
        if (cap > (SIZE_MAX - AETHER_SLICE_HEAD) / (elem_size ? elem_size : 1)) {
            bytes = 0;   /* no allocation takes it */
        } else if (!shared) {
            n = (AetherSliceBuf*)realloc(h, bytes);
        } else {
            n = (AetherSliceBuf*)malloc(bytes);
        }
        if (!n) {
            if (taken) aether_slice_unref(s);
            aether_slice_panicf("%s:%d: append: out of memory", file, line);
        }
        if (!shared) {
            if (!h) {
                memset(n, 0, AETHER_SLICE_HEAD);
                n->refs = 1;
                n->elem_size = elem_size;
                n->ops = ops;
            }
        } else {
            /* Another place holds the buffer too: it keeps it, and `s`
             * goes on in storage of its own, each element a copy that holds
             * what it held. */
            memcpy(n, h, AETHER_SLICE_HEAD + (size_t)len * elem_size);
            n->refs = 1;
            if (n->ops && n->ops->copy) {
                char* e = (char*)n + AETHER_SLICE_HEAD;
                for (uint64_t i = 0; i < len; i++) n->ops->copy(e + i * elem_size);
            }
            aether_slice_unref(s);
        }
        s = aether_slice_make((char*)n + AETHER_SLICE_HEAD, len);
        s.flags = AETHER_SLICE_MADE | AETHER_SLICE_GROWN;
        h = n;
    }
    memset((char*)s.ptr + len * elem_size, 0, elem_size);
    s.len = len + 1;
    h->len = len + 1;
    return s;
}

/* ---- Slice ownership (docs/memory-management.md, Slices) ----
 *
 * Every place that holds a slice of a counted buffer holds one reference to
 * it (R2, #3568): a local, a `@keep` parameter, an element of a slice of
 * slices, a field, a module-level `var`, a part of a tuple, an optional, a
 * sum or an array, a closure's environment or cell, an actor's state, a
 * message. Leaving a scope or a store over it gives the reference back; a
 * parameter borrows its caller's. A move reads the slice and leaves the
 * place empty (aether_slice_take), so the release that follows finds
 * nothing. A copy into a place takes a reference of its own
 * (aether_slice_copy) and is marked BORROWED: it shares the buffer, so it
 * cannot grow it or free it for the others. */

/* A move: the slice at `p`, and `p` left empty. */
static inline AetherSlice aether_slice_take(AetherSlice* p) {
    AetherSlice s = *p;
    p->ptr = NULL;
    p->len = 0;
    p->flags = 0;
    p->off = 0;
    return s;
}

/* `s` marked as a view of what another place holds: growing it, or freeing
 * it for the others, is a named panic. The empty slice stays itself, so a
 * place given one may start a buffer of its own (`push`, `append`). */
static inline AetherSlice aether_slice_lend(AetherSlice s) {
    if (s.ptr != NULL || s.len != 0) s.flags |= AETHER_SLICE_BORROWED;
    return s;
}

/* A copy of `s` for a place to hold: a reference of its own to the buffer
 * (none for an uncounted slice), with `s`'s marks. A copy of the whole
 * buffer may grow: the buffer being shared, the growth moves the copy to
 * storage of its own (aether_slice_grow1). */
static inline AetherSlice aether_slice_copy(AetherSlice s) {
    aether_slice_retain(s);
    return s;
}

/* The release of a place: the reference it held given back, the last
 * giving back the buffer and what its elements hold, and the place left
 * empty. An uncounted slice, the empty slice and a moved-from place release
 * nothing. */
static inline void aether_slice_release(AetherSlice* p) {
    aether_slice_unref(aether_slice_take(p));
}

/* aether_slice_release of the slice at `p`: the element drop of a slice of
 * slices, a cell's and an unwind's. */
static inline void aether_slice_release_at(const void* p) {
    aether_slice_release((AetherSlice*)(void*)p);
}

/* A reference of its own for the slice at `p`, an element of a buffer that
 * is being copied: the element copy of a slice of slices. */
static inline void aether_slice_retain_at(void* p) {
    aether_slice_retain(*(AetherSlice*)p);
}

/* The publish of the slice at `p`, a row of a published slice of slices. */
static inline void aether_slice_publish_at(void* p) {
    aether_slice_publish(*(AetherSlice*)p);
}

/* The element ops of a slice of slices: each row is a counted slice of its
 * own, which carries its element type in its own header. */
static inline const AetherSliceOps* aether_slice_row_ops(void) {
    static const AetherSliceOps ops = {aether_slice_release_at, aether_slice_retain_at,
                                       aether_slice_publish_at};
    return &ops;
}

/* The drop of a slice place, as aether_slice_release (an owning local at
 * its scope's exit). */
static inline void aether_slice_drop(AetherSlice* p) {
    aether_slice_release(p);
}

/* A store into a slice place (`xs = v`, `rows[i] = v`, `s.items = v`): `v`
 * holds a reference of its own already (a move, a copy, a fresh result), so
 * the reference the place held is given back and `v` is what to store. `v`
 * may view the buffer the place held (`xs = xs[1..]`): its own reference
 * keeps that buffer (#3568). */
static inline AetherSlice aether_slice_set(AetherSlice* p, AetherSlice v) {
    aether_slice_release(p);
    return v;
}

/* aether_slice_set for a place other threads may read (a module `var`, an
 * actor's state): the value is published first. */
static inline AetherSlice aether_slice_set_shared(AetherSlice* p, AetherSlice v) {
    aether_slice_publish(v);
    aether_slice_release(p);
    return v;
}

/* #3568: the lock of a module-level slice `var`, as a string var's (#3522):
 * held for a load and a retain, or for a swap, a few instructions, so a
 * spin. Generated code declares one beside each slice var. */
static inline void aether_slice_var_lock(atomic_flag* lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) AETHER_CPU_PAUSE();
}

static inline void aether_slice_var_unlock(atomic_flag* lock) {
    atomic_flag_clear_explicit(lock, memory_order_release);
}

/* A read of the var at `place`: its slice, read whole under the lock, with a
 * reference of its own taken there, so no store frees what the reader holds
 * and none tears the slice it sees. The var's slice was published by its
 * store, so the count is atomic. */
static inline AetherSlice aether_slice_var_take(const AetherSlice* place, atomic_flag* lock) {
    aether_slice_var_lock(lock);
    AetherSlice s = *place;
    aether_slice_retain(s);
    aether_slice_var_unlock(lock);
    return s;
}

/* The var at `place` left empty, and its slice handed over with the
 * reference the var held (`free(g)`). */
static inline AetherSlice aether_slice_var_take_out(AetherSlice* place, atomic_flag* lock) {
    aether_slice_var_lock(lock);
    AetherSlice s = *place;
    *place = aether_slice_make(NULL, 0);
    aether_slice_var_unlock(lock);
    return s;
}

/* A store into the var at `place`: `v`, which holds a reference of its own,
 * published before any other thread can reach it, swapped in under the lock,
 * and what the var held released once the lock is let go. Two stores at once
 * each release the slice they replaced. */
static inline void aether_slice_var_store(AetherSlice* place, atomic_flag* lock, AetherSlice v) {
    aether_slice_publish(v);
    aether_slice_var_lock(lock);
    AetherSlice old = *place;
    *place = v;
    aether_slice_var_unlock(lock);
    aether_slice_unref(old);
}

/* #3072: `free(s)` of a place: the buffer `make([]T, n)` handed out, a view
 * of one, or a pointer the program owns viewed from its start. A counted
 * slice gives back the place's reference (#3568), the buffer going with the
 * last, so a view keeps what it views. An uncounted view owns nothing (R1,
 * R2): `free` of a read-only one (string.bytes) or of one of an array's
 * elements is a named panic, where the allocator was given a pointer it never
 * handed out (heap corruption) or one its owner still holds (a double free
 * later). The element type's drop comes from the buffer. A string's bytes
 * are read-only whether the view holds the string or, where the string's
 * header does not fit its offset bits, a counted copy of them: `free` of
 * either is the same named panic on every target. */
static inline void aether_slice_free(AetherSlice s, const char* file, int line) {
    if (s.flags & AETHER_SLICE_READONLY)
        aether_slice_panicf("%s:%d: free of a read-only view of a string, which owns nothing",
                            file, line);
    if (s.flags & AETHER_SLICE_MADE) {
        aether_slice_unref(s);
        return;
    }
    if (s.flags & AETHER_SLICE_BORROWED)
        aether_slice_panicf("%s:%d: free of a view of elements another slice or an array "
                            "holds, which owns nothing", file, line);
    free(s.ptr);
}

/* `free(v)` of a value its place holds no reference to: a field of a struct
 * parameter (its caller's), a view borrowed for the statement. A view of
 * elements something else holds (an array, a list pattern's tail) is the
 * named panic a view gets. A counted buffer or string gives back nothing:
 * the place that owns it releases it (R3: parameters borrow), where giving
 * back the owner's reference freed the buffer under it. Uncounted memory
 * the program viewed from its start is freed, as before. */
static inline void aether_slice_free_view(AetherSlice s, const char* file, int line) {
    if (s.flags & AETHER_SLICE_BORROWED)
        aether_slice_panicf("%s:%d: free of a view of elements another slice or an array "
                            "holds, which owns nothing", file, line);
    if (s.flags & (AETHER_SLICE_MADE | AETHER_SLICE_STRING)) return;
    aether_slice_free(s, file, line);
}

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SLICE_H */
