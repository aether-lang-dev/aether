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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

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
        "                  rewritten (docs/ae-pad.md lists the reasons). Run it over the\n"
        "                  whole project: what any file of the run refuses, no file\n"
        "                  rewrites, and a module no program of the run imports is left\n"
        "                  as it is\n");
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

/* Whether `path` is a symbolic link (on Windows, a symlink or a junction:
 * a reparse point of those two tags. Other reparse points are ordinary
 * files and folders, a OneDrive placeholder among them). */
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
static int pad_is_link(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;   /* cannot tell: treated as a link, never followed */
    FindClose(h);
    return fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* `path` as one spelling per file, for telling whether the path a plan
 * names and the path the run walked are the same file. A string the caller
 * frees. */
static char* pad_canon(const char* path) {
#ifdef _WIN32
    char* c = _fullpath(NULL, path, 0);
    if (!c) c = strdup(path);
    for (char* p = c; p && *p; p++) {
        if (*p == '\\') *p = '/';
        *p = (char)tolower((unsigned char)*p);
    }
    return c;
#else
    char* c = realpath(path, NULL);
    return c ? c : strdup(path);
#endif
}

/* The .ae files under `path`, hidden directories left out. A directory
 * reached through a symbolic link is not entered, as find does not by
 * default: a link back up the tree would walk forever. 0 when the path does
 * not exist. */
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
        if (stat(child, &cst) == 0 &&
            ((S_ISDIR(cst.st_mode) && !pad_is_link(child)) ||
             (!S_ISDIR(cst.st_mode) && pad_has_ae_ext(e->d_name))))
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

/* Writes `data` beside `path` and moves it over `path`, keeping the file's
 * mode. The temporary is created exclusively under a name of this process,
 * so no file that happens to be there is truncated or replaced. */
static int pad_write_file(const char* path, const char* data, size_t len) {
    struct stat st;
    int have_mode = stat(path, &st) == 0;
    int mode = have_mode ? (int)(st.st_mode & 07777) : 0644;
    /* A file the run may not write is left alone, and before a temporary
     * exists: on Windows a read-only one could be neither replaced nor its
     * read-only temporary removed. */
    if (have_mode && access(path, W_OK) != 0) return -1;
    size_t need = strlen(path) + 48;
    char* tmp = (char*)malloc(need);
    if (!tmp) return -1;
    static unsigned seq = 0;
    int fd = -1;
    for (int tries = 0; tries < 100 && fd < 0; tries++) {
        snprintf(tmp, need, "%s.pad-%ld-%u", path, (long)getpid(), seq++);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, mode | 0200);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) { free(tmp); return -1; }
    int ok = 1;
    for (size_t done = 0; ok && done < len;) {
        long w = (long)write(fd, data + done, (unsigned)(len - done > 1u << 30 ? 1u << 30 : len - done));
        if (w <= 0) ok = 0;
        else done += (size_t)w;
    }
    if (close(fd) != 0) ok = 0;
    /* open() applied the umask; the file gets the mode it had. */
    if (ok && have_mode && chmod(tmp, mode) != 0) ok = 0;
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
    int block_lines;  /* a block comment spans lines, or does not close */
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
            /* As the lexer reads one (skip_comment): the closing `*` and `/`
             * are looked for after the opening pair, so `/*` followed by `/`
             * is still open. */
            b->kind[i] = '/'; b->depth[i] = depth;
            b->kind[i + 1] = '/'; b->depth[i + 1] = depth;
            size_t j = i + 2;
            while (j < b->n && !(b->s[j] == '*' && j + 1 < b->n && b->s[j + 1] == '/')) {
                if (b->s[j] == '\n') b->block_lines = 1;
                b->kind[j] = '/'; b->depth[j] = depth; j++;
            }
            if (j >= b->n) b->block_lines = 1;   /* not closed inside the body */
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
    PadBody all = { src + open + 1, len - open - 1, NULL, NULL, 0 };
    if (!pad_classify(&all)) { *why = "out of memory"; return NULL; }
    size_t close = (size_t)-1;
    for (size_t i = 0; i < all.n; i++)
        if (all.kind[i] == 'c' && all.s[i] == '}' && all.depth[i] == -1) { close = i; break; }
    free(all.kind);
    free(all.depth);
    if (close == (size_t)-1) { *why = "its closing brace was not found"; return NULL; }

    PadBody b = { src + open + 1, close, NULL, NULL, 0 };
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
    /* A block comment over several lines has no one field to go with, and
     * cutting the body between fields would cut it too. */
    if (b.block_lines) { *why = "a block comment spans lines"; goto done; }
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
            /* `    c: int }`: moved up, that line would put the next field on
             * the same line as this one. */
            if (v_at[i] == b.n && (b.n == 0 || b.s[b.n - 1] != '\n')) {
                *why = "a field shares its line with the closing brace";
                goto done;
            }
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

/* ---- --fix over the whole run ---- */

/* What one file's plan (`aetherc --emit=layout --layout-plan`) says:
 * whether the file is a program, which files' declarations it sees, and
 * its proposed rewrites; its refusals go to the run's list. */
typedef struct {
    char* path;       /* as walked */
    char* canon;
    char* plan;       /* NULL when the file does not compile */
    int has_main;
    char** uses;      /* canonical paths of the files it imports */
    int nuses;
} PadPlan;

typedef struct {
    char* canon;      /* the file that declares the struct */
    char* name;
    char* reason;
    const char* from; /* the file whose plan refused it */
} PadRefusal;

typedef struct {
    PadPlan* plans;
    int nplans;
    PadRefusal* refs;
    int nrefs;
    int refcap;
    /* A file of the run did not compile, so what it hands to C is not
     * known: no module's struct is rewritten. */
    int incomplete;
} PadRun;

/* The next line of `*cur` (cut in place, '\r' dropped), or NULL. */
static char* pad_next_line(char** cur) {
    char* line = *cur;
    if (!line || !*line) return NULL;
    char* nl = strchr(line, '\n');
    if (nl) { *nl = '\0'; *cur = nl + 1; } else { *cur = line + strlen(line); }
    size_t ll = strlen(line);
    if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';
    return line;
}

/* Splits a tab-separated line in place into at most `cap` fields. */
static int pad_tabs(char* line, char** f, int cap) {
    int n = 0;
    while (n < cap) {
        f[n++] = line;
        char* tab = strchr(line, '\t');
        if (!tab) break;
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

/* Phase one: every file's plan, and every struct any of them refuses. */
static void pad_plan_run(PadRun* run, const PadList* files, const PadOpts* o, int* bad) {
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout --layout-plan%s", o->layout_flags);
    run->plans = (PadPlan*)calloc((size_t)(files->n > 0 ? files->n : 1), sizeof(PadPlan));
    if (!run->plans) return;
    run->nplans = files->n;
    for (int i = 0; i < files->n; i++) {
        PadPlan* p = &run->plans[i];
        p->path = files->v[i];
        p->canon = pad_canon(p->path);
        char* text = NULL;
        if (aetherc_capture_stdout_whole(flags, p->path, NULL, &text) != 0) {
            fprintf(stderr, "ae pad: %s does not compile; nothing rewritten in it, nor in any "
                            "module, since what it hands to C is not known\n", p->path);
            free(text);
            *bad = 1;
            run->incomplete = 1;
            continue;
        }
        p->plan = text;
        char* copy = strdup(text);
        char* cur = copy;
        char* line;
        while (copy && (line = pad_next_line(&cur)) != NULL) {
            char* f[4];
            int nf = pad_tabs(line, f, 4);
            if (nf >= 3 && strcmp(f[0], "file") == 0) {
                p->has_main = atoi(f[2]);
            } else if (nf >= 2 && strcmp(f[0], "uses") == 0) {
                char** nv = (char**)realloc(p->uses, (size_t)(p->nuses + 1) * sizeof(char*));
                if (!nv) continue;
                p->uses = nv;
                p->uses[p->nuses++] = pad_canon(f[1]);
            } else if (nf == 4 && strcmp(f[0], "refuse") == 0) {
                if (run->nrefs == run->refcap) {
                    int cap = run->refcap ? run->refcap * 2 : 64;
                    PadRefusal* nr = (PadRefusal*)realloc(run->refs, (size_t)cap * sizeof(PadRefusal));
                    if (!nr) continue;
                    run->refs = nr;
                    run->refcap = cap;
                }
                PadRefusal* r = &run->refs[run->nrefs++];
                r->canon = pad_canon(f[1]);
                r->name = strdup(f[2]);
                r->reason = strdup(f[3]);
                r->from = p->path;
            }
        }
        free(copy);
    }
}

static void pad_run_free(PadRun* run) {
    for (int i = 0; i < run->nplans; i++) {
        PadPlan* p = &run->plans[i];
        free(p->canon);
        free(p->plan);
        for (int k = 0; k < p->nuses; k++) free(p->uses[k]);
        free(p->uses);
    }
    free(run->plans);
    for (int i = 0; i < run->nrefs; i++) {
        free(run->refs[i].canon);
        free(run->refs[i].name);
        free(run->refs[i].reason);
    }
    free(run->refs);
}

/* Whether plan `j` imports the file `canon`. */
static int pad_plan_uses(const PadPlan* p, const char* canon) {
    for (int k = 0; k < p->nuses; k++)
        if (strcmp(p->uses[k], canon) == 0) return 1;
    return 0;
}

/* Why no rewrite of struct `name` in plan `p`'s file may stand, from what
 * the other files of the run see; NULL when none says so. `from` gets the
 * file that refused it. */
static const char* pad_run_refusal(const PadRun* run, const PadPlan* p, const char* name,
                                   const char** from) {
    for (int i = 0; i < run->nrefs; i++) {
        const PadRefusal* r = &run->refs[i];
        if (strcmp(r->name, name) == 0 && strcmp(r->canon, p->canon) == 0) {
            *from = r->from;
            return r->reason;
        }
    }
    if (!p->has_main && run->incomplete) {
        *from = NULL;
        return "a file of this run does not compile, so its uses of this module are not known";
    }
    if (!p->has_main) {
        for (int j = 0; j < run->nplans; j++)
            if (run->plans[j].plan && run->plans[j].has_main && pad_plan_uses(&run->plans[j], p->canon))
                return NULL;
        *from = NULL;
        return "a module's users outside this run are not known (no program in the run imports it)";
    }
    return NULL;
}

/* Phase two on one file: the rewrites its plan proposes that no file of the
 * run refuses. The file and every program of the run that imports it are
 * compiled again afterwards; when one does not, the file is put back. Only
 * rewrites that stand are reported and counted. */
static int pad_fix_file(PadRun* run, int idx, const PadOpts* o, int* fixed) {
    PadPlan* p = &run->plans[idx];
    const char* path = p->path;
    if (!p->plan || !strstr(p->plan, "fix\t")) return 0;
    if (pad_is_link(path)) {
        fprintf(stderr, "ae pad: %s is a symbolic link; --fix rewrites the file it names, not "
                        "the link\n", path);
        return 1;
    }
    size_t len = 0;
    char* src = pad_read_file(path, &len);
    if (!src) { fprintf(stderr, "ae pad: cannot read %s\n", path); return 1; }
    if (memchr(src, '\0', len)) {
        fprintf(stderr, "ae pad: %s holds a NUL byte; nothing rewritten\n", path);
        free(src);
        return 1;
    }
    char* orig = (char*)malloc(len + 1);
    if (!orig) { free(src); return 1; }
    memcpy(orig, src, len + 1);
    size_t orig_len = len;
    int rc = 0, pending = 0;
    PadBuf done_msgs = { NULL, 0, 0 };
    char* copy = strdup(p->plan);
    char* cur = copy;
    char* line;
    while (copy && (line = pad_next_line(&cur)) != NULL) {
        /* fix <file> <line> <column> <name> <size> <new size> <fields> <order> */
        char* f[9];
        if (strncmp(line, "fix\t", 4) != 0 || pad_tabs(line, f, 9) != 9) continue;
        int sline = atoi(f[2]);
        const char* name = f[4];
        const char* from = NULL;
        const char* refused = pad_run_refusal(run, p, name, &from);
        if (refused) {
            printf("%s:%d: struct %s not rewritten: %s%s%s%s\n", path, sline, name, refused,
                   from ? " (seen from " : "", from ? from : "", from ? ")" : "");
            continue;
        }
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
            pending++;
            char msg[2048];
            int m = snprintf(msg, sizeof(msg), "%s:%d: struct %s reordered to ", path, sline, name);
            for (int i = 0; i < ns && m > 0 && (size_t)m < sizeof(msg); i++)
                m += snprintf(msg + m, sizeof(msg) - (size_t)m, "%s%s", i ? ", " : "", sugn[i]);
            if (m > 0 && (size_t)m < sizeof(msg))
                snprintf(msg + m, sizeof(msg) - (size_t)m, " (%s -> %s bytes)\n", f[5], f[6]);
            pad_put(&done_msgs, msg, strlen(msg));
        } else {
            fprintf(stderr, "ae pad: %s:%d: struct %s left as it is: %s\n", path, sline, name, why);
            rc = 1;
        }
        free(sug);
    }
    free(copy);
    if (pending > 0) {
        char flags[600];
        snprintf(flags, sizeof(flags), "--emit=layout --layout-plan%s", o->layout_flags);
        int broke = 0;
        if (pad_write_file(path, src, len) != 0) {
            fprintf(stderr, "ae pad: cannot write %s; nothing rewritten\n", path);
            rc = 1;
            pending = 0;
        } else {
            /* Never leave the run with a file that stopped compiling: this
             * one, or a program of the run built from it. */
            const char* which = NULL;
            char* text = NULL;
            if (aetherc_capture_stdout_whole(flags, path, NULL, &text) != 0) which = path;
            free(text);
            for (int j = 0; j < run->nplans && !which; j++) {
                PadPlan* q = &run->plans[j];
                if (j == idx || !q->plan || !q->has_main || !pad_plan_uses(q, p->canon)) continue;
                text = NULL;
                if (aetherc_capture_stdout_whole(flags, q->path, NULL, &text) != 0) which = q->path;
                free(text);
            }
            if (which) {
                broke = 1;
                rc = 1;
                if (pad_write_file(path, orig, orig_len) != 0)
                    fprintf(stderr, "ae pad: %s does not compile after the rewrite of %s, and %s "
                                    "could not be put back: it holds the rewrite\n",
                            which, path, path);
                else
                    fprintf(stderr, "ae pad: %s does not compile after the rewrite of %s; %s is "
                                    "put back as it was\n", which, path, path);
            }
        }
        if (!broke && pending > 0) {
            if (done_msgs.s) fputs(done_msgs.s, stdout);
            *fixed += pending;
        }
    }
    free(done_msgs.s);
    free(src);
    free(orig);
    return rc;
}

/* What a failed --verify of one file was (#3806). */
enum { PAD_VERIFY_OK = 0, PAD_VERIFY_MISMATCH = 1, PAD_VERIFY_BROKEN = 2 };

/* Whether the log of a failed -fsyntax-only run names a failed static
 * assertion (the layout report disagrees with the compiler) rather than C
 * that does not compile for another reason. With `show`, the lines that
 * say so (the assertions, or the first errors) go to stderr, indented. */
static int pad_verify_explain(const char* log, int show) {
    FILE* f = fopen(log, "rb");
    if (!f) return 0;
    char line[1024];
    int asserted = 0, shown = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "static assertion") || strstr(line, "static_assert failed")) {
            asserted = 1;
            break;
        }
    }
    rewind(f);
    while (show && fgets(line, sizeof(line), f) && shown < 4) {
        int wanted = asserted ? (strstr(line, "static assert") || strstr(line, "static_assert") != NULL)
                              : (strstr(line, "error") != NULL);
        if (!wanted) continue;
        fprintf(stderr, "        %s", line);
        if (!strchr(line, '\n')) fputc('\n', stderr);
        shown++;
    }
    fclose(f);
    return asserted;
}

