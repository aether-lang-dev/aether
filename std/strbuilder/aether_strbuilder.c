#include "aether_strbuilder.h"
#include "../mem/aether_grow.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define AETHER_STRBUILDER_DEFAULT_CAP 64

/* The builder writes into ONE block laid out as an inline AetherString --
 * [header][payload] -- from the first byte on, so `finish` can hand the
 * block over as a length-carrying string without copying it: it fills in
 * the header and returns the block. `data` points at the payload, just
 * past the header, and is re-derived after every realloc.
 *
 * It used to be a bare buffer returned as a header-less char*. That was
 * correct, and quadratic to read: every `string.char_at`, `substring` or
 * `length` on the result took the strlen fallback, so a character-by-
 * character scan of a finished string was O(n) calls x O(n) strlen --
 * instant on a test input, a hang on a production one, with the right
 * answer throughout (asks/strbuilder-finish-headerless-string-is-on2-to-
 * scan.md). With the header in place those are O(1), and the result is
 * freed by whoever owns it exactly as before: the codegen's heap tracker
 * already dispatches on the magic header (string_release for a string,
 * free for a buffer), and string_release recognises the inline layout by
 * position and releases the block whole. `capacity` is the payload
 * capacity; the block is sizeof(AetherString) + capacity bytes, which is
 * also what string_release credits back. */
struct AetherStrBuilder {
    AetherString* block; /* NULL until the first reserve */
    char*  data;         /* (char*)(block + 1) */
    size_t length;
    size_t capacity;     /* payload bytes available after the header */
};

/* Grow the block so the payload has at least `min_capacity` bytes.
 * Doubles each step to keep amortised-O(1) append cost; falls back to
 * `min_capacity` directly once doubling stops being enough or would
 * overflow. Returns 1 on success, 0 on OOM / cap exceeded. */
static int strbuilder_reserve(AetherStrBuilder* b, size_t min_capacity) {
    if (!b) return 0;
    if (b->capacity >= min_capacity) return 1;
    size_t new_cap = aether_buf_grow_capacity(b->capacity,
                                              AETHER_STRBUILDER_DEFAULT_CAP,
                                              min_capacity, 1);
    if (!new_cap) return 0;
    if (new_cap > (size_t)-1 - sizeof(AetherString)) return 0;
    AetherString* nb = (AetherString*)aether_caps_realloc(
        b->block,
        b->block ? sizeof(AetherString) + b->capacity : 0,
        sizeof(AetherString) + new_cap);
    if (!nb) return 0;
    b->block = nb;
    b->data = (char*)(nb + 1);
    b->capacity = new_cap;
    return 1;
}

AetherStrBuilder* aether_strbuilder_new(int cap_hint) {
    AetherStrBuilder* b = (AetherStrBuilder*)aether_caps_malloc(sizeof(AetherStrBuilder));
    if (!b) return NULL;
    b->block = NULL;
    b->data = NULL;
    b->length = 0;
    b->capacity = 0;
    size_t cap = (cap_hint > 0) ? (size_t)cap_hint : AETHER_STRBUILDER_DEFAULT_CAP;
    if (!strbuilder_reserve(b, cap)) {
        aether_caps_free(b, sizeof(AetherStrBuilder));
        return NULL;
    }
    return b;
}

int aether_strbuilder_append(AetherStrBuilder* b, const void* s) {
    if (!b || !s) return 0;
    /* Length comes from the AetherString header if present, else
     * strlen — meaning content with embedded NULs handed in as a
     * plain `const char*` will truncate. Callers who need binary-safe
     * append should use append_n. */
    size_t n = is_aether_string(s)
        ? aether_string_length(s)
        : strlen((const char*)s);
    return aether_strbuilder_append_n(b, s, (int)n);
}

int aether_strbuilder_append_n(AetherStrBuilder* b, const void* s, int n) {
    if (!b || !s) return 0;
    if (n < 0) return 0;
    if (n == 0) return 1;
    size_t need = b->length + (size_t)n;
    if (need < b->length) return 0;  /* overflow */
    if (!strbuilder_reserve(b, need)) return 0;
    const char* payload = aether_string_data(s);
    if (!payload) return 0;
    memcpy(b->data + b->length, payload, (size_t)n);
    b->length = need;
    return 1;
}

int aether_strbuilder_append_byte(AetherStrBuilder* b, int c) {
    if (!b) return 0;
    size_t need = b->length + 1;
    if (!strbuilder_reserve(b, need)) return 0;
    b->data[b->length] = (char)(c & 0xff);
    b->length = need;
    return 1;
}

