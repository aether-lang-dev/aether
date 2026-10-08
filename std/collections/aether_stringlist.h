#ifndef AETHER_STRINGLIST_H
#define AETHER_STRINGLIST_H

#include <stddef.h>

/* std.stringlist — refcount-aware list for AetherString values.
 *
 * The plain `std.list` (ArrayList) stores items as raw `void*` and
 * does not retain on insert / release on remove or free. That works
 * for caller-managed lifetimes but silently dangles when the list
 * is asked to hold AetherString-bearing values whose refcount drops
 * to zero before the list itself is freed.
 *
 * StringList wraps the same backing storage but takes a strong
 * reference on every insert (via aether_string_retain) and releases
 * on remove / clear / free / overwrite. Plain `char*` literals are
 * passed through unchanged — string_retain is a no-op on values
 * that don't bear the AetherString magic header. Issue #274.
 */

typedef struct StringList StringList;

/* Allocate an empty list. Returns NULL on allocation failure. */
StringList* string_list_new(void);

/* Append `s`. The list takes a strong reference so `s` survives
 * even when the original variable goes out of scope. Returns 1 on
 * success, 0 on null list / OOM. */
int string_list_add(StringList* list, const void* s);

/* Read at `index`. Returns NULL on null list / OOB. The element is the
 * list's own AetherString copy, every byte of what was added, embedded
 * NULs included (#2469). The pointer is borrowed: callers that need to
 * outlive the list should string_retain it themselves or `string.copy` it. */
const void* string_list_get(StringList* list, int index);

/* Replace the slot at `index`. Releases the previous occupant and
 * retains the new one. No-op on null list / OOB. */
void string_list_set(StringList* list, int index, const void* s);

/* Number of stored elements. -1 on null list. */
int string_list_size(StringList* list);

/* Remove the entry at `index`, releasing its refcount. No-op on
 * null list / OOB. */
void string_list_remove(StringList* list, int index);

/* Drop every entry — releases refcounts but keeps the backing
 * allocation. No-op on null. */
void string_list_clear(StringList* list);

/* Release every entry then free the list. Idempotent on null. */
void string_list_free(StringList* list);

/* #967: stable in-place sort by a caller-supplied comparator closure
 * `fn(string, string) -> int` (negative / 0 / positive for a<b / a==b /
 * a>b, like strcmp). `cmp_box` is a codegen-boxed `_AeClosure` passed as
 * a ptr; this function OWNS it and frees it before returning (matching
 * the seq-combinator convention). Reorders the backing pointers only —
 * no element is copied or freed — so it sidesteps the get/set aliasing
 * trap of a hand-rolled swap. No-op (box still freed) on null / OOM. */
void string_list_sort(StringList* list, void* cmp_box);

/* #967: stable in-place ascending lexicographic (byte-wise `strcmp`)
 * sort — the common case, no closure needed. No-op on null. */
void string_list_sort_lex(StringList* list);

#endif
