/* aether_string_inline.h: a string's length and one byte of it, as `static
 * inline` (#3836).
 *
 * std.string asks for this header with `@c_include` (the #1986 mechanism),
 * so in a program that imports it `string.length(s)` and
 * `string.char_at(s, i)` are the header reads they name rather than calls
 * the C compiler cannot see through: `while i < string.length(s)` made two
 * calls per character, each checking the magic, and GCC hoisted neither.
 * A 138-character scan took 515 ns out of line and 170 ns inline.
 *
 * ONE BODY. aether_string.h includes this header too, and the exported
 * string_length / string_char_at in aether_string.c are these functions,
 * so C that calls the exported names and an Aether program that calls the
 * inline ones read a string the same way: null is 0 long and has no bytes,
 * an index out of range reads 0, a byte is unsigned, and a length past
 * INT_MAX panics (a cold call of aether_panic).
 *
 * Injected into the translation unit of every program that imports
 * std.string, so it declares nothing that is not prefixed (the lesson of
 * #2162): the layout as a struct of its own name, and aether_string_*
 * functions. These are borrowed reads of a string the caller holds; they
 * take and drop no reference. */
#ifndef AETHER_STRING_INLINE_H
#define AETHER_STRING_INLINE_H

#include <stddef.h>   /* size_t */
#include <string.h>   /* strlen, for a bare C string */
#include <limits.h>   /* INT_MAX */

#include "aether_string_abi.h"

/* The header of a counted string, as aether_string.h's AetherString. */
struct aether_string_hdr {
    AETHER_STRING_FIELDS
};

/* A string's count, read where another thread may be changing a shared
 * string's count at the same time (atomically, see
 * runtime/utils/aether_count.h), so the read is atomic too. Relaxed:
 * aether_string_is_counted looks only at its sign. MSVC gets a volatile
 * read, which it does not tear. */
static inline int aether_string_count_now(const int* p) {
#if defined(_MSC_VER) && !defined(__clang__)
    return *(const volatile int*)p;
#else
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#endif
}

/* Is `ptr` a counted AetherString (rather than a bare char*)?
 *
 * Reads bytes from the pointer to look for the magic header. Done
 * byte-by-byte with short-circuit so that for the 99.6% of inputs
 * whose first byte mismatches, we read exactly one byte, keeping
 * the dispatch ASan-clean on short string allocations like "hi" (2
 * bytes) or "x" (1 byte) where a naive `s->magic` 4-byte read
 * overshoots the allocation.
 *
 * On a confirmed magic match, also validate that the rest of the
 * header looks like a real AetherString (non-negative ref_count,
 * capacity >= length, non-NULL data) to harden against the 1-in-2^32
 * chance that arbitrary content happens to begin with the magic
 * bytes. This catches spurious dispatch before we deref s->data. */
static inline int aether_string_is_counted(const void* ptr) {
    if (!ptr) return 0;
    const unsigned char* p = (const unsigned char*)ptr;
    /* Magic 0xAE57C0DE in little-endian = DE C0 57 AE. */
    if (p[0] != 0xDE) return 0;
    if (p[1] != 0xC0) return 0;
    if (p[2] != 0x57) return 0;
    if (p[3] != 0xAE) return 0;
    /* All four bytes matched. Validate the header for plausibility
     * to reject coincidental magic-bytes-in-payload before any
     * downstream s->data deref. */
    const struct aether_string_hdr* s = (const struct aether_string_hdr*)ptr;
    if (aether_string_count_now(&s->ref_count) < 0) return 0;
    if (s->capacity < s->length) return 0;
    if (!s->data) return 0;
    return 1;
}

/* The bytes of a counted string or a bare char*; "" for null. */
static inline const char* aether_string_bytes(const void* s) {
    if (!s) return "";
    if (aether_string_is_counted(s)) return ((const struct aether_string_hdr*)s)->data;
    return (const char*)s;
}

/* The stored length of a counted string, strlen of a bare char*, 0 for
 * null. */
static inline size_t aether_string_byte_count(const void* s) {
    if (!s) return 0;
    if (aether_string_is_counted(s)) return ((const struct aether_string_hdr*)s)->length;
    return strlen((const char*)s);
}

/* The runtime's panic (runtime/actors/aether_panic.h), declared here as it
 * is there. A length past INT_MAX, which no int the string API returns can
 * hold, panics with it (#3523).
 *
 * This header is compiled into every program and library that imports
 * std.string, so what it calls is part of what such a library needs from
 * the runtime it loads on. It calls nothing newer than the runtime ABI it
 * is built against already exports: a library is linked to bind every
 * symbol at load (-z now), and one that named a function an older shared
 * runtime lacks was refused by the loader before its preflight could say
 * which runtime it wants (abi_matrix, docs/abi-policy.md). */
void aether_panic(const char* reason);

/* string.length: the length in bytes. */
static inline int aether_string_length_inline(const void* s) {
    size_t n = aether_string_byte_count(s);
    if (n > (size_t)INT_MAX) {
        aether_panic("string: a length or position past 2 GiB does not fit an int");
        return 0;
    }
    return (int)n;
}

/* string.char_at: the byte at `index`, 0..255, or 0 when `s` is null or
 * `index` is out of range. Unsigned: a byte >= 0x80 used to come back
 * sign-extended, so 0x85 read as -123 and every binary parser written
 * against this silently broke on high bytes (#1516). The length is
 * compared as a size_t (#3523): `(int)len` of a string over 2 GiB is
 * negative, and every index read as out of range. */
static inline int aether_string_char_at_inline(const void* s, int index) {
    if (!s || index < 0) return 0;
    /* The header is asked once: its count is read atomically, and the C
     * compiler merges no two such reads, so a second test (the length,
     * then the bytes) was a second walk of the magic per byte. */
    const char* bytes;
    size_t len;
    if (aether_string_is_counted(s)) {
        bytes = ((const struct aether_string_hdr*)s)->data;
        len = ((const struct aether_string_hdr*)s)->length;
    } else {
        bytes = (const char*)s;
        len = strlen(bytes);
    }
    if ((size_t)index >= len) return 0;
    return (int)(unsigned char)bytes[index];
}

#endif
