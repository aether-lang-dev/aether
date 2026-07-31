#include "aether_string.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>  // SIZE_MAX (not in <limits.h> on MinGW)

#ifndef _WIN32
#include <fnmatch.h>  // POSIX glob-pattern matching (string_glob_match_raw)
#endif

// Helper: get data pointer and length from either AetherString* or plain char*
static inline const char* str_data(const void* s) {
    if (!s) return "";
    if (is_aether_string(s)) return ((const AetherString*)s)->data;
    return (const char*)s;
}

static inline size_t str_len(const void* s) {
    if (!s) return 0;
    if (is_aether_string(s)) return ((const AetherString*)s)->length;
    return strlen((const char*)s);
}

// Alias for string literal creation
AetherString* string_from_literal(const char* cstr) {
    return string_new(cstr);
}

// Alias for from_cstr
AetherString* string_from_cstr(const char* cstr) {
    /* #952: `from_cstr` may be handed a magic AetherString* rather than a
     * raw C string — e.g. a value stored with list.add_string_owned (which
     * keeps the AetherString header) and read back via list.get. A naive
     * string_new(cstr) would read the 24-byte header as character data and
     * copy garbage (or crash dereferencing it). str_data() detects the magic
     * header and yields the real payload bytes for either shape (and "" for
     * NULL), so the round-trip is correct whichever the caller holds. */
    return string_new(str_data(cstr));
}

// Alias for free
void string_free(const void* str) {
    string_release(str);
}

// String creation
AetherString* string_new(const char* cstr) {
    if (!cstr) return string_empty();
    return string_new_with_length(cstr, strlen(cstr));
}

AetherString* string_new_with_length(const char* data, size_t length) {
    /* Issue #343: cap-aware allocation. Both the struct and the data
     * buffer are accounted; string_release frees both with their
     * recorded sizes (sizeof(AetherString) and capacity), keeping
     * the cap counter at current-usage rather than high-water-mark. */
    AetherString* str = (AetherString*)aether_caps_malloc(sizeof(AetherString));
    if (!str) return NULL;
    char* buf = (char*)aether_caps_malloc(length + 1);
    if (!buf) { aether_caps_free(str, sizeof(AetherString)); return NULL; }
    str->magic = AETHER_STRING_MAGIC;
    str->length = length;
    str->capacity = length + 1;
    str->data = buf;
    if (data && length) memcpy(buf, data, length);
    buf[length] = '\0';
    str->ref_count = 1;
    return str;
}

AetherString* string_empty() {
    return string_new_with_length("", 0);
}

/* Adopt an aether_caps_malloc'd buffer (`cap` bytes, holding `length`
 * payload bytes + a NUL at [length]) into a fresh magic AetherString
 * WITHOUT copying. The buffer MUST have come from aether_caps_malloc —
 * string_release reclaims it via aether_caps_free(data, capacity). On
 * struct-alloc failure the buffer is freed and NULL returned. This is
 * the zero-copy mint the string ops (concat/substring/upper/lower/trim)
 * use so they yield magic AetherStrings without the extra copy
 * string_new_with_length would make. */
static AetherString* string_adopt_caps_buffer(char* buf, size_t length, size_t cap) {
    if (!buf) return NULL;
    AetherString* s = (AetherString*)aether_caps_malloc(sizeof(AetherString));
    if (!s) { aether_caps_free(buf, cap); return NULL; }
    s->magic = AETHER_STRING_MAGIC;
    s->ref_count = 1;
    s->length = length;
    s->capacity = cap;
    s->data = buf;
    return s;
}

// Reference counting — safe to call with plain char* (no-op)
void string_retain(const void* str) {
    if (str && is_aether_string(str)) ((AetherString*)str)->ref_count++;
}

void string_release(const void* str) {
    if (!str || !is_aether_string(str)) return;
    AetherString* s = (AetherString*)str;
    s->ref_count--;
    if (s->ref_count <= 0) {
        /* Cap accounting: data buffer was allocated with `capacity`
         * bytes (length + 1 NUL terminator); struct is one
         * sizeof(AetherString). Both pair with the alloc-side
         * accounting in string_new_with_length. */
        aether_caps_free(s->data, s->capacity);
        aether_caps_free(s, sizeof(AetherString));
    }
}

// String operations
// Returns a magic AetherString* (refcounted, length-aware) — the unified
// owned-heap string representation. Freed via string_release (dispatched
// by aether_heap_str_free on the magic header); binary-safe (embedded
// NULs preserved via the stored length). One zero-copy direct mint:
// allocate the data buffer with aether_caps_malloc, write both halves,
// adopt into an AetherString (no intermediate copy).
AetherString* string_concat(const void* a, const void* b) {
    if (!a || !b) return NULL;
    size_t la = str_len(a), lb = str_len(b);
    // Guard against size_t overflow on pathological inputs before
    // adding 1 for the null terminator.
    if (la > SIZE_MAX - lb - 1) return NULL;
    size_t new_length = la + lb;
    char* buf = (char*)aether_caps_malloc(new_length + 1);
    if (!buf) return NULL;
    if (la) memcpy(buf, str_data(a), la);
    if (lb) memcpy(buf + la, str_data(b), lb);
    buf[new_length] = '\0';
    return string_adopt_caps_buffer(buf, new_length, new_length + 1);
}

// Length-bearing variant of string_concat. Returns an AetherString*
// (refcounted, length-aware) rather than a bare char* — callers that
// later run `string.length(result)` on this value read the stored
// length from the magic-dispatch path rather than falling through to
// strlen() and silently truncating at the first embedded NUL.
//
// Use this when the inputs may contain binary bytes (base64-decoded
// payloads, file content from fs.read_binary, message frames with
// length-prefix bytes, …). For ASCII-text accumulation in print /
// interpolation contexts the plain `string_concat` is fine. See #270.
AetherString* string_concat_wrapped(const void* a, const void* b) {
    if (!a || !b) return NULL;
    size_t la = str_len(a), lb = str_len(b);
    const char* da = str_data(a);
    const char* db = str_data(b);

    if (la > SIZE_MAX - lb - 1) return NULL;
    size_t new_length = la + lb;

    char* joined = (char*)malloc(new_length + 1);
    if (!joined) return NULL;
    if (la) memcpy(joined, da, la);
    if (lb) memcpy(joined + la, db, lb);
    joined[new_length] = '\0';

    AetherString* wrapped = string_new_with_length(joined, new_length);
    free(joined);
    return wrapped;
}

