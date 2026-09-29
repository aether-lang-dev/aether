#ifndef AETHER_INTMAP_H
#define AETHER_INTMAP_H

#include "aether_collections.h"

// Hash map from 64-bit integer keys to 64-bit integer values.
//
// std.map keys every entry by a string: an integer key has to be rendered
// (an allocation), hashed byte by byte, and compared with memcmp, which is
// what put Distance::NGram at 26x Go in the LangArena port (#1986). This
// table hashes the integer itself and stores key and value inline in an
// open-addressing array, so a lookup touches one cache line and a count
// increment is a single probe.
//
// Keys and values are long long, not long: Aether `long` is 64-bit and
// Windows C `long` is 32 (the aether_pqueue convention). A value is a bare
// integer the map never interprets, so it can carry an int, a long, or a
// pointer via std.mem.ptr_to_long; nothing is freed on the caller's behalf.
// Aether-facing wrappers live in std/intmap/module.ae.

typedef struct AetherIntMap AetherIntMap;

// Returns NULL on allocation failure.
AetherIntMap* aether_intmap_new(void);

// Insert or overwrite. 1 on success, 0 on a null map or allocation failure.
int aether_intmap_put(AetherIntMap* map, long long key, long long value);

// The value stored under `key`, or `fallback` when the key is absent (or
// the map is null). A stored value equal to `fallback` is indistinguishable
// here; use aether_intmap_has when that matters.
long long aether_intmap_get_or(AetherIntMap* map, long long key, long long fallback);

// 1 if the key is present, 0 otherwise (also 0 for a null map).
int aether_intmap_has(AetherIntMap* map, long long key);

// Add `delta` to the value under `key`, treating an absent key as 0, and
// return the new value. One probe: this is the counting idiom. On a null
// map or an allocation failure nothing is stored and 0 is returned; a
// caller that must distinguish that from a legitimate 0 checks
// aether_intmap_has afterwards.
long long aether_intmap_add(AetherIntMap* map, long long key, long long delta);

// 1 if the key was present and is now removed, 0 if it was absent.
int aether_intmap_remove(AetherIntMap* map, long long key);

// Number of live entries; 0 for a null map.
int aether_intmap_size(AetherIntMap* map);

// Drops every entry but keeps the map usable.
void aether_intmap_clear(AetherIntMap* map);

void aether_intmap_free(AetherIntMap* map);

// Snapshots of the live keys, and of their values in the same order, as
// std.longarr buffers the caller releases with longarr_free. Order is slot
// order and unspecified; the two snapshots line up only while the map is
// not mutated between the calls. NULL on a null map or allocation failure.
LongArray* aether_intmap_keys(AetherIntMap* map);
LongArray* aether_intmap_values(AetherIntMap* map);

// Slots the table currently has room for before it grows: a test hook for
// the resize path, not part of the Aether-facing API.
int aether_intmap_capacity(AetherIntMap* map);

#endif // AETHER_INTMAP_H
