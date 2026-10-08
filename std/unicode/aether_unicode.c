/* std.unicode — Unicode normalization, case/accent folding, and
 * grapheme-cluster-aware length/substring, over the vendored utf8proc
 * (std/unicode/utf8proc, v2.9.0, MIT).
 *
 * The byte-indexed operations in std.string cut multi-byte characters; these
 * are codepoint- and grapheme-correct. Returned strings are managed
 * AetherStrings (string_new_with_length), so the heap-string tracker owns and
 * frees them like any other std return. */
#include "aether_unicode.h"
#include "utf8proc/utf8proc.h"
#include "../string/aether_string.h" /* string_new_with_length — return managed strings */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A growable byte buffer for map_to_string's invalid-input path. */
typedef struct { char* p; size_t n, cap; } map_buf;
static int map_buf_put(map_buf* b, const void* src, size_t len) {
    if (b->n + len > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->n + len) cap *= 2;
        char* grown = (char*)realloc(b->p, cap);
        if (!grown) return 0;
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, src, len);
    b->n += len;
    return 1;
}

/* Run utf8proc_map over `input` with `options` and hand the result back as a
 * managed AetherString. utf8proc_map malloc()s a fresh NUL-terminated UTF-8
 * buffer; we copy it into a managed string and free the utf8proc buffer, so
 * ownership is uniform with the rest of std.
 *
 * Input that is not valid UTF-8 is mapped run by run: each maximal valid run
 * goes through utf8proc, and each byte that does not start a valid sequence
 * is kept as it is. utf8proc refuses the whole string at the first such
 * byte, and returning "" for it made every invalid string fold to the same
 * empty result: equals_ignore_case called any two of them equal, and
 * casefold("ABC\xff") lost the "ABC". Valid input takes the single
 * utf8proc_map call it always did. Out of memory still yields "". */
static void* map_to_string(const char* input, utf8proc_option_t options) {
    if (!input) return string_new_with_length("", 0);
    utf8proc_ssize_t inlen = (utf8proc_ssize_t)strlen(input);
    utf8proc_uint8_t* out = NULL;
    utf8proc_ssize_t n = utf8proc_map((const utf8proc_uint8_t*)input, inlen,
                                      &out, options);
    if (n >= 0 && out) {
        void* s = string_new_with_length((const char*)out, (size_t)n);
        free(out);
        return s;
    }
    if (out) free(out);
    if (n != UTF8PROC_ERROR_INVALIDUTF8) return string_new_with_length("", 0);

    map_buf b = { NULL, 0, 0 };
    utf8proc_ssize_t pos = 0;
    int ok = 1;
    while (ok && pos < inlen) {
        utf8proc_ssize_t run = pos;
        while (pos < inlen) {
            utf8proc_int32_t cp;
            utf8proc_ssize_t adv = utf8proc_iterate(
                (const utf8proc_uint8_t*)(input + pos), inlen - pos, &cp);
            if (adv <= 0 || cp < 0) break;
            pos += adv;
        }
        if (pos > run) {
            utf8proc_uint8_t* seg = NULL;
            utf8proc_ssize_t m = utf8proc_map((const utf8proc_uint8_t*)(input + run),
                                              pos - run, &seg, options);
            ok = m >= 0 && seg && map_buf_put(&b, seg, (size_t)m);
            if (seg) free(seg);
        }
        if (ok && pos < inlen) {
            ok = map_buf_put(&b, input + pos, 1);   /* the invalid byte, as is */
            pos++;
        }
    }
    void* s = ok ? string_new_with_length(b.p ? b.p : "", b.n)
                 : string_new_with_length("", 0);
    free(b.p);
    return s;
}

/* NFC / NFD normalization. STABLE keeps the result stable across utf8proc
 * versions for the same Unicode data. */
void* aether_unicode_nfc(const char* input) {
    return map_to_string(input,
        (utf8proc_option_t)(UTF8PROC_COMPOSE | UTF8PROC_STABLE));
}
void* aether_unicode_nfd(const char* input) {
    return map_to_string(input,
        (utf8proc_option_t)(UTF8PROC_DECOMPOSE | UTF8PROC_STABLE));
}

