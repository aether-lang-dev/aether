/* A C header that owns a struct with a `const char*` field and ships no
 * typedef, like most C APIs that take a name or a path in a struct
 * (VkApplicationInfo.pApplicationName, struct option.name, ...). The
 * struct borrows the string: whoever fills it keeps the string alive.
 *
 * Force-included into the Aether-generated TU via aether.toml's
 * `cflags = "-include label.h"`. */
#ifndef C_IMPORT_STRUCT_STRING_FIELD_LABEL_H
#define C_IMPORT_STRUCT_STRING_FIELD_LABEL_H

struct label {
    const char* text;
    int         size;
};

struct tagged {
    int          id;
    struct label label;
};

#endif
