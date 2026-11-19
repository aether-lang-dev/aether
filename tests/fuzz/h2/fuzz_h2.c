/* Coverage-guided fuzz target for the HTTP/2 server glue in
 * std/http/server/h2/aether_h2.c: the nghttp2 callbacks that turn a client's
 * HEADERS and DATA into a request (ng_on_header_callback,
 * ng_on_data_chunk_recv, ng_on_frame_recv_callback, ng_on_stream_close), the
 * dispatch into the server's middleware (request_from_stream), and the
 * response written back (submit_response_for_stream, the data source).
 *
 * The glue needs no socket: aether_h2_session_feed takes the client's bytes
 * and aether_h2_session_drain hands back what the server writes. The server
 * is one http_server_create'd object whose only middleware records each
 * request and echoes it: status 200, the request body as the body.
 *
 * Input: byte 0, bit 0 set: the rest is fed after the client preface as raw
 * frames. Clear: the rest is a script of client frames the harness encodes
 * (HPACK with nghttp2's own deflater), so it knows what each stream sent:
 *   op 0 HEADERS: a stream (1 + 2k), flags (END_STREAM; leave out :method,
 *        :path or :scheme; add `connection` or `te: gzip`; HEAD), a path and
 *        up to five fields from the input;
 *   op 1 DATA: a stream, END_STREAM or not, up to 63 bytes;
 *   op 2 RST_STREAM, op 3 PING, op 4 WINDOW_UPDATE, op 5 raw bytes, else an
 *        empty SETTINGS.
 *
 * Each check is a property, and a failure abort()s with what disagreed:
 *
 *   G1  Every request the middleware sees is one a stream asked for: the
 *       client opened it with HEADERS carrying :method, :path (not empty)
 *       and :scheme, ended it, and did not reset it; it sent no
 *       connection-specific field (RFC 9113 8.2.2) and no TE other than
 *       "trailers". The method, the path and query (split at the first
 *       '?'), the fields in order and the body are what it sent. No stream
 *       is dispatched twice.
 *   G2  A stream the script opened and ended with a request every rule
 *       allows, on stream ids that only grow and with nothing raw in the
 *       script, is dispatched.
 *   G3  What the server writes is HTTP/2 frames: each header is whole, no
 *       frame is longer than 16384 bytes, and every dispatched stream gets a
 *       HEADERS frame with :status 200 and every field the middleware set,
 *       in order (k of them on stream index k, 300 on the last, past the 256
 *       the glue once kept), and DATA that is its body (none for HEAD).
 *   G4  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2/nghttp2.h>
#include "aether_http_server.h"
#include "../../../std/http/server/h2/aether_h2.h"

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_h2: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

#define MAX_STREAMS 16
#define MAX_FIELDS 8

typedef struct { uint8_t* p; size_t n, cap; } Buf;

static void buf_put(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 1024;
        while (c < b->n + n) c *= 2;
        b->p = (uint8_t*)realloc(b->p, c);
        CHECK(b->p != NULL, "out of memory");
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}

/* What the script sent on a stream. */
typedef struct {
    int opened, ended, reset, dispatched, valid, head;
    int cancelled;             /* the client sent RST_STREAM on it, ended or not */
    int judged;                /* sent a field nghttp2 judges against the rest */
    int out_of_order;          /* opened at a lower id than one before it */
    char method[8], path[40], scheme[8];
    int has_method, has_path, has_scheme, forbidden;
    char names[MAX_FIELDS][16], values[MAX_FIELDS][40];
    int nfields;
    Buf body;
} Stream;

static Stream g_streams[MAX_STREAMS];
static int g_scripted;        /* the input is a script, not raw frames */
static int g_any_raw;
static Buf g_out;

static int stream_index(int32_t id) {
    if (id <= 0 || (id & 1) == 0) return -1;
    int k = (id - 1) / 2;
    return k < MAX_STREAMS ? k : -1;
}

/* How many fields the response to stream index k carries beside :status:
 * k of them, and 300 on the last, past the 256 the glue used to keep. */
static int extra_fields(int k) { return k == MAX_STREAMS - 1 ? 300 : k; }

