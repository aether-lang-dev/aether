// aether_yaml.c — Bindings for std.yaml using libfyaml.
//
// libfyaml comes one of three ways, chosen by the Makefile's YAML setting:
//   - vendored (the default): the in-tree copy in std/yaml/libfyaml/,
//     compiled into libaether.a by aether_libfyaml_vendored.c. The build
//     defines AETHER_HAS_YAML and AETHER_VENDOR_YAML and passes
//     -I<std>/yaml/libfyaml/include; no program links -lfyaml.
//   - a system libfyaml (YAML=1, or YAML=auto when pkg-config finds one):
//     AETHER_HAS_YAML alone, and -lfyaml for programs that import std.yaml.
//   - none (YAML=0): this file must still compile, since it is in STD_SRC.
//     The libfyaml-backed bodies live under `#ifdef AETHER_HAS_YAML`; the
//     `#else` branch provides stubs that report "unavailable" at runtime,
//     like the OpenSSL/zlib stubs elsewhere in std/.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(AETHER_NO_THREADING)
#include "../../runtime/actors/aether_panic.h"   /* aether_context_local */
#endif
#ifdef AETHER_HAS_YAML
#ifdef AETHER_VENDOR_YAML
/* The vendored library's symbols are all renamed aether_<name> (see
 * aether_libfyaml_vendored.c); this header makes the fy_* calls below use
 * those names. It has to come before <libfyaml.h>. */
#include "aether_libfyaml_names.h"
#endif
#include <libfyaml.h>
#endif

// ---------------------------------------------------------------------------
// Thread-local storage portability shim
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
    #define YAML_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define YAML_THREAD_LOCAL _Thread_local
#else
    #define YAML_THREAD_LOCAL __thread
#endif

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

#if defined(AETHER_NO_THREADING)
/* A cooperative build has no thread-locals of Aether's own (ESP-IDF would put
 * one on every FreeRTOS task's stack): the error and the emit buffer are a
 * block of the calling task's execution context (aether_context_local, docs/
 * design/task-execution-contexts.md), the buffer given back with it. No
 * memory for the block is a panic, never another task's buffer. */
typedef struct { char err_buf[256]; int err_set; char* emit_buf; size_t emit_cap; } YamlState;
static const char g_yaml_key = 0;
static void yaml_state_release(void* p) { free(((YamlState*)p)->emit_buf); }
static YamlState* yaml_state_here(void) {
    YamlState* y = (YamlState*)aether_context_local(&g_yaml_key, sizeof(YamlState), yaml_state_release);
    if (!y) aether_panic("yaml: no memory for this task's error and emit buffer");
    return y;
}
#define g_yaml_err_buf (yaml_state_here()->err_buf)
#define g_yaml_err_set (yaml_state_here()->err_set)
#else
static YAML_THREAD_LOCAL char g_yaml_err_buf[256];
static YAML_THREAD_LOCAL int  g_yaml_err_set = 0;
#endif

const char* yaml_last_error(void) {
    return g_yaml_err_set ? g_yaml_err_buf : "";
}

#ifdef AETHER_HAS_YAML
// ---------------------------------------------------------------------------
// Emission thread-local buffer to avoid memory leaks
// ---------------------------------------------------------------------------

#if defined(AETHER_NO_THREADING)
#define g_yaml_emit_buf (yaml_state_here()->emit_buf)
#define g_yaml_emit_cap (yaml_state_here()->emit_cap)
#else
static YAML_THREAD_LOCAL char *g_yaml_emit_buf = NULL;
static YAML_THREAD_LOCAL size_t g_yaml_emit_cap = 0;
#endif

static const char *store_emit_string(char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    if (len + 1 > g_yaml_emit_cap) {
        /* Through a temporary: assigning realloc's result straight back would
         * lose the old buffer when it fails, and would leave the capacity
         * claiming a size the now-NULL buffer does not have, so every later
         * call short of that size would skip the grow and return NULL. */
        char *grown = realloc(g_yaml_emit_buf, len + 1024);
        if (!grown) {
            free(str);
            return NULL;
        }
        g_yaml_emit_buf = grown;
        g_yaml_emit_cap = len + 1024;
    }
    if (g_yaml_emit_buf) {
        memcpy(g_yaml_emit_buf, str, len + 1);
    }
    free(str); // free the libfyaml malloc'd string
    return g_yaml_emit_buf;
}

