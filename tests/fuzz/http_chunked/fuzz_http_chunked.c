/* Coverage-guided fuzz target for the chunked transfer-coding: the framing
 * check (http_chunked_frame) the server and the client run as body bytes
 * arrive, and the decoder (http_dechunk), in std/net/aether_http.c.
 *
 * The input is a chunked body as it arrives, and maybe what follows it. Each
 * check is a property, and a failure abort()s with what disagreed:
 *
 *   C1  The answer is the one a reference reading of RFC 9112 7.1 gives,
 *       written here from the grammar and the limits aether_http.h
 *       documents, not from the code: a chunk-size is 1 to 16 hex digits
 *       read exactly, at most INT64_MAX; chunk extensions are
 *       BWS ";" BWS token [ BWS "=" BWS ( token / quoted-string ) ]; each
 *       line ends in CR LF and holds no other CR; a chunk's data is followed
 *       by CR LF; trailer lines end at the empty line; no line is longer
 *       than 1024 bytes. 1 with the framed length when the body is whole, 0
 *       when more has to arrive, -1 when no more bytes can make it framing.
 *   C2  http_dechunk decodes exactly what frames, to the reference's
 *       payload (the chunks' data, NUL bytes included), and refuses the
 *       rest.
 *   C3  The answer does not depend on where the bytes were cut: every
 *       prefix of a whole body answers 0, every longer buffer answers 1
 *       with the same length, and once a prefix answers -1 every longer one
 *       does. This is what the server relies on, calling the check after
 *       every read.
 *   C4  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether_http.h"

#define C_LINE_MAX 1024
#define C_SIZE_MAX ((uint64_t)INT64_MAX)

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_http_chunked: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

static int is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* qdtext / quoted-pair octets (RFC 9110 5.6.4): HTAB, SP, VCHAR, obs-text. */
static int qd_ok(unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); }

/* One chunk-size line without its CR LF: 0 with the size, or -1. */
static int ref_size_line(const char* s, size_t n, uint64_t* size) {
    size_t i = 0;
    uint64_t v = 0;
    while (i < n && hexval((unsigned char)s[i]) >= 0) {
        if (i == 16) return -1;
        v = v * 16 + (uint64_t)hexval((unsigned char)s[i]);
        i++;
    }
    if (i == 0 || v > C_SIZE_MAX) return -1;
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i == n) break;
        if (s[i] != ';') return -1;
        i++;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t name = i;
        while (i < n && is_tchar((unsigned char)s[i])) i++;
        if (i == name) return -1;
        size_t after = i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i == n || s[i] != '=') { i = after; continue; }
        i++;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i < n && s[i] == '"') {
            i++;
            for (;;) {
                if (i == n) return -1;
                unsigned char c = (unsigned char)s[i];
                if (c == '"') { i++; break; }
                if (c == '\\') {
                    if (++i == n || !qd_ok((unsigned char)s[i])) return -1;
                } else if (!qd_ok(c)) {
                    return -1;
                }
                i++;
            }
        } else {
            size_t val = i;
            while (i < n && is_tchar((unsigned char)s[i])) i++;
            if (i == val) return -1;
        }
    }
    /* BWS before an extension is allowed; after the size with none, it is
     * not (the loop above accepted it only to find the ';'). */
    if (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) return -1;
    *size = v;
    return 0;
}

/* The next line at `off`: 1 with its length (CR LF excluded) when it is a
 * whole, well-ended line, 0 when its end has not arrived, -1 when it can
 * never be one. */
static int ref_line(const char* b, size_t len, size_t off, size_t* line_len) {
    const char* lf = (const char*)memchr(b + off, '\n', len - off);
    if (!lf) return (len - off > C_LINE_MAX) ? -1 : 0;
    size_t n = (size_t)(lf - (b + off));
    if (n > C_LINE_MAX || n == 0 || b[off + n - 1] != '\r') return -1;
    if (memchr(b + off, '\r', n - 1)) return -1;
    *line_len = n - 1;
    return 1;
}

static int ref_walk(const char* b, size_t len, size_t* frame_len, char* out, size_t* out_len) {
    size_t off = 0, o = 0;
    for (;;) {
        size_t n = 0;
        int r = ref_line(b, len, off, &n);
        if (r <= 0) return r;
        uint64_t size = 0;
        if (ref_size_line(b + off, n, &size) != 0) return -1;
        off += n + 2;
        if (size == 0) break;
        uint64_t left = (uint64_t)(len - off);
        if (left < size) return 0;
        memcpy(out + o, b + off, (size_t)size);
        o += (size_t)size;
        off += (size_t)size;
        if (len - off >= 1 && b[off] != '\r') return -1;
        if (len - off >= 2 && b[off + 1] != '\n') return -1;
        if (len - off < 2) return 0;
        off += 2;
    }
    for (;;) {
        size_t n = 0;
        int r = ref_line(b, len, off, &n);
        if (r <= 0) return r;
        off += n + 2;
        if (n == 0) break;
    }
    *frame_len = off;
    *out_len = o;
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* in = (const char*)data;
    char* ref_out = (char*)malloc(size + 1);
    CHECK(ref_out != NULL, "out of memory");
    size_t ref_frame = 0, ref_len = 0;
    int ref = ref_walk(in, size, &ref_frame, ref_out, &ref_len);

    /* C1 */
    size_t frame_len = 777;
    int rc = http_chunked_frame(in, size, &frame_len);
    CHECK(rc == ref, "http_chunked_frame answered %d, RFC 9112 7.1 says %d", rc, ref);
    if (rc == 1) CHECK(frame_len == ref_frame, "framed %zu bytes, the body takes %zu", frame_len, ref_frame);
    else CHECK(frame_len == 0, "frame_len %zu with answer %d", frame_len, rc);

    /* C2 */
    size_t dl = 999;
    char* dec = http_dechunk(in, size, &dl);
    CHECK((dec != NULL) == (rc == 1), "framing answered %d but http_dechunk %s", rc,
          dec ? "decoded" : "refused");
    if (dec) {
        CHECK(dl == ref_len && memcmp(dec, ref_out, dl) == 0,
              "decoded %zu bytes, the chunks hold %zu (or other bytes)", dl, ref_len);
        CHECK(dec[dl] == '\0', "the decoded payload is not NUL-terminated");
        free(dec);
    }

    /* C3: the same answer however the bytes were cut. Every cut for a short
     * input, a spread of them for a long one. */
    size_t step = size <= 512 ? 1 : size / 256;
    int refused_at = -1;
    for (size_t k = 0; k <= size; k += step) {
        size_t fl = 0;
        int pr = http_chunked_frame(in, k, &fl);
        if (rc == 1 && k < frame_len) CHECK(pr == 0, "the %zu-byte prefix of a %zu-byte body answered %d", k, frame_len, pr);
        if (rc == 1 && k >= frame_len) CHECK(pr == 1 && fl == frame_len,
                                             "%zu bytes answered %d/%zu, the whole body %zu", k, pr, fl, frame_len);
        if (rc == 0) CHECK(pr == 0, "the %zu-byte prefix answered %d, all %zu bytes 0", k, pr, size);
        if (pr == -1 && refused_at < 0) refused_at = (int)k;
        if (refused_at >= 0) CHECK(pr == -1, "refused at %d bytes, %zu bytes answered %d", refused_at, k, pr);
        if (step > 1 && k + step > size && k != size) k = size - step;
    }
    free(ref_out);
    return 0;
}
