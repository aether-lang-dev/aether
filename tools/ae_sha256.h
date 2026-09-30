/* ae_sha256.h — SHA-256 (FIPS 180-4) for `ae`'s own use (#2322).
 *
 * `ae add` verifies a downloaded release artifact against its published
 * `.sha256`. It used to shell out to `sha256sum` / `shasum -a 256` through
 * system(), which on Windows is cmd.exe: MSYS's /usr/bin is not on its PATH,
 * so no hasher was found and every binary-package install was refused.
 * Hashing in-process needs no tool, no shell and no PATH on any platform. */
#ifndef AE_SHA256_H
#define AE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;          /* message length so far */
    unsigned char block[64];
    size_t used;             /* bytes waiting in `block` */
} AeSha256;

void ae_sha256_init(AeSha256* ctx);
void ae_sha256_update(AeSha256* ctx, const void* data, size_t len);
void ae_sha256_final(AeSha256* ctx, unsigned char digest[32]);

/* The lowercase hex digest of the file at `path` into `hex` (65 bytes with
 * the NUL). Returns 0, or -1 when the file cannot be opened or read. */
int ae_sha256_file_hex(const char* path, char hex[65]);

#endif /* AE_SHA256_H */
