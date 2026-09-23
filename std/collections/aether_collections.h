#ifndef AETHER_COLLECTIONS_H
#define AETHER_COLLECTIONS_H

#include "../string/aether_string.h"
#include "../alloc/aether_alloc.h"
#include <stddef.h>

typedef struct ArrayList ArrayList;
typedef struct HashMap HashMap;
/* #1986: the packed-array layouts and their `static inline` unchecked
 * accessors. A program pulls in just that header through `@c_include`;
 * this one re-exports it so every C user of the collections sees the
 * same definitions. */
#include "aether_arr_inline.h"

/* The packed-array layouts live in the header above, under Aether-prefixed
 * names, because that one is injected into user programs by `@c_include`
 * and must not claim a common name there (#2162). These aliases are for C
 * that includes THIS header deliberately -- an extern shim, the runtime's
 * own sources -- where the short spelling reads better and nothing is
 * being imposed on anybody. */
typedef AetherIntArray   IntArray;
typedef AetherFloatArray FloatArray;
typedef AetherLongArray  LongArray;
typedef struct StrArray StrArray;

ArrayList* list_new(void);
ArrayList* list_new_in(AetherAllocator* alloc);
int list_add_raw(ArrayList* list, void* item);

/* Heap-string-aware add (#467). Retains the value (AetherString
 * refcount bump), tags the list as owning string elements, then
 * stores the pointer. list_free walks owned-string lists and
 * releases each element before freeing the backing array.
 *
 * Codegen routes `list.add(list, expr)` here when `expr` is a
 * heap-classified string expression (string.concat, string
 * interp, heap-returning user-fn, heap-tracked local). Plain
 * literals stay on `list_add_raw` (no retain, list won't release
 * at free). */
/* Owning add: the list acquires its own reference (retain for a
 * refcounted AetherString, an owned copy for a plain pointer), so the
 * caller keeps theirs. Safe to share one value across containers and
 * safe with string literals. */
int list_add_string_owned(ArrayList* list, const void* item);
/* Adopting add: takes over the caller's single reference. Emitted by
 * codegen for values escaping into the list; not for hand-written
 * calls (see the note in aether_collections.c). */
int list_add_string_adopted(ArrayList* list, const void* item);
/* Add a heap closure box the list owns (owned_flags == 2): list_free
 * reclaims the box AND its captured env. Routed when a `fn`-typed closure
 * value is stored into a list. */
int list_add_closure_owned(ArrayList* list, void* box);
// Return element at `index`, or NULL for out-of-bounds / null list. The
// Aether wrapper `list.get` in std/collections/module.ae turns these into
// Go-style `(value, err)` returns.
void* list_get_raw(ArrayList* list, int index);
void list_set(ArrayList* list, int index, void* item);
int list_size(ArrayList* list);
void list_remove(ArrayList* list, int index);
void list_clear(ArrayList* list);
void list_free(ArrayList* list);

HashMap* map_new(void);
int map_put_raw(HashMap* map, const char* key, void* value);

/* Heap-string-aware put (#467). Retains the value and tags the
 * map so map_free / map_clear release each string value before
 * freeing the bucket entries. Codegen routes
 * `map.put(m, k, heap_string_expr)` here when the value is heap-
 * classified. Plain literals stay on map_put_raw (no retain, map
 * won't release at free). */
/* Owning put / adopting put: same split as the list pair above. */
int map_put_string_owned(HashMap* map, const char* key, const void* value);
int map_put_string_adopted(HashMap* map, const char* key, const void* value);
// Return value for `key`, or NULL for absent / null-input. The Aether
// wrapper `map.get` distinguishes "absent" (null, "") from wrong-input
// (null, "null map").
void* map_get_raw(HashMap* map, const char* key);
int map_has(HashMap* map, const char* key);
void map_remove(HashMap* map, const char* key);
int map_size(HashMap* map);
void map_clear(HashMap* map);
void map_free(HashMap* map);

typedef struct {
    AetherString** keys;
    int count;
} MapKeys;

// Allocates a MapKeys snapshot; caller frees with map_keys_free. Returns
// NULL on allocation failure or null map. Aether wrapper `map.keys` gives
// a Go-style `(keys, err)` tuple.
MapKeys* map_keys_raw(HashMap* map);
void map_keys_free(MapKeys* keys);

// Read a snapshot. The returned string is borrowed: valid until
// map_keys_free, and only while the map still holds that entry. Out-of-range
// or NULL yields NULL.
int map_keys_size_raw(MapKeys* keys);
AetherString* map_keys_get_raw(MapKeys* keys, int index);

// -------------------------------------------------------------------
// IntArray — fixed-size packed int buffer with O(1) random access.
//
// The std.list wrappers store `void*` items, so packing ints costs a
// boxing allocation per entry. For DP tables (blame LCS, edit-distance,
// etc.) and other hot loops doing flat int-keyed lookup, IntArray is
// the simpler, faster tool: heap-allocated `int[size]`, direct access.
//
// Size is fixed at allocation — no amortised growth, no over-capacity.
// Callers that need a growing buffer reach for std.list. Callers that
// need a multi-dimensional access pattern (M rows × N cols) compute
// `row * N + col` themselves; IntArray is flat.
//
// Bounds checking: intarr_set and intarr_get both check and return
// safely on out-of-range (set is a no-op, get returns 0). The `_unchecked`
// variants skip the check for hot loops that have already validated
// the index — use them sparingly.
// -------------------------------------------------------------------

