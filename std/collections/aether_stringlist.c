#include "aether_stringlist.h"
#include "aether_collections.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

/* Own an independent copy of `s`, as a refcounted AetherString of all
 * its bytes. `s` is an AetherString (the externs' `string_` prefix passes
 * an Aether string with its header) or a plain `char*`; the aether_string_*
 * helpers read either, NULL-safe. Copying up to the length, not to the
 * first NUL, keeps a value such as url.decode("x%00y") whole (#2469); the
 * copy used to be a strlen-bounded `char*`, which kept "x". The list frees
 * these copies with sl_free_item; the caller keeps ownership of its
 * argument. Returns NULL on OOM. */
static AetherString* sl_dup_item(const void* s) {
    if (!s) return string_new_with_length("", 0);
    return string_new_with_length(aether_string_data(s), aether_string_length(s));
}

/* Free a copy produced by sl_dup_item. NULL-safe. */
static void sl_free_item(const void* item) {
    if (item) string_release(item);
}

/* StringList is a thin layer over the existing ArrayList. We could
 * have reused ArrayList directly with retain/release sprinkled on
 * top, but a distinct type makes the contract clear in C consumers
 * and lets `string_list_*` be safe to call even when the user mixed
 * a plain ArrayList in by accident — they'd get a NULL back rather
 * than corrupting refcounts. */
struct StringList {
    ArrayList* items;
};

StringList* string_list_new(void) {
    /* #463: cap-aware wrapper struct. The backing ArrayList is
     * already caps-aware (#471); only the StringList shell
     * converts here. */
    StringList* sl = (StringList*)aether_caps_malloc(sizeof(StringList));
    if (!sl) return NULL;
    sl->items = list_new();
    if (!sl->items) {
        aether_caps_free(sl, sizeof(StringList));
        return NULL;
    }
    return sl;
}

int string_list_add(StringList* list, const void* s) {
    if (!list || !list->items) return 0;
    /* Store an independent copy the list owns. The caller keeps
     * ownership of its argument (the `string` param is copied, not
     * retained) — this is what lets a heap `char*` element
     * (string.concat / split result) be reclaimed: the caller frees
     * its value at scope exit, and the list frees its copy in
     * free/clear/remove. Storing the caller's raw pointer instead
     * leaked every such element, because string_release is a no-op on
     * a non-magic `char*`. */
    AetherString* copy = sl_dup_item(s);
    if (!copy) return 0;
    if (!list_add_raw(list->items, copy)) {
        sl_free_item(copy);
        return 0;
    }
    return 1;
}

const void* string_list_get(StringList* list, int index) {
    if (!list || !list->items) return NULL;
    return list_get_raw(list->items, index);
}

void string_list_set(StringList* list, int index, const void* s) {
    if (!list || !list->items) return;
    if (index < 0 || index >= list_size(list->items)) return;
    /* Copy the new value FIRST, then swap it in and free the old copy.
     * Copying first makes a self-set
     * (`string_list_set(L, i, string_list_get(L, i))`) safe: the
     * source bytes are duplicated before the old element is freed, so
     * there is no read-after-free even when `s` aliases the slot. */
    AetherString* copy = sl_dup_item(s);
    if (!copy) return;
    const void* old = list_get_raw(list->items, index);
    list_set(list->items, index, copy);
    sl_free_item(old);
}

int string_list_size(StringList* list) {
    if (!list || !list->items) return -1;
    return list_size(list->items);
}

void string_list_remove(StringList* list, int index) {
    if (!list || !list->items) return;
    if (index < 0 || index >= list_size(list->items)) return;
    const void* old = list_get_raw(list->items, index);
    list_remove(list->items, index);
    sl_free_item(old);
}

void string_list_clear(StringList* list) {
    if (!list || !list->items) return;
    int n = list_size(list->items);
    for (int i = 0; i < n; i++) {
        const void* item = list_get_raw(list->items, i);
        sl_free_item(item);
    }
    list_clear(list->items);
}

void string_list_free(StringList* list) {
    if (!list) return;
    if (list->items) {
        int n = list_size(list->items);
        for (int i = 0; i < n; i++) {
            const void* item = list_get_raw(list->items, i);
            sl_free_item(item);
        }
        list_free(list->items);
    }
    aether_caps_free(list, sizeof(StringList));
}

