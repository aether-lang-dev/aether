/*
 * Aether Documentation Generator
 *
 * Generates searchable HTML documentation for the Aether standard library
 * from the PUBLIC `.ae` API surface.
 *
 * For each std/<module>/module.ae the generator parses the `exports (...)`
 * list and, for every exported name, locates its definition further down the
 * file — an `extern raw(...) -> T` declaration, an Aether wrapper
 * `name(params) -> Ret { ... }`, a `const NAME = value`, or a `struct Name`.
 * The rendered documentation reflects that .ae layer: real parameter
 * names+types, real return types (including tuple returns like
 * `(string, string)`), and the `//` doc-comment block immediately above the
 * definition.
 *
 * This documents the layer users actually call (`json.get_string`,
 * `fs.read`, `os.run_capture`, ...) rather than the underlying C symbols.
 *
 * Usage: docgen <std_dir> <output_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
/* MinGW's mkdir takes the path only; POSIX's also takes a mode, which
 * Windows has no use for. Without this the generator did not build there. */
#define docgen_mkdir(p) _mkdir(p)
#else
#define docgen_mkdir(p) mkdir((p), 0755)
#endif

#define MAX_FUNCTIONS 256
#define MAX_EXPORTS 256
#define MAX_LINE 4096
#define MAX_DOC 6144
#define MAX_FILE_BYTES (1 << 20)   /* 1 MiB per module.ae — generous */

typedef enum {
    DEF_FUNCTION,   /* Aether wrapper: name(params) -> Ret { ... }            */
    DEF_EXTERN,     /* extern name(params) -> Ret                             */
    DEF_CONST,      /* const NAME = value                                     */
    DEF_STRUCT      /* struct Name { ... }                                    */
} DefKind;

typedef struct {
    char name[128];
    char params[1024];        /* rendered "p1: T1, p2: T2"                    */
    char return_type[256];    /* rendered return, "" when none / void        */
    char doc[MAX_DOC];        /* doc-comment block                           */
    char value[256];          /* const value (DEF_CONST only)                */
    DefKind kind;
    int has_def;              /* 1 once a definition was found               */
} Function;

typedef struct {
    char name[64];
    char description[1024];
    Function functions[MAX_FUNCTIONS];
    int function_count;
} Module;

/* One heap block per module, found through a growing array of pointers. A
 * Module carries MAX_FUNCTIONS full doc buffers (about 2 MB), so the old
 * fixed `modules[64]` could not simply be enlarged, and past 64 the loader
 * returned NULL and the remaining modules (std has 87) were dropped without
 * a word: every page after `snapshot` alphabetically went stale. */
static Module** modules = NULL;
static int module_count = 0;
static int module_cap = 0;

/* ---------------------------------------------------------------------- */
/* Small string helpers                                                    */
/* ---------------------------------------------------------------------- */

static char* trim(char* str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static int is_ident_char(int c) {
    return isalnum(c) || c == '_';
}

/* Collapse runs of whitespace to single spaces, in place. */
static void collapse_ws(char* s) {
    char* w = s;
    int prev_space = 0;
    for (char* r = s; *r; r++) {
        if (isspace((unsigned char)*r)) {
            if (!prev_space) { *w++ = ' '; prev_space = 1; }
        } else {
            *w++ = *r;
            prev_space = 0;
        }
    }
    *w = '\0';
    trim(s);
}

/* Strip Aether memory annotations (@heap, @retain, @borrow, ...) from a
 * type/param fragment. They are internal lifetime hints, not part of the
 * user-facing type. */
static void strip_annotations(char* s, size_t cap) {
    char out[1024];
    size_t j = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n && j < sizeof(out) - 1; ) {
        if (s[i] == '@') {
            i++;
            while (i < n && is_ident_char((unsigned char)s[i])) i++;  /* skip word */
            /* skip a single following space so we don't leave doubled spaces */
            if (i < n && s[i] == ' ') i++;
            continue;
        }
        out[j++] = s[i++];
    }
    out[j] = '\0';
    strncpy(s, out, cap - 1);
    s[cap - 1] = '\0';
    collapse_ws(s);
    /* Annotation removal can leave a space before a separator
     * (`string @heap,` -> `string ,`); drop it. */
    char* w = s;
    for (char* r = s; *r; r++) {
        if (*r == ' ' && (r[1] == ',' || r[1] == ')')) continue;
        *w++ = *r;
    }
    *w = '\0';
}

/* ---------------------------------------------------------------------- */
/* HTML escaping                                                           */
/* ---------------------------------------------------------------------- */

static void html_escape(const char* src, char* dest, size_t dest_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dest_size - 7; i++) {
        switch (src[i]) {
            case '<': strcpy(dest + j, "&lt;");  j += 4; break;
            case '>': strcpy(dest + j, "&gt;");  j += 4; break;
            case '&': strcpy(dest + j, "&amp;"); j += 5; break;
            case '"': strcpy(dest + j, "&quot;"); j += 6; break;
            default:  dest[j++] = src[i]; break;
        }
    }
    dest[j] = '\0';
}

/* ---------------------------------------------------------------------- */
/* Module loading                                                          */
/* ---------------------------------------------------------------------- */

static Module* find_or_create_module(const char* name) {
    for (int i = 0; i < module_count; i++) {
        if (strcmp(modules[i]->name, name) == 0) return modules[i];
    }
    if (module_count >= module_cap) {
        int cap = module_cap ? module_cap * 2 : 64;
        Module** grown = realloc(modules, (size_t)cap * sizeof(*grown));
        if (!grown) return NULL;
        modules = grown;
        module_cap = cap;
    }
    Module* m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    strncpy(m->name, name, sizeof(m->name) - 1);
    modules[module_count++] = m;
    return m;
}

/* Read a whole file into a freshly-malloc'd NUL-terminated buffer. Returns
 * NULL on failure (caller frees on success). */
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > MAX_FILE_BYTES) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* ---------------------------------------------------------------------- */
/* exports (...) list parsing                                              */
/* ---------------------------------------------------------------------- */

/* Collect comma/whitespace-separated identifiers from the `exports ( ... )`
 * block into `out`. Returns the export count. Inline `//` comments inside
 * the block are skipped. */
