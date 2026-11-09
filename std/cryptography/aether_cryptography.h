/* std.cryptography — cryptographic hash primitives + Base64 codec.
 *
 * Surface (in module.ae's wrapper layer):
 *   sha1_hex(data, length)       — one-shot SHA-1 → lowercase hex
 *   sha256_hex(data, length)     — one-shot SHA-256 → lowercase hex
 *   hash_hex(algo, data, length) — algorithm-by-name dispatcher
 *   hash_supported(algo)         — config-time probe; 1/0 answer
 *   base64_encode(data, length)  — RFC 4648 §4 standard alphabet, unpadded
 *   base64_decode(b64)           — owned length-carrying string, NULL on error
 *
 * Streaming (incremental) digests are supported via the digest-context
 * API (digest_new / digest_update / digest_final_*). Key derivation,
 * symmetric ciphers, signing, URL-safe Base64 (RFC 4648 §5), and
 * PKCS#7 / PEM parsing remain deliberately out of scope. See
 * docs/stdlib-vs-contrib.md for the "one obvious shape" criterion.
 *
 * When the build has AETHER_HAS_OPENSSL (the default on every
 * platform where OpenSSL is available), the implementation is a
 * thin veneer over libcrypto. Without OpenSSL, every entry point
 * returns NULL / 0 so the Go-style Aether wrappers report
 * "openssl unavailable" cleanly.
 */

#ifndef AETHER_CRYPTOGRAPHY_H
#define AETHER_CRYPTOGRAPHY_H

/* Return a lowercase-hex digest (40 chars for SHA-1, 64 chars for
 * SHA-256) as a fresh counted AetherString (cast to char*), or NULL on
 * failure. Caller owns it and releases it with string_release().
 *
 * `data` may be an AetherString* or a plain char*; `length` is the
 * explicit byte count (binary-safe, embedded NULs OK). A `length`
 * of 0 hashes the empty string — SHA-256 of "" is a well-defined
 * constant. */
char* cryptography_sha1_hex_raw(const char* data, int length);
char* cryptography_sha256_hex_raw(const char* data, int length);

/* Generic algorithm-by-name digest. `algo` is one of "sha1", "sha256",
 * or any other name OpenSSL's EVP_get_digestbyname() recognizes
 * ("sha384", "sha512", "sha3-256", ...). Returns the same
 * lowercase-hex shape as the per-algorithm functions, or NULL on
 * unknown algorithm / digest failure / no OpenSSL. Caller releases. */
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length);

/* Probe whether the OpenSSL backend in this build can compute `algo`.
 * Returns 1 yes, 0 no. Always succeeds (never errors); callers use
 * this at config time to validate user-supplied algorithm names
 * before they hit hash_hex_raw. Returns 0 when built without
 * OpenSSL. */
/* _raw like its siblings: the Aether wrapper is `hash_supported`, which
 * mangles to `cryptography_hash_supported` and would collide with this. */
int cryptography_hash_supported_raw(const char* algo);

/* MD4 — per-block strong checksum in the .zsync wire format (#637).
 * Bound to EVP_md4() directly rather than via EVP_get_digestbyname()
 * so it works on stock OpenSSL 3 where MD4 lives in the legacy
 * provider that the by-name lookup doesn't consult. Returns
 * lowercase hex (32 chars). On OpenSSL builds without MD4 at all,
 * returns NULL. */
char* cryptography_md4_hex_raw(const char* data, int length);

/* MD5 — Content-MD5 / ETag interop (#631). Same shape and contract
 * as the sha*_hex_raw siblings; the algorithm is documented as legacy
 * in module.ae. Returns lowercase hex (32 chars). */
char* cryptography_md5_hex_raw(const char* data, int length);

/* HMAC-SHA256 (#631). Thin veneer over libcrypto's HMAC(). The
 * raw variant returns the 32-byte digest via the binary-digest TLS
 * split-accessor (see below); the hex variant returns
 * lowercase hex (64 chars) the caller releases. SigV4's key-derivation
 * chain needs the raw form — each round's output keys the next. */
char* cryptography_hmac_sha256_hex_raw(const char* key, int key_len,
                                      const char* msg, int msg_len);
/* Sets the binary-digest TLS slot. Returns 1 on success, 0 on
 * failure / no OpenSSL. Bytes available via
 * cryptography_get_binary_digest() / ..._length(). */
int   cryptography_hmac_sha256_bytes_raw(const char* key, int key_len,
                                         const char* msg, int msg_len);

/* Binary (raw-bytes) digest accessors for length-aware, binary-safe
 * digest output (#637). Same TLS split-accessor shape as base64_decode
 * — the digest lives in a per-thread buffer that remains valid until
 * the next bytes-producing call on the same thread. Aether-side
 * wrappers copy into an AetherString via string_new_with_length.
 *
 *   md4_bytes        → 16-byte MD4 digest
 *   sha1_bytes       → 20-byte SHA-1
 *   sha256_bytes     → 32-byte SHA-256
 *   hash_bytes(algo) → algorithm-by-name (same dispatcher rules as
 *                      hash_hex_raw)
 *   hmac_sha256_bytes (above) shares the same TLS slot.
 *
 * Returns 1 on success, 0 on failure / no OpenSSL / unknown
 * algorithm. */
