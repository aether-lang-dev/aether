#include "aether_zlib.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_ZLIB
#include <zlib.h>
#endif

/* Unwrap the payload from a `data` argument that may be either an
 * AetherString* or a plain char*. Mirrors the helper in
 * std/fs/aether_fs.c and std/cryptography/aether_cryptography.c —
 * without this dispatch a length-aware AetherString from
 * fs.read_binary would leak its struct header into the stream. */
static inline const unsigned char* zlib_unwrap_bytes(const char* data, int length, size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (length >= 0) ? (size_t)length : s->length;
        return (const unsigned char*)s->data;
    }
    *out_len = (length >= 0) ? (size_t)length : strlen(data);
    return (const unsigned char*)data;
}

/* Thread-local buffers for the split-accessor pattern. One pair per
 * direction so a caller can interleave deflate + inflate without the
 * two fighting over a single slot.
 *
 * Resource-caps tracking (#343): both directions remember the
 * allocation capacity alongside the payload length so the matching
 * free goes through `aether_caps_free` with the right byte count.
 * The cap counter stays at current-usage rather than high-water-mark
 * — important for plugin-host sandboxes that re-arm the cap between
 * compressor invocations. Length and capacity differ when the chosen
 * `compressBound` over-allocated relative to the actual compressed
 * size; `_cap` holds the allocator's view (the count that must
 * round-trip to free), `_len` holds the payload's view. */
static _Thread_local unsigned char* tls_deflate_buf = NULL;
static _Thread_local size_t         tls_deflate_cap = 0;
static _Thread_local int            tls_deflate_len = 0;
static _Thread_local unsigned char* tls_inflate_buf = NULL;
static _Thread_local size_t         tls_inflate_cap = 0;
static _Thread_local int            tls_inflate_len = 0;

static void free_deflate_tls(void) {
    if (tls_deflate_buf) {
        aether_caps_free(tls_deflate_buf, tls_deflate_cap);
        tls_deflate_buf = NULL;
    }
    tls_deflate_cap = 0;
    tls_deflate_len = 0;
}
static void free_inflate_tls(void) {
    if (tls_inflate_buf) {
        aether_caps_free(tls_inflate_buf, tls_inflate_cap);
        tls_inflate_buf = NULL;
    }
    tls_inflate_cap = 0;
    tls_inflate_len = 0;
}

int zlib_backend_available(void) {
#ifdef AETHER_HAS_ZLIB
    return 1;
#else
    return 0;
#endif
}

#ifdef AETHER_HAS_ZLIB

int zlib_try_deflate(const char* data, int length, int level) {
    free_deflate_tls();  /* drop any leftover from a previous call */
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    uLongf bound = compressBound((uLong)in_len);
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) return 0;

    uLongf out_size = bound;
    int lvl = (level < -1 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    int rc = compress2(out, &out_size, in, (uLong)in_len, lvl);
    if (rc != Z_OK) { aether_caps_free(out, alloc_cap); return 0; }

    tls_deflate_buf = out;
    tls_deflate_cap = alloc_cap;
    tls_deflate_len = (int)out_size;
    return 1;
}

int zlib_try_gzip_deflate(const char* data, int length, int level) {
    free_deflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int lvl = (level < -1 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    if (deflateInit2(&strm, lvl, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return 0;
    }

    uLong bound = deflateBound(&strm, (uLong)in_len);
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) { deflateEnd(&strm); return 0; }

    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;

    int rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&strm);
        aether_caps_free(out, alloc_cap);
        return 0;
    }

    tls_deflate_buf = out;
    tls_deflate_cap = alloc_cap;
    tls_deflate_len = (int)strm.total_out;
    deflateEnd(&strm);
    return 1;
}

int zlib_try_inflate(const char* data, int length) {
    free_inflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len == 0) return 0;  /* empty isn't a valid zlib stream */
    if (!in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;

    if (inflateInit(&strm) != Z_OK) return 0;

    /* 4x the input as a first-cut output guess. Most deflate streams
     * fit in 2-8x their compressed size; grow geometrically if not. */
    size_t cap = in_len * 4;
    if (cap < 64) cap = 64;
    unsigned char* out = (unsigned char*)aether_caps_malloc(cap);
    if (!out) { inflateEnd(&strm); return 0; }

    size_t produced = 0;
    for (;;) {
        strm.next_out = out + produced;
        strm.avail_out = (uInt)(cap - produced);

        int rc = inflate(&strm, Z_NO_FLUSH);
        produced = cap - strm.avail_out;

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
        if (strm.avail_out == 0) {
            size_t new_cap = cap * 2;
            if (new_cap < cap) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            unsigned char* bigger = (unsigned char*)aether_caps_realloc(out, cap, new_cap);
            if (!bigger) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            out = bigger;
            cap = new_cap;
        }
    }
    inflateEnd(&strm);

    tls_inflate_buf = out;
    tls_inflate_cap = cap;
    tls_inflate_len = (int)produced;
    return 1;
}

