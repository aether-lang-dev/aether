/* Coverage-guided fuzz target for the HTTP/1 request head on the server's
 * path: http_find_header_end, http_request_head_check, the chunked framing
 * and decoding of a body (http_chunked_frame, http_dechunk) and
 * http_parse_request_n, taken in the order the worker takes them
 * (std/net/aether_http_server.c, the request read after the head), for each
 * request of a pipelined stream.
 *
 * The input is the byte stream a client sent on one connection. Each check
 * is a property, and a failure abort()s with what disagreed:
 *
 *   H1  http_find_header_end finds the first CR LF CR LF and no other.
 *   H2  http_request_head_check gives the answer a model of its contract
 *       gives (below, written from the RFC 9112 rules its comment cites, not
 *       from its code): a NUL, or a CR or LF outside a CR LF, is malformed;
 *       a request line of 2048 bytes or more is 414; a header line of 1024
 *       or more, or a 51st header, is 431; an obs-fold line, a line with no
 *       colon, whitespace before the colon, or a name that is not a token is
 *       400; and so are two Content-Length values that differ, one that is
 *       not a plain decimal count that fits a long, a Transfer-Encoding
 *       beside a Content-Length, and Transfer-Encoding fields whose codings,
 *       read as one list, do not end in chunked, name it twice, or hold
 *       something that is not a coding. A Content-Length it accepts is that
 *       exact decimal value, and the client's reader of a response's
 *       Transfer-Encoding fields (http_te_in_block) reads them as it does.
 *   H3  A chunked body that frames decodes, to no more bytes than its
 *       framing, and a body that does not frame is refused, not decoded.
 *   H4  http_parse_request_n reads the head the head check passed the way
 *       the check read it: the request line is METHOD SP target SP
 *       HTTP/1.x and parses exactly when it has that shape, the headers are
 *       the same lines in the same order with the same names and values
 *       (whitespace around a value trimmed), and the body is exactly the
 *       Content-Length bytes, or the decoded chunks, NUL bytes included.
 *   H5  Every request is freed (LeakSanitizer reports a leak).
 *
 * The limits are those of aether_http_server.c (HTTP_MAX_REQUEST_LINE,
 * HTTP_MAX_HEADER_LINE, HTTP_MAX_HEADERS), mirrored here; when one moves,
 * this file moves with it. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "aether_http.h"
#include "aether_http_server.h"

#define H_MAX_REQUEST_LINE 2048
#define H_MAX_HEADER_LINE  1024
#define H_MAX_HEADERS      50
#define H_MAX_NAME         128
#define H_MAX_PIPELINED    8

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_http_head: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

/* tchar, RFC 9110 5.6.2. */
static int is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_ows(char c) { return c == ' ' || c == '\t'; }

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char* a, size_t an, const char* b) {
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

typedef struct {
    const char* name;  size_t name_len;
    const char* value; size_t value_len;
} Field;

typedef struct {
    int   rc;
    long  content_length;
    int   chunked;
    int   te_count;        /* Transfer-Encoding fields */
    int   count;
    Field f[H_MAX_HEADERS];
} HeadModel;

/* The digits of LONG_MAX: a longer Content-Length is refused even when the
 * extra digits are leading zeros (http_parse_count's contract). */
static size_t long_max_digits(void) {
    size_t d = 1;
    for (long m = LONG_MAX; m >= 10; m /= 10) d++;
    return d;
}

/* A Content-Length value: 1*DIGIT, no more digits than LONG_MAX has, at
 * most LONG_MAX. 0 with the value, or -1. */
static int model_count(const char* s, size_t n, long* out) {
    if (n == 0 || n > long_max_digits()) return -1;
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (unsigned long long)(s[i] - '0');
        if (v > (unsigned long long)LONG_MAX) return -1;
    }
    *out = (long)v;
    return 0;
}

/* The Transfer-Encoding codings, every field's list joined in order (RFC
 * 9110 5.3): 1 when the last one is chunked and no other is, 0 otherwise.
 * An empty element is skipped (RFC 9110 5.6.1); an element is a token, then
 * nothing or its parameters after a ';' (RFC 9112 7: chunked has none), and
 * anything else is no coding and gives 0. */
