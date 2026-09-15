/* StrArray — a GROWABLE array of string pointers whose backing IS a bare
 * `string[]` (a `const char**`), so it feeds std.sort.strings_by / .strings /
 * .string_search directly.
 *
 * The gap it fills: `string[]` is a compile-time literal only, and the runtime
 * producers give other shapes — string.split → AetherStringArray*, fs.glob →
 * dir_list. There was no way to build a `string[]` of runtime-determined length
 * to sort. strarr is the string companion to intarr/longarr/floatarr, except it
 * grows (push) because the whole point is a count you don't know up front.
 *
 * Ownership: strarr_push BORROWS — exactly like a `string[]` literal, which
 * points at strings the caller owns — so the caller keeps those strings alive as
 * long as the strarr (or a sort result taken from it) is read. strarr_push_copy
 * instead takes an OWNED reference (retains a magic AetherString, copies a bare
 * literal) and frees it in strarr_free: the "I built this string, take it" case,
 * so a per-iteration concat/basename need not be kept alive by hand.
 *
 * The owned references live in a SEPARATE `owned_refs` free-list, NOT a
 * per-index flag on `data`: std.sort permutes `data` in place, which would
 * scramble any parallel per-index ownership, but the free-list is order-
 * independent so free is always correct after a sort. `data` stays a pure
 * `string[]` for sorting. */

#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"
#include <stdlib.h>
#include <string.h>

struct StrArray {
    const char** data;        /* the string[] backing — contiguous const char* */
    int size;                 /* number of elements pushed */
    int cap;                  /* allocated data slots */
    const char** owned_refs;  /* references push_copy created, freed at free()  */
    int owned_count;
    int owned_cap;
};

/* Allocate an empty StrArray with room for `hint` elements (0 is fine — the
 * first push allocates). A negative hint is treated as 0. Returns NULL on OOM. */
StrArray* strarr_new_raw(int hint) {
    StrArray* a = (StrArray*)aether_caps_malloc(sizeof(*a));
    if (!a) return NULL;
    a->data = NULL;
    a->size = 0;
    a->cap = 0;
    a->owned_refs = NULL;
    a->owned_count = 0;
    a->owned_cap = 0;
    if (hint > 0) {
        a->data = (const char**)aether_caps_malloc((size_t)hint * sizeof(const char*));
        if (!a->data) { aether_caps_free(a, sizeof(*a)); return NULL; }
        a->cap = hint;
    }
    return a;
}

int strarr_size(StrArray* a) {
    return a ? a->size : -1;
}

/* Ensure room for one more `data` element (2× growth). 1 on success, 0 on OOM. */
static int strarr_reserve_data(StrArray* a) {
    if (a->size < a->cap) return 1;
    int newcap = a->cap ? a->cap * 2 : 8;
    const char** gd = (const char**)aether_caps_realloc(
        a->data, (size_t)a->cap * sizeof(const char*),
        (size_t)newcap * sizeof(const char*));
    if (!gd) return 0;
    a->data = gd;
    a->cap = newcap;
    return 1;
}

/* Record an owned reference to free at strarr_free (order-independent, so a
 * later sort of `data` never disturbs it). 1 on success, 0 on OOM. */
static int strarr_track_owned(StrArray* a, const char* ref) {
    if (a->owned_count >= a->owned_cap) {
        int newcap = a->owned_cap ? a->owned_cap * 2 : 8;
        const char** g = (const char**)aether_caps_realloc(
            a->owned_refs, (size_t)a->owned_cap * sizeof(const char*),
            (size_t)newcap * sizeof(const char*));
        if (!g) return 0;
        a->owned_refs = g;
        a->owned_cap = newcap;
    }
    a->owned_refs[a->owned_count++] = ref;
    return 1;
}

/* Append `s` BORROWED — not copied, not freed by strarr. Grows 2× when full.
 * Returns 1 on success, 0 on OOM (unchanged). */
int strarr_push_raw(StrArray* a, const char* s) {
    if (!a) return 0;
    if (!strarr_reserve_data(a)) return 0;
    a->data[a->size++] = s;
    return 1;
}

/* Append an OWNED reference to `s` — retains an existing AetherString, or copies
 * a bare literal — and frees it in strarr_free. The "I built this string, take
 * it" case: a per-iteration concat/basename need not be kept alive by hand.
 * Returns 1 on success, 0 on OOM (unchanged; any copy made is rolled back). */
int strarr_push_copy_raw(StrArray* a, const char* s) {
    if (!a) return 0;
    if (!strarr_reserve_data(a)) return 0;
    const char* store = s;
    int made_ref = 0;
    if (s) {
        if (is_aether_string(s)) {
            string_retain(s);                 /* own a reference, no copy */
            made_ref = 1;
        } else {
            size_t n = strlen(s);
            AetherString* copy = string_new_with_length(s, n);
            if (!copy) return 0;
            store = (const char*)copy;
            made_ref = 1;
        }
    }
    if (made_ref && !strarr_track_owned(a, store)) {
        string_release(store);   /* couldn't record it — don't leak or store it */
        return 0;
    }
    a->data[a->size++] = store;
    return 1;
}

/* Read at `i`. Returns NULL if `a` is NULL or `i` is out of range. */
const char* strarr_get_raw(StrArray* a, int i) {
    if (!a || i < 0 || i >= a->size) return NULL;
    return a->data[i];
}

/* Overwrite the `data` slot at `i` with a BORROWED `s`. Any owned reference this
 * displaces is NOT freed here (it stays on the owned free-list and is released
 * at strarr_free) — harmless over-retention, never a leak or an early free, and
 * it keeps `data` a pure sortable buffer. No-op if out of range. */
void strarr_set_raw(StrArray* a, int i, const char* s) {
    if (!a || i < 0 || i >= a->size) return;
    a->data[i] = s;
}

/* The raw `string[]` backing — a `const char**` of `strarr_size` elements, laid
 * out exactly as a `string[]` literal, so `sort.strings_by(a.data() as string[],
 * a.size(), cmp)` sorts it in place. The pointer is valid until the next push
 * (which may realloc) or free; re-fetch after pushing. */
const char** strarr_data(StrArray* a) {
    return a ? a->data : NULL;
}

/* Free the spine (data buffer + struct) and release every owned reference from
 * push_copy. Borrowed elements (from push) are the caller's to free. */
void strarr_free(StrArray* a) {
    if (!a) return;
    for (int i = 0; i < a->owned_count; i++) {
        if (a->owned_refs[i]) string_release(a->owned_refs[i]);
    }
    aether_caps_free((void*)a->owned_refs, (size_t)a->owned_cap * sizeof(const char*));
    aether_caps_free((void*)a->data, (size_t)a->cap * sizeof(const char*));
    aether_caps_free(a, sizeof(*a));
}