int string_length(const void* str) {
    return (int)str_len(str);
}

char string_char_at(const void* str, int index) {
    size_t len = str_len(str);
    if (!str || index < 0 || index >= (int)len) return '\0';
    return str_data(str)[index];
}

int string_equals(const void* a, const void* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    size_t la = str_len(a), lb = str_len(b);
    if (la != lb) return 0;
    return memcmp(str_data(a), str_data(b), la) == 0;
}

int string_compare(const void* a, const void* b) {
    if (!a || !b) return 0;
    return strcmp(str_data(a), str_data(b));
}

// String methods
//
// `prefix`/`suffix` accept either a magic AetherString* or a plain
// `const char*` — like `str`, dispatched through str_data/str_len.
// A naive strlen(prefix) would read 16 bytes of struct header from a
// magic AetherString and return the wrong length (or NUL-truncate at
// the first zero byte in the header). Same hazard as
// string_glob_match_raw; same fix.
int string_starts_with(const void* str, const void* prefix) {
    if (!str || !prefix) return 0;
    size_t prefix_len = str_len(prefix);
    size_t slen = str_len(str);
    if (prefix_len > slen) return 0;
    return memcmp(str_data(str), str_data(prefix), prefix_len) == 0;
}

int string_ends_with(const void* str, const void* suffix) {
    if (!str || !suffix) return 0;
    size_t suffix_len = str_len(suffix);
    size_t slen = str_len(str);
    if (suffix_len > slen) return 0;
    return memcmp(str_data(str) + (slen - suffix_len),
                  str_data(suffix), suffix_len) == 0;
}

int string_contains(const void* str, const void* substring) {
    return string_index_of(str, substring) >= 0;
}