static int model_te_chunked(const Field* te, int te_count) {
    int last_chunked = 0, chunked_seen = 0, any = 0;
    for (int k = 0; k < te_count; k++) {
        const char* v = te[k].value;
        size_t n = te[k].value_len, i = 0;
        while (i <= n) {
            size_t start = i;
            while (i < n && v[i] != ',') i++;
            size_t a = start, b = i;
            while (a < b && is_ows(v[a])) a++;
            while (b > a && is_ows(v[b - 1])) b--;
            if (b > a) {
                size_t t = a;
                while (t < b && is_tchar((unsigned char)v[t])) t++;
                if (t == a) return 0;
                int is_chunked = ieq(v + a, t - a, "chunked");
                size_t p = t;
                while (p < b && is_ows(v[p])) p++;
                if (p < b && (v[p] != ';' || is_chunked)) return 0;
                if (is_chunked) chunked_seen++;
                last_chunked = is_chunked;
                any = 1;
            }
            i++;
        }
    }
    return any && last_chunked && chunked_seen == 1;
}

static void model_head(const char* h, size_t n, HeadModel* m) {
    memset(m, 0, sizeof *m);
    m->rc = HTTP_HEAD_OK;
    if (memchr(h, '\0', n)) { m->rc = HTTP_HEAD_MALFORMED; return; }
    /* Every CR is followed by an LF and every LF follows a CR; the head's
     * last line is followed by the CR LF CR LF that ends it. */
    for (size_t i = 0; i < n; i++) {
        if ((h[i] == '\r' && (i + 1 >= n || h[i + 1] != '\n')) ||
            (h[i] == '\n' && (i == 0 || h[i - 1] != '\r'))) {
            m->rc = HTTP_HEAD_MALFORMED;
            return;
        }
    }
    const char* rl = (const char*)memchr(h, '\n', n);
    size_t rl_len = rl ? (size_t)(rl - h) : n;
    if (rl_len >= H_MAX_REQUEST_LINE) { m->rc = HTTP_HEAD_URI_TOO_LONG; return; }

    Field te[H_MAX_HEADERS];
    int te_count = 0, cl_count = 0, cl_differ = 0;
    const char* cl_v = NULL;
    size_t cl_n = 0;
    size_t at = rl ? rl_len + 1 : n;
    while (at < n) {
        const char* line = h + at;
        const char* eol = (const char*)memchr(line, '\n', n - at);
        size_t end = eol ? (size_t)(eol - h) : n;
        size_t len = end - at;
        if (len && line[len - 1] == '\r') len--;
        if (len >= H_MAX_HEADER_LINE) { m->rc = HTTP_HEAD_TOO_LARGE; return; }
        if (len > 0) {
            if (m->count + 1 > H_MAX_HEADERS) { m->rc = HTTP_HEAD_TOO_LARGE; return; }
            if (is_ows(line[0])) { m->rc = HTTP_HEAD_BAD_FRAMING; return; }
            const char* colon = (const char*)memchr(line, ':', len);
            if (!colon) { m->rc = HTTP_HEAD_BAD_FRAMING; return; }
            size_t name_len = (size_t)(colon - line);
            if (name_len == 0 || name_len >= H_MAX_NAME || is_ows(colon[-1])) {
                m->rc = HTTP_HEAD_BAD_FRAMING;
                return;
            }
            for (size_t i = 0; i < name_len; i++)
                if (!is_tchar((unsigned char)line[i])) { m->rc = HTTP_HEAD_BAD_FRAMING; return; }
            const char* v = colon + 1;
            const char* ve = line + len;
            while (v < ve && is_ows(*v)) v++;
            while (ve > v && is_ows(ve[-1])) ve--;
            Field* f = &m->f[m->count++];
            f->name = line;
            f->name_len = name_len;
            f->value = v;
            f->value_len = (size_t)(ve - v);
            if (ieq(line, name_len, "Content-Length")) {
                if (cl_count++ == 0) { cl_v = f->value; cl_n = f->value_len; }
                else if (f->value_len != cl_n || memcmp(f->value, cl_v, cl_n) != 0) cl_differ = 1;
            } else if (ieq(line, name_len, "Transfer-Encoding")) {
                te[te_count++] = *f;
            }
        }
        if (!eol) break;
        at = end + 1;
    }
    m->te_count = te_count;
    if (cl_count > 0) {
        if (cl_differ || model_count(cl_v, cl_n, &m->content_length) != 0) {
            m->rc = HTTP_HEAD_BAD_FRAMING;
            return;
        }
    }
    if (te_count > 0) {
        if (cl_count > 0 || !model_te_chunked(te, te_count)) {
            m->rc = HTTP_HEAD_BAD_FRAMING;
            return;
        }
        m->chunked = 1;
    }
}

