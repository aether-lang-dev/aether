#include "aether_aes.h"
#include "../../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* FIPS-197 over a raw pointer. Correctness is pinned by the same KATs the
 * Aether implementation is pinned by (tests/regression/test_aes.ae). */

static const uint8_t SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t ISBOX[256];
static int g_isbox_ready = 0;

static void build_isbox(void) {
    if (g_isbox_ready) return;
    for (int i = 0; i < 256; i++) ISBOX[SBOX[i]] = (uint8_t)i;
    g_isbox_ready = 1;
}

typedef struct {
    uint8_t rk[240];   /* expanded key, up to 15 rounds x 16 bytes */
    int rounds;
    int encrypt;
} AeAes;

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static void expand_key(AeAes* c, const uint8_t* key, int keylen) {
    int nk = keylen / 4;
    int nr = nk + 6;
    c->rounds = nr;
    memcpy(c->rk, key, (size_t)keylen);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (nr + 1); i++) {
        uint8_t t[4];
        memcpy(t, c->rk + (i - 1) * 4, 4);
        if (i % nk == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[tmp];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) {
            c->rk[i * 4 + j] = (uint8_t)(c->rk[(i - nk) * 4 + j] ^ t[j]);
        }
    }
}

static void add_round_key(uint8_t* s, const uint8_t* rk, int rnd) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[rnd * 16 + i];
}

static void encrypt_block(const AeAes* c, const uint8_t* in, uint8_t* out) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, c->rk, 0);
    for (int rnd = 1; rnd <= c->rounds; rnd++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        /* ShiftRows, column-major state as in FIPS-197 */
        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13];  s[13] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;    t = s[6];  s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3];  s[3] = t;
        if (rnd != c->rounds) {
            /* MixColumns via xtime only. The gmul form costs a bit-loop per
               coefficient, which is ~29 million calls per megabyte. */
            for (int col = 0; col < 4; col++) {
                uint8_t* p = s + col * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t t = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ t ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ t ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ t ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ t ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        add_round_key(s, c->rk, rnd);
    }
    memcpy(out, s, 16);
}

static void decrypt_block(const AeAes* c, const uint8_t* in, uint8_t* out) {
    build_isbox();
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, c->rk, c->rounds);
    for (int rnd = c->rounds - 1; rnd >= 0; rnd--) {
        uint8_t t;
        /* InvShiftRows */
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2];  s[2] = s[10]; s[10] = t;   t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3];  s[3] = s[7];  s[7] = s[11]; s[11] = s[15]; s[15] = t;
        for (int i = 0; i < 16; i++) s[i] = ISBOX[s[i]];
        add_round_key(s, c->rk, rnd);
        if (rnd != 0) {
            /* InvMixColumns as the standard pre-step plus forward MixColumns,
               so it is xtime-only too. */
            for (int col = 0; col < 4; col++) {
                uint8_t* p = s + col * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t u = xtime(xtime((uint8_t)(a0 ^ a2)));
                uint8_t v = xtime(xtime((uint8_t)(a1 ^ a3)));
                a0 = (uint8_t)(a0 ^ u); a1 = (uint8_t)(a1 ^ v);
                a2 = (uint8_t)(a2 ^ u); a3 = (uint8_t)(a3 ^ v);
                uint8_t t = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ t ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ t ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ t ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ t ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
    }
    memcpy(out, s, 16);
}

int aether_aes_available(void) { return 1; }

void* aether_aes_new(const void* key, int keylen, int encrypt) {
    if (!key) return NULL;
    if (keylen != 16 && keylen != 24 && keylen != 32) return NULL;
    AeAes* c = (AeAes*)aether_caps_malloc(sizeof(AeAes));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->encrypt = encrypt ? 1 : 0;
    expand_key(c, (const uint8_t*)key, keylen);
    return c;
}

void aether_aes_free(void* ctx) {
    if (!ctx) return;
    /* Key material must not outlive the context. */
    memset(ctx, 0, sizeof(AeAes));
    aether_caps_free(ctx, sizeof(AeAes));
}

int aether_aes_block(void* ctx, const void* in, void* out) {
    AeAes* c = (AeAes*)ctx;
    if (!c || !in || !out) return 0;
    if (c->encrypt) encrypt_block(c, (const uint8_t*)in, (uint8_t*)out);
    else            decrypt_block(c, (const uint8_t*)in, (uint8_t*)out);
    return 1;
}

int aether_aes_cbc_encrypt(void* ctx, const void* iv, const void* in,
                           void* out, int n) {
    AeAes* c = (AeAes*)ctx;
    if (!c || !c->encrypt || !iv || !in || !out) return 0;
    if (n < 0 || n % 16 != 0) return 0;
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    const uint8_t* ip = (const uint8_t*)in;
    uint8_t* op = (uint8_t*)out;
    for (int off = 0; off < n; off += 16) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(ip[off + i] ^ prev[i]);
        encrypt_block(c, blk, op + off);
        memcpy(prev, op + off, 16);
    }
    return 1;
}

int aether_aes_cbc_decrypt(void* ctx, const void* iv, const void* in,
                           void* out, int n) {
    AeAes* c = (AeAes*)ctx;
    if (!c || c->encrypt || !iv || !in || !out) return 0;
    if (n < 0 || n % 16 != 0) return 0;
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    const uint8_t* ip = (const uint8_t*)in;
    uint8_t* op = (uint8_t*)out;
    for (int off = 0; off < n; off += 16) {
        memcpy(cur, ip + off, 16);          /* in and out may alias */
        decrypt_block(c, cur, op + off);
        for (int i = 0; i < 16; i++) op[off + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
    return 1;
}

int aether_aes_ctr_xor(void* ctx, const void* iv, const void* in,
                       void* out, int n) {
    AeAes* c = (AeAes*)ctx;
    if (!c || !c->encrypt || !iv || !in || !out) return 0;
    if (n < 0) return 0;
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    const uint8_t* ip = (const uint8_t*)in;
    uint8_t* op = (uint8_t*)out;
    for (int off = 0; off < n; off += 16) {
        encrypt_block(c, ctr, ks);
        int chunk = (n - off) < 16 ? (n - off) : 16;
        for (int i = 0; i < chunk; i++) op[off + i] = (uint8_t)(ip[off + i] ^ ks[i]);
        for (int i = 15; i >= 0; i--) { if (++ctr[i]) break; }
    }
    return 1;
}
