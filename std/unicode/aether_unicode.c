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

/* Run utf8proc_map over `input` with `options` and hand the result back as a
 * managed AetherString. utf8proc_map malloc()s a fresh NUL-terminated UTF-8
 * buffer; we copy it into a managed string and free the utf8proc buffer, so
 * ownership is uniform with the rest of std. On any error, returns an empty
 * managed string rather than NULL — the caller sees "" and its length is 0. */
static void* map_to_string(const char* input, utf8proc_option_t options) {
    if (!input) return string_new_with_length("", 0);
    utf8proc_ssize_t inlen = (utf8proc_ssize_t)strlen(input);
    utf8proc_uint8_t* out = NULL;
    utf8proc_ssize_t n = utf8proc_map((const utf8proc_uint8_t*)input, inlen,
                                      &out, options);
    if (n < 0 || !out) {
        if (out) free(out);
        return string_new_with_length("", 0);
    }
    void* s = string_new_with_length((const char*)out, (size_t)n);
    free(out);
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
 * the end, or count <= 0, yields "". count < 0 is treated as "to the end". */
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
        if (adv <= 0 || cp < 0) break;
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
