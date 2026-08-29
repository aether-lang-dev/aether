#ifndef AETHER_BYTES_H
#define AETHER_BYTES_H

#include <stddef.h>

/* std.bytes — mutable byte buffer with random-access write and
 * overlap-safe forward copy_within.
 *
 * Aether's `string` is immutable; every concat allocates fresh. This
 * module fills the gap any binary-codec / streaming-buffer workload
 * needs: a buffer where you can set a byte at an arbitrary index and
 * read bytes that were written earlier in the same loop iteration.
 *
 * The canonical motivating case is the RLE overlap trick used by
 * svndiff and similar codecs:
 *
 *     for (size_t i = 0; i < length; i++) {
 *         dst[tpos + i] = dst[offset + i];   // offset < tpos
 *     }
 *
 * The action-1 instruction reads from a position that may itself
 * have just been written by a previous iteration of the same
 * instruction. memmove() is wrong here — it deliberately handles
 * overlap by choosing the safe direction; we want the unsafe forward
 * direction so byte i sees the freshly-written byte (i - run_length).
 *
 * Lifecycle: bytes.new() / set / copy_* / length, then either
 * bytes.finish(b, n) (hand off to a refcounted AetherString,
 * destroying the buffer) or bytes.free(b) (discard without
 * finishing). Issue #288.
 */

typedef struct AetherBytes AetherBytes;

/* Allocate a new buffer with at least `initial_capacity` bytes
 * reserved. Returns NULL on allocation failure or negative capacity.
 * Initial length is 0. */
AetherBytes* aether_bytes_new(int initial_capacity);

/* One-call ptr -> bytes crossing: a fresh AetherBytes copying `length` bytes
 * from `src`, with its logical length set. NULL on negative length / OOM. */
AetherBytes* aether_bytes_from_ptr(void* src, int length);

/* One-call ptr -> string crossing: a fresh refcounted AetherString copying
 * `length` bytes from `src`. NULL on negative length. */
void* aether_bytes_string_from_ptr(void* src, int length);

/* Number of bytes the buffer logically contains. -1 if `b` is NULL. */
int aether_bytes_length(AetherBytes* b);

/* Reserved capacity in bytes (always >= length). -1 if `b` is NULL.
 * Distinct from length: a buffer created with `aether_bytes_new(512)`
 * has capacity 512 and length 0. */
int aether_bytes_capacity(AetherBytes* b);

/* Pointer to the buffer's mutable storage, for zero-copy I/O: a caller
 * can `read(2)`/`pread(2)` directly into the reserved region (up to
 * `capacity` bytes) and then publish the byte count with
 * `aether_bytes_set_length`, avoiding a read-into-temp-then-copy. NULL if
 * `b` is NULL or its capacity is 0. BORROWED and short-lived: the pointer
 * is invalidated by any call that may reallocate the buffer (`set`,
 * `copy_from_*`) and by `aether_bytes_free` / `aether_bytes_finish`. */
void* aether_bytes_data(AetherBytes* b);

/* Set the buffer's logical length, clamped to [0, capacity]. Used to
 * publish how many bytes a direct write into `aether_bytes_data`'s region
 * actually produced. Returns the resulting length, or -1 if `b` is NULL. */
int aether_bytes_set_length(AetherBytes* b, int length);

/* Write a single byte at `index`. Grows the buffer if needed. The
 * logical length advances to max(length, index + 1) — gaps between
 * the previous tail and `index` are zero-filled. No-op if `b` is
 * NULL or `index` is negative. Returns 1 on success, 0 on failure. */
int aether_bytes_set(AetherBytes* b, int index, int byte);

/* Read a single byte at `index`. Returns the byte as an unsigned
 * value 0..255, or -1 if `b` is NULL, `index` is negative, or
 * `index` >= the buffer's logical length. Pairs with `set` so a
 * caller can build, then walk, a buffer in-place — the canonical
 * binary-codec encoder pattern. */
int aether_bytes_get(AetherBytes* b, int index);

/* Little-endian packed-int read/write helpers. The 16/32-bit
 * variants store / read consecutive bytes at `index .. index + N - 1`
 * in little-endian order. `set_*` grows the buffer if needed
 * (advancing logical length to cover the write); `get_*` returns -1
 * if any byte in the range is past the buffer's logical length, the
 * buffer is NULL, or `index` is negative. The 32-bit getters return
 * the value in an `int` (host-endian), so on 32-bit-int hosts the
 * stored value's top bit reads back as the sign bit — fine for byte-
 * packing use cases that just round-trip the value. */