// ---------------------------------------------------------------------------
// Public API Implementations
// ---------------------------------------------------------------------------

/* A parsed document, and the text it was parsed from, which it owns. libfyaml
 * does not copy a string it parses: the document's scalars point into it,
 * and its emitter, keeping each scalar's original style, writes a block
 * scalar from the source lines. So the text lives as long as the document,
 * not as long as the caller's string, which an Aether caller may release
 * while the document is still in use.
 *
 * The copy ends in a line break. The emitter takes a block scalar's last
 * line for scalar content only when a line break follows it, so a document
 * whose last line was one (`k: >\n  abc`, at the end of the input) emitted
 * as `k: >` with the content gone, and read back as an empty scalar (#3749,
 * found by the YAML fuzz target). A final line break does not change what a
 * document says. The vendored libfyaml takes no local patches
 * (libfyaml/VENDOR.md), so the text is made the shape it handles. */
typedef struct {
    struct fy_document* fyd;
    char* src;
} AetherYamlDoc;

static struct fy_document* yaml_fyd(void* doc) {
    return doc ? ((AetherYamlDoc*)doc)->fyd : NULL;
}

void* yaml_parse_raw(const char* yaml_str) {
    g_yaml_err_set = 0;
    g_yaml_err_buf[0] = '\0';

    if (!yaml_str) {
        snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "null input");
        g_yaml_err_set = 1;
        return NULL;
    }

    struct fy_parse_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    // Allow duplicate keys on mappings to align with standard lenient parsing tests
    cfg.flags = FYPCF_ALLOW_DUPLICATE_KEYS;

    struct fy_diag_cfg dcfg;
    fy_diag_cfg_default(&dcfg);
    dcfg.level = FYET_MAX; // Suppress direct print-to-stderr of error/warning diagnostics

    struct fy_diag *diag = fy_diag_create(&dcfg);
    if (diag) {
        fy_diag_set_collect_errors(diag, true);
        cfg.diag = diag;
    }

    size_t n = strlen(yaml_str);
    int add_lb = n > 0 && yaml_str[n - 1] != '\n' && yaml_str[n - 1] != '\r';
    char* src = (char*)malloc(n + (size_t)add_lb + 1);
    if (!src) {
        if (diag) fy_diag_unref(diag);
        snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "out of memory");
        g_yaml_err_set = 1;
        return NULL;
    }
    memcpy(src, yaml_str, n);
    if (add_lb) src[n] = '\n';
    src[n + (size_t)add_lb] = '\0';

    struct fy_document *doc = fy_document_build_from_string(&cfg, src, n + (size_t)add_lb);

    if (!doc && diag) {
        void *prev = NULL;
        struct fy_diag_error *err = fy_diag_errors_iterate(diag, &prev);
        if (err) {
            snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "%s at %d:%d",
                     err->msg ? err->msg : "parse error", err->line, err->column);
            g_yaml_err_set = 1;
        } else {
            /* No error, and no document: a stream with none in it (empty,
             * or only comments and white space), which is valid YAML. It
             * came back as NULL with no message, so `parse` answered a
             * document of null with no error, a pair its callers read as
             * neither (#3749, found by the fuzzer). It is an empty
             * document: one with no root node. */
            doc = fy_document_create(&cfg);
        }
    }
    if (!doc && !g_yaml_err_set) {
        snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "parse error");
        g_yaml_err_set = 1;
    }

    // Clear collected errors then drop our ref. The collected-error entries are
    // owned by the diag and are NOT reclaimed by fy_diag_unref alone (leaks ~64
    // bytes per failed parse), so reset them first. Reset is safe on a diag with
    // no collected errors (the success path), so it runs unconditionally.
    if (diag) {
        fy_diag_reset_error(diag);
        fy_diag_unref(diag);
    }

    if (!doc) {
        free(src);
        return NULL;
    }
    AetherYamlDoc* d = (AetherYamlDoc*)malloc(sizeof(*d));
    if (!d) {
        fy_document_destroy(doc);
        free(src);
        snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "out of memory");
        g_yaml_err_set = 1;
        return NULL;
    }
    d->fyd = doc;
    d->src = src;
    return d;
}

