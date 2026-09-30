/* ae_sha256.c — SHA-256 (FIPS 180-4) for `ae`'s own use (#2322).
 * Why `ae` hashes in-process is in ae_sha256.h. */
#include "ae_sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(uint32_t s[8], const unsigned char b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
               ((uint32_t)b[4 * i + 2] << 8) | (uint32_t)b[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b2 = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b2) ^ (a & c) ^ (b2 & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b2; b2 = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b2; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void ae_sha256_init(AeSha256* ctx) {
    static const uint32_t H0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->state, H0, sizeof(H0));
    ctx->bytes = 0;
    ctx->used = 0;
}

void ae_sha256_update(AeSha256* ctx, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    ctx->bytes += len;
    while (len > 0) {
        size_t take = 64 - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64) {
            compress(ctx->state, ctx->block);
            ctx->used = 0;
        }
    }
}

void ae_sha256_final(AeSha256* ctx, unsigned char digest[32]) {
    uint64_t bits = ctx->bytes * 8;
    unsigned char pad = 0x80;
    ae_sha256_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->used != 56) ae_sha256_update(ctx, &zero, 1);
    unsigned char len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (unsigned char)(bits >> (56 - 8 * i));
    ae_sha256_update(ctx, len_be, 8);
    for (int i = 0; i < 8; i++) {
        digest[4 * i] = (unsigned char)(ctx->state[i] >> 24);
        digest[4 * i + 1] = (unsigned char)(ctx->state[i] >> 16);
        digest[4 * i + 2] = (unsigned char)(ctx->state[i] >> 8);
        digest[4 * i + 3] = (unsigned char)ctx->state[i];
    }
}

int ae_sha256_file_hex(const char* path, char hex[65]) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    AeSha256 ctx;
    ae_sha256_init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) ae_sha256_update(&ctx, buf, n);
    int failed = ferror(f);
    fclose(f);
    if (failed) return -1;
    unsigned char digest[32];
    ae_sha256_final(&ctx, digest);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    return 0;
}