/* The request line's shape: METHOD SP request-target SP HTTP-version, the
 * method a token, the target free of whitespace and controls, the version
 * HTTP/1.0 or HTTP/1.1, ended by CR LF (RFC 9112 3). */
static int model_request_line(const char* h, size_t n,
                              const char** method, size_t* method_len,
                              const char** target, size_t* target_len) {
    const char* lf = (const char*)memchr(h, '\n', n);
    if (!lf || lf == h || lf[-1] != '\r') return 0;
    size_t len = (size_t)(lf - h) - 1;
    const char* sp1 = (const char*)memchr(h, ' ', len);
    if (!sp1 || sp1 == h) return 0;
    for (const char* c = h; c < sp1; c++)
        if (!is_tchar((unsigned char)*c)) return 0;
    const char* t = sp1 + 1;
    const char* sp2 = (const char*)memchr(t, ' ', len - (size_t)(t - h));
    if (!sp2 || sp2 == t) return 0;
    for (const char* c = t; c < sp2; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c == 0x7f) return 0;
    size_t vlen = len - (size_t)(sp2 + 1 - h);
    if (!(vlen == 8 && (memcmp(sp2 + 1, "HTTP/1.1", 8) == 0 || memcmp(sp2 + 1, "HTTP/1.0", 8) == 0)))
        return 0;
    *method = h;
    *method_len = (size_t)(sp1 - h);
    *target = t;
    *target_len = (size_t)(sp2 - t);
    return 1;
}

static int span_eq(const char* s, const char* span, size_t n) {
    return s && strlen(s) == n && memcmp(s, span, n) == 0;
}

/* One request at the start of `in` (`n` bytes). Returns the bytes it took,
 * or 0 when the stream ends here: no whole head, a refusal (the connection
 * is closed), or a body still to arrive. */
