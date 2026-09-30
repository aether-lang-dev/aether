/* aether_intmap.h — a hash map with integer keys and integer values (#1986).
 *
 * std.map keys by string, so a program counting integer keys (n-grams packed
 * into a long, ids, coordinates) rendered each key with string.from_int and
 * paid an allocation and a string hash per lookup: LangArena's Distance::NGram
 * ran at 26.6x Go on it. This map keys by `int64_t` directly.
 *
 * Open addressing with linear probing over a power-of-two table; keys are
 * spread by the splitmix64 finalizer, so sequential or strided keys do not
 * cluster. A removal leaves a tombstone, and a table whose live entries plus
 * tombstones pass 70% of it is rebuilt (doubled when live entries alone pass
 * half). Storage goes through the resource-cap allocator like the other
 * collections. */
#ifndef AETHER_INTMAP_H
#define AETHER_INTMAP_H

#include <stdint.h>

typedef struct AetherIntMap AetherIntMap;

/* A map with room for about `capacity_hint` entries before it grows (0 for
 * the default). NULL on allocation failure. */
AetherIntMap* aether_intmap_new(int capacity_hint);
void aether_intmap_free(AetherIntMap* m);

/* Set `key` to `value`. 1 when the key was new, 0 when it replaced a value,
 * -1 on allocation failure or a NULL map. */
int aether_intmap_put(AetherIntMap* m, int64_t key, int64_t value);

/* The value at `key`, or `missing` when absent (or the map is NULL). */
int64_t aether_intmap_get(AetherIntMap* m, int64_t key, int64_t missing);

/* 1 when `key` is present. */
int aether_intmap_has(AetherIntMap* m, int64_t key);

/* Add `delta` to the value at `key` (an absent key starts at 0) and return
 * the new value: one probe for the counting loop. When inserting a new key
 * fails to allocate, nothing changes, INT64_MIN is returned and
 * aether_intmap_add_failed(m) reports 1 until the next add (std.intmap's
 * wrapper turns that into the Aether panic, as `make` does). A NULL map
 * returns 0. */
int64_t aether_intmap_add(AetherIntMap* m, int64_t key, int64_t delta);

/* 1 when the last aether_intmap_add on `m` failed to allocate. */
int aether_intmap_add_failed(AetherIntMap* m);

/* Remove `key`. 1 when it was present. */
int aether_intmap_remove(AetherIntMap* m, int64_t key);

int aether_intmap_size(AetherIntMap* m);
void aether_intmap_clear(AetherIntMap* m);

/* Iteration by slot: the first occupied slot at or after `slot`, or -1 when
 * there is none. Start at 0 and continue from the returned slot + 1. Any put,
 * add of a new key, or clear may rebuild the table and invalidates slots. */
int aether_intmap_next(AetherIntMap* m, int slot);
int64_t aether_intmap_key_at(AetherIntMap* m, int slot);
int64_t aether_intmap_value_at(AetherIntMap* m, int slot);

#endif /* AETHER_INTMAP_H */