/* Accent folding: decompose, then strip the combining marks. "café" -> "cafe".
 * STRIPMARK requires DECOMPOSE (utf8proc's contract), and DECOMPOSE and COMPOSE
 * are mutually exclusive in one utf8proc_map call, so the result is left in
 * decomposed form. Once the marks are gone there is nothing to recompose for
 * the Latin text this targets (a base letter with no mark composes to itself),
 * so the output is the accent-free string either way. */
void* aether_unicode_fold_accents(const char* input) {
    return map_to_string(input, (utf8proc_option_t)(
        UTF8PROC_DECOMPOSE | UTF8PROC_STRIPMARK | UTF8PROC_STABLE));
}

/* Case folding for case-insensitive comparison. Not the same as lowercasing:
 * casefolding maps e.g. 'ß' -> "ss" and is designed for caseless matching.
 * NFC-composed after folding so equal strings fold to byte-identical results. */
void* aether_unicode_casefold(const char* input) {
    return map_to_string(input, (utf8proc_option_t)(
        UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE | UTF8PROC_STABLE));
}

/* The number of grapheme clusters (user-perceived characters) in `input`.
 * Decodes codepoints with utf8proc_iterate and counts a boundary before each
 * codepoint per utf8proc_grapheme_break_stateful. A malformed byte ends the
 * scan (returns the count so far) rather than looping. */
int aether_unicode_grapheme_len(const char* input) {
    if (!input) return 0;
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(input);
    utf8proc_ssize_t pos = 0;
    utf8proc_int32_t prev = 0;
    utf8proc_int32_t state = 0;
    int count = 0;
    int have_prev = 0;
    while (pos < len) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t adv = utf8proc_iterate(
            (const utf8proc_uint8_t*)(input + pos), len - pos, &cp);
        if (adv <= 0 || cp < 0) break; /* malformed: stop */
        if (!have_prev) {
            count = 1; /* the first codepoint opens the first cluster */
            have_prev = 1;
        } else if (utf8proc_grapheme_break_stateful(prev, cp, &state)) {
            count++;
        }
        prev = cp;
        pos += adv;
    }
    return count;
}

/* The substring of `input` spanning grapheme clusters [start, start+count),
 * as a managed AetherString. Grapheme-correct: it never splits a multi-byte
 * character or a combining sequence, unlike a byte-indexed slice. A start past
 * the end, or count <= 0, yields "". count < 0 is treated as "to the end".
 * As in grapheme_len, a malformed byte ends the scan: the span stops before
 * it (it used to run on to the end of the input, the bad byte included). */
void* aether_unicode_grapheme_substring(const char* input, int start, int count) {
    if (!input || start < 0) return string_new_with_length("", 0);
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(input);
    utf8proc_ssize_t pos = 0;
    utf8proc_int32_t prev = 0;
    utf8proc_int32_t state = 0;
    int cluster = -1;             /* index of the cluster the cursor is in */
    int have_prev = 0;
    utf8proc_ssize_t byte_start = -1; /* byte offset where cluster `start` begins */
    utf8proc_ssize_t byte_end = len;  /* byte offset where the span ends */
    int taken = 0;

    while (pos < len) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t adv = utf8proc_iterate(
            (const utf8proc_uint8_t*)(input + pos), len - pos, &cp);
        if (adv <= 0 || cp < 0) { byte_end = pos; break; }
        int boundary = !have_prev ||
            utf8proc_grapheme_break_stateful(prev, cp, &state);
        if (boundary) {
            cluster++;
            if (cluster == start) byte_start = pos;
            if (byte_start >= 0 && cluster >= start) {
                if (count >= 0 && taken >= count) { byte_end = pos; break; }
                taken++;
            }
        }
        prev = cp;
        have_prev = 1;
        pos += adv;
    }
    if (byte_start < 0) return string_new_with_length("", 0);
    return string_new_with_length(input + byte_start,
                                  (size_t)(byte_end - byte_start));
}
