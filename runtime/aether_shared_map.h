#ifndef AETHER_SHARED_MAP_H
#define AETHER_SHARED_MAP_H

#include <stddef.h>
#include <stdint.h>

// The shared map: a token-guarded map of byte strings, for a host to hand a
// hosted language (a guest bridge, a sandboxed child) its inputs and read
// back its outputs.
//
// Keys and values are bytes with a length: a NUL inside one is a byte like
// any other, never the end. The map copies what it stores. What a get or an
// *_at returns is the map's own copy, NUL-terminated after its length (so a
// value with no NUL in it can be used as a C string), valid until that key is
// put again or the map is freed; never free it. A NULL value is stored as an
// empty one.
//
// From Aether, std.sharedmap wraps this with `string` keys and values.

// Opaque shared map handle
typedef struct AetherSharedMap AetherSharedMap;

// At most this many keys in one map; a put of a new key past it is refused.
#define AETHER_SHARED_MAP_MAX_ENTRIES 256

// Create a new shared map. Returns a token for hosted language access.
AetherSharedMap* aether_shared_map_new(uint64_t* out_token);
// The map's token (0 for NULL).
uint64_t aether_shared_map_token(AetherSharedMap* map);

// Aether-side: put/get (no token needed: the caller owns the map).
// put: 0 stored; -1 refused (no map, a NULL key, the map full, no memory),
// with the map unchanged.
int aether_shared_map_put(AetherSharedMap* map, const char* key, size_t key_len,
                          const char* value, size_t value_len);
// The value stored under key, its length in *value_len (may be NULL); NULL
// when the key is absent.
const char* aether_shared_map_get(AetherSharedMap* map, const char* key, size_t key_len,
                                  size_t* value_len);
// The index of key (for *_at), or -1.
int aether_shared_map_find(AetherSharedMap* map, const char* key, size_t key_len);
void aether_shared_map_free(AetherSharedMap* map);

// Hosted-language-side: get inputs and put outputs via token.
// Phase 1 (during hosted execution): get = reads inputs, put = writes to output area
// After revoke: token is dead, no access.
const char* aether_shared_map_get_by_token(uint64_t token, const char* key, size_t key_len,
                                           size_t* value_len);
// 0 stored; -1 refused: a dead token, a frozen input key, the map full.
int aether_shared_map_put_by_token(uint64_t token, const char* key, size_t key_len,
                                   const char* value, size_t value_len);

// Invalidate a token (called after hosted code returns)
void aether_shared_map_revoke_token(uint64_t token);

// Freeze inputs: after this, hosted code can read inputs but not overwrite them.
void aether_shared_map_freeze_inputs(AetherSharedMap* map);
void aether_shared_map_freeze_inputs_by_token(uint64_t token);

// Iteration (for injecting map contents into hosted language hashes): the
// key or value at index i and its length in *len (may be NULL); NULL out of
// range. The *_len_at forms give the length alone, -1 out of range.
int aether_shared_map_count(AetherSharedMap* map);
const char* aether_shared_map_key_at(AetherSharedMap* map, int index, size_t* len);
const char* aether_shared_map_value_at(AetherSharedMap* map, int index, size_t* len);
long aether_shared_map_key_len_at(AetherSharedMap* map, int index);
long aether_shared_map_value_len_at(AetherSharedMap* map, int index);

// Token-based iteration
int aether_shared_map_count_by_token(uint64_t token);
const char* aether_shared_map_key_at_by_token(uint64_t token, int index, size_t* len);
const char* aether_shared_map_value_at_by_token(uint64_t token, int index, size_t* len);

// ---- Cross-process: the map as a POSIX shared memory block ----------------
//
// Layout 2, every integer a little-endian uint32:
//
//   offset  size  field
//   0       4     magic, the bytes 'A' 'E' 'S' 'M'
//   4       4     0, always: a reader of layout 1 (key\0value\0...\0\0 after
//                 a 4-byte count) stops here, at an empty map, instead of
//                 reading lengths as text
//   8       4     layout version, 2
//   12      4     frozen_count: entries 0..frozen_count-1 are the host's inputs
//   16      4     entry_count
//   20      ...   entry_count entries, each: key_len, value_len, then the
//                 key's bytes, then the value's (no terminators, no padding)
//
// The block's size is what its entries need; a reader ignores zero bytes
// after the last entry (macOS rounds a shm object up to a page) and refuses
// any other trailing byte. A block whose magic, zero word or version is not
// this, or whose entries run past its end, is refused whole: nothing of it
// is read.
#define AETHER_SHARED_MAP_SHM_MAGIC "AESM"
#define AETHER_SHARED_MAP_SHM_VERSION 2
#define AETHER_SHARED_MAP_SHM_HEADER 20
// The largest block to_shm writes (as layout 1's 64 KiB buffer did) and
// read_outputs_from_shm reads (a guest's outputs may outgrow its inputs).
#define AETHER_SHARED_MAP_SHM_MAX_WRITE 65536
#define AETHER_SHARED_MAP_SHM_MAX_READ (64 * 1024 * 1024)

// Write the map to a new shm block; its name (the caller frees it, after
// aether_shared_map_unlink_shm). NULL when it cannot: the map is larger than
// AETHER_SHARED_MAP_SHM_MAX_WRITE (errno E2BIG), or shm_open, ftruncate or
// mmap failed (their errno), or this platform has no POSIX shared memory
// block here (Windows, Android, the BSDs: errno ENOSYS).
char* aether_shared_map_to_shm(AetherSharedMap* map);
// Read the outputs a hosted process wrote to the block back into the map:
// every entry whose key is not one of the map's own frozen inputs (an input
// is never overwritten, whatever the block says). 0 read; -1 refused (no
// such block, or not a layout-2 block, or malformed, or no memory): the map
// is unchanged.
int aether_shared_map_read_outputs_from_shm(AetherSharedMap* map, const char* shm_name);
// Clean up shm
void aether_shared_map_unlink_shm(const char* shm_name);

// Token-based cross-process serialization (convenience wrappers)
char* aether_shared_map_to_shm_by_token(uint64_t token);
int aether_shared_map_read_outputs_from_shm_by_token(uint64_t token, const char* shm_name);

// The block's bytes for a map (malloc'd, *n its size), and a map's outputs
// read from such bytes: the format without the shm, for tests and for a
// platform that carries the block some other way. NULL / -1 as above.
unsigned char* aether_shared_map_encode(AetherSharedMap* map, size_t* n);
int aether_shared_map_decode_outputs(AetherSharedMap* map, const unsigned char* data, size_t n);

#endif
