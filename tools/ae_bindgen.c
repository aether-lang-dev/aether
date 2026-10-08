/* ae bindgen consts — import simple C macro constants into Aether (#1245).
 *
 * Ports like Aedis hand-copy hundreds of C flag macros (SRI_O_DOWN, client
 * flags, log levels, error codes) into .ae consts, which drifts silently
 * when the C header changes. This generates that file from the header.
 *
 * The C preprocessor is the only tool that evaluates C macros correctly,
 * so it does the work here rather than a regex over #define lines:
 *
 *   1. `cc -E -dM <header>` discovers every object-like macro name.
 *   2. A probe file (`@AE@ NAME NAME` per candidate) run through `cc -E`
 *      yields each macro's FULL expansion, nested macros included.
 *   3. Expansions that are integer constant expressions are folded by the
 *      evaluator below; string-literal expansions pass through with
 *      adjacent-literal concatenation; everything else (function-like
 *      macros, casts, identifiers) is skipped and listed in a trailing
 *      comment so the omission is visible, never silent.
 *
 * Nothing is executed; both steps are preprocessor-only.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ae_line.h"

#include <stdarg.h>

#ifdef _WIN32
#  include <process.h>
#  ifndef getpid
#    define getpid _getpid
#  endif
#  ifndef popen
#    define popen  _popen
#    define pclose _pclose
#  endif
#  define BG_ERR_SINK "2>NUL"
#else
#  include <unistd.h>
#  define BG_ERR_SINK "2>/dev/null"
#endif

/* Open a pipe on a command whose first token is a quoted program path.
 *
 * On Windows `popen` hands the string to `cmd.exe /c`, which strips the
 * FIRST and LAST quote of the whole line before parsing. A command that
 * legitimately begins with a quoted path (needed when the path contains
 * spaces, e.g. the WinLibs gcc under "C:/Users/.../Program Files") is
 * therefore mangled into `gcc" -E -dM "C:` and fails with "is not
 * recognized as an internal or external command". cmd.exe's documented
 * remedy is an extra outer pair, which it consumes and the inner quotes
 * survive intact. Verified on MSYS2 + MinGW: the current form yields 0
 * lines, the wrapped form the full macro dump.
 *
 * POSIX /bin/sh has no such rule, so the command passes through as-is. */
static FILE* bg_popen(const char* cmd) {
#ifdef _WIN32
    size_t n = strlen(cmd) + 3;
    char* wrapped = (char*)malloc(n);
    if (!wrapped) return NULL;
    snprintf(wrapped, n, "\"%s\"", cmd);
    FILE* p = popen(wrapped, "r");
    free(wrapped);
    return p;
#else
    return popen(cmd, "r");
#endif
}

#define BG_NAME_MAX     128

typedef struct {
    char name[BG_NAME_MAX];
    /* Heap, held whole (NULL until the preprocessor reports it): a 1 KB
     * copy cut a longer one, and `1+1+...+1` imported as a smaller number
     * (#2536). */
    char* expansion;
    /* 0 = pending, 1 = integer, 2 = string, 3 = float, -1 = skipped */
    int  kind;
    long long ival;
} BgMacro;

/* Grown as needed: a fixed 4096 dropped the candidates past it, unlisted
 * even among the skipped ones (#2537). */
typedef struct {
    BgMacro* macros;
    int count;
    int cap;
} BgSet;

static void bg_set_free(BgSet* set) {
    for (int i = 0; i < set->count; i++) free(set->macros[i].expansion);
    free(set->macros);
}

/* ---- integer constant-expression evaluator ------------------------------
 * Grammar (C precedence, the subset macros use):
 *   or:    xor  ('|' xor)*
 *   xor:   and  ('^' and)*
 *   and:   shift('&' shift)*
 *   shift: add  (('<<'|'>>') add)*
 *   add:   mul  (('+'|'-') mul)*
 *   mul:   unary(('*'|'/'|'%') unary)*
 *   unary: ('-'|'+'|'~')* primary
 *   primary: integer | char-literal | '(' or ')'
 * Anything else (identifiers, casts, floats, ',') fails the parse and the
 * macro is skipped. Suffixes u/U/l/L on integers are accepted and ignored.
 */