/* #967: sort support.
 *
 * The reorder strategy sidesteps the get/set aliasing footgun (issue
 * #967): `string_list_get` hands back the slot's internal pointer and a
 * naive adjacent-swap frees a slot that another borrowed pointer still
 * aliases. Instead we snapshot the borrowed element pointers into a
 * scratch array, sort the scratch (a permutation of the same pointers),
 * then write them back with `list_set` — which only overwrites the slot
 * pointer (never frees). No element is copied or freed, so the pointer
 * set is preserved exactly: no leak, no double-free, no read-after-free.
 */

/* Layout-compatible view of the codegen's boxed `_AeClosure`
 * (see std/collections/aether_stringseq.c and codegen.c's prologue):
 *     typedef struct { void (*fn)(void); void* env; } _AeClosure;
 * `env` is the implicit first argument to `fn`. */
typedef struct { void (*fn)(void); void* env; } AeStrListClosure;

/* The comparator is declared `@noescape` (#2523): its box is the caller's
 * stack and its env the caller's to release after the sort, as for the seq
 * combinators. Nothing here frees either. */

/* Stable top-down merge sort of borrowed pointers, invoking the Aether
 * comparator. Taking the left run on a tie (`cmp <= 0`) preserves the
 * input order of equal elements — the stability the issue asks for. */
static void sl_msort(const char** a, const char** tmp, int lo, int hi,
                     int (*cmp)(void*, const char*, const char*), void* env) {
    if (hi - lo <= 1) return;
    int mid = lo + (hi - lo) / 2;
    sl_msort(a, tmp, lo, mid, cmp, env);
    sl_msort(a, tmp, mid, hi, cmp, env);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (cmp(env, a[i] ? a[i] : "", a[j] ? a[j] : "") <= 0)
            tmp[k++] = a[i++];
        else
            tmp[k++] = a[j++];
    }
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi)  tmp[k++] = a[j++];
    for (int x = lo; x < hi; x++) a[x] = tmp[x];
}

void string_list_sort(StringList* list, void* cmp_box) {
    if (!list || !list->items || !cmp_box) return;
    int n = list_size(list->items);
    if (n <= 1) return;

    AeStrListClosure clo = *(AeStrListClosure*)cmp_box;
    int (*cmp)(void*, const char*, const char*) =
        (int (*)(void*, const char*, const char*))clo.fn;

    size_t bytes = (size_t)n * sizeof(const char*);
    const char** snap = (const char**)aether_caps_malloc(bytes);
    const char** tmp  = (const char**)aether_caps_malloc(bytes);
    if (!snap || !tmp) {
        if (snap) aether_caps_free(snap, bytes);
        if (tmp)  aether_caps_free(tmp, bytes);
        return;
    }

    for (int i = 0; i < n; i++)
        snap[i] = (const char*)list_get_raw(list->items, i);
    sl_msort(snap, tmp, 0, n, cmp, clo.env);
    for (int i = 0; i < n; i++)
        list_set(list->items, i, (void*)snap[i]);

    aether_caps_free(snap, bytes);
    aether_caps_free(tmp, bytes);
}

/* Byte order over borrowed elements, for qsort: string_compare reads
 * each element's whole length, embedded NULs included. Equal elements are
 * byte-identical strings, so qsort's instability is unobservable here. */
static int sl_cmp_lex(const void* pa, const void* pb) {
    return string_compare(*(const void* const*)pa, *(const void* const*)pb);
}

void string_list_sort_lex(StringList* list) {
    if (!list || !list->items) return;
    int n = list_size(list->items);
    if (n <= 1) return;

    size_t bytes = (size_t)n * sizeof(const char*);
    const char** snap = (const char**)aether_caps_malloc(bytes);
    if (!snap) return;
    for (int i = 0; i < n; i++)
        snap[i] = (const char*)list_get_raw(list->items, i);
    qsort(snap, (size_t)n, sizeof(const char*), sl_cmp_lex);
    for (int i = 0; i < n; i++)
        list_set(list->items, i, (void*)snap[i]);
    aether_caps_free(snap, bytes);
}
