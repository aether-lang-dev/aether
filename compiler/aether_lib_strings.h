/* aether_lib_strings.h — which values a library's catalog spells hold a
 * counted string. Two copies of the runtime cannot free each other's, so
 * `ae` refuses to import such a value between a program and a library that
 * keeps a runtime of its own (#2931, ae_refuse_split_ownership in
 * tools/ae.c), and the compiler names the library's `own_runtime` duty for
 * the same values (#3481, emit_lib_metadata in compiler/codegen/codegen.c).
 * Both read the catalog's own spellings through this header, so what one
 * refuses and the other names cannot drift.
 *
 * Standalone, like aether_lib_path.h: `tools/ae.c` reads a built library's
 * catalog and has no AST. */

#ifndef AETHER_LIB_STRINGS_H
#define AETHER_LIB_STRINGS_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The type of field `k` of the struct the catalog records under the first
 * `len` bytes of `name`, as the catalog spells it; NULL past its last field,
 * or when the catalog records no such struct. */
typedef const char* (*AetherLibFieldType)(const void* ctx, const char* name, size_t len, int k);

/* A walk's state: the catalog, and the structs it has entered. A struct
 * entered once is not entered again, so a struct that names itself, or
 * many that name each other, cost one visit each, and the walk ends. Every
 * struct it enters is kept (#3574): 64 slots and a depth of 64 left a
 * string 65 structs down unseen, so a value holding one crossed between
 * two runtimes. */
typedef struct {
    AetherLibFieldType field;
    const void* ctx;
    const char** seen;
    size_t* seen_len;
    int seen_count;
    int seen_cap;
} AetherLibStringWalk;

static inline int aether_lib_walk_holds_string(AetherLibStringWalk* w, const char* type) {
    if (!type) return 0;
    /* A parameter's `@keep` or `@into` (#3013) is not part of its type. */
    while (*type == ' ' || *type == '@') {
        if (*type == '@')
            while (*type && *type != ' ') type++;
        else
            type++;
    }
    /* A typed pointer reaches its struct's strings: the program's stores
     * through it release them. */
    while (*type == ' ' || *type == '*') type++;
    if (strncmp(type, "struct ", 7) == 0) type += 7;
    /* A tuple holds what its elements hold. */
    if (*type == '(') {
        const char* el = type + 1;
        int nest = 0;
        for (const char* p = el; *p; p++) {
            if (*p == '(' || *p == '[') nest++;
            else if ((*p == ')' || *p == ']') && nest > 0) nest--;
            else if (nest == 0 && (*p == ',' || *p == ')')) {
                if (aether_lib_walk_holds_string(w, el)) return 1;
                if (*p == ')') return 0;
                el = p + 1;
            }
        }
        return 0;
    }
    /* A function value holds none. */
    if (strncmp(type, "fn", 2) == 0 && !(isalnum((unsigned char)type[2]) || type[2] == '_'))
        return 0;
    /* The name its spelling starts with: `string`, `string?`, `string[]`
     * and `string!` all hold one; a struct's name holds what its fields
     * hold. */
    size_t n = 0;
    while (isalnum((unsigned char)type[n]) || type[n] == '_') n++;
    if (n == 6 && strncmp(type, "string", 6) == 0) return 1;
    if (n == 0 || !w->field) return 0;
    for (int i = 0; i < w->seen_count; i++)
        if (w->seen_len[i] == n && strncmp(w->seen[i], type, n) == 0) return 0;
    if (w->seen_count == w->seen_cap) {
        int cap = w->seen_cap ? w->seen_cap * 2 : 16;
        const char** seen = (const char**)realloc((void*)w->seen, sizeof(char*) * (size_t)cap);
        if (seen) w->seen = seen;
        size_t* seen_len = seen ? (size_t*)realloc(w->seen_len, sizeof(size_t) * (size_t)cap) : NULL;
        if (seen_len) w->seen_len = seen_len;
        /* Out of memory: say it holds one, the side that refuses. */
        if (!seen || !seen_len) return 1;
        w->seen_cap = cap;
    }
    w->seen[w->seen_count] = type;
    w->seen_len[w->seen_count] = n;
    w->seen_count++;
    for (int k = 0; ; k++) {
        const char* ft = w->field(w->ctx, type, n, k);
        if (!ft) return 0;
        if (aether_lib_walk_holds_string(w, ft)) return 1;
    }
}

/* Whether a value of the catalog type `type` holds a counted string,
 * itself or in a field, element or pointee. */
static inline int aether_lib_type_holds_string(const char* type, AetherLibFieldType field,
                                               const void* ctx) {
    AetherLibStringWalk w;
    w.field = field;
    w.ctx = ctx;
    w.seen = NULL;
    w.seen_len = NULL;
    w.seen_count = 0;
    w.seen_cap = 0;
    int holds = aether_lib_walk_holds_string(&w, type);
    free((void*)w.seen);
    free(w.seen_len);
    return holds;
}

/* The type a signature ("(a: A, b: B) -> R" or "(A, B) -> R") passes that
 * holds a counted string, copied to `out`; NULL when there is none. A plain
 * string crosses as a copy of itself, so it does not count here. */
static inline const char* aether_lib_signature_string_type(const char* sig, AetherLibFieldType field,
                                                           const void* ctx, char* out, size_t cap) {
    if (!sig || cap == 0) return NULL;
    int depth = 0;
    const char* start = sig[0] == '(' ? sig + 1 : sig;
    for (const char* p = start; ; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && depth > 0) depth--;
        else if (*p == '\0' || (depth == 0 && (*p == ',' || *p == ')'))) {
            const char* t = start;
            for (const char* q = start; q < p; q++)
                if (*q == ':') { t = q + 1; break; }
            while (t < p && (*t == ' ' || *t == '-' || *t == '>')) t++;
            size_t len = (size_t)(p - t);
            while (len > 0 && t[len - 1] == ' ') len--;
            /* The whole spelling decides (#3574); `out` only shows it. Read
             * through `out`, a 200-byte one cut a long spelling, and a
             * struct holding a string was not found by its cut name. */
            char* type = (char*)malloc(len + 1);
            int holds = 1;   /* out of memory: the side that refuses */
            if (type) {
                memcpy(type, t, len);
                type[len] = '\0';
                const char* bare = type;
                while (*bare == '*') bare++;
                if (strncmp(bare, "struct ", 7) == 0) bare += 7;
                holds = strcmp(bare, "string") != 0 && aether_lib_type_holds_string(type, field, ctx);
                free(type);
            }
            if (holds) {
                snprintf(out, cap, "%.*s", (int)len, t);
                return out;
            }
            if (*p == '\0') return NULL;
            start = p + 1;
            if (*p == ')') {
                /* The parameters end here; the result follows the arrow. */
                const char* arrow = strstr(start, "->");
                if (!arrow) return NULL;
                p = arrow + 1;
                start = arrow + 2;
            }
        }
    }
}

#endif /* AETHER_LIB_STRINGS_H */