typedef struct {
    const char* p;
    int failed;
} BgEval;

static void bg_skip_ws(BgEval* e) {
    while (*e->p == ' ' || *e->p == '\t') e->p++;
}

static long long bg_parse_or(BgEval* e);

static long long bg_parse_primary(BgEval* e) {
    bg_skip_ws(e);
    if (*e->p == '(') {
        e->p++;
        long long v = bg_parse_or(e);
        bg_skip_ws(e);
        if (*e->p != ')') { e->failed = 1; return 0; }
        e->p++;
        return v;
    }
    if (*e->p == '\'') {
        /* Char literal: 'x' or a single escape. Multi-char literals are
         * implementation-defined in C; skip those. */
        e->p++;
        long long v;
        if (*e->p == '\\') {
            e->p++;
            switch (*e->p) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = '\0'; break;
                case '\\': v = '\\'; break;
                case '\'': v = '\''; break;
                default: e->failed = 1; return 0;
            }
            e->p++;
        } else if (*e->p && *e->p != '\'') {
            v = (unsigned char)*e->p;
            e->p++;
        } else {
            e->failed = 1; return 0;
        }
        if (*e->p != '\'') { e->failed = 1; return 0; }
        e->p++;
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        char* end = NULL;
        /* strtoull handles 0x / 0 / decimal; the cast preserves the bit
         * pattern for full-width unsigned literals like 0xFFFFFFFF. */
        unsigned long long uv = strtoull(e->p, &end, 0);
        if (end == e->p) { e->failed = 1; return 0; }
        e->p = end;
        while (*e->p == 'u' || *e->p == 'U' || *e->p == 'l' || *e->p == 'L')
            e->p++;
        /* A float slipped past strtoull (e.g. "1.5" parses "1"): reject. */
        if (*e->p == '.' || *e->p == 'e' || *e->p == 'E' ||
            *e->p == 'f' || *e->p == 'F') { e->failed = 1; return 0; }
        return (long long)uv;
    }
    e->failed = 1;
    return 0;
}

static long long bg_parse_unary(BgEval* e) {
    bg_skip_ws(e);
    if (*e->p == '-') { e->p++; return -bg_parse_unary(e); }
    if (*e->p == '+') { e->p++; return  bg_parse_unary(e); }
    if (*e->p == '~') { e->p++; return ~bg_parse_unary(e); }
    return bg_parse_primary(e);
}

static long long bg_parse_mul(BgEval* e) {
    long long v = bg_parse_unary(e);
    for (;;) {
        bg_skip_ws(e);
        if (*e->p == '*') { e->p++; v *= bg_parse_unary(e); }
        else if (*e->p == '/' ) {
            e->p++;
            long long d = bg_parse_unary(e);
            if (d == 0) { e->failed = 1; return 0; }
            v /= d;
        } else if (*e->p == '%') {
            e->p++;
            long long d = bg_parse_unary(e);
            if (d == 0) { e->failed = 1; return 0; }
            v %= d;
        } else break;
    }
    return v;
}

static long long bg_parse_add(BgEval* e) {
    long long v = bg_parse_mul(e);
    for (;;) {
        bg_skip_ws(e);
        if (*e->p == '+') { e->p++; v += bg_parse_mul(e); }
        else if (*e->p == '-') { e->p++; v -= bg_parse_mul(e); }
        else break;
    }
    return v;
}

static long long bg_parse_shift(BgEval* e) {
    long long v = bg_parse_add(e);
    for (;;) {
        bg_skip_ws(e);
        if (e->p[0] == '<' && e->p[1] == '<') { e->p += 2; v <<= bg_parse_add(e); }
        else if (e->p[0] == '>' && e->p[1] == '>') { e->p += 2; v >>= bg_parse_add(e); }
        else break;
    }
    return v;
}

static long long bg_parse_and(BgEval* e) {
    long long v = bg_parse_shift(e);
    for (;;) {
        bg_skip_ws(e);
        /* single '&' only; '&&' is not a constant-flag expression */
        if (e->p[0] == '&' && e->p[1] != '&') { e->p++; v &= bg_parse_shift(e); }
        else break;
    }
    return v;
}