/* `--verify` on one file: its C with the report's numbers as
 * _Static_asserts, compiled -fsyntax-only by the C compiler ae builds with.
 * PAD_VERIFY_OK, or what failed: an assertion (the report and the compiler
 * disagree on a layout) or the C itself (an ae pad defect, not a layout
 * one), each said as such. */
static int pad_verify_file(const char* path, const PadOpts* o, int* asserts) {
    static int seq = 0;
    char cpath[1200], name[64];
    snprintf(name, sizeof(name), "ae_pad_verify_%d.c", seq++);
    if (!ae_private_temp_path(cpath, sizeof(cpath), name)) {
        fprintf(stderr, "ae pad: no temporary directory\n");
        return PAD_VERIFY_BROKEN;   /* nothing was compared */
    }
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout --layout-asserts%s", o->layout_flags);
    char* text = NULL;
    if (aetherc_capture_stdout_whole(flags, path, cpath, &text) != 0) {
        fprintf(stderr, "ae pad: %s does not compile as Aether; not verified\n", path);
        free(text);
        remove(cpath);
        return PAD_VERIFY_BROKEN;
    }
    int n = 0;
    for (const char* p = text; (p = strstr(p, "_Static_assert(")) != NULL; p++) n++;
    FILE* f = fopen(cpath, "ab");
    int ok = f && fputs(text, f) >= 0;
    /* Test hook: text appended to the verify C, so the tests can show a
     * failed assertion and C that does not compile told apart (#3806). */
    const char* extra = getenv("AE_PAD_VERIFY_APPEND");
    if (ok && extra && *extra) ok = fputs("\n", f) >= 0 && fputs(extra, f) >= 0 && fputs("\n", f) >= 0;
    if (f && fclose(f) != 0) ok = 0;
    free(text);
    if (!ok) { fprintf(stderr, "ae pad: cannot write %s\n", cpath); remove(cpath); return PAD_VERIFY_BROKEN; }
    char* cmd = (char*)malloc(AE_CMD_BUF);
    if (!cmd) { remove(cpath); return PAD_VERIFY_BROKEN; }
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
    char log[1300];
    snprintf(log, sizeof(log), "%s.log", cpath);
    int rc = run_cmd_capture_all(cmd, log);
    free(cmd);
    remove(cpath);
    if (rc != 0) {
        int kind = pad_verify_explain(log, 0) ? PAD_VERIFY_MISMATCH : PAD_VERIFY_BROKEN;
        if (kind == PAD_VERIFY_MISMATCH)
            fprintf(stderr, "ae pad: %s: the C compiler does not agree with the layout report:\n",
                    path);
        else
            fprintf(stderr, "ae pad: %s: its C did not compile, so nothing was verified; this is "
                            "an ae pad defect, not a layout one (please report it):\n", path);
        pad_verify_explain(log, 1);
        remove(log);
        return kind;
    }
    remove(log);
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
    if (o.json && (o.fix || o.verify)) {
        /* Their lines are text; mixed into the JSON they would break it. */
        fprintf(stderr, "ae pad: --json is the report alone; run --fix and --verify without "
                        "it (--json --min-waste=N is a JSON report with the gate)\n");
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
    /* One entry per file however many paths reach it (`src src/a.ae`):
     * phase one plans each once, and a second visit would apply a plan made
     * before the first one's rewrite. */
    {
        PadList uniq;
        memset(&uniq, 0, sizeof(uniq));
        char** seen = (char**)calloc((size_t)(files.n > 0 ? files.n : 1), sizeof(char*));
        int nseen = 0;
        for (int i = 0; seen && i < files.n; i++) {
            char* c = pad_canon(files.v[i]);
            int dup = 0;
            for (int k = 0; c && k < nseen && !dup; k++) dup = strcmp(seen[k], c) == 0;
            if (dup || !c) {
                free(c);
                continue;
            }
            seen[nseen++] = c;
            pad_list_add(&uniq, files.v[i]);
        }
        if (seen) {
            for (int k = 0; k < nseen; k++) free(seen[k]);
            free(seen);
            for (int i = 0; i < files.n; i++) free(files.v[i]);
            free(files.v);
            files = uniq;
        }
    }

    PadTotals t;
    memset(&t, 0, sizeof(t));
    int report = (!o.fix && !o.verify) || o.min_waste > 0;
    int fixed = 0, asserts = 0, verified = 0, mismatched = 0, broken = 0, nfailed = 0;
    const char* failed_names[8];
    char flags[600];
    snprintf(flags, sizeof(flags), "--emit=layout%s%s", o.json ? " --json" : "", o.layout_flags);
    if (o.fix) {
        /* Two phases: what every file of the run refuses is known before
         * any file is rewritten, so a module's struct that a program of the
         * run hands to C stays as it is. */
        PadRun run;
        memset(&run, 0, sizeof(run));
        pad_plan_run(&run, &files, &o, &bad);
        for (int i = 0; i < run.nplans; i++)
            if (pad_fix_file(&run, i, &o, &fixed) != 0) bad = 1;
        pad_run_free(&run);
    }
    if (report && o.json) printf("{\"files\": [");
    int first = 1;
    for (int i = 0; i < files.n; i++) {
        const char* path = files.v[i];
        if (o.verify) {
            int v = pad_verify_file(path, &o, &asserts);
            if (v == PAD_VERIFY_OK) {
                verified++;
            } else {
                bad = 1;
                if (v == PAD_VERIFY_MISMATCH) mismatched++;
                else broken++;
                if (nfailed < 8) failed_names[nfailed++] = path;
            }
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
    if (o.verify) {
        printf("ae pad --verify: %d file%s verified (%d asserts held)", verified,
               verified == 1 ? "" : "s", asserts);
        if (mismatched + broken > 0) {
            printf("; %d FAILED (%d disagree with the layout report, %d did not compile):",
                   mismatched + broken, mismatched, broken);
            for (int k = 0; k < nfailed; k++) printf(" %s", failed_names[k]);
            if (mismatched + broken > nfailed) printf(" ...");
        }
        printf("\n");
    }
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
