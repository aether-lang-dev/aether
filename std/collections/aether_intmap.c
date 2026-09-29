#include "aether_intmap.h"
#include "../../runtime/aether_resource_caps.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* Open addressing with linear probing over a power-of-two slot array.
 *
 * A slot is empty, full, or a tombstone. A remove leaves a tombstone so
 * the probe chains through it stay connected; a put reuses the first
 * tombstone it passes. The table grows when live entries plus tombstones
 * pass 3/4 of the slots, and a rebuild drops every tombstone, so a
 * workload that churns keys through remove/put cannot fill the table
 * with dead slots and turn every miss into a full scan. */

#define INTMAP_INITIAL_CAPACITY 16
#define INTMAP_LOAD_NUMERATOR   3
#define INTMAP_LOAD_DENOMINATOR 4

enum { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_TOMB = 2 };

typedef struct {
    long long key;
    long long value;
    uint8_t   state;
} IntMapSlot;

struct AetherIntMap {
    IntMapSlot* slots;
    int         capacity;   /* always a power of two */
    int         size;       /* live entries */
    int         used;       /* live entries + tombstones */
};

static size_t slots_bytes(int capacity) {
    return (size_t)capacity * sizeof(IntMapSlot);
}

/* splitmix64's finalizer: every input bit reaches every output bit, so
 * keys that differ only in their high bits (n-grams packed as
 * c0<<24|c1<<16|c2<<8|c3, timestamps, pointers) do not collide in the low
 * bits the mask keeps. An identity hash would put every multiple of the
 * capacity in one probe chain. */
static uint64_t mix(long long key) {
    uint64_t z = (uint64_t)key;
    z ^= z >> 30;
    z *= 0xbf58476d1ce4e5b9ULL;
    z ^= z >> 27;
    z *= 0x94d049bb133111ebULL;
    z ^= z >> 31;
    return z;
}

static IntMapSlot* alloc_slots(int capacity) {
    return (IntMapSlot*)aether_caps_calloc((size_t)capacity, sizeof(IntMapSlot));
}

AetherIntMap* aether_intmap_new(void) {
    AetherIntMap* map = (AetherIntMap*)aether_caps_malloc(sizeof(AetherIntMap));
    if (!map) return NULL;
    map->capacity = INTMAP_INITIAL_CAPACITY;
    map->size = 0;
    map->used = 0;
    map->slots = alloc_slots(map->capacity);
    if (!map->slots) { aether_caps_free(map, sizeof(AetherIntMap)); return NULL; }
    return map;
}

/* The slot `key` lives in, or the slot a put of `key` should take: the
 * first tombstone on its probe path when there is one, else the empty
 * slot that ends the path. The table is never full, so the loop ends. */
static IntMapSlot* find_slot(AetherIntMap* map, long long key, int* found) {
    unsigned int mask = (unsigned int)map->capacity - 1;
    unsigned int i = (unsigned int)mix(key) & mask;
    IntMapSlot* tomb = NULL;
    for (;;) {
        IntMapSlot* s = &map->slots[i];
        if (s->state == SLOT_EMPTY) {
            *found = 0;
            return tomb ? tomb : s;
        }
        if (s->state == SLOT_FULL) {
            if (s->key == key) { *found = 1; return s; }
        } else if (!tomb) {
            tomb = s;
        }
        i = (i + 1) & mask;
    }
}

/* Rebuild into `new_capacity` slots, dropping tombstones. Keeps the old
 * table on allocation failure so the caller's insert can still land in a
 * tombstone or a spare slot. */
static int rebuild(AetherIntMap* map, int new_capacity) {
    IntMapSlot* old = map->slots;
    int old_capacity = map->capacity;
    IntMapSlot* fresh = alloc_slots(new_capacity);
    if (!fresh) return 0;

    map->slots = fresh;
    map->capacity = new_capacity;
    map->used = map->size;
    for (int i = 0; i < old_capacity; i++) {
        if (old[i].state != SLOT_FULL) continue;
        int found = 0;
        IntMapSlot* s = find_slot(map, old[i].key, &found);
        s->key = old[i].key;
        s->value = old[i].value;
        s->state = SLOT_FULL;
    }
    aether_caps_free(old, slots_bytes(old_capacity));
    return 1;
}

