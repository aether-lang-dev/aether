/* `ae pad` (#3775): the layout report of Aether structs, messages and actor
 * state, a lint gate on the bytes a field order wastes, a proof of the
 * report against the C compiler, and a rewrite of the source to the tighter
 * order.
 *
 * The compiler does the layout (`aetherc --emit=layout`, from the table in
 * compiler/analysis/type_layout.c) and decides what may move; this driver
 * walks the paths, totals the files, and edits the source only where the
 * compiler's `--layout-plan` says a struct is reorderable and smaller in the
 * suggested order. A rewrite moves whole fields, each with the comment lines
 * above it and the comment after it, and keeps the separators where they
 * were, so the file reads as before with its fields in another order. */

#include "ae_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void ae_pad_usage(FILE* out) {
    fprintf(out,
        "Usage: ae pad [paths...] [--json] [--min-waste=N] [--ptr=4|8] [--target=<triple>]\n"
        "              [--verify] [--fix]\n"
        "  Report the layout of every struct, message and actor the .ae files declare\n"
        "  (directories are searched for .ae files): each field's offset and size, the\n"
        "  holes between them and the tail, the order that would be smallest and what\n"
        "  it saves, and whether `--fix` may rewrite the struct. With no path, the\n"
        "  project's src/ directory.\n"
        "\nOptions:\n"
        "  --json          One JSON object: the report of each file and the totals\n"
        "  --min-waste=N   Exit 1 when a struct --fix may rewrite would save N bytes or\n"
        "                  more (a lint gate for CI)\n"
        "  --ptr=4|8       Lay out for 4-byte pointers (i386 SysV) or 8 (the default)\n"
        "  --target=<t>    Lay out for a target triple (x86_64-linux, i386-linux,\n"
        "                  arm-linux-gnueabihf, aarch64-macos, wasm32-wasi, ...)\n"
        "  --verify        Compile the program's C with a _Static_assert of every offset,\n"
        "                  size and alignment reported, -fsyntax-only, with the C compiler\n"
        "                  ae builds with (-m32 for --ptr=4), proving the report\n"
        "  --fix           Rewrite each reorderable struct to its smallest order, keeping\n"
        "                  each field's comments with it; a second run changes nothing.\n"
        "                  Messages, actor state and any struct C may see are never\n"
        "                  rewritten (docs/ae-pad.md lists the reasons)\n");
}

/* ---- the files ---- */

typedef struct {
    char** v;
    int n;
    int cap;
} PadList;

static void pad_list_add(PadList* l, const char* s) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char** nv = (char**)realloc(l->v, (size_t)cap * sizeof(char*));
        if (!nv) return;
        l->v = nv;
        l->cap = cap;
    }
    char* d = (char*)malloc(strlen(s) + 1);
    if (!d) return;
    strcpy(d, s);
    l->v[l->n++] = d;
}

static int pad_has_ae_ext(const char* name) {
    size_t n = strlen(name);
    return n > 3 && strcmp(name + n - 3, ".ae") == 0;
}

static int pad_cmp(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

/* The .ae files under `path`, hidden directories left out. 0 when the path
 * does not exist. */
static int pad_walk(const char* path, PadList* out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        pad_list_add(out, path);
        return 1;
    }
    DIR* d = opendir(path);
    if (!d) return 0;
    PadList here = { NULL, 0, 0 };
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t need = strlen(path) + strlen(e->d_name) + 2;
        char* child = (char*)malloc(need);
        if (!child) continue;
        size_t pl = strlen(path);
        snprintf(child, need, "%s%s%s", path,
                 pl && (path[pl - 1] == '/' || path[pl - 1] == '\\') ? "" : "/", e->d_name);
        struct stat cst;
        if (stat(child, &cst) == 0 && (S_ISDIR(cst.st_mode) || pad_has_ae_ext(e->d_name)))
            pad_list_add(&here, child);
        free(child);
    }
    closedir(d);
    /* readdir's order is the file system's; a report is in name order. */
    qsort(here.v, (size_t)here.n, sizeof(char*), pad_cmp);
    for (int i = 0; i < here.n; i++) {
        pad_walk(here.v[i], out);
        free(here.v[i]);
    }
    free(here.v);
    return 1;
}

