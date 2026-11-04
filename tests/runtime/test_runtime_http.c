#include "test_harness.h"
#include "../../std/net/aether_http.h"
#include "../../std/string/aether_string.h"
/* The URL resolver and the proxy URL parser (#3552), on every platform. */
#include "../../std/net/aether_http_internal.h"
/* The connect-completion contract below is exercised only where the driver
 * that depends on it is built. That driver needs a poller and a pipe, so it
 * is POSIX-only, and on Windows http_upstream_connected has no caller. */
#if !defined(_WIN32)
#include "../../std/net/aether_http_internal.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../std/http/proxy/aether_proxy_internal.h"
#include "../../std/http/proxy/aether_proxy.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#endif

TEST_CATEGORY(http_response_structure, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 200;
    resp->body = string_new("test body");
    resp->headers = string_new("Content-Type: text/html");
    resp->error = NULL;

    ASSERT_EQ(200, resp->status_code);
    ASSERT_NOT_NULL(resp->body);
    ASSERT_NOT_NULL(resp->headers);
    ASSERT_NULL(resp->error);

    http_response_free(resp);
}

TEST_CATEGORY(http_url_parsing, TEST_CATEGORY_NETWORK) {
    // Test URL query string parsing
    const char* url = "/search?q=test&limit=10";
    ASSERT_NOT_NULL(url);
    ASSERT_TRUE(strstr(url, "?") != NULL);  // Has query string
    ASSERT_TRUE(strstr(url, "q=test") != NULL);
}

TEST_CATEGORY(http_response_cleanup, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 404;
    resp->body = string_new("Not Found");
    resp->headers = NULL;
    resp->error = string_new("Error message");

    ASSERT_EQ(404, resp->status_code);
    ASSERT_NOT_NULL(resp->body);
    ASSERT_NOT_NULL(resp->error);
    ASSERT_NULL(resp->headers);
    http_response_free(resp);
}

// ---------------------------------------------------------------------------
// Response accessor tests
// ---------------------------------------------------------------------------
// These cover http_response_status/body/headers/error/ok. They construct
// HttpResponse values by hand so the tests work regardless of network access,
// and they exercise all the edge cases (success, HTTP error, transport error,
// null response, partial fields).