static int parse_exports(const char* src, char out[][128], int max) {
    const char* p = strstr(src, "exports");
    if (!p) return 0;
    /* advance to the opening paren */
    p = strchr(p, '(');
    if (!p) return 0;
    p++;

    int count = 0;
    while (*p && *p != ')') {
        /* skip line comments inside the export block */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (!is_ident_char((unsigned char)*p)) { p++; continue; }
        /* read identifier */
        char ident[128];
        size_t j = 0;
        while (*p && is_ident_char((unsigned char)*p) && j < sizeof(ident) - 1) {
            ident[j++] = *p++;
        }
        ident[j] = '\0';
        if (count < max) {
            strncpy(out[count], ident, 127);
            out[count][127] = '\0';
            count++;
        }
    }
    return count;
}

/* ---------------------------------------------------------------------- */
/* Definition extraction                                                   */
/* ---------------------------------------------------------------------- */

/* Render a parameter list source fragment (text between the outer parens)
 * into a clean "p1: T1, p2: T2" string with annotations stripped. */
static void render_params(const char* raw, char* out, size_t out_size) {
    char work[1024];
    strncpy(work, raw, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    collapse_ws(work);

    char* t = trim(work);
    if (*t == '\0' || strcmp(t, "void") == 0) { out[0] = '\0'; return; }

    /* Split on top-level commas (params here never nest parens). */
    out[0] = '\0';
    char* save = NULL;
    char* tok = strtok_r(work, ",", &save);
    int first = 1;
    while (tok) {
        char piece[512];
        strncpy(piece, tok, sizeof(piece) - 1);
        piece[sizeof(piece) - 1] = '\0';
        strip_annotations(piece, sizeof(piece));
        char* pt = trim(piece);
        if (*pt) {
            if (!first) strncat(out, ", ", out_size - strlen(out) - 1);
            strncat(out, pt, out_size - strlen(out) - 1);
            first = 0;
        }
        tok = strtok_r(NULL, ",", &save);
    }
}

/* Given the source position just AFTER the closing paren of a signature,
 * capture the return type. Handles:
 *   -> Type
 *   -> (T1, T2, ...)
 *   -> *Struct
 *   (none)                 — extern / void function
 * The capture stops at `{`, `\n`, or end of source.  Annotations stripped. */
static void capture_return(const char* after_paren, char* out, size_t out_size) {
    out[0] = '\0';
    const char* p = after_paren;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '-' || p[1] != '>') return;   /* no return arrow */
    p += 2;
    while (*p == ' ' || *p == '\t') p++;

    char buf[256];
    size_t j = 0;
    if (*p == '(') {
        /* tuple return — copy up to and including the matching ')' */
        int depth = 0;
        while (*p && j < sizeof(buf) - 1) {
            if (*p == '(') depth++;
            else if (*p == ')') { depth--; buf[j++] = *p++; if (depth == 0) break; continue; }
            buf[j++] = *p++;
        }
    } else {
        /* scalar return — copy until '{', newline, or end */
        while (*p && *p != '{' && *p != '\n' && j < sizeof(buf) - 1) {
            buf[j++] = *p++;
        }
    }
    buf[j] = '\0';
    strip_annotations(buf, sizeof(buf));
    char* t = trim(buf);
    /* `void` reads as no return for documentation purposes */
    if (strcmp(t, "void") == 0) t[0] = '\0';
    strncpy(out, t, out_size - 1);
    out[out_size - 1] = '\0';
}

/* Try to read a `name(params) ...` definition starting at `start` (which
 * points at the first char of the name). On success fills name/params/return
 * and returns 1; returns 0 if this is not a callable definition. */
static int read_callable(const char* start, char* name_out,
                         char* params_out, char* return_out) {
    const char* p = start;
    /* identifier */
    size_t j = 0;
    char name[128];
    while (*p && is_ident_char((unsigned char)*p) && j < sizeof(name) - 1) {
        name[j++] = *p++;
    }
    name[j] = '\0';
    if (j == 0) return 0;

    /* whitespace then '(' */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return 0;
    p++;

    /* capture param text up to the matching ')' (params don't nest parens
     * in the .ae extern/wrapper grammar) */
    const char* params_start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth == 0) break;
        p++;
    }
    if (depth != 0) return 0;  /* unterminated */
    size_t plen = (size_t)(p - params_start);
    char raw_params[1024];
    if (plen >= sizeof(raw_params)) plen = sizeof(raw_params) - 1;
    memcpy(raw_params, params_start, plen);
    raw_params[plen] = '\0';
    p++;  /* skip ')' */

    strncpy(name_out, name, 127);
    name_out[127] = '\0';
    render_params(raw_params, params_out, 1024);
    capture_return(p, return_out, 256);
    return 1;
}

/* Find or create a function record for `name` in module `mod`. */
static Function* upsert_function(Module* mod, const char* name) {
    /* The `<module>.` namespace consumes a leading `<module>_` prefix when a
     * call is resolved (`string.pad_start` -> export `string_pad_start`), so
     * the call-site name we document is the suffix. Strip it for display;
     * exports without the prefix (e.g. json's `get_string`) are shown as-is. */
    size_t mlen = strlen(mod->name);
    if (strncmp(name, mod->name, mlen) == 0 && name[mlen] == '_' && name[mlen + 1] != '\0') {
        name += mlen + 1;
    }
    for (int i = 0; i < mod->function_count; i++) {
        if (strcmp(mod->functions[i].name, name) == 0) return &mod->functions[i];
    }
    if (mod->function_count >= MAX_FUNCTIONS) return NULL;
    Function* fn = &mod->functions[mod->function_count++];
    memset(fn, 0, sizeof(*fn));
    strncpy(fn->name, name, sizeof(fn->name) - 1);
    return fn;
}

/* Append a doc-comment line (text after `//`) to a pending-doc buffer. */
static void append_doc_line(char* doc, const char* text) {
    size_t cur = strlen(doc);
    size_t add = strlen(text);
    if (cur + add + 2 >= MAX_DOC) return;
    if (cur > 0) { doc[cur++] = ' '; doc[cur] = '\0'; }
    strcat(doc, text);
}