/* Make room for one more entry if the load factor asks for it. Doubles
 * when the live entries need the space; rebuilds at the same size when
 * tombstones are what filled it. */
static int ensure_room(AetherIntMap* map) {
    if ((long)(map->used + 1) * INTMAP_LOAD_DENOMINATOR <=
        (long)map->capacity * INTMAP_LOAD_NUMERATOR) {
        return 1;
    }
    int live_needs_growth = (long)(map->size + 1) * INTMAP_LOAD_DENOMINATOR >
                            (long)map->capacity * INTMAP_LOAD_NUMERATOR / 2;
    if (!live_needs_growth) return rebuild(map, map->capacity);
    /* CRITICAL: capacity is an int scaled by sizeof(IntMapSlot); doubling
     * unchecked would wrap into a small allocation the probes run past. */
    if (map->capacity > INT_MAX / 2) return 0;
    return rebuild(map, map->capacity * 2);
}

/* Slot to write `key` into, inserting it with `initial` when absent.
 * NULL when the map is null or the table could not make room. */
static IntMapSlot* slot_for_write(AetherIntMap* map, long long key, long long initial) {
    if (!map) return NULL;
    int found = 0;
    IntMapSlot* s = find_slot(map, key, &found);
    if (found) return s;
    if (!ensure_room(map)) {
        /* No growth: the slot find_slot chose is still valid, but only
         * take it when the table has a genuinely free slot left, so a
         * probe can always terminate on an empty one. */
        if (map->used + 1 >= map->capacity) return NULL;
    } else {
        s = find_slot(map, key, &found);
    }
    if (s->state == SLOT_EMPTY) map->used++;
    s->key = key;
    s->value = initial;
    s->state = SLOT_FULL;
    map->size++;
    return s;
}

int aether_intmap_put(AetherIntMap* map, long long key, long long value) {
    IntMapSlot* s = slot_for_write(map, key, value);
    if (!s) return 0;
    s->value = value;
    return 1;
}

long long aether_intmap_get_or(AetherIntMap* map, long long key, long long fallback) {
    if (!map) return fallback;
    int found = 0;
    IntMapSlot* s = find_slot(map, key, &found);
    return found ? s->value : fallback;
}

int aether_intmap_has(AetherIntMap* map, long long key) {
    if (!map) return 0;
    int found = 0;
    find_slot(map, key, &found);
    return found;
}

long long aether_intmap_add(AetherIntMap* map, long long key, long long delta) {
    if (!map) return 0;
    int found = 0;
    IntMapSlot* s = find_slot(map, key, &found);
    if (found) {
        s->value += delta;
        return s->value;
    }
    s = slot_for_write(map, key, delta);
    return s ? s->value : 0;
}

int aether_intmap_remove(AetherIntMap* map, long long key) {
    if (!map) return 0;
    int found = 0;
    IntMapSlot* s = find_slot(map, key, &found);
    if (!found) return 0;
    s->state = SLOT_TOMB;
    map->size--;
    return 1;
}

int aether_intmap_size(AetherIntMap* map) {
    return map ? map->size : 0;
}

int aether_intmap_capacity(AetherIntMap* map) {
    return map ? map->capacity : 0;
}

void aether_intmap_clear(AetherIntMap* map) {
    if (!map) return;
    for (int i = 0; i < map->capacity; i++) map->slots[i].state = SLOT_EMPTY;
    map->size = 0;
    map->used = 0;
}

void aether_intmap_free(AetherIntMap* map) {
    if (!map) return;
    aether_caps_free(map->slots, slots_bytes(map->capacity));
    aether_caps_free(map, sizeof(AetherIntMap));
}

/* One walk serves both snapshots so their orders cannot drift apart. */
static LongArray* snapshot(AetherIntMap* map, int want_values) {
    if (!map) return NULL;
    LongArray* out = longarr_new_raw(map->size);
    if (!out) return NULL;
    int n = 0;
    for (int i = 0; i < map->capacity; i++) {
        const IntMapSlot* s = &map->slots[i];
        if (s->state != SLOT_FULL) continue;
        longarr_set_raw(out, n++, want_values ? s->value : s->key);
    }
    return out;
}

LongArray* aether_intmap_keys(AetherIntMap* map) {
    return snapshot(map, 0);
}

LongArray* aether_intmap_values(AetherIntMap* map) {
    return snapshot(map, 1);
}
