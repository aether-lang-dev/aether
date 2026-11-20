/* Coverage-guided fuzz target for std.yaml (std/yaml/aether_yaml.c and the
 * vendored libfyaml it wraps, std/yaml/aether_libfyaml_vendored.c, both
 * compiled into the harness with the fuzzer's coverage and the sanitizers).
 *
 * The input is a document. yaml_parse_raw takes a C string, as std.yaml's
 * parse passes an Aether string, so it reads up to the first NUL byte. Each
 * check is a property, and a failure abort()s with what disagreed:
 *
 *   Y1  A refusal is NULL with a message in yaml_last_error, and anything
 *       else is a document with the message cleared: a stream with no
 *       document in it (empty, or only comments) is a document with no root.
 *   Y2  The tree a program walks is consistent: every node is a scalar, a
 *       sequence or a mapping; a scalar has a value and the collection
 *       accessors refuse it (-1, NULL); a sequence of n items hands out n
 *       nodes and nothing at n; a mapping of n pairs hands out n keys and n
 *       values, and nothing at n; yaml_mapping_lookup_raw, given a scalar
 *       key's text, finds the value of the first pair with that key.
 *   Y3  An accepted document emits (yaml_emit_document_raw), the text parses
 *       again, and it is the same tree: the same kinds, sizes, order and
 *       scalar values. The root node emits on its own (yaml_emit_node_raw).
 *   Y4  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libfyaml itself, for a scalar's length (a key may hold NUL bytes, which
 * the C-string accessor cannot show). The vendored copy's symbols are
 * renamed aether_<name>, as aether_yaml.c includes it. */
#ifdef AETHER_VENDOR_YAML
#include "aether_libfyaml_names.h"
#endif
#include <libfyaml.h>

extern void* yaml_parse_raw(const char* yaml_str);
extern const char* yaml_last_error(void);
extern void yaml_free_raw(void* doc);
extern void* yaml_root_raw(void* doc);
extern int yaml_node_type_raw(void* node);
extern const char* yaml_node_get_scalar_raw(void* node);
extern int yaml_sequence_size_raw(void* node);
extern void* yaml_sequence_get_raw(void* node, int index);
extern int yaml_mapping_size_raw(void* node);
extern void* yaml_mapping_get_key_raw(void* node, int index);
extern void* yaml_mapping_get_value_raw(void* node, int index);
extern void* yaml_mapping_lookup_raw(void* node, const char* key);
extern const char* yaml_emit_node_raw(void* node);
extern const char* yaml_emit_document_raw(void* doc);

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_yaml: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

enum { SCALAR = 1, SEQUENCE = 2, MAPPING = 3 };
/* The walks recurse once a level; libfyaml nests far deeper than a stack
 * frame per level allows, so a tree is compared to this depth. */
#define MAX_WALK_DEPTH 256

/* Y2 */
static void walk(void* node, int depth) {
    CHECK(node != NULL, "a NULL node inside the tree");
    int t = yaml_node_type_raw(node);
    CHECK(t == SCALAR || t == SEQUENCE || t == MAPPING, "node kind %d", t);
    if (depth > MAX_WALK_DEPTH) return;
    if (t == SCALAR) {
        CHECK(yaml_node_get_scalar_raw(node) != NULL, "a scalar with no value");
        CHECK(yaml_sequence_size_raw(node) == -1 && yaml_mapping_size_raw(node) == -1,
              "a scalar answered as a collection");
        CHECK(yaml_sequence_get_raw(node, 0) == NULL && yaml_mapping_get_key_raw(node, 0) == NULL,
              "a scalar handed out an item");
        return;
    }
    if (t == SEQUENCE) {
        int n = yaml_sequence_size_raw(node);
        CHECK(n >= 0, "a sequence of %d items", n);
        CHECK(yaml_mapping_size_raw(node) == -1, "a sequence answered as a mapping");
        for (int i = 0; i < n; i++) walk(yaml_sequence_get_raw(node, i), depth + 1);
        CHECK(yaml_sequence_get_raw(node, n) == NULL, "a sequence of %d handed out item %d", n, n);
        CHECK(yaml_sequence_get_raw(node, -n - 1) == NULL, "a sequence of %d handed out item %d", n, -n - 1);
        return;
    }
    int n = yaml_mapping_size_raw(node);
    CHECK(n >= 0, "a mapping of %d pairs", n);
    CHECK(yaml_sequence_size_raw(node) == -1, "a mapping answered as a sequence");
    for (int i = 0; i < n; i++) {
        void* k = yaml_mapping_get_key_raw(node, i);
        void* v = yaml_mapping_get_value_raw(node, i);
        /* A null key or value (`? ` with nothing) is a NULL node. */
        if (k) walk(k, depth + 1);
        if (v) walk(v, depth + 1);
        /* A scalar key (one with no NUL, which a C string cannot name) finds
         * the value of the first pair with that key. */
        size_t klen = 0;
        const char* ks = k && yaml_node_type_raw(k) == SCALAR ? fy_node_get_scalar((struct fy_node*)k, &klen) : NULL;
        if (ks && memchr(ks, 0, klen) == NULL) {
            char* key = (char*)malloc(klen + 1);
            CHECK(key != NULL, "out of memory");
            memcpy(key, ks, klen);
            key[klen] = '\0';
            void* want = v;
            for (int j = 0; j < i; j++) {
                void* kj = yaml_mapping_get_key_raw(node, j);
                size_t lj = 0;
                const char* sj = kj && yaml_node_type_raw(kj) == SCALAR
                               ? fy_node_get_scalar((struct fy_node*)kj, &lj) : NULL;
                if (sj && lj == klen && memcmp(sj, key, klen) == 0) {
                    want = yaml_mapping_get_value_raw(node, j);
                    break;
                }
            }
            CHECK(yaml_mapping_lookup_raw(node, key) == want,
                  "lookup of the key '%s' does not find the value of the first pair with it", key);
            free(key);
        }
    }
    CHECK(yaml_mapping_get_key_raw(node, n) == NULL && yaml_mapping_get_value_raw(node, n) == NULL,
          "a mapping of %d handed out pair %d", n, n);
}

