#include "aether_cryptography.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif
#endif

/* Unwrap the payload from a `data` argument that may be either an
 * AetherString* or a plain char*. Mirrors the helper in
 * std/fs/aether_fs.c — when callers pass a length-aware AetherString
 * (e.g. from fs.read_binary), the raw pointer is the struct, not
 * the bytes. Without this dispatch, we'd hash the struct header. */
static inline const unsigned char* cryptography_unwrap_bytes(const char* data, int length, size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (length >= 0) ? (size_t)length : s->length;
        return (const unsigned char*)s->data;
    }
    *out_len = (length >= 0) ? (size_t)length : strlen(data);
    return (const unsigned char*)data;
}

static char* hex_encode(const unsigned char* digest, size_t digest_len) {
    /* Two hex chars per byte + trailing NUL. */
    char* hex = (char*)malloc(digest_len * 2 + 1);
    if (!hex) return NULL;
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < digest_len; i++) {
        hex[i * 2]     = HEX[(digest[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = HEX[digest[i] & 0x0F];
    }
    hex[digest_len * 2] = '\0';
    return hex;
}

#ifdef AETHER_HAS_OPENSSL
static char* sha_hex(const EVP_MD* md, const char* data, int length) {
    if (length < 0) return NULL;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return NULL;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        (want > 0 && EVP_DigestUpdate(ctx, bytes, want) != 1) ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return NULL;
    }
    EVP_MD_CTX_free(ctx);

    return hex_encode(digest, (size_t)digest_len);
}

char* cryptography_sha1_hex_raw(const char* data, int length) {
    return sha_hex(EVP_sha1(), data, length);
}

char* cryptography_sha256_hex_raw(const char* data, int length) {
    return sha_hex(EVP_sha256(), data, length);
}

/* Algorithm-by-name dispatcher. EVP_get_digestbyname returns NULL for
 * unrecognized names; we let that NULL propagate through sha_hex's
 * NULL check. The set of names libcrypto recognizes is broader than
 * what this stdlib documents — sha384, sha512, sha3-256, etc. all
 * work — but the wrapper layer's docs only commit to sha1 + sha256
 * as supported names. Future stdlib additions (per-algo hex helpers)
 * land here too. */
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length) {
    if (!algo) return NULL;
    const EVP_MD* md = EVP_get_digestbyname(algo);
    if (!md) return NULL;
    return sha_hex(md, data, length);
}

int cryptography_hash_supported(const char* algo) {
    if (!algo) return 0;
    return EVP_get_digestbyname(algo) != NULL ? 1 : 0;
}

/* MD4 (#637) and MD5 (#631). EVP_md4() / EVP_md5() bind the algorithm
 * directly — on OpenSSL 3 MD4 lives in the legacy provider, which
 * EVP_get_digestbyname("md4") doesn't consult, but EVP_md4() does.
 *
 * OpenSSL 3 quirk: EVP_md4() returns a non-NULL handle even when the
 * legacy provider isn't loaded, but EVP_DigestInit_ex then fails with
 * "unsupported algorithm" because no actual implementation is wired
 * up. We load the legacy provider on first MD4 use; the default
 * provider stays loaded too so SHA-1/256/MD5 keep working. Loading
 * is one-shot via an atomic flag — the load itself is internally
 * idempotent, but pthread_once-style gating avoids repeated calls
 * during a busy hash loop.
 *
 * Returns NULL when MD4 is genuinely unavailable (OpenSSL built
 * without the legacy provider at all — e.g. some hardened-build
 * options on RHEL/FIPS systems). */
#if defined(AETHER_HAS_OPENSSL) && OPENSSL_VERSION_NUMBER >= 0x30000000L
static int md4_provider_attempted = 0;
static void ensure_legacy_provider(void) {
    if (md4_provider_attempted) return;
    md4_provider_attempted = 1;
    /* CRITICAL: loading just "legacy" implicitly unloads the default
     * provider, which then takes MD5/SHA-1/SHA-256 with it. We must
     * load BOTH so the modern algorithms keep working too. The
     * default provider is loaded automatically at first crypto use
     * elsewhere in this file, but once we explicitly touch the
     * provider system that auto-load behavior changes — we have to
     * be explicit. */
    OSSL_PROVIDER_load(NULL, "default");
    OSSL_PROVIDER_load(NULL, "legacy");
}
#else
static void ensure_legacy_provider(void) { /* nothing to do */ }
#endif

char* cryptography_md4_hex_raw(const char* data, int length) {
    ensure_legacy_provider();
    const EVP_MD* md = EVP_md4();
    if (!md) return NULL;
    return sha_hex(md, data, length);
}

