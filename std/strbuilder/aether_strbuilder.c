#include "aether_strbuilder.h"
#include "../mem/aether_grow.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define AETHER_STRBUILDER_DEFAULT_CAP 64

struct AetherStrBuilder {
    char*  data;
    size_t length;
    size_t capacity;
};

/* Grow the underlying buffer so it has at least `min_capacity` bytes.
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
    char* new_data = (char*)aether_caps_realloc(b->data, b->capacity, new_cap);
    if (!new_data) return 0;
    b->data = new_data;
    b->capacity = new_cap;
    return 1;
}

AetherStrBuilder* aether_strbuilder_new(int cap_hint) {
    AetherStrBuilder* b = (AetherStrBuilder*)aether_caps_malloc(sizeof(AetherStrBuilder));
    if (!b) return NULL;
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
    /* Hand the data buffer off to the caller as a plain libc-freeable
     * char* and free only the wrapper. This matches the @heap-extern
     * contract used by the rest of the stdlib (cf. make_owned in
     * tests/integration/extern_single_value_heap): the
     * heap-string-tracker's reassignment-wrapper emits a plain libc
     * free on the previous value, which on an AetherString* would
     * free the 24-byte header struct and dangle the data buffer
     * (the uniform-heap return shim passes is_heap=1 through
     * unchanged for heap-flagged returns, so we can't piggy-back on
     * its struct-aware path either). A plain char* sidesteps both:
     * libc-free reclaims the whole allocation, the cap drift on the
     * single transfer is acceptable (caps API explicitly allows
     * libc-free of caps-malloc'd memory, header-comment in
     * runtime/aether_resource_caps.h:89-94).
     *
     * Trade-off: the returned char* has no length-bearing header, so
     * downstream operations that use strlen will truncate at the
     * first NUL. Binary content with embedded NULs needs append_n
     * on the input side AND length-aware consumers on the output
     * side (e.g. string_length_n, string_substring_n) if the caller
     * carries the length themselves. The ASCII / UTF-8 case — JSON,
     * log lines, templates, paths — is the overwhelming majority of
     * the motivating use cases.
     *
     * PERFORMANCE trap (not just the NUL angle): because the buffer
     * has no length header, str_len() takes its strlen() fallback
     * every call, so string.char_at / string.substring / string.length
     * are each O(n) on a finish()'d string. A char-by-char scan is
     * therefore O(n²) — instant on a test input, a hang on a
     * production one, with correct results throughout (so it never
     * looks like a bug). Scanning-heavy consumers (tokenizers,
     * interpreters, template renderers) should take the length ONCE
     * and use the length-carrying accessors — string_char_at_n(s,
     * len, i), string_substring_n(s, len, a, b) — or finish with
     * finish_with_length() and thread that length through. See
     * std/strbuilder/README.md. */
    if (!b->data) {
        char* empty = (char*)aether_caps_malloc(1);
        aether_caps_free(b, sizeof(AetherStrBuilder));
        if (!empty) return NULL;
        empty[0] = '\0';
        return empty;
    }
    /* Grow to fit the NUL terminator if the buffer is exactly full. */
    if (b->length + 1 > b->capacity) {
        if (!strbuilder_reserve(b, b->length + 1)) {
            aether_caps_free(b->data, b->capacity);
            aether_caps_free(b, sizeof(AetherStrBuilder));
            return NULL;
        }
    }
    b->data[b->length] = '\0';
    char* out = b->data;
    aether_caps_free(b, sizeof(AetherStrBuilder));
    return out;
}

_tuple_ptr_int aether_strbuilder_finish_with_length(AetherStrBuilder* b) {
    /* Binary-safe finalise: hand back the raw data buffer and its
     * exact byte length, with NO NUL terminator appended. This is the
     * shape binary protocol assembly needs (CBOR / msgpack / frame
     * encoders) where embedded NULs are content, not terminators —
     * the v1 `finish` would round-trip them but a strlen-based
     * consumer downstream would truncate. The caller owns the
     * returned pointer and frees it with a plain libc free(); it is
     * NOT heap-tracked by the codegen (position 0 is `ptr`, not
     * `string`). Same buffer-handoff + free-the-wrapper-only
     * discipline as `finish`. After this call `b` is invalid. */
    if (!b) {
        _tuple_ptr_int err = { NULL, -1 };
        return err;
    }
    if (!b->data) {
        /* Empty builder — return a non-null 1-byte buffer with
         * length 0 so the caller's free() has something valid to
         * reclaim and never sees a NULL data pointer for a
         * successful finish. */
        char* empty = (char*)aether_caps_malloc(1);
        aether_caps_free(b, sizeof(AetherStrBuilder));
        if (empty) empty[0] = '\0';
        _tuple_ptr_int out = { empty, empty ? 0 : -1 };
        return out;
    }
    _tuple_ptr_int out = { b->data, (int)b->length };
    aether_caps_free(b, sizeof(AetherStrBuilder));
    return out;
}

void aether_strbuilder_free(AetherStrBuilder* b) {
    if (!b) return;
    aether_caps_free(b->data, b->capacity);
    aether_caps_free(b, sizeof(AetherStrBuilder));
}
