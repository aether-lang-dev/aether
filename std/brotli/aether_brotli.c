#include "aether_brotli.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/actors/aether_panic.h"   /* AETHER_CONTEXT_CAPS_BUFFER */

#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_BROTLI
#include <brotli/encode.h>
#include <brotli/decode.h>
#endif

/* The input bytes and their count. `data` is always raw: the std module
 * passes a byte[] slice's data pointer through a `ptr`-typed extern
 * (#2301), and nothing else calls in. This used to sniff for an
 * AetherString header and unwrap one, but a sniff on raw bytes misreads a
 * payload beginning DE C0 57 AE (the header magic) as a header and reads
 * from a `data` pointer taken out of the bytes that follow. */
static inline const unsigned char* brotli_unwrap_bytes(const char* data, int length,
                                                       size_t* out_len) {
    if (!data || length <= 0) { *out_len = 0; return (const unsigned char*)data; }
    *out_len = (size_t)length;
    return (const unsigned char*)data;
}

/* One-shot output slot, thread-local like std.zlib's. The STREAMING output
 * lives on the handle instead; see the header. */
#if defined(AETHER_NO_THREADING)
/* A cooperative build: a block of the calling task's execution context
 * (aether_panic.h), not a thread-local on every task's stack. */
AETHER_CONTEXT_CAPS_BUFFER(tls_comp)
#define tls_comp_buf (tls_comp_here()->buf)
#define tls_comp_cap (tls_comp_here()->cap)
#define tls_comp_len (tls_comp_here()->len)
#else
static _Thread_local unsigned char* tls_comp_buf = NULL;
static _Thread_local size_t         tls_comp_cap = 0;
static _Thread_local int            tls_comp_len = 0;
#endif

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

/* CVE-2026-111497 ("broccoli2shell"): libbrotlidec 1.0.3 to 1.2.0 can write
 * out of bounds on a malformed block switch, which is remote code execution
 * for anything decoding untrusted `br`. The fix (1.0.10, 1.1.1, 1.2.1, and
 * distro backports that keep the old version number, Debian's
 * 1.0.9-2+deb12u1 among them) gave the reserved error code -17 a name,
 * BLOCK_SWITCH, in the same commit. The library's own error-string table is
 * compiled from that list, so asking it to name -17 tells a fixed decoder
 * from an unfixed one where the version number cannot. An unfixed library
 * is refused: decoding fails closed, encoding is unaffected. */
int brotli_decoder_patched(void) {
    const char* name = BrotliDecoderErrorString((BrotliDecoderErrorCode)-17);
    return name && strstr(name, "BLOCK_SWITCH") ? 1 : 0;
}

/* One-shot decompression output slot, thread-local */
#if defined(AETHER_NO_THREADING)
/* A cooperative build: a block of the calling task's execution context
 * (aether_panic.h), not a thread-local on every task's stack. */
AETHER_CONTEXT_CAPS_BUFFER(tls_decomp)
#define tls_decomp_buf (tls_decomp_here()->buf)
#define tls_decomp_cap (tls_decomp_here()->cap)
#define tls_decomp_len (tls_decomp_here()->len)
#else
static _Thread_local unsigned char* tls_decomp_buf = NULL;
static _Thread_local size_t         tls_decomp_cap = 0;
static _Thread_local int            tls_decomp_len = 0;
#endif

static void free_decomp_tls(void) {
    if (tls_decomp_buf) {
        aether_caps_free(tls_decomp_buf, tls_decomp_cap);
        tls_decomp_buf = NULL;
    }
    tls_decomp_cap = 0;
    tls_decomp_len = 0;
}