// Allocate an IntArray of `size` elements, zero-initialised. Returns
// NULL if size is negative or allocation failed. Name is `_raw` so the
// Aether-side wrapper can be named `intarr.new` without colliding with
// this extern's mangled C name.
IntArray* intarr_new_raw(int size);

// Allocate and fill with `init` at every index. Same failure modes as
// intarr_new_raw.
IntArray* intarr_new_filled_raw(int size, int init);

// Number of elements. Returns -1 if `arr` is NULL — distinguishable
// from a legal empty array (size 0).
int intarr_size(IntArray* arr);

// Read the value at `i`. Returns 0 if `arr` is NULL or `i` is
// out-of-range. Aether wrapper `intarr.get` turns these into Go-style
// `(value, err)` returns.
int intarr_get_raw(IntArray* arr, int i);

// Write `value` at `i`. No-op if `arr` is NULL or `i` is out-of-range.
void intarr_set_raw(IntArray* arr, int i, int value);

// Hot-path skip-the-bounds-check variants. Caller is responsible for
// keeping the index in [0, size). Out-of-range access is undefined
// behaviour. Used for inner loops in DP table walks.

// Fill every element with `value`. Useful for resetting a DP table
// between problem instances without reallocating.
void intarr_fill(IntArray* arr, int value);

// Release the backing buffer and the struct. Idempotent on NULL.
void intarr_free(IntArray* arr);

// StrArray — a GROWABLE array of borrowed string pointers whose backing IS a
// `string[]` (const char**), so it feeds std.sort.strings_by directly. The
// string companion to IntArray, except it grows (push) because the count is
// runtime-determined. Borrows its elements (never frees them); strarr_free
// releases only the spine. See aether_strarr.c.
StrArray* strarr_new_raw(int hint);
int strarr_size(StrArray* a);
int strarr_push_raw(StrArray* a, const char* s);
int strarr_push_copy_raw(StrArray* a, const char* s);
const char* strarr_get_raw(StrArray* a, int i);
void strarr_set_raw(StrArray* a, int i, const char* s);
const char** strarr_data(StrArray* a);
void strarr_free(StrArray* a);

// -------------------------------------------------------------------
// FloatArray — fixed-size packed double buffer with O(1) random access.
//
// The float twin of IntArray. Same shape, same bounds-check policy.
// Aether's `float` lowers to C `double`, so the element type is
// `double` end-to-end. Use this for SVG path-command arg storage,
// rasterizer edge tables, bbox accumulators, blur kernel coefficients,
// and any other workload that wants a flat packed-double buffer
// without going through std.list's void*-boxed-per-entry overhead.
// -------------------------------------------------------------------

// Allocate a FloatArray of `size` elements, zero-initialised. Returns
// NULL if size is negative or allocation failed. The `_raw` suffix
// matches intarr_new_raw's collision-avoidance with the Aether-side
// `floatarr.new` wrapper.
FloatArray* floatarr_new_raw(int size);

// Allocate and fill with `init` at every index.
FloatArray* floatarr_new_filled_raw(int size, double init);

// Number of elements. Returns -1 if `arr` is NULL — distinguishable
// from a legal empty array (size 0).
int floatarr_size(FloatArray* arr);

// Read the value at `i`. Returns 0.0 if `arr` is NULL or `i` is
// out-of-range. Aether wrapper `floatarr.get` turns these into
// Go-style `(value, err)` returns.
double floatarr_get_raw(FloatArray* arr, int i);

// Write `value` at `i`. No-op if `arr` is NULL or `i` is out-of-range.
void floatarr_set_raw(FloatArray* arr, int i, double value);

// Hot-path skip-the-bounds-check variants. Caller is responsible for
// keeping the index in [0, size); OOB is undefined behaviour.

// Fill every element with `value`.
void floatarr_fill(FloatArray* arr, double value);

// Release the backing buffer and the struct. Idempotent on NULL.
void floatarr_free(FloatArray* arr);

// -------------------------------------------------------------------
// LongArray — fixed-size packed long-long buffer with O(1) random access.
//
// The 64-bit twin of IntArray. Same shape, same bounds-check policy.
// Aether's `long` lowers to C `int64_t`, so the element type is
// `int64_t` end-to-end. Use this for 64-bit-keyed lookup, packed-word
// tables (hash state words, crypto round buffers), and any other
// workload that wants a flat packed-long buffer without going through
// std.list's void*-boxed-per-entry overhead.
// -------------------------------------------------------------------

// Allocate a LongArray of `size` elements, zero-initialised. Returns
// NULL if size is negative or allocation failed. The `_raw` suffix
// matches intarr_new_raw's collision-avoidance with the Aether-side
// `longarr.new` wrapper.
LongArray* longarr_new_raw(int size);

// Allocate and fill with `init` at every index.
LongArray* longarr_new_filled_raw(int size, int64_t init);

// Number of elements. Returns -1 if `arr` is NULL — distinguishable
// from a legal empty array (size 0).
int longarr_size(LongArray* arr);

// Read the value at `i`. Returns 0 if `arr` is NULL or `i` is
// out-of-range. Aether wrapper `longarr.get` turns these into
// Go-style `(value, err)` returns.
int64_t longarr_get_raw(LongArray* arr, int i);

// Write `value` at `i`. No-op if `arr` is NULL or `i` is out-of-range.
void longarr_set_raw(LongArray* arr, int i, int64_t value);

// Hot-path skip-the-bounds-check variants. Caller is responsible for
// keeping the index in [0, size); OOB is undefined behaviour.

// Fill every element with `value`.
void longarr_fill(LongArray* arr, int64_t value);

// Release the backing buffer and the struct. Idempotent on NULL.
void longarr_free(LongArray* arr);

#endif