int string_index_of(const void* str, const void* substring) {
    if (!str || !substring) return -1;
    // Needle is binary-aware too: accepts AetherString* (for
    // embedded NULs) or plain char*. Before this, passing an
    // AetherString needle ran strlen past the struct header and
    // returned garbage lengths — matters for packed-string
    // protocols that use string_from_char(2) as a separator.
    size_t sub_len = str_len(substring);
    const char* sub_data = str_data(substring);
    size_t slen = str_len(str);
    const char* sdata = str_data(str);
    if (sub_len > slen) return -1;

    for (size_t i = 0; i <= slen - sub_len; i++) {
        if (memcmp(sdata + i, sub_data, sub_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Like string_index_of but starts the scan at byte offset `start`.
// Returns the absolute offset of the hit (not relative to `start`),
// or -1 on miss. Negative `start` is clamped to 0; `start` past the
// end returns -1.
//
// Both haystack and needle are binary-aware (accept AetherString*
// or plain char*). Common idiom: scanning for the next record
// separator in a multi-record packed string. Before this API,
// callers did `substring(s, start, length) + index_of(tail, needle)
// + start` which allocates a fresh copy of the tail on each step.
int string_index_of_from(const void* str, const void* substring, int start) {
    if (!str || !substring) return -1;
    size_t sub_len = str_len(substring);
    const char* sub_data = str_data(substring);
    size_t slen = str_len(str);
    const char* sdata = str_data(str);
    if (start < 0) start = 0;
    if ((size_t)start > slen) return -1;
    if (sub_len > slen - (size_t)start) return -1;

    for (size_t i = (size_t)start; i <= slen - sub_len; i++) {
        if (memcmp(sdata + i, sub_data, sub_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Construct a 1-byte AetherString whose single byte is `code & 0xff`.
// Fills the gap where callers want to encode a known ASCII / low-byte
// marker (e.g. \x01, \x02) into a string without routing through a
// NUL-terminated literal. `code` is masked to the low 8 bits —
// higher bits are silently dropped (caller's responsibility not to
// pass >255 for legitimate single-byte values).
//
// The returned AetherString length is always 1, even when `code` is
// 0 (a NUL byte) — embedded NULs are preserved via
// string_new_with_length, not string_new.
AetherString* string_from_char(int code) {
    char byte = (char)(code & 0xff);
    return string_new_with_length(&byte, 1);
}

AetherString* string_substring(const void* str, int start, int end) {
    if (!str) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);
    if (start < 0) start = 0;
    if (end > (int)slen) end = (int)slen;
    if (start >= end) {
        char* empty = (char*)aether_caps_malloc(1);
        if (!empty) return NULL;
        empty[0] = '\0';
        return string_adopt_caps_buffer(empty, 0, 1);
    }

    size_t len = end - start;
    char* result = (char*)aether_caps_malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, sdata + start, len);
    result[len] = '\0';
    return string_adopt_caps_buffer(result, len, len + 1);
}

/* Shared engine for string_replace / string_replace_all. Replaces up
 * to `max_hits` non-overlapping, left-to-right occurrences of
 * `old_sub` with `new_sub` (max_hits < 0 = unlimited). Binary-aware
 * on all three inputs (AetherString* or char*). Always returns a
 * fresh managed string, so caller-side lifetimes match
 * string_substring: the input is never aliased or mutated.
 *
 * An empty `old_sub` returns a copy of `str` unchanged — matching
 * Go's strings.Replace guard — because "match the empty string at
 * every position" either infinite-loops or interleaves `new_sub`
 * between every byte; neither is what a caller ever wants.
 *
 * Two passes: count hits, then fill an exact-size buffer. No
 * realloc churn, one allocation regardless of hit count. */
static AetherString* string_replace_impl(const void* str, const void* old_sub,
                                         const void* new_sub, long max_hits) {
    if (!str) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);
    size_t old_len = old_sub ? str_len(old_sub) : 0;
    const char* old_data = old_sub ? str_data(old_sub) : NULL;
    size_t new_len = new_sub ? str_len(new_sub) : 0;
    const char* new_data = new_sub ? str_data(new_sub) : NULL;

    long hits = 0;
    if (old_len > 0 && old_len <= slen && max_hits != 0) {
        for (size_t i = 0; i + old_len <= slen; ) {
            if (memcmp(sdata + i, old_data, old_len) == 0) {
                hits++;
                i += old_len;
                if (max_hits > 0 && hits >= max_hits) break;
            } else {
                i++;
            }
        }
    }
    if (hits == 0) {
        return string_new_with_length(sdata, slen);
    }

    size_t out_len = slen + (size_t)hits * new_len - (size_t)hits * old_len;
    char* result = (char*)aether_caps_malloc(out_len + 1);
    if (!result) return NULL;

    size_t w = 0;
    long done = 0;
    for (size_t i = 0; i < slen; ) {
        if (done < hits && i + old_len <= slen &&
            memcmp(sdata + i, old_data, old_len) == 0) {
            memcpy(result + w, new_data, new_len);
            w += new_len;
            i += old_len;
            done++;
        } else {
            result[w++] = sdata[i++];
        }
    }
    result[out_len] = '\0';
    return string_adopt_caps_buffer(result, out_len, out_len + 1);
}

AetherString* string_replace(const void* str, const void* old_sub, const void* new_sub) {
    return string_replace_impl(str, old_sub, new_sub, 1);
}

AetherString* string_replace_all(const void* str, const void* old_sub, const void* new_sub) {
    return string_replace_impl(str, old_sub, new_sub, -1);
}

/* Length-aware sibling of string_substring — caller supplies the
 * source length explicitly. Reach for this when `str` arrives as a
 * `string` parameter at a function boundary (where #297's auto-
 * unwrap may have stripped the AetherString header) AND the content
 * may contain embedded NULs. The plain string_substring would call
 * str_len() → strlen() on the unwrapped data and silently truncate
 * at the first NUL.
 *
 * Trusts the caller's `str_len` parameter — does NOT consult the
 * AetherString header even if one happens to be present. start/end
 * are clamped to [0, str_len]. */
AetherString* string_substring_n(const void* str, int str_len_bytes, int start, int end) {
    if (!str) return NULL;
    if (str_len_bytes < 0) str_len_bytes = 0;
    /* Resolve to the payload through str_data so we work uniformly
     * for both AetherString* inputs (bytes.finish, string_concat_wrapped,
     * string_new_with_length, fs.read_binary, etc.) and bare char*
     * inputs (literals, the unwrapped-return string_concat). The
     * codegen's auto-unwrap pass at the call site is SKIPPED for
     * `string_*`-prefixed externs (is_stdlib_string_aware_extern), so
     * this function must do its own header dispatch.
     *
     * `str_len_bytes` stays authoritative for slice bounds — we do
     * NOT replace it with str_len(str). Callers may have tracked the
     * length themselves through binary content with embedded NULs,
     * which is the whole reason the `_n` variant exists. */
    const char* sdata = str_data(str);
    if (start < 0) start = 0;
    if (end > str_len_bytes) end = str_len_bytes;
    if (start >= end) {
        char* empty = (char*)aether_caps_malloc(1);
        if (!empty) return NULL;
        empty[0] = '\0';
        return string_adopt_caps_buffer(empty, 0, 1);
    }

    size_t len = (size_t)(end - start);
    char* result = (char*)aether_caps_malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, sdata + start, len);
    result[len] = '\0';
    return string_adopt_caps_buffer(result, len, len + 1);
}

/* Identity helper that documents intent: in code that receives a
 * `string` parameter plus an explicit length, the explicit length
 * IS the truth — don't consult the AetherString header. This
 * function exists so `n = string.length_n(s, n)` reads as "yes I
 * know my length" at the source level instead of looking like a
 * forgotten `string.length(s)` that would have truncated at NUL.
 * Pure no-op at the C level; clamps negative input to 0. */
int string_length_n(const void* str, int known_length) {
    (void)str;
    return (known_length < 0) ? 0 : known_length;
}

/* Length-aware char_at. Skips the strlen-bounds-check the bare
 * string_char_at pays on every call when `str` arrives as a plain
 * `const char*`. Filed in string-length-aware-accessors.md after
 * avn's bench profile showed __strlen_avx2 dominating CPU at the
 * slow batches. Trusts the caller's known_length absolutely — does
 * NOT consult an AetherString header even if one is present. Same
 * miss-return convention as string_char_at ('\0' for out-of-range
 * and NULL input). */
char string_char_at_n(const void* str, int known_length, int index) {
    if (!str) return '\0';
    if (known_length < 0) return '\0';
    if (index < 0 || index >= known_length) return '\0';
    return str_data(str)[index];
}

/* Length-aware index_of_from. Same motivation as string_char_at_n:
 * caller has already cached the haystack length. Needle is still
 * strlen'd (small / fixed). `start` clamped to [0, known_length];
 * `start` > known_length OR needle longer than the searchable
 * range returns -1. */
int string_index_of_from_n(const void* str, int known_length,
                           const char* substring, int start) {
    if (!str || !substring) return -1;
    if (known_length < 0) return -1;
    size_t sub_len = str_len(substring);
    const char* sub_data = str_data(substring);
    const char* sdata = str_data(str);
    if (start < 0) start = 0;
    if ((size_t)start > (size_t)known_length) return -1;
    if (sub_len > (size_t)known_length - (size_t)start) return -1;
    for (size_t i = (size_t)start; i <= (size_t)known_length - sub_len; i++) {
        if (memcmp(sdata + i, sub_data, sub_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

AetherString* string_to_upper(const void* str) {
    if (!str) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);

    char* new_data = (char*)aether_caps_malloc(slen + 1);
    if (!new_data) return NULL;
    for (size_t i = 0; i < slen; i++) {
        new_data[i] = toupper((unsigned char)sdata[i]);
    }
    new_data[slen] = '\0';
    return string_adopt_caps_buffer(new_data, slen, slen + 1);
}

AetherString* string_to_lower(const void* str) {
    if (!str) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);

    char* new_data = (char*)aether_caps_malloc(slen + 1);
    if (!new_data) return NULL;
    for (size_t i = 0; i < slen; i++) {
        new_data[i] = tolower((unsigned char)sdata[i]);
    }
    new_data[slen] = '\0';
    return string_adopt_caps_buffer(new_data, slen, slen + 1);
}

AetherString* string_trim(const void* str) {
    if (!str) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);

    size_t start = 0;
    size_t end = slen;

    while (start < slen && isspace((unsigned char)sdata[start])) start++;
    while (end > start && isspace((unsigned char)sdata[end - 1])) end--;

    size_t len = end - start;
    char* result = (char*)aether_caps_malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, sdata + start, len);
    result[len] = '\0';
    return string_adopt_caps_buffer(result, len, len + 1);
}

// Helper: free a partially-built array on OOM during split. Used on
// every early-exit path so no cleanup branch leaks memory.
//
// #463: cap-aware. `built` is how many elements were constructed
// (release each); `capacity` is how many the strings array was
// ALLOCATED for — distinct from `built` and from `arr->count`
// (which isn't finalized until split succeeds), so the matching
// caps_free byte count is `capacity * sizeof(AetherString*)`.
static void string_array_partial_free(AetherStringArray* arr,
                                      size_t built, size_t capacity) {
    if (!arr) return;
    if (arr->strings) {
        for (size_t k = 0; k < built; k++) {
            if (arr->strings[k]) string_release(arr->strings[k]);
        }
        aether_caps_free(arr->strings, capacity * sizeof(AetherString*));
    }
    aether_caps_free(arr, sizeof(AetherStringArray));
}

// String array operations
AetherStringArray* string_split(const void* str, const void* delimiter) {
    if (!str || !delimiter) return NULL;
    size_t slen = str_len(str);
    const char* sdata = str_data(str);
    size_t delim_len = str_len(delimiter);
    const char* delim_data = str_data(delimiter);

    /* #463: cap-aware. AetherStringArray shape is
     * {AetherString** strings; size_t count;}. The strings array's
     * allocated element count equals `arr->count` in every success
     * path, so string_array_free recovers the byte count from
     * arr->count; the OOM cleanup uses string_array_partial_free
     * which is passed the allocation capacity explicitly (count
     * isn't finalized yet). Paired with the string_seq_to_array
     * conversion in std/collections/aether_stringseq.c, which builds
     * the same layout and is freed by this same string_array_free. */
    AetherStringArray* arr = (AetherStringArray*)aether_caps_malloc(sizeof(AetherStringArray));
    if (!arr) return NULL;
    arr->count = 0;
    arr->strings = NULL;

    // Empty delimiter → one entry per byte.
    if (delim_len == 0) {
        if (slen == 0) return arr;
        arr->strings = (AetherString**)aether_caps_malloc(sizeof(AetherString*) * slen);
        if (!arr->strings) { aether_caps_free(arr, sizeof(AetherStringArray)); return NULL; }
        for (size_t i = 0; i < slen; i++) {
            AetherString* piece = string_new_with_length(sdata + i, 1);
            if (!piece) { string_array_partial_free(arr, i, slen); return NULL; }
            arr->strings[i] = piece;
        }
        arr->count = slen;
        return arr;
    }

    // Input shorter than the delimiter → one piece, the whole input.
    if (slen < delim_len) {
        arr->strings = (AetherString**)aether_caps_malloc(sizeof(AetherString*));
        if (!arr->strings) { aether_caps_free(arr, sizeof(AetherStringArray)); return NULL; }
        arr->strings[0] = string_new_with_length(sdata, slen);
        if (!arr->strings[0]) {
            aether_caps_free(arr->strings, sizeof(AetherString*));
            aether_caps_free(arr, sizeof(AetherStringArray));
            return NULL;
        }
        arr->count = 1;
        return arr;
    }

    // Count how many pieces we'll produce. At this point slen >= delim_len
    // so the loop bound `slen - delim_len` won't underflow.
    size_t count = 1;
    size_t upper = slen - delim_len;
    for (size_t i = 0; i <= upper; i++) {
        if (memcmp(sdata + i, delim_data, delim_len) == 0) {
            count++;
            i += delim_len - 1;
        }
    }

    arr->strings = (AetherString**)aether_caps_malloc(sizeof(AetherString*) * count);
    if (!arr->strings) { aether_caps_free(arr, sizeof(AetherStringArray)); return NULL; }

    size_t start = 0;
    size_t idx = 0;
    for (size_t i = 0; i <= upper; i++) {
        if (memcmp(sdata + i, delim_data, delim_len) == 0) {
            AetherString* piece = string_new_with_length(sdata + start, i - start);
            if (!piece) { string_array_partial_free(arr, idx, count); return NULL; }
            arr->strings[idx++] = piece;
            start = i + delim_len;
            i += delim_len - 1;
        }
    }
    // Tail piece (may be empty if input ends with the delimiter).
    AetherString* tail = string_new_with_length(sdata + start, slen - start);
    if (!tail) { string_array_partial_free(arr, idx, count); return NULL; }
    arr->strings[idx] = tail;
    arr->count = count;

    return arr;
}

int string_array_size(AetherStringArray* arr) {
    return arr ? (int)arr->count : 0;
}

// Returns the raw C string data (const char*) for the element at index.
// Aether treats strings as const char* — returning AetherString* would cause
// printf("%s", ...) to print garbage (struct pointer instead of char data).
//
// LIFETIME: returned pointer is BORROWED — it aliases arr->strings[index]->data
// and is invalidated by string_array_free(arr). Aether-side callers that need
// the value to outlive the array must copy first (e.g. string.concat(v, ""))
// or use string_split_to_seq, which returns refcounted cells that the
// compiler reclaims deterministically at scope exit (the recommended shape;
// see std/string/module.ae). The module.ae extern carries the same warning
// for the Aether-side reader.
const char* string_array_get(AetherStringArray* arr, int index) {
    if (!arr || index < 0 || (size_t)index >= arr->count) return NULL;
    AetherString* s = arr->strings[index];
    return s ? s->data : NULL;
}

void string_array_free(AetherStringArray* arr) {
    if (!arr) return;
    for (size_t i = 0; i < arr->count; i++) {
        string_release(arr->strings[i]);
    }
    /* #463: the strings array's allocated element count equals
     * arr->count in every path that produces a fully-built array
     * (string_split's three branches + string_seq_to_array all set
     * count to the allocation size). caps_free(NULL, 0) is a no-op
     * for the empty-delimiter zero-length case (strings == NULL). */
    aether_caps_free(arr->strings, arr->count * sizeof(AetherString*));
    aether_caps_free(arr, sizeof(AetherStringArray));
}

/* string_split_to_seq — sibling of string_split that materialises the
 * result as a cons-cell *StringSeq instead of an AetherStringArray*.
 *
 * Implementation note: we delegate to `string_split` and then
 * re-shape via the StringSeq builder. The double-allocation is
 * intentional for the V1 surface — keeps the split logic in one
 * place, no risk of behaviour drift between the two return shapes.
 * If the cost shows up in profiles later, fuse the walk with cell
 * allocation in a follow-up.
 *
 * The layering here is asymmetric: std/string can call into
 * std/collections (it's a higher layer in the Makefile order), but
 * not the other way round. We declare the StringSeq surface inline
 * to avoid a #include from this header that downstream tools
 * (header compatibility check, MSVC build) would have to resolve. */
struct StringSeq;  /* forward decl — full def in std/collections/aether_stringseq.h */
extern struct StringSeq* string_seq_cons(const char* head, struct StringSeq* tail);
extern void string_seq_free(struct StringSeq* s);
extern const char* string_seq_head(struct StringSeq* s);
extern struct StringSeq* string_seq_tail(struct StringSeq* s);
extern int string_seq_length(struct StringSeq* s);

/* Join a *StringSeq into one string with `sep` between elements — the
 * linear-cost complement to string_split (issue #1346). Two passes: size
 * the result (sum of element lengths + sep_len*(n-1)), allocate ONE cap-aware
 * buffer, then copy. Returns a magic AetherString* (heap-owned; the @heap
 * extern makes the caller own it, reclaimed at scope exit). Empty seq -> "".
 * Both elements and `sep` may be magic AetherString* or plain char* (str_len/
 * str_data unwrap either), so the join is binary-safe. */
void* string_join(struct StringSeq* seq, const void* sep) {
    int n = string_seq_length(seq);
    if (n <= 0) return string_empty();
    size_t sep_len = str_len(sep);
    /* Pass 1: total size. */
    size_t total = 0;
    struct StringSeq* cur = seq;
    for (int i = 0; i < n && cur; i++) {
        total += str_len(string_seq_head(cur));
        cur = string_seq_tail(cur);
    }
    total += sep_len * (size_t)(n - 1);
    /* One allocation (+1 for the NUL terminator string_adopt expects). */
    char* buf = (char*)aether_caps_malloc(total + 1);
    if (!buf) return NULL;
    /* Pass 2: copy elements, interleaving the separator. */
    size_t off = 0;
    cur = seq;
    const char* sep_data = str_data(sep);
    for (int i = 0; i < n && cur; i++) {
        if (i > 0 && sep_len > 0) {
            memcpy(buf + off, sep_data, sep_len);
            off += sep_len;
        }
        const void* h = string_seq_head(cur);
        size_t hl = str_len(h);
        if (hl > 0) { memcpy(buf + off, str_data(h), hl); off += hl; }
        cur = string_seq_tail(cur);
    }
    buf[off] = '\0';
    return string_adopt_caps_buffer(buf, off, total + 1);
}

void* string_split_to_seq(const void* str, const void* delimiter) {
    AetherStringArray* arr = string_split(str, delimiter);
    if (!arr) return NULL;
    /* Build back-to-front so each cons is O(1). cons retains its
     * head and tail; we drop the previous local tail ref so only
     * the new cell holds it. */
    struct StringSeq* head = NULL;
    for (size_t i = arr->count; i > 0; i--) {
        struct StringSeq* cell = string_seq_cons((const char*)arr->strings[i - 1], head);
        if (!cell) {
            string_seq_free(head);
            string_array_free(arr);
            return NULL;
        }
        string_seq_free(head); /* drop old local; cell owns the retained ref */
        head = cell;
    }
    string_array_free(arr); /* releases each piece's array-side ref;
                             * the seq cells retained their own. */
    return head;
}

// Conversion
const char* string_to_cstr(const void* str) {
    if (!str) return "";
    if (is_aether_string(str)) return ((const AetherString*)str)->data;
    // Already a plain char*
    return (const char*)str;
}

// Public FFI accessors. See aether_string.h for the rationale — C
// shims that consume a `-> string` extern return must NOT treat the
// pointer as `const char*` for memcpy/strlen, or they read into the
// struct header. These helpers unwrap the AetherString if present and
// fall back to strlen-based handling for plain char* returns (legacy
// raw-TLS externs). Safe on NULL.
const char* aether_string_data(const void* s) {
    return str_data(s);
}

size_t aether_string_length(const void* s) {
    return str_len(s);
}

AetherString* string_from_int(int value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    return string_new(buffer);
}

// Sibling of string_from_int that preserves the full 64-bit range.
// `long long` covers Aether's `long` type; callers formatting byte
// counts, file sizes, revision numbers, or other values that can
// exceed INT_MAX reach for this instead of truncating through
// `string_from_int`.
AetherString* string_from_long(long long value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    return string_new(buffer);
}

// Aether's `float` lowers to C `double` (8 bytes), so the parameter
// MUST be declared `double` here — not C `float`. Same ABI-mismatch
// hazard PR #562 caught for the symmetric parse side
// (`string_get_float`): the Aether caller pushes 8 bytes, the C side
// reads only the low 4 as an IEEE-754 binary32, and most doubles'
// low 32 bits are mostly mantissa → garbage on read. `1.0` happens to
// serialise as `"0"` because its low 32 mantissa bits are zero, which
// reads back as +0.0f — the smoking-gun symptom that surfaced from
// the aether-ui CVG port.
//
// `%g` already prints a `double` correctly (it's the default for
// floating-point promotion in varargs), so the format string stays.
AetherString* string_from_float(double value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%g", value);
    return string_new(buffer);
}

// Inverse of string_to_int_radix: render `value` as a base-N digit
// string. radix in [2, 36]; out-of-range radix yields the empty
// string (caller-detectable, matches the existing string_empty()
// failure convention for stdlib emit-side functions). Negative
// values get a leading '-'.
//
// Hand-rolled digit loop because libc only offers %x / %o / %d in
// snprintf, not arbitrary base-N. Buffer size: 64-bit binary needs
// 64 digits + sign + NUL = 66 bytes; we allocate 80 for headroom.
//
// `long long` parameter (NOT C `long`): Aether's `long` lowers to
// int64_t on every platform; C `long` is 32-bit on Windows LLP64
// — same lesson PR #562 pinned for string_to_long_raw, applied
// preemptively.
AetherString* string_from_int_radix(long long value, int radix) {
    if (radix < 2 || radix > 36) return string_empty();

    char buf[80];
    int neg = (value < 0);
    /* Negate via unsigned to avoid UB on LLONG_MIN (which can't be
     * represented as a positive long long). */
    unsigned long long v = neg
        ? (unsigned long long)(-(value + 1)) + 1ULL
        : (unsigned long long)value;

    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v > 0) {
            int d = (int)(v % (unsigned long long)radix);
            buf[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
            v /= (unsigned long long)radix;
        }
    }
    if (neg) buf[n++] = '-';

    /* Reverse in place. */
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
    }
    buf[n] = '\0';
    return string_new_with_length(buf, (size_t)n);
}

/* Pad-start / pad-end helpers — mirror JS String.prototype.padStart
 * / padEnd. If `s` is already >= total_width chars, returns a fresh
 * copy (no truncation; same allocation behaviour as the pad path so
 * callers can always release the result uniformly). total_width <= 0
 * also returns a fresh copy.
 *
 * `pad_char` is a single-byte char code (int), NOT a multi-char fill
 * string. This matches string_char_at's convention (returns int char
 * code), avoids string-alloc overhead in tight loops, and keeps the
 * C side a clean memset+memcpy. The aether-ui CVG port wants `48`
 * ('0') for zero-padded hex bytes; columnar-output use cases want
 * `32` (' '). Multi-char fill is deferred until someone needs it. */
AetherString* string_pad_start(const void* s, int total_width, int pad_char) {
    const char* data = str_data(s);
    size_t len = str_len(s);
    if (!s || total_width <= 0 || (size_t)total_width <= len) {
        return string_new_with_length(data ? data : "", data ? len : 0);
    }
    size_t pad_n = (size_t)total_width - len;
    char* tmp = (char*)malloc((size_t)total_width + 1);
    if (!tmp) return string_empty();
    memset(tmp, (char)pad_char, pad_n);
    if (data && len > 0) memcpy(tmp + pad_n, data, len);
    tmp[total_width] = '\0';
    AetherString* out = string_new_with_length(tmp, (size_t)total_width);
    free(tmp);
    return out;
}

AetherString* string_pad_end(const void* s, int total_width, int pad_char) {
    const char* data = str_data(s);
    size_t len = str_len(s);
    if (!s || total_width <= 0 || (size_t)total_width <= len) {
        return string_new_with_length(data ? data : "", data ? len : 0);
    }
    size_t pad_n = (size_t)total_width - len;
    char* tmp = (char*)malloc((size_t)total_width + 1);
    if (!tmp) return string_empty();
    if (data && len > 0) memcpy(tmp, data, len);
    memset(tmp + len, (char)pad_char, pad_n);
    tmp[total_width] = '\0';
    AetherString* out = string_new_with_length(tmp, (size_t)total_width);
    free(tmp);
    return out;
}

// Parsing functions - convert string to numbers.
// The `_raw` variants take an out-parameter and return 1/0 for ok/fail.
// The Aether-native Go-style wrappers `string.to_int` etc. in module.ae
// call the `_try`/`_get` pairs below for a cleaner tuple-return shape.
// Base-N integer parse. `radix` must be 2..36 (strtoll's accepted
// range; same as C's strtol). No "0x" / "0b" prefix recognition — the
// caller passes the digit-only substring (strtoll *does* honour an
// "0x" prefix when radix is 0 or 16, but Aether callers historically
// pass already-stripped substrings, and base-16 with surprise prefix
// handling would be a footgun in things like CSV/HSV color parsing).
// `out_value` is `long long*` (Aether `long` = int64) — same LLP64
// safety as string_to_long_raw. Returns 1 on success, 0 on:
//   - null/empty input or null out_value
//   - radix outside [2, 36]
//   - no conversion (first char not a valid digit for the radix)
//   - ERANGE overflow
//   - trailing non-whitespace garbage
int string_to_int_radix_raw(const void* str, int radix, long long* out_value) {
    const char* data = str_data(str);
    if (!str || !data[0] || !out_value) return 0;
    if (radix < 2 || radix > 36) return 0;

    char* endptr;
    errno = 0;
    long long val = strtoll(data, &endptr, radix);

    if (endptr == data || errno == ERANGE) {
        return 0;
    }
    // Skip trailing whitespace; anything else is an error.
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n' || *endptr == '\r') {
        endptr++;
    }
    if (*endptr != '\0') return 0;

    *out_value = val;
    return 1;
}

int string_to_int_raw(const void* str, int* out_value) {
    const char* data = str_data(str);
    if (!str || !data[0] || !out_value) return 0;

    char* endptr;
    errno = 0;
    long val = strtol(data, &endptr, 10);

    // Check for errors: no conversion, overflow, or trailing garbage
    if (endptr == data || errno == ERANGE || val > INT_MAX || val < INT_MIN) {
        return 0;
    }

    // Skip trailing whitespace
    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return 0;  // Trailing non-whitespace

    *out_value = (int)val;
    return 1;
}

// out_value is a real 64-bit slot: Aether's `long` is int64 everywhere,
// so we use `long long` + strtoll, NOT C `long` + strtol. On Windows
// (LLP64) C `long` is only 32 bits, so the old `long`/strtol pair wrote
// 4 bytes into Aether's 8-byte slot and truncated 64-bit values (e.g. a
// stashed pointer) — fine on LP64 Linux/macOS, broken on Windows.
int string_to_long_raw(const void* str, long long* out_value) {
    const char* data = str_data(str);
    if (!str || !data[0] || !out_value) return 0;

    char* endptr;
    errno = 0;
    long long val = strtoll(data, &endptr, 10);

    if (endptr == data || errno == ERANGE) {
        return 0;
    }

    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return 0;

    *out_value = val;
    return 1;
}

int string_to_float_raw(const void* str, float* out_value) {
    const char* data = str_data(str);
    if (!str || !data[0] || !out_value) return 0;

    char* endptr;
    errno = 0;
    float val = strtof(data, &endptr);

    if (endptr == data || errno == ERANGE) {
        return 0;
    }

    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return 0;

    *out_value = val;
    return 1;
}

int string_to_double_raw(const void* str, double* out_value) {
    const char* data = str_data(str);
    if (!str || !data[0] || !out_value) return 0;

    char* endptr;
    errno = 0;
    double val = strtod(data, &endptr);

    if (endptr == data || errno == ERANGE) {
        return 0;
    }

    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return 0;

    *out_value = val;
    return 1;
}

// Split-return helpers for Aether's Go-style tuple wrappers. Each `_try`
// function returns 1 if the parse would succeed, 0 otherwise. The `_get`
// function returns the parsed value (or 0/0.0 if unparseable). Callers
// pair them: `if (try) return get(), ""; else return 0, "invalid"`.
int string_try_int(const void* s) {
    int v; return string_to_int_raw(s, &v);
}
int string_get_int(const void* s) {
    int v = 0; string_to_int_raw(s, &v); return v;
}
int string_try_long(const void* s) {
    long long v; return string_to_long_raw(s, &v);
}
// long long, not long: Aether's `long` is 64-bit on every platform; C
// `long` is 32-bit on Windows (LLP64) and would truncate here.
long long string_get_long(const void* s) {
    long long v = 0; string_to_long_raw(s, &v); return v;
}
// Base-N split-return helpers. Same shape as try_long / get_long; the
// `radix` parameter is forwarded to string_to_int_radix_raw. Out-slot
// is `long long` for LLP64 safety. The Aether-side wrapper in
// std/string/module.ae assembles these into a Go-style
// (value, error) tuple.
int string_try_int_radix(const void* s, int radix) {
    long long v; return string_to_int_radix_raw(s, radix, &v);
}
long long string_get_int_radix(const void* s, int radix) {
    long long v = 0; string_to_int_radix_raw(s, radix, &v); return v;
}
int string_try_float(const void* s) {
    float v; return string_to_float_raw(s, &v);
}
// Returns double, not float: Aether's `float` type is 64-bit (it lowers
// to C `double`), so the extern `string_get_float -> float` expects a
// 64-bit return. Returning a 32-bit C `float` here left Aether reading
// the result register as a double → garbage (e.g. to_float("2.5") came
// back 5.3e-315). We still parse at float precision (to_float's contract;
// use to_double for full precision), then widen for the ABI.
double string_get_float(const void* s) {
    float v = 0.0f; string_to_float_raw(s, &v); return (double)v;
}
int string_try_double(const void* s) {
    double v; return string_to_double_raw(s, &v);
}
double string_get_double(const void* s) {
    double v = 0.0; string_to_double_raw(s, &v); return v;
}

// Printf-style string formatting
AetherString* string_format(const char* fmt, ...) {
    if (!fmt) return string_empty();

    va_list args;

    // First pass: calculate required size
    va_start(args, fmt);
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (size < 0) return string_empty();

    // Allocate buffer
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) return string_empty();

    // Second pass: format string
    va_start(args, fmt);
    vsnprintf(buffer, size + 1, fmt, args);
    va_end(args);

    AetherString* result = string_new_with_length(buffer, size);
    free(buffer);
    return result;
}

