/* FloatArray — a fixed-size packed-double buffer.
 *
 * The float twin of IntArray (aether_intarr.c). Same shape; same
 * bounds-check policy; same allocation discipline. Aether's `float`
 * lowers to C `double`, so the element type is `double` end-to-end.
 *
 * Intended for hot-path float-keyed lookup (SVG path-command args,
 * rasterizer edge tables, bbox corner accumulators, blur kernel
 * coefficients) where going through std.list's void*-boxed items
 * would cost an allocation per entry and chase an extra pointer. */

/* This TU emits the floatarr_* accessors from aether_arr_inline.h as
 * external definitions (#2169): libaether exports them for C that calls
 * them without the header. The bodies live in the header, once. */
#define AETHER_ARR_EMIT_FLOATARR
#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

FloatArray* floatarr_new_raw(int size) {
    if (size < 0) return NULL;
    FloatArray* arr = (FloatArray*)aether_caps_malloc(sizeof(*arr));
    if (!arr) return NULL;
    arr->size = size;
    if (size == 0) {
        arr->data = NULL;   /* legal empty array; floatarr_free still OK */
        return arr;
    }
    arr->data = (double*)aether_caps_calloc((size_t)size, sizeof(double));
    if (!arr->data) { aether_caps_free(arr, sizeof(*arr)); return NULL; }
    return arr;
}

FloatArray* floatarr_new_filled_raw(int size, double init) {
    FloatArray* arr = floatarr_new_raw(size);
    if (!arr) return NULL;
    for (int i = 0; i < size; i++) arr->data[i] = init;
    return arr;
}

int floatarr_size(FloatArray* arr) {
    return arr ? arr->size : -1;
}

double floatarr_get_raw(FloatArray* arr, int i) {
    if (!arr || i < 0 || i >= arr->size) return 0.0;
    return arr->data[i];
}

void floatarr_set_raw(FloatArray* arr, int i, double value) {
    if (!arr || i < 0 || i >= arr->size) return;
    arr->data[i] = value;
}

/* floatarr_get_unchecked / _set_unchecked are `static inline` in
 * aether_collections.h (#1986): the definition has to be visible to the
 * translation unit doing the indexing, or the loop cannot vectorise. */

void floatarr_fill(FloatArray* arr, double value) {
    if (!arr || arr->size == 0) return;
    for (int i = 0; i < arr->size; i++) arr->data[i] = value;
}

void floatarr_free(FloatArray* arr) {
    if (!arr) return;
    aether_caps_free(arr->data, (size_t)arr->size * sizeof(double));
    aether_caps_free(arr, sizeof(*arr));
}