void yaml_free_raw(void* doc) {
    if (doc) {
        AetherYamlDoc* d = (AetherYamlDoc*)doc;
        fy_document_destroy(d->fyd);
        free(d->src);
        free(d);
    }
}

void* yaml_root_raw(void* doc) {
    if (!doc) return NULL;
    return fy_document_root(yaml_fyd(doc));
}

int yaml_node_type_raw(void* node) {
    if (!node) return 0;
    enum fy_node_type t = fy_node_get_type((struct fy_node*)node);
    // FYNT_SCALAR = 0 -> 1, FYNT_SEQUENCE = 1 -> 2, FYNT_MAPPING = 2 -> 3
    return (int)t + 1;
}

const char* yaml_node_get_scalar_raw(void* node) {
    if (!node) return NULL;
    return fy_node_get_scalar0((struct fy_node*)node);
}

int yaml_sequence_size_raw(void* node) {
    if (!node) return -1;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_SEQUENCE) return -1;
    return fy_node_sequence_item_count((struct fy_node*)node);
}

void* yaml_sequence_get_raw(void* node, int index) {
    if (!node) return NULL;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_SEQUENCE) return NULL;
    return fy_node_sequence_get_by_index((struct fy_node*)node, index);
}

int yaml_mapping_size_raw(void* node) {
    if (!node) return -1;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_MAPPING) return -1;
    return fy_node_mapping_item_count((struct fy_node*)node);
}

void* yaml_mapping_get_key_raw(void* node, int index) {
    if (!node) return NULL;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_MAPPING) return NULL;
    struct fy_node_pair* pair = fy_node_mapping_get_by_index((struct fy_node*)node, index);
    if (!pair) return NULL;
    return fy_node_pair_key(pair);
}

void* yaml_mapping_get_value_raw(void* node, int index) {
    if (!node) return NULL;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_MAPPING) return NULL;
    struct fy_node_pair* pair = fy_node_mapping_get_by_index((struct fy_node*)node, index);
    if (!pair) return NULL;
    return fy_node_pair_value(pair);
}

/* The value of the first pair whose key is the scalar `key`, compared as the
 * text it is. libfyaml's fy_node_mapping_lookup_by_string reads its argument
 * as YAML source, so a key that YAML would read as something else ("a: b",
 * "[x]", "#c", a key ending in a line break, one with quotes) was not found
 * in a mapping that holds it (#3749, found by the fuzzer). A key given as
 * YAML source that names a non-scalar key ("[a, b]") is still looked up that
 * way when no scalar key matches. */
void* yaml_mapping_lookup_raw(void* node, const char* key) {
    if (!node || !key) return NULL;
    struct fy_node* map = (struct fy_node*)node;
    if (fy_node_get_type(map) != FYNT_MAPPING) return NULL;
    size_t klen = strlen(key);
    void* iter = NULL;
    struct fy_node_pair* pair;
    while ((pair = fy_node_mapping_iterate(map, &iter)) != NULL) {
        struct fy_node* k = fy_node_pair_key(pair);
        if (!k || fy_node_get_type(k) != FYNT_SCALAR) continue;
        size_t n = 0;
        const char* s = fy_node_get_scalar(k, &n);
        if (s && n == klen && memcmp(s, key, n) == 0) return fy_node_pair_value(pair);
    }
    return fy_node_mapping_lookup_by_string(map, key, klen);
}

const char* yaml_emit_node_raw(void* node) {
    if (!node) return NULL;
    char* str = fy_emit_node_to_string((struct fy_node*)node, FYECF_DEFAULT);
    return store_emit_string(str);
}

/* Whether two nodes are the same tree, as a reader of std.yaml sees it: the
 * same kind, a scalar's bytes (its length too: a key may hold NUL), a
 * collection's size and each item in order, a mapping's keys and values.
 * Two absent nodes (an empty value) are the same. Iterative, with its own
 * stack, so a deeply nested document cannot run the C stack out. */
