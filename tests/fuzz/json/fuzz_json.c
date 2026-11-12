/* Coverage-guided fuzz target for std.json's parser and serializer
 * (std/json/aether_json.c): json_parse_raw_n, json_stringify_raw and the
 * accessors a program reads a parsed tree with.
 *
 * The input is a document, every byte of it, NUL bytes included. Each check
 * is a property, and a failure abort()s with what disagreed:
 *
 *   J1  The parser accepts exactly the documents RFC 8259 does, as a
 *       validator written here from the RFC's grammar decides: one value
 *       with optional whitespace around it, no trailing commas, leading
 *       zeros, comments or bare control bytes in strings, only the eight
 *       escapes and \u with four hex digits (a high surrogate followed by a
 *       low one, never a lone one), and UTF-8 that is well formed (no
 *       overlong forms, surrogates or code points past U+10FFFF). A value
 *       more than 256 levels inside the outermost one is refused
 *       (aether_json.c's JSON_MAX_DEPTH): 257 nested arrays parse, and a
 *       value inside the innermost does not.
 *   J2  A refusal is an error with a kind, a position and a message; a
 *       success clears them.
 *   J3  A parsed value serializes, the text reads back to the same value
 *       (numbers to the same double or the same integer, strings and keys
 *       byte for byte, U+0000 included, members in the same order; an
 *       infinity, which JSON cannot spell, as null), and serializing that
 *       gives the same text again.
 *   J4  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "aether_json.h"
#include "aether_string.h"

#define J_MAX_DEPTH 256

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_json: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

/* ---- J1: an RFC 8259 validator ---------------------------------------- */

typedef struct { const unsigned char* p; const unsigned char* end; int too_deep; } V;

static void v_ws(V* v) {
    while (v->p < v->end && (*v->p == ' ' || *v->p == '\t' || *v->p == '\n' || *v->p == '\r')) v->p++;
}

static int v_lit(V* v, const char* lit) {
    size_t n = strlen(lit);
    if ((size_t)(v->end - v->p) < n || memcmp(v->p, lit, n) != 0) return 0;
    v->p += n;
    return 1;
}

static int v_digit(V* v) { return v->p < v->end && *v->p >= '0' && *v->p <= '9'; }

static int v_number(V* v) {
    if (v->p < v->end && *v->p == '-') v->p++;
    if (!v_digit(v)) return 0;
    if (*v->p == '0') v->p++;
    else while (v_digit(v)) v->p++;
    if (v->p < v->end && *v->p == '.') {
        v->p++;
        if (!v_digit(v)) return 0;
        while (v_digit(v)) v->p++;
    }
    if (v->p < v->end && (*v->p == 'e' || *v->p == 'E')) {
        v->p++;
        if (v->p < v->end && (*v->p == '+' || *v->p == '-')) v->p++;
        if (!v_digit(v)) return 0;
        while (v_digit(v)) v->p++;
    }
    return 1;
}

static int v_hex4(V* v, unsigned* out) {
    if (v->end - v->p < 4) return 0;
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = v->p[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        x = x * 16 + (unsigned)d;
    }
    v->p += 4;
    *out = x;
    return 1;
}

/* One UTF-8 sequence starting with a byte >= 0x80 (RFC 3629 4). */
static int v_utf8(V* v) {
    unsigned char c = *v->p;
    int n;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) n = 1;
    else if (c == 0xE0) { n = 2; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) n = 2;
    else if (c == 0xED) { n = 2; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) n = 2;
    else if (c == 0xF0) { n = 3; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 3;
    else if (c == 0xF4) { n = 3; hi = 0x8F; }
    else return 0;
    if (v->end - v->p < n + 1) return 0;
    for (int i = 1; i <= n; i++) {
        unsigned char b = v->p[i];
        unsigned l = i == 1 ? lo : 0x80, h = i == 1 ? hi : 0xBF;
        if (b < l || b > h) return 0;
    }
    v->p += n + 1;
    return 1;
}