/* Aether-callable formatter — Aether externs don't support varargs,
 * so the public surface takes an ArrayList of arguments and walks
 * the format string substituting `{}` placeholders with each list
 * entry. Closes #272.
 *
 * Placeholders: `{}` is replaced with the next arg (Rust-style).
 * `{{` is a literal `{`; `}}` is a literal `}`. List entries are
 * read as strings (AetherString* or plain char*); ints and other
 * types should be converted via `string.from_int(...)` first.
 *
 * Why `{}` not `%s`: it composes more cleanly with Aether's existing
 * `${...}` interpolation surface (interpolation is for callsite-
 * literal substitution; format is for runtime-built strings) and it
 * leaves the `%`-prefix open for typed printf-style formatters
 * (`%d`, `%.3f`) without breaking compatibility if those land later.
 */
extern int   list_size(void* list);
extern void* list_get_raw(void* list, int index);

AetherString* string_format_list(const char* fmt, void* args) {
    if (!fmt) return string_empty();
    int n_args = args ? list_size(args) : 0;
    int next_arg = 0;

    /* First pass: compute total length so we can allocate exactly
     * once. Read placeholders and arg lengths to size the buffer. */
    size_t total = 0;
    const char* p = fmt;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') { total++; p += 2; continue; }
        if (p[0] == '}' && p[1] == '}') { total++; p += 2; continue; }
        if (p[0] == '{' && p[1] == '}') {
            if (next_arg < n_args) {
                void* a = list_get_raw(args, next_arg);
                size_t alen = a ? str_len(a) : 0;
                total += alen;
                next_arg++;
            }
            p += 2; continue;
        }
        total++; p++;
    }

    char* out = (char*)malloc(total + 1);
    if (!out) return string_empty();

    /* Second pass: write the bytes. */
    next_arg = 0;
    size_t pos = 0;
    p = fmt;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') { out[pos++] = '{'; p += 2; continue; }
        if (p[0] == '}' && p[1] == '}') { out[pos++] = '}'; p += 2; continue; }
        if (p[0] == '{' && p[1] == '}') {
            if (next_arg < n_args) {
                void* a = list_get_raw(args, next_arg);
                if (a) {
                    size_t alen = str_len(a);
                    const char* adata = str_data(a);
                    if (alen > 0) memcpy(out + pos, adata, alen);
                    pos += alen;
                }
                next_arg++;
            }
            p += 2; continue;
        }
        out[pos++] = *p++;
    }
    out[pos] = '\0';

    AetherString* result = string_new_with_length(out, pos);
    free(out);
    return result;
}

