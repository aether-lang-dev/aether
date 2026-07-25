// aether_yaml.c — Bindings for std.yaml using libfyaml.
//
// libfyaml is an OPTIONAL dependency: the Makefile defines AETHER_HAS_YAML
// (and links -lfyaml) only when pkg-config finds libfyaml. When it's absent
// this file must still compile — otherwise std.yaml being in STD_SRC would
// break the whole stdlib build on any machine without libfyaml. So the
// libfyaml-backed bodies live under `#ifdef AETHER_HAS_YAML`; the `#else`
// branch provides stubs that report "unavailable" at runtime, exactly like
// the OpenSSL/zlib/pcre2 stubs elsewhere in std/.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef AETHER_HAS_YAML
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

static YAML_THREAD_LOCAL char g_yaml_err_buf[256];
static YAML_THREAD_LOCAL int  g_yaml_err_set = 0;

const char* yaml_last_error(void) {
    return g_yaml_err_set ? g_yaml_err_buf : "";
}

#ifdef AETHER_HAS_YAML
// ---------------------------------------------------------------------------
// Emission thread-local buffer to avoid memory leaks
// ---------------------------------------------------------------------------

static YAML_THREAD_LOCAL char *g_yaml_emit_buf = NULL;
static YAML_THREAD_LOCAL size_t g_yaml_emit_cap = 0;

static const char *store_emit_string(char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    if (len + 1 > g_yaml_emit_cap) {
        g_yaml_emit_cap = len + 1024;
        g_yaml_emit_buf = realloc(g_yaml_emit_buf, g_yaml_emit_cap);
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

    struct fy_document *doc = fy_document_build_from_string(&cfg, yaml_str, strlen(yaml_str));

    if (!doc && diag) {
        void *prev = NULL;
        struct fy_diag_error *err = fy_diag_errors_iterate(diag, &prev);
        if (err) {
            snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf), "%s at %d:%d",
                     err->msg ? err->msg : "parse error", err->line, err->column);
            g_yaml_err_set = 1;
        }
    }

    // Clear collected errors then drop our ref. The collected-error entries are
    // owned by the diag and are NOT reclaimed by fy_diag_unref alone (leaks ~64
    // bytes per failed parse), so reset them first. Reset is safe on a diag with
    // no collected errors (the success path), so it runs unconditionally.
    if (diag) {
        fy_diag_reset_error(diag);
        fy_diag_unref(diag);
    }

    return doc;
}

void yaml_free_raw(void* doc) {
    if (doc) {
        fy_document_destroy((struct fy_document*)doc);
    }
}

void* yaml_root_raw(void* doc) {
    if (!doc) return NULL;
    return fy_document_root((struct fy_document*)doc);
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

void* yaml_mapping_lookup_raw(void* node, const char* key) {
    if (!node || !key) return NULL;
    if (fy_node_get_type((struct fy_node*)node) != FYNT_MAPPING) return NULL;
    return fy_node_mapping_lookup_by_string((struct fy_node*)node, key, strlen(key));
}

const char* yaml_emit_node_raw(void* node) {
    if (!node) return NULL;
    char* str = fy_emit_node_to_string((struct fy_node*)node, FYECF_DEFAULT);
    return store_emit_string(str);
}

const char* yaml_emit_document_raw(void* doc) {
    if (!doc) return NULL;
    char* str = fy_emit_document_to_string((struct fy_document*)doc, FYECF_DEFAULT);
    return store_emit_string(str);
}

#else  /* !AETHER_HAS_YAML — libfyaml not available at build time */

/* Stubs so std.yaml links everywhere; every entry point reports the feature
 * as unavailable. yaml_parse_raw sets the error so callers' (doc, err) tuple
 * surfaces a clear message rather than a silent NULL. */
static void yaml_set_unavailable(void) {
    snprintf(g_yaml_err_buf, sizeof(g_yaml_err_buf),
             "std.yaml unavailable: this build has no libfyaml "
             "(install libfyaml + rebuild, or set YAML=1)");
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