/* The server's only middleware: G1 for each request, then the echo. */
static int record_and_echo(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)ud;
    if (g_scripted) {
        /* Which stream: the path carries its index as "/s<k>". */
        int k = -1;
        CHECK(req->path && sscanf(req->path, "/s%d/", &k) == 1 && k >= 0 && k < MAX_STREAMS,
              "a request for '%s', a path no stream sent", req->path ? req->path : "(null)");
        Stream* s = &g_streams[k];
        CHECK(s->opened, "stream %d was dispatched and never opened", k);
        CHECK(!s->dispatched, "stream %d was dispatched twice", k);
        CHECK(s->ended && !s->reset, "stream %d was dispatched before it ended, or after a reset", k);
        CHECK(s->has_method && s->has_path && s->has_scheme, "stream %d was dispatched without a pseudo-header", k);
        CHECK(!s->forbidden, "stream %d sent a connection-specific field and was dispatched", k);
        s->dispatched = 1;
        CHECK(strcmp(req->method, s->method) == 0, "stream %d: method '%s', it sent '%s'", k, req->method, s->method);
        const char* q = strchr(s->path, '?');
        size_t plen = q ? (size_t)(q - s->path) : strlen(s->path);
        CHECK(strlen(req->path) == plen && strncmp(req->path, s->path, plen) == 0,
              "stream %d: path '%s', it sent '%s'", k, req->path, s->path);
        CHECK(strcmp(req->query_string ? req->query_string : "", q ? q + 1 : "") == 0,
              "stream %d: query '%s', it sent '%s'", k, req->query_string, s->path);
        CHECK(req->header_count == s->nfields, "stream %d: %d fields, it sent %d", k, req->header_count, s->nfields);
        for (int i = 0; i < s->nfields; i++)
            CHECK(strcmp(req->header_keys[i], s->names[i]) == 0 && strcmp(req->header_values[i], s->values[i]) == 0,
                  "stream %d: field %d is %s: %s, it sent %s: %s", k, i, req->header_keys[i], req->header_values[i],
                  s->names[i], s->values[i]);
        CHECK(req->body_length == s->body.n && (s->body.n == 0 || memcmp(req->body, s->body.p, s->body.n) == 0),
              "stream %d: a body of %zu bytes, it sent %zu", k, req->body_length, s->body.n);
    }
    http_response_set_status(res, 200);
    if (req->body_length > 0) http_response_set_body_n(res, req->body, (int)req->body_length);
    /* G3's fields: x-f<i> on the response, as many as extra_fields says. */
    if (g_scripted) {
        int k = -1;
        if (req->path && sscanf(req->path, "/s%d/", &k) == 1 && k >= 0 && k < MAX_STREAMS) {
            for (int i = 0; i < extra_fields(k); i++) {
                char name[16], value[16];
                snprintf(name, sizeof name, "x-f%d", i);
                snprintf(value, sizeof value, "v%d", i);
                http_response_set_header(res, name, value);
            }
        }
    }
    return 0;
}

static int collect(void* ud, const uint8_t* buf, size_t len) {
    (void)ud;
    buf_put(&g_out, buf, len);
    return (int)len;
}