int brotli_try_decompress(const char* data, int length, int max_bytes) {
    free_decomp_tls();
    if (length < 0 || max_bytes < 0) return 0;
    if (!brotli_decoder_patched()) return -4;

    size_t in_len;
    const unsigned char* in = brotli_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    BrotliDecoderState* dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!dec) return 0;

    size_t limit = (max_bytes > 0) ? (size_t)max_bytes : ((size_t)1 << 30);
    size_t initial_cap = (in_len > 0 && in_len * 4 > 256) ? (in_len * 4) : 256;
    if (initial_cap > limit) initial_cap = limit;
    if (initial_cap == 0) initial_cap = 1;

    size_t cap = initial_cap;
    unsigned char* out = (unsigned char*)aether_caps_malloc(cap);
    if (!out) {
        BrotliDecoderDestroyInstance(dec);
        return 0;
    }

    size_t avail_in = in_len;
    const uint8_t* next_in = (const uint8_t*)in;
    size_t total_out = 0;
    int status = 0;

    for (;;) {
        size_t avail_out = cap - total_out;
        uint8_t* next_out = (uint8_t*)(out + total_out);

        BrotliDecoderResult res = BrotliDecoderDecompressStream(
            dec, &avail_in, &next_in, &avail_out, &next_out, NULL);

        total_out = cap - avail_out;

        if (res == BROTLI_DECODER_RESULT_SUCCESS) {
            if (avail_in > 0) {
                status = 0; /* Trailing garbage after valid stream */
            } else {
                status = 1;
            }
            break;
        } else if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            if (total_out >= limit) {
                status = -2;
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > limit) ncap = limit;
            if (ncap <= cap) {
                status = -2;
                break;
            }
            unsigned char* nb = (unsigned char*)aether_caps_malloc(ncap);
            if (!nb) {
                status = 0;
                break;
            }
            memcpy(nb, out, total_out);
            aether_caps_free(out, cap);
            out = nb;
            cap = ncap;
        } else if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            status = -3; /* the input ends before the stream does */
            break;
        } else {
            status = 0;
            break;
        }
    }

    BrotliDecoderDestroyInstance(dec);

    if (status == 1) {
        tls_decomp_buf = out;
        tls_decomp_cap = cap;
        tls_decomp_len = (int)total_out;
        return 1;
    } else {
        aether_caps_free(out, cap);
        return status;
    }
}

const char* brotli_get_decompress_bytes(void) {
    return (const char*)(tls_decomp_buf ? tls_decomp_buf : (unsigned char*)"");
}

int brotli_get_decompress_length(void) {
    return tls_decomp_len;
}

void brotli_release_decompress(void) {
    free_decomp_tls();
}

typedef struct {
    BrotliDecoderState* dec;
    unsigned char* out;
    size_t out_cap;
    int    out_len;
    size_t total_out;
    size_t limit;
    int    finished;
    int    broken;
    int    limit_exceeded;
} BrotliDecompressStream;

void* brotli_try_decompress_stream_new(int max_bytes) {
    if (max_bytes < 0 || !brotli_decoder_patched()) return NULL;
    BrotliDecompressStream* z = (BrotliDecompressStream*)aether_caps_calloc(1, sizeof(BrotliDecompressStream));
    if (!z) return NULL;
    z->dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!z->dec) {
        aether_caps_free(z, sizeof(BrotliDecompressStream));
        return NULL;
    }
    z->limit = (max_bytes > 0) ? (size_t)max_bytes : ((size_t)1 << 30);
    return z;
}

