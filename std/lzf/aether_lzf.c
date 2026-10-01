#include "aether_lzf.h"
#include "lzf.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The input bytes and their count. `data` is always raw: the std module
 * passes a byte[] slice's data pointer through a `ptr`-typed extern
 * (#2301), and nothing else calls in. This used to sniff for an
 * AetherString header and unwrap one, but a sniff on raw bytes misreads a
 * payload beginning DE C0 57 AE (the header magic) as a header and reads
 * from a `data` pointer taken out of the bytes that follow. */
static inline const unsigned char* lzf_unwrap_bytes(const char* data, int length, size_t* out_len) {
    if (!data || length <= 0) { *out_len = 0; return (const unsigned char*)data; }
    *out_len = (size_t)length;
    return (const unsigned char*)data;
}

/* Resource-caps tracking (#343): like the zlib path, length and
 * capacity differ when the LZF compressor produces a result smaller
 * than its safety bound; `_cap` is what aether_caps_free needs.  */
static _Thread_local unsigned char* tls_compress_buf = NULL;
static _Thread_local size_t         tls_compress_cap = 0;
static _Thread_local int            tls_compress_len = 0;
static _Thread_local unsigned char* tls_decompress_buf = NULL;
static _Thread_local size_t         tls_decompress_cap = 0;
static _Thread_local int            tls_decompress_len = 0;

static void free_compress_tls(void) {
    if (tls_compress_buf) {
        aether_caps_free(tls_compress_buf, tls_compress_cap);
        tls_compress_buf = NULL;
    }
    tls_compress_cap = 0;
    tls_compress_len = 0;
}

static void free_decompress_tls(void) {
    if (tls_decompress_buf) {
        aether_caps_free(tls_decompress_buf, tls_decompress_cap);
        tls_decompress_buf = NULL;
    }
    tls_decompress_cap = 0;
    tls_decompress_len = 0;
}

int lzf_max_compressed_size(int length) {
    if (length < 0) return 0;
    return (int)LZF_MAX_COMPRESSED_SIZE((unsigned int)length);
}

int lzf_try_compress(const char* data, int length) {
    free_compress_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = lzf_unwrap_bytes(data, length, &in_len);
    if (in_len > UINT_MAX) return 0;
    if (in_len > 0 && !in) return 0;

    /* Empty input → empty output, legitimate success without invoking
     * the underlying codec (which has no defined empty-stream form). */
    if (in_len == 0) {
        /* tls_compress_buf already NULL, tls_compress_len already 0
         * after free_compress_tls; getters return "" / 0. */
        return 1;
    }

    unsigned int bound = LZF_MAX_COMPRESSED_SIZE((unsigned int)in_len);
    size_t alloc_cap = (size_t)bound;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) return 0;

    unsigned int out_len = lzf_compress(in, (unsigned int)in_len, out, bound);
    if (out_len == 0) {
        /* Incompressible payload didn't fit in the bound. Caller must
         * fall back to storing data uncompressed (with their own flag). */
        aether_caps_free(out, alloc_cap);
        return 0;
    }

    tls_compress_buf = out;
    tls_compress_cap = alloc_cap;
    tls_compress_len = (int)out_len;
    return 1;
}

int lzf_try_decompress(const char* data, int length, int output_length) {
    free_decompress_tls();
    if (length < 0 || output_length < 0) return 0;

    size_t in_len;
    const unsigned char* in = lzf_unwrap_bytes(data, length, &in_len);
    if (in_len > UINT_MAX || (size_t)output_length > UINT_MAX) return 0;
    if (in_len > 0 && !in) return 0;

    /* LZF doesn't define an empty stream; (empty in, 0 out) is a
     * legitimate identity that we resolve without calling the codec
     * (which would read past in_data on in_len == 0). Non-empty in
     * with output_length == 0 is a corruption mismatch. */
    if (output_length == 0) {
        if (in_len != 0) return 0;
        /* tls_decompress_buf already NULL, len 0 after free_decompress_tls. */
        return 1;
    }
    if (in_len == 0) return 0;

    size_t alloc_cap = (size_t)output_length;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) return 0;

    unsigned int out_len = lzf_decompress(in, (unsigned int)in_len, out, (unsigned int)output_length);
    if (out_len != (unsigned int)output_length) {
        aether_caps_free(out, alloc_cap);
        return 0;
    }

    tls_decompress_buf = out;
    tls_decompress_cap = alloc_cap;
    tls_decompress_len = (int)out_len;
    return 1;
}

const char* lzf_get_compress_bytes(void) {
    return (const char*)(tls_compress_buf ? tls_compress_buf : (unsigned char*)"");
}

int lzf_get_compress_length(void) { return tls_compress_len; }
void lzf_release_compress(void) { free_compress_tls(); }

const char* lzf_get_decompress_bytes(void) {
    return (const char*)(tls_decompress_buf ? tls_decompress_buf : (unsigned char*)"");
}

int lzf_get_decompress_length(void) { return tls_decompress_len; }
void lzf_release_decompress(void) { free_decompress_tls(); }