static int v_string(V* v) {
    if (v->p >= v->end || *v->p != '"') return 0;
    v->p++;
    for (;;) {
        if (v->p >= v->end) return 0;
        unsigned char c = *v->p;
        if (c == '"') { v->p++; return 1; }
        if (c < 0x20) return 0;
        if (c == '\\') {
            v->p++;
            if (v->p >= v->end) return 0;
            c = *v->p++;
            if (c == 'u') {
                unsigned u;
                if (!v_hex4(v, &u)) return 0;
                if (u >= 0xDC00 && u <= 0xDFFF) return 0;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    unsigned lo;
                    if (!v_lit(v, "\\u") || !v_hex4(v, &lo) || lo < 0xDC00 || lo > 0xDFFF) return 0;
                }
            } else if (!strchr("\"\\/bfnrt", c) || c == 0) {
                return 0;
            }
            continue;
        }
        if (c >= 0x80) { if (!v_utf8(v)) return 0; continue; }
        v->p++;
    }
}

static int v_value(V* v, int depth);

static int v_container(V* v, int depth, char open, char close, int object) {
    v->p++;
    v_ws(v);
    if (v->p < v->end && *v->p == close) { v->p++; return 1; }
    (void)open;
    for (;;) {
        if (object) {
            if (!v_string(v)) return 0;
            v_ws(v);
            if (v->p >= v->end || *v->p != ':') return 0;
            v->p++;
            v_ws(v);
        }
        if (!v_value(v, depth + 1)) return 0;
        v_ws(v);
        if (v->p >= v->end) return 0;
        if (*v->p == close) { v->p++; return 1; }
        if (*v->p != ',') return 0;
        v->p++;
        v_ws(v);
    }
}

static int v_value(V* v, int depth) {
    /* As aether_json.c counts it: the outermost value is at depth 0, and a
     * value deeper than JSON_MAX_DEPTH is refused when it is reached. */
    if (depth > J_MAX_DEPTH) { v->too_deep = 1; return 0; }
    if (v->p >= v->end) return 0;
    switch (*v->p) {
        case '{': return v_container(v, depth, '{', '}', 1);
        case '[': return v_container(v, depth, '[', ']', 0);
        case '"': return v_string(v);
        case 't': return v_lit(v, "true");
        case 'f': return v_lit(v, "false");
        case 'n': return v_lit(v, "null");
        default:  return v_number(v);
    }
}

/* 1 valid, 0 invalid, -1 valid only past the depth limit. */
static int rfc8259_valid(const uint8_t* d, size_t n) {
    V v = { d, d + n, 0 };
    v_ws(&v);
    int ok = v_value(&v, 0);
    if (ok) { v_ws(&v); ok = v.p == v.end; }
    return ok ? 1 : (v.too_deep ? -1 : 0);
}

/* ---- J3: value equality ------------------------------------------------ */

