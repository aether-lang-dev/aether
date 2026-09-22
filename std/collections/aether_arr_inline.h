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

struct IntArray   { int*       data; int size; };
struct FloatArray { double*    data; int size; };
struct LongArray  { long long* data; int size; };

typedef struct IntArray   IntArray;
typedef struct FloatArray FloatArray;
typedef struct LongArray  LongArray;

/* Hot-path skip-the-bounds-check accessors. The caller keeps the index in
 * [0, size); out of range is undefined behaviour, exactly as it is for the
 * C array the buffer is. The checked forms (`*_get_raw` / `*_set_raw`, and
 * the Aether-side `get` / `set` wrappers) stay out of line in the .c. */
static inline int intarr_get_unchecked(IntArray* arr, int i) {
    return arr->data[i];
}
static inline void intarr_set_unchecked(IntArray* arr, int i, int value) {
    arr->data[i] = value;
}
static inline double floatarr_get_unchecked(FloatArray* arr, int i) {
    return arr->data[i];
}
static inline void floatarr_set_unchecked(FloatArray* arr, int i, double value) {
    arr->data[i] = value;
}
static inline long long longarr_get_unchecked(LongArray* arr, int i) {
    return arr->data[i];
}
static inline void longarr_set_unchecked(LongArray* arr, int i, long long value) {
    arr->data[i] = value;
}

#endif /* AETHER_ARR_INLINE_H */
