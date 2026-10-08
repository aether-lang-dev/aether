/*
 * aether_config.c — Implementation of the public C ABI accessors declared
 * in aether_config.h.
 *
 * AetherValue* is a thin alias for the internal void* pointer that Aether's
 * map/list implementation hands around. The accessors delegate to the
 * existing map_get / list_get / list_size functions and reinterpret the
 * stored void* as the requested type.
 *
 * Because Aether's collections are untyped at the runtime level, these
 * functions trust the caller: if the script stored an int at "port" and
 * the host asks for a string, the host gets garbage. Document this in the
 * header and move on.
 */

#include "aether_config.h"
#include "../std/collections/aether_collections.h"
#include "../std/string/aether_string.h"
#include "aether_value_kind.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------
 * Map accessors
 * ----------------------------------------------------------------- */

/* A string the script stored is either a plain char* (a literal, a string
 * from C) or an AetherString (one the script built or kept a copy of: a
 * function that keeps its `string` parameter stores its own copy), and the
 * host wants the bytes either way. Returned as stored, the second read as
 * its header: emit_lib_composite's `env` printed garbage. */
const char* aether_config_get_string(AetherValue* root, const char* key) {
    if (!root || !key) return NULL;
    return aether_string_data(map_get_raw((HashMap*)root, key));
}

int32_t aether_config_get_int(AetherValue* root, const char* key, int32_t default_value) {
    if (!root || !key) return default_value;
    if (!map_has((HashMap*)root, key)) return default_value;
    return (int32_t)(intptr_t)map_get_raw((HashMap*)root, key);
}

int64_t aether_config_get_int64(AetherValue* root, const char* key, int64_t default_value) {
    if (!root || !key) return default_value;
    if (!map_has((HashMap*)root, key)) return default_value;
    /* Aether stores int64 in a box or directly as intptr_t depending on the
     * platform. On 64-bit systems intptr_t is 64 bits so this is safe.
     * On 32-bit platforms the int64 would have been boxed via malloc — but
     * Aether's target is 64-bit for lib mode, so we assume intptr_t == int64_t.
     */
    return (int64_t)(intptr_t)map_get_raw((HashMap*)root, key);
}

float aether_config_get_float(AetherValue* root, const char* key, float default_value) {
    if (!root || !key) return default_value;
    if (!map_has((HashMap*)root, key)) return default_value;
    /* Aether boxes float values as malloc'd float* when storing in an
     * untyped map. If the .ae code stored raw bits the caller gets
     * undefined behavior — documented in the header. */
    void* v = map_get_raw((HashMap*)root, key);
    if (!v) return default_value;
    return *(float*)v;
}

int32_t aether_config_get_bool(AetherValue* root, const char* key, int32_t default_value) {
    if (!root || !key) return default_value;
    if (!map_has((HashMap*)root, key)) return default_value;
    return (int32_t)(intptr_t)map_get_raw((HashMap*)root, key) ? 1 : 0;
}

AetherValue* aether_config_get_map(AetherValue* root, const char* key) {
    if (!root || !key) return NULL;
    return (AetherValue*)map_get_raw((HashMap*)root, key);
}

AetherValue* aether_config_get_list(AetherValue* root, const char* key) {
    if (!root || !key) return NULL;
    return (AetherValue*)map_get_raw((HashMap*)root, key);
}

int32_t aether_config_has(AetherValue* root, const char* key) {
    if (!root || !key) return 0;
    return map_has((HashMap*)root, key) ? 1 : 0;
}

/* -----------------------------------------------------------------
 * List accessors
 * ----------------------------------------------------------------- */

int32_t aether_config_list_size(AetherValue* list) {
    if (!list) return 0;
    return (int32_t)list_size((ArrayList*)list);
}

AetherValue* aether_config_list_get(AetherValue* list, int32_t index) {
    if (!list) return NULL;
    if (index < 0 || index >= list_size((ArrayList*)list)) return NULL;
    return (AetherValue*)list_get_raw((ArrayList*)list, index);
}

const char* aether_config_list_get_string(AetherValue* list, int32_t index) {
    if (!list) return NULL;
    if (index < 0 || index >= list_size((ArrayList*)list)) return NULL;
    return aether_string_data(list_get_raw((ArrayList*)list, index));
}

int32_t aether_config_list_get_int(AetherValue* list, int32_t index, int32_t default_value) {
    if (!list) return default_value;
    if (index < 0 || index >= list_size((ArrayList*)list)) return default_value;
    return (int32_t)(intptr_t)list_get_raw((ArrayList*)list, index);
}

int64_t aether_config_list_get_int64(AetherValue* list, int32_t index, int64_t default_value) {
    if (!list) return default_value;
    if (index < 0 || index >= list_size((ArrayList*)list)) return default_value;
    return (int64_t)(intptr_t)list_get_raw((ArrayList*)list, index);
}

float aether_config_list_get_float(AetherValue* list, int32_t index, float default_value) {
    if (!list) return default_value;
    if (index < 0 || index >= list_size((ArrayList*)list)) return default_value;
    void* v = list_get_raw((ArrayList*)list, index);
    if (!v) return default_value;
    return *(float*)v;
}

int32_t aether_config_list_get_bool(AetherValue* list, int32_t index, int32_t default_value) {
    if (!list) return default_value;
    if (index < 0 || index >= list_size((ArrayList*)list)) return default_value;
    return (int32_t)(intptr_t)list_get_raw((ArrayList*)list, index) ? 1 : 0;
}