static long long bg_parse_xor(BgEval* e) {
    long long v = bg_parse_and(e);
    for (;;) {
        bg_skip_ws(e);
        if (*e->p == '^') { e->p++; v ^= bg_parse_and(e); }
        else break;
    }
    return v;
}

static long long bg_parse_or(BgEval* e) {
    long long v = bg_parse_xor(e);
    for (;;) {
        bg_skip_ws(e);
        if (e->p[0] == '|' && e->p[1] != '|') { e->p++; v |= bg_parse_xor(e); }
        else break;
    }
    return v;
}

/* Try the expansion as an integer constant expression. Returns 1 and fills
 * *out when the WHOLE expansion parses; 0 otherwise. */
static int bg_eval_int(const char* s, long long* out) {
    BgEval e = { s, 0 };
    long long v = bg_parse_or(&e);
    bg_skip_ws(&e);
    if (e.failed || *e.p != '\0') return 0;
    *out = v;
    return 1;
}

/* Try the expansion as one-or-more juxtaposed string literals ("a" "b").
 * Writes the concatenated literal (with its escapes verbatim, wrapped in a
 * single pair of quotes) into out. Returns 1 on success. */
static int bg_eval_string(const char* s, char* out, size_t out_sz) {
    size_t pos = 0;
    int saw_any = 0;
    if (pos < out_sz) out[pos++] = '"';
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;
        if (*s != '"') return 0;
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1]) {
                if (pos + 2 >= out_sz) return 0;
                out[pos++] = *s++;
                out[pos++] = *s++;
                continue;
            }
            if (pos + 1 >= out_sz) return 0;
            out[pos++] = *s++;
        }
        if (*s != '"') return 0;
        s++;
        saw_any = 1;
    }
    if (!saw_any || pos + 2 >= out_sz) return 0;
    out[pos++] = '"';
    out[pos] = '\0';
    return 1;
}

/* Single float literal (1.5, 1e6, .5f): passes through with its C suffix
 * stripped. Returns 1 on success. */
static int bg_eval_float(const char* s, char* out, size_t out_sz) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = NULL;
    strtod(s, &end);
    if (end == s) return 0;
    size_t n = (size_t)(end - s);
    const char* tail = end;
    if (*tail == 'f' || *tail == 'F' || *tail == 'l' || *tail == 'L') tail++;
    while (*tail == ' ' || *tail == '\t') tail++;
    if (*tail != '\0') return 0;
    /* Must actually contain a float marker, else it was an integer. */
    if (!memchr(s, '.', n) && !memchr(s, 'e', n) && !memchr(s, 'E', n)) return 0;
    if (n + 1 > out_sz) return 0;
    memcpy(out, s, n);
    out[n] = '\0';
    return 1;
}

/* ---- pipeline ------------------------------------------------------------ */

