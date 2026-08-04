/* Header that owns the prototypes the Aether probe imports via
 * `extern ... @c_import` (#1239). The spellings here are deliberately the
 * ones an Aether extern cannot reproduce: uint8_t and size_t parameters,
 * and a typed struct pointer. Aether's own prototype would be
 * ABI-compatible but differently spelled, which is what produced the LTO
 * type-mismatch warnings this test exists to prevent. */
#ifndef AETHER_TEST_API_H
#define AETHER_TEST_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct blob { size_t len; uint8_t first; } blob;

uint8_t  api_scale(uint8_t v);
size_t   api_span(size_t a, size_t b);
blob*    api_blob(void);
size_t   api_blob_len(const blob* b);

/* Variadic, returns a malloc'd buffer the caller owns. Declared in Aether as
 * `... -> string @heap @c_import`: three markers stacked on one extern, which
 * is what regression-tests that a trailing attribute no longer discards the
 * variadic marker recorded earlier during parameter parsing. */
char* api_fmt(const char* fmt, ...);

/* static inline in the same header (#1241): no linkable symbol at all, so a
 * non-static prototype for it would be meaningless. @c_import emits none. */
static inline uint8_t api_inline_twice(uint8_t v) { return (uint8_t)(v * 2); }

#endif