/* ---------------------------------------------------------------------- */
/* Inferred-return resolution                                              */
/* ---------------------------------------------------------------------- */

/* A module-local table mapping EVERY extern (exported or not) to its
 * declared return type, so thin wrappers written as `name(...) -> { return
 * some_extern(...) }` can inherit the real return shape the extern documents
 * (e.g. fs.copy -> fs_copy_raw -> "(int, int, string)"). */
typedef struct { char name[128]; char ret[256]; } ExternRet;
static ExternRet extern_rets[MAX_FUNCTIONS];
static int extern_ret_count = 0;

static void extern_ret_reset(void) { extern_ret_count = 0; }

static void extern_ret_add(const char* name, const char* ret) {
    if (extern_ret_count >= MAX_FUNCTIONS) return;
    strncpy(extern_rets[extern_ret_count].name, name, 127);
    extern_rets[extern_ret_count].name[127] = '\0';
    strncpy(extern_rets[extern_ret_count].ret, ret, 255);
    extern_rets[extern_ret_count].ret[255] = '\0';
    extern_ret_count++;
}

static const char* extern_ret_lookup(const char* name) {
    for (int i = 0; i < extern_ret_count; i++) {
        if (strcmp(extern_rets[i].name, name) == 0) return extern_rets[i].ret;
    }
    return NULL;
}

/* Best-effort type of a single return-expression token. */
static const char* infer_value_type(const char* expr) {
    char e[256];
    strncpy(e, expr, sizeof(e) - 1);
    e[sizeof(e) - 1] = '\0';
    char* t = trim(e);
    if (*t == '\0') return "";
    if (strcmp(t, "null") == 0) return "ptr";
    if (t[0] == '"') return "string";                       /* string literal */
    if (t[0] == '-' || isdigit((unsigned char)t[0])) {      /* numeric literal */
        return strchr(t, '.') ? "float" : "int";
    }
    return "";   /* unknown — left as an unnamed tuple slot */
}

/* Resolve an inferred (`-> {`) wrapper return by inspecting its body.
 * `body` points just after the opening brace of the function. Writes a
 * rendered return type into `out` ("" if nothing could be inferred). */