static int bg_is_ident(const char* s) {
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (s++; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

/* Collect object-like, user-named macro names from one `cc -E -dM` run
 * into a NUL-separated list (a double NUL ends it) the caller frees. Names
 * with a leading underscore (reserved namespace) and function-like macros
 * are not importable constants. 1 when done, 0 when the preprocessor could
 * not be run, -1 when out of memory.
 *
 * Whole lines, and as many names as there are (#2537): a line read in
 * 4 KB pieces gave a later piece of a long definition that began with
 * `#define ` (text in a string) as a macro of its own, and a 256 KB list
 * dropped the names past it, which a large header (windows.h) reaches. */
/* A command in a string sized to fit, for free(); NULL when out of memory.
 * A 4 KB buffer cut a long compiler, header or temp path (#2546). */
static char* bg_command(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char* cmd = (char*)malloc((size_t)n + 1);
    if (!cmd) return NULL;
    va_start(ap, fmt);
    vsnprintf(cmd, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return cmd;
}

static int bg_dump_names(const char* cc, const char* file,
                         const char* include_flags, char** names) {
    *names = NULL;
    char* cmd = bg_command("%s -E -dM %s \"%s\" " BG_ERR_SINK, cc, include_flags, file);
    if (!cmd) {
        fprintf(stderr, "ae bindgen: out of memory\n");
        return -1;
    }
    FILE* p = bg_popen(cmd);
    free(cmd);
    if (!p) return 0;
    size_t pos = 0, cap = 4096;
    char* out = (char*)malloc(cap);
    char* line = NULL;
    size_t line_cap = 0;
    int got = out ? 0 : -1;
    while (out && (got = ae_read_line(p, &line, &line_cap)) > 0) {
        if (strncmp(line, "#define ", 8) != 0) continue;
        char* name = line + 8;
        char* sp = strpbrk(name, " (\t\n");
        if (!sp || *sp == '(') continue;          /* function-like: skip */
        *sp = '\0';
        if (name[0] == '_') continue;             /* reserved namespace */
        if (!bg_is_ident(name)) continue;
        size_t n = strlen(name);
        if (n == 0 || n >= BG_NAME_MAX) continue;
        if (pos + n + 2 > cap) {
            char* grown = (char*)realloc(out, cap * 2);
            if (!grown) { got = -1; break; }
            out = grown;
            cap *= 2;
        }
        memcpy(out + pos, name, n + 1);
        pos += n + 1;
    }
    free(line);
    pclose(p);
    if (got < 0) {
        free(out);
        fprintf(stderr, "ae bindgen: out of memory\n");
        return -1;
    }
    out[pos] = '\0';                              /* double-NUL terminator */
    *names = out;
    return 1;
}

static int bg_name_in(const char* names, const char* name) {
    for (const char* q = names; *q; q += strlen(q) + 1)
        if (strcmp(q, name) == 0) return 1;
    return 0;
}

/* Stage 1: candidates = macros defined by the HEADER, computed as the set
 * difference against a baseline dump of an empty file. `-dM` includes the
 * compiler's predefined macros, and those are not all underscore-reserved:
 * Linux gcc predefines `linux` and `unix`, which imported as consts and
 * made the output environment-dependent. 1 when there are candidates, 0
 * when there are none, -1 when out of memory (said). */
static int bg_discover(const char* cc, const char* header,
                       const char* include_flags, const char* match,
                       BgSet* set, const char* tmp_dir) {
    /* Named for the process: with one fixed name, two bindgen runs at once
     * (parallel tests) wrote each other's probe and read the wrong macros. */
    char empty_path[1200];
    snprintf(empty_path, sizeof(empty_path), "%s/ae_bindgen_empty_%d.c", tmp_dir, (int)getpid());
    FILE* f = fopen(empty_path, "w");
    if (!f) return 0;
    if (fclose(f) != 0) { remove(empty_path); return 0; }

    char* base_names = NULL;
    char* hdr_names = NULL;
    int ok = bg_dump_names(cc, empty_path, "", &base_names);
    remove(empty_path);
    if (ok > 0) ok = bg_dump_names(cc, header, include_flags, &hdr_names);

    for (const char* q = hdr_names; ok > 0 && *q; q += strlen(q) + 1) {
        if (bg_name_in(base_names, q)) continue;   /* compiler-predefined */
        if (match && *match && strncmp(q, match, strlen(match)) != 0) continue;
        size_t qn = strlen(q);
        if (qn >= BG_NAME_MAX) continue;   /* already enforced by the dump */
        if (set->count == set->cap) {
            int ncap = set->cap ? set->cap * 2 : 256;
            BgMacro* grown = (BgMacro*)realloc(set->macros, (size_t)ncap * sizeof(BgMacro));
            if (!grown) {
                fprintf(stderr, "ae bindgen: out of memory\n");
                ok = -1;
                break;
            }
            set->macros = grown;
            set->cap = ncap;
        }
        BgMacro* m = &set->macros[set->count++];
        memset(m, 0, sizeof(*m));
        memcpy(m->name, q, qn + 1);
    }
    free(base_names);
    free(hdr_names);
    if (ok < 0) return -1;
    return ok > 0 && set->count > 0;
}

/* Stage 2: full expansion of every candidate through `cc -E` on a probe
 * that includes the header. The @AE@ marker keys each output line back to
 * its macro; the preprocessor fully resolves nested macros for us. 1 when
 * done, 0 when the preprocessor could not be run, -1 when out of memory. */
static int bg_expand(const char* cc, const char* header,
                     const char* include_flags, BgSet* set,
                     const char* tmp_dir) {
    char probe_path[1200];
    snprintf(probe_path, sizeof(probe_path), "%s/ae_bindgen_probe_%d.c", tmp_dir, (int)getpid());
    FILE* f = fopen(probe_path, "w");
    if (!f) return 0;
    fprintf(f, "#include \"%s\"\n", header);
    /* The key is a string literal because the preprocessor expands every
     * bare occurrence of the macro name, including one meant as a label. */
    for (int i = 0; i < set->count; i++)
        fprintf(f, "@AE@ \"%s\" %s\n", set->macros[i].name, set->macros[i].name);
    if (fclose(f) != 0) { remove(probe_path); return 0; }

    char* cmd = bg_command("%s -E %s \"%s\" " BG_ERR_SINK, cc, include_flags, probe_path);
    if (!cmd) {
        remove(probe_path);
        fprintf(stderr, "ae bindgen: out of memory\n");
        return -1;
    }
    FILE* p = bg_popen(cmd);
    free(cmd);
    if (!p) { remove(probe_path); return 0; }
    /* Whole lines (#2536): one expansion is one line, of any length. */
    char* line = NULL;
    size_t line_cap = 0;
    int got;
    int next = 0;
    while ((got = ae_read_line(p, &line, &line_cap)) > 0) {
        if (strncmp(line, "@AE@ \"", 6) != 0) continue;
        char* name = line + 6;
        char* endq = strchr(name, '"');
        if (!endq || endq[1] != ' ') continue;
        *endq = '\0';
        char* exp = endq + 2;
        size_t n = strlen(exp);
        while (n && (exp[n-1] == '\n' || exp[n-1] == '\r' || exp[n-1] == ' '))
            exp[--n] = '\0';
        /* The lines come back in probe order, so the search starts after the
         * last match: linear over the set, not quadratic, now that a large
         * header's thousands of macros are all candidates. */
        for (int k = 0; k < set->count; k++) {
            int i = (next + k) % set->count;
            if (strcmp(set->macros[i].name, name) == 0) {
                char* copy = (char*)malloc(n + 1);
                if (!copy) { got = -1; break; }
                memcpy(copy, exp, n + 1);
                free(set->macros[i].expansion);
                set->macros[i].expansion = copy;
                next = i + 1;
                break;
            }
        }
        if (got < 0) break;
    }
    free(line);
    pclose(p);
    remove(probe_path);
    if (got < 0) {
        fprintf(stderr, "ae bindgen: out of memory\n");
        return -1;
    }
    return 1;
}

int ae_bindgen_consts(const char* cc, int argc, char** argv) {
    const char* header = NULL;
    const char* out_path = NULL;
    const char* match = NULL;
    char include_flags[2048] = "";
    size_t inc_pos = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            int w = snprintf(include_flags + inc_pos,
                             sizeof(include_flags) - inc_pos,
                             "-I\"%s\" ", argv[++i]);
            if (w < 0 || (size_t)w >= sizeof(include_flags) - inc_pos) {
                fprintf(stderr, "ae bindgen: too many -I flags\n");
                return 1;
            }
            inc_pos += (size_t)w;
        } else if (strcmp(argv[i], "--match") == 0 && i + 1 < argc) {
            match = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ae bindgen consts <header.h> [-I dir]... [--match PREFIX] [-o out.ae]\n"
                   "  Imports object-like C macros that expand to integer constant\n"
                   "  expressions, string literals, or float literals as Aether consts.\n"
                   "  Skipped macros are listed in a comment at the end of the output.\n"
                   "  With no -o, writes to stdout.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ae bindgen: unknown flag '%s' (see ae bindgen consts --help)\n", argv[i]);
            return 1;
        } else if (!header) {
            header = argv[i];
        } else {
            fprintf(stderr, "ae bindgen: exactly one header expected\n");
            return 1;
        }
    }
    if (!header) {
        fprintf(stderr, "Usage: ae bindgen consts <header.h> [-I dir]... [--match PREFIX] [-o out.ae]\n");
        return 1;
    }

    BgSet set = { NULL, 0, 0 };

    const char* tmp_dir = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp_dir || !*tmp_dir) tmp_dir = getenv("TEMP");
    if (!tmp_dir || !*tmp_dir) tmp_dir = ".";
