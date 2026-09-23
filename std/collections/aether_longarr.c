/* Copyright (c) 2026 Aether Developers. */
/* LongArray — a fixed-size packed-long-long buffer (the 64-bit twin
 * of IntArray).
 *
 * Intended for hot-path 64-bit-keyed lookup and packed-word tables
 * (hash state words, crypto round buffers, wide DP tables) where going
 * through std.list's void*-boxed items would cost an allocation per
 * entry and chase an extra pointer. Aether's `long` lowers to C
 * `int64_t`, so the element type is `int64_t` end-to-end.
 *
 * Shape is as minimal as it gets: heap-allocate size*int64_t, store a
 * size next to it, expose direct index ops plus a bulk-fill. No
 * amortised growth, no capacity-beyond-size. Callers that need resize
 * on write use std.list. */

/* This TU emits the longarr_* accessors from aether_arr_inline.h as
 * external definitions (#2169): libaether exports them for C that calls
 * them without the header. The bodies live in the header, once. */
#define AETHER_ARR_EMIT_LONGARR
#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

LongArray* longarr_new_raw(int size) {
    if (size < 0) return NULL;
    /* #463: cap-aware. The struct is sizeof(LongArray); the data
     * array is size×sizeof(int64_t). Both byte counts are recoverable
     * at free time from `arr->size`. */
    LongArray* arr = (LongArray*)aether_caps_malloc(sizeof(*arr));
    if (!arr) return NULL;
    arr->size = size;
    if (size == 0) {
        arr->data = NULL;   // legal empty array; longarr_free still OK
        return arr;
    }
    arr->data = (int64_t*)aether_caps_calloc((size_t)size, sizeof(int64_t));
    if (!arr->data) { aether_caps_free(arr, sizeof(*arr)); return NULL; }
    return arr;
}

LongArray* longarr_new_filled_raw(int size, int64_t init) {
    LongArray* arr = longarr_new_raw(size);
    if (!arr) return NULL;
    for (int i = 0; i < size; i++) arr->data[i] = init;
    return arr;
}

int longarr_size(LongArray* arr) {
    return arr ? arr->size : -1;
}

int64_t longarr_get_raw(LongArray* arr, int i) {
    if (!arr || i < 0 || i >= arr->size) return 0;
    return arr->data[i];
}

void longarr_set_raw(LongArray* arr, int i, int64_t value) {
    if (!arr || i < 0 || i >= arr->size) return;
    arr->data[i] = value;
}

/* longarr_get_unchecked / _set_unchecked are `static inline` in
 * aether_collections.h (#1986), for the same reason as the int and float
 * twins: the definition must be visible where the indexing happens. */

void longarr_fill(LongArray* arr, int64_t value) {
    if (!arr || arr->size == 0) return;
    for (int i = 0; i < arr->size; i++) arr->data[i] = value;
}

void longarr_free(LongArray* arr) {
    if (!arr) return;
    /* data was calloc'd size×sizeof(int64_t) (NULL for the empty
     * array, where caps_free is a no-op). */
    aether_caps_free(arr->data, (size_t)arr->size * sizeof(int64_t));
    aether_caps_free(arr, sizeof(*arr));
}
