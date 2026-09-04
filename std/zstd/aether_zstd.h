/* std.zstd — Zstandard compression (RFC 8878), streaming and one-shot.
 *
 * Zstandard is a different FORMAT from DEFLATE, not a faster implementation of
 * it: this module wraps libzstd and has nothing to do with std.zlib beyond the
 * similar library name. Its case is strongest away from the browser --
 * archives, logs, snapshots, internal RPC -- because `Content-Encoding: zstd`
 * support is still thin compared with `br` and `gzip`. It compresses harder
 * than gzip at comparable speed, and decompresses faster than both.
 *
 * The surface mirrors std.zlib's and std.brotli's streaming shape, so a caller
 * choosing an encoding writes the same code whichever it picks:
 *
 *   s = stream_new(level)
 *   stream_write(s, data, len)   -- buffer input, may emit nothing
 *   stream_flush(s)              -- emit a decodable boundary, KEEP the window
 *   stream_finish(s)             -- close the frame
 *   stream_free(s)
 *
 * As there, the handle owns its output buffer rather than sharing a
 * thread-local slot, so two streams may be open on one thread.
 *
 * Without libzstd (AETHER_HAS_ZSTD undefined) zstd_try_stream_new returns NULL
 * and the one-shot try_ entry points return 0, so a caller sees "zstd
 * unavailable" rather than silently shipping empty output.
 */

#ifndef AETHER_ZSTD_H
#define AETHER_ZSTD_H

int zstd_backend_available(void);

/* `level` is 1..22 (libzstd's own range; 3 is its default). Values outside it
 * are clamped rather than rejected, matching std.zlib's treatment of `level`
 * and std.brotli's of `quality`. */
void* zstd_try_stream_new(int level);
int   zstd_try_stream_write(void* handle, const char* data, int length);
int   zstd_try_stream_flush(void* handle);
int   zstd_try_stream_finish(void* handle);

/* Bytes produced by the most recent write/flush/finish on this handle.
 * Borrowed; valid until the next call on the same handle. */
const char* zstd_get_stream_bytes(void* handle);
int         zstd_get_stream_length(void* handle);
void        zstd_release_stream(void* handle);

/* One-shot, for a body already in memory. Split-accessor shape, as std.zlib. */
int         zstd_try_compress(const char* data, int length, int level);
const char* zstd_get_compress_bytes(void);
int         zstd_get_compress_length(void);
void        zstd_release_compress(void);

#endif /* AETHER_ZSTD_H */
