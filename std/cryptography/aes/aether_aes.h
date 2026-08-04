#ifndef AETHER_AES_H
#define AETHER_AES_H

#include <stddef.h>

/* Native AES core (#1394). The Aether implementation in module.ae reaches
 * every byte of state through bytes.get/bytes.set, which is about 15 million
 * calls per megabyte for AES-256 and measured 4.4 MB/s. This is the same
 * cipher over a raw pointer, used by the bulk modes when available; the
 * Aether path stays as the portable fallback and the reference the tests
 * compare against.
 *
 * Returns 0 on failure (bad key length, allocation, unsupported length). */

/* 1 when the native core is compiled in. 0 leaves every caller on the
 * Aether path, which is what a target without a C backend gets. */
int aether_aes_available(void);

/* keylen is 16, 24 or 32. `encrypt` selects the key schedule direction;
 * a decryptor is required for cbc_decrypt and ecb_decrypt. */
void* aether_aes_new(const void* key, int keylen, int encrypt);
void  aether_aes_free(void* ctx);

/* Single block, 16 bytes in and out. Direction follows the context. */
int aether_aes_block(void* ctx, const void* in, void* out);

/* `n` must be a multiple of 16. `iv` is 16 bytes and is not modified. */
int aether_aes_cbc_encrypt(void* ctx, const void* iv, const void* in,
                           void* out, int n);
int aether_aes_cbc_decrypt(void* ctx, const void* iv, const void* in,
                           void* out, int n);

/* CTR keystream XOR. `n` is any length; the counter is big-endian over the
 * whole 16-byte block, matching SP800-38A. Needs an ENCRYPT context. */
int aether_aes_ctr_xor(void* ctx, const void* iv, const void* in,
                       void* out, int n);

#endif