char* cryptography_md5_hex_raw(const char* data, int length) {
    const EVP_MD* md = EVP_md5();
    if (!md) return NULL;
    return sha_hex(md, data, length);
}

/* Binary (raw-bytes) digest TLS split-accessor (#637). Same shape as
 * the base64-decode TLS slot above. Independent buffer — concurrent
 * b64 + digest work on the same thread don't clobber each other. */
static __thread unsigned char* g_dig_buf = NULL;
static __thread size_t         g_dig_cap = 0;
static __thread int            g_dig_len = 0;

static void release_dig_locked(void) {
    if (g_dig_buf) aether_caps_free(g_dig_buf, g_dig_cap);
    g_dig_buf = NULL;
    g_dig_cap = 0;
    g_dig_len = 0;
}

void cryptography_release_binary_digest(void) {
    release_dig_locked();
}

const char* cryptography_get_binary_digest(void) {
    return g_dig_buf ? (const char*)g_dig_buf : "";
}

int cryptography_get_binary_digest_length(void) {
    return g_dig_len;
}

/* Shared core for the *_bytes_raw family — computes EVP_Digest into
 * the TLS slot, replacing any previous content. */
static int digest_into_tls(const EVP_MD* md, const char* data, int length) {
    if (!md || length < 0) return 0;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        (want > 0 && EVP_DigestUpdate(ctx, bytes, want) != 1) ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return 0;
    }
    EVP_MD_CTX_free(ctx);

    release_dig_locked();
    /* Allocate at least 1 byte so the caller can distinguish "empty
     * digest" (impossible — no real digest is 0 bytes) from "no data
     * yet" via the accessor. */
    size_t alloc = digest_len > 0 ? digest_len : 1;
    g_dig_buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!g_dig_buf) { g_dig_cap = 0; g_dig_len = 0; return 0; }
    g_dig_cap = alloc;
    memcpy(g_dig_buf, digest, digest_len);
    g_dig_len = (int)digest_len;
    return 1;
}

int cryptography_md4_bytes_raw(const char* data, int length) {
    ensure_legacy_provider();
    return digest_into_tls(EVP_md4(), data, length);
}
int cryptography_md5_bytes_raw(const char* data, int length) {
    return digest_into_tls(EVP_md5(), data, length);
}
int cryptography_sha1_bytes_raw(const char* data, int length) {
    return digest_into_tls(EVP_sha1(), data, length);
}
int cryptography_sha256_bytes_raw(const char* data, int length) {
    return digest_into_tls(EVP_sha256(), data, length);
}
int cryptography_hash_bytes_raw(const char* algo, const char* data, int length) {
    if (!algo) return 0;
    return digest_into_tls(EVP_get_digestbyname(algo), data, length);
}

/* HMAC-SHA256 (#631). Veneer over libcrypto's HMAC() one-shot. The
 * hex variant returns a caller-frees lowercase-hex string (64 chars);
 * the bytes variant stashes the 32-byte raw digest in the binary-
 * digest TLS slot. SigV4's key-derivation chain needs the raw form —
 * each round's output keys the next, so hex would round-trip through
 * unhex on every step. */
char* cryptography_hmac_sha256_hex_raw(const char* key, int key_len,
                                      const char* msg, int msg_len) {
    if (key_len < 0 || msg_len < 0) return NULL;
    size_t key_n, msg_n;
    const unsigned char* kp = cryptography_unwrap_bytes(key, key_len, &key_n);
    const unsigned char* mp = cryptography_unwrap_bytes(msg, msg_len, &msg_n);
    if ((key_n > 0 && !kp) || (msg_n > 0 && !mp)) return NULL;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    unsigned char* rc = HMAC(EVP_sha256(), kp, (int)key_n, mp, msg_n,
                             digest, &digest_len);
    if (!rc) return NULL;
    return hex_encode(digest, digest_len);
}

int cryptography_hmac_sha256_bytes_raw(const char* key, int key_len,
                                       const char* msg, int msg_len) {
    if (key_len < 0 || msg_len < 0) return 0;
    size_t key_n, msg_n;
    const unsigned char* kp = cryptography_unwrap_bytes(key, key_len, &key_n);
    const unsigned char* mp = cryptography_unwrap_bytes(msg, msg_len, &msg_n);
    if ((key_n > 0 && !kp) || (msg_n > 0 && !mp)) return 0;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    unsigned char* rc = HMAC(EVP_sha256(), kp, (int)key_n, mp, msg_n,
                             digest, &digest_len);
    if (!rc) return 0;

    release_dig_locked();
    size_t alloc = digest_len > 0 ? digest_len : 1;
    g_dig_buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!g_dig_buf) { g_dig_cap = 0; g_dig_len = 0; return 0; }
    g_dig_cap = alloc;
    memcpy(g_dig_buf, digest, digest_len);
    g_dig_len = (int)digest_len;
    return 1;
}