int brotli_try_decompress_stream_write(void* handle, const char* data, int length) {
    BrotliDecompressStream* z = (BrotliDecompressStream*)handle;
    if (!z || z->finished || z->broken || length < 0) return 0;

    size_t in_len;
    const unsigned char* in = brotli_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    z->out_len = 0;
    size_t used = 0;

    if (z->out_cap < 256) {
        size_t cap = 256;
        unsigned char* nb = (unsigned char*)aether_caps_malloc(cap);
        if (!nb) { z->broken = 1; return 0; }
        if (z->out) aether_caps_free(z->out, z->out_cap);
        z->out = nb;
        z->out_cap = cap;
    }

    size_t avail_in = in_len;
    const uint8_t* next_in = (const uint8_t*)in;

    for (;;) {
        size_t avail_out = z->out_cap - used;
        uint8_t* next_out = (uint8_t*)(z->out + used);

        BrotliDecoderResult res = BrotliDecoderDecompressStream(
            z->dec, &avail_in, &next_in, &avail_out, &next_out, NULL);

        used = z->out_cap - avail_out;

        if (z->total_out + used > z->limit) {
            z->broken = 1;
            z->limit_exceeded = 1;
            return -2;
        }

        if (res == BROTLI_DECODER_RESULT_SUCCESS) {
            /* Bytes after the end of the stream are corruption, not a
             * second stream: the one-shot path refuses them too. */
            if (avail_in > 0) { z->broken = 1; return 0; }
            z->finished = 1;
            break;
        } else if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            if (avail_in == 0) break;
        } else if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            size_t remaining_limit = z->limit - z->total_out;
            if (used >= remaining_limit) {
                z->broken = 1;
                z->limit_exceeded = 1;
                return -2;
            }
            size_t ncap = z->out_cap * 2;
            if (ncap > remaining_limit) ncap = remaining_limit;
            if (ncap <= z->out_cap) {
                z->broken = 1;
                z->limit_exceeded = 1;
                return -2;
            }
            unsigned char* nb = (unsigned char*)aether_caps_malloc(ncap);
            if (!nb) { z->broken = 1; return 0; }
            memcpy(nb, z->out, used);
            aether_caps_free(z->out, z->out_cap);
            z->out = nb;
            z->out_cap = ncap;
        } else {
            z->broken = 1;
            return 0;
        }
    }

    z->total_out += used;
    z->out_len = (int)used;
    return 1;
}

/* The stream must have reached its end by now: a decoder still waiting for
 * input means the body was cut short, and a truncated body is an error
 * rather than the shorter plaintext decoded so far. Returns -3 for that. */
int brotli_try_decompress_stream_finish(void* handle) {
    BrotliDecompressStream* z = (BrotliDecompressStream*)handle;
    if (!z || z->broken) return 0;
    z->out_len = 0;
    if (z->finished) return 1;
    z->broken = 1;
    return -3;
}

const char* brotli_get_decompress_stream_bytes(void* handle) {
    BrotliDecompressStream* z = (BrotliDecompressStream*)handle;
    if (!z || !z->out) return "";
    return (const char*)z->out;
}

int brotli_get_decompress_stream_length(void* handle) {
    BrotliDecompressStream* z = (BrotliDecompressStream*)handle;
    return z ? z->out_len : 0;
}

void brotli_release_decompress_stream(void* handle) {
    BrotliDecompressStream* z = (BrotliDecompressStream*)handle;
    if (!z) return;
    if (z->dec) BrotliDecoderDestroyInstance(z->dec);
    if (z->out) aether_caps_free(z->out, z->out_cap);
    aether_caps_free(z, sizeof(BrotliDecompressStream));
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

int brotli_decoder_patched(void) { return 0; }

int brotli_try_decompress(const char* data, int length, int max_bytes) {
    (void)data; (void)length; (void)max_bytes; return 0;
}

const char* brotli_get_decompress_bytes(void) { return ""; }
int brotli_get_decompress_length(void) { return 0; }
void brotli_release_decompress(void) {}

void* brotli_try_decompress_stream_new(int max_bytes) {
    (void)max_bytes; return NULL;
}

int brotli_try_decompress_stream_write(void* handle, const char* data, int length) {
    (void)handle; (void)data; (void)length; return 0;
}

int brotli_try_decompress_stream_finish(void* handle) {
    (void)handle; return 0;
}

const char* brotli_get_decompress_stream_bytes(void* handle) { (void)handle; return ""; }
int brotli_get_decompress_stream_length(void* handle) { (void)handle; return 0; }
void brotli_release_decompress_stream(void* handle) { (void)handle; }

#endif /* AETHER_HAS_BROTLI */

const char* brotli_get_compress_bytes(void) {
    return (const char*)(tls_comp_buf ? tls_comp_buf : (unsigned char*)"");
}
int  brotli_get_compress_length(void) { return tls_comp_len; }
void brotli_release_compress(void)    { free_comp_tls(); }