static char* pad_read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8192, n = 0;
    char* buf = (char*)malloc(cap);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (cap - n - 1 == 0) {
            char* nb = (char*)realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

/* Writes `data` beside `path` and moves it over `path`. */
static int pad_write_file(const char* path, const char* data, size_t len) {
    size_t need = strlen(path) + 16;
    char* tmp = (char*)malloc(need);
    if (!tmp) return -1;
    snprintf(tmp, need, "%s.pad-tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    if (!ok || ae_replace_file(tmp, path) != 0) {
        remove(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* ---- the totals ---- */

typedef struct {
    int files;
    int failed;
    int structs, messages, actors;
    long long holes, tail, saving, all_saving;
    long long largest;
    char largest_name[256];
    char largest_file[1024];
} PadTotals;

static long long pad_num_after(const char* s, const char* key) {
    const char* p = s ? strstr(s, key) : NULL;
    return p ? strtoll(p + strlen(key), NULL, 10) : 0;
}

/* Folds one file's summary into the totals: the text report's last line,
 * or the JSON report's summary object. */
static void pad_add_summary(PadTotals* t, const char* report, int json, const char* file) {
    long long largest;
    const char* name_at;
    if (json) {
        const char* s = strstr(report, "\"summary\": {");
        if (!s) return;
        t->structs += (int)pad_num_after(s, "\"structs\": ");
        t->messages += (int)pad_num_after(s, "\"messages\": ");
        t->actors += (int)pad_num_after(s, "\"actors\": ");
        t->holes += pad_num_after(s, "\"holes\": ");
        t->tail += pad_num_after(s, "\"tail\": ");
        t->saving += pad_num_after(s, "\"reorderable_saving\": ");
        t->all_saving += pad_num_after(s, "\"all_saving\": ");
        largest = pad_num_after(s, "\"largest_saving\": ");
        name_at = strstr(s, "\"largest_name\": \"");
        if (name_at) name_at += strlen("\"largest_name\": \"");
    } else {
        const char* s = strstr(report, "\nsummary: ");
        if (!s) return;
        s += 1;
        char* p = (char*)s + strlen("summary: ");
        t->structs += (int)strtol(p, &p, 10);
        p = strchr(p, ',');
        if (p) t->messages += (int)strtol(p + 2, &p, 10);
        p = p ? strchr(p, ',') : NULL;
        if (p) t->actors += (int)strtol(p + 2, &p, 10);
        t->holes += pad_num_after(s, "; holes ");
        t->tail += pad_num_after(s, ", tail ");
        t->saving += pad_num_after(s, "reorderable saving ");
        t->all_saving += pad_num_after(s, "all saving ");
        largest = pad_num_after(s, "(largest ");
        name_at = strstr(s, "(largest ");
        name_at = name_at ? strstr(name_at, " in ") : NULL;
        if (name_at) name_at += 4;
    }
    if (largest > t->largest && name_at) {
        t->largest = largest;
        size_t k = 0;
        while (name_at[k] && name_at[k] != '"' && name_at[k] != ')' && k + 1 < sizeof(t->largest_name)) {
            t->largest_name[k] = name_at[k];
            k++;
        }
        t->largest_name[k] = '\0';
        snprintf(t->largest_file, sizeof(t->largest_file), "%s", file);
    }
}

/* ---- --fix: rewriting a struct's field list ---- */

/* What each byte of a struct body is: code, a comment or a string, and the
 * bracket depth of the code. */
typedef struct {
    const char* s;
    size_t n;
    char* kind;   /* 'c' code, '/' comment, '"' string */
    int* depth;
} PadBody;

static int pad_classify(PadBody* b) {
    b->kind = (char*)malloc(b->n + 1);
    b->depth = (int*)malloc((b->n + 1) * sizeof(int));
    if (!b->kind || !b->depth) return 0;
    int depth = 0;
    for (size_t i = 0; i < b->n; i++) {
        char c = b->s[i];
        if (c == '/' && i + 1 < b->n && b->s[i + 1] == '/') {
            while (i < b->n && b->s[i] != '\n') { b->kind[i] = '/'; b->depth[i] = depth; i++; }
            if (i < b->n) { b->kind[i] = 'c'; b->depth[i] = depth; }
            continue;
        }
        if (c == '/' && i + 1 < b->n && b->s[i + 1] == '*') {
            size_t j = i;
            while (j < b->n && !(b->s[j] == '*' && j + 1 < b->n && b->s[j + 1] == '/')) {
                b->kind[j] = '/'; b->depth[j] = depth; j++;
            }
            for (size_t k = j; k < j + 2 && k < b->n; k++) { b->kind[k] = '/'; b->depth[k] = depth; }
            i = j + 1;
            continue;
        }
        if (c == '"') {
            size_t j = i;
            b->kind[j] = '"'; b->depth[j] = depth; j++;
            while (j < b->n && b->s[j] != '"' && b->s[j] != '\n') {
                if (b->s[j] == '\\' && j + 1 < b->n) { b->kind[j] = '"'; b->depth[j] = depth; j++; }
                b->kind[j] = '"'; b->depth[j] = depth; j++;
            }
            if (j < b->n) { b->kind[j] = '"'; b->depth[j] = depth; }
            i = j;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        b->kind[i] = 'c';
        b->depth[i] = depth;
        if (c == ')' || c == ']' || c == '}') { depth--; b->depth[i] = depth; }
    }
    return 1;
}

static int pad_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* The start of field `name` (its `using` included) at depth 0 at or after
 * `from`, written `name: type`; (size_t)-1 when there is none. */
static size_t pad_find_field(const PadBody* b, size_t from, const char* name) {
    size_t nl = strlen(name);
    size_t using_at = (size_t)-1;
    for (size_t i = from; i < b->n; i++) {
        if (b->kind[i] != 'c' || b->depth[i] != 0) continue;
        if (!pad_ident_char(b->s[i]) || (i > 0 && pad_ident_char(b->s[i - 1]))) continue;
        size_t j = i;
        while (j < b->n && pad_ident_char(b->s[j])) j++;
        if (j - i == 5 && strncmp(b->s + i, "using", 5) == 0) { using_at = i; i = j - 1; continue; }
        if (j - i == nl && strncmp(b->s + i, name, nl) == 0) {
            size_t k = j;
            while (k < b->n && (b->s[k] == ' ' || b->s[k] == '\t')) k++;
            if (k < b->n && b->s[k] == ':' && b->kind[k] == 'c') {
                if (using_at != (size_t)-1) {
                    int only_space = 1;
                    for (size_t q = using_at + 5; q < i; q++)
                        if (b->s[q] != ' ' && b->s[q] != '\t') only_space = 0;
                    if (only_space) return using_at;
                }
                return i;
            }
        }
        using_at = (size_t)-1;
        i = j - 1;
    }
    return (size_t)-1;
}

static size_t pad_line_start(const char* s, size_t at) {
    while (at > 0 && s[at - 1] != '\n') at--;
    return at;
}

static size_t pad_line_end(const PadBody* b, size_t at) {
    while (at < b->n && b->s[at] != '\n') at++;
    return at < b->n ? at + 1 : at;
}

static int pad_count_lines(const char* s, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from; i < to; i++) if (s[i] == '\n') n++;
    return n;
}

/* Whether [from, to) holds only blank lines and comments. */
static int pad_only_comments(const PadBody* b, size_t from, size_t to) {
    for (size_t i = from; i < to; i++)
        if (b->kind[i] == 'c' && !isspace((unsigned char)b->s[i])) return 0;
    return 1;
}

typedef struct {
    char* s;
    size_t n;
    size_t cap;
} PadBuf;

static int pad_put(PadBuf* o, const char* s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (o->n + n + 1 > cap) cap *= 2;
        char* ns = (char*)realloc(o->s, cap);
        if (!ns) return 0;
        o->s = ns;
        o->cap = cap;
    }
    memcpy(o->s + o->n, s, n);
    o->n += n;
    o->s[o->n] = '\0';
    return 1;
}

/* Rewrites the body of struct `name` (whose `struct` keyword is on line
 * `line`) of `src` so its fields come in the order `sug` (indices into
 * `decl`, the fields in declaration order). Returns the new source, or NULL
 * with the reason in *why. */
static char* pad_rewrite(const char* src, size_t len, int line, const char* name,
                         char** decl, const int* sug, int n, const char** why) {
    size_t at = 0;
    for (int l = 1; l < line && at < len; at++) if (src[at] == '\n') l++;
    /* `struct Name` on that line. */
    size_t nl = strlen(name), kw = (size_t)-1;
    for (size_t i = at; i < len && src[i] != '\n'; i++) {
        if (strncmp(src + i, "struct", 6) != 0 || (i > 0 && pad_ident_char(src[i - 1]))) continue;
        size_t j = i + 6;
        if (j >= len || !isspace((unsigned char)src[j])) continue;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
        if (strncmp(src + j, name, nl) == 0 && !pad_ident_char(src[j + nl])) { kw = j + nl; break; }
    }
    if (kw == (size_t)-1) { *why = "its `struct` line is not where the compiler says"; return NULL; }
    size_t open = kw;
    while (open < len && src[open] != '{') {
        if (src[open] == '/' && open + 1 < len && src[open + 1] == '/')
            while (open < len && src[open] != '\n') open++;
        else open++;
    }
    if (open >= len) { *why = "its body was not found"; return NULL; }
    /* The matching brace, past strings and comments. */
    PadBody all = { src + open + 1, len - open - 1, NULL, NULL };
    if (!pad_classify(&all)) { *why = "out of memory"; return NULL; }
    size_t close = (size_t)-1;
    for (size_t i = 0; i < all.n; i++)
        if (all.kind[i] == 'c' && all.s[i] == '}' && all.depth[i] == -1) { close = i; break; }
    free(all.kind);
    free(all.depth);
    if (close == (size_t)-1) { *why = "its closing brace was not found"; return NULL; }

    PadBody b = { src + open + 1, close, NULL, NULL };
    size_t* s_at = (size_t*)calloc((size_t)n + 1, sizeof(size_t));
    size_t* e_at = (size_t*)calloc((size_t)n + 1, sizeof(size_t));
    size_t* sep_at = (size_t*)calloc((size_t)n + 1, sizeof(size_t));
    size_t* u_at = (size_t*)calloc((size_t)n + 1, sizeof(size_t));
    size_t* v_at = (size_t*)calloc((size_t)n + 1, sizeof(size_t));
    char* out = NULL;
    PadBuf o = { NULL, 0, 0 };
    if (!s_at || !e_at || !sep_at || !u_at || !v_at || !pad_classify(&b)) {
        *why = "out of memory";
        goto done;
    }
    for (int i = 0; i < n; i++) {
        size_t from = i == 0 ? 0 : s_at[i - 1] + strlen(decl[i - 1]);
        s_at[i] = pad_find_field(&b, from, decl[i]);
        if (s_at[i] == (size_t)-1) {
            static char why_buf[512];
            snprintf(why_buf, sizeof(why_buf),
                     "field `%s` is not written `%s: type` where it is expected", decl[i], decl[i]);
            *why = why_buf;
            goto done;
        }
    }
    for (int i = 0; i < n; i++) {
        size_t limit = i + 1 < n ? s_at[i + 1] : b.n;
        size_t e = s_at[i] + strlen(decl[i]);
        while (e < limit && !(b.kind[e] == '/' ||
                              (b.kind[e] == 'c' && b.depth[e] == 0 &&
                               (b.s[e] == ',' || b.s[e] == ';' || b.s[e] == '\n'))))
            e++;
        while (e > s_at[i] && (b.s[e - 1] == ' ' || b.s[e - 1] == '\t' || b.s[e - 1] == '\r')) e--;
        e_at[i] = e;
        size_t k = e;
        while (k < limit && (b.s[k] == ' ' || b.s[k] == '\t')) k++;
        sep_at[i] = (k < limit && b.kind[k] == 'c' && (b.s[k] == ',' || b.s[k] == ';')) ? k : (size_t)-1;
    }
    int one_line = pad_count_lines(b.s, 0, b.n) == 0 ||
                   pad_count_lines(b.s, s_at[0], e_at[n - 1]) == 0;
    if (one_line) {
        /* `{ a: byte, b: long }`: the fields move and every gap, with the
         * separator in it, stays where it was. */
        for (size_t i = s_at[0]; i < e_at[n - 1]; i++)
            if (b.kind[i] == '/') { *why = "a comment sits between fields on one line"; goto done; }
        if (!pad_put(&o, b.s, s_at[0])) goto oom;
        for (int p = 0; p < n; p++) {
            int f = sug[p];
            if (!pad_put(&o, b.s + s_at[f], e_at[f] - s_at[f])) goto oom;
            size_t g0 = e_at[p], g1 = p + 1 < n ? s_at[p + 1] : b.n;
            if (!pad_put(&o, b.s + g0, g1 - g0)) goto oom;
        }
    } else {
        /* One field per line: a field moves with the comment lines above it
         * and the comment after it; each line keeps the separator its place
         * had. */
        for (int i = 0; i < n; i++) {
            size_t ls = pad_line_start(b.s, s_at[i]);
            if (ls == 0 && pad_count_lines(b.s, 0, s_at[i]) == 0) {
                *why = "a field shares the line of the opening brace";
                goto done;
            }
            for (size_t q = ls; q < s_at[i]; q++)
                if (b.s[q] != ' ' && b.s[q] != '\t') { *why = "two fields share a line"; goto done; }
            if (pad_count_lines(b.s, s_at[i], e_at[i]) != 0) {
                *why = "a field spans several lines";
                goto done;
            }
            v_at[i] = pad_line_end(&b, e_at[i]);
            size_t after = sep_at[i] != (size_t)-1 ? sep_at[i] + 1 : e_at[i];
            if (!pad_only_comments(&b, after, v_at[i])) { *why = "two fields share a line"; goto done; }
        }
        size_t first_line_end = pad_line_end(&b, 0);
        for (int i = 0; i < n; i++) {
            size_t floor = i == 0 ? first_line_end : v_at[i - 1];
            size_t u = pad_line_start(b.s, s_at[i]);
            if (!pad_only_comments(&b, floor, u)) { *why = "text between fields is not a comment"; goto done; }
            /* Back over the comment lines that end right above the field; a
             * blank line stops it, so a comment that heads a group stays. */
            while (u > floor) {
                size_t prev = pad_line_start(b.s, u - 1);
                int blank = 1, comment = 0;
                for (size_t q = prev; q < u; q++) {
                    if (b.kind[q] == '/') comment = 1;
                    else if (!isspace((unsigned char)b.s[q])) blank = 0;
                }
                if (!blank || !comment || prev < floor) break;
                u = prev;
            }
            u_at[i] = u;
        }
        if (!pad_only_comments(&b, v_at[n - 1], b.n)) { *why = "text after the last field"; goto done; }
        if (!pad_put(&o, b.s, u_at[0])) goto oom;
        for (int p = 0; p < n; p++) {
            int f = sug[p];
            /* The lines between this place and the previous one stay. */
            if (p > 0 && !pad_put(&o, b.s + v_at[p - 1], u_at[p] - v_at[p - 1])) goto oom;
            if (!pad_put(&o, b.s + u_at[f], e_at[f] - u_at[f])) goto oom;
            if (sep_at[p] != (size_t)-1 && !pad_put(&o, b.s + sep_at[p], 1)) goto oom;
            size_t after = sep_at[f] != (size_t)-1 ? sep_at[f] + 1 : e_at[f];
            if (!pad_put(&o, b.s + after, v_at[f] - after)) goto oom;
        }
        if (!pad_put(&o, b.s + v_at[n - 1], b.n - v_at[n - 1])) goto oom;
    }
    {
        PadBuf whole = { NULL, 0, 0 };
        if (!pad_put(&whole, src, open + 1) || !pad_put(&whole, o.s ? o.s : "", o.n) ||
            !pad_put(&whole, src + open + 1 + close, len - open - 1 - close)) {
            free(whole.s);
            goto oom;
        }
        out = whole.s;
    }
    goto done;
oom:
    *why = "out of memory";
done:
    free(o.s);
    free(b.kind);
    free(b.depth);
    free(s_at);
    free(e_at);
    free(sep_at);
    free(u_at);
    free(v_at);
    return out;
}

/* ---- the commands ---- */

typedef struct {
    int json;
    long long min_waste;
    const char* layout_flags;   /* " --ptr=4", " --target=...", or "" */
    int ptr4;
    int target_set;
    int verify;
    int fix;
} PadOpts;

/* Splits a comma-separated list in place. */
static int pad_split(char* s, char** out, int cap) {
    int n = 0;
    while (s && *s && n < cap) {
        out[n++] = s;
        char* c = strchr(s, ',');
        if (!c) break;
        *c = '\0';
        s = c + 1;
    }
    return n;
}

static int pad_index_of(char** names, int n, const char* name) {
    for (int i = 0; i < n; i++) if (strcmp(names[i], name) == 0) return i;
    return -1;
}

/* `--fix` on one file: the compiler's plan, then each struct rewritten.
 * The file is compiled again afterwards; a rewrite that does not compile is
 * undone. */
static int pad_fix_file(const char* path, const PadOpts* o, int* fixed) {
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout --layout-plan%s", o->layout_flags);
    char* plan = NULL;
    if (aetherc_capture_stdout_whole(flags, path, NULL, &plan) != 0) {
        fprintf(stderr, "ae pad: %s does not compile; nothing rewritten\n", path);
        free(plan);
        return 1;
    }
    if (!strstr(plan, "fix\t")) { free(plan); return 0; }
    size_t len = 0;
    char* src = pad_read_file(path, &len);
    if (!src) { fprintf(stderr, "ae pad: cannot read %s\n", path); free(plan); return 1; }
    char* orig = (char*)malloc(len + 1);
    if (!orig) { free(src); free(plan); return 1; }
    memcpy(orig, src, len + 1);
    size_t orig_len = len;
    int rc = 0, changed = 0;
    char* line = plan;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';
        /* fix <file> <line> <column> <name> <size> <new size> <fields> <order> */
        char* f[9];
        int nf = 0;
        if (strncmp(line, "fix\t", 4) == 0) {
            char* p = line;
            while (nf < 9) {
                f[nf++] = p;
                char* tab = strchr(p, '\t');
                if (!tab) break;
                *tab = '\0';
                p = tab + 1;
            }
        }
        if (nf == 9) {
            int sline = atoi(f[2]);
            const char* name = f[4];
            char* decl[1024];
            char* sugn[1024];
            int nd = pad_split(f[7], decl, 1024);
            int ns = pad_split(f[8], sugn, 1024);
            int* sug = (int*)malloc((size_t)(nd > 0 ? nd : 1) * sizeof(int));
            int ok = sug && nd == ns && nd > 0;
            for (int i = 0; ok && i < ns; i++) {
                sug[i] = pad_index_of(decl, nd, sugn[i]);
                if (sug[i] < 0) ok = 0;
            }
            const char* why = "the compiler's plan does not name its fields";
            char* out = ok ? pad_rewrite(src, len, sline, name, decl, sug, nd, &why) : NULL;
            if (out) {
                free(src);
                src = out;
                len = strlen(src);
                changed = 1;
                (*fixed)++;
                printf("%s:%d: struct %s reordered to ", path, sline, name);
                for (int i = 0; i < ns; i++) printf("%s%s", i ? ", " : "", sugn[i]);
                printf(" (%s -> %s bytes)\n", f[5], f[6]);
            } else {
                fprintf(stderr, "ae pad: %s:%d: struct %s left as it is: %s\n", path, sline,
                        name, why);
                rc = 1;
            }
            free(sug);
        }
        line = next;
    }
    free(plan);
    plan = NULL;
    if (changed) {
        if (pad_write_file(path, src, len) != 0) {
            fprintf(stderr, "ae pad: cannot write %s\n", path);
            rc = 1;
        } else if (aetherc_capture_stdout_whole(flags, path, NULL, &plan) != 0) {
            /* Never leave a file that stopped compiling. */
            fprintf(stderr, "ae pad: %s does not compile after the rewrite; it is restored\n", path);
            pad_write_file(path, orig, orig_len);
            rc = 1;
        }
        free(plan);
    }
    free(src);
    free(orig);
    return rc;
}

/* `--verify` on one file: its C with the report's numbers as
 * _Static_asserts, compiled -fsyntax-only by the C compiler ae builds with. */
static int pad_verify_file(const char* path, const PadOpts* o, int* asserts) {
    static int seq = 0;
    char cpath[1200], name[64];
    snprintf(name, sizeof(name), "ae_pad_verify_%d.c", seq++);
    if (!ae_private_temp_path(cpath, sizeof(cpath), name)) {
        fprintf(stderr, "ae pad: no temporary directory\n");
        return 1;
    }
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout --layout-asserts%s", o->layout_flags);
    char* text = NULL;
    if (aetherc_capture_stdout_whole(flags, path, cpath, &text) != 0) {
        fprintf(stderr, "ae pad: %s does not compile; not verified\n", path);
        free(text);
        remove(cpath);
        return 1;
    }
    int n = 0;
    for (const char* p = text; (p = strstr(p, "_Static_assert(")) != NULL; p++) n++;
    FILE* f = fopen(cpath, "ab");
    int ok = f && fputs(text, f) >= 0;
    if (f && fclose(f) != 0) ok = 0;
    free(text);
    if (!ok) { fprintf(stderr, "ae pad: cannot write %s\n", cpath); remove(cpath); return 1; }
    char* cmd = (char*)malloc(AE_CMD_BUF);
    if (!cmd) { remove(cpath); return 1; }
    /* --ptr=4 asks the compiler for 32-bit x86. MinGW's i686 layout is the
     * Microsoft one, which aligns int64_t and double to 8 inside a struct;
     * the i386 SysV placement the table models is asked for by name. GCC
     * notes on each 64-bit atomic field there that its alignment changed in
     * GCC 11, which says nothing about this program; -Wno-psabi drops it. */
#ifdef _WIN32
    const char* m32 = " -m32 -mno-align-double -mno-ms-bitfields -Wno-psabi";
#else
    const char* m32 = " -m32 -Wno-psabi";
#endif
    build_syntax_check_cmd(cmd, AE_CMD_BUF, cpath, o->ptr4 ? m32 : "");
    int rc = run_cmd(cmd);
    free(cmd);
    remove(cpath);
    if (rc != 0) {
        fprintf(stderr, "ae pad: %s: the C compiler does not agree with the layout report "
                        "(the failed _Static_assert names the number)\n", path);
        return 1;
    }
    *asserts += n;
    printf("verified: %s (%d asserts)\n", path, n);
    return 0;
}

int cmd_pad(int argc, char** argv) {
    PadOpts o = { 0, 0, "", 0, 0, 0, 0 };
    static char layout_flags[512];
    PadList files = { NULL, 0, 0 };
    PadList paths = { NULL, 0, 0 };
    int bad = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            ae_pad_usage(stdout);
            return 0;
        } else if (strcmp(a, "--json") == 0) {
            o.json = 1;
        } else if (strcmp(a, "--verify") == 0) {
            o.verify = 1;
        } else if (strcmp(a, "--fix") == 0) {
            o.fix = 1;
        } else if (strncmp(a, "--min-waste=", 12) == 0) {
            char* end = NULL;
            o.min_waste = strtoll(a + 12, &end, 10);
            if (!end || *end || o.min_waste < 1) {
                fprintf(stderr, "ae pad: --min-waste= takes a number of bytes, 1 or more\n");
                return 2;
            }
        } else if (strcmp(a, "--ptr=4") == 0 || strcmp(a, "--ptr=8") == 0) {
            snprintf(layout_flags, sizeof(layout_flags), " %s", a);
            o.ptr4 = a[6] == '4';
            o.target_set = 0;
        } else if (strncmp(a, "--target=", 9) == 0 && a[9]) {
            /* aetherc checks the triple against its tables. */
            snprintf(layout_flags, sizeof(layout_flags), " --target=%s", a + 9);
            o.target_set = 1;
            o.ptr4 = 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "ae pad: unknown option '%s'\n", a);
            ae_pad_usage(stderr);
            return 2;
        } else {
            pad_list_add(&paths, a);
        }
    }
    o.layout_flags = layout_flags;
    if (o.verify && o.target_set) {
        fprintf(stderr, "ae pad: --verify compiles with this host's C compiler, so it takes "
                        "--ptr=4|8, not --target=\n");
        return 2;
    }
    if (paths.n == 0) {
        if (path_exists("aether.toml") && path_exists("src")) {
            pad_list_add(&paths, "src");
        } else {
            fprintf(stderr, "ae pad: name the files or directories to report\n");
            ae_pad_usage(stderr);
            return 2;
        }
    }
    for (int i = 0; i < paths.n; i++) {
        if (!pad_walk(paths.v[i], &files)) {
            fprintf(stderr, "ae pad: no such path: %s\n", paths.v[i]);
            bad = 1;
        }
    }

    PadTotals t;
    memset(&t, 0, sizeof(t));
    int report = (!o.fix && !o.verify) || o.min_waste > 0;
    int fixed = 0, asserts = 0, verified = 0;
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout%s%s", o.json ? " --json" : "", o.layout_flags);
    if (report && o.json) printf("{\"files\": [");
    int first = 1;
    for (int i = 0; i < files.n; i++) {
        const char* path = files.v[i];
        if (o.fix && pad_fix_file(path, &o, &fixed) != 0) bad = 1;
        if (o.verify) {
            if (pad_verify_file(path, &o, &asserts) != 0) bad = 1;
            else verified++;
        }
        if (!report) continue;
        char* text = NULL;
        if (aetherc_capture_stdout_whole(flags, path, NULL, &text) != 0) {
            fprintf(stderr, "ae pad: %s does not compile; skipped\n", path);
            t.failed++;
            free(text);
            continue;
        }
        t.files++;
        pad_add_summary(&t, text, o.json, path);
        if (o.json) {
            size_t tl = strlen(text);
            while (tl && (text[tl - 1] == '\n' || text[tl - 1] == '\r')) text[--tl] = '\0';
            printf("%s\n%s", first ? "" : ",", text);
            first = 0;
        } else {
            fputs(text, stdout);
            if (files.n > 1) fputc('\n', stdout);
        }
        free(text);
    }
    if (report && o.json) {
        printf("],\n \"totals\": {\"files\": %d, \"failed\": %d, \"structs\": %d, \"messages\": %d, "
               "\"actors\": %d, \"holes\": %lld, \"tail\": %lld, \"reorderable_saving\": %lld, "
               "\"largest_saving\": %lld, \"all_saving\": %lld}}\n",
               t.files, t.failed, t.structs, t.messages, t.actors, t.holes, t.tail, t.saving,
               t.largest, t.all_saving);
    } else if (report && files.n > 1) {
        printf("ae pad: %d file%s (%d not compiled), %d structs, %d messages, %d actors; holes "
               "%lld, tail %lld; reorderable saving %lld (largest %lld%s%s); all saving %lld\n",
               t.files, t.files == 1 ? "" : "s", t.failed, t.structs, t.messages, t.actors,
               t.holes, t.tail, t.saving, t.largest, t.largest ? " in " : "",
               t.largest ? t.largest_name : "", t.all_saving);
    }
    if (o.fix) printf("ae pad --fix: %d struct%s rewritten\n", fixed, fixed == 1 ? "" : "s");
    if (o.verify)
        printf("ae pad --verify: %d file%s, %d asserts held\n", verified,
               verified == 1 ? "" : "s", asserts);
    int gate = 0;
    if (o.min_waste > 0 && t.largest >= o.min_waste) {
        fprintf(stderr, "ae pad: struct %s in %s can save %lld bytes, at least the %lld "
                        "--min-waste allows; `ae pad --fix` rewrites it\n",
                t.largest_name, t.largest_file, t.largest, o.min_waste);
        gate = 1;
    }
    for (int i = 0; i < files.n; i++) free(files.v[i]);
    for (int i = 0; i < paths.n; i++) free(paths.v[i]);
    free(files.v);
    free(paths.v);
    if (gate) return 1;
    return bad || t.failed ? 2 : 0;
}
