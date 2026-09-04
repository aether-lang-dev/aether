#include "aether_zstd.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_ZSTD
#include <zstd.h>
#endif

/* Unwrap a `data` argument that may be an AetherString* or a plain char*.
 * Same helper shape as std/zlib, std/brotli and std/fs -- without this
 * dispatch a length-aware AetherString leaks its struct header into the
 * stream. */
static inline const unsigned char* zstd_unwrap_bytes(const char* data, int length,
                                                     size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (length >= 0) ? (size_t)length : s->length;
        return (const unsigned char*)s->data;
    }
    *out_len = (length >= 0) ? (size_t)length : strlen(data);
    return (const unsigned char*)data;
}

/* One-shot output slot, thread-local like std.zlib's. Streaming output lives
 * on the handle; see the header. */
static _Thread_local unsigned char* tls_comp_buf = NULL;
static _Thread_local size_t         tls_comp_cap = 0;
static _Thread_local int            tls_comp_len = 0;

static void free_comp_tls(void) {
    if (tls_comp_buf) {
        aether_caps_free(tls_comp_buf, tls_comp_cap);
        tls_comp_buf = NULL;
    }
    tls_comp_cap = 0;
    tls_comp_len = 0;
}

int zstd_backend_available(void) {
#ifdef AETHER_HAS_ZSTD
    return 1;
#else
    return 0;
#endif
}

#ifdef AETHER_HAS_ZSTD

typedef struct {
    ZSTD_CCtx* cctx;
    unsigned char* out;     /* owned; bytes of the most recent call */
    size_t out_cap;         /* allocator's view, for aether_caps_free */
    int    out_len;         /* payload's view */
    int    finished;
    int    broken;
} ZstdStream;

static int clamp_level(int level) {
    int lo = ZSTD_minCLevel();
    int hi = ZSTD_maxCLevel();
    if (level < lo || level > hi) return ZSTD_defaultCLevel();
    return level;
}

/* Run ZSTD_compressStream2 with `op` until the input is consumed and zstd
 * reports nothing left to flush.
 *
 * The termination condition is zstd's own and is a THIRD shape, different from
 * both siblings: zlib stops when it left avail_out spare, brotli when
 * BrotliEncoderHasMoreOutput goes false, and zstd when the call RETURNS 0 --
 * "@return provides a minimum amount of data remaining to be flushed", so
 * nonzero means go round again. Reusing either sibling's condition here would
 * stop early and truncate the frame. */
static int zstd_pump(ZstdStream* z, ZSTD_EndDirective op,
                     const unsigned char* in, size_t in_len) {
    if (!z || z->broken) return 0;

    ZSTD_inBuffer inb = { in, in_len, 0 };
    size_t used = 0;

    if (z->out_cap < 256) {
        size_t cap = ZSTD_CStreamOutSize();
        if (cap < 256) cap = 256;
        unsigned char* nb = (unsigned char*)aether_caps_malloc(cap);
        if (!nb) { z->broken = 1; return 0; }
        if (z->out) aether_caps_free(z->out, z->out_cap);
        z->out = nb;
        z->out_cap = cap;
    }

    for (;;) {
        ZSTD_outBuffer outb = { z->out, z->out_cap, used };
        size_t remaining = ZSTD_compressStream2(z->cctx, &outb, &inb, op);
        if (ZSTD_isError(remaining)) { z->broken = 1; return 0; }
        used = outb.pos;

        /* Done when the input is drained and zstd says nothing is pending. */
        if (inb.pos == inb.size && remaining == 0) break;

        /* Only grow when the buffer is actually full; otherwise zstd simply
         * wants another pass with the room it already has. */
        if (outb.pos == outb.size) {
            size_t ncap = z->out_cap * 2;
            if (ncap > (size_t)1 << 30) { z->broken = 1; return 0; }
            unsigned char* nb = (unsigned char*)aether_caps_malloc(ncap);
            if (!nb) { z->broken = 1; return 0; }
            memcpy(nb, z->out, used);
            aether_caps_free(z->out, z->out_cap);
            z->out = nb;
            z->out_cap = ncap;
        }
    }

    z->out_len = (int)used;
    return 1;
}