static int inflate_with_window_bits(const char* data, int length, int window_bits) {
    free_inflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len == 0) return 0;
    if (!in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;

    if (inflateInit2(&strm, window_bits) != Z_OK) return 0;

    size_t cap = in_len * 4;
    if (cap < 64) cap = 64;
    unsigned char* out = (unsigned char*)aether_caps_malloc(cap);
    if (!out) { inflateEnd(&strm); return 0; }

    size_t produced = 0;
    for (;;) {
        strm.next_out = out + produced;
        strm.avail_out = (uInt)(cap - produced);

        int rc = inflate(&strm, Z_NO_FLUSH);
        produced = cap - strm.avail_out;

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
        if (strm.avail_out == 0) {
            size_t new_cap = cap * 2;
            if (new_cap < cap) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            unsigned char* bigger = (unsigned char*)aether_caps_realloc(out, cap, new_cap);
            if (!bigger) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            out = bigger;
            cap = new_cap;
        }
    }
    inflateEnd(&strm);

    tls_inflate_buf = out;
    tls_inflate_cap = cap;
    tls_inflate_len = (int)produced;
    return 1;
}

int zlib_try_gzip_inflate(const char* data, int length) {
    return inflate_with_window_bits(data, length, 15 + 16);
}

/* ---- Streaming deflate (#1890) ---------------------------------- */

typedef struct {
    z_stream strm;
    unsigned char* out;      /* owned; holds the bytes of the last call */
    size_t out_cap;          /* allocator's view, for aether_caps_free */
    int    out_len;          /* payload's view */
    int    finished;         /* Z_FINISH already emitted */
    int    broken;           /* a zlib error was seen; refuse further work */
} ZlibStream;

/* windowBits for the three framings, matching the one-shot calls:
 * negative = raw (no wrapper), 15 = zlib, 15+16 = gzip. */
static int zlib_window_bits(int format) {
    switch (format) {
        case AETHER_ZLIB_FORMAT_RAW:  return -15;
        case AETHER_ZLIB_FORMAT_ZLIB: return 15;
        case AETHER_ZLIB_FORMAT_GZIP: return 15 + 16;
        default: return 0;   /* caller rejects */
    }
}

/* Run one deflate() pass with `flush`, growing the output buffer until
 * zlib stops filling it.
 *
 * The growth loop is the substance of this file. Z_SYNC_FLUSH can emit
 * MORE than it consumed (a flush point costs ~5 bytes even with no
 * pending input, and incompressible input expands), and unlike the
 * one-shot path there is no deflateBound for a stream that stays open —
 * the bound depends on what is still in the window. So the only correct
 * termination condition is "zlib left room spare", i.e. avail_out > 0
 * after the call. Sizing by input length instead would silently truncate
 * on incompressible data, which is the bug this loop exists to avoid. */
static int zlib_stream_pump(ZlibStream* z, int flush) {
    if (!z || z->broken) return 0;

    size_t used = 0;
    if (z->out_cap < 256) {
        size_t cap = 256;
        unsigned char* nb = (unsigned char*)aether_caps_malloc(cap);
        if (!nb) { z->broken = 1; return 0; }
        if (z->out) aether_caps_free(z->out, z->out_cap);
        z->out = nb;
        z->out_cap = cap;
    }

    for (;;) {
        z->strm.next_out  = z->out + used;
        z->strm.avail_out = (uInt)(z->out_cap - used);
        uInt before = z->strm.avail_out;

        int rc = deflate(&z->strm, flush);
        /* Z_BUF_ERROR from deflate is "no progress possible", which is
         * not fatal on a flush with nothing pending; every other
         * non-OK code is. Z_STREAM_END is expected from Z_FINISH. */
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            z->broken = 1;
            return 0;
        }
        used += (size_t)(before - z->strm.avail_out);

        if (z->strm.avail_out > 0) break;   /* zlib had room spare: done */
        if (rc == Z_STREAM_END) break;

        /* Filled the buffer exactly — there may be more. Grow and retry. */
        size_t ncap = z->out_cap * 2;
        if (ncap > (size_t)1 << 30) { z->broken = 1; return 0; }
        unsigned char* nb = (unsigned char*)aether_caps_malloc(ncap);
        if (!nb) { z->broken = 1; return 0; }
        memcpy(nb, z->out, used);
        aether_caps_free(z->out, z->out_cap);
        z->out = nb;
        z->out_cap = ncap;
    }

    z->out_len = (int)used;
    return 1;
}