/* ─── string_glob_match_raw ─────────────────────────────────────────
 * Glob-pattern matching. POSIX delegates to fnmatch(3); Windows gets
 * a pure-C implementation covering the same surface (`*`, `?`, `[…]`,
 * `[!…]`, `[a-z]`, `\*` / `\?`, plus the FNM_PATHNAME flag — when set,
 * `*` / `?` / `[…]` do not match a literal `/`).
 *
 * Aether-callable input is `string` (auto-unwrapped to `const char*`
 * by the extern boundary's #297 unwrap). The Aether-side `flags` int
 * is a SENTINEL, NOT the libc fnmatch flag word: pass 0 for plain
 * matching, any non-zero value for the pathname-aware form. The C
 * side translates that sentinel into the platform's actual
 * FNM_PATHNAME constant before calling libc fnmatch — `FNM_PATHNAME`
 * is 0x01 on glibc/musl but 0x02 on macOS / BSD libc, so passing the
 * raw Aether-side int through to fnmatch directly produced wrong
 * results on macOS (the literal `1` was interpreted as
 * `FNM_NOESCAPE` rather than `FNM_PATHNAME`, allowing `*` to cross
 * `/` in the pathname-aware form). The wrapper layer
 * (`glob_match_pathname`) passes 1 unconditionally; the translation
 * happens here.
 *
 * Returns: 1 = match, 0 = no match, -1 = pattern syntax error
 * (unmatched bracket).
 * ────────────────────────────────────────────────────────────────── */