static int yaml_same_tree(struct fy_node* a, struct fy_node* b) {
    size_t cap = 64, n = 0;
    struct fy_node** st = (struct fy_node**)malloc(cap * 2 * sizeof(*st));
    if (!st) return 0;
    st[n * 2] = a; st[n * 2 + 1] = b; n++;
    int same = 1;
    while (same && n > 0) {
        n--;
        struct fy_node* x = st[n * 2];
        struct fy_node* y = st[n * 2 + 1];
        if (!x || !y) { same = x == y; continue; }
        enum fy_node_type t = fy_node_get_type(x);
        if (t != fy_node_get_type(y)) { same = 0; break; }
        int items = 0;
        if (t == FYNT_SCALAR) {
            size_t xl = 0, yl = 0;
            const char* xs = fy_node_get_scalar(x, &xl);
            const char* ys = fy_node_get_scalar(y, &yl);
            same = xl == yl && (xl == 0 || (xs && ys && memcmp(xs, ys, xl) == 0));
            continue;
        } else if (t == FYNT_SEQUENCE) {
            items = fy_node_sequence_item_count(x);
            if (items != fy_node_sequence_item_count(y)) { same = 0; break; }
        } else {
            items = fy_node_mapping_item_count(x);
            if (items != fy_node_mapping_item_count(y)) { same = 0; break; }
        }
        size_t need = n + (size_t)items * 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            struct fy_node** grown = (struct fy_node**)realloc(st, cap * 2 * sizeof(*st));
            if (!grown) { same = 0; break; }
            st = grown;
        }
        for (int i = 0; i < items; i++) {
            if (t == FYNT_SEQUENCE) {
                st[n * 2] = fy_node_sequence_get_by_index(x, i);
                st[n * 2 + 1] = fy_node_sequence_get_by_index(y, i);
                n++;
            } else {
                struct fy_node_pair* px = fy_node_mapping_get_by_index(x, i);
                struct fy_node_pair* py = fy_node_mapping_get_by_index(y, i);
                st[n * 2] = px ? fy_node_pair_key(px) : NULL;
                st[n * 2 + 1] = py ? fy_node_pair_key(py) : NULL;
                n++;
                st[n * 2] = px ? fy_node_pair_value(px) : NULL;
                st[n * 2 + 1] = py ? fy_node_pair_value(py) : NULL;
                n++;
            }
        }
    }
    free(st);
    return same;
}

/* Whether `text` reads back as `root`: it parses as yaml_parse_raw parses it
 * (the same flags, no diagnostics printed; a stream with no document in it is
 * the empty document it is) to the same tree. When it does not, why goes to
 * `why`. */
static int yaml_text_reads_back(const char* text, struct fy_node* root, char* why, size_t why_cap) {
    if (why_cap) why[0] = '\0';
    struct fy_parse_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYPCF_ALLOW_DUPLICATE_KEYS;
    struct fy_diag_cfg dcfg;
    fy_diag_cfg_default(&dcfg);
    dcfg.level = FYET_MAX;
    struct fy_diag* diag = fy_diag_create(&dcfg);
    if (diag) {
        fy_diag_set_collect_errors(diag, true);
        cfg.diag = diag;
    }
    struct fy_document* d = fy_document_build_from_string(&cfg, text, strlen(text));
    int ok = d != NULL;
    if (d) {
        if (!yaml_same_tree(root, fy_document_root(d))) {
            ok = 0;
            if (why_cap) snprintf(why, why_cap, "it reads back as another document");
        }
        fy_document_destroy(d);
    } else if (diag) {
        void* prev = NULL;
        struct fy_diag_error* err = fy_diag_errors_iterate(diag, &prev);
        /* No document and no error is an empty stream: the same only as a
         * document with no root. */
        ok = err == NULL && root == NULL;
        if (err && why_cap)
            snprintf(why, why_cap, "%s at %d:%d", err->msg ? err->msg : "parse error", err->line, err->column);
        else if (!ok && why_cap)
            snprintf(why, why_cap, "it reads back as an empty document");
    }
    if (diag) {
        fy_diag_reset_error(diag);   /* collected errors are not freed by unref */
        fy_diag_unref(diag);
    }
    return ok;
}