int aether_strbuilder_append_int(AetherStrBuilder* b, int v) {
    if (!b) return 0;
    char numbuf[32];
    int nlen = snprintf(numbuf, sizeof(numbuf), "%d", v);
    if (nlen < 0 || nlen >= (int)sizeof(numbuf)) return 0;
    return aether_strbuilder_append_n(b, numbuf, nlen);
}

int aether_strbuilder_append_long(AetherStrBuilder* b, long long v) {
    if (!b) return 0;
    char numbuf[32];  /* INT64_MIN is 20 chars + sign + NUL — fits */
    int nlen = snprintf(numbuf, sizeof(numbuf), "%lld", v);
    if (nlen < 0 || nlen >= (int)sizeof(numbuf)) return 0;
    return aether_strbuilder_append_n(b, numbuf, nlen);
}

int aether_strbuilder_append_hex(AetherStrBuilder* b, long long v, int width) {
    if (!b) return 0;
    if (width < 0) return 0;
    /* `%0*llx`: width is a MINIMUM field width — a value needing more
     * nibbles than `width` grows the output rather than truncating
     * (issue #489's resolution of the width=2 question). width=0 is
     * natural width. Hex of a negative value uses the unsigned bit
     * pattern, matching C's printf and every hex-dump convention. */
    char numbuf[80];  /* 16 nibbles for 64-bit + generous pad headroom */
    int nlen = snprintf(numbuf, sizeof(numbuf), "%0*llx",
                        width, (unsigned long long)v);
    if (nlen < 0 || nlen >= (int)sizeof(numbuf)) return 0;
    return aether_strbuilder_append_n(b, numbuf, nlen);
}

