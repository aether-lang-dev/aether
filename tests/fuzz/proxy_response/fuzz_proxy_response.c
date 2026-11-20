/* Coverage-guided fuzz target for the reverse proxy's handling of what an
 * upstream sends back (std/http/proxy/aether_proxy_middleware.c), on both of
 * its paths:
 *
 *   - the copying path: the client reads the bytes into a response
 *     (http_response_fill_from_bytes, std/net/aether_http.c), and the
 *     exchange copies its status, headers and body onto the response the
 *     server writes (aether_proxy_exchange_resume, px_copy);
 *   - the pass-through the proxy event loop takes for a response whose
 *     length is known (aether_proxy_direct_take, aether_proxy_direct_head),
 *     which writes the head itself and sends the body from where it landed.
 *
 * The input is the upstream's bytes, after one byte that picks the
 * request's method (bit 0: HEAD, else GET). The bytes are framed as the event
 * loop frames them (http_response_is_complete, which the harness reaches by
 * including aether_http.c). Each check is a property, and a failure abort()s
 * with what disagreed:
 *
 *   P1  The head the client receives, on either path, is one well-formed
 *       response head: a status line `HTTP/1.1 <3 digits> <reason>`, then
 *       `name: value` lines whose name is a token and whose value holds no
 *       CR, LF or NUL (a header carrying a line break would let the rest be
 *       read as a second response, CWE-113), then the empty line.
 *   P2  No hop-by-hop header of the upstream's connection is passed on
 *       (Connection, Keep-Alive, Proxy-Authenticate, Proxy-Authorization,
 *       TE, Trailer, Transfer-Encoding, Upgrade, Proxy-Connection).
 *   P3  An answer to GET states one Content-Length, the length of the body
 *       it carries; an answer to HEAD carries no body (RFC 9110 9.3.2).
 *   P4  The pass-through, when it takes a response, writes the head the
 *       copying path writes for it, byte for byte, with the same status and
 *       the same body: the two paths cannot answer a client differently.
 *   P5  Nothing leaks (LeakSanitizer). */
#include "../../../std/net/aether_http.c"

#include "../../../std/net/aether_http_server.h"
#include "../../../std/http/proxy/aether_proxy.h"
#include "../../../std/http/proxy/aether_proxy_internal.h"

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_proxy_response: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

static int is_tchar(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static const char* const HOP[] = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization", "te",
    "trailer", "transfer-encoding", "upgrade", "proxy-connection", NULL
};

/* P1-P3 over one head (`head_len` bytes ending in the empty line) and the
 * body sent after it. Returns the status. */
static int check_head(const char* what, const char* head, size_t head_len, size_t body_len, int is_head) {
    CHECK(head_len >= 4 && memcmp(head + head_len - 4, "\r\n\r\n", 4) == 0, "%s: the head does not end in an empty line", what);
    CHECK(memchr(head, '\0', head_len) == NULL, "%s: the head holds a NUL", what);
    const char* eol = memchr(head, '\n', head_len);
    CHECK(eol && eol > head && eol[-1] == '\r', "%s: no status line", what);
    CHECK(eol - head >= 13 && memcmp(head, "HTTP/1.1 ", 9) == 0 &&
          isdigit((unsigned char)head[9]) && isdigit((unsigned char)head[10]) &&
          isdigit((unsigned char)head[11]) && (head[12] == ' ' || head[12] == '\r'),
          "%s: the status line is '%.*s'", what, (int)(eol - head - 1), head);
    int status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');
    for (const char* s = head + 12; s < eol - 1; s++)
        CHECK((unsigned char)*s >= 0x20 || *s == '\t', "%s: a control byte in the reason phrase", what);

    int content_lengths = 0;
    long long cl = -1;
    const char* p = eol + 1;
    const char* end = head + head_len - 2;
    while (p < end) {
        const char* le = memchr(p, '\n', (size_t)(end - p));
        CHECK(le && le > p && le[-1] == '\r', "%s: a header line not ended by CR LF", what);
        const char* colon = memchr(p, ':', (size_t)(le - p));
        CHECK(colon && colon > p, "%s: a header line with no name: '%.*s'", what, (int)(le - p - 1), p);
        for (const char* c = p; c < colon; c++)
            CHECK(is_tchar((unsigned char)*c), "%s: the header name '%.*s' is not a token", what, (int)(colon - p), p);
        for (const char* c = colon + 1; c < le - 1; c++)
            CHECK(*c != '\r' && *c != '\n', "%s: a line break inside the value of '%.*s'", what, (int)(colon - p), p);
        size_t nl = (size_t)(colon - p);
        for (int h = 0; HOP[h]; h++)
            CHECK(!(nl == strlen(HOP[h]) && strncasecmp(p, HOP[h], nl) == 0),
                  "%s: the hop-by-hop header %s was passed on", what, HOP[h]);
        if (nl == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
            content_lengths++;
            const char* v = colon + 1;
            while (*v == ' ') v++;
            cl = strtoll(v, NULL, 10);
        }
        p = le + 1;
    }
    if (is_head) {
        CHECK(body_len == 0, "%s: an answer to HEAD carries %zu body bytes", what, body_len);
    } else {
        CHECK(content_lengths == 1, "%s: %d Content-Length headers", what, content_lengths);
        CHECK(cl == (long long)body_len, "%s: Content-Length %lld for a body of %zu bytes", what, cl, body_len);
    }
    return status;
}

/* A fresh pool, upstream and options: the breaker remembers what each
 * exchange recorded, and a run of 5xx would open it. No retries, so a 5xx is
 * an answer. */
