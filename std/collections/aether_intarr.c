/* IntArray — a fixed-size packed-int buffer.
 *
 * Intended for hot-path int-keyed lookup (DP tables, flat index
 * buffers) where going through std.list's void*-boxed items would
 * cost an allocation per entry and chase an extra pointer.
 *
 * Shape is as minimal as it gets: heap-allocate size*int, store a
 * size next to it, expose direct index ops plus a bulk-fill. No
 * amortised growth, no capacity-beyond-size. Callers that need
 * resize on write use std.list. */

/* This TU emits the intarr_* accessors from aether_arr_inline.h as
 * external definitions (#2169): libaether exports them for C that calls
 * them without the header. The bodies live in the header, once. */
#define AETHER_ARR_EMIT_INTARR
#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

IntArray* intarr_new_raw(int size) {
    if (size < 0) return NULL;
    /* #463: cap-aware. The struct is sizeof(IntArray); the data
     * array is size×sizeof(int). Both byte counts are recoverable at
     * free time from `arr->size`. */
    IntArray* arr = (IntArray*)aether_caps_malloc(sizeof(*arr));
    if (!arr) return NULL;
    arr->size = size;
    if (size == 0) {
        arr->data = NULL;   // legal empty array; intarr_free still OK
        return arr;
    }
    arr->data = (int*)aether_caps_calloc((size_t)size, sizeof(int));
    if (!arr->data) { aether_caps_free(arr, sizeof(*arr)); return NULL; }
    return arr;
}

IntArray* intarr_new_filled_raw(int size, int init) {
    IntArray* arr = intarr_new_raw(size);
    if (!arr) return NULL;
    for (int i = 0; i < size; i++) arr->data[i] = init;
    return arr;
}

int intarr_size(IntArray* arr) {
    return arr ? arr->size : -1;
}

int intarr_get_raw(IntArray* arr, int i) {
    if (!arr || i < 0 || i >= arr->size) return 0;
    return arr->data[i];
}

void intarr_set_raw(IntArray* arr, int i, int value) {
    if (!arr || i < 0 || i >= arr->size) return;
    arr->data[i] = value;
}

/* intarr_get_unchecked / _set_unchecked are `static inline` in
 * aether_collections.h (#1986): a call the C compiler cannot see through is
 * what kept a loop over a packed buffer from vectorising. */

void intarr_fill(IntArray* arr, int value) {
    if (!arr || arr->size == 0) return;
    for (int i = 0; i < arr->size; i++) arr->data[i] = value;
}

void intarr_free(IntArray* arr) {
    if (!arr) return;
    /* data was calloc'd size×sizeof(int) (NULL for the empty array,
     * where caps_free is a no-op). */
    aether_caps_free(arr->data, (size_t)arr->size * sizeof(int));
    aether_caps_free(arr, sizeof(*arr));
}
