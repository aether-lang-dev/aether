/* The client for http_server_h2_limits (#3738): raw HTTP/2 frames over a
 * cleartext prior-knowledge connection, against server.ae (h2 on, body
 * limit 20000 bytes, idle timeout 1 s, head and body budgets 1 s each,
 * connections live 3 s).
 *
 * One connection:
 *   1. the server's SETTINGS carry MAX_CONCURRENT_STREAMS = 100, although
 *      the program asked for no particular number, and
 *      MAX_HEADER_LIST_SIZE = 32768;
 *   2. stream 1 sends 30000 bytes of body in two DATA frames, without
 *      ending the stream: the server resets it (RST_STREAM, CANCEL) rather
 *      than holding them;
 *   3. stream 3 then sends 1000 bytes and ends: answered "len=1000", so the
 *      connection is still good;
 *   4. stream 5 sends a header list of about 41 KiB (HEADERS and
 *      CONTINUATION): answered 431, never dispatched;
 *   5. streams 7 and 9 each send 15000 bytes and leave the stream open:
 *      the connection would hold 30000, past the 20000 limit, so stream 9
 *      is reset with REFUSED_STREAM, and stream 7, ended, is answered
 *      "len=15000".
 * Each on a connection of its own, timed:
 *   6. a stream that sends one byte of DATA every 200 ms, well inside the
 *      idle timeout, never ending: reset with CANCEL within its head and
 *      body budgets (2 s) and a margin;
 *   7. a connection with no stream that sends a PING every 200 ms: GOAWAY
 *      and closed within the idle timeout and a margin; a PING is not a
 *      request;
 *   8. a connection that sends a request every 300 ms, each answered, and
 *      one more begun just before its age and finished after the GOAWAY:
 *      GOAWAY at its 3 s age, the open request still answered (the GOAWAY
 *      is graceful), closed within the idle timeout after.
 * The old code reset nothing in 6 and closed neither 7 nor 8.
 *
 * Usage: h2_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"
#ifndef _WIN32
#include <sys/select.h>
#include <time.h>
#endif

static int failures;
static int port;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* 1 when `s` is readable within `ms`. */
static int readable(raw_sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, &rf, NULL, NULL, &tv) > 0;
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

/* An HPACK integer with an `bits`-bit prefix, the first byte's other bits
 * `flags` (RFC 7541 5.1). */
static size_t hpack_int(unsigned char* out, int bits, unsigned char flags, size_t v) {
    size_t n = 0, max = ((size_t)1 << bits) - 1;
    if (v < max) { out[n++] = (unsigned char)(flags | v); return n; }
    out[n++] = (unsigned char)(flags | max);
    v -= max;
    while (v >= 128) { out[n++] = (unsigned char)(0x80 | (v & 0x7F)); v >>= 7; }
    out[n++] = (unsigned char)v;
    return n;
}

/* HPACK "literal header field without indexing, new name", no Huffman. */
static size_t hpack_literal(unsigned char* out, const char* name, const char* value) {
    size_t n = 0, nl = strlen(name), vl = strlen(value);
    out[n++] = 0x00;
    n += hpack_int(out + n, 7, 0, nl);
    memcpy(out + n, name, nl); n += nl;
    n += hpack_int(out + n, 7, 0, vl);
    memcpy(out + n, value, vl); n += vl;
    return n;
}

static int send_headers(raw_sock s, unsigned stream, const char* method,
                        const char* path, int end_stream) {
    unsigned char block[256];
    size_t n = 0;
    n += hpack_literal(block + n, ":method", method);
    n += hpack_literal(block + n, ":scheme", "http");
    n += hpack_literal(block + n, ":path", path);
    n += hpack_literal(block + n, ":authority", "127.0.0.1");
    return send_frame(s, 0x1, 0x4 /* END_HEADERS */ | (end_stream ? 0x1 : 0), stream, block, n);
}

static int send_post_headers(raw_sock s, unsigned stream, const char* path) {
    return send_headers(s, stream, "POST", path, 0);
}

/* A connection, the preface sent and the server's SETTINGS read and
 * acknowledged; their MAX_CONCURRENT_STREAMS and MAX_HEADER_LIST_SIZE into
 * the pointers when given. */
static raw_sock h2_open(long* max_streams, long* max_list) {
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (s == RAW_SOCK_INVALID || connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("FAIL: connect\n");
        return RAW_SOCK_INVALID;
    }
    raw_recv_timeout_ms(s, 5000);
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    raw_send_all(s, preface, sizeof(preface) - 1);
    send_frame(s, 0x4, 0, 0, NULL, 0);   /* our SETTINGS, empty */
    static unsigned char buf[65536];
    int type = 0, flags = 0;
    unsigned stream = 0;
    for (;;) {
        long n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) break;
        if (type == 0x4 && !(flags & 0x1)) {
            for (long i = 0; i + 6 <= n; i += 6) {
                unsigned id = ((unsigned)buf[i] << 8) | buf[i + 1];
                unsigned long v = ((unsigned long)buf[i + 2] << 24) | ((unsigned long)buf[i + 3] << 16) |
                                  ((unsigned long)buf[i + 4] << 8) | buf[i + 5];
                if (id == 0x3 && max_streams) *max_streams = (long)v;
                if (id == 0x6 && max_list) *max_list = (long)v;
            }
            break;
        }
    }
    send_frame(s, 0x4, 0x1, 0, NULL, 0);   /* SETTINGS ACK */
    return s;
}

