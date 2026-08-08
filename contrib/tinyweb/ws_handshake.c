// ws_handshake.c — WebSocket handshake helpers for TinyWeb/Aether
// MIT License — Copyright (c) 2025 Aether Programming Language Contributors
//
// Provides the SHA-1 + Base64 needed for the Sec-WebSocket-Accept key.
// Provides SHA-1 + Base64 for the Sec-WebSocket-Accept key (RFC 6455).
//
// Link with: cc -c ws_handshake.c -o ws_handshake.o

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ---- Minimal SHA-1 (RFC 3174) ----

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Pre-processing: pad to 64-byte blocks
    size_t new_len = len + 1;
    while (new_len % 64 != 56) new_len++;
    uint8_t *msg = (uint8_t *)calloc(new_len + 8, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        msg[new_len + i] = (uint8_t)(bits >> (56 - 8 * i));

    for (size_t offset = 0; offset < new_len + 8; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[offset + 4*i] << 24) |
                    ((uint32_t)msg[offset + 4*i+1] << 16) |
                    ((uint32_t)msg[offset + 4*i+2] << 8) |
                    ((uint32_t)msg[offset + 4*i+3]);
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (t << 1) | (t >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);

    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[4*i]   = (uint8_t)(h[i] >> 24);
        out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >> 8);
        out[4*i+3] = (uint8_t)(h[i]);
    }
}

// ---- Base64 encode ----

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* ws_base64_encode(const uint8_t *data, int len) {
    int out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    int j = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i+1]) << 8;
        if (i + 2 < len) n |= (uint32_t)data[i+2];
        out[j++] = b64[(n >> 18) & 0x3F];
        out[j++] = b64[(n >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64[n & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

// ---- WebSocket accept key generation ----
// Concatenates client_key + RFC 6455 magic GUID, SHA-1 hashes, Base64 encodes.
// Concatenates client_key + RFC 6455 magic GUID, SHA-1 hashes, Base64 encodes.

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ---- WebSocket frame unmasking (RFC 6455 §5.3) ----
// Client-to-server frames are always masked: payload[i] ^= mask_key[i % 4].
// Returns a freshly malloc'd NUL-terminated copy of the unmasked payload.
// `len` is the true payload byte length (payloads may contain NUL bytes, so we
// do NOT rely on strlen — the caller passes the frame's declared length).
char* ws_unmask(const char *payload, const char *mask_key, int len) {
    if (len < 0) len = 0;
    char *out = (char *)malloc((size_t)len + 1);
    for (int i = 0; i < len; i++) {
        out[i] = (char)((unsigned char)payload[i] ^ (unsigned char)mask_key[i & 3]);
    }
    out[len] = '\0';
    return out;
}

char* ws_generate_accept_key(const char *client_key) {
    size_t key_len = strlen(client_key);
    size_t magic_len = strlen(WS_MAGIC);
    char *combined = (char *)malloc(key_len + magic_len + 1);
    memcpy(combined, client_key, key_len);
    memcpy(combined + key_len, WS_MAGIC, magic_len);
    combined[key_len + magic_len] = '\0';

    uint8_t hash[20];
    sha1((const uint8_t *)combined, key_len + magic_len, hash);
    free(combined);

    return ws_base64_encode(hash, 20);
}