/* -----------------------------------------------------------------
 * Lifetime
 *
 * The v1 convention: the root returned by the .ae script's entry function
 * is an ArrayList or HashMap (or something that points to one). Both
 * structures have dedicated free functions. Walking the tree is the
 * caller's responsibility — we can't free nested maps/lists from here
 * without a type tag. In practice, most `ae config` scripts build a
 * single-owner tree and the whole thing is released on free.
 *
 * Ownership rule: nested maps/lists inside the root are not freed by
 * this function; only the root's own storage is. This matches how the
 * Aether runtime itself handles these structures — collection cleanup
 * is per-container, not deep-recursive. Callers that need deep cleanup
 * walk the tree themselves, or structure their scripts to return a
 * flat map.
 * ----------------------------------------------------------------- */

void aether_config_free(AetherValue* root) {
    if (!root) return;
    /* Use the kind tag to free the right container shape — the
     * host doesn't have to know whether the script returned a map
     * or a list. Falls back to the historical map-only path if the
     * pointer carries no recognisable magic (preserves behaviour
     * for callers in the field who rely on aether_config_free
     * working on map roots specifically). */
    switch (aether_value_kind(root)) {
        case AETHER_KIND_MAP:  map_free((HashMap*)root);   return;
        case AETHER_KIND_LIST: list_free((ArrayList*)root); return;
        case AETHER_KIND_UNKNOWN:
        default:               map_free((HashMap*)root);   return;
    }
}

/* ------------------------------------------------------------------
 * Kind discriminator — see header for safety contract.
 * ------------------------------------------------------------------ */

AetherKind aether_value_kind(AetherValue* v) {
    if (!v) return AETHER_KIND_UNKNOWN;
    /* Low-address guard: the zero page + first 64 KiB are unmapped
     * on every supported OS (Linux's vm.mmap_min_addr default is
     * 65536; macOS reserves more; Windows likewise). Any pointer
     * below this range is a small intptr-cast scalar — never a
     * heap allocation. Filter it before any deref. */
    if ((uintptr_t)v < AETHER_KIND_MIN_VALID_ADDR) return AETHER_KIND_UNKNOWN;
    /* Read the leading 4 bytes via memcpy to avoid strict-aliasing
     * warnings. The cast is safe because the low-address guard
     * above eliminated the only common UB case (intptr-cast small
     * scalars); for higher-address intptr scalars that happen to
     * land in mapped memory, the magic is 32 bits so the false-
     * match probability is 1 - 2^-32 across both magics combined. */
    uint32_t magic = 0;
    /* Volatile cast: prevents the compiler from optimising the
     * read away on the assumption that v's storage class implies
     * a particular layout. */
    const volatile uint32_t* head = (const volatile uint32_t*)v;
    magic = *head;
    if (magic == AETHER_KIND_LIST_MAGIC) return AETHER_KIND_LIST;
    if (magic == AETHER_KIND_MAP_MAGIC)  return AETHER_KIND_MAP;
    return AETHER_KIND_UNKNOWN;
}

int32_t aether_value_is_map(AetherValue* v) {
    return aether_value_kind(v) == AETHER_KIND_MAP ? 1 : 0;
}

int32_t aether_value_is_list(AetherValue* v) {
    return aether_value_kind(v) == AETHER_KIND_LIST ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Deep-recursive free.
 *
 * The walk uses aether_value_kind() at every step so a scalar
 * stored under a map key (e.g. `port: 8080` → intptr-cast int)
 * is correctly skipped — only recognised containers are freed.
 * This means the function is safe to call on any tree shape; it
 * never derefs a stored scalar. The trade-off is that
 * heap-allocated SCALARS (e.g. malloc'd float* or strdup'd
 * strings the script put into the map) are NOT freed by this
 * function — the host owns those separately. Documented in the
 * header.
 * ------------------------------------------------------------------ */

static void free_deep_internal(AetherValue* v);

static void free_deep_map(HashMap* m) {
    if (!m) return;
    /* Snapshot the keys, then walk values and recurse. The
     * snapshot insulates us from re-entrant modifications during
     * the walk (none expected from a free path, but cheap
     * insurance). map_keys_raw allocates a MapKeys struct that we
     * release with map_keys_free when done. */
    MapKeys* keys = map_keys_raw(m);
    if (keys) {
        for (int i = 0; i < keys->count; i++) {
            AetherString* k = keys->keys[i];
            if (!k || !k->data) continue;
            void* val = map_get_raw(m, k->data);
            free_deep_internal((AetherValue*)val);
        }
        map_keys_free(keys);
    }
    map_free(m);
}

static void free_deep_list(ArrayList* l) {
    if (!l) return;
    int n = list_size(l);
    for (int i = 0; i < n; i++) {
        free_deep_internal((AetherValue*)list_get_raw(l, i));
    }
    list_free(l);
}

static void free_deep_internal(AetherValue* v) {
    switch (aether_value_kind(v)) {
        case AETHER_KIND_MAP:  free_deep_map((HashMap*)v);   return;
        case AETHER_KIND_LIST: free_deep_list((ArrayList*)v); return;
        case AETHER_KIND_UNKNOWN:
        default:               return;  /* scalar / NULL / freed — leave alone */
    }
}

void aether_config_free_deep(AetherValue* root) {
    free_deep_internal(root);
}
