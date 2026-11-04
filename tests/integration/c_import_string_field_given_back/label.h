/* A C header that owns a struct with a `const char*` field and ships no
 * typedef, as c_import_struct_string_field's does. C keeps the pointer it
 * is given; whoever filled the field says when C is done with it.
 *
 * Force-included into the Aether-generated TU via aether.toml's
 * `cflags = "-include label.h"`. */
#ifndef C_IMPORT_STRING_FIELD_GIVEN_BACK_LABEL_H
#define C_IMPORT_STRING_FIELD_GIVEN_BACK_LABEL_H

#include <string.h>

struct label {
    const char* text;
    int         size;
};

struct tagged {
    int          id;
    struct label label;
};

/* What C sees in the field: its length by strlen, -1 for NULL. */
static inline int label_c_length(const struct label* l) {
    return l->text ? (int)strlen(l->text) : -1;
}

#endif