void* zlib_try_stream_new(int format, int level) {
    int wbits = zlib_window_bits(format);
    if (wbits == 0) return NULL;
    int lvl = (level < -1 || level > 9) ? Z_DEFAULT_COMPRESSION : level;

    ZlibStream* z = (ZlibStream*)aether_caps_calloc(1, sizeof(ZlibStream));
    if (!z) return NULL;
    if (deflateInit2(&z->strm, lvl, Z_DEFLATED, wbits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        aether_caps_free(z, sizeof(ZlibStream));
        return NULL;
    }
    return z;
}

int zlib_try_stream_write(void* handle, const char* data, int length) {
    ZlibStream* z = (ZlibStream*)handle;
    if (!z || z->finished || z->broken || length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    z->strm.next_in  = (Bytef*)in;
    z->strm.avail_in = (uInt)in_len;
    /* Z_NO_FLUSH: let zlib buffer for compression ratio. Emitting here
     * is allowed but not required, so out_len is often 0. */
    if (!zlib_stream_pump(z, Z_NO_FLUSH)) return 0;
    /* All input must have been consumed; the pump only stops when zlib
     * had output room spare, which implies it drained avail_in. */
    return z->strm.avail_in == 0;
}

int zlib_try_stream_flush(void* handle) {
    ZlibStream* z = (ZlibStream*)handle;
    if (!z || z->finished || z->broken) return 0;
    z->strm.next_in = NULL;
    z->strm.avail_in = 0;
    return zlib_stream_pump(z, Z_SYNC_FLUSH);
}

int zlib_try_stream_finish(void* handle) {
    ZlibStream* z = (ZlibStream*)handle;
    if (!z || z->broken) return 0;
    if (z->finished) { z->out_len = 0; return 1; }  /* idempotent */
    z->strm.next_in = NULL;
    z->strm.avail_in = 0;
    if (!zlib_stream_pump(z, Z_FINISH)) return 0;
    z->finished = 1;
    return 1;
}

const char* zlib_get_stream_bytes(void* handle) {
    ZlibStream* z = (ZlibStream*)handle;
    if (!z || !z->out) return "";
    return (const char*)z->out;
}

int zlib_get_stream_length(void* handle) {
    ZlibStream* z = (ZlibStream*)handle;
    return z ? z->out_len : 0;
}

void zlib_release_stream(void* handle) {
    ZlibStream* z = (ZlibStream*)handle;
    if (!z) return;
    deflateEnd(&z->strm);
    if (z->out) aether_caps_free(z->out, z->out_cap);
    aether_caps_free(z, sizeof(ZlibStream));
}

#else /* !AETHER_HAS_ZLIB */

int zlib_try_deflate(const char* data, int length, int level) {
    (void)data; (void)length; (void)level; return 0;
}
int zlib_try_inflate(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int zlib_try_gzip_deflate(const char* data, int length, int level) {
    (void)data; (void)length; (void)level; return 0;
}
int zlib_try_gzip_inflate(const char* data, int length) {
    (void)data; (void)length; return 0;
}

/* Streaming stubs. stream_new returns NULL, which is the one signal a
 * caller cannot ignore: every other entry point requires a handle, so a
 * program built without zlib fails at the point it asks for a stream
 * rather than silently emitting nothing. (base64 in #1884 returned empty
 * from its no-backend branch and callers shipped broken output for it --
 * a stub must be visibly unavailable, not quietly useless.) */
void* zlib_try_stream_new(int format, int level) {
    (void)format; (void)level; return NULL;
}
int zlib_try_stream_write(void* handle, const char* data, int length) {
    (void)handle; (void)data; (void)length; return 0;
}
int zlib_try_stream_flush(void* handle)  { (void)handle; return 0; }
int zlib_try_stream_finish(void* handle) { (void)handle; return 0; }
const char* zlib_get_stream_bytes(void* handle) { (void)handle; return ""; }
int zlib_get_stream_length(void* handle) { (void)handle; return 0; }
void zlib_release_stream(void* handle) { (void)handle; }

#endif /* AETHER_HAS_ZLIB */

const char* zlib_get_deflate_bytes(void) {
    return (const char*)(tls_deflate_buf ? tls_deflate_buf : (unsigned char*)"");
}
int zlib_get_deflate_length(void) { return tls_deflate_len; }
void zlib_release_deflate(void)   { free_deflate_tls(); }

const char* zlib_get_inflate_bytes(void) {
    return (const char*)(tls_inflate_buf ? tls_inflate_buf : (unsigned char*)"");
}
int zlib_get_inflate_length(void) { return tls_inflate_len; }
void zlib_release_inflate(void)   { free_inflate_tls(); }