static void infer_return_from_body(const char* body, char* out, size_t out_size) {
    out[0] = '\0';
    /* find the first `return` keyword at a statement boundary, skipping line
     * comments and string literals so prose like `// Returns ...` and inline
     * "return" text never mis-fires */
    const char* p = body;
    const char* ret_kw = NULL;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') {            /* skip line comment */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '"') {                              /* skip string literal */
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p) p++;
            continue;
        }
        if (strncmp(p, "return", 6) == 0 &&
            (p == body || !is_ident_char((unsigned char)p[-1])) &&
            !is_ident_char((unsigned char)p[6])) {
            ret_kw = p + 6;
            break;
        }
        if (*p == '}') break;   /* end of this function, no return found */
        p++;
    }
    if (!ret_kw) return;

    /* isolate the return expression up to end-of-line */
    while (*ret_kw == ' ' || *ret_kw == '\t') ret_kw++;
    char expr[512];
    size_t j = 0;
    int depth = 0;
    while (*ret_kw && j < sizeof(expr) - 1) {
        if (*ret_kw == '(') depth++;
        else if (*ret_kw == ')') depth--;
        else if (*ret_kw == '\n' && depth <= 0) break;
        expr[j++] = *ret_kw++;
    }
    expr[j] = '\0';
    char* et = trim(expr);
    if (*et == '\0') return;

    /* Case 1: `return some_call(args)` — a single function call. Inherit the
     * callee's declared return type if it's a known extern. */
    {
        const char* q = et;
        char callee[128]; size_t k = 0;
        while (*q && is_ident_char((unsigned char)*q) && k < sizeof(callee) - 1) callee[k++] = *q++;
        callee[k] = '\0';
        while (*q == ' ' || *q == '\t') q++;
        if (k > 0 && *q == '(') {
            /* ensure this is the WHOLE expression (no top-level comma after the call) */
            int d = 0; const char* r = q; int comma_outside = 0;
            for (; *r; r++) {
                if (*r == '(') d++;
                else if (*r == ')') d--;
                else if (*r == ',' && d == 0) { comma_outside = 1; break; }
            }
            if (!comma_outside) {
                const char* inherited = extern_ret_lookup(callee);
                if (inherited && inherited[0]) {
                    strncpy(out, inherited, out_size - 1);
                    out[out_size - 1] = '\0';
                    return;
                }
            }
        }
    }

    /* Case 2: comma-separated tuple of values — render an inferred tuple with
     * best-effort element types. */
    {
        int has_comma = 0, d = 0;
        for (const char* r = et; *r; r++) {
            if (*r == '(') d++;
            else if (*r == ')') d--;
            else if (*r == ',' && d == 0) { has_comma = 1; break; }
        }
        if (!has_comma) {
            /* single non-call value — infer a scalar type when the literal
             * makes it obvious (e.g. `return ""` -> string, `return 0` -> int) */
            const char* ty = infer_value_type(et);
            if (*ty) {
                strncpy(out, ty, out_size - 1);
                out[out_size - 1] = '\0';
            }
            return;
        }

        char work[512];
        strncpy(work, et, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';

        char tuple[256] = "(";
        char* save = NULL;
        char* tok = strtok_r(work, ",", &save);
        int first = 1, slot = 0;
        while (tok) {
            const char* ty = infer_value_type(trim(tok));
            char label[32];
            if (*ty) {
                snprintf(label, sizeof(label), "%s", ty);
            } else {
                snprintf(label, sizeof(label), "_%d", slot);   /* unknown slot */
            }
            if (!first) strncat(tuple, ", ", sizeof(tuple) - strlen(tuple) - 1);
            strncat(tuple, label, sizeof(tuple) - strlen(tuple) - 1);
            first = 0; slot++;
            tok = strtok_r(NULL, ",", &save);
        }
        strncat(tuple, ")", sizeof(tuple) - strlen(tuple) - 1);
        strncpy(out, tuple, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Record a found definition against an exported name, if `name` is in the
 * export list and has no definition yet. */
static void record_def(Module* mod, char exports[][128], int export_count,
                       const char* name, DefKind kind,
                       const char* params, const char* return_type,
                       const char* value, const char* doc) {
    for (int i = 0; i < export_count; i++) {
        if (strcmp(exports[i], name) != 0) continue;
        Function* fn = upsert_function(mod, name);
        if (fn && !fn->has_def) {
            fn->kind = kind;
            if (params)      { strncpy(fn->params, params, sizeof(fn->params) - 1); }
            if (return_type) { strncpy(fn->return_type, return_type, sizeof(fn->return_type) - 1); }
            if (value)       { strncpy(fn->value, value, sizeof(fn->value) - 1); }
            if (doc)         { strncpy(fn->doc, doc, sizeof(fn->doc) - 1); }
            fn->has_def = 1;
        }
        return;
    }
}

/* ---------------------------------------------------------------------- */
/* Parse one module.ae                                                     */
/* ---------------------------------------------------------------------- */

static void parse_module_ae(const char* filepath, const char* module_name) {
    char* src = read_file(filepath);
    if (!src) {
        fprintf(stderr, "Warning: cannot read %s\n", filepath);
        return;
    }

    Module* mod = find_or_create_module(module_name);
    if (!mod) { free(src); return; }

    /* ---- exported names ---- */
    static char exports[MAX_EXPORTS][128];
    int export_count = parse_exports(src, exports, MAX_EXPORTS);
    if (export_count == 0) { free(src); return; }

    /* Pre-create the function records in export order so the page lists
     * functions in the same order the module author declared them. */
    for (int i = 0; i < export_count; i++) {
        upsert_function(mod, exports[i]);
    }

    /* Fresh extern-return table for inferred-wrapper resolution. */
    extern_ret_reset();

    /* ---- walk the file line by line collecting docs + definitions ---- */
    char module_doc[1024] = "";
    int seen_exports = 0;
    char pending_doc[MAX_DOC] = "";

    char* cursor = src;
    while (*cursor) {
        /* isolate the current physical line [line, eol) */
        char* line = cursor;
        char* eol = strchr(cursor, '\n');
        size_t line_len = eol ? (size_t)(eol - cursor) : strlen(cursor);

        char buf[MAX_LINE];
        size_t cp = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, line, cp);
        buf[cp] = '\0';

        char* t = trim(buf);

        if (t[0] == '\0') {
            /* blank line: a doc block ends here */
            pending_doc[0] = '\0';
        } else if (t[0] == '/' && t[1] == '/') {
            const char* comment = t + 2;
            while (*comment == ' ') comment++;
            /* skip pure separator lines (----, ===) so the doc stays clean */
            int only_sep = 1;
            for (const char* c = comment; *c; c++) {
                if (*c != '-' && *c != '=' && *c != ' ') { only_sep = 0; break; }
            }
            if (!seen_exports) {
                if (!only_sep && *comment) {
                    size_t cur = strlen(module_doc);
                    size_t add = strlen(comment);
                    if (cur + add + 2 < sizeof(module_doc)) {
                        if (cur > 0) { module_doc[cur++] = ' '; module_doc[cur] = '\0'; }
                        strcat(module_doc, comment);
                    }
                }
            } else if (!only_sep && *comment) {
                append_doc_line(pending_doc, comment);
            }
        } else {
            /* code line */
            if (strncmp(t, "exports", 7) == 0 && !seen_exports) {
                seen_exports = 1;
                pending_doc[0] = '\0';
            }

            if (strncmp(t, "extern", 6) == 0 && isspace((unsigned char)t[6])) {
                const char* defstart = t + 6;
                while (*defstart == ' ' || *defstart == '\t') defstart++;
                /* resolve against the ORIGINAL source so multi-line param
                 * lists are captured: compute defstart's offset in `line`. */
                size_t off = (size_t)(defstart - buf);
                const char* src_defstart = line + off;
                char nm[128], pr[1024], rt[256];
                if (read_callable(src_defstart, nm, pr, rt)) {
                    /* Register EVERY extern's return so thin wrappers can
                     * inherit it, even when the extern itself isn't exported. */
                    extern_ret_add(nm, rt);
                    record_def(mod, exports, export_count, nm, DEF_EXTERN,
                               pr, rt, NULL, pending_doc);
                }
            } else if (strncmp(t, "const", 5) == 0 && isspace((unsigned char)t[5])) {
                const char* q = t + 5;
                while (*q == ' ' || *q == '\t') q++;
                char nm[128]; size_t j = 0;
                while (*q && is_ident_char((unsigned char)*q) && j < sizeof(nm) - 1) nm[j++] = *q++;
                nm[j] = '\0';
                while (*q == ' ' || *q == '\t') q++;
                char val[256] = "";
                if (*q == '=') {
                    q++;
                    while (*q == ' ' || *q == '\t') q++;
                    size_t k = 0;
                    while (*q && *q != '/' && k < sizeof(val) - 1) val[k++] = *q++;
                    val[k] = '\0';
                    char* vt = trim(val);
                    memmove(val, vt, strlen(vt) + 1);
                }
                record_def(mod, exports, export_count, nm, DEF_CONST,
                           NULL, NULL, val, pending_doc);
            } else if (strncmp(t, "struct", 6) == 0 && isspace((unsigned char)t[6])) {
                const char* q = t + 6;
                while (*q == ' ' || *q == '\t') q++;
                char nm[128]; size_t j = 0;
                while (*q && is_ident_char((unsigned char)*q) && j < sizeof(nm) - 1) nm[j++] = *q++;
                nm[j] = '\0';
                record_def(mod, exports, export_count, nm, DEF_STRUCT,
                           NULL, NULL, NULL, pending_doc);
            } else if (seen_exports && is_ident_char((unsigned char)t[0]) &&
                       !isdigit((unsigned char)t[0])) {
                /* Candidate Aether wrapper definition. */
                size_t lead = (size_t)(t - buf);
                const char* src_start = line + lead;
                char nm[128], pr[1024], rt[256];
                if (read_callable(src_start, nm, pr, rt)) {
                    /* Wrappers written `name(...) -> { ... }` declare no
                     * explicit return type; reconstruct it from the body's
                     * first `return` statement (Go-style (value, err) tuples). */
                    if (rt[0] == '\0') {
                        const char* brace = strchr(src_start, '{');
                        if (brace) infer_return_from_body(brace + 1, rt, sizeof(rt));
                    }
                    record_def(mod, exports, export_count, nm, DEF_FUNCTION,
                               pr, rt, NULL, pending_doc);
                }
            }

            pending_doc[0] = '\0';
        }

        if (!eol) break;
        cursor = eol + 1;
    }

    collapse_ws(module_doc);
    if (module_doc[0]) {
        strncpy(mod->description, module_doc, sizeof(mod->description) - 1);
        mod->description[sizeof(mod->description) - 1] = '\0';
    }

    free(src);
}

/* ---------------------------------------------------------------------- */
/* Module descriptions (fallback when the file header has none)            */
/* ---------------------------------------------------------------------- */

static const char* fallback_description(const char* name) {
    if (strcmp(name, "string") == 0) return "String manipulation and formatting";
    if (strcmp(name, "collections") == 0) return "Lists, maps, sets, and data structures";
    if (strcmp(name, "net") == 0) return "HTTP client, server, and networking";
    if (strcmp(name, "json") == 0) return "JSON parsing and serialization";
    if (strcmp(name, "fs") == 0) return "File system operations";
    if (strcmp(name, "io") == 0) return "Input/output and console";
    if (strcmp(name, "math") == 0) return "Mathematical functions";
    if (strcmp(name, "log") == 0) return "Logging and diagnostics";
    if (strcmp(name, "os") == 0) return "Shell and process execution";
    return "";
}

/* A concise (single-sentence) description suitable for cards/headers. The
 * module header doc can be long; clip it to the first sentence. */
static void short_description(const Module* mod, char* out, size_t out_size) {
    const char* desc = (mod->description[0]) ? mod->description : fallback_description(mod->name);
    size_t j = 0;
    for (size_t i = 0; desc[i] && j < out_size - 1; i++) {
        out[j++] = desc[i];
        if (desc[i] == '.' && (desc[i + 1] == ' ' || desc[i + 1] == '\0')) break;
    }
    out[j] = '\0';
    trim(out);
}

/* ---------------------------------------------------------------------- */
/* HTML emitters (templates preserved from the original generator)         */
/* ---------------------------------------------------------------------- */

/* Build the rendered usage call, e.g. "get_string(value)", from the param
 * names only. */
static void build_usage(const Function* fn, char* call_out, size_t call_size) {
    snprintf(call_out, call_size, "%s(", fn->name);
    if (fn->params[0]) {
        char params_copy[1024];
        strncpy(params_copy, fn->params, sizeof(params_copy) - 1);
        params_copy[sizeof(params_copy) - 1] = '\0';
        char* save = NULL;
        char* tok = strtok_r(params_copy, ",", &save);
        int first = 1;
        while (tok) {
            char* colon = strchr(tok, ':');
            if (colon) *colon = '\0';
            char* nm = trim(tok);
            if (*nm) {
                if (!first) strncat(call_out, ", ", call_size - strlen(call_out) - 1);
                strncat(call_out, nm, call_size - strlen(call_out) - 1);
                first = 0;
            }
            tok = strtok_r(NULL, ",", &save);
        }
    }
    strncat(call_out, ")", call_size - strlen(call_out) - 1);
}

static const char* kind_label(DefKind k) {
    switch (k) {
        case DEF_EXTERN:   return "extern";
        case DEF_CONST:    return "const";
        case DEF_STRUCT:   return "struct";
        case DEF_FUNCTION: default: return "fn";
    }
}

static void emit_head(FILE* f, const char* title) {
    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "  <meta charset=\"UTF-8\">\n");
    fprintf(f, "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "  <title>%s</title>\n", title);
    fprintf(f, "  <link rel=\"stylesheet\" href=\"style.css\">\n");
    fprintf(f, "</head>\n<body>\n");
}

static void generate_index(const char* output_dir) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/index.html", output_dir);
    FILE* f = fopen(filepath, "wb");  /* LF on every platform: the pages are committed */
    if (!f) { fprintf(stderr, "Error: cannot create %s\n", filepath); return; }

    emit_head(f, "Aether Standard Library");

    fprintf(f, "<nav class=\"sidebar\">\n");
    fprintf(f, "  <div class=\"sidebar-header\">\n");
    fprintf(f, "    <h1>Aether</h1>\n");
    fprintf(f, "    <p class=\"tagline\">Standard Library</p>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <input type=\"text\" id=\"search\" placeholder=\"Search functions...\" onkeyup=\"search()\">\n");
    fprintf(f, "  <h2>Modules</h2>\n");
    fprintf(f, "  <ul class=\"module-list\">\n");
    for (int i = 0; i < module_count; i++) {
        fprintf(f, "    <li><a href=\"%s.html\">%s</a></li>\n", modules[i]->name, modules[i]->name);
    }
    fprintf(f, "  </ul>\n</nav>\n");

    fprintf(f, "<main class=\"content\">\n");
    fprintf(f, "  <div class=\"hero\">\n");
    fprintf(f, "    <h1>Standard Library</h1>\n");
    fprintf(f, "    <p>The public <code>.ae</code> API: import a module with "
               "<code>import std.&lt;name&gt;</code> and call its exports as "
               "<code>&lt;name&gt;.&lt;function&gt;</code>.</p>\n");
    fprintf(f, "  </div>\n");

    fprintf(f, "  <div class=\"module-grid\">\n");
    for (int i = 0; i < module_count; i++) {
        char desc[600], esc[1200];
        short_description(modules[i], desc, sizeof(desc));
        html_escape(desc, esc, sizeof(esc));
        fprintf(f, "    <a href=\"%s.html\" class=\"module-card\">\n", modules[i]->name);
        fprintf(f, "      <h3>%s</h3>\n", modules[i]->name);
        fprintf(f, "      <p>%s</p>\n", esc);
        fprintf(f, "      <span class=\"count\">%d exports</span>\n", modules[i]->function_count);
        fprintf(f, "    </a>\n");
    }
    fprintf(f, "  </div>\n");

    /* Hidden search index across every exported symbol. */
    fprintf(f, "  <div id=\"search-results\" class=\"search-results hidden\">\n");
    fprintf(f, "    <h2>Search Results</h2>\n");
    fprintf(f, "    <ul class=\"function-list\" id=\"all-functions\">\n");
    for (int i = 0; i < module_count; i++) {
        for (int j = 0; j < modules[i]->function_count; j++) {
            Function* fn = &modules[i]->functions[j];
            char esc[256];
            html_escape(fn->name, esc, sizeof(esc));
            fprintf(f, "      <li class=\"function-item\" data-name=\"%s\">"
                       "<a href=\"%s.html#%s\"><span class=\"fn-name\">%s</span>"
                       "<span class=\"fn-module\">%s</span></a></li>\n",
                    esc, modules[i]->name, esc, esc, modules[i]->name);
        }
    }
    fprintf(f, "    </ul>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "</main>\n");

    fprintf(f, "<script src=\"search.js\"></script>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("Generated: %s\n", filepath);
}

static void generate_module_page(const char* output_dir, Module* mod) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/%s.html", output_dir, mod->name);
    FILE* f = fopen(filepath, "wb");  /* LF on every platform: the pages are committed */
    if (!f) { fprintf(stderr, "Error: cannot create %s\n", filepath); return; }

    char title[128];
    snprintf(title, sizeof(title), "%s - Aether", mod->name);
    emit_head(f, title);

    char short_desc[600], short_esc[1200];
    short_description(mod, short_desc, sizeof(short_desc));
    html_escape(short_desc, short_esc, sizeof(short_esc));

    /* Sidebar */
    fprintf(f, "<nav class=\"sidebar\">\n");
    fprintf(f, "  <div class=\"sidebar-header\">\n");
    fprintf(f, "    <h1><a href=\"index.html\">Aether</a></h1>\n");
    fprintf(f, "    <p class=\"tagline\">Standard Library</p>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <input type=\"text\" id=\"search\" placeholder=\"Search %s...\" onkeyup=\"searchModule()\">\n", mod->name);
    fprintf(f, "  <h2>Exports</h2>\n");
    fprintf(f, "  <ul class=\"function-nav\">\n");
    for (int i = 0; i < mod->function_count; i++) {
        char esc[256];
        html_escape(mod->functions[i].name, esc, sizeof(esc));
        fprintf(f, "    <li><a href=\"#%s\">%s</a></li>\n", esc, esc);
    }
    fprintf(f, "  </ul>\n");
    fprintf(f, "  <hr>\n");
    fprintf(f, "  <h2>Modules</h2>\n");
    fprintf(f, "  <ul class=\"module-list\">\n");
    for (int i = 0; i < module_count; i++) {
        const char* active = (strcmp(modules[i]->name, mod->name) == 0) ? " class=\"active\"" : "";
        fprintf(f, "    <li%s><a href=\"%s.html\">%s</a></li>\n", active, modules[i]->name, modules[i]->name);
    }
    fprintf(f, "  </ul>\n</nav>\n");

    /* Main content */
    fprintf(f, "<main class=\"content\">\n");
    fprintf(f, "  <div class=\"module-header\">\n");
    fprintf(f, "    <h1>%s</h1>\n", mod->name);
    fprintf(f, "    <p class=\"module-desc\">%s</p>\n", short_esc);
    fprintf(f, "    <p class=\"import-line\"><code>import std.%s</code></p>\n", mod->name);
    fprintf(f, "  </div>\n");

    fprintf(f, "  <div class=\"functions\" id=\"functions\">\n");
    for (int i = 0; i < mod->function_count; i++) {
        Function* fn = &mod->functions[i];

        char esc_name[256], esc_doc[MAX_DOC * 2];
        html_escape(fn->name, esc_name, sizeof(esc_name));
        html_escape(fn->doc, esc_doc, sizeof(esc_doc));

        fprintf(f, "    <div class=\"function\" id=\"%s\" data-name=\"%s\">\n", esc_name, esc_name);
        fprintf(f, "      <h3 class=\"function-name\">%s <span class=\"kind\">%s</span></h3>\n",
                esc_name, kind_label(fn->kind));

        if (fn->kind == DEF_CONST) {
            char esc_val[512];
            html_escape(fn->value[0] ? fn->value : "", esc_val, sizeof(esc_val));
            fprintf(f, "      <pre class=\"usage\"><code>%s%s%s</code></pre>\n",
                    esc_name,
                    fn->value[0] ? " = " : "",
                    esc_val);
        } else if (fn->kind == DEF_STRUCT) {
            fprintf(f, "      <pre class=\"usage\"><code>struct %s</code></pre>\n", esc_name);
        } else {
            char call[1024], esc_call[2048];
            build_usage(fn, call, sizeof(call));
            html_escape(call, esc_call, sizeof(esc_call));

            if (fn->return_type[0]) {
                char esc_ret[512];
                html_escape(fn->return_type, esc_ret, sizeof(esc_ret));
                fprintf(f, "      <pre class=\"usage\"><code>%s <span class=\"arrow\">-&gt;</span> %s</code></pre>\n",
                        esc_call, esc_ret);
            } else {
                fprintf(f, "      <pre class=\"usage\"><code>%s</code></pre>\n", esc_call);
            }

            /* Full typed parameter line. */
            if (fn->params[0]) {
                char esc_params[2048];
                html_escape(fn->params, esc_params, sizeof(esc_params));
                fprintf(f, "      <p class=\"signature\"><span class=\"sig-params\">%s</span></p>\n", esc_params);
            }
        }

        if (fn->doc[0]) {
            fprintf(f, "      <p class=\"doc\">%s</p>\n", esc_doc);
        }
        fprintf(f, "    </div>\n");
    }
    fprintf(f, "  </div>\n");
    fprintf(f, "</main>\n");

    fprintf(f, "<script src=\"search.js\"></script>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("Generated: %s\n", filepath);
}

static void generate_css(const char* output_dir) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/style.css", output_dir);
    FILE* f = fopen(filepath, "wb");  /* LF on every platform: the pages are committed */
    if (!f) { fprintf(stderr, "Error: cannot create %s\n", filepath); return; }

    fprintf(f,
"* { margin: 0; padding: 0; box-sizing: border-box; }\n\n"

"body {\n"
"  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Inter', Roboto, sans-serif;\n"
"  background: #0a0a0a;\n"
"  color: #d4d4d4;\n"
"  line-height: 1.6;\n"
"  display: flex;\n"
"  min-height: 100vh;\n"
"}\n\n"

"a { color: inherit; text-decoration: none; }\n"
"code { font-family: 'SF Mono', 'Fira Code', monospace; }\n\n"

".sidebar {\n"
"  width: 240px;\n"
"  background: #111;\n"
"  padding: 20px 16px;\n"
"  position: fixed;\n"
"  height: 100vh;\n"
"  overflow-y: auto;\n"
"  border-right: 1px solid #222;\n"
"}\n\n"

".sidebar-header h1 { font-size: 1.1rem; font-weight: 600; color: #fff; }\n"
".sidebar-header .tagline { font-size: 0.75rem; color: #666; margin-top: 2px; }\n\n"

"#search {\n"
"  width: 100%%;\n"
"  padding: 8px 12px;\n"
"  border: 1px solid #333;\n"
"  border-radius: 6px;\n"
"  background: #0a0a0a;\n"
"  color: #d4d4d4;\n"
"  font-size: 0.85rem;\n"
"  margin: 16px 0;\n"
"}\n\n"

"#search:focus { outline: none; border-color: #4a9eff; }\n\n"

".sidebar h2 {\n"
"  font-size: 0.7rem;\n"
"  font-weight: 500;\n"
"  color: #555;\n"
"  text-transform: uppercase;\n"
"  letter-spacing: 0.08em;\n"
"  margin: 16px 0 8px;\n"
"}\n\n"

".module-list, .function-nav { list-style: none; }\n\n"

".module-list li a, .function-nav li a {\n"
"  display: block;\n"
"  padding: 6px 10px;\n"
"  border-radius: 4px;\n"
"  font-size: 0.85rem;\n"
"  color: #888;\n"
"  transition: all 0.1s;\n"
"}\n\n"

".function-nav li a {\n"
"  font-family: 'SF Mono', 'Fira Code', monospace;\n"
"  font-size: 0.8rem;\n"
"  padding: 4px 10px;\n"
"  overflow: hidden;\n"
"  text-overflow: ellipsis;\n"
"  white-space: nowrap;\n"
"}\n\n"

".module-list li a:hover, .function-nav li a:hover { background: #1a1a1a; color: #fff; }\n"
".module-list .active a { background: #1a2a3a; color: #4a9eff; }\n\n"

"hr { border: none; border-top: 1px solid #222; margin: 16px 0; }\n\n"

".content {\n"
"  margin-left: 240px;\n"
"  padding: 32px 40px;\n"
"  max-width: 900px;\n"
"  width: 100%%;\n"
"}\n\n"

".hero { margin-bottom: 40px; }\n"
".hero h1 { font-size: 1.5rem; font-weight: 600; color: #fff; margin-bottom: 8px; }\n"
".hero p { color: #888; font-size: 0.95rem; }\n"
".hero code, .module-desc code, .import-line code { color: #7dd3fc; background: #141414; padding: 1px 5px; border-radius: 4px; font-size: 0.85em; }\n\n"

".module-header { margin-bottom: 32px; }\n"
".module-header h1 { font-size: 1.4rem; font-weight: 600; color: #fff; margin-bottom: 4px; }\n"
".module-desc { color: #888; font-size: 0.9rem; }\n"
".import-line { margin-top: 8px; }\n\n"

".module-grid {\n"
"  display: grid;\n"
"  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));\n"
"  gap: 12px;\n"
"}\n\n"

".module-card {\n"
"  display: block;\n"
"  background: #141414;\n"
"  padding: 16px 18px;\n"
"  border-radius: 8px;\n"
"  border: 1px solid #222;\n"
"  transition: all 0.15s;\n"
"}\n\n"

".module-card:hover { border-color: #333; background: #181818; }\n"
".module-card h3 { font-size: 0.95rem; color: #fff; margin-bottom: 4px; }\n"
".module-card p { font-size: 0.8rem; color: #888; margin-bottom: 8px; }\n"
".module-card .count { font-size: 0.7rem; color: #555; }\n\n"

".search-results { margin-top: 32px; }\n"
".search-results h2 { font-size: 0.85rem; color: #888; margin-bottom: 16px; }\n\n"

".function-list { list-style: none; }\n"
".function-item a {\n"
"  display: flex;\n"
"  justify-content: space-between;\n"
"  padding: 8px 12px;\n"
"  border-radius: 6px;\n"
"  margin-bottom: 2px;\n"
"  transition: background 0.1s;\n"
"}\n"
".function-item a:hover { background: #181818; }\n"
".fn-name { font-family: 'SF Mono', monospace; font-size: 0.85rem; color: #d4d4d4; }\n"
".fn-module { font-size: 0.75rem; color: #555; }\n\n"

".function {\n"
"  background: #141414;\n"
"  padding: 20px;\n"
"  border-radius: 8px;\n"
"  margin-bottom: 12px;\n"
"  border: 1px solid #222;\n"
"}\n\n"

".function-name {\n"
"  font-size: 1rem;\n"
"  font-weight: 500;\n"
"  color: #fff;\n"
"  margin-bottom: 8px;\n"
"  font-family: 'SF Mono', 'Fira Code', monospace;\n"
"}\n\n"

".function-name .kind {\n"
"  font-family: -apple-system, sans-serif;\n"
"  font-size: 0.65rem;\n"
"  font-weight: 500;\n"
"  text-transform: uppercase;\n"
"  letter-spacing: 0.05em;\n"
"  color: #4a9eff;\n"
"  background: #14202e;\n"
"  padding: 2px 6px;\n"
"  border-radius: 4px;\n"
"  margin-left: 8px;\n"
"  vertical-align: middle;\n"
"}\n\n"

".usage {\n"
"  background: #0a0a0a;\n"
"  padding: 10px 14px;\n"
"  border-radius: 6px;\n"
"  margin-bottom: 8px;\n"
"  overflow-x: auto;\n"
"}\n\n"

".usage code { font-size: 0.85rem; color: #7dd3fc; }\n"
".usage .arrow { color: #666; }\n\n"

".signature {\n"
"  font-family: 'SF Mono', 'Fira Code', monospace;\n"
"  font-size: 0.78rem;\n"
"  color: #888;\n"
"  margin-bottom: 8px;\n"
"}\n"
".signature .sig-params { color: #c0a0e0; }\n\n"

".doc { color: #999; font-size: 0.9rem; }\n\n"

".hidden { display: none !important; }\n\n"

"@media (max-width: 768px) {\n"
"  .sidebar { display: none; }\n"
"  .content { margin-left: 0; padding: 20px; }\n"
"}\n"
    );

    fclose(f);
    printf("Generated: %s\n", filepath);
}

static void generate_search_js(const char* output_dir) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/search.js", output_dir);
    FILE* f = fopen(filepath, "wb");  /* LF on every platform: the pages are committed */
    if (!f) { fprintf(stderr, "Error: cannot create %s\n", filepath); return; }

    fprintf(f,
"// Index page search - shows results, hides module grid\n"
"function search() {\n"
"  const query = document.getElementById('search').value.toLowerCase().trim();\n"
"  const results = document.getElementById('search-results');\n"
"  const grid = document.querySelector('.module-grid');\n"
"  const items = document.querySelectorAll('.function-item');\n"
"  \n"
"  if (query.length === 0) {\n"
"    results.classList.add('hidden');\n"
"    grid.classList.remove('hidden');\n"
"    return;\n"
"  }\n"
"  \n"
"  results.classList.remove('hidden');\n"
"  grid.classList.add('hidden');\n"
"  \n"
"  items.forEach(item => {\n"
"    const name = item.dataset.name.toLowerCase();\n"
"    item.classList.toggle('hidden', !name.includes(query));\n"
"  });\n"
"}\n\n"

"// Module page search - filters functions\n"
"function searchModule() {\n"
"  const query = document.getElementById('search').value.toLowerCase().trim();\n"
"  const functions = document.querySelectorAll('.function');\n"
"  const navItems = document.querySelectorAll('.function-nav li');\n"
"  \n"
"  functions.forEach(func => {\n"
"    const name = func.dataset.name.toLowerCase();\n"
"    func.classList.toggle('hidden', query.length > 0 && !name.includes(query));\n"
"  });\n"
"  \n"
"  navItems.forEach(item => {\n"
"    const link = item.querySelector('a');\n"
"    if (link) {\n"
"      const name = link.textContent.toLowerCase();\n"
"      item.classList.toggle('hidden', query.length > 0 && !name.includes(query));\n"
"    }\n"
"  });\n"
"}\n\n"

"// Highlight active function on scroll\n"
"let ticking = false;\n"
"document.addEventListener('scroll', () => {\n"
"  if (!ticking) {\n"
"    requestAnimationFrame(() => {\n"
"      const functions = document.querySelectorAll('.function');\n"
"      const navLinks = document.querySelectorAll('.function-nav a');\n"
"      let current = '';\n"
"      functions.forEach(func => {\n"
"        const rect = func.getBoundingClientRect();\n"
"        if (rect.top <= 80) current = func.id;\n"
"      });\n"
"      navLinks.forEach(link => {\n"
"        link.parentElement.classList.remove('active');\n"
"        if (link.getAttribute('href') === '#' + current) {\n"
"          link.parentElement.classList.add('active');\n"
"        }\n"
"      });\n"
"      ticking = false;\n"
"    });\n"
"    ticking = true;\n"
"  }\n"
"});\n"
    );

    fclose(f);
    printf("Generated: %s\n", filepath);
}

/* ---------------------------------------------------------------------- */
/* Driver                                                                  */
/* ---------------------------------------------------------------------- */

/* qsort comparator: modules alphabetically by name. */
static int module_cmp(const void* a, const void* b) {
    return strcmp((*(Module* const*)a)->name, (*(Module* const*)b)->name);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <std_dir> <output_dir>\n", argv[0]);
        fprintf(stderr, "Example: %s std docs/api\n", argv[0]);
        return 1;
    }

    const char* std_dir = argv[1];
    const char* output_dir = argv[2];

    docgen_mkdir(output_dir);

    DIR* dir = opendir(std_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open %s\n", std_dir);
        return 1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char subdir_path[600];
        snprintf(subdir_path, sizeof(subdir_path), "%s/%s", std_dir, entry->d_name);

        struct stat st;
        if (stat(subdir_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char module_ae[700];
        snprintf(module_ae, sizeof(module_ae), "%s/module.ae", subdir_path);
        if (stat(module_ae, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        printf("Scanning module: %s\n", entry->d_name);
        parse_module_ae(module_ae, entry->d_name);
    }
    closedir(dir);

    /* Sort modules alphabetically for a stable, predictable layout. */
    qsort(modules, module_count, sizeof(*modules), module_cmp);

    /* Drop modules whose exports yielded nothing documentable (defensive). */
    int kept = 0;
    for (int i = 0; i < module_count; i++) {
        if (modules[i]->function_count > 0) {
            if (kept != i) modules[kept] = modules[i];
            kept++;
        }
    }
    module_count = kept;

    int total = 0;
    printf("\nParsed %d modules:\n", module_count);
    for (int i = 0; i < module_count; i++) {
        printf("  - %s: %d exports\n", modules[i]->name, modules[i]->function_count);
        total += modules[i]->function_count;
    }
    printf("  total exports: %d\n", total);

    printf("\nGenerating documentation...\n");
    generate_css(output_dir);
    generate_search_js(output_dir);
    generate_index(output_dir);
    for (int i = 0; i < module_count; i++) {
        generate_module_page(output_dir, modules[i]);
    }

    printf("\nDone! Open %s/index.html in a browser.\n", output_dir);
    return 0;
}