static int same_value(JsonValue* a, JsonValue* b, char* why, size_t cap) {
    JsonType ta = json_type(a), tb = json_type(b);
    if (ta == JSON_NUMBER && !json_is_integer(a) && isinf(json_get_number(a))) {
        if (tb != JSON_NULL) { snprintf(why, cap, "an infinity came back as type %d, not null", (int)tb); return 0; }
        return 1;
    }
    if (ta != tb) { snprintf(why, cap, "type %d came back as %d", (int)ta, (int)tb); return 0; }
    switch (ta) {
        case JSON_NULL: return 1;
        case JSON_BOOL:
            if (json_get_bool(a) != json_get_bool(b)) { snprintf(why, cap, "a bool changed"); return 0; }
            return 1;
        case JSON_NUMBER:
            /* A double with no fraction is written as JavaScript writes it,
             * with no fraction ("1.0" and "-1e0" as "1" and "-1"), and reads
             * back as the integer of the same value, negative ones included.
             * Only a negative zero would lose something as an integer (its
             * sign); it is written "-0", which reads back as a double, so
             * it never gets here, and if it did that would be a change. The
             * check refused every negative value here (signbit), which is
             * what CI's first fuzz_json run found on "-1e0" (#3543). */
            if (!json_is_integer(a) && json_is_integer(b)) {
                double x = json_get_number(a);
                if ((double)json_get_long(b) != x || (x == 0 && signbit(x))) {
                    snprintf(why, cap, "%.17g came back as the integer %lld", x, json_get_long(b));
                    return 0;
                }
                return 1;
            }
            if (json_is_integer(a) != json_is_integer(b)) {
                snprintf(why, cap, "integer flag %d came back %d", json_is_integer(a), json_is_integer(b));
                return 0;
            }
            if (json_is_integer(a)) {
                if (json_get_long(a) != json_get_long(b)) {
                    snprintf(why, cap, "%lld came back as %lld", json_get_long(a), json_get_long(b));
                    return 0;
                }
            } else {
                double x = json_get_number(a), y = json_get_number(b);
                if (memcmp(&x, &y, sizeof x) != 0) {
                    snprintf(why, cap, "%.17g came back as %.17g", x, y);
                    return 0;
                }
            }
            return 1;
        case JSON_STRING: {
            int la = json_get_string_length(a), lb = json_get_string_length(b);
            if (la != lb || memcmp(json_get_string_data(a), json_get_string_data(b), (size_t)la) != 0) {
                snprintf(why, cap, "a %d-byte string came back as %d bytes or other bytes", la, lb);
                return 0;
            }
            return 1;
        }
        case JSON_ARRAY: {
            int n = json_array_size(a);
            if (n != json_array_size(b)) { snprintf(why, cap, "an array of %d came back with %d", n, json_array_size(b)); return 0; }
            for (int i = 0; i < n; i++)
                if (!same_value(json_array_get_raw(a, i), json_array_get_raw(b, i), why, cap)) return 0;
            return 1;
        }
        case JSON_OBJECT: {
            int n = json_object_size_raw(a);
            if (n != json_object_size_raw(b)) { snprintf(why, cap, "an object of %d came back with %d", n, json_object_size_raw(b)); return 0; }
            for (int i = 0; i < n; i++) {
                int ka = json_object_key_len_at(a, i), kb = json_object_key_len_at(b, i);
                if (ka != kb || memcmp(json_object_key_data_at(a, i), json_object_key_data_at(b, i), (size_t)ka) != 0) {
                    snprintf(why, cap, "key %d changed", i);
                    return 0;
                }
                if (!same_value(json_object_value_at(a, i), json_object_value_at(b, i), why, cap)) return 0;
            }
            return 1;
        }
    }
    snprintf(why, cap, "unknown type %d", (int)ta);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    /* An exact-size copy, so a read past the end is one past an allocation. */
    char* doc = (char*)malloc(size ? size : 1);
    CHECK(doc != NULL, "out of memory");
    memcpy(doc, data, size);

    JsonValue* v = json_parse_raw_n(doc, size);
    int valid = rfc8259_valid(data, size);

    /* J1, J2 */
    if (!v) {
        const char* err = json_last_error();
        CHECK(json_last_error_kind() != 0 && err && err[0] != '\0',
              "a refusal with kind %d and message '%s'", json_last_error_kind(), err ? err : "(null)");
        CHECK(json_last_error_kind() == 2 || valid != 1,
              "an RFC 8259 document was refused: %s", err);
        free(doc);
        return 0;
    }
    CHECK(json_last_error_kind() == 0 && json_last_error()[0] == '\0',
          "a success left the error kind %d '%s'", json_last_error_kind(), json_last_error());
    CHECK(valid == 1, "a document RFC 8259 %s was accepted",
          valid < 0 ? "allows, but nested past the 256 levels parse allows," : "does not allow");

    /* J3 */
    char* s1 = json_stringify_raw(v);
    CHECK(s1 != NULL, "a parsed value did not serialize");
    size_t n1 = aether_string_length(s1);
    const char* t1 = aether_string_data(s1);
    CHECK(memchr(t1, '\0', n1) == NULL, "the serialized text holds a raw NUL");
    JsonValue* v2 = json_parse_raw_n(t1, n1);
    CHECK(v2 != NULL, "the serialized text does not parse: %s\n  text: %.*s", json_last_error(),
          (int)(n1 > 400 ? 400 : n1), t1);
    char why[256] = "";
    CHECK(same_value(v, v2, why, sizeof why), "the value changed through serializing: %s", why);
    char* s2 = json_stringify_raw(v2);
    CHECK(s2 != NULL, "the re-read value did not serialize");
    CHECK(aether_string_length(s2) == n1 && memcmp(aether_string_data(s2), t1, n1) == 0,
          "serializing twice gave two texts:\n  %.*s\n  %.*s", (int)(n1 > 300 ? 300 : n1), t1,
          (int)(aether_string_length(s2) > 300 ? 300 : aether_string_length(s2)), aether_string_data(s2));

    string_release(s2);
    string_release(s1);
    json_free(v2);
    json_free(v);
    free(doc);
    return 0;
}
