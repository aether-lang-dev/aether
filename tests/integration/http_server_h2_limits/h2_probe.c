/* The client for http_server_h2_limits (#3738): raw HTTP/2 frames over a
 * cleartext prior-knowledge connection, against server.ae (h2 on, body
 * limit 20000 bytes).
 *
 *   1. the server's SETTINGS carry MAX_CONCURRENT_STREAMS = 100, although
 *      the program asked for no particular number;
 *   2. stream 1 sends 30000 bytes of body in two DATA frames, without
 *      ending the stream: the server resets it (RST_STREAM, CANCEL) rather
 *      than holding them;
 *   3. stream 3 then sends 1000 bytes and ends: answered "len=1000", so the
 *      connection is still good.
 *
 * Usage: h2_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"

static int failures;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int recv_n(raw_sock s, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = (int)recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int send_frame(raw_sock s, int type, int flags, unsigned stream,
                      const unsigned char* payload, size_t len) {
    unsigned char h[9];
    h[0] = (unsigned char)(len >> 16);
    h[1] = (unsigned char)(len >> 8);
    h[2] = (unsigned char)len;
    h[3] = (unsigned char)type;
    h[4] = (unsigned char)flags;
    h[5] = (unsigned char)((stream >> 24) & 0x7F);
    h[6] = (unsigned char)(stream >> 16);
    h[7] = (unsigned char)(stream >> 8);
    h[8] = (unsigned char)stream;
    if (raw_send_all(s, (const char*)h, 9) != 0) return -1;
    return len ? raw_send_all(s, (const char*)payload, len) : 0;
}

/* One frame: its type, flags and stream, its payload into buf (up to
 * cap). Returns the payload length, or -1 on EOF, timeout or overflow. */
static long read_frame(raw_sock s, int* type, int* flags, unsigned* stream,
                       unsigned char* buf, size_t cap) {
    unsigned char h[9];
    if (recv_n(s, h, 9) != 0) return -1;
    size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
    *type = h[3];
    *flags = h[4];
    *stream = ((unsigned)(h[5] & 0x7F) << 24) | ((unsigned)h[6] << 16) |
              ((unsigned)h[7] << 8) | h[8];
    if (len > cap) return -1;
    if (len && recv_n(s, buf, len) != 0) return -1;
    return (long)len;
}

/* HPACK "literal header field without indexing, new name", no Huffman:
 * enough for a request a server must decode. Names and values are short. */
static size_t hpack_literal(unsigned char* out, const char* name, const char* value) {
    size_t n = 0, nl = strlen(name), vl = strlen(value);
    out[n++] = 0x00;
    out[n++] = (unsigned char)nl;
    memcpy(out + n, name, nl); n += nl;
    out[n++] = (unsigned char)vl;
    memcpy(out + n, value, vl); n += vl;
    return n;
}

static int send_post_headers(raw_sock s, unsigned stream, const char* path) {
    unsigned char block[256];
    size_t n = 0;
    n += hpack_literal(block + n, ":method", "POST");
    n += hpack_literal(block + n, ":scheme", "http");
    n += hpack_literal(block + n, ":path", path);
    n += hpack_literal(block + n, ":authority", "127.0.0.1");
    return send_frame(s, 0x1, 0x4 /* END_HEADERS */, stream, block, n);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (raw_socket_start() != 0) return 2;
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)atoi(argv[1]));
    if (s == RAW_SOCK_INVALID || connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("FAIL: connect\n");
        return 1;
    }
    raw_recv_timeout_ms(s, 5000);

    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    raw_send_all(s, preface, sizeof(preface) - 1);
    send_frame(s, 0x4, 0, 0, NULL, 0);   /* our SETTINGS, empty */

    static unsigned char buf[65536];
    int type = 0, flags = 0;
    unsigned stream = 0;
    long n;

    /* 1. The server's first SETTINGS. */
    long max_streams = -1;
    for (;;) {
        n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) break;
        if (type == 0x4 && !(flags & 0x1)) {
            for (long i = 0; i + 6 <= n; i += 6) {
                unsigned id = ((unsigned)buf[i] << 8) | buf[i + 1];
                unsigned long v = ((unsigned long)buf[i + 2] << 24) | ((unsigned long)buf[i + 3] << 16) |
                                  ((unsigned long)buf[i + 4] << 8) | buf[i + 5];
                if (id == 0x3) max_streams = (long)v;
            }
            break;
        }
    }
    printf("  server SETTINGS_MAX_CONCURRENT_STREAMS: %ld\n", max_streams);
    check(max_streams == 100, "SETTINGS advertise MAX_CONCURRENT_STREAMS 100 by default");
    send_frame(s, 0x4, 0x1, 0, NULL, 0);   /* SETTINGS ACK */

    /* 2. A body past the limit, the stream left open. */
    static unsigned char body[15000];
    memset(body, 'x', sizeof(body));
    send_post_headers(s, 1, "/len");
    send_frame(s, 0x0, 0, 1, body, sizeof(body));
    send_frame(s, 0x0, 0, 1, body, sizeof(body));
    long rst_code = -1;
    int answered_1 = 0;
    for (;;) {
        n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) break;
        if (stream == 1 && (type == 0x1 || type == 0x0)) answered_1 = 1;
        if (type == 0x3 && stream == 1 && n == 4) {
            rst_code = ((long)buf[0] << 24) | ((long)buf[1] << 16) | ((long)buf[2] << 8) | buf[3];
            break;
        }
    }
    printf("  stream 1 RST_STREAM code: %ld\n", rst_code);
    check(rst_code == 0x8 && !answered_1,
          "30000 bytes on a stream past the 20000 limit reset it with CANCEL");

    /* 3. The connection still serves a stream under the limit. */
    send_post_headers(s, 3, "/len");
    send_frame(s, 0x0, 0x1 /* END_STREAM */, 3, body, 1000);
    char got[64] = "";
    for (;;) {
        n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) break;
        if (type == 0x0 && stream == 3) {
            size_t k = (size_t)n < sizeof(got) - 1 ? (size_t)n : sizeof(got) - 1;
            memcpy(got, buf, k);
            got[k] = '\0';
            break;
        }
    }
    check(strcmp(got, "len=1000") == 0, "a 1000-byte body on the next stream is answered len=1000");
    raw_socket_close(s);

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: h2 limits\n");
    return 0;
}
