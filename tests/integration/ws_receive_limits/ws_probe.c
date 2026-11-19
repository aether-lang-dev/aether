/* The client for ws_receive_limits (#3739): raw WebSocket frames no library
 * client would send, against wsserver.ae (cap 4096 on /ws, 64 MiB on
 * /wsbig).
 *
 *   1. a 4000-byte message in two fragments is reassembled and echoed;
 *   2. a 6000-byte message in three fragments closes with 1009 at the
 *      fragment that passes the cap;
 *   3. one frame declaring 1 MiB closes with 1009 before its payload;
 *   4. a 125-byte ping is answered with a 125-byte pong;
 *   5. a 126-byte ping closes with 1002;
 *   6. a fragmented ping closes with 1002;
 *   7. on /wsbig, a frame declaring 48 MiB with 1000 bytes of it sent
 *      leaves the server holding far less than 48 MiB.
 *
 * Usage: ws_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"

static int port;
static int failures;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static raw_sock dial(void) {
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RAW_SOCK_INVALID) return s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        raw_socket_close(s);
        return RAW_SOCK_INVALID;
    }
    raw_recv_timeout_ms(s, 5000);
    return s;
}

/* Exactly n bytes, or -1. */
static int recv_n(raw_sock s, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = (int)recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Open a WebSocket on `path`: the upgrade request, then the head of the
 * 101, read a byte at a time so no frame after it is swallowed. */
static raw_sock ws_open(const char* path) {
    raw_sock s = dial();
    if (s == RAW_SOCK_INVALID) return s;
    raw_sendf(s, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n\r\n", path);
    char head[2048];
    size_t n = 0;
    while (n < sizeof(head) - 1) {
        if (recv_n(s, (unsigned char*)head + n, 1) != 0) break;
        n++;
        head[n] = '\0';
        if (n >= 4 && memcmp(head + n - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[n] = '\0';
    if (strncmp(head, "HTTP/1.1 101", 12) != 0) {
        printf("  upgrade of %s failed: %s\n", path, head);
        raw_socket_close(s);
        return RAW_SOCK_INVALID;
    }
    return s;
}

/* A masked client frame whose header declares `declared` payload bytes, of
 * which the first `len` (from `payload`) are sent. */
static int send_frame(raw_sock s, int fin, int opcode, const unsigned char* payload,
                      size_t len, unsigned long long declared) {
    unsigned char hdr[14];
    size_t h = 0;
    static const unsigned char key[4] = { 0x11, 0x22, 0x33, 0x44 };
    hdr[h++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0F));
    if (declared < 126) {
        hdr[h++] = (unsigned char)(0x80 | declared);
    } else if (declared <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(declared >> 8);
        hdr[h++] = (unsigned char)declared;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)(declared >> (8 * i));
    }
    memcpy(hdr + h, key, 4);
    h += 4;
    if (raw_send_all(s, (const char*)hdr, h) != 0) return -1;
    unsigned char* m = (unsigned char*)malloc(len ? len : 1);
    if (!m) return -1;
    for (size_t i = 0; i < len; i++) m[i] = (unsigned char)(payload[i] ^ key[i & 3]);
    int rc = len ? raw_send_all(s, (const char*)m, len) : 0;
    free(m);
    return rc;
}

/* One server frame: its opcode, and its payload into buf (up to cap).
 * Returns the payload length, or -1 on EOF or timeout. */
static long read_frame(raw_sock s, int* opcode, unsigned char* buf, size_t cap) {
    unsigned char h[2];
    if (recv_n(s, h, 2) != 0) return -1;
    *opcode = h[0] & 0x0F;
    unsigned long long pl = h[1] & 0x7F;
    if (pl == 126) {
        unsigned char e[2];
        if (recv_n(s, e, 2) != 0) return -1;
        pl = ((unsigned long long)e[0] << 8) | e[1];
    } else if (pl == 127) {
        unsigned char e[8];
        if (recv_n(s, e, 8) != 0) return -1;
        pl = 0;
        for (int i = 0; i < 8; i++) pl = (pl << 8) | e[i];
    }
    if (pl > cap) return -1;
    if (pl && recv_n(s, buf, (size_t)pl) != 0) return -1;
    return (long)pl;
}

/* The close code the server sends next, or -1 when it sends something
 * else, nothing, or closes without a close frame. */
static int close_code(raw_sock s) {
    unsigned char buf[256];
    int op = 0;
    long n = read_frame(s, &op, buf, sizeof(buf));
    if (n < 0) return -1;
    if (op != 0x8) {
        printf("  (got opcode %d, %ld bytes, where a close was due)\n", op, n);
        return -1;
    }
    if (n < 2) return 0;
    return (buf[0] << 8) | buf[1];
}

/* The allocator's byte count, from GET /used. */
static long long used_bytes(void) {
    raw_sock s = dial();
    if (s == RAW_SOCK_INVALID) return -1;
    raw_sendf(s, "GET /used HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    char resp[4096];
    size_t n = 0;
    for (;;) {
        int r = (int)recv(s, resp + n, (int)(sizeof(resp) - 1 - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= sizeof(resp) - 1) break;
    }
    resp[n] = '\0';
    raw_socket_close(s);
    char* body = strstr(resp, "\r\n\r\n");
    return body ? atoll(body + 4) : -1;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    port = atoi(argv[1]);
    if (raw_socket_start() != 0) return 2;
    unsigned char data[6000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (unsigned char)('a' + i % 26);
    unsigned char buf[256];
    int op = 0;

    /* 1. Under the cap, fragmented: reassembled and echoed. */
    raw_sock s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 0, 0x1, data, 2000, 2000);
    send_frame(s, 1, 0x0, data + 2000, 2000, 2000);
    long n = read_frame(s, &op, buf, sizeof(buf) - 1);
    if (n >= 0) buf[n] = '\0';
    check(n >= 0 && op == 0x1 && strcmp((char*)buf, "len=4000") == 0,
          "a 4000-byte message in two fragments is echoed as len=4000");

    /* 2. Fragments that add up past the cap: 1009. */
    send_frame(s, 0, 0x1, data, 2000, 2000);
    send_frame(s, 0, 0x0, data + 2000, 2000, 2000);
    /* The header alone: the server refuses on the declared length, and
     * payload it never reads would turn its close into a reset. */
    send_frame(s, 1, 0x0, data + 4000, 0, 2000);
    check(close_code(s) == 1009, "6000 bytes in three fragments close with 1009");
    raw_socket_close(s);

    /* 3. One frame declaring past the cap: 1009 before its payload. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x2, data, 0, 1024ULL * 1024ULL);
    check(close_code(s) == 1009, "a frame declaring 1 MiB closes with 1009 before its payload");
    raw_socket_close(s);

    /* 4. A ping at the 125-byte limit is answered. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x9, data, 125, 125);
    n = read_frame(s, &op, buf, sizeof(buf));
    check(n == 125 && op == 0xA && memcmp(buf, data, 125) == 0,
          "a 125-byte ping is answered with its 125-byte pong");

    /* 5. A ping past it: 1002. */
    send_frame(s, 1, 0x9, data, 0, 126);
    check(close_code(s) == 1002, "a 126-byte ping closes with 1002");
    raw_socket_close(s);

    /* 6. A fragmented ping: 1002. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 0, 0x9, data, 0, 4);
    check(close_code(s) == 1002, "a fragmented ping closes with 1002");
    raw_socket_close(s);

    /* 7. A large declared frame costs what was sent, not what was declared. */
    long long before = used_bytes();
    s = ws_open("/wsbig");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x2, data, 1000, 48ULL * 1024ULL * 1024ULL);
    raw_sleep_ms(500);
    long long during = used_bytes();
    raw_socket_close(s);
    printf("  allocator held %lld bytes before, %lld with the frame open\n", before, during);
    check(before >= 0 && during >= 0 && during - before < 8LL * 1024 * 1024,
          "a frame declaring 48 MiB with 1000 bytes sent holds well under 8 MiB");

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: ws receive limits\n");
    return 0;
}