void* zstd_try_stream_new(int level) {
    ZstdStream* z = (ZstdStream*)aether_caps_calloc(1, sizeof(ZstdStream));
    if (!z) return NULL;
    z->cctx = ZSTD_createCCtx();
    if (!z->cctx) {
        aether_caps_free(z, sizeof(ZstdStream));
        return NULL;
    }
    ZSTD_CCtx_setParameter(z->cctx, ZSTD_c_compressionLevel, clamp_level(level));
    return z;
}

int zstd_try_stream_write(void* handle, const char* data, int length) {
    ZstdStream* z = (ZstdStream*)handle;
    if (!z || z->finished || z->broken || length < 0) return 0;
    size_t in_len;
    const unsigned char* in = zstd_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;
    return zstd_pump(z, ZSTD_e_continue, in, in_len);
}

int zstd_try_stream_flush(void* handle) {
    ZstdStream* z = (ZstdStream*)handle;
    if (!z || z->finished || z->broken) return 0;
    return zstd_pump(z, ZSTD_e_flush, NULL, 0);
}

int zstd_try_stream_finish(void* handle) {
    ZstdStream* z = (ZstdStream*)handle;
    if (!z || z->broken) return 0;
    if (z->finished) { z->out_len = 0; return 1; }   /* idempotent */
    if (!zstd_pump(z, ZSTD_e_end, NULL, 0)) return 0;
    z->finished = 1;
    return 1;
}

const char* zstd_get_stream_bytes(void* handle) {
    ZstdStream* z = (ZstdStream*)handle;
    if (!z || !z->out) return "";
    return (const char*)z->out;
}

int zstd_get_stream_length(void* handle) {
    ZstdStream* z = (ZstdStream*)handle;
    return z ? z->out_len : 0;
}

void zstd_release_stream(void* handle) {
    ZstdStream* z = (ZstdStream*)handle;
    if (!z) return;
    if (z->cctx) ZSTD_freeCCtx(z->cctx);
    if (z->out) aether_caps_free(z->out, z->out_cap);
    aether_caps_free(z, sizeof(ZstdStream));
}

int zstd_try_compress(const char* data, int length, int level) {
    free_comp_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zstd_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    size_t bound = ZSTD_compressBound(in_len);
    if (ZSTD_isError(bound) || bound == 0) return 0;
    unsigned char* out = (unsigned char*)aether_caps_malloc(bound);
    if (!out) return 0;

    size_t n = ZSTD_compress(out, bound, in, in_len, clamp_level(level));
    if (ZSTD_isError(n)) { aether_caps_free(out, bound); return 0; }

    tls_comp_buf = out;
    tls_comp_cap = bound;
    tls_comp_len = (int)n;
    return 1;
}

#else /* !AETHER_HAS_ZSTD */

/* stream_new returning NULL is the signal a caller cannot ignore: every other
 * stream entry point needs a handle, so a program built without zstd fails
 * where it ASKS for a stream rather than silently emitting nothing. (base64 in
 * #1884 returned empty from its no-backend branch and callers shipped broken
 * output for it.) */
void* zstd_try_stream_new(int level) { (void)level; return NULL; }
int zstd_try_stream_write(void* handle, const char* data, int length) {
    (void)handle; (void)data; (void)length; return 0;
}
int zstd_try_stream_flush(void* handle)  { (void)handle; return 0; }
int zstd_try_stream_finish(void* handle) { (void)handle; return 0; }
const char* zstd_get_stream_bytes(void* handle) { (void)handle; return ""; }
int zstd_get_stream_length(void* handle) { (void)handle; return 0; }
void zstd_release_stream(void* handle) { (void)handle; }

int zstd_try_compress(const char* data, int length, int level) {
    (void)data; (void)length; (void)level; return 0;
}

#endif /* AETHER_HAS_ZSTD */

const char* zstd_get_compress_bytes(void) {
    return (const char*)(tls_comp_buf ? tls_comp_buf : (unsigned char*)"");
}
int  zstd_get_compress_length(void) { return tls_comp_len; }
void zstd_release_compress(void)    { free_comp_tls(); }
