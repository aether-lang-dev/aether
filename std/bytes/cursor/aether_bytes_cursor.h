/* Copyright (c) 2026 Aether Developers. */
#ifndef AETHER_BYTES_CURSOR_H
#define AETHER_BYTES_CURSOR_H

#include "../aether_bytes.h"

/* std.bytes.cursor — a forward read-position over an existing
 * AetherBytes buffer.
 *
 * Binary parsers (cryptography wire formats, codec headers) want to walk a
 * buffer left-to-right pulling fixed-width big-endian fields and
 * length-prefixed slices, tracking a position and bounds-checking each
 * read. A cursor packages that: it BORROWS the AetherBytes (does not
 * own it) and tracks a read offset.
 *
 * BORROW SEMANTICS: the cursor must NOT outlive its AetherBytes.
 * bytes_cursor_free reclaims only the cursor struct, never the
 * underlying buffer. Free the buffer separately (and after the
 * cursor). All reads go through the public aether_bytes_get accessor,
 * so the cursor respects the buffer's encapsulation and its logical
 * length. */

typedef struct BytesCursor BytesCursor;

/* Create a cursor positioned at offset 0 over `b`. Returns NULL if `b`
 * is NULL or allocation fails. The cursor borrows `b`. */
BytesCursor* bytes_cursor_new(AetherBytes* b);

/* A cursor over `len` bytes of raw memory: the backing of a `byte[]` slice
 * (#2301). Borrows `data` exactly as bytes_cursor_new borrows its buffer.
 * NULL for a negative length or NULL data with a positive length. */
BytesCursor* bytes_cursor_new_from_ptr(const void* data, int len);

/* The address of the next `n` bytes, advancing past them, without copying:
 * a borrowed view (#2301). NULL (cursor unchanged) when fewer than `n`
 * remain or `n` is negative. For a buffer-backed cursor the address is into
 * the buffer's storage and is valid until the buffer grows or is freed. */
const void* bytes_cursor_read_view(BytesCursor* c, int n);

/* Read one byte (0..255) and advance by 1. Returns -1 at EOF (cursor
 * left unchanged). */
int bytes_cursor_read_u8(BytesCursor* c);

/* Read a big-endian 16/32-bit value and advance by 2/4. Returns -1 if
 * fewer than that many bytes remain (cursor left unchanged). The 32-bit
 * read returns the value's bit pattern in an `int` (top bit becomes the
 * sign bit — same convention as aether_bytes_get_be32); callers needing
 * the unsigned interpretation use std.bits.lsr32 / udiv32 / etc. */
int bytes_cursor_read_be_u16(BytesCursor* c);
int bytes_cursor_read_be_u32(BytesCursor* c);

/* Read a big-endian 64-bit value and advance by 8. Returns -1 if fewer
 * than 8 bytes remain (cursor left unchanged). */
long long bytes_cursor_read_be_u64(BytesCursor* c);

/* Little-endian counterparts of the big-endian readers above: read a
 * 16/32/64-bit value least-significant-byte-first and advance by 2/4/8.
 * Same contract, returns -1 (cursor unchanged) when fewer than that many
 * bytes remain. The 32-bit read returns the bit pattern in an `int`
 * (top bit becomes the sign bit, as with aether_bytes_get_le32). For
 * little-endian on-disk formats these let a validation loop walk the
 * buffer with bounds-checked cursor reads instead of manual indexing. */
int bytes_cursor_read_le_u16(BytesCursor* c);
int bytes_cursor_read_le_u32(BytesCursor* c);
long long bytes_cursor_read_le_u64(BytesCursor* c);

/* Read the next `n` bytes into a freshly-allocated AetherBytes and
 * advance by `n`. Returns NULL if fewer than `n` bytes remain (cursor
 * left unchanged), `n` is negative, or allocation fails. The caller
 * owns the returned buffer and must free it. */
AetherBytes* bytes_cursor_read_slice(BytesCursor* c, int n);

/* Bytes left to read: len - pos (>= 0). 0 if `c` is NULL. */
int bytes_cursor_remaining(BytesCursor* c);

/* Next byte (0..255) without advancing. Returns -1 at EOF. */
int bytes_cursor_peek(BytesCursor* c);

/* 1 if the cursor is at or past the end, else 0. (1 if `c` is NULL.) */
int bytes_cursor_eof(BytesCursor* c);

/* Current read offset. -1 if `c` is NULL. */
int bytes_cursor_pos(BytesCursor* c);

/* Move the read offset to `pos`, clamped to [0, len]. No-op if `c` is
 * NULL. */
void bytes_cursor_seek(BytesCursor* c, int pos);

/* Free the cursor struct. Does NOT free the borrowed AetherBytes.
 * Idempotent on NULL. */
void bytes_cursor_free(BytesCursor* c);

#endif
