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
 * Every string it reads or renders is grown to fit, never cut to a fixed
 * buffer: the generator used to truncate names, signatures, doc blocks and
 * module headers at fixed sizes without a word, so a page could be wrong
 * and still pass the docs/api check that compares it with this output.
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

typedef enum {
    DEF_FUNCTION,   /* Aether wrapper: name(params) -> Ret { ... }            */
    DEF_EXTERN,     /* extern name(params) -> Ret                             */
    DEF_CONST,      /* const NAME = value                                     */
    DEF_STRUCT,     /* struct Name { ... }                                    */
    DEF_MESSAGE,    /* message Name { field: T ... }                          */
    DEF_PROTOCOL    /* protocol Name { from -> Message -> to ... } (#385)     */
} DefKind;

typedef struct {
    char* name;
    char* params;             /* rendered "p1: T1, p2: T2"                    */
    char* return_type;        /* rendered return, "" when none / void        */
    char* doc;                /* doc-comment block                           */
    char* value;              /* const value (DEF_CONST only)                */
    DefKind kind;
    int has_def;              /* 1 once a definition was found               */
} Function;

typedef struct {
    char* name;
    char* description;
    Function* functions;
    int function_count;
    int function_cap;
} Module;

static Module** modules = NULL;
static int module_count = 0;
static int module_cap = 0;

/* ---------------------------------------------------------------------- */
/* Memory and strings                                                      */
/* ---------------------------------------------------------------------- */

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "docgen: out of memory\n");
        exit(1);
    }
    return q;
}

static char* xstrndup(const char* s, size_t n) {
    char* d = xrealloc(NULL, n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char* xstrdup(const char* s) { return xstrndup(s, strlen(s)); }

/* A string that grows as it is appended to. */
typedef struct {
    char* p;
    size_t len, cap;
} Str;

static void str_addn(Str* s, const char* t, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 64;
        while (cap < s->len + n + 1) cap *= 2;
        s->p = xrealloc(s->p, cap);
        s->cap = cap;
    }
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = '\0';
}

static void str_add(Str* s, const char* t) { str_addn(s, t, strlen(t)); }

static const char* str_get(const Str* s) { return s->p ? s->p : ""; }

static void str_clear(Str* s) {
    s->len = 0;
    if (s->p) s->p[0] = '\0';
}

/* The text, owned by the caller; `s` is empty again. */
static char* str_take(Str* s) {
    char* r = s->p ? s->p : xstrdup("");
    s->p = NULL;
    s->len = s->cap = 0;
    return r;
}

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

/* Collapse runs of whitespace to single spaces and trim, in place. */
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
    char* t = trim(s);
    if (t != s) memmove(s, t, strlen(t) + 1);
}

/* Strip Aether memory annotations (@heap, @retain, @borrow, ...) from a
 * type/param fragment, in place: they are internal lifetime hints, not part
 * of the user-facing type. The result is never longer than the input. */
