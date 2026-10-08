/* The AetherString header, defined once for the runtime and the compiler.
 *
 * The runtime (aether_string.h) declares AetherString from these fields, and
 * the compiler emits a static AetherString from the same fields and
 * constants for a string literal that holds a NUL (#2520). Both expand this
 * one definition, so the layout the compiler writes cannot drift from the
 * layout the runtime reads.
 */
#ifndef AETHER_STRING_ABI_H
#define AETHER_STRING_ABI_H

/* Distinguishes an AetherString* from a raw char*. */
#define AETHER_STRING_MAGIC 0xAE57C0DE

/* The ref_count of a pinned string: one that lives as long as the program
 * and is never freed. string_retain and string_release leave it alone. */
#define AETHER_STRING_PINNED_REFS 0x7FFFFFFF

/* The header's fields, in layout order. */
#define AETHER_STRING_FIELDS \
    unsigned int magic;      /* AETHER_STRING_MAGIC */ \
    int ref_count;                                     \
    size_t length;                                     \
    size_t capacity;                                   \
    char* data;

#endif
