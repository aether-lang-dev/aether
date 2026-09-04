#include "aether_brotli.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_BROTLI
#include <brotli/encode.h>
#endif

/* Unwrap a `data` argument that may be an AetherString* or a plain char*.
 * Same helper shape as std/zlib and std/fs — without this dispatch a
 * length-aware AetherString would leak its struct header into the stream. */
static inline const unsigned char* brotli_unwrap_bytes(const char* data, int length,
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

/* One-shot output slot, thread-local like std.zlib's. The STREAMING output
 * lives on the handle instead; see the header. */
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

int brotli_backend_available(void) {
#ifdef AETHER_HAS_BROTLI
    return 1;
#else
    return 0;
#endif
}

#ifdef AETHER_HAS_BROTLI

typedef struct {
    BrotliEncoderState* enc;
    unsigned char* out;     /* owned; bytes of the most recent call */
    size_t out_cap;         /* allocator's view, for aether_caps_free */
    int    out_len;         /* payload's view */
    int    finished;
    int    broken;
} BrotliStream;

static int clamp_quality(int q) {
    if (q < BROTLI_MIN_QUALITY) return BROTLI_DEFAULT_QUALITY;
    if (q > BROTLI_MAX_QUALITY) return BROTLI_MAX_QUALITY;
    return q;
}

/* Run one BrotliEncoderCompressStream pass and drain everything it produced.
 *
 * The drain differs from zlib's in a way worth stating: brotli reports
 * "more output pending" explicitly via BrotliEncoderHasMoreOutput, so the
 * loop condition is that flag plus a non-empty input, rather than zlib's
 * "did it leave avail_out spare". Copying zlib's shape here would stop early
 * whenever the output happened to land exactly on the buffer boundary.
 *
 * `in`/`in_len` may be NULL/0 for a flush or finish. */
static int brotli_pump(BrotliStream* z, BrotliEncoderOperation op,
                       const unsigned char* in, size_t in_len) {
    if (!z || z->broken) return 0;

    size_t avail_in = in_len;
    const uint8_t* next_in = (const uint8_t*)in;
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
        size_t avail_out = z->out_cap - used;
        uint8_t* next_out = (uint8_t*)(z->out + used);

        if (!BrotliEncoderCompressStream(z->enc, op, &avail_in, &next_in,
                                         &avail_out, &next_out, NULL)) {
            z->broken = 1;
            return 0;
        }
        used = z->out_cap - avail_out;

        /* Done when brotli has consumed the input and says nothing is
         * pending. Otherwise grow and go round again. */
        if (avail_in == 0 && !BrotliEncoderHasMoreOutput(z->enc)) break;

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

void* brotli_try_stream_new(int quality, int window) {
    BrotliStream* z = (BrotliStream*)aether_caps_calloc(1, sizeof(BrotliStream));
    if (!z) return NULL;
    z->enc = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!z->enc) {
        aether_caps_free(z, sizeof(BrotliStream));
        return NULL;
    }
    BrotliEncoderSetParameter(z->enc, BROTLI_PARAM_QUALITY,
                              (uint32_t)clamp_quality(quality));
    if (window >= BROTLI_MIN_WINDOW_BITS && window <= BROTLI_MAX_WINDOW_BITS) {
        BrotliEncoderSetParameter(z->enc, BROTLI_PARAM_LGWIN, (uint32_t)window);
    }
    return z;
}

int brotli_try_stream_write(void* handle, const char* data, int length) {
    BrotliStream* z = (BrotliStream*)handle;
    if (!z || z->finished || z->broken || length < 0) return 0;
    size_t in_len;
    const unsigned char* in = brotli_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;
    return brotli_pump(z, BROTLI_OPERATION_PROCESS, in, in_len);
}

int brotli_try_stream_flush(void* handle) {
    BrotliStream* z = (BrotliStream*)handle;
    if (!z || z->finished || z->broken) return 0;
    return brotli_pump(z, BROTLI_OPERATION_FLUSH, NULL, 0);
}

int brotli_try_stream_finish(void* handle) {
    BrotliStream* z = (BrotliStream*)handle;
    if (!z || z->broken) return 0;
    if (z->finished) { z->out_len = 0; return 1; }   /* idempotent */
    if (!brotli_pump(z, BROTLI_OPERATION_FINISH, NULL, 0)) return 0;
    z->finished = 1;
    return 1;
}

const char* brotli_get_stream_bytes(void* handle) {
    BrotliStream* z = (BrotliStream*)handle;
    if (!z || !z->out) return "";
    return (const char*)z->out;
}

int brotli_get_stream_length(void* handle) {
    BrotliStream* z = (BrotliStream*)handle;
    return z ? z->out_len : 0;
}

void brotli_release_stream(void* handle) {
    BrotliStream* z = (BrotliStream*)handle;
    if (!z) return;
    if (z->enc) BrotliEncoderDestroyInstance(z->enc);
    if (z->out) aether_caps_free(z->out, z->out_cap);
    aether_caps_free(z, sizeof(BrotliStream));
}

int brotli_try_compress(const char* data, int length, int quality) {
    free_comp_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = brotli_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    size_t bound = BrotliEncoderMaxCompressedSize(in_len);
    if (bound == 0) bound = 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(bound);
    if (!out) return 0;

    size_t out_size = bound;
    if (!BrotliEncoderCompress(clamp_quality(quality), BROTLI_DEFAULT_WINDOW,
                               BROTLI_MODE_GENERIC, in_len, in, &out_size, out)) {
        aether_caps_free(out, bound);
        return 0;
    }
    tls_comp_buf = out;
    tls_comp_cap = bound;
    tls_comp_len = (int)out_size;
    return 1;
}

#else /* !AETHER_HAS_BROTLI */

/* stream_new returning NULL is the one signal a caller cannot ignore: every
 * other stream entry point needs a handle, so a program built without brotli
 * fails where it asks for a stream rather than silently emitting nothing.
 * (base64 in #1884 returned empty from its no-backend branch and callers
 * shipped broken output for it.) */
void* brotli_try_stream_new(int quality, int window) {
    (void)quality; (void)window; return NULL;
}
int brotli_try_stream_write(void* handle, const char* data, int length) {
    (void)handle; (void)data; (void)length; return 0;
}
int brotli_try_stream_flush(void* handle)  { (void)handle; return 0; }
int brotli_try_stream_finish(void* handle) { (void)handle; return 0; }
const char* brotli_get_stream_bytes(void* handle) { (void)handle; return ""; }
int brotli_get_stream_length(void* handle) { (void)handle; return 0; }
void brotli_release_stream(void* handle) { (void)handle; }

int brotli_try_compress(const char* data, int length, int quality) {
    (void)data; (void)length; (void)quality; return 0;
}

#endif /* AETHER_HAS_BROTLI */

const char* brotli_get_compress_bytes(void) {
    return (const char*)(tls_comp_buf ? tls_comp_buf : (unsigned char*)"");
}
int  brotli_get_compress_length(void) { return tls_comp_len; }
void brotli_release_compress(void)    { free_comp_tls(); }
