/* A C header that defines a union, with no typedef, the way Vulkan's
 * VkClearValue is one (that one does carry a typedef). The Aether side
 * declares it `extern union onion @c_import`, and the generated C must
 * spell it `union onion` everywhere it needs a tag: `struct onion` is a tag
 * mismatch C compilers reject (#2561).
 *
 * Force-included into the Aether-generated TU via aether.toml's
 * `cflags = "-include onion.h"`. */
#ifndef C_IMPORT_UNION_ONION_H
#define C_IMPORT_UNION_ONION_H

#include <stdint.h>
#include <string.h>

union onion {
    float    f[4];
    int32_t  i[4];
    uint32_t u[4];
};

/* A header struct with a string field, for a module-level `var` set to a
 * struct literal (#2590): its field is a plain C string, not an
 * AetherString. */
struct tag_s {
    const char* text;
    int n;
};

static inline int tag_len(struct tag_s* t) { return (int)strlen(t->text) + t->n; }

/* Reads the union through the header's own spelling. */
static inline float onion_first(union onion* o) { return o->f[0]; }

#endif