int cryptography_md4_bytes_raw(const char* data, int length);
int cryptography_md5_bytes_raw(const char* data, int length);
int cryptography_sha1_bytes_raw(const char* data, int length);
int cryptography_sha256_bytes_raw(const char* data, int length);
int cryptography_hash_bytes_raw(const char* algo, const char* data, int length);
const char* cryptography_get_binary_digest(void);
int         cryptography_get_binary_digest_length(void);
void        cryptography_release_binary_digest(void);

/* Incremental (streaming) digest context (fbs-core ask #4). A heap
 * EVP_MD_CTX handle the caller threads through update() then final().
 * Lets a streaming consumer (blob store, large-file hasher) hash each
 * window as it arrives instead of buffering the whole input — the S3
 * ETag use case, where reading the stored object back purely to MD5 it
 * is the architectural compromise this removes.
 *
 *   digest_new(algo)        → opaque ctx handle, or NULL on unknown
 *                             algorithm / OOM / no OpenSSL. `algo` uses
 *                             the same names as hash_hex ("md5",
 *                             "sha256", "sha1", "md4", ...).
 *   digest_update(ctx,d,n)  → 1 on success, 0 on failure. Binary-safe;
 *                             `d` may be an AetherString* or char*.
 *                             Feeding 0 bytes is a no-op success.
 *   digest_final_hex(ctx)   → caller-released lowercase hex; FREES ctx.
 *   digest_final_bytes(ctx) → raw digest into the binary-digest TLS
 *                             slot (for SigV4 chaining); FREES ctx.
 *   digest_free(ctx)        → abandon without finalizing (aborted
 *                             upload). NULL-safe.
 *
 * The two final() variants free the context, so the success path needs
 * no explicit free; call digest_free only when bailing out before
 * final. */
void* cryptography_digest_new_raw(const char* algo);
int   cryptography_digest_update_raw(void* handle, const char* data, int length);
char* cryptography_digest_final_hex_raw(void* handle);
int   cryptography_digest_final_bytes_raw(void* handle);
void  cryptography_digest_free_raw(void* handle);

/* Base64 encode `length` bytes of `data` using the RFC 4648 §4
 * standard alphabet, unpadded (callers needing `=` padding append
 * `=` themselves to make the length a multiple of 4). Returns a
 * fresh counted AetherString (cast to char*) the caller releases
 * with string_release(), or NULL on OOM. */
char* cryptography_base64_encode_raw(const char* data, int length);

/* Padded sibling of cryptography_base64_encode_raw — RFC 4648 §4
 * standard alphabet WITH `=` padding to a multiple of 4 bytes. Used
 * by callers whose wire format expects padded base64 (most decoders
 * that aren't RFC-strict; some auth headers; common JSON-encoded
 * blob formats). Same lifetime contract as the unpadded form:
 * caller releases the returned string. Returns NULL on OOM. */
char* cryptography_base64_encode_padded_raw(const char* data, int length);

/* RFC 4648 §5 URL-safe siblings (`-_` alphabet): unpadded / padded. */
char* cryptography_base64url_encode_raw(const char* data, int length);
char* cryptography_base64url_encode_padded_raw(const char* data, int length);

/* Decode Base64 (standard alphabet) / Base64url. Returns an owned,
 * length-carrying AetherString holding the decoded bytes (embedded NULs
 * preserved), or NULL on malformed input / OOM. No shared state: the
 * result belongs to the caller alone. Both accept padded or unpadded
 * input and skip line breaks. */
char* cryptography_base64_decode_owned(const char* b64);
char* cryptography_base64url_decode_owned(const char* b64);

/* CSPRNG (#630). The cryptographically-secure RNG missing from
 * std.math (which is a seedable PRNG, fine for sampling, NOT for
 * secrets). Backed by the OS CSPRNG on every supported platform:
 *   - Linux:    getrandom(2) → /dev/urandom fallback
 *   - macOS/BSD: arc4random_buf(3)
 *   - Windows:  BCryptGenRandom (CNG)
 *
 * Same TLS split-accessor shape as the binary digests / base64.
 * Independent buffer slot — concurrent random_bytes and digest work
 * on the same thread don't clobber. No OpenSSL dependency: the OS
 * sources are the right choice and don't need libcrypto init. */
int cryptography_random_bytes_raw(int n);
const char* cryptography_get_random_bytes(void);
int         cryptography_get_random_bytes_length(void);
void        cryptography_release_random_bytes(void);

#endif /* AETHER_CRYPTOGRAPHY_H */