/* ---- Incremental (streaming) digest context (fbs-core ask #4) ----
 *
 * A heap EVP_MD_CTX handle the caller threads through update()/final().
 * The architectural win for a blob store: hash each window AS it streams
 * to disk instead of reading the whole object back to compute its ETag.
 * S3 ETag = md5-of-the-object; multipart ETag = md5-of-md5s — both want
 * a context that survives across reads.
 *
 * The handle is the EVP_MD_CTX* itself, returned to Aether as an opaque
 * `ptr`. new() allocates + EVP_DigestInit_ex; update() feeds bytes;
 * final_hex()/final_bytes() finalize AND free the context (so the common
 * path needs no explicit free); free() is the abandon-without-finalize
 * cleanup path (e.g. an aborted upload). Algorithm is chosen by name at
 * new() time — same dispatcher rules as hash_hex (md4/md5 bind directly
 * so they work on OpenSSL 3's legacy provider; everything else goes
 * through EVP_get_digestbyname). */

static const EVP_MD* resolve_md_by_name(const char* algo) {
    if (!algo) return NULL;
    /* md4/md5 must bind directly: EVP_get_digestbyname doesn't consult
     * the legacy provider where MD4 lives on OpenSSL 3, and we want the
     * same provider behaviour the one-shot helpers already have. */
    if (strcmp(algo, "md4") == 0) { ensure_legacy_provider(); return EVP_md4(); }
    if (strcmp(algo, "md5") == 0) { return EVP_md5(); }
    return EVP_get_digestbyname(algo);
}

void* cryptography_digest_new_raw(const char* algo) {
    const EVP_MD* md = resolve_md_by_name(algo);
    if (!md) return NULL;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

int cryptography_digest_update_raw(void* handle, const char* data, int length) {
    if (!handle || length < 0) return 0;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return 0;
    if (want == 0) return 1;  /* feeding zero bytes is a no-op success */
    return EVP_DigestUpdate((EVP_MD_CTX*)handle, bytes, want) == 1 ? 1 : 0;
}

/* Finalize → caller-frees lowercase hex; frees the context either way. */
char* cryptography_digest_final_hex_raw(void* handle) {
    if (!handle) return NULL;
    EVP_MD_CTX* ctx = (EVP_MD_CTX*)handle;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    int ok = EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return NULL;
    return hex_encode(digest, (size_t)digest_len);
}

/* Finalize → binary-digest TLS slot (for SigV4 payload hashing, where
 * the raw bytes key the next round). Frees the context either way.
 * Bytes via cryptography_get_binary_digest() / ..._length(). */
int cryptography_digest_final_bytes_raw(void* handle) {
    if (!handle) return 0;
    EVP_MD_CTX* ctx = (EVP_MD_CTX*)handle;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    int ok = EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return 0;

    release_dig_locked();
    size_t alloc = digest_len > 0 ? digest_len : 1;
    g_dig_buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!g_dig_buf) { g_dig_cap = 0; g_dig_len = 0; return 0; }
    g_dig_cap = alloc;
    memcpy(g_dig_buf, digest, digest_len);
    g_dig_len = (int)digest_len;
    return 1;
}

/* Abandon a context without finalizing — the aborted-upload cleanup
 * path. NULL-safe so callers can free unconditionally. */
void cryptography_digest_free_raw(void* handle) {
    if (handle) EVP_MD_CTX_free((EVP_MD_CTX*)handle);
}


#else /* !AETHER_HAS_OPENSSL */

char* cryptography_sha1_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_sha256_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length) {
    (void)algo; (void)data; (void)length; return NULL;
}
int cryptography_hash_supported(const char* algo) {
    (void)algo; return 0;
}