TEST_CATEGORY(http_accessors_success_response, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 200;
    resp->body = string_new("Hello, world");
    resp->headers = string_new("Content-Type: text/plain");
    resp->error = NULL;

    ASSERT_EQ(200, http_response_status(resp));
    /* http_response_body now returns an OWNED (retained) AetherString — read
     * its content via aether_string_data and release the extra ref. */
    const char* body = http_response_body(resp);
    ASSERT_STREQ("Hello, world", aether_string_data(body));
    string_release(body);
    ASSERT_STREQ("Content-Type: text/plain", http_response_headers(resp));
    ASSERT_STREQ("", http_response_error(resp));
    ASSERT_EQ(1, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_http_error_status, TEST_CATEGORY_NETWORK) {
    // 404 is not ok even though there's no transport error.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 404;
    resp->body = string_new("Not Found");
    resp->headers = NULL;
    resp->error = NULL;

    ASSERT_EQ(404, http_response_status(resp));
    const char* body404 = http_response_body(resp);
    ASSERT_STREQ("Not Found", aether_string_data(body404));
    string_release(body404);
    ASSERT_STREQ("", http_response_headers(resp));
    ASSERT_STREQ("", http_response_error(resp));
    ASSERT_EQ(0, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_transport_error, TEST_CATEGORY_NETWORK) {
    // Matches the shape produced by http_request() on DNS failure.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 0;
    resp->body = NULL;
    resp->headers = NULL;
    resp->error = string_new("Could not resolve host");

    ASSERT_EQ(0, http_response_status(resp));
    /* NULL body → a fresh owned empty AetherString; content is "", release it. */
    const char* ebody = http_response_body(resp);
    ASSERT_STREQ("", aether_string_data(ebody));
    string_release(ebody);
    ASSERT_STREQ("", http_response_headers(resp));
    ASSERT_STREQ("Could not resolve host", http_response_error(resp));
    ASSERT_EQ(0, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_null_response_safe, TEST_CATEGORY_NETWORK) {
    // All accessors must tolerate NULL without crashing. This guards against
    // the out-of-memory path where http_request() returns NULL directly.
    ASSERT_EQ(0, http_response_status(NULL));
    const char* nbody = http_response_body(NULL);
    ASSERT_STREQ("", aether_string_data(nbody));
    string_release(nbody);
    ASSERT_STREQ("", http_response_headers(NULL));
    ASSERT_STREQ("", http_response_error(NULL));
    ASSERT_EQ(0, http_response_ok(NULL));
}

TEST_CATEGORY(http_accessors_boundary_status_codes, TEST_CATEGORY_NETWORK) {
    // http_response_ok is defined as 2xx. Walk the boundaries.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->body = NULL;
    resp->headers = NULL;
    resp->error = NULL;

    resp->status_code = 199;
    ASSERT_EQ(0, http_response_ok(resp));  // just below 2xx
    resp->status_code = 200;
    ASSERT_EQ(1, http_response_ok(resp));  // start of 2xx
    resp->status_code = 204;
    ASSERT_EQ(1, http_response_ok(resp));  // mid 2xx
    resp->status_code = 299;
    ASSERT_EQ(1, http_response_ok(resp));  // end of 2xx
    resp->status_code = 300;
    ASSERT_EQ(0, http_response_ok(resp));  // just past 2xx
    resp->status_code = 500;
    ASSERT_EQ(0, http_response_ok(resp));  // 5xx

    http_response_free(resp);
}

/* http_upstream_connected has to separate a connect that has finished from
 * one still in flight. SO_ERROR is 0 for both, so a check built on it alone
 * called a socket with no peer "connected", and the request was then written
 * into it and failed with ENOTCONN. A poller may wake a caller for another
 * descriptor or for nothing at all, so the question gets asked when the
 * answer is genuinely still "not yet". */
#if !defined(_WIN32)
TEST_CATEGORY(http_upstream_connected_contract, TEST_CATEGORY_NETWORK) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listener >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                       /* any free port */
    ASSERT_EQ(0, bind(listener, (struct sockaddr*)&addr, sizeof(addr)));
    ASSERT_EQ(0, listen(listener, 4));

    socklen_t alen = sizeof(addr);
    ASSERT_EQ(0, getsockname(listener, (struct sockaddr*)&addr, &alen));

    /* A socket that has never been connected: still in flight, not ready. */
    HttpUpstreamConn idle;
    memset(&idle, 0, sizeof(idle));
    idle.t.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(idle.t.sockfd >= 0);
    ASSERT_EQ(0, http_upstream_connected(&idle));
    close(idle.t.sockfd);

    /* A socket with a peer: connected. This is the one the old check got
     * wrong, reporting the same value it gave for a socket with no peer. */
    HttpUpstreamConn live;
    memset(&live, 0, sizeof(live));
    live.t.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(live.t.sockfd >= 0);
    ASSERT_EQ(0, connect(live.t.sockfd, (struct sockaddr*)&addr, sizeof(addr)));
    ASSERT_EQ(1, http_upstream_connected(&live));
    close(live.t.sockfd);

    /* A closed descriptor is a failure, not a wait. */
    HttpUpstreamConn gone;
    memset(&gone, 0, sizeof(gone));
    gone.t.sockfd = -1;
    ASSERT_EQ(-1, http_upstream_connected(&gone));

    close(listener);
}
#endif  /* !_WIN32 */

/* Whether a response lets its connection carry another request, the rule both
 * clients pool by (#3259): not after Connection: close, nor after HTTP/1.0
 * unless it says keep-alive. The head is read where it lies and put back. */
static int keeps_conn(const char* raw) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", raw);
    const char* end = strstr(buf, "\r\n\r\n");
    if (!end) return -1;
    size_t header_bytes = (size_t)((end + 4) - buf);
    int keep = http_response_head_keeps_conn(buf, header_bytes);
    if (strcmp(buf, raw) != 0) return -2;   /* the head was not put back */
    return keep;
}

TEST_CATEGORY(http_response_head_keeps_conn_rule, TEST_CATEGORY_NETWORK) {
    ASSERT_EQ(1, keeps_conn("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    ASSERT_EQ(0, keeps_conn("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"));
    ASSERT_EQ(0, keeps_conn("HTTP/1.1 200 OK\r\nconnection: Close\r\n\r\n"));
    ASSERT_EQ(0, keeps_conn("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    ASSERT_EQ(1, keeps_conn("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n"));
}

#if !defined(_WIN32)
/* A reverse proxy holds an upstream connection for the length of one request
 * and hands it back, so it needs as many as it has requests in flight. Left
 * at the client's cap of eight per host, every connection past the eighth was
 * closed on release and dialled again for the next request: measured as 5.61
 * TCP segments per request against nginx's 4.02, and a TIME_WAIT socket every
 * sixth request. Mounting a proxy has to resize the pool.
 */
TEST_CATEGORY(http_pool_sized_for_proxy, TEST_CATEGORY_NETWORK) {
    /* Start from the client's defaults, whatever an earlier test left. */
    http_client_pool_configure_raw(64, 8, -1);

    int idle = 0, per_host = 0;
    http_client_pool_caps_raw(&idle, &per_host);
    ASSERT_EQ(64, idle);
    ASSERT_EQ(8, per_host);

    AetherProxyOpts* opts = aether_proxy_opts_new();
    ASSERT_NOT_NULL(opts);

    http_client_pool_caps_raw(&idle, &per_host);
    ASSERT_TRUE(idle >= 64);
    /* The point of the fix: one backend may take the whole pool, so the
     * per-host cap must rise with it rather than stay at the client's eight. */
    ASSERT_TRUE(per_host >= 64);
    ASSERT_EQ(idle, per_host);

    /* Caps are only ever raised, so a deliberate configuration survives. */
    http_client_pool_configure_raw(4096, 4096, -1);
    aether_proxy_opts_free(aether_proxy_opts_new());
    http_client_pool_caps_raw(&idle, &per_host);
    ASSERT_EQ(4096, idle);
    ASSERT_EQ(4096, per_host);

    aether_proxy_opts_free(opts);
    http_client_pool_configure_raw(64, 8, -1);
}
#endif  /* !_WIN32 */

#if !defined(_WIN32)
/* http_build_request_head allocates through the resource cap, so its buffer
 * has to be released through the cap too. Freeing it with plain free() hands
 * the memory back but leaves the counter believing those bytes are still
 * live, and the counter is what decides whether the next allocation is
 * allowed. The proxy's retry path did exactly that, so a server whose
 * upstreams were failing drifted upward until the cap started refusing
 * allocations the machine had memory for.
 *
 * This pins the contract rather than the call site: it says the buffer is
 * accounted, which is what makes a plain free() wrong. */
TEST_CATEGORY(http_request_head_is_cap_accounted, TEST_CATEGORY_NETWORK) {
    HttpClientRequest* req = http_request_raw("GET", "http://127.0.0.1:1/x");
    ASSERT_NOT_NULL(req);

    uint64_t base = aether_caps_used_bytes();

    size_t len = 0, cap = 0;
    HttpReqHead p = { req, "GET", "/x", "127.0.0.1", 80, 0, 0, NULL, 0, NULL, 1 };
    char* head = http_build_request_head(&p, &len, &cap);
    ASSERT_NOT_NULL(head);
    ASSERT_TRUE(cap > 0);
    /* The build is accounted: the counter moved. */
    ASSERT_TRUE(aether_caps_used_bytes() > base);

    aether_caps_free(head, cap);
    /* And releasing it through the cap returns the counter exactly. */
    ASSERT_EQ(base, aether_caps_used_bytes());

    http_request_free_raw(req);
}
#endif  /* !_WIN32 */

#if !defined(_WIN32)
/* The head a pass-through emits, for the responses an integration test does
 * not reach: an upstream sending no headers at all, sending only hop-by-hop
 * ones, repeating a header, or putting a line ending inside a value.
 *
 * These bytes are what a client reads, and the emitter writes them itself
 * rather than serialising a response object, so they are pinned exactly
 * rather than checked for plausibility. */
static char* direct_head_of(const char* raw, size_t body_len) {
    const char* end = strstr(raw, "\r\n\r\n");
    if (!end) return NULL;
    size_t header_bytes = (size_t)((end + 4) - raw);

    AetherProxyDirect d;
    memset(&d, 0, sizeof(d));
    d.status = 200;
    d.body = raw + header_bytes;
    d.body_len = body_len;

    char*  buf = NULL;
    size_t cap = 0, len = 0;
    if (aether_proxy_direct_head(&d, raw, header_bytes, &buf, &cap, &len) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

TEST_CATEGORY(http_proxy_direct_head_bytes, TEST_CATEGORY_NETWORK) {
    char* h;

    /* No headers at all. Content-Type and Server always appear, because the
     * response object the copying path builds is created holding both. */
    h = direct_head_of("HTTP/1.1 200 OK\r\n\r\n", 5);
    ASSERT_NOT_NULL(h);
    ASSERT_STREQ("HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Server: Aether/1.0\r\n"
                 "Content-Length: 5\r\n\r\n", h);
    free(h);

    /* The upstream's values replace both defaults, in the default positions. */
    h = direct_head_of("HTTP/1.1 200 OK\r\nServer: nginx\r\nContent-Type: text/plain\r\n"
                       "X-Tag: keep\r\nContent-Length: 99\r\n\r\n", 5);
    ASSERT_NOT_NULL(h);
    ASSERT_STREQ("HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "Server: nginx\r\n"
                 "X-Tag: keep\r\n"
                 "Content-Length: 5\r\n\r\n", h);   /* ours, not the upstream's 99 */
    free(h);

    /* Hop-by-hop headers belong to the connection this proxy terminated, and
     * must not be forwarded onto the next one. */
    h = direct_head_of("HTTP/1.1 200 OK\r\nConnection: close\r\nKeep-Alive: timeout=5\r\n"
                       "Transfer-Encoding: chunked\r\nUpgrade: h2c\r\nX-Kept: yes\r\n\r\n", 0);
    ASSERT_NOT_NULL(h);
    ASSERT_TRUE(strstr(h, "X-Kept: yes\r\n") != NULL);
    ASSERT_TRUE(strstr(h, "Connection:") == NULL);
    ASSERT_TRUE(strstr(h, "Keep-Alive:") == NULL);
    ASSERT_TRUE(strstr(h, "Transfer-Encoding:") == NULL);
    ASSERT_TRUE(strstr(h, "Upgrade:") == NULL);
    free(h);

    /* A bare carriage return inside a value would end the head early and let
     * what follows be read as a second response (CWE-113). Parsing stops at
     * the malformed line and everything after it is dropped, which is what
     * the copying path's loop does too: it takes the first CR as the end of
     * the line and gives up when no LF follows. Truncating is safe; emitting
     * the value is not. */
    h = direct_head_of("HTTP/1.1 200 OK\r\nX-Bad: a\rInjected: yes\r\nX-Good: b\r\n\r\n", 0);
    ASSERT_NOT_NULL(h);
    ASSERT_TRUE(strstr(h, "Injected") == NULL);
    ASSERT_TRUE(strstr(h, "X-Bad") == NULL);
    ASSERT_TRUE(strstr(h, "X-Good") == NULL);   /* parsing stopped, as above */
    /* And the head is still well formed: it ends with the blank line. */
    {
        size_t hn = strlen(h);
        ASSERT_TRUE(hn >= 4 && strcmp(h + hn - 4, "\r\n\r\n") == 0);
    }
    free(h);

    /* An empty value stays an empty value rather than becoming a malformed
     * line. */
    h = direct_head_of("HTTP/1.1 200 OK\r\nX-Empty:\r\n\r\n", 0);
    ASSERT_NOT_NULL(h);
    ASSERT_TRUE(strstr(h, "X-Empty: \r\n") != NULL);
    free(h);

    /* The head always ends with the blank line that separates it from a body. */
    h = direct_head_of("HTTP/1.1 200 OK\r\nX-A: 1\r\n\r\n", 0);
    ASSERT_NOT_NULL(h);
    size_t n = strlen(h);
    ASSERT_TRUE(n >= 4 && strcmp(h + n - 4, "\r\n\r\n") == 0);
    free(h);
}
#endif  /* !_WIN32 */

#if !defined(_WIN32)
/* A connection builds a request head per request. It used to take a fresh
 * allocation each time and release it a moment later; it now reuses one
 * buffer, released when the connection ends.
 *
 * That moves the buffer's lifetime from a request to a connection, and the
 * cap's accounting has to follow: a build that reused the buffer must not
 * account for it again, or a long-lived connection drifts upward until the
 * cap refuses allocations the machine has memory for. */
TEST_CATEGORY(http_request_head_reuse_is_accounted, TEST_CATEGORY_NETWORK) {
    HttpClientRequest* req = http_request_raw("GET", "http://127.0.0.1:1/x");
    ASSERT_NOT_NULL(req);

    uint64_t base = aether_caps_used_bytes();

    char*  buf = NULL;
    size_t cap = 0, len = 0;
    HttpReqHead p = { req, "GET", "/x", "127.0.0.1", 80, 0, 0, NULL, 0, NULL, 1 };

    ASSERT_NOT_NULL(http_build_request_head_into(&p, &buf, &cap, &len));
    uint64_t after_first = aether_caps_used_bytes();
    ASSERT_TRUE(after_first > base);
    char*  first_buf = buf;
    size_t first_cap = cap;
    size_t first_len = len;

    /* Many more builds into the same buffer: same pointer, same capacity,
     * same accounted total. Nothing new is taken. */
    for (int i = 0; i < 64; i++) {
        ASSERT_NOT_NULL(http_build_request_head_into(&p, &buf, &cap, &len));
        ASSERT_TRUE(buf == first_buf);
        ASSERT_EQ((int)first_cap, (int)cap);
        ASSERT_EQ((int)first_len, (int)len);   /* and it rebuilds, not appends */
    }
    ASSERT_EQ(after_first, aether_caps_used_bytes());

    /* One release, with the capacity the buffer actually reached, returns the
     * counter exactly. */
    aether_caps_free(buf, cap);
    ASSERT_EQ(base, aether_caps_used_bytes());

    http_request_free_raw(req);
}
#endif  /* !_WIN32 */

#if !defined(_WIN32)
/* A driver pins the clock for one pass of its loop so the several readers in a
 * request share one counter access. The contract is narrow and worth stating:
 * pinned, everyone sees the same millisecond; unpinned, everyone sees the real
 * clock; and pinning again takes a fresh reading. Getting the last one wrong
 * would freeze every deadline in the process. */
TEST_CATEGORY(http_clock_pin_contract, TEST_CATEGORY_NETWORK) {
    http_clock_unpin();

    struct timespec nap = { 0, 3 * 1000 * 1000 };   /* 3ms, clear of a tick */

    /* Unpinned, it moves. */
    uint64_t a = http_clock_ms();
    nanosleep(&nap, NULL);
    uint64_t b = http_clock_ms();
    ASSERT_TRUE(b > a);

    /* Pinned, it does not, however long the pass takes. */
    http_clock_pin();
    uint64_t p1 = http_clock_ms();
    nanosleep(&nap, NULL);
    ASSERT_EQ(p1, http_clock_ms());
    ASSERT_TRUE(p1 >= b);

    /* Pinning again reads afresh: a loop that never advanced its pinned clock
     * would never time anything out. */
    http_clock_pin();
    ASSERT_TRUE(http_clock_ms() > p1);

    /* And releasing it goes back to the real clock. */
    http_clock_unpin();
    uint64_t c = http_clock_ms();
    nanosleep(&nap, NULL);
    ASSERT_TRUE(http_clock_ms() > c);
}
#endif  /* !_WIN32 */

/* The token test is a table now, and a table is only as good as the grammar it
 * was written from. This walks every byte value and compares against RFC 9110's
 * token rule spelled out independently, so a mistyped entry cannot pass. A
 * character wrongly accepted here is a header name the proxy would forward
 * unaltered. */
TEST_CATEGORY(http_header_name_token_set, TEST_CATEGORY_NETWORK) {
    for (int i = 1; i < 256; i++) {
        char one[2] = { (char)i, '\0' };
        int alpha = (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z');
        int digit = (i >= '0' && i <= '9');
        int punct = strchr("!#$%&'*+-.^_`|~", i) != NULL && i != 0;
        int want  = (alpha || digit || punct) ? 1 : 0;
        ASSERT_EQ(want, http_header_name_ok(one) ? 1 : 0);
    }

    /* The empty name and a NULL are not tokens either. */
    ASSERT_EQ(0, http_header_name_ok(""));
    ASSERT_EQ(0, http_header_name_ok(NULL));

    /* A whole name, and one with a single bad byte in it. */
    ASSERT_EQ(1, http_header_name_ok("X-Forwarded-For"));
    ASSERT_EQ(0, http_header_name_ok("X-Forwarded For"));
    ASSERT_EQ(0, http_header_name_ok("X-Bad\r\nInjected"));
}

/* The digits the proxy now writes itself instead of asking snprintf for. */
TEST_CATEGORY(http_write_dec_digits, TEST_CATEGORY_NETWORK) {
    struct { unsigned long long v; const char* want; } cases[] = {
        { 0, "0" }, { 7, "7" }, { 10, "10" }, { 80, "80" }, { 443, "443" },
        { 19001, "19001" }, { 65535, "65535" }, { 4294967295ULL, "4294967295" },
        { 18446744073709551615ULL, "18446744073709551615" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[32];
        size_t n = http_write_dec(buf, cases[i].v);
        buf[n] = '\0';
        ASSERT_EQ(strlen(cases[i].want), n);
        ASSERT_STREQ(cases[i].want, buf);
    }
}

/* The header-block scan rejects most lines on the colon position and the first
 * letter before it compares the name properly. A prefilter that is too eager
 * drops a header the proxy is required to see, so this checks the cases that
 * would expose one: mixed case, a name that is a prefix of another, a first
 * letter differing only in case, and a value that must still be read whole. */
TEST_CATEGORY(http_find_header_in_block_matching, TEST_CATEGORY_NETWORK) {
    const char* block =
        "Content-Length: 42\r\n"
        "content-type: text/plain\r\n"
        "X-Long-Name: kept\r\n"
        "X: short\r\n";
    const char* end = block + strlen(block);
    char out[64];
    int differing = 0;

    ASSERT_EQ(1, http_find_header_in_block(block, end, "Content-Length",
                                           out, sizeof(out), &differing));
    ASSERT_STREQ("42", out);

    /* Matching is case-insensitive in both directions. */
    ASSERT_EQ(1, http_find_header_in_block(block, end, "CONTENT-LENGTH",
                                           out, sizeof(out), &differing));
    ASSERT_STREQ("42", out);
    ASSERT_EQ(1, http_find_header_in_block(block, end, "Content-Type",
                                           out, sizeof(out), &differing));
    ASSERT_STREQ("text/plain", out);

    /* A one-character name must not match a longer one starting with it. */
    ASSERT_EQ(1, http_find_header_in_block(block, end, "X",
                                           out, sizeof(out), &differing));
    ASSERT_STREQ("short", out);

    /* A name that is not there is not found. */
    ASSERT_EQ(0, http_find_header_in_block(block, end, "Server",
                                           out, sizeof(out), &differing));

    /* Two of the same name, disagreeing, is what `differing` reports: the
     * proxy refuses a request framed by two different Content-Lengths. */
    const char* dup = "Content-Length: 1\r\nContent-Length: 2\r\n";
    ASSERT_EQ(2, http_find_header_in_block(dup, dup + strlen(dup),
                                           "Content-Length",
                                           out, sizeof(out), &differing));
    ASSERT_EQ(1, differing);
}

/* The header-block terminator is found by scanning for CR rather than with
 * strstr. An off-by-one here either misses a complete header block, hanging
 * the read, or reports one that is not there, which is a framing error. The
 * boundary cases are what this covers. */
TEST_CATEGORY(http_find_header_end_boundaries, TEST_CATEGORY_NETWORK) {
    const char* full = "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody";
    const char* hit = http_find_header_end(full, strlen(full));
    ASSERT_NOT_NULL(hit);
    ASSERT_EQ(0, memcmp(hit, "\r\n\r\n", 4));
    ASSERT_STREQ("body", hit + 4);

    /* Terminator sitting exactly at the end of the buffer. */
    const char* exact = "A: b\r\n\r\n";
    ASSERT_NOT_NULL(http_find_header_end(exact, strlen(exact)));

    /* One byte short of a terminator is not one. */
    ASSERT_NULL(http_find_header_end("A: b\r\n\r", 7));
    ASSERT_NULL(http_find_header_end("\r\n\r", 3));
    ASSERT_NULL(http_find_header_end("", 0));
    ASSERT_NULL(http_find_header_end("\r\n\r\n", 3));

    /* The shortest possible match, and a lone CR before it. */
    ASSERT_NOT_NULL(http_find_header_end("\r\n\r\n", 4));
    const char* decoy = "\rX\r\n\r\n";
    ASSERT_EQ(2, (int)(http_find_header_end(decoy, strlen(decoy)) - decoy));

    /* A CR run that only completes on the last possible byte. */
    const char* run = "\r\r\r\r\n\r\n";
    ASSERT_EQ(3, (int)(http_find_header_end(run, strlen(run)) - run));

    /* Not present at all, and binary-safe past an embedded NUL. */
    ASSERT_NULL(http_find_header_end("no terminator here", 18));
    const char nul[] = { 'A', '\0', '\r', '\n', '\r', '\n' };
    ASSERT_EQ(2, (int)(http_find_header_end(nul, sizeof(nul)) - nul));
}

/* Response framing reads Transfer-Encoding where it lies rather than copying
 * it out. That matters because the whole value decides how the body is framed:
 * a copy into a fixed buffer truncates, and a "chunked" past the cut would be
 * missed, which is the framing disagreement request smuggling is built on.
 * This pins the span to the full value and shows the copying form does not
 * reach that far. */
TEST_CATEGORY(http_find_header_span_is_untruncated, TEST_CATEGORY_NETWORK) {
    char block[1024];
    int n = snprintf(block, sizeof(block), "Server: x\r\nTransfer-Encoding: ");
    for (int i = 0; i < 300; i++) block[n++] = 'a';
    n += snprintf(block + n, sizeof(block) - (size_t)n, ", chunked\r\n");
    const char* end = block + n;

    const char* v = NULL;
    size_t vl = 0;
    ASSERT_EQ(1, http_find_header_span(block, end, "Transfer-Encoding",
                                       NULL, 0, NULL, &v, &vl));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ((size_t)(300 + 9), vl);              /* padding + ", chunked" */
    ASSERT_EQ(0, memcmp(v + vl - 7, "chunked", 7));

    /* The copying form stops well before that, which is why framing uses the
     * span and not this. */
    char small[64];
    ASSERT_EQ(1, http_find_header_in_block(block, end, "Transfer-Encoding",
                                           small, sizeof(small), NULL));
    ASSERT_EQ((size_t)63, strlen(small));
    ASSERT_NULL(strstr(small, "chunked"));

    /* The span still reports a value the copying form can hold. */
    const char* plain = "Transfer-Encoding: chunked\r\n";
    v = NULL; vl = 0;
    ASSERT_EQ(1, http_find_header_span(plain, plain + strlen(plain),
                                       "transfer-encoding",
                                       NULL, 0, NULL, &v, &vl));
    ASSERT_EQ((size_t)7, vl);
    ASSERT_EQ(0, memcmp(v, "chunked", 7));

    /* Absent means no span written and a zero count. */
    v = (const char*)0x1; vl = 99;
    ASSERT_EQ(0, http_find_header_span(plain, plain + strlen(plain),
                                       "Content-Length", NULL, 0, NULL,
                                       &v, &vl));
    ASSERT_EQ((size_t)99, vl);
}

/* The inline case-insensitive compare replaces strncasecmp on the proxy's hot
 * path, so it has to agree with it on every input, not merely on the names it
 * happens to be used with. The interesting pairs are bytes differing only in
 * 0x20 that are not letters: '^' and '~', '@' and '`', '-' and CR. A compare
 * that treats those as equal would match a header name that is not the one it
 * was looking for. This walks all 65,536 byte pairs. */
TEST_CATEGORY(http_ci_eq_matches_strncasecmp, TEST_CATEGORY_NETWORK) {
    int mismatches = 0;
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 256; j++) {
            char a[2] = { (char)i, '\0' };
            char b[2] = { (char)j, '\0' };
            int want = (strncasecmp(a, b, 1) == 0) ? 1 : 0;
            if (http_ci_eq(a, b, 1) != want) mismatches++;
        }
    }
    ASSERT_EQ(0, mismatches);

    /* The pairs that a naive 0x20 trick gets wrong, called out by name. */
    ASSERT_EQ(0, http_ci_eq("^", "~", 1));
    ASSERT_EQ(0, http_ci_eq("@", "`", 1));
    ASSERT_EQ(0, http_ci_eq("\r", "-", 1));
    ASSERT_EQ(0, http_ci_eq("\0", " ", 1));

    /* And the names it is actually used on. */
    ASSERT_EQ(1, http_ci_eq("content-type", "Content-Type", 12));
    ASSERT_EQ(1, http_ci_eq("CONTENT-LENGTH", "Content-Length", 14));
    ASSERT_EQ(1, http_ci_eq("server", "Server", 6));
    ASSERT_EQ(0, http_ci_eq("servel", "Server", 6));
    ASSERT_EQ(1, http_ci_eq("anything", "anything", 0));
}

/* Reference resolution (#3552). The redirect path resolved a Location by
 * hand: from parse_url's host, which has no IPv6 brackets, and from the last
 * '/' of the base's path and query together. It now resolves by RFC 3986 5.2
 * on the parsed parts. These are RFC 3986 5.4's examples, normal (5.4.1) and
 * abnormal (5.4.2), against its base, then the two shapes of the issue. Each
 * miss is printed, so one run names every one. */
static int uri_resolve_misses(const char* base, const char* const (*cases)[2], int n) {
    int misses = 0;
    for (int i = 0; i < n; i++) {
        char* got = http_uri_resolve(base, cases[i][0]);
        if (!got || strcmp(got, cases[i][1]) != 0) {
            fprintf(stderr, "  resolve '%s' against '%s': got '%s', want '%s'\n",
                    cases[i][0], base, got ? got : "(null)", cases[i][1]);
            misses++;
        }
        free(got);
    }
    return misses;
}

TEST_CATEGORY(http_uri_resolve_rfc3986_examples, TEST_CATEGORY_NETWORK) {
    static const char* const normal[][2] = {
        { "g:h", "g:h" },           { "g", "http://a/b/c/g" },
        { "./g", "http://a/b/c/g" }, { "g/", "http://a/b/c/g/" },
        { "/g", "http://a/g" },     { "//g", "http://g" },
        { "?y", "http://a/b/c/d;p?y" }, { "g?y", "http://a/b/c/g?y" },
        { "#s", "http://a/b/c/d;p?q#s" }, { "g#s", "http://a/b/c/g#s" },
        { "g?y#s", "http://a/b/c/g?y#s" }, { ";x", "http://a/b/c/;x" },
        { "g;x", "http://a/b/c/g;x" }, { "g;x?y#s", "http://a/b/c/g;x?y#s" },
        { "", "http://a/b/c/d;p?q" }, { ".", "http://a/b/c/" },
        { "./", "http://a/b/c/" },  { "..", "http://a/b/" },
        { "../", "http://a/b/" },   { "../g", "http://a/b/g" },
        { "../..", "http://a/" },   { "../../", "http://a/" },
        { "../../g", "http://a/g" },
    };
    static const char* const abnormal[][2] = {
        { "../../../g", "http://a/g" }, { "../../../../g", "http://a/g" },
        { "/./g", "http://a/g" },   { "/../g", "http://a/g" },
        { "g.", "http://a/b/c/g." }, { ".g", "http://a/b/c/.g" },
        { "g..", "http://a/b/c/g.." }, { "..g", "http://a/b/c/..g" },
        { "./../g", "http://a/b/g" }, { "./g/.", "http://a/b/c/g/" },
        { "g/./h", "http://a/b/c/g/h" }, { "g/../h", "http://a/b/c/h" },
        { "g;x=1/./y", "http://a/b/c/g;x=1/y" }, { "g;x=1/../y", "http://a/b/c/y" },
        { "g?y/./x", "http://a/b/c/g?y/./x" }, { "g?y/../x", "http://a/b/c/g?y/../x" },
        { "g#s/./x", "http://a/b/c/g#s/./x" }, { "g#s/../x", "http://a/b/c/g#s/../x" },
        { "http:g", "http:g" },
    };
    int misses = uri_resolve_misses("http://a/b/c/d;p?q", normal,
                                    (int)(sizeof normal / sizeof normal[0]));
    misses += uri_resolve_misses("http://a/b/c/d;p?q", abnormal,
                                 (int)(sizeof abnormal / sizeof abnormal[0]));
    ASSERT_EQ(0, misses);
}

TEST_CATEGORY(http_uri_resolve_keeps_shapes, TEST_CATEGORY_NETWORK) {
    /* An IPv6 authority keeps its brackets and its port. */
    static const char* const v6[][2] = {
        { "/b", "http://[::1]:8080/b" }, { "b?x", "http://[::1]:8080/b?x" },
        { "//[::2]/c", "http://[::2]/c" },
    };
    int misses = uri_resolve_misses("http://[::1]:8080/a", v6, (int)(sizeof v6 / sizeof v6[0]));
    /* A relative path merges with the base's path, not its query. */
    static const char* const q[][2] = {
        { "c", "http://h/a/c" }, { "?z", "http://h/a/b?z" }, { "", "http://h/a/b?x=/y" },
    };
    misses += uri_resolve_misses("http://h/a/b?x=/y", q, (int)(sizeof q / sizeof q[0]));
    /* A base with an authority and an empty path merges under "/". */
    static const char* const empty[][2] = { { "g", "http://h:81/g" } };
    misses += uri_resolve_misses("http://h:81", empty, 1);
    /* The scheme comes out in lower case; a space and bytes past ASCII come
     * out percent-encoded in the path, the query and the fragment, and a
     * '%' already there stays. */
    static const char* const enc[][2] = {
        { "HTTPS://H/x", "https://H/x" }, { "a b", "http://h/d/a%20b" },
        { "/\xc3\xa9?q=\xc3\xa9#\xc3\xa9 ", "http://h/%C3%A9?q=%C3%A9#%C3%A9%20" },
        { "x%20y", "http://h/d/x%20y" },
    };
    misses += uri_resolve_misses("http://h/d/e", enc, (int)(sizeof enc / sizeof enc[0]));
    ASSERT_EQ(0, misses);

    /* Not a URI reference: a first segment with a ':' that ends no scheme,
     * and a base without a scheme. */
    ASSERT_NULL(http_uri_resolve("http://h/", "1a:b"));
    ASSERT_NULL(http_uri_resolve("http://h/", ":b"));
    ASSERT_NULL(http_uri_resolve("/a/b", "c"));
}

TEST_CATEGORY(http_resolve_location_redirect_rules, TEST_CATEGORY_NETWORK) {
    /* A base without a scheme is http, as parse_url reads it. */
    char* got = http_resolve_location("h:8080/a/b", "c");
    ASSERT_NOT_NULL(got);
    ASSERT_STREQ("http://h:8080/a/c", got);
    free(got);
    /* A Location without a fragment keeps the request's (RFC 9110 10.2.2);
     * one with a fragment has its own. */
    got = http_resolve_location("http://h/a#top", "/b");
    ASSERT_NOT_NULL(got);
    ASSERT_STREQ("http://h/b#top", got);
    free(got);
    got = http_resolve_location("http://h/a#top", "/b#end");
    ASSERT_NOT_NULL(got);
    ASSERT_STREQ("http://h/b#end", got);
    free(got);
    /* A query naming a URL is no scheme: "x?u=http://y" is relative. */
    got = http_resolve_location("http://h/a/b", "x?u=http://y");
    ASSERT_NOT_NULL(got);
    ASSERT_STREQ("http://h/a/x?u=http://y", got);
    free(got);
}

TEST_CATEGORY(http_parse_proxy_url_shapes, TEST_CATEGORY_NETWORK) {
    char host[256];
    int port = 0;
    ASSERT_EQ(0, http_parse_proxy_url("http://[::1]:3129", host, sizeof host, &port));
    ASSERT_STREQ("::1", host);
    ASSERT_EQ(3129, port);
    ASSERT_EQ(0, http_parse_proxy_url("[::1]", host, sizeof host, &port));
    ASSERT_STREQ("::1", host);
    ASSERT_EQ(3128, port);
    ASSERT_EQ(0, http_parse_proxy_url("http://proxy:8080?x", host, sizeof host, &port));
    ASSERT_STREQ("proxy", host);
    ASSERT_EQ(8080, port);
    ASSERT_EQ(0, http_parse_proxy_url("https://proxy/", host, sizeof host, &port));
    ASSERT_STREQ("proxy", host);
    ASSERT_EQ(3128, port);
    /* An unclosed bracket, anything but a port after the closing one, an
     * empty host, an empty or bad port, and an IPv6 host without brackets. */
    ASSERT_EQ(-1, http_parse_proxy_url("http://[::1", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://[::1]x", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://:3128", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://[]:3128", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://proxy:", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://proxy:70000", host, sizeof host, &port));
    ASSERT_EQ(-1, http_parse_proxy_url("http://::1:3128", host, sizeof host, &port));
}

/* A NUL or a bare CR in a response's header section (#3556). The buffered
 * reader found the end of the section with strstr, which stops at a NUL, and
 * a bare CR stayed inside a field value, so a Location carrying one went out
 * in the request line of the redirect. The section is now found past any NUL
 * and refused when it holds either; a NUL in the body is body. */
static HttpResponse* filled(const char* bytes, size_t n) {
    HttpResponse* resp = http_response_alloc_empty();
    char* buf = (char*)malloc(n + 1);
    memcpy(buf, bytes, n);
    buf[n] = '\0';
    http_response_fill_from_bytes(resp, buf, n);
    free(buf);
    return resp;
}

TEST_CATEGORY(http_response_head_refuses_nul_and_bare_cr, TEST_CATEGORY_NETWORK) {
    static const char nul_head[] = "HTTP/1.1 302 Found\r\nLocation: /x\0y\r\n\r\n";
    HttpResponse* r = filled(nul_head, sizeof nul_head - 1);
    ASSERT_NOT_NULL(r->error);
    ASSERT_STREQ("malformed response header section", string_to_cstr(r->error));
    ASSERT_EQ(0, r->status_code);
    ASSERT_NULL(r->headers);
    http_response_free(r);

    static const char cr_head[] = "HTTP/1.1 302 Found\r\nLocation: /x\rX-Injected: y\r\n\r\n";
    r = filled(cr_head, sizeof cr_head - 1);
    ASSERT_NOT_NULL(r->error);
    ASSERT_STREQ("malformed response header section", string_to_cstr(r->error));
    ASSERT_NULL(r->headers);
    http_response_free(r);

    /* A CR right before the empty line that is not followed by LF. */
    static const char cr_end[] = "HTTP/1.1 200 OK\r\nA: b\r\r\n\r\n";
    r = filled(cr_end, sizeof cr_end - 1);
    ASSERT_NOT_NULL(r->error);
    http_response_free(r);

    /* A NUL in the body is the body's. */
    static const char nul_body[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b";
    r = filled(nul_body, sizeof nul_body - 1);
    ASSERT_NULL(r->error);
    ASSERT_EQ(200, r->status_code);
    ASSERT_EQ(3, http_response_body_length(r));
    http_response_free(r);
}

TEST_CATEGORY(http_uri_resolve_refuses_control_bytes, TEST_CATEGORY_NETWORK) {
    static const char* const refs[] = { "/x\ty", "/x\x01y", "/x\ry", "/x\ny", "/x\x7fy", "\t" };
    for (size_t i = 0; i < sizeof refs / sizeof refs[0]; i++) {
        char* got = http_uri_resolve("http://h/a", refs[i]);
        if (got) fprintf(stderr, "  resolved a control byte: '%s'\n", got);
        ASSERT_NULL(got);
    }
    ASSERT_NULL(http_resolve_location("http://h/a", "/x\ty"));
}