#ifdef _WIN32
/* Windows lacks libc fnmatch. Recursive matcher: walks pattern and
 * string together, with `*` triggering a rest-of-string search. The
 * recursion depth is bounded by the number of `*` segments in the
 * pattern; flat patterns (no `*`) tail-match in O(|s|). */
static int win_glob_match(const char* p, const char* s, int pathname_flag) {
    while (*p) {
        if (*p == '*') {
            /* Skip consecutive '*'s. */
            while (*p == '*') p++;
            if (!*p) {
                /* Trailing '*' matches everything remaining — except
                 * '/' under FNM_PATHNAME. */
                if (pathname_flag) {
                    while (*s) {
                        if (*s == '/') return 0;
                        s++;
                    }
                    return 1;
                }
                return 1;
            }
            while (*s) {
                int r = win_glob_match(p, s, pathname_flag);
                if (r) return r;
                if (pathname_flag && *s == '/') return 0;
                s++;
            }
            return win_glob_match(p, s, pathname_flag);  /* try empty tail */
        }
        if (*p == '?') {
            if (!*s) return 0;
            if (pathname_flag && *s == '/') return 0;
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            if (!*s) return 0;
            if (pathname_flag && *s == '/') return 0;
            const char* class_start = p;
            p++;
            int negate = 0;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int matched = 0;
            char c = *s;
            int first = 1;
            while (*p && (*p != ']' || first)) {
                first = 0;
                char low = *p;
                if (*p == '\\' && *(p + 1)) { low = *(p + 1); p += 2; }
                else                         { p++; }
                char high = low;
                if (*p == '-' && *(p + 1) && *(p + 1) != ']') {
                    p++;
                    if (*p == '\\' && *(p + 1)) { high = *(p + 1); p += 2; }
                    else                         { high = *p; p++; }
                }
                if (c >= low && c <= high) matched = 1;
            }
            if (!*p) {
                /* Unmatched bracket — pattern is malformed. */
                (void)class_start;
                return -1;
            }
            p++;  /* consume ']' */
            if (matched == negate) return 0;
            s++;
            continue;
        }
        if (*p == '\\' && *(p + 1)) {
            if (*s != *(p + 1)) return 0;
            p += 2;
            s++;
            continue;
        }
        if (*p != *s) return 0;
        p++;
        s++;
    }
    return *s == '\0' ? 1 : 0;
}
#endif