static AetherProxyOpts* new_opts(void) {
    AetherProxyPool* pool = aether_proxy_pool_new(AETHER_PROXY_LB_ROUND_ROBIN, 5, 0, 0);
    CHECK(pool != NULL, "aether_proxy_pool_new failed");
    CHECK(*aether_proxy_upstream_add(pool, "http://127.0.0.1:9", 1) == '\0', "aether_proxy_upstream_add failed");
    AetherProxyOpts* opts = aether_proxy_opts_new();
    CHECK(opts != NULL, "aether_proxy_opts_new failed");
    opts->pool = pool;   /* the options free the pool with themselves */
    return opts;
}

/* Begin an exchange for a request of `method`, as the event loop does, up to
 * the point where the upstream's answer is handed back. */
static int begin(AetherProxyExchange* px, AetherProxyOpts* opts, const char* method,
                 HttpRequest** req, HttpServerResponse** res) {
    char line[64];
    snprintf(line, sizeof line, "%s /x HTTP/1.1\r\nHost: h\r\n\r\n", method);
    *req = http_parse_request(line);
    CHECK(*req != NULL, "the request did not parse");
    *res = http_response_create();
    CHECK(*res != NULL, "http_response_create failed");
    int r = aether_proxy_exchange_begin(px, *req, *res, opts, NULL);
    CHECK(r == PX_NEED_SEND, "the exchange did not ask for a send (%d)", r);
    http_request_free_raw(px->outbound);
    free(px->upstream_url);
    px->outbound = NULL;
    px->upstream_url = NULL;
    return r;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    int is_head = data[0] & 1;
    const char* method = is_head ? "HEAD" : "GET";
    size_t len = size - 1;
    char* raw = (char*)malloc(len + 1);
    CHECK(raw != NULL, "out of memory");
    memcpy(raw, data + 1, len);
    raw[len] = '\0';

    /* Framed as the event loop frames it: complete when its framing says
     * so, the body ending where the framing says (ev_response_end). */
    HttpRespFraming f;
    memset(&f, 0, sizeof f);
    int complete = http_response_is_complete(&f, raw, len, method);
    size_t end = len;
    if (complete && f.definite && !f.chunked && !f.invalid && f.body_target >= f.header_bytes &&
        f.body_target <= len)
        end = f.body_target;

    /* The copying path, over a copy of the bytes: the client's reader
     * rewrites the buffer it is given, and the event loop runs it only after
     * the pass-through declined. */
    AetherProxyOpts* opts = new_opts();
    AetherProxyExchange px;
    HttpRequest* req;
    HttpServerResponse* res;
    begin(&px, opts, method, &req, &res);
    HttpResponse* resp = http_response_alloc_empty();
    CHECK(resp != NULL, "http_response_alloc_empty failed");
    char* raw_copy = (char*)malloc(len + 1);
    CHECK(raw_copy != NULL, "out of memory");
    memcpy(raw_copy, raw, len + 1);
    http_response_fill_from_bytes(resp, raw_copy, end);
    px.resp = resp;
    int r = aether_proxy_exchange_resume(&px, 0);
    CHECK(r != PX_NEED_SEND, "an exchange with no retries asked to send again");
    /* As the server writes it (response_serialize_wire): an answer to HEAD
     * goes without its body. */
    char* body = res->body;
    size_t body_length = res->body_length;
    if (is_head) { res->body = NULL; res->body_length = 0; }
    size_t out_len = 0;
    char* out = http_response_serialize_len(res, &out_len);
    if (is_head) { res->body = body; res->body_length = body_length; }
    CHECK(out != NULL, "the copied response did not serialize");
    const char* hend = http_find_header_end(out, out_len);
    CHECK(hend != NULL, "the copied response has no head");
    size_t head_len = (size_t)(hend + 4 - out);
    int copy_status = check_head("copying path", out, head_len, out_len - head_len, is_head);

    /* The pass-through, where the event loop would take it (ev_try_direct). */
    if (complete && !f.invalid && !f.chunked && f.definite && f.header_bytes && !f.repaired) {
        AetherProxyOpts* opts2 = new_opts();
        AetherProxyExchange px2;
        HttpRequest* req2;
        HttpServerResponse* res2;
        begin(&px2, opts2, method, &req2, &res2);
        AetherProxyDirect d;
        memset(&d, 0, sizeof d);
        if (aether_proxy_direct_take(&px2, 0, raw, end, f.header_bytes, &d) == 0) {
            char* buf = NULL;
            size_t cap = 0, dlen = 0;
            CHECK(aether_proxy_direct_head(&d, raw, f.header_bytes, &buf, &cap, &dlen) == 0,
                  "the pass-through took the response and could not write its head");
            int status = check_head("pass-through", buf, dlen, d.body_len, is_head);
            CHECK(status == d.status, "the pass-through wrote status %d for %d", status, d.status);
            CHECK(status == copy_status, "the pass-through answers %d, the copying path %d", status, copy_status);
            CHECK(dlen == head_len && memcmp(buf, out, dlen) == 0,
                  "the pass-through's head differs from the copying path's:\n%.*s\n--- copying path:\n%.*s",
                  (int)dlen, buf, (int)head_len, out);
            CHECK(d.body_len == out_len - head_len &&
                  (d.body_len == 0 || memcmp(d.body, out + head_len, d.body_len) == 0),
                  "the pass-through's body (%zu bytes) is not the copying path's (%zu)", d.body_len,
                  out_len - head_len);
            free(buf);
        } else {
            /* Declined: the copying path answers, and nothing was recorded. */
            aether_proxy_exchange_abandon(&px2);
        }
        http_server_response_free(res2);
        http_request_free(req2);
        aether_proxy_opts_free(opts2);
    }

    free(out);
    http_server_response_free(res);
    http_request_free(req);
    aether_proxy_opts_free(opts);
    free(raw_copy);
    free(raw);
    return 0;
}