int aether_strbuilder_append_codepoint(AetherStrBuilder* b, int cp) {
    if (!b) return 0;
    /* Reject what UTF-8 cannot legally encode: negatives, the
     * UTF-16 surrogate range (0xD800–0xDFFF — never valid scalar
     * values), and anything past the Unicode ceiling 0x10FFFF. */
    if (cp < 0 || cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    unsigned char enc[4];
    int n;
    if (cp <= 0x7F) {
        enc[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        enc[0] = (unsigned char)(0xC0 | (cp >> 6));
        enc[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        enc[0] = (unsigned char)(0xE0 | (cp >> 12));
        enc[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        enc[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        enc[0] = (unsigned char)(0xF0 | (cp >> 18));
        enc[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        enc[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        enc[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return aether_strbuilder_append_n(b, (const char*)enc, n);
}

int aether_strbuilder_vappend_format(AetherStrBuilder* b, const char* fmt,
                                     va_list ap) {
    if (!b || !fmt) return 0;
    /* Fast path: format into a stack scratch buffer. vsnprintf always
     * reports the length it WOULD have written, so a single probe
     * tells us whether the scratch was large enough.
     *
     * The probe runs against a va_copy of `ap`, not `ap` itself: a
     * va_list is single-pass on most ABIs, and the slow path below
     * needs a fresh traversal to re-format. The caller's `ap` is
     * still consumed by the time this returns (see the slow path);
     * that is the documented vsnprintf-style contract. */
    char scratch[256];
    va_list ap_probe;
    va_copy(ap_probe, ap);
    int needed = vsnprintf(scratch, sizeof(scratch), fmt, ap_probe);
    va_end(ap_probe);
    if (needed < 0) return 0;  /* encoding error */
    if (needed < (int)sizeof(scratch)) {
        return aether_strbuilder_append_n(b, scratch, needed);
    }
    /* Slow path: output exceeded the scratch. Reserve room in the
     * builder for `needed` bytes plus a NUL (vsnprintf always writes
     * a terminator), format directly into the tail using the
     * caller's `ap`, then bump the logical length by `needed` only —
     * the NUL is not part of the content. */
    size_t end = b->length + (size_t)needed;
    if (end < b->length) return 0;  /* overflow */
    if (!strbuilder_reserve(b, end + 1)) return 0;
    int wrote = vsnprintf(b->data + b->length, (size_t)needed + 1, fmt, ap);
    if (wrote != needed) return 0;
    b->length = end;
    return 1;
}

int aether_strbuilder_append_format(AetherStrBuilder* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = aether_strbuilder_vappend_format(b, fmt, ap);
    va_end(ap);
    return rc;
}

int aether_strbuilder_length(AetherStrBuilder* b) {
    if (!b) return -1;
    return (int)b->length;
}

int aether_strbuilder_capacity(AetherStrBuilder* b) {
    if (!b) return -1;
    return (int)b->capacity;
}

int aether_strbuilder_reserve(AetherStrBuilder* b, int additional) {
    if (!b) return 0;
    if (additional < 0) return 0;
    if (additional == 0) return 1;
    size_t need = b->length + (size_t)additional;
    if (need < b->length) return 0;  /* overflow */
    return strbuilder_reserve(b, need);
}

int aether_strbuilder_truncate(AetherStrBuilder* b, int new_length) {
    if (!b) return 0;
    /* Pop content back to `new_length` bytes. Only ever shrinks — a
     * `new_length` past the current length would expose uninitialised
     * capacity bytes as content, so that is rejected rather than
     * silently grown. The buffer itself is kept (capacity unchanged)
     * so a truncate-then-append reuse pattern keeps the doubling
     * amortisation across loop iterations. */
    if (new_length < 0) return 0;
    if ((size_t)new_length > b->length) return 0;
    b->length = (size_t)new_length;
    return 1;
}

int aether_strbuilder_clear(AetherStrBuilder* b) {
    if (!b) return 0;
    b->length = 0;
    return 1;
}

void* aether_strbuilder_finish(AetherStrBuilder* b) {
    if (!b) return NULL;
    /* Hand the block over as a length-carrying string: NUL-terminate the
     * payload, fill in the header the block has carried since its first
     * reserve, free only the wrapper. No copy. The caller owns the result
     * under the @heap contract and the codegen's heap tracker releases it
     * through string_release, which frees the block whole. Embedded NULs
     * survive too, since every string-aware consumer reads `length`. */
    if (!b->block) {
        /* An empty builder: mint the smallest inline string. */
        AetherString* empty = string_alloc_inline(0);
        aether_caps_free(b, sizeof(AetherStrBuilder));
        return empty;
    }
    /* Grow to fit the NUL terminator if the payload is exactly full. */
    if (b->length + 1 > b->capacity) {
        if (!strbuilder_reserve(b, b->length + 1)) {
            aether_caps_free(b->block, sizeof(AetherString) + b->capacity);
            aether_caps_free(b, sizeof(AetherStrBuilder));
            return NULL;
        }
    }
    b->data[b->length] = '\0';
    AetherString* out = b->block;
    out->magic = AETHER_STRING_MAGIC;
    out->ref_count = 1;
    out->length = b->length;
    out->capacity = b->capacity;
    out->data = b->data;
    aether_caps_free(b, sizeof(AetherStrBuilder));
    return out;
}

_tuple_ptr_int aether_strbuilder_finish_with_length(AetherStrBuilder* b) {
    /* Binary-safe finalise: hand back a raw data buffer and its exact
     * byte length, with NO NUL terminator appended. This is the shape
     * binary protocol assembly needs (CBOR / msgpack / frame encoders)
     * where embedded NULs are content, not terminators. The caller owns
     * the returned pointer and frees it with a plain libc free(); it is
     * NOT heap-tracked by the codegen (position 0 is `ptr`, not
     * `string`).
     *
     * The block carries an inline-string header ahead of the payload
     * (see the struct comment), and a buffer the caller will free() has
     * to start at the allocation, so the payload is shifted down over
     * the header first. One memmove of the content, no allocation; the
     * few bytes of header at the tail are slack. After this call `b` is
     * invalid. */
    if (!b) {
        _tuple_ptr_int err = { NULL, -1 };
        return err;
    }
    if (!b->block) {
        /* Empty builder -- return a non-null 1-byte buffer with
         * length 0 so the caller's free() has something valid to
         * reclaim and never sees a NULL data pointer for a
         * successful finish. */
        char* empty = (char*)aether_caps_malloc(1);
        aether_caps_free(b, sizeof(AetherStrBuilder));
        if (empty) empty[0] = '\0';
        _tuple_ptr_int out = { empty, empty ? 0 : -1 };
        return out;
    }
    char* raw = (char*)b->block;
    memmove(raw, b->data, b->length);
    _tuple_ptr_int out = { raw, (int)b->length };
    aether_caps_free(b, sizeof(AetherStrBuilder));
    return out;
}

void aether_strbuilder_free(AetherStrBuilder* b) {
    if (!b) return;
    if (b->block) aether_caps_free(b->block, sizeof(AetherString) + b->capacity);
    aether_caps_free(b, sizeof(AetherStrBuilder));
}