int string_glob_match_raw(const void* pattern, const void* s, int flags) {
    if (!pattern || !s) return 0;
    /* Mixed-representation hazard: the call-site #297 auto-unwrap
     * skips `string_*`-prefixed externs (presumed string-aware), so
     * we receive `const void*` that may be either a magic
     * AetherString* or a bare char*, independently per arg. Dispatch
     * through str_data on both — fnmatch/win_glob_match would
     * otherwise read a 16-byte struct header as the first bytes of
     * pattern/s, returning wrong results when one arg is heap-magic
     * and the other a literal. */
    const char* p = str_data(pattern);
    const char* ss = str_data(s);
#ifdef _WIN32
    /* On Windows, treat any non-zero flag as the pathname-aware form.
     * The plain (flags=0) and path (flags=1) forms are the only two
     * we expose Aether-side, so this collapses correctly. */
    return win_glob_match(p, ss, flags ? 1 : 0);
#else
    /* Translate the Aether-side sentinel (0 = plain, non-zero =
     * pathname-aware) into the platform's actual FNM_PATHNAME bit.
     * Without this translation, macOS's fnmatch treats the raw `1`
     * as FNM_NOESCAPE (since on BSD libc FNM_PATHNAME is 0x02, not
     * 0x01) and `*` is allowed to cross `/` in pathname mode —
     * the symptom that surfaced on the Mac/Clang CI lane. */
    int libc_flags = (flags != 0) ? FNM_PATHNAME : 0;
    int r = fnmatch(p, ss, libc_flags);
    if (r == 0)            return 1;
    if (r == FNM_NOMATCH)  return 0;
    return -1;  /* any other libc error code maps to syntax-error */
#endif
}