const char* yaml_emit_document_raw(void* doc) {
    g_yaml_err_set = 0;   /* a refusal below says why; an earlier parse's error is not it */
    g_yaml_err_buf[0] = '\0';
    if (!doc) return NULL;
    /* A document with no root (what an empty stream parses to) is the empty
     * text, which parses back to it; libfyaml emits nothing for it. */
    if (!fy_document_root(yaml_fyd(doc))) {
        char* empty = malloc(1);
        if (!empty) return NULL;
        empty[0] = '\0';
        return store_emit_string(empty);
    }
    /* A root with an anchor or a tag is written after a "---": libfyaml reads
     * a bare document's properties on the line of a block scalar indicator
     * (`&a |-` then the content) as an empty scalar, and emitted it in just
     * that shape, so `&a\n|-\nn?$` came back as '' (#3749, the yaml fuzz
     * target). After "---" it reads the scalar whole. Other documents are
     * emitted as they were. */
    struct fy_node* root = fy_document_root(yaml_fyd(doc));
    enum fy_emitter_cfg_flags flags = FYECF_DEFAULT;
    if (fy_node_get_anchor(root) || fy_node_get_tag(root, NULL)) {
        flags = (enum fy_emitter_cfg_flags)((flags & ~FYECF_DOC_START_MARK(FYECF_DOC_START_MARK_MASK)) |
                                            FYECF_DOC_START_MARK_ON);
    }
    char* str = fy_emit_document_to_string(yaml_fyd(doc), flags);
    /* What this returns reads back as this document, or it is not returned.
     * libfyaml's emitter writes some documents in a shape its own parser
     * refuses or reads as something else: an empty entry of a sequence under
     * a key comes out as `b:` then `- ` at the key's column ("did not find
     * expected node content", and flow style drops the entry), and a plain
     * scalar `?` in a flow sequence comes out unquoted, where it starts a
     * mapping (#3749, the yaml fuzz target, both). The vendored copy takes
     * no patches, so such a document is refused with the reason, rather than
     * handed back as text that does not mean it. */
    char why[200];
    if (str && !yaml_text_reads_back(str, root, why, sizeof why)) {
        free(str);
        snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf),
                 "the emitter's text for this document does not parse back (%s), a libfyaml limitation",
                 why);
        g_yaml_err_set = 1;
        return NULL;
    }
    return store_emit_string(str);
}

#else  /* !AETHER_HAS_YAML — libfyaml not available at build time */

/* Stubs so std.yaml links everywhere; every entry point reports the feature
 * as unavailable. yaml_parse_raw sets the error so callers' (doc, err) tuple
 * surfaces a clear message rather than a silent NULL. */
static void yaml_set_unavailable(void) {
    snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf),
             "std.yaml unavailable: this build has no libfyaml "
             "(it was built with YAML=0; rebuild Aether with the default YAML=vendored)");
    g_yaml_err_set = 1;
}

void* yaml_parse_raw(const char* yaml_str) { (void)yaml_str; yaml_set_unavailable(); return NULL; }
void  yaml_free_raw(void* doc) { (void)doc; }
void* yaml_root_raw(void* doc) { (void)doc; return NULL; }
int   yaml_node_type_raw(void* node) { (void)node; return 0; }
const char* yaml_node_get_scalar_raw(void* node) { (void)node; return NULL; }
int   yaml_sequence_size_raw(void* node) { (void)node; return -1; }
void* yaml_sequence_get_raw(void* node, int index) { (void)node; (void)index; return NULL; }
int   yaml_mapping_size_raw(void* node) { (void)node; return -1; }
void* yaml_mapping_get_key_raw(void* node, int index) { (void)node; (void)index; return NULL; }
void* yaml_mapping_get_value_raw(void* node, int index) { (void)node; (void)index; return NULL; }
void* yaml_mapping_lookup_raw(void* node, const char* key) { (void)node; (void)key; return NULL; }
const char* yaml_emit_node_raw(void* node) { (void)node; return NULL; }
const char* yaml_emit_document_raw(void* doc) { (void)doc; return NULL; }

#endif /* AETHER_HAS_YAML */