static size_t one_request(const char* in, size_t n) {
    /* H1 */
    const char* hdr_end = http_find_header_end(in, n);
    const char* first = NULL;
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(in + i, "\r\n\r\n", 4) == 0) { first = in + i; break; }
    CHECK(hdr_end == first, "http_find_header_end gave offset %ld, the first CRLFCRLF is at %ld",
          hdr_end ? (long)(hdr_end - in) : -1L, first ? (long)(first - in) : -1L);
    if (!hdr_end) return 0;
    size_t header_size = (size_t)(hdr_end - in) + 4;
    /* The worker refuses a head this large before checking it. */
    if (header_size >= HTTP_CONN_BUF_CAP) return 0;

    /* H2 */
    long cl = -7;
    int chunked = -7;
    int rc = http_request_head_check(in, hdr_end, &cl, &chunked);
    HeadModel m;
    model_head(in, (size_t)(hdr_end - in), &m);
    CHECK(rc == m.rc, "http_request_head_check answered %d, the contract says %d", rc, m.rc);
    if (rc != HTTP_HEAD_OK) return 0;
    CHECK(cl == m.content_length, "Content-Length read as %ld, it says %ld", cl, m.content_length);
    CHECK(chunked == m.chunked, "chunked read as %d, the head says %d", chunked, m.chunked);
    /* The client's reader of a response's Transfer-Encoding fields
     * (http_te_in_block, #3670) reads the same header lines the same way. */
    {
        const char* fields = (const char*)memchr(in, '\n', (size_t)(hdr_end - in));
        int te_chunked = -1;
        int te_fields = fields ? http_te_in_block(fields + 1, hdr_end, &te_chunked)
                               : http_te_in_block(hdr_end, hdr_end, &te_chunked);
        CHECK((te_fields > 0) == (m.te_count > 0) && te_chunked == chunked,
              "the client's reader found %d Transfer-Encoding fields, chunked %d; the head check %d, %d",
              te_fields, te_chunked, m.te_count, chunked);
    }
    /* A body past one buffer is streamed off the socket, not parsed here. */
    if (cl > HTTP_CONN_BUF_CAP) return 0;

    /* H3: the body, as the worker assembles it before the parse. */
    const char* body = in + header_size;
    size_t avail = n - header_size;
    char* decoded = NULL;
    size_t body_len = 0, consumed = 0;
    if (chunked) {
        size_t frame_len = 12345;
        int framed = http_chunked_frame(body, avail, &frame_len);
        CHECK(framed >= -1 && framed <= 1, "http_chunked_frame returned %d", framed);
        size_t dl = 0;
        decoded = http_dechunk(body, avail, &dl);
        CHECK((decoded != NULL) == (framed == 1),
              "framing said %d but http_dechunk %s", framed, decoded ? "decoded it" : "refused it");
        if (framed != 1) {
            CHECK(frame_len == 0, "frame_len %zu after framing %d", frame_len, framed);
            return 0;
        }
        CHECK(frame_len <= avail, "frame_len %zu past the %zu bytes there", frame_len, avail);
        CHECK(dl <= frame_len, "decoded %zu bytes out of %zu of framing", dl, frame_len);
        /* Decoding exactly the framed bytes gives the same payload. */
        size_t dl2 = 0;
        char* again = http_dechunk(body, frame_len, &dl2);
        CHECK(again && dl2 == dl && memcmp(again, decoded, dl) == 0,
              "decoding the %zu framed bytes alone gave another payload", frame_len);
        free(again);
        body_len = dl;
        consumed = header_size + frame_len;
    } else {
        if ((size_t)cl > avail) return 0;
        body_len = (size_t)cl;
        consumed = header_size + body_len;
    }

    /* The slice the worker hands the parser: the head, then the body (the
     * decoded payload where it was chunked), NUL-terminated. */
    size_t total = header_size + body_len;
    char* slice = (char*)malloc(total + 1);
    CHECK(slice != NULL, "out of memory");
    memcpy(slice, in, header_size);
    memcpy(slice + header_size, decoded ? decoded : body, body_len);
    slice[total] = '\0';

    /* H4 */
    HttpRequest* req = http_parse_request_n(slice, total);
    const char *method = NULL, *target = NULL;
    size_t method_len = 0, target_len = 0;
    int shaped = model_request_line(in, header_size, &method, &method_len, &target, &target_len);
    CHECK((req != NULL) == shaped, "the request line %s, and the parser %s it",
          shaped ? "has the RFC 9112 shape" : "does not have the RFC 9112 shape",
          req ? "accepted" : "refused");
    if (req) {
        CHECK(span_eq(http_request_method(req), method, method_len), "method '%s'", http_request_method(req));
        const char* q = (const char*)memchr(target, '?', target_len);
        size_t path_len = q ? (size_t)(q - target) : target_len;
        CHECK(span_eq(http_request_path(req), target, path_len), "path '%s'", http_request_path(req));
        const char* query = http_request_query(req);
        if (q) CHECK(span_eq(query, q + 1, target_len - path_len - 1), "query '%s'", query);
        else CHECK(query == NULL || query[0] == '\0', "query '%s' with no '?'", query);
        int hc = http_request_header_count(req);
        CHECK(hc == m.count, "the parser has %d headers, the head check read %d lines", hc, m.count);
        for (int i = 0; i < hc; i++) {
            const char* name = http_request_header_name(req, i);
            const char* value = http_request_header_value(req, i);
            CHECK(span_eq(name, m.f[i].name, m.f[i].name_len),
                  "header %d is named '%s', the line says '%.*s'", i, name,
                  (int)m.f[i].name_len, m.f[i].name);
            CHECK(span_eq(value, m.f[i].value, m.f[i].value_len),
                  "header %d '%s' has the value '%s', the line says '%.*s'", i, name, value,
                  (int)m.f[i].value_len, m.f[i].value);
        }
        if (!chunked && m.content_length > 0) {
            const char* clv = http_get_header(req, "Content-Length");
            long back = -1;
            CHECK(clv && model_count(clv, strlen(clv), &back) == 0 && back == cl,
                  "the parsed Content-Length '%s' is not %ld", clv ? clv : "(none)", cl);
        }
        int bl = http_request_body_length(req);
        const char* b = http_request_body(req);
        CHECK(bl >= 0 && (size_t)bl == body_len, "the body is %d bytes, the framing says %zu", bl, body_len);
        CHECK(body_len == 0 || memcmp(b, decoded ? decoded : body, body_len) == 0,
              "the body's bytes differ from those framed");
        http_request_free(req);
    }
    free(slice);
    free(decoded);
    return consumed;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* in = (const char*)data;
    for (int r = 0; r < H_MAX_PIPELINED && size > 0; r++) {
        size_t took = one_request(in, size);
        if (took == 0) break;
        in += took;
        size -= took;
    }
    return 0;
}