/* Frames until one for `want_stream` of type `want_type` (or a GOAWAY when
 * want_type is 0x7); its payload in buf. -1 when the connection ends. */
static long await_frame(raw_sock s, unsigned want_stream, int want_type, int* flags,
                        unsigned char* buf, size_t cap) {
    int type = 0;
    unsigned stream = 0;
    for (;;) {
        long n = read_frame(s, &type, flags, &stream, buf, cap);
        if (n < 0) return -1;
        if (type == want_type && (want_type == 0x7 || stream == want_stream)) return n;
    }
}

static long rst_code_of(const unsigned char* buf) {
    return ((long)buf[0] << 24) | ((long)buf[1] << 16) | ((long)buf[2] << 8) | buf[3];
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (raw_socket_start() != 0) return 2;
    port = atoi(argv[1]);

    static unsigned char buf[65536];
    int type = 0, flags = 0;
    unsigned stream = 0;
    long n;

    /* 1. The server's first SETTINGS. */
    long max_streams = -1, max_list = -1;
    raw_sock s = h2_open(&max_streams, &max_list);
    if (s == RAW_SOCK_INVALID) return 1;
    printf("  server SETTINGS_MAX_CONCURRENT_STREAMS: %ld, MAX_HEADER_LIST_SIZE: %ld\n",
           max_streams, max_list);
    check(max_streams == 100, "SETTINGS advertise MAX_CONCURRENT_STREAMS 100 by default");
    check(max_list == 32768, "SETTINGS advertise MAX_HEADER_LIST_SIZE 32768 by default");

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
            rst_code = rst_code_of(buf);
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
    n = await_frame(s, 3, 0x0, &flags, buf, sizeof(buf));
    if (n >= 0) {
        size_t k = (size_t)n < sizeof(got) - 1 ? (size_t)n : sizeof(got) - 1;
        memcpy(got, buf, k);
        got[k] = '\0';
    }
    check(strcmp(got, "len=1000") == 0, "a 1000-byte body on the next stream is answered len=1000");

    /* 4. A header list past 32 KiB: 41 fields of 1000 bytes, in a HEADERS
     * frame and CONTINUATIONs of 16384 bytes. */
    {
        static unsigned char block[48 * 1024];
        static char value[1001];
        memset(value, 'v', 1000);
        value[1000] = '\0';
        size_t bn = 0;
        bn += hpack_literal(block + bn, ":method", "POST");
        bn += hpack_literal(block + bn, ":scheme", "http");
        bn += hpack_literal(block + bn, ":path", "/len");
        bn += hpack_literal(block + bn, ":authority", "127.0.0.1");
        for (int i = 0; i < 41; i++) {
            char name[16];
            snprintf(name, sizeof(name), "x-pad-%02d", i);
            bn += hpack_literal(block + bn, name, value);
        }
        size_t off = 0;
        int first = 1;
        while (off < bn) {
            size_t take = bn - off < 16384 ? bn - off : 16384;
            int last = off + take == bn;
            send_frame(s, first ? 0x1 : 0x9, (last ? 0x4 : 0) | (first ? 0x1 : 0), 5,
                       block + off, take);
            off += take;
            first = 0;
        }
        n = await_frame(s, 5, 0x1, &flags, buf, sizeof(buf));
        int has_431 = 0;
        for (long i = 0; i + 3 <= n; i++)
            if (memcmp(buf + i, "431", 3) == 0) has_431 = 1;
        printf("  stream 5 answered with HEADERS of %ld bytes%s\n", n, has_431 ? ", status 431" : "");
        check(n > 0 && has_431, "a 41 KiB header list is answered 431");
    }

    /* 5. Two streams that would hold 30000 bytes on the connection. */
    send_post_headers(s, 7, "/len");
    send_frame(s, 0x0, 0, 7, body, sizeof(body));
    send_post_headers(s, 9, "/len");
    send_frame(s, 0x0, 0, 9, body, sizeof(body));
    n = await_frame(s, 9, 0x3, &flags, buf, sizeof(buf));
    long code9 = n == 4 ? rst_code_of(buf) : -1;
    printf("  stream 9 RST_STREAM code: %ld\n", code9);
    check(code9 == 0x7, "a second 15000-byte body past the connection's 20000 is refused (REFUSED_STREAM)");
    send_frame(s, 0x0, 0x1 /* END_STREAM */, 7, NULL, 0);
    got[0] = '\0';
    n = await_frame(s, 7, 0x0, &flags, buf, sizeof(buf));
    if (n >= 0) {
        size_t k = (size_t)n < sizeof(got) - 1 ? (size_t)n : sizeof(got) - 1;
        memcpy(got, buf, k);
        got[k] = '\0';
    }
    check(strcmp(got, "len=15000") == 0, "the first stream's 15000 bytes are answered len=15000");
    raw_socket_close(s);

    /* 6. A request that never ends, one byte of DATA every 200 ms. */
    s = h2_open(NULL, NULL);
    if (s == RAW_SOCK_INVALID) return 1;
    long long t0 = now_ms();
    send_post_headers(s, 1, "/len");
    long code = -1;
    while (now_ms() - t0 < 5000 && code < 0) {
        if (readable(s, 200)) {
            n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
            if (n < 0) break;
            if (type == 0x3 && stream == 1 && n == 4) code = rst_code_of(buf);
            continue;
        }
        send_frame(s, 0x0, 0, 1, body, 1);
    }
    long long took = now_ms() - t0;
    printf("  the trickled stream was reset with %ld after %lld ms\n", code, took);
    check(code == 0x8 && took < 3500, "a request trickled a byte every 200 ms is reset (CANCEL) within its budget");
    raw_socket_close(s);

    /* 7. No stream, a PING every 200 ms. */
    s = h2_open(NULL, NULL);
    if (s == RAW_SOCK_INVALID) return 1;
    t0 = now_ms();
    int goaway = 0, closed = 0;
    static const unsigned char ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    while (now_ms() - t0 < 5000 && !closed) {
        if (readable(s, 200)) {
            n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
            if (n < 0) { closed = 1; break; }
            if (type == 0x7) goaway = 1;
            continue;
        }
        send_frame(s, 0x6, 0, 0, ping, sizeof(ping));
    }
    took = now_ms() - t0;
    printf("  the pinging idle connection: goaway=%d closed=%d after %lld ms\n", goaway, closed, took);
    check(goaway && closed && took < 2500, "a connection with no stream closes after the idle timeout, PINGs or not");
    raw_socket_close(s);

    /* 8. A request every 300 ms, for longer than the connection may live. */
    s = h2_open(NULL, NULL);
    if (s == RAW_SOCK_INVALID) return 1;
    t0 = now_ms();
    goaway = 0;
    closed = 0;
    long long goaway_at = -1;
    unsigned next_stream = 1;
    /* A POST begun before the age and finished after the GOAWAY: the GOAWAY
     * lets open streams finish, so it is answered. Begun a whole second
     * before, whatever the loop is doing: the age counts from the server's
     * accept, before this clock starts, and a POST begun 400 ms before it,
     * in a quiet moment between answers, missed it on a slow runner (macOS
     * x86), when the server rightly closed a connection with no stream open.
     * 2000 bytes sent at once buy the body 2 s of its budget (1 s and 1 s a
     * 1000 bytes), so it is still alive when the last byte goes after the
     * GOAWAY. */
    static unsigned char post_body[2000];
    memset(post_body, 'x', sizeof(post_body));
    unsigned post_stream = 0;
    int post_answered = 0;
    while (now_ms() - t0 < 7000 && !closed) {
        if (!goaway && !post_stream && now_ms() - t0 >= 2000) {
            post_stream = next_stream;
            next_stream += 2;
            send_post_headers(s, post_stream, "/len");
            send_frame(s, 0x0, 0, post_stream, post_body, sizeof(post_body));
        }
        if (readable(s, 300)) {
            n = read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
            if (n < 0) { closed = 1; break; }
            if (type == 0x7 && !goaway) {
                goaway = 1;
                goaway_at = now_ms() - t0;
                if (post_stream) send_frame(s, 0x0, 0x1 /* END_STREAM */, post_stream, body, 1);
            }
            if (type == 0x0 && post_stream && stream == post_stream && n == 8 &&
                memcmp(buf, "len=2001", 8) == 0)
                post_answered = 1;
            continue;
        }
        if (!goaway) {
            send_headers(s, next_stream, "GET", "/ping", 1);
            next_stream += 2;
        }
    }
    took = now_ms() - t0;
    printf("  the busy connection: GOAWAY after %lld ms, closed=%d after %lld ms, open request answered=%d\n",
           goaway_at, closed, took, post_answered);
    check(goaway && goaway_at >= 2500 && goaway_at < 4000 && closed && took < 5500,
          "a connection busy past its 3 s age gets GOAWAY and is closed within the idle timeout");
    check(post_answered, "a request open when the GOAWAY went out is still answered");
    raw_socket_close(s);

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: h2 limits\n");
    return 0;
}