int aether_bytes_set_le16(AetherBytes* b, int index, int value);
int aether_bytes_get_le16(AetherBytes* b, int index);
int aether_bytes_set_le32(AetherBytes* b, int index, int value);
int aether_bytes_get_le32(AetherBytes* b, int index);
int       aether_bytes_set_le64(AetherBytes* b, int index, long long value);
long long aether_bytes_get_le64(AetherBytes* b, int index);

/* Big-endian packed-int read/write helpers. Same bounds-check and
 * grow-on-write policy as the little-endian variants, but the bytes at
 * `index .. index + N - 1` are stored most-significant-first. The 32-bit
 * getter returns the value in an `int` (so the top bit reads back as the
 * sign bit — fine for round-tripping); the 64-bit getter returns a
 * `long long` and `set_be64` takes a `long long` value. `get_*` return
 * -1 if any byte in the range is past the buffer's logical length, the
 * buffer is NULL, or `index` is negative. */
int       aether_bytes_set_be16(AetherBytes* b, int index, int value);
int       aether_bytes_get_be16(AetherBytes* b, int index);
int       aether_bytes_set_be32(AetherBytes* b, int index, int value);
int       aether_bytes_get_be32(AetherBytes* b, int index);
int       aether_bytes_set_be64(AetherBytes* b, int index, long long value);
long long aether_bytes_get_be64(AetherBytes* b, int index);

/* Copy `src_len` bytes from `src` into the buffer starting at offset
 * `dst`. Grows the buffer if needed. `src` may be either a plain
 * `const char*` or an `AetherString*` (the function reads the payload
 * via aether_string_data). Returns 1 on success, 0 on failure
 * (NULL buffer / negative offsets / NULL src / OOM). */
int aether_bytes_copy_from_string(AetherBytes* b, int dst,
                                  const void* src, int src_len);

/* Copy `length` bytes from `src` buffer offset `src_off` into `dst`
 * buffer offset `dst_off`. The two buffers must be distinct; for an
 * in-place copy use `aether_bytes_copy_within` (which has the deliberate
 * forward-overlap semantics for RLE expansion). Grows the destination
 * buffer if `dst_off + length` exceeds capacity, zero-filling any gap
 * between the previous tail and `dst_off`.
 *
 * Returns 1 on success, 0 on failure (either buffer NULL, negative
 * offset/length, source range past `src->length`, or destination
 * grow OOM). Two-buffer copies are common in image-codec two-pass
 * algorithms (separable Gaussian: horizontal pass into temp buffer,
 * vertical pass from temp back into the output). */
int aether_bytes_copy_from_bytes(AetherBytes* dst, int dst_off,
                                 AetherBytes* src, int src_off,
                                 int length);

/* Copy `length` bytes from offset `src` to offset `dst` *within the
 * same buffer*, forward byte-by-byte. Bytes already written in this
 * call are visible to subsequent reads inside it — the deliberate
 * RLE-overlap behaviour. memmove() handles overlap differently and
 * would defeat the purpose. Grows the buffer if `dst + length`
 * exceeds capacity.
 *
 * Constraints:
 *   - `src` must point into the buffer's current logical length
 *     (need at least one valid byte to read).
 *   - When `dst > src` (the RLE case), `src + length` may extend
 *     past the current tail; the loop reads bytes it just wrote.
 *   - When `dst <= src`, the full source range must already exist
 *     (no overlap-from-future-bytes story).
 *
 * Returns 1 on success, 0 on failure (NULL buffer, negative offsets,
 * src out of range, or dst<=src with src+length > current length). */
int aether_bytes_copy_within(AetherBytes* b, int dst, int src, int length);

/* Hand off the buffer to a refcounted AetherString and destroy the
 * AetherBytes wrapper. After this call, `b` is invalid (do NOT free
 * it). The string carries the explicit length so embedded NULs
 * survive end-to-end. Returns NULL if `b` is NULL or `length` is
 * negative; clamps `length` to the buffer's logical length if it's
 * greater. */
void* aether_bytes_finish(AetherBytes* b, int length);

/* Non-consuming twin of aether_bytes_finish: mint a fresh refcounted
 * AetherString from the first `length` bytes WITHOUT destroying the
 * buffer. The caller keeps owning the AetherBytes and may call this
 * repeatedly. The string copies the data, so it is independent of `b`.
 * Returns NULL if `b` is NULL or `length` is negative; clamps `length`
 * to the buffer's logical length. */
void* aether_bytes_to_string(AetherBytes* b, int length);

/* Discard the buffer without converting to a string. Idempotent on
 * NULL. Use this when the caller decides mid-build that the buffer
 * isn't needed (error path, etc.). */
void aether_bytes_free(AetherBytes* b);

#endif