/* Y3: the same tree, node for node. */
static void same_tree(void* a, void* b, int depth, const char* what) {
    CHECK((a == NULL) == (b == NULL), "%s: a node %s", what, a ? "went missing" : "appeared");
    if (!a || depth > MAX_WALK_DEPTH) return;
    int t = yaml_node_type_raw(a);
    CHECK(t == yaml_node_type_raw(b), "%s: a node of kind %d came back as kind %d", what, t,
          yaml_node_type_raw(b));
    if (t == SCALAR) {
        const char* x = yaml_node_get_scalar_raw(a);
        const char* y = yaml_node_get_scalar_raw(b);
        CHECK(strcmp(x, y) == 0, "%s: the scalar '%s' came back as '%s'", what, x, y);
    } else if (t == SEQUENCE) {
        int n = yaml_sequence_size_raw(a);
        CHECK(n == yaml_sequence_size_raw(b), "%s: a sequence of %d came back with %d", what, n,
              yaml_sequence_size_raw(b));
        for (int i = 0; i < n; i++)
            same_tree(yaml_sequence_get_raw(a, i), yaml_sequence_get_raw(b, i), depth + 1, what);
    } else {
        int n = yaml_mapping_size_raw(a);
        CHECK(n == yaml_mapping_size_raw(b), "%s: a mapping of %d came back with %d", what, n,
              yaml_mapping_size_raw(b));
        for (int i = 0; i < n; i++) {
            same_tree(yaml_mapping_get_key_raw(a, i), yaml_mapping_get_key_raw(b, i), depth + 1, what);
            same_tree(yaml_mapping_get_value_raw(a, i), yaml_mapping_get_value_raw(b, i), depth + 1, what);
        }
    }
}

static char* dup_s(const char* s) {
    size_t n = strlen(s);
    char* d = (char*)malloc(n + 1);
    CHECK(d != NULL, "out of memory");
    memcpy(d, s, n + 1);
    return d;
}

/* Y3: emit the document, parse the text again and compare. */
static void round_trip(void* doc) {
    const char* what = "emit_document";
    const char* text = yaml_emit_document_raw(doc);
    /* A document libfyaml cannot write so that it reads back is refused,
     * with the reason, never handed back as text that does not parse. */
    if (text == NULL) {
        const char* why = yaml_last_error();
        CHECK(why && strstr(why, "does not parse back"),
              "%s: an accepted document did not emit, and not for a text that would not read back (%s)",
              what, why ? why : "(no message)");
        return;
    }
    /* The emit buffer is reused by the next emit: keep a copy. */
    char* copy = dup_s(text);
    void* again = yaml_parse_raw(copy);
    CHECK(again != NULL, "%s: the emitted text does not parse (%s):\n%s", what, yaml_last_error(), copy);
    same_tree(yaml_root_raw(doc), yaml_root_raw(again), 0, what);
    yaml_free_raw(again);
    free(copy);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* text = (char*)malloc(size + 1);
    CHECK(text != NULL, "out of memory");
    memcpy(text, data, size);
    text[size] = '\0';

    void* doc = yaml_parse_raw(text);
    const char* err = yaml_last_error();
    if (!doc) {
        CHECK(err && *err, "a refusal with no message");
        free(text);
        return 0;
    }
    CHECK(err && !*err, "an accepted document left the error '%s'", err);

    void* root = yaml_root_raw(doc);
    if (root) walk(root, 0);
    round_trip(doc);
    /* A node emits on its own too. Its text is not always a document: a tag
     * it carries may use a handle the document's %TAG directive declared,
     * which only emit_document writes. */
    if (root) CHECK(yaml_emit_node_raw(root) != NULL, "the root node did not emit");

    yaml_free_raw(doc);
    free(text);
    return 0;
}
