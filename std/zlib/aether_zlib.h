/* std.zlib — one-shot zlib deflate/inflate.
 *
 * v1 exposes two pure functions that operate on in-memory byte
 * buffers. Streaming is deliberately out of scope (separate future
 * API). gzip-framed variants use the same split-accessor shape.
 *
 * The Aether boundary uses the split-accessor pattern (same as
 * fs.read_binary): a `try_` entry point performs the operation and
 * stashes the result in a thread-local buffer; `get_bytes` /
 * `get_length` read it borrowed; `release` frees the buffer early
 * (otherwise it's freed on the next `try_` call on the same thread).
 *
 * This avoids passing C out-parameters through the Aether calling
 * convention, and keeps the output binary-safe — the Aether wrapper
 * in module.ae copies the buffer into a length-aware AetherString
 * via string_new_with_length, preserving embedded NULs.
 *
 * When built without zlib (AETHER_HAS_ZLIB undefined — e.g. bare
 * embedded), `try_` returns 0 so the Go-style wrappers report
 * "zlib unavailable" cleanly.
 */

#ifndef AETHER_ZLIB_H
#define AETHER_ZLIB_H

/* Returns 1 when the toolchain was built with AETHER_HAS_ZLIB set
 * (zlib detected at build time), 0 otherwise. Callers use this to
 * distinguish "no backend available" from "backend returned error
 * at runtime", because the two failure modes warrant different
 * user-facing messages — and in tests, different SKIP vs. FAIL
 * decisions. Named `zlib_backend_available` rather than
 * `zlib_available` so it doesn't collide with the Aether-side
 * `zlib.available()` wrapper's mangled name (`zlib_available`). */
int zlib_backend_available(void);

/* Deflate: compress `length` bytes of `data` at `level` (0..9; -1
 * for default). Returns 1 on success, 0 on failure. On success the
 * result is in the TLS buffer exposed by zlib_get_deflate_bytes /
 * zlib_get_deflate_length. `data` may be an AetherString* or a
 * plain char*. length=0 is legal (produces a valid empty-stream
 * output, ~8 bytes). */
int zlib_try_deflate(const char* data, int length, int level);
const char* zlib_get_deflate_bytes(void);
int         zlib_get_deflate_length(void);
void        zlib_release_deflate(void);

/* Inflate: decompress `length` bytes of `data`. Returns 1 on
 * success, 0 on corruption / truncation / empty-input / allocation
 * failure. The output buffer grows geometrically — callers don't
 * need to know the decompressed size in advance. */
int zlib_try_inflate(const char* data, int length);
const char* zlib_get_inflate_bytes(void);
int         zlib_get_inflate_length(void);
void        zlib_release_inflate(void);

/* gzip-framed siblings (RFC 1952), intended for HTTP
 * Content-Encoding: gzip. They share the same TLS output slots as
 * deflate/inflate respectively, so the same get/release accessors
 * apply after a successful call. */
int zlib_try_gzip_deflate(const char* data, int length, int level);
int zlib_try_gzip_inflate(const char* data, int length);

/* ---- Streaming deflate (#1890) ---------------------------------
 *
 * The one-shot calls above run deflateInit2 -> deflate(Z_FINISH) ->
 * deflateEnd inside a single call, so each one necessarily emits a
 * COMPLETE stream. That is wrong for a long-lived response: an SSE
 * connection carrying many small events needs ONE deflate stream held
 * open for the life of the connection, flushed at each event boundary,
 * so the client sees a single continuous stream. Compressing each event
 * independently produces N complete streams concatenated, which no
 * `Content-Encoding: gzip` client will decode.
 *
 * The handle owns its own output buffer rather than sharing the TLS
 * slots above: two streams may be open on one thread (two SSE
 * connections served by one event loop), and TLS slots would let them
 * overwrite each other's pending bytes.
 *
 * Lifecycle mirrors std.cryptography's streaming hashes:
 *   s = stream_new(format, level)
 *   stream_write(s, data, len)      -- buffer input, may emit nothing
 *   stream_flush(s)                 -- Z_SYNC_FLUSH: emit a decodable
 *                                      boundary, KEEP the window
 *   stream_finish(s)                -- Z_FINISH: emit the tail
 *   stream_free(s)
 *
 * After any of write/flush/finish returning 1, the bytes produced by
 * THAT call are read with stream_get_bytes / stream_get_length. They
 * stay valid until the next call on the same handle.
 *
 * `format`: 0 = raw deflate, 1 = zlib wrapper, 2 = gzip wrapper. These
 * match the windowBits selection the one-shot calls already use. */
#define AETHER_ZLIB_FORMAT_RAW  0
#define AETHER_ZLIB_FORMAT_ZLIB 1
#define AETHER_ZLIB_FORMAT_GZIP 2

/* Returns an opaque handle, or NULL when zlib is absent, the format or
 * level is out of range, or allocation fails. */
void* zlib_try_stream_new(int format, int level);

/* Feed `length` bytes. Returns 1 on success (possibly emitting nothing
 * -- deflate buffers internally), 0 on error. */
int zlib_try_stream_write(void* handle, const char* data, int length);

/* Z_SYNC_FLUSH: emit everything buffered so far and end on a byte
 * boundary the decoder can consume, WITHOUT ending the stream. This is
 * the call that makes the whole thing work. Returns 1 on success. */
int zlib_try_stream_flush(void* handle);

/* Z_FINISH: emit the tail (and the gzip/zlib trailer). The handle
 * accepts no further writes afterwards. Returns 1 on success. */
int zlib_try_stream_finish(void* handle);

/* Bytes produced by the most recent write/flush/finish on this handle.
 * Borrowed; valid until the next call on the same handle. */
const char* zlib_get_stream_bytes(void* handle);
int         zlib_get_stream_length(void* handle);

void zlib_release_stream(void* handle);

#endif /* AETHER_ZLIB_H */
