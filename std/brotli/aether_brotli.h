/* std.brotli — Brotli compression (RFC 7932), streaming and one-shot.
 *
 * Brotli is what browsers actually prefer over gzip today: `Accept-Encoding`
 * lists `br` ahead of `gzip` in every current browser, and it typically beats
 * gzip by 15-25% on text at comparable speed. This module wraps the system
 * libbrotlienc (apt: libbrotli-dev, brew: brotli) rather than vendoring an
 * implementation — see the note at the foot of this header.
 *
 * The surface deliberately mirrors std.zlib's streaming deflate (#1890), so a
 * caller negotiating `br` vs `gzip` writes the same shape either way:
 *
 *   s = stream_new(quality, window)
 *   stream_write(s, data, len)   -- buffer input, may emit nothing
 *   stream_flush(s)              -- emit a decodable boundary, KEEP the window
 *   stream_finish(s)             -- emit the tail
 *   stream_free(s)
 *
 * As with zlib, the handle owns its output buffer rather than sharing a
 * thread-local slot: two streams may be open on one thread (two SSE
 * connections on one event loop) and TLS slots would let them overwrite each
 * other's pending bytes.
 *
 * When built without libbrotlienc (AETHER_HAS_BROTLI undefined),
 * brotli_try_stream_new returns NULL and the one-shot try_ entry points
 * return 0, so a caller sees "brotli unavailable" rather than silently
 * shipping empty output.
 */

#ifndef AETHER_BROTLI_H
#define AETHER_BROTLI_H

/* 1 when the toolchain was built with libbrotlienc detected. Callers use it
 * to distinguish "no backend" from "backend failed", and tests to choose SKIP
 * over FAIL. Named _backend_available so it doesn't collide with the
 * Aether-side `brotli.available()` wrapper's mangled name. */
int brotli_backend_available(void);

/* Quality is 0..11 (libbrotli's own range; 11 is the default and is slow —
 * 4..6 is the usual choice for a live HTTP response). `window` is the lg2
 * window size, 10..24; pass 0 for the library default. Out-of-range values
 * are clamped rather than rejected, matching how std.zlib treats `level`. */
void* brotli_try_stream_new(int quality, int window);
int   brotli_try_stream_write(void* handle, const char* data, int length);
int   brotli_try_stream_flush(void* handle);
int   brotli_try_stream_finish(void* handle);

/* Bytes produced by the most recent write/flush/finish on this handle.
 * Borrowed; valid until the next call on the same handle. */
const char* brotli_get_stream_bytes(void* handle);
int         brotli_get_stream_length(void* handle);
void        brotli_release_stream(void* handle);

/* One-shot compression, for a body already in memory. Uses the same
 * split-accessor shape as std.zlib: try_ performs the work and stashes the
 * result, get_ reads it borrowed, release_ frees early. */
int         brotli_try_compress(const char* data, int length, int quality);
const char* brotli_get_compress_bytes(void);
int         brotli_get_compress_length(void);
void        brotli_release_compress(void);

/* Why a system library rather than a vendored implementation:
 * libbrotlienc ships in every mainstream distro (apt libbrotli-dev, brew
 * brotli) and exposes exactly the streaming encoder this needs
 * (BrotliEncoderCompressStream with BROTLI_OPERATION_FLUSH). The reference
 * implementation carries a mandatory ~120k-line static dictionary that is
 * part of the FORMAT, so vendoring means carrying that whichever route is
 * taken; binding the system library avoids transliterating ~17k lines of
 * bit-exact entropy coding as well. Decompression is deliberately absent for
 * now: an HTTP server compresses responses, and nothing in-tree needs to
 * decode br. */

#endif /* AETHER_BROTLI_H */