char* cryptography_md4_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_md5_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_hmac_sha256_hex_raw(const char* key, int key_len,
                                      const char* msg, int msg_len) {
    (void)key; (void)key_len; (void)msg; (void)msg_len; return NULL;
}
int cryptography_hmac_sha256_bytes_raw(const char* key, int key_len,
                                       const char* msg, int msg_len) {
    (void)key; (void)key_len; (void)msg; (void)msg_len; return 0;
}
int cryptography_md4_bytes_raw(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int cryptography_md5_bytes_raw(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int cryptography_sha1_bytes_raw(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int cryptography_sha256_bytes_raw(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int cryptography_hash_bytes_raw(const char* algo, const char* data, int length) {
    (void)algo; (void)data; (void)length; return 0;
}
const char* cryptography_get_binary_digest(void) { return ""; }
int cryptography_get_binary_digest_length(void) { return 0; }
void cryptography_release_binary_digest(void) {}

void* cryptography_digest_new_raw(const char* algo) {
    (void)algo; return NULL;
}
int cryptography_digest_update_raw(void* handle, const char* data, int length) {
    (void)handle; (void)data; (void)length; return 0;
}
char* cryptography_digest_final_hex_raw(void* handle) {
    (void)handle; return NULL;
}
int cryptography_digest_final_bytes_raw(void* handle) {
    (void)handle; return 0;
}
void cryptography_digest_free_raw(void* handle) {
    (void)handle;
}

#endif /* AETHER_HAS_OPENSSL */

/* ---- Base64 (portable, not OpenSSL) ----
 *
 * Base64 is a transform, not a cipher: it needs no crypto library, and
 * requiring one meant every build WITHOUT OpenSSL silently answered NULL
 * from encode and 0 from decode. The Windows cross-build is such a build,
 * which is how the Wine runtime lane (#1876) found this — a program that
 * encodes anything got '(null)' back and nothing reported a problem.
 *
 * Deliberately outside the AETHER_HAS_OPENSSL split, so there is ONE
 * implementation and no chance of the two drifting.
 *
 * Contract kept exactly as it was on the OpenSSL path: encode_raw is
 * unpadded and encode_padded_raw pads to a multiple of 4 (RFC 4648 §4);
 * decode accepts padded or unpadded input and publishes its result through
 * the thread-local accessor trio. Allocation stays cap-aware (#343). */

static const char AE_B64_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int ae_b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char* ae_b64_encode(const char* data, int length, int pad) {
    if (length < 0) return NULL;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return NULL;

    size_t out_cap = ((want + 2) / 3) * 4 + 1;
    char* out = (char*)aether_caps_malloc(out_cap);
    if (!out) return NULL;

    size_t i = 0, o = 0;
    while (want >= 3 && i + 3 <= want) {
        unsigned v = ((unsigned)bytes[i] << 16) |
                     ((unsigned)bytes[i + 1] << 8) |
                      (unsigned)bytes[i + 2];
        out[o++] = AE_B64_ALPHA[(v >> 18) & 63];
        out[o++] = AE_B64_ALPHA[(v >> 12) & 63];
        out[o++] = AE_B64_ALPHA[(v >> 6) & 63];
        out[o++] = AE_B64_ALPHA[v & 63];
        i += 3;
    }
    size_t rem = want - i;
    if (rem == 1) {
        unsigned v = (unsigned)bytes[i] << 16;
        out[o++] = AE_B64_ALPHA[(v >> 18) & 63];
        out[o++] = AE_B64_ALPHA[(v >> 12) & 63];
        if (pad) { out[o++] = '='; out[o++] = '='; }
    } else if (rem == 2) {
        unsigned v = ((unsigned)bytes[i] << 16) | ((unsigned)bytes[i + 1] << 8);
        out[o++] = AE_B64_ALPHA[(v >> 18) & 63];
        out[o++] = AE_B64_ALPHA[(v >> 12) & 63];
        out[o++] = AE_B64_ALPHA[(v >> 6) & 63];
        if (pad) out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

char* cryptography_base64_encode_raw(const char* data, int length) {
    return ae_b64_encode(data, length, 0);
}

char* cryptography_base64_encode_padded_raw(const char* data, int length) {
    return ae_b64_encode(data, length, 1);
}

/* Thread-local decode buffer, mirroring std.fs.read_binary's split-accessor
 * shape: the result lives until the next decode on the same thread, and the
 * Aether wrapper copies it out. */
static __thread unsigned char* g_b64_buf = NULL;
static __thread size_t         g_b64_cap = 0;
static __thread int            g_b64_len = 0;

static void release_b64_locked(void) {
    if (g_b64_buf) aether_caps_free(g_b64_buf, g_b64_cap);
    g_b64_buf = NULL;
    g_b64_cap = 0;
    g_b64_len = 0;
}

void cryptography_release_base64_decode(void) {
    release_b64_locked();
}

int cryptography_base64_decode_raw(const char* b64) {
    release_b64_locked();
    if (!b64) return 0;

    size_t in_len;
    const unsigned char* in = cryptography_unwrap_bytes(b64, -1, &in_len);
    if (in_len > 0 && !in) return 0;

    /* Decoding "" is a valid request yielding zero bytes; allocate one byte
     * so the caller can tell "decoded nothing" from "no data". */
    size_t out_alloc = (in_len / 4) * 3 + 3;
    if (out_alloc == 0) out_alloc = 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(out_alloc);
    if (!out) return 0;

    unsigned acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = ae_b64_value(c);
        if (v < 0) { aether_caps_free(out, out_alloc); return 0; }
        acc = (acc << 6) | (unsigned)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o < out_alloc) out[o++] = (unsigned char)((acc >> nbits) & 0xFF);
        }
    }

    g_b64_buf = out;
    g_b64_cap = out_alloc;
    g_b64_len = (int)o;
    return 1;
}

const char* cryptography_get_base64_decode(void) {
    return g_b64_buf ? (const char*)g_b64_buf : "";
}

int cryptography_get_base64_decode_length(void) {
    return g_b64_len;
}


/* ===================================================================
 * CSPRNG (#630). Sits OUTSIDE the AETHER_HAS_OPENSSL branch because the
 * RNG is backed by the OS syscall, not libcrypto — works on every
 * supported build regardless of OpenSSL availability.
 * =================================================================== */

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
/* arc4random_buf is in stdlib.h on macOS/BSD. */
#else
#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

/* Independent TLS slot — concurrent random_bytes + digest work on the
 * same thread don't clobber each other. Per-thread so cross-thread
 * draws also don't interfere. */
static __thread unsigned char* g_rng_buf = NULL;
static __thread size_t         g_rng_cap = 0;
static __thread int            g_rng_len = 0;

static void release_rng_locked(void) {
    if (g_rng_buf) aether_caps_free(g_rng_buf, g_rng_cap);
    g_rng_buf = NULL;
    g_rng_cap = 0;
    g_rng_len = 0;
}

void cryptography_release_random_bytes(void) {
    release_rng_locked();
}

const char* cryptography_get_random_bytes(void) {
    return g_rng_buf ? (const char*)g_rng_buf : "";
}

int cryptography_get_random_bytes_length(void) {
    return g_rng_len;
}

#ifdef _WIN32
static int fill_random(unsigned char* out, size_t n) {
    NTSTATUS rc = BCryptGenRandom(NULL, out, (ULONG)n,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return rc == 0 ? 1 : 0;
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
static int fill_random(unsigned char* out, size_t n) {
    /* arc4random_buf cannot fail — it abort()s if the kernel can't
     * provide bytes, which is a fatal condition anyway. */
    arc4random_buf(out, n);
    return 1;
}
#elif defined(__wasi__)
/* WASI. wasi-libc exposes the host's CSPRNG through getentropy(3), which is
 * backed by the random_get syscall — there is no getrandom(2) and no
 * /dev/urandom to fall back to (WASI's filesystem is capability-scoped and a
 * preopened /dev is not guaranteed). getentropy fills the whole buffer or
 * fails; its documented cap is 256 bytes per call, so loop for larger asks. */
static int fill_random(unsigned char* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t chunk = n - got;
        if (chunk > 256) chunk = 256;
        if (getentropy(out + got, chunk) != 0) return 0;
        got += chunk;
    }
    return 1;
}
#else
/* Linux. getrandom(2) (glibc 2.25+, kernel 3.17+) is the right
 * source. The /dev/urandom fallback handles very old kernels (CI
 * matrices, embedded). EINTR retry is required on both paths —
 * a syscall interrupted by a signal returns -1 with errno=EINTR
 * and must be re-tried, not failed. */
static int fill_random(unsigned char* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) break;  /* fall through to /dev/urandom */
            return 0;
        }
        got += (size_t)r;
    }
    if (got == n) return 1;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return 0;
        }
        if (r == 0) { close(fd); return 0; }
        got += (size_t)r;
    }
    close(fd);
    return 1;
}
#endif

int cryptography_random_bytes_raw(int n) {
    release_rng_locked();
    if (n < 0) return 0;

    /* n == 0 yields a 1-byte buffer with length 0, mirroring the
     * base64_decode("") shape — gives callers a non-NULL accessor
     * pointer they can pass to string_new_with_length(_, 0). */
    size_t alloc = n > 0 ? (size_t)n : 1;
    unsigned char* buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!buf) return 0;

    if (n > 0 && !fill_random(buf, (size_t)n)) {
        aether_caps_free(buf, alloc);
        return 0;
    }
    if (n == 0) buf[0] = 0;

    g_rng_buf = buf;
    g_rng_cap = alloc;
    g_rng_len = n;
    return 1;
}