#else
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
#endif

    int discovered = bg_discover(cc, header, include_flags, match, &set, tmp_dir);
    if (discovered <= 0) {
        if (discovered == 0)
            fprintf(stderr, "ae bindgen: no importable macros found in %s "
                            "(is the path right? does it preprocess standalone?)\n", header);
        bg_set_free(&set);
        return 1;
    }

    int expanded = bg_expand(cc, header, include_flags, &set, tmp_dir);
    if (expanded <= 0) {
        if (expanded == 0) fprintf(stderr, "ae bindgen: preprocessor expansion failed\n");
        bg_set_free(&set);
        return 1;
    }

    /* Classify every expansion. A string or float literal is rewritten into
     * a buffer as long as the expansion, which always holds it. */
    int imported = 0;
    for (int i = 0; i < set.count; i++) {
        BgMacro* m = &set.macros[i];
        if (m->kind == -1 || !m->expansion || m->expansion[0] == '\0') { m->kind = -1; continue; }
        if (bg_eval_int(m->expansion, &m->ival)) {
            m->kind = 1; imported++;
            continue;
        }
        size_t buf_sz = strlen(m->expansion) + 3;
        char* buf = (char*)malloc(buf_sz);
        if (!buf) {
            fprintf(stderr, "ae bindgen: out of memory\n");
            bg_set_free(&set);
            return 1;
        }
        if (bg_eval_string(m->expansion, buf, buf_sz)) {
            m->kind = 2; imported++;
        } else if (bg_eval_float(m->expansion, buf, buf_sz)) {
            m->kind = 3; imported++;
        } else {
            m->kind = -1;
        }
        if (m->kind > 0) {
            free(m->expansion);
            m->expansion = buf;
        } else {
            free(buf);
        }
    }
    if (imported == 0) {
        fprintf(stderr, "ae bindgen: %d macros found but none expand to an "
                        "importable constant\n", set.count);
        bg_set_free(&set);
        return 1;
    }

    FILE* out = out_path ? fopen(out_path, "w") : stdout;
    if (!out) {
        fprintf(stderr, "ae bindgen: cannot open '%s' for writing\n", out_path);
        bg_set_free(&set);
        return 1;
    }

    fprintf(out, "// Generated by `ae bindgen consts %s`%s%s, do not edit.\n",
            header, match ? " --match " : "", match ? match : "");
    fprintf(out, "// Regenerate after the header changes; skipped macros are listed at the end.\n\n");

    fprintf(out, "exports (");
    int first = 1;
    for (int i = 0; i < set.count; i++) {
        if (set.macros[i].kind <= 0) continue;
        fprintf(out, "%s%s", first ? "" : ", ", set.macros[i].name);
        first = 0;
    }
    fprintf(out, ")\n\n");

    for (int i = 0; i < set.count; i++) {
        BgMacro* m = &set.macros[i];
        switch (m->kind) {
            case 1: fprintf(out, "const %s = %lld\n", m->name, m->ival); break;
            case 2:
            case 3: fprintf(out, "const %s = %s\n", m->name, m->expansion); break;
            default: break;
        }
    }

    int skipped = 0;
    for (int i = 0; i < set.count; i++) if (set.macros[i].kind <= 0) skipped++;
    if (skipped > 0) {
        fprintf(out, "\n// Skipped (not a scalar constant expression):\n");
        for (int i = 0; i < set.count; i++) {
            if (set.macros[i].kind > 0) continue;
            const char* e = set.macros[i].expansion ? set.macros[i].expansion : "";
            fprintf(out, "//   %s%s%.60s\n", set.macros[i].name, e[0] ? " = " : "", e);
        }
    }

    int write_failed = ferror(out) != 0;
    if (out_path) {
        if (fclose(out) != 0) write_failed = 1;
    }
    bg_set_free(&set);
    if (write_failed) {
        fprintf(stderr, "ae bindgen: failed writing '%s'\n",
                out_path ? out_path : "stdout");
        return 1;
    }
    fprintf(stderr, "ae bindgen: %d consts imported, %d skipped\n", imported, skipped);
    return 0;
}