static void frame_header(Buf* b, size_t len, int type, int flags, int32_t id) {
    uint8_t h[9] = { (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, (uint8_t)type, (uint8_t)flags,
                     (uint8_t)((id >> 24) & 0x7f), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    buf_put(b, h, 9);
}

/* A byte as a lower-case token character, a path character or a visible
 * field-value character. */
static char tok(uint8_t c) { return "abcdefghijklmnopqrstuvwxyz0123456789-_.!#$%&'*+^`|~"[c % 51]; }
static char pathc(uint8_t c) { return "abcxyz0189/-._~%?=&;:@!$'()*+,"[c % 30]; }
static char valc(uint8_t c) { return (char)(0x21 + c % 94); }

typedef struct { const uint8_t* p; size_t n, i; } In;
static int in_byte(In* in) { return in->i < in->n ? in->p[in->i++] : -1; }

static void script(In* in, Buf* wire, nghttp2_hd_deflater* deflater) {
    int32_t last_opened = 0;
    for (int ops = 0; ops < 64; ops++) {
        int op = in_byte(in);
        if (op < 0) break;
        switch (op % 8) {
        case 0: {   /* HEADERS */
            int sk = in_byte(in), fl = in_byte(in);
            if (sk < 0 || fl < 0) return;
            int k = sk % MAX_STREAMS;
            int32_t id = 1 + 2 * k;
            Stream* s = &g_streams[k];
            int first = !s->opened;
            nghttp2_nv nv[4 + MAX_FIELDS + 2];
            int n = 0;
            char path[40];
            int pl = in_byte(in);
            pl = pl < 0 ? 0 : pl % 24;
            int w = snprintf(path, sizeof path, "/s%d/", k);
            for (int i = 0; i < pl && w < (int)sizeof path - 1; i++) {
                int c = in_byte(in);
                path[w++] = c < 0 ? 'a' : pathc((uint8_t)c);
            }
            path[w] = '\0';
            const char* method = (fl & 64) ? "HEAD" : "GET";
            if (first) {
                s->opened = 1;
                s->head = (fl & 64) != 0;
                s->out_of_order = id <= last_opened;
                if (id > last_opened) last_opened = id;
                snprintf(s->method, sizeof s->method, "%s", method);
                snprintf(s->path, sizeof s->path, "%s", path);
                snprintf(s->scheme, sizeof s->scheme, "https");
            } else {
                s->valid = 0;   /* a second HEADERS: trailers, or a protocol error */
                s->out_of_order = 1;
            }
            #define NV(nm, vl) do { nv[n].name = (uint8_t*)(nm); nv[n].namelen = strlen(nm); \
                nv[n].value = (uint8_t*)(vl); nv[n].valuelen = strlen(vl); nv[n].flags = NGHTTP2_NV_FLAG_NONE; n++; } while (0)
            if (!(fl & 2)) { NV(":method", method); if (first) s->has_method = 1; }
            if (!(fl & 4)) { NV(":path", path); if (first) s->has_path = 1; }
            if (!(fl & 8)) { NV(":scheme", "https"); if (first) s->has_scheme = 1; }
            NV(":authority", "h");
            int nf = in_byte(in);
            nf = nf < 0 ? 0 : nf % 6;
            for (int f = 0; f < nf && first && s->nfields < MAX_FIELDS; f++) {
                char* nm = s->names[s->nfields];
                char* vl = s->values[s->nfields];
                int nl = in_byte(in), vlen = in_byte(in);
                nl = nl < 0 ? 1 : 1 + nl % 12;
                vlen = vlen < 0 ? 0 : vlen % 32;
                for (int i = 0; i < nl; i++) { int c = in_byte(in); nm[i] = tok((uint8_t)(c < 0 ? 0 : c)); }
                nm[nl] = '\0';
                for (int i = 0; i < vlen; i++) { int c = in_byte(in); vl[i] = valc((uint8_t)(c < 0 ? 0 : c)); }
                vl[vlen] = '\0';
                NV(nm, vl);
                s->nfields++;
                /* A name the input happened to spell that a request may not
                 * carry (RFC 9113 8.2.2), and content-length, which nghttp2
                 * holds to the DATA that follows. */
                if (!strcmp(nm, "connection") || !strcmp(nm, "upgrade") || !strcmp(nm, "keep-alive") ||
                    !strcmp(nm, "transfer-encoding") || !strcmp(nm, "proxy-connection") ||
                    (!strcmp(nm, "te") && strcmp(vl, "trailers") != 0))
                    s->forbidden = 1;
                if (!strcmp(nm, "content-length") || !strcmp(nm, "host")) s->judged = 1;
            }
            if (fl & 16) { NV("connection", "close"); if (first) s->forbidden = 1; }
            if (fl & 32) { NV("te", "gzip"); if (first) s->forbidden = 1; }
            #undef NV
            size_t bound = nghttp2_hd_deflate_bound(deflater, nv, (size_t)n);
            uint8_t* block = (uint8_t*)malloc(bound ? bound : 1);
            CHECK(block != NULL, "out of memory");
            ssize_t bl = nghttp2_hd_deflate_hd(deflater, block, bound, nv, (size_t)n);
            CHECK(bl >= 0, "the deflater refused the fields");
            frame_header(wire, (size_t)bl, NGHTTP2_HEADERS, NGHTTP2_FLAG_END_HEADERS | ((fl & 1) ? NGHTTP2_FLAG_END_STREAM : 0), id);
            buf_put(wire, block, (size_t)bl);
            free(block);
            if (first) s->valid = 1;
            if (fl & 1) s->ended = 1;
            break;
        }
        case 1: {   /* DATA */
            int sk = in_byte(in), fl = in_byte(in);
            if (sk < 0 || fl < 0) return;
            Stream* s = &g_streams[sk % MAX_STREAMS];
            int32_t id = 1 + 2 * (sk % MAX_STREAMS);
            int len = fl >> 2;
            uint8_t d[64];
            for (int i = 0; i < len; i++) { int c = in_byte(in); d[i] = (uint8_t)(c < 0 ? 0 : c); }
            frame_header(wire, (size_t)len, NGHTTP2_DATA, (fl & 1) ? NGHTTP2_FLAG_END_STREAM : 0, id);
            buf_put(wire, d, (size_t)len);
            if (!s->opened || s->ended) s->valid = 0;
            else buf_put(&s->body, d, (size_t)len);
            if (fl & 1) s->ended = 1;
            break;
        }
        case 2: {   /* RST_STREAM */
            int sk = in_byte(in);
            if (sk < 0) return;
            Stream* s = &g_streams[sk % MAX_STREAMS];
            uint8_t code[4] = { 0, 0, 0, 8 };   /* CANCEL */
            frame_header(wire, 4, NGHTTP2_RST_STREAM, 0, 1 + 2 * (sk % MAX_STREAMS));
            buf_put(wire, code, 4);
            if (!s->ended) s->reset = 1;
            s->cancelled = 1;
            s->valid = 0;
            break;
        }
        case 3: {   /* PING */
            uint8_t ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            frame_header(wire, 8, NGHTTP2_PING, 0, 0);
            buf_put(wire, ping, 8);
            break;
        }
        case 4: {   /* WINDOW_UPDATE on the connection */
            uint8_t inc[4] = { 0, 0, 0x10, 0 };
            frame_header(wire, 4, NGHTTP2_WINDOW_UPDATE, 0, 0);
            buf_put(wire, inc, 4);
            break;
        }
        case 5: {   /* raw bytes */
            int len = in_byte(in);
            if (len < 0) return;
            len %= 32;
            for (int i = 0; i < len; i++) { int c = in_byte(in); uint8_t b = (uint8_t)(c < 0 ? 0 : c); buf_put(wire, &b, 1); }
            g_any_raw = 1;
            break;
        }
        default:
            frame_header(wire, 0, NGHTTP2_SETTINGS, 0, 0);
            break;
        }
    }
}

/* G3: the frames the server wrote. */
static void check_output(int standing) {
    nghttp2_hd_inflater* inflater = NULL;
    CHECK(nghttp2_hd_inflate_new(&inflater) == 0, "nghttp2_hd_inflate_new failed");
    int got_headers[MAX_STREAMS] = {0};
    int got_fields[MAX_STREAMS] = {0};   /* x-f<i>: v<i>, in order */
    int bad_field[MAX_STREAMS] = {0};
    Buf got_body[MAX_STREAMS];
    memset(got_body, 0, sizeof got_body);
    size_t p = 0;
    int hpack_ok = 1;
    while (p < g_out.n) {
        CHECK(g_out.n - p >= 9, "the server wrote a frame header cut short");
        size_t len = ((size_t)g_out.p[p] << 16) | ((size_t)g_out.p[p + 1] << 8) | g_out.p[p + 2];
        int type = g_out.p[p + 3], flags = g_out.p[p + 4];
        int32_t id = (int32_t)(((uint32_t)(g_out.p[p + 5] & 0x7f) << 24) | ((uint32_t)g_out.p[p + 6] << 16) |
                               ((uint32_t)g_out.p[p + 7] << 8) | g_out.p[p + 8]);
        CHECK(len <= 16384, "the server wrote a frame of %zu bytes", len);
        CHECK(g_out.n - p - 9 >= len, "the server wrote a frame cut short");
        const uint8_t* pl = g_out.p + p + 9;
        int k = stream_index(id);
        if (type == NGHTTP2_HEADERS && k >= 0 && hpack_ok) {
            size_t off = 0, end = len;
            if (flags & NGHTTP2_FLAG_PADDED) { off = 1; end -= pl[0]; }
            if (flags & NGHTTP2_FLAG_PRIORITY) off += 5;
            int final = 1;
            for (;;) {
                nghttp2_nv nv;
                int fl = 0;
                ssize_t r = nghttp2_hd_inflate_hd2(inflater, &nv, &fl, pl + off, end - off, final);
                if (r < 0) { hpack_ok = 0; break; }
                off += (size_t)r;
                if (fl & NGHTTP2_HD_INFLATE_EMIT) {
                    if (nv.namelen == 7 && memcmp(nv.name, ":status", 7) == 0)
                        got_headers[k] = (nv.valuelen == 3 && memcmp(nv.value, "200", 3) == 0) ? 200 : -1;
                    if (nv.namelen > 3 && memcmp(nv.name, "x-f", 3) == 0) {
                        char want_n[16], want_v[16];
                        int wn = snprintf(want_n, sizeof want_n, "x-f%d", got_fields[k]);
                        int wv = snprintf(want_v, sizeof want_v, "v%d", got_fields[k]);
                        if (nv.namelen != (size_t)wn || memcmp(nv.name, want_n, (size_t)wn) != 0 ||
                            nv.valuelen != (size_t)wv || memcmp(nv.value, want_v, (size_t)wv) != 0)
                            bad_field[k] = 1;
                        got_fields[k]++;
                    }
                }
                if (fl & NGHTTP2_HD_INFLATE_FINAL) { nghttp2_hd_inflate_end_headers(inflater); break; }
                if (r == 0 && off >= end) break;
            }
        }
        if (type == NGHTTP2_DATA && k >= 0) {
            size_t off = 0, end = len;
            if (flags & NGHTTP2_FLAG_PADDED) { off = 1; end -= pl[0]; }
            buf_put(&got_body[k], pl + off, end - off);
        }
        p += 9 + len;
    }
    /* A response is owed while the connection stands: one the client ended
     * with a connection error is not written, and nor is one the client
     * cancelled. A client may send RST_STREAM on a stream it has ended (it is
     * half-closed (local) on its side, RFC 9113 5.1), and the server then
     * stops: nghttp2 drops a response it has not written yet. The harness
     * owed one regardless, and failed when the RST_STREAM arrived in the same
     * read as the request (the h2 crash of CI run 44568798645, now the seed
     * crash_rst_after_end). */
    for (int k = 0; k < MAX_STREAMS && standing; k++) {
        Stream* s = &g_streams[k];
        if (!s->dispatched || s->cancelled || !hpack_ok) continue;
        CHECK(got_headers[k] == 200, "dispatched stream %d got no :status 200 (%d)", k, got_headers[k]);
        CHECK(got_fields[k] == extra_fields(k) && !bad_field[k],
              "stream %d: the response carries %d of its %d fields%s", k, got_fields[k], extra_fields(k),
              bad_field[k] ? ", or not as they were set" : "");
        size_t want = s->head ? 0 : s->body.n;
        CHECK(got_body[k].n == want && (want == 0 || memcmp(got_body[k].p, s->body.p, want) == 0),
              "stream %d: the response body is %zu bytes, the request's %zu", k, got_body[k].n, want);
    }
    for (int k = 0; k < MAX_STREAMS; k++) free(got_body[k].p);
    nghttp2_hd_inflate_del(inflater);
}

static HttpServer* g_server;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    if (!g_server) {
        g_server = http_server_create(0);
        CHECK(g_server != NULL, "http_server_create failed");
        g_server->h2_dispatch_workers = 0;   /* handlers on this thread */
        http_server_use_middleware(g_server, record_and_echo, NULL);
    }
    memset(g_streams, 0, sizeof g_streams);
    g_any_raw = 0;
    g_out.n = 0;
    g_scripted = !(data[0] & 1);

    Buf wire = {0};
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    buf_put(&wire, preface, sizeof preface - 1);
    frame_header(&wire, 0, NGHTTP2_SETTINGS, 0, 0);
    if (g_scripted) {
        nghttp2_hd_deflater* deflater = NULL;
        CHECK(nghttp2_hd_deflate_new(&deflater, 4096) == 0, "nghttp2_hd_deflate_new failed");
        In in = { data + 1, size - 1, 0 };
        script(&in, &wire, deflater);
        nghttp2_hd_deflate_del(deflater);
    } else {
        buf_put(&wire, data + 1, size - 1);
    }

    AetherH2Session* sess = aether_h2_session_new(g_server, NULL);
    CHECK(sess != NULL, "aether_h2_session_new failed");
    CHECK(aether_h2_session_send_initial_settings(sess) == 0, "the initial SETTINGS were refused");
    /* Fed in two pieces, cut where the input says, as reads arrive. */
    size_t cut = wire.n / 2;
    int fed = aether_h2_session_feed(sess, wire.p, cut);
    if (fed >= 0) {
        (void)aether_h2_session_drain(sess, collect);
        fed = aether_h2_session_feed(sess, wire.p + cut, wire.n - cut);
    }
    for (int i = 0; i < 8; i++) {
        int r = aether_h2_session_drain(sess, collect);
        if (r != 0) break;
    }

    /* G2 */
    /* Only while the connection stands: a frame the script laid out of turn
     * (DATA or RST_STREAM on an idle stream, an id that went down) is a
     * connection error, which ends every stream (RFC 9113 5.4.1). */
    if (g_scripted && !g_any_raw && fed >= 0 && !aether_h2_session_want_close(sess)) {
        for (int k = 0; k < MAX_STREAMS; k++) {
            Stream* s = &g_streams[k];
            if (s->opened && s->valid && s->ended && !s->reset && !s->out_of_order && s->has_method &&
                s->has_path && s->has_scheme && !s->forbidden && !s->judged)
                CHECK(s->dispatched, "stream %d sent a whole, valid request and was not dispatched", k);
        }
    }
    check_output(g_scripted && !g_any_raw && fed >= 0 && !aether_h2_session_want_close(sess));

    aether_h2_session_free(sess);
    for (int k = 0; k < MAX_STREAMS; k++) free(g_streams[k].body.p);
    free(wire.p);
    return 0;
}
