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
 *   8. a frame with reserved data opcode 0x3 closes with 1002 (#3759);
 *   9. a frame with reserved control opcode 0xB closes with 1002;
 *  10. an unmasked client frame closes with 1002 (RFC 6455 5.1).
 *  11-13. a frame with RSV1, RSV2 or RSV3 set closes with 1002, since no
 *      extension is negotiated (RFC 6455 5.2, #3764);
 *  14. a continuation with no message in progress closes with 1002 (5.4);
 *  15. a new data frame inside a fragmented message closes with 1002;
 *  16. a ping between fragments is answered and the message reassembles.
 *  17. code points split across fragments reassemble as UTF-8;
 *  18. a binary message is not checked as UTF-8;
 *  19-24. text that is not UTF-8 (a byte 0xFF, an overlong form, a
 *      surrogate, a code point past U+10FFFF, a message ending inside a
 *      code point) and a close reason that is not UTF-8 close with 1007
 *      (RFC 6455 8.1).
 *
 * Usage: ws_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 *
 * ws_probe relay <port> checks the client side of 5.1 (#3759): a client
 * fails the connection on a masked frame from the server. It listens on a
 * loopback port of its own and prints READY <port>; the client under test
 * dials it. The upgrade request goes on to the server on <port> and its 101
 * comes back, so the handshake is a real one; then, in the server's place,
 * the relay sends the client one masked text frame, and expects the
 * client's close with 1002. Prints PASS/FAIL; exits 0 on a pass.
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
 * which the first `len` (from `payload`) are sent, with the reserved bits
 * `rsv` (0x40 RSV1, 0x20 RSV2, 0x10 RSV3) set in its first byte. */
static int send_frame_rsv(raw_sock s, int fin, int rsv, int opcode,
                          const unsigned char* payload, size_t len,
                          unsigned long long declared) {
    unsigned char hdr[14];
    size_t h = 0;
    static const unsigned char key[4] = { 0x11, 0x22, 0x33, 0x44 };
    hdr[h++] = (unsigned char)((fin ? 0x80 : 0) | (rsv & 0x70) | (opcode & 0x0F));
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

/* The same with no reserved bit set: every frame a conforming client sends
 * when no extension was negotiated. */
static int send_frame(raw_sock s, int fin, int opcode, const unsigned char* payload,
                      size_t len, unsigned long long declared) {
    return send_frame_rsv(s, fin, 0, opcode, payload, len, declared);
}

/* A frame header with no mask, as a server sends it and a client must not:
 * `opcode`, FIN set, no payload. */
static int send_unmasked_header(raw_sock s, int opcode) {
    unsigned char hdr[2] = { (unsigned char)(0x80 | (opcode & 0x0F)), 0 };
    return raw_send_all(s, (const char*)hdr, 2);
}

/* One frame: its opcode, and its payload into buf (up to cap), unmasked when
 * it came masked (a client's). Returns the payload length, or -1 on EOF or
 * timeout. */
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
    unsigned char key[4] = { 0, 0, 0, 0 };
    if ((h[1] & 0x80) && recv_n(s, key, 4) != 0) return -1;
    if (pl > cap) return -1;
    if (pl && recv_n(s, buf, (size_t)pl) != 0) return -1;
    for (size_t i = 0; i < pl; i++) buf[i] ^= key[i & 3];
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

/* A head (up to its blank line) from `s`, a byte at a time so nothing after
 * it is taken. 0 on success. */
static int read_head(raw_sock s, char* head, size_t cap) {
    size_t n = 0;
    while (n < cap - 1) {
        if (recv_n(s, (unsigned char*)head + n, 1) != 0) return -1;
        n++;
        head[n] = '\0';
        if (n >= 4 && memcmp(head + n - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

/* ws_probe relay <port>: see the comment at the top. */
static int relay(void) {
    raw_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == RAW_SOCK_INVALID) return 2;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t al = sizeof(a);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr*)&a, &al) != 0) {
        printf("FAIL: relay could not listen\n");
        return 1;
    }
    printf("READY %d\n", ntohs(a.sin_port));
    fflush(stdout);
    raw_sock c = accept(ls, NULL, NULL);
    raw_socket_close(ls);
    if (c == RAW_SOCK_INVALID) return 1;
    raw_recv_timeout_ms(c, 5000);
    char head[4096];
    if (read_head(c, head, sizeof(head)) != 0) {
        printf("FAIL: no upgrade request from the client\n");
        return 1;
    }
    raw_sock s = dial();
    if (s == RAW_SOCK_INVALID) return 1;
    raw_send_all(s, head, strlen(head));
    if (read_head(s, head, sizeof(head)) != 0 || strncmp(head, "HTTP/1.1 101", 12) != 0) {
        printf("FAIL: the server did not upgrade: %s\n", head);
        return 1;
    }
    raw_send_all(c, head, strlen(head));
    /* A masked text frame "hi", which a server must never send. */
    static const unsigned char frame[] = { 0x81, 0x82, 0x01, 0x02, 0x03, 0x04,
                                           'h' ^ 0x01, 'i' ^ 0x02 };
    raw_send_all(c, (const char*)frame, sizeof(frame));
    unsigned char buf[256];
    int op = 0;
    long n = read_frame(c, &op, buf, sizeof(buf));
    int code = (n >= 2 && op == 0x8) ? (buf[0] << 8) | buf[1] : -1;
    raw_socket_close(c);
    raw_socket_close(s);
    if (code != 1002) {
        printf("FAIL: the client answered a masked server frame with opcode %d, code %d, not a close with 1002\n",
               n >= 0 ? op : -1, code);
        return 1;
    }
    printf("PASS: a client closes with 1002 on a masked server frame\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (raw_socket_start() != 0) return 2;
    if (strcmp(argv[1], "relay") == 0) {
        if (argc < 3) return 2;
        port = atoi(argv[2]);
        return relay();
    }
    port = atoi(argv[1]);
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

    /* 8, 9. Reserved opcodes: 1002 (RFC 6455 5.2). The frames carry no
     * payload, so the server has read all of each when it closes. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x3, data, 0, 0);
    check(close_code(s) == 1002, "a frame with reserved data opcode 0x3 closes with 1002");
    raw_socket_close(s);

    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0xB, data, 0, 0);
    check(close_code(s) == 1002, "a frame with reserved control opcode 0xB closes with 1002");
    raw_socket_close(s);

    /* 10. An unmasked client frame: 1002 (RFC 6455 5.1). */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_unmasked_header(s, 0x1);
    check(close_code(s) == 1002, "an unmasked client frame closes with 1002");
    raw_socket_close(s);

    /* 11-13. A reserved bit with no extension negotiated: 1002 (RFC 6455
     * 5.2, #3764). The server negotiates none, so RSV1 (permessage-deflate's
     * bit) is as reserved as the others, on a data frame and on a control
     * frame alike. */
    static const struct { int rsv; int opcode; const char* what; } rsv_cases[] = {
        { 0x40, 0x1, "a text frame with RSV1 set closes with 1002" },
        { 0x20, 0x2, "a binary frame with RSV2 set closes with 1002" },
        { 0x10, 0x9, "a ping with RSV3 set closes with 1002" },
    };
    for (size_t i = 0; i < sizeof(rsv_cases) / sizeof(rsv_cases[0]); i++) {
        s = ws_open("/ws");
        if (s == RAW_SOCK_INVALID) return 1;
        send_frame_rsv(s, 1, rsv_cases[i].rsv, rsv_cases[i].opcode, data, 0, 0);
        check(close_code(s) == 1002, rsv_cases[i].what);
        raw_socket_close(s);
    }

    /* 14. A continuation with no message in progress: 1002 (RFC 6455 5.4).
     * Taken as a message of its own, it was echoed as len=5. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x0, data, 5, 5);
    check(close_code(s) == 1002, "a continuation frame with no message in progress closes with 1002");
    raw_socket_close(s);

    /* 15. A new data frame in the middle of a fragmented message: 1002. It
     * restarted the message, dropping the first fragment. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 0, 0x1, data, 5, 5);
    send_frame(s, 1, 0x1, data, 7, 7);
    check(close_code(s) == 1002, "a text frame inside a fragmented message closes with 1002");
    raw_socket_close(s);

    /* 16. A control frame between fragments is allowed (5.4) and the message
     * still reassembles: the ping is answered, then len=10 comes back. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 0, 0x2, data, 5, 5);
    send_frame(s, 1, 0x9, data, 3, 3);
    send_frame(s, 1, 0x0, data + 5, 5, 5);
    n = read_frame(s, &op, buf, sizeof(buf) - 1);
    int pong_ok = (n == 3 && op == 0xA);
    n = read_frame(s, &op, buf, sizeof(buf) - 1);
    if (n >= 0) buf[n] = '\0';
    check(pong_ok && n >= 0 && op == 0x1 && strcmp((char*)buf, "len=10") == 0,
          "a ping between two fragments is answered and the message reassembles");
    raw_socket_close(s);

    /* 17. A text message is UTF-8 across its fragments (RFC 6455 8.1): "é"
     * split between two fragments, "€" between the next two, is whole. */
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 0, 0x1, (const unsigned char*)"caf\xc3", 4, 4);
    send_frame(s, 0, 0x0, (const unsigned char*)"\xa9 \xe2\x82", 4, 4);
    send_frame(s, 1, 0x0, (const unsigned char*)"\xac", 1, 1);
    n = read_frame(s, &op, buf, sizeof(buf) - 1);
    if (n >= 0) buf[n] = '\0';
    check(n >= 0 && op == 0x1 && strcmp((char*)buf, "len=9") == 0,
          "code points split across fragments reassemble as UTF-8 (len=9)");

    /* 18. Binary is not text: bytes that are not UTF-8 are echoed. */
    send_frame(s, 1, 0x2, (const unsigned char*)"\xff\xfe\xc0", 3, 3);
    n = read_frame(s, &op, buf, sizeof(buf) - 1);
    if (n >= 0) buf[n] = '\0';
    check(n >= 0 && op == 0x1 && strcmp((char*)buf, "len=3") == 0,
          "a binary message is not checked as UTF-8");
    raw_socket_close(s);

    /* 19-24. Text that is not UTF-8 closes with 1007: a byte that never
     * begins a sequence, an overlong form, a surrogate, a code point past
     * U+10FFFF, a message ending inside a code point, and a close reason
     * that is not UTF-8. Each was handed to the program as text. */
    static const struct { const char* bytes; size_t len; int fragments; const char* what; } bad[] = {
        { "ok \xff", 4, 1, "a text byte 0xFF closes with 1007" },
        { "\xc0\xaf", 2, 1, "an overlong form closes with 1007" },
        { "\xed\xa0\x80", 3, 1, "a surrogate (U+D800) closes with 1007" },
        { "\xf4\x90\x80\x80", 4, 1, "a code point past U+10FFFF closes with 1007" },
        { "abc\xe2\x82", 5, 2, "a text message ending inside a code point closes with 1007" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s = ws_open("/ws");
        if (s == RAW_SOCK_INVALID) return 1;
        if (bad[i].fragments == 2) {
            send_frame(s, 0, 0x1, (const unsigned char*)bad[i].bytes, 3, 3);
            send_frame(s, 1, 0x0, (const unsigned char*)bad[i].bytes + 3, bad[i].len - 3, bad[i].len - 3);
        } else {
            send_frame(s, 1, 0x1, (const unsigned char*)bad[i].bytes, bad[i].len, bad[i].len);
        }
        check(close_code(s) == 1007, bad[i].what);
        raw_socket_close(s);
    }
    s = ws_open("/ws");
    if (s == RAW_SOCK_INVALID) return 1;
    send_frame(s, 1, 0x8, (const unsigned char*)"\x03\xe8\xc3(", 4, 4);
    check(close_code(s) == 1007, "a close reason that is not UTF-8 closes with 1007");
    raw_socket_close(s);

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: ws receive limits\n");
    return 0;
}
