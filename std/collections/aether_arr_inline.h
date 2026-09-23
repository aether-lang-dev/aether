/* aether_arr_inline.h — the packed-array layouts and their unchecked
 * accessors, as `static inline` (#1986).
 *
 * Split out of aether_collections.h so an Aether program can pull in JUST
 * this through `@c_include("aether_arr_inline.h")` on std.intarr /
 * std.floatarr / std.longarr: the full collections header declares the
 * string and list surfaces too, whose prototypes the generated C already
 * writes for itself, and two spellings of the same function in one
 * translation unit is a `conflicting types` error.
 *
 * Why inline at all: declared out of line, `floatarr_get_unchecked(a, i)`
 * is a call the C compiler cannot see through, so a hot loop over a packed
 * buffer neither folds the load nor vectorises. Measured on a 256x256
 * double matmul, -O2, Windows/UCRT64 i7-1265U: 19.6 ms through the
 * out-of-line accessors against 2.9 ms for the same loop over a raw
 * buffer — 6.8x. With the definitions visible the two are the same code.
 *
 * The struct fields are the three modules' business. Nothing outside
 * std/collections may read or write them directly; the accessors are the
 * interface, and they are free.
 */
#ifndef AETHER_ARR_INLINE_H
#define AETHER_ARR_INLINE_H

#include <stddef.h>   /* NULL, for the *_data view accessors below */
#include <stdint.h>   /* int64_t: what Aether's `long` lowers to */

/* Aether-prefixed, and that prefix is the point (#2162).
 *
 * This header is not the runtime's own business: `@c_include` puts it in
 * the TRANSLATION UNIT OF EVERY PROGRAM that imports std.intarr /
 * std.floatarr / std.longarr. A bare `struct IntArray` there claims a
 * common name inside someone else's program, and a module that declared
 * its own `IntArray` stopped compiling with an error naming neither of
 * them -- which is what happened to aephysics on 0.708.0, where the only
 * change was that these became visible in the caller's TU at all.
 *
 * So a header the runtime injects declares nothing that is not prefixed.
 * `aether_collections.h` keeps the short aliases for C code that includes
 * it deliberately; that header is not injected into anyone. */
struct AetherIntArray   { int*       data; int size; };
struct AetherFloatArray { double*    data; int size; };
struct AetherLongArray  { int64_t*   data; int size; };

typedef struct AetherIntArray   AetherIntArray;
typedef struct AetherFloatArray AetherFloatArray;
typedef struct AetherLongArray  AetherLongArray;

/* Hot-path skip-the-bounds-check accessors. The caller keeps the index in
 * [0, size); out of range is undefined behaviour, exactly as it is for the
 * C array the buffer is. The checked forms (`*_get_raw` / `*_set_raw`, and
 * the Aether-side `get` / `set` wrappers) stay out of line in the .c.
 *
 * ONE BODY, TWO LINKAGES (#2169).
 *
 * A translation unit that includes this header gets each accessor as
 * `static inline`, which is the whole point of #1986: the C compiler sees
 * the load and can fold and vectorise it.
 *
 * But these names were an exported part of libaether's ABI long before they
 * were inline, and C outside the standard library declares and calls them
 * without this header -- aether-ui's GTK4 and Win32 backends do
 * `extern double floatarr_get_unchecked(void* arr, int i);`. Making them
 * inline-only removed the symbols, and every program linking the toolkit
 * stopped linking on 0.708.0 with `undefined reference to
 * floatarr_get_unchecked`. An ABI break in a minor release.
 *
 * So each module's own .c defines AETHER_ARR_EMIT_<FAMILY> before including
 * this, and in that one translation unit its family's bodies below become
 * ordinary external definitions under the same names. A `static inline` in
 * one TU and an external definition in another is plain C -- internal and
 * external linkage never meet.
 *
 * Each body is written once, here. Two copies -- an inline one and an
 * exported twin in the .c -- would drift: a guard added to one would not
 * reach the other, and C consumers would quietly behave differently from
 * Aether callers. tests/integration/packed_array_c_abi links a C program
 * that only declares these, checks every one is exported, and checks that
 * including the header still inlines. */
#ifdef AETHER_ARR_EMIT_INTARR
#  define AETHER_ARR_INTARR_FN
#else
#  define AETHER_ARR_INTARR_FN static inline
#endif
#ifdef AETHER_ARR_EMIT_FLOATARR
#  define AETHER_ARR_FLOATARR_FN
#else
#  define AETHER_ARR_FLOATARR_FN static inline
#endif
#ifdef AETHER_ARR_EMIT_LONGARR
#  define AETHER_ARR_LONGARR_FN
#else
#  define AETHER_ARR_LONGARR_FN static inline
#endif

AETHER_ARR_INTARR_FN int intarr_get_unchecked(AetherIntArray* arr, int i) {
    return arr->data[i];
}
AETHER_ARR_INTARR_FN void intarr_set_unchecked(AetherIntArray* arr, int i, int value) {
    arr->data[i] = value;
}
AETHER_ARR_FLOATARR_FN double floatarr_get_unchecked(AetherFloatArray* arr, int i) {
    return arr->data[i];
}
AETHER_ARR_FLOATARR_FN void floatarr_set_unchecked(AetherFloatArray* arr, int i, double value) {
    arr->data[i] = value;
}
AETHER_ARR_LONGARR_FN int64_t longarr_get_unchecked(AetherLongArray* arr, int i) {
    return arr->data[i];
}
AETHER_ARR_LONGARR_FN void longarr_set_unchecked(AetherLongArray* arr, int i, int64_t value) {
    arr->data[i] = value;
}

/* The buffer itself, as a plain C array (#2041).
 *
 * `a[i]` on the handle cannot work: the handle is a bare `ptr` and `[]`
 * has no element type to dispatch on. A VIEW does work, because it is
 * typed -- `v = intarr.array(a)` then `v[i]`, which the compiler lowers to
 * the same load the accessor inlines to. `std.strarr` has had exactly this
 * shape since it was written (`strarr.array` feeding `sort.strings_by`).
 *
 * The view borrows: it is valid until the handle is freed or resized, and
 * bounds are the caller's, as they are for the unchecked accessors. */
AETHER_ARR_INTARR_FN int* intarr_data(AetherIntArray* arr) {
    return arr ? arr->data : NULL;
}
AETHER_ARR_FLOATARR_FN double* floatarr_data(AetherFloatArray* arr) {
    return arr ? arr->data : NULL;
}
AETHER_ARR_LONGARR_FN int64_t* longarr_data(AetherLongArray* arr) {
    return arr ? arr->data : NULL;
}

#endif /* AETHER_ARR_INLINE_H */