static void strip_annotations(char* s) {
    char* w = s;
    for (char* r = s; *r; ) {
        if (*r == '@') {
            r++;
            while (*r && is_ident_char((unsigned char)*r)) r++;  /* skip word */
            /* skip a single following space so we don't leave doubled spaces */
            if (*r == ' ') r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
    collapse_ws(s);
    /* Annotation removal can leave a space before a separator
     * (`string @heap,` -> `string ,`); drop it. */
    w = s;
    for (char* r = s; *r; r++) {
        if (*r == ' ' && (r[1] == ',' || r[1] == ')')) continue;
        *w++ = *r;
    }
    *w = '\0';
}

/* ---------------------------------------------------------------------- */
/* HTML escaping                                                           */
/* ---------------------------------------------------------------------- */

/* Write `s` to `f` with the four characters HTML gives a meaning escaped. */
static void html_out(FILE* f, const char* s) {
    for (; *s; s++) {
        switch (*s) {
            case '<': fputs("&lt;", f); break;
            case '>': fputs("&gt;", f); break;
            case '&': fputs("&amp;", f); break;
            case '"': fputs("&quot;", f); break;
            default:  fputc(*s, f); break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Module loading                                                          */
/* ---------------------------------------------------------------------- */

static Module* find_or_create_module(const char* name) {
    for (int i = 0; i < module_count; i++) {
        if (strcmp(modules[i]->name, name) == 0) return modules[i];
    }
    if (module_count >= module_cap) {
        module_cap = module_cap ? module_cap * 2 : 64;
        modules = xrealloc(modules, (size_t)module_cap * sizeof(*modules));
    }
    Module* m = xrealloc(NULL, sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->name = xstrdup(name);
    m->description = xstrdup("");
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
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* buf = xrealloc(NULL, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* ---------------------------------------------------------------------- */
/* exports (...) list parsing                                              */
/* ---------------------------------------------------------------------- */

/* Where the `exports (` that opens the export list is: on a code line, at
 * its start. A comment may say "exports" too (std.http's header says it
 * "re-exports HTTP functions ... (this module)"), and the first "exports"
 * anywhere in the file once made that comment's words the export list.
 * Returns the position just after the `(`, or NULL. */
static const char* find_exports_open(const char* src) {
    for (const char* line = src; *line; ) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "exports", 7) == 0 && !is_ident_char((unsigned char)p[7])) {
            p += 7;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '(') return p + 1;
        }
        const char* eol = strchr(line, '\n');
        if (!eol) break;
        line = eol + 1;
    }
    return NULL;
}

static void free_exports(char** exports, int count) {
    for (int i = 0; i < count; i++) free(exports[i]);
    free(exports);
}

/* Collect comma/whitespace-separated identifiers from the `exports ( ... )`
 * block into `*out` (each owned). Returns the export count, or -1 when the
 * block never closes, with nothing handed back: the names read so far are
 * freed here, since the caller cannot know how many there were. Inline `//`
 * comments inside the block are skipped. */
static int parse_exports(const char* src, char*** out) {
    *out = NULL;
    const char* p = find_exports_open(src);
    if (!p) return 0;

    int count = 0, cap = 0;
    while (*p && *p != ')') {
        /* skip line comments inside the export block */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (!is_ident_char((unsigned char)*p)) { p++; continue; }
        const char* start = p;
        while (*p && is_ident_char((unsigned char)*p)) p++;
        if (count >= cap) {
            cap = cap ? cap * 2 : 64;
            *out = xrealloc(*out, (size_t)cap * sizeof(**out));
        }
        (*out)[count++] = xstrndup(start, (size_t)(p - start));
    }
    if (*p != ')') {
        free_exports(*out, count);
        *out = NULL;
        return -1;
    }
    return count;
}

/* ---------------------------------------------------------------------- */
/* Definition extraction                                                   */
/* ---------------------------------------------------------------------- */

/* Render a parameter list source fragment (text between the outer parens)
 * into a clean "p1: T1, p2: T2" string with annotations stripped. */
static char* render_params(const char* raw) {
    char* work = xstrdup(raw);
    collapse_ws(work);
    if (*work == '\0' || strcmp(work, "void") == 0) { free(work); return xstrdup(""); }

    /* Split on commas and join with ", ": a function-typed parameter's own
     * commas come back as they were. */
    Str out = {0};
    char* save = NULL;
    char* tok = strtok_r(work, ",", &save);
    int first = 1;
    while (tok) {
        strip_annotations(tok);
        char* pt = trim(tok);
        if (*pt) {
            if (!first) str_add(&out, ", ");
            str_add(&out, pt);
            first = 0;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(work);
    return str_take(&out);
}

/* Given the source position just AFTER the closing paren of a signature,
 * capture the return type. Handles:
 *   -> Type
 *   -> (T1, T2, ...)
 *   -> *Struct
 *   (none)                 — extern / void function
 * The capture stops at `{`, `\n`, or end of source.  Annotations stripped. */
static char* capture_return(const char* after_paren) {
    const char* p = after_paren;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '-' || p[1] != '>') return xstrdup("");   /* no return arrow */
    p += 2;
    while (*p == ' ' || *p == '\t') p++;

    const char* start = p;
    if (*p == '(') {
        /* tuple return — up to and including the matching ')' */
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')' && --depth == 0) { p++; break; }
            p++;
        }
    } else {
        /* scalar return — until '{', newline, or end */
        while (*p && *p != '{' && *p != '\n') p++;
    }
    char* buf = xstrndup(start, (size_t)(p - start));
    strip_annotations(buf);
    /* `void` reads as no return for documentation purposes */
    if (strcmp(buf, "void") == 0) buf[0] = '\0';
    return buf;
}

/* Try to read a `name(params) ...` definition starting at `start` (which
 * points at the first char of the name). On success sets the three owned
 * strings and returns 1; returns 0 if this is not a callable definition. */
static int read_callable(const char* start, char** name_out,
                         char** params_out, char** return_out) {
    const char* p = start;
    while (*p && is_ident_char((unsigned char)*p)) p++;
    if (p == start) return 0;
    const char* name_end = p;

    /* whitespace then '(' */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return 0;
    p++;

    /* the param text up to the matching ')' */
    const char* params_start = p;
    int depth = 1;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) break;
        p++;
    }
    if (depth != 0) return 0;  /* unterminated */

    char* raw = xstrndup(params_start, (size_t)(p - params_start));
    *name_out = xstrndup(start, (size_t)(name_end - start));
    *params_out = render_params(raw);
    *return_out = capture_return(p + 1);
    free(raw);
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
    if (mod->function_count >= mod->function_cap) {
        mod->function_cap = mod->function_cap ? mod->function_cap * 2 : 32;
        mod->functions = xrealloc(mod->functions, (size_t)mod->function_cap * sizeof(Function));
    }
    Function* fn = &mod->functions[mod->function_count++];
    memset(fn, 0, sizeof(*fn));
    fn->name = xstrdup(name);
    fn->params = xstrdup("");
    fn->return_type = xstrdup("");
    fn->doc = xstrdup("");
    fn->value = xstrdup("");
    return fn;
}

/* Append a doc-comment line (text after `//`) to a pending doc block. */
static void append_doc_line(Str* doc, const char* text) {
    if (doc->len > 0) str_add(doc, " ");
    str_add(doc, text);
}

/* ---------------------------------------------------------------------- */
/* Inferred-return resolution                                              */
/* ---------------------------------------------------------------------- */

/* A module-local table mapping EVERY extern (exported or not) to its
 * declared return type, so thin wrappers written as `name(...) -> { return
 * some_extern(...) }` can inherit the real return shape the extern documents
 * (e.g. fs.copy -> fs_copy_raw -> "(int, int, string)"). */
typedef struct { char* name; char* ret; } ExternRet;
static ExternRet* extern_rets = NULL;
static int extern_ret_count = 0;
static int extern_ret_cap = 0;

static void extern_ret_reset(void) {
    for (int i = 0; i < extern_ret_count; i++) {
        free(extern_rets[i].name);
        free(extern_rets[i].ret);
    }
    extern_ret_count = 0;
}

static void extern_ret_add(const char* name, const char* ret) {
    if (extern_ret_count >= extern_ret_cap) {
        extern_ret_cap = extern_ret_cap ? extern_ret_cap * 2 : 64;
        extern_rets = xrealloc(extern_rets, (size_t)extern_ret_cap * sizeof(*extern_rets));
    }
    extern_rets[extern_ret_count].name = xstrdup(name);
    extern_rets[extern_ret_count].ret = xstrdup(ret);
    extern_ret_count++;
}

static const char* extern_ret_lookup(const char* name) {
    for (int i = 0; i < extern_ret_count; i++) {
        if (strcmp(extern_rets[i].name, name) == 0) return extern_rets[i].ret;
    }
    return NULL;
}

/* Best-effort type of a single return-expression token (trimmed). */
static const char* infer_value_type(const char* t) {
    if (*t == '\0') return "";
    if (strcmp(t, "null") == 0) return "ptr";
    if (t[0] == '"') return "string";                       /* string literal */
    if (t[0] == '-' || isdigit((unsigned char)t[0])) {      /* numeric literal */
        return strchr(t, '.') ? "float" : "int";
    }
    return "";   /* unknown — left as an unnamed tuple slot */
}

/* Resolve an inferred (`-> {`) wrapper return by inspecting its body.
 * `body` points just after the opening brace of the function. Returns the
 * rendered return type, owned ("" if nothing could be inferred). */
static char* infer_return_from_body(const char* body) {
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
    if (!ret_kw) return xstrdup("");

    /* isolate the return expression up to end-of-line */
    while (*ret_kw == ' ' || *ret_kw == '\t') ret_kw++;
    const char* end = ret_kw;
    int depth = 0;
    while (*end) {
        if (*end == '(') depth++;
        else if (*end == ')') depth--;
        else if (*end == '\n' && depth <= 0) break;
        end++;
    }
    char* expr = xstrndup(ret_kw, (size_t)(end - ret_kw));
    char* et = trim(expr);
    if (*et == '\0') { free(expr); return xstrdup(""); }

    /* Case 1: `return some_call(args)` — a single function call. Inherit the
     * callee's declared return type if it's a known extern. */
    {
        const char* q = et;
        while (*q && is_ident_char((unsigned char)*q)) q++;
        size_t k = (size_t)(q - et);
        while (*q == ' ' || *q == '\t') q++;
        if (k > 0 && *q == '(') {
            /* ensure this is the WHOLE expression (no top-level comma after the call) */
            int d = 0; int comma_outside = 0;
            for (const char* r = q; *r; r++) {
                if (*r == '(') d++;
                else if (*r == ')') d--;
                else if (*r == ',' && d == 0) { comma_outside = 1; break; }
            }
            if (!comma_outside) {
                char* callee = xstrndup(et, k);
                const char* inherited = extern_ret_lookup(callee);
                free(callee);
                if (inherited && inherited[0]) { free(expr); return xstrdup(inherited); }
            }
        }
    }

    /* Case 2: comma-separated tuple of values — render an inferred tuple with
     * best-effort element types. */
    int has_comma = 0, d = 0;
    for (const char* r = et; *r; r++) {
        if (*r == '(') d++;
        else if (*r == ')') d--;
        else if (*r == ',' && d == 0) { has_comma = 1; break; }
    }
    if (!has_comma) {
        /* single non-call value — infer a scalar type when the literal
         * makes it obvious (e.g. `return ""` -> string, `return 0` -> int) */
        char* r = xstrdup(infer_value_type(et));
        free(expr);
        return r;
    }

    Str tuple = {0};
    str_add(&tuple, "(");
    char* save = NULL;
    char* tok = strtok_r(et, ",", &save);
    int first = 1, slot = 0;
    while (tok) {
        const char* ty = infer_value_type(trim(tok));
        char label[32];
        if (*ty) snprintf(label, sizeof(label), "%s", ty);
        else snprintf(label, sizeof(label), "_%d", slot);   /* unknown slot */
        if (!first) str_add(&tuple, ", ");
        str_add(&tuple, label);
        first = 0; slot++;
        tok = strtok_r(NULL, ",", &save);
    }
    str_add(&tuple, ")");
    free(expr);
    return str_take(&tuple);
}

/* Record a found definition against an exported name, if `name` is in the
 * export list and has no definition yet. */
static void record_def(Module* mod, char** exports, int export_count,
                       const char* name, DefKind kind,
                       const char* params, const char* return_type,
                       const char* value, const char* doc) {
    for (int i = 0; i < export_count; i++) {
        if (strcmp(exports[i], name) != 0) continue;
        Function* fn = upsert_function(mod, name);
        if (!fn->has_def) {
            fn->kind = kind;
            if (params)      { free(fn->params); fn->params = xstrdup(params); }
            if (return_type) { free(fn->return_type); fn->return_type = xstrdup(return_type); }
            if (value)       { free(fn->value); fn->value = xstrdup(value); }
            if (doc)         { free(fn->doc); fn->doc = xstrdup(doc); }
            fn->has_def = 1;
        }
        return;
    }
}

/* The lines of a `message` or `protocol` declaration that starts at `decl`,
 * between its braces, each trimmed and on a line of its own indented by four
 * spaces: "\n    a -> M -> b\n" for one transition. A one-line declaration
 * (`message Close {}`) gives "". */
static char* decl_body(const char* decl) {
    const char* open = strchr(decl, '{');
    if (!open) return xstrdup("");
    const char* close = strchr(open + 1, '}');
    if (!close) close = open + 1 + strlen(open + 1);
    Str out = {0};
    const char* p = open + 1;
    while (p < close) {
        const char* eol = memchr(p, '\n', (size_t)(close - p));
        const char* end = eol ? eol : close;
        char* piece = xstrndup(p, (size_t)(end - p));
        char* tr = trim(piece);
        if (tr[0]) {
            str_add(&out, "\n    ");
            str_add(&out, tr);
        }
        free(piece);
        p = eol ? eol + 1 : close;
    }
    if (out.len) str_add(&out, "\n");
    return out.len ? str_take(&out) : xstrdup("");
}

/* The value of `const NAME = value // note`: up to a `//` that is not inside
 * a string literal, trimmed. A plain `/` stays: `const SEP = "/"` was cut
 * to `"` when the value stopped at the first slash. */
static char* const_value(const char* q) {
    const char* end = q;
    while (*end) {
        if (*end == '"') {
            end++;
            while (*end && *end != '"') { if (*end == '\\' && end[1]) end++; end++; }
            if (*end) end++;
            continue;
        }
        if (end[0] == '/' && end[1] == '/') break;
        end++;
    }
    char* v = xstrndup(q, (size_t)(end - q));
    char* t = trim(v);
    if (t != v) memmove(v, t, strlen(t) + 1);
    return v;
}

/* ---------------------------------------------------------------------- */
/* Parse one module.ae                                                     */
/* ---------------------------------------------------------------------- */

static int parse_module_ae(const char* filepath, const char* module_name) {
    char* src = read_file(filepath);
    if (!src) {
        fprintf(stderr, "docgen: cannot read %s\n", filepath);
        return 0;
    }

    /* ---- exported names ---- */
    char** exports = NULL;
    int export_count = parse_exports(src, &exports);
    if (export_count < 0) {
        fprintf(stderr, "docgen: %s: the exports ( list is never closed\n", filepath);
        free(src);
        return 0;
    }
    if (export_count == 0) { free(src); return 1; }

    Module* mod = find_or_create_module(module_name);

    /* Pre-create the function records in export order so the page lists
     * functions in the same order the module author declared them. */
    for (int i = 0; i < export_count; i++) {
        upsert_function(mod, exports[i]);
    }

    /* Fresh extern-return table for inferred-wrapper resolution. */
    extern_ret_reset();

    /* ---- walk the file line by line collecting docs + definitions ---- */
    Str module_doc = {0};
    Str pending_doc = {0};
    int seen_exports = 0;

    char* cursor = src;
    while (*cursor) {
        /* isolate the current physical line [line, eol) */
        char* line = cursor;
        char* eol = strchr(cursor, '\n');
        size_t line_len = eol ? (size_t)(eol - cursor) : strlen(cursor);

        char* buf = xstrndup(line, line_len);
        char* t = trim(buf);

        if (t[0] == '\0') {
            /* blank line: a doc block ends here */
            str_clear(&pending_doc);
        } else if (t[0] == '/' && t[1] == '/') {
            const char* comment = t + 2;
            while (*comment == ' ') comment++;
            /* skip pure separator lines (----, ===) so the doc stays clean */
            int only_sep = 1;
            for (const char* c = comment; *c; c++) {
                if (*c != '-' && *c != '=' && *c != ' ') { only_sep = 0; break; }
            }
            if (!only_sep && *comment) {
                append_doc_line(seen_exports ? &pending_doc : &module_doc, comment);
            }
        } else {
            /* code line */
            if (!seen_exports && strncmp(t, "exports", 7) == 0 &&
                !is_ident_char((unsigned char)t[7])) {
                seen_exports = 1;
                str_clear(&pending_doc);
            }

            char *nm = NULL, *pr = NULL, *rt = NULL;
            if (strncmp(t, "extern", 6) == 0 && isspace((unsigned char)t[6])) {
                const char* defstart = t + 6;
                while (*defstart == ' ' || *defstart == '\t') defstart++;
                /* resolve against the ORIGINAL source so multi-line param
                 * lists are captured: compute defstart's offset in `line`. */
                const char* src_defstart = line + (defstart - buf);
                if (read_callable(src_defstart, &nm, &pr, &rt)) {
                    /* Register EVERY extern's return so thin wrappers can
                     * inherit it, even when the extern itself isn't exported. */
                    extern_ret_add(nm, rt);
                    record_def(mod, exports, export_count, nm, DEF_EXTERN,
                               pr, rt, NULL, str_get(&pending_doc));
                }
            } else if (strncmp(t, "const", 5) == 0 && isspace((unsigned char)t[5])) {
                const char* q = t + 5;
                while (*q == ' ' || *q == '\t') q++;
                const char* name_start = q;
                while (*q && is_ident_char((unsigned char)*q)) q++;
                nm = xstrndup(name_start, (size_t)(q - name_start));
                while (*q == ' ' || *q == '\t') q++;
                char* val = *q == '=' ? const_value(q + 1) : xstrdup("");
                record_def(mod, exports, export_count, nm, DEF_CONST,
                           NULL, NULL, val, str_get(&pending_doc));
                free(val);
            } else if ((strncmp(t, "message", 7) == 0 && isspace((unsigned char)t[7])) ||
                       (strncmp(t, "protocol", 8) == 0 && isspace((unsigned char)t[8]))) {
                /* #385: a message or a protocol, rendered as declared: its
                 * lines up to the closing brace, each trimmed. */
                int is_proto = t[0] == 'p';
                const char* q = t + (is_proto ? 8 : 7);
                while (*q == ' ' || *q == '\t') q++;
                const char* name_start = q;
                while (*q && is_ident_char((unsigned char)*q)) q++;
                nm = xstrndup(name_start, (size_t)(q - name_start));
                char* body = decl_body(line + (t - buf));
                record_def(mod, exports, export_count, nm, is_proto ? DEF_PROTOCOL : DEF_MESSAGE,
                           NULL, NULL, body, str_get(&pending_doc));
                free(body);
            } else if (strncmp(t, "struct", 6) == 0 && isspace((unsigned char)t[6])) {
                const char* q = t + 6;
                while (*q == ' ' || *q == '\t') q++;
                const char* name_start = q;
                while (*q && is_ident_char((unsigned char)*q)) q++;
                nm = xstrndup(name_start, (size_t)(q - name_start));
                record_def(mod, exports, export_count, nm, DEF_STRUCT,
                           NULL, NULL, NULL, str_get(&pending_doc));
            } else if (seen_exports && is_ident_char((unsigned char)t[0]) &&
                       !isdigit((unsigned char)t[0])) {
                /* Candidate Aether wrapper definition. */
                const char* src_start = line + (t - buf);
                if (read_callable(src_start, &nm, &pr, &rt)) {
                    /* Wrappers written `name(...) -> { ... }` declare no
                     * explicit return type; reconstruct it from the body's
                     * first `return` statement (Go-style (value, err) tuples). */
                    if (rt[0] == '\0') {
                        const char* brace = strchr(src_start, '{');
                        if (brace) { free(rt); rt = infer_return_from_body(brace + 1); }
                    }
                    record_def(mod, exports, export_count, nm, DEF_FUNCTION,
                               pr, rt, NULL, str_get(&pending_doc));
                }
            }
            free(nm);
            free(pr);
            free(rt);

            str_clear(&pending_doc);
        }

        free(buf);
        if (!eol) break;
        cursor = eol + 1;
    }

    char* desc = str_take(&module_doc);
    collapse_ws(desc);
    if (desc[0]) {
        free(mod->description);
        mod->description = desc;
    } else {
        free(desc);
    }
    free(str_take(&pending_doc));
    free_exports(exports, export_count);
    free(src);
    return 1;
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

/* A concise (single-sentence) description for cards and headers, owned:
 * the module header can be long, so it is the first sentence of it. */
static char* short_description(const Module* mod) {
    const char* desc = mod->description[0] ? mod->description : fallback_description(mod->name);
    size_t n = 0;
    while (desc[n]) {
        n++;
        if (desc[n - 1] == '.' && (desc[n] == ' ' || desc[n] == '\0')) break;
    }
    char* out = xstrndup(desc, n);
    collapse_ws(out);
    return out;
}

/* ---------------------------------------------------------------------- */
/* HTML emitters (templates preserved from the original generator)         */
/* ---------------------------------------------------------------------- */

/* The rendered usage call, e.g. "get_string(value)", from the param names
 * only. Owned. */
static char* build_usage(const Function* fn) {
    Str call = {0};
    str_add(&call, fn->name);
    str_add(&call, "(");
    if (fn->params[0]) {
        char* params_copy = xstrdup(fn->params);
        char* save = NULL;
        char* tok = strtok_r(params_copy, ",", &save);
        int first = 1;
        while (tok) {
            char* colon = strchr(tok, ':');
            if (colon) *colon = '\0';
            char* nm = trim(tok);
            if (*nm) {
                if (!first) str_add(&call, ", ");
                str_add(&call, nm);
                first = 0;
            }
            tok = strtok_r(NULL, ",", &save);
        }
        free(params_copy);
    }
    str_add(&call, ")");
    return str_take(&call);
}

static const char* kind_label(DefKind k) {
    switch (k) {
        case DEF_EXTERN:   return "extern";
        case DEF_CONST:    return "const";
        case DEF_STRUCT:   return "struct";
        case DEF_MESSAGE:  return "message";
        case DEF_PROTOCOL: return "protocol";
        case DEF_FUNCTION: default: return "fn";
    }
}

/* Open `<output_dir>/<name>` for writing, binary so the committed pages are
 * LF on every platform. NULL, said, when it cannot be created. */
static FILE* open_output(const char* output_dir, const char* name, Str* path) {
    str_clear(path);
    str_add(path, output_dir);
    str_add(path, "/");
    str_add(path, name);
    FILE* f = fopen(str_get(path), "wb");
    if (!f) fprintf(stderr, "docgen: cannot create %s\n", str_get(path));
    return f;
}

static void emit_head(FILE* f, const char* title) {
    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "  <meta charset=\"UTF-8\">\n");
    fprintf(f, "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "  <title>%s</title>\n", title);
    fprintf(f, "  <link rel=\"stylesheet\" href=\"style.css\">\n");
    fprintf(f, "</head>\n<body>\n");
}

static int generate_index(const char* output_dir) {
    Str path = {0};
    FILE* f = open_output(output_dir, "index.html", &path);
    if (!f) { free(str_take(&path)); return 0; }

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
        char* desc = short_description(modules[i]);
        fprintf(f, "    <a href=\"%s.html\" class=\"module-card\">\n", modules[i]->name);
        fprintf(f, "      <h3>%s</h3>\n", modules[i]->name);
        fputs("      <p>", f);
        html_out(f, desc);
        fputs("</p>\n", f);
        fprintf(f, "      <span class=\"count\">%d exports</span>\n", modules[i]->function_count);
        fprintf(f, "    </a>\n");
        free(desc);
    }
    fprintf(f, "  </div>\n");

    /* Hidden search index across every exported symbol. */
    fprintf(f, "  <div id=\"search-results\" class=\"search-results hidden\">\n");
    fprintf(f, "    <h2>Search Results</h2>\n");
    fprintf(f, "    <ul class=\"function-list\" id=\"all-functions\">\n");
    for (int i = 0; i < module_count; i++) {
        for (int j = 0; j < modules[i]->function_count; j++) {
            const char* name = modules[i]->functions[j].name;
            fputs("      <li class=\"function-item\" data-name=\"", f);
            html_out(f, name);
            fprintf(f, "\"><a href=\"%s.html#", modules[i]->name);
            html_out(f, name);
            fputs("\"><span class=\"fn-name\">", f);
            html_out(f, name);
            fprintf(f, "</span><span class=\"fn-module\">%s</span></a></li>\n", modules[i]->name);
        }
    }
    fprintf(f, "    </ul>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "</main>\n");

    fprintf(f, "<script src=\"search.js\"></script>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("Generated: %s\n", str_get(&path));
    free(str_take(&path));
    return 1;
}

static int generate_module_page(const char* output_dir, Module* mod) {
    Str path = {0}, title = {0};
    str_add(&title, mod->name);
    str_add(&title, ".html");
    FILE* f = open_output(output_dir, str_get(&title), &path);
    if (!f) { free(str_take(&path)); free(str_take(&title)); return 0; }

    str_clear(&title);
    str_add(&title, mod->name);
    str_add(&title, " - Aether");
    emit_head(f, str_get(&title));
    free(str_take(&title));

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
        fputs("    <li><a href=\"#", f);
        html_out(f, mod->functions[i].name);
        fputs("\">", f);
        html_out(f, mod->functions[i].name);
        fputs("</a></li>\n", f);
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
    char* short_desc = short_description(mod);
    fprintf(f, "<main class=\"content\">\n");
    fprintf(f, "  <div class=\"module-header\">\n");
    fprintf(f, "    <h1>%s</h1>\n", mod->name);
    fputs("    <p class=\"module-desc\">", f);
    html_out(f, short_desc);
    fputs("</p>\n", f);
    fprintf(f, "    <p class=\"import-line\"><code>import std.%s</code></p>\n", mod->name);
    fprintf(f, "  </div>\n");
    free(short_desc);

    fprintf(f, "  <div class=\"functions\" id=\"functions\">\n");
    for (int i = 0; i < mod->function_count; i++) {
        Function* fn = &mod->functions[i];

        fputs("    <div class=\"function\" id=\"", f);
        html_out(f, fn->name);
        fputs("\" data-name=\"", f);
        html_out(f, fn->name);
        fputs("\">\n", f);
        fputs("      <h3 class=\"function-name\">", f);
        html_out(f, fn->name);
        fprintf(f, " <span class=\"kind\">%s</span></h3>\n", kind_label(fn->kind));

        if (fn->kind == DEF_CONST) {
            fputs("      <pre class=\"usage\"><code>", f);
            html_out(f, fn->name);
            if (fn->value[0]) fputs(" = ", f);
            html_out(f, fn->value);
            fputs("</code></pre>\n", f);
        } else if (fn->kind == DEF_MESSAGE || fn->kind == DEF_PROTOCOL) {
            fputs("      <pre class=\"usage\"><code>", f);
            fputs(fn->kind == DEF_PROTOCOL ? "protocol " : "message ", f);
            html_out(f, fn->name);
            fputs(" {", f);
            html_out(f, fn->value);
            fputs("}</code></pre>\n", f);
        } else if (fn->kind == DEF_STRUCT) {
            fputs("      <pre class=\"usage\"><code>struct ", f);
            html_out(f, fn->name);
            fputs("</code></pre>\n", f);
        } else {
            char* call = build_usage(fn);
            fputs("      <pre class=\"usage\"><code>", f);
            html_out(f, call);
            if (fn->return_type[0]) {
                fputs(" <span class=\"arrow\">-&gt;</span> ", f);
                html_out(f, fn->return_type);
            }
            fputs("</code></pre>\n", f);
            free(call);

            /* Full typed parameter line. */
            if (fn->params[0]) {
                fputs("      <p class=\"signature\"><span class=\"sig-params\">", f);
                html_out(f, fn->params);
                fputs("</span></p>\n", f);
            }
        }

        if (fn->doc[0]) {
            fputs("      <p class=\"doc\">", f);
            html_out(f, fn->doc);
            fputs("</p>\n", f);
        }
        fprintf(f, "    </div>\n");
    }
    fprintf(f, "  </div>\n");
    fprintf(f, "</main>\n");

    fprintf(f, "<script src=\"search.js\"></script>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("Generated: %s\n", str_get(&path));
    free(str_take(&path));
    return 1;
}

static int generate_css(const char* output_dir) {
    Str path = {0};
    FILE* f = open_output(output_dir, "style.css", &path);
    if (!f) { free(str_take(&path)); return 0; }

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
    printf("Generated: %s\n", str_get(&path));
    free(str_take(&path));
    return 1;
}

static int generate_search_js(const char* output_dir) {
    Str path = {0};
    FILE* f = open_output(output_dir, "search.js", &path);
    if (!f) { free(str_take(&path)); return 0; }

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
    printf("Generated: %s\n", str_get(&path));
    free(str_take(&path));
    return 1;
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
        fprintf(stderr, "docgen: cannot open %s\n", std_dir);
        return 1;
    }

    /* A module that cannot be read, or a page that cannot be written, fails
     * the run: docs/api missing a module would otherwise pass for complete. */
    int ok = 1;
    Str subdir = {0}, module_ae = {0};
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        str_clear(&subdir);
        str_add(&subdir, std_dir);
        str_add(&subdir, "/");
        str_add(&subdir, entry->d_name);

        struct stat st;
        if (stat(str_get(&subdir), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        str_clear(&module_ae);
        str_add(&module_ae, str_get(&subdir));
        str_add(&module_ae, "/module.ae");
        if (stat(str_get(&module_ae), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        printf("Scanning module: %s\n", entry->d_name);
        if (!parse_module_ae(str_get(&module_ae), entry->d_name)) ok = 0;
    }
    closedir(dir);
    free(str_take(&subdir));
    free(str_take(&module_ae));

    /* Sort modules alphabetically for a stable, predictable layout. */
    qsort(modules, module_count, sizeof(*modules), module_cmp);

    int total = 0;
    printf("\nParsed %d modules:\n", module_count);
    for (int i = 0; i < module_count; i++) {
        printf("  - %s: %d exports\n", modules[i]->name, modules[i]->function_count);
        total += modules[i]->function_count;
    }
    printf("  total exports: %d\n", total);

    printf("\nGenerating documentation...\n");
    if (!generate_css(output_dir)) ok = 0;
    if (!generate_search_js(output_dir)) ok = 0;
    if (!generate_index(output_dir)) ok = 0;
    for (int i = 0; i < module_count; i++) {
        if (!generate_module_page(output_dir, modules[i])) ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "docgen: the documentation is incomplete; see the errors above\n");
        return 1;
    }
    printf("\nDone! Open %s/index.html in a browser.\n", output_dir);
    return 0;
}

