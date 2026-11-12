#include "test_harness.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>   /* getsockname; ahead of anything that pulls in windows.h */
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#endif
#include "../../std/net/aether_http_server.h"
#include "../../std/net/aether_http.h"   /* http_find_header_end */
#include "../../std/http/proxy/aether_proxy.h"
#include "../../std/http/proxy/aether_proxy_internal.h"
#include "../../std/http/middleware/aether_middleware.h"
#if !defined(_WIN32)
#include "../../std/net/aether_http_internal.h"
#endif

// HTTP Server Tests - Real tests for implemented functionality

TEST(http_server_create) {
    HttpServer* server = http_server_create(8080);
    ASSERT_NOT_NULL(server);
    ASSERT_EQ(8080, server->port);
    ASSERT_EQ(0, server->is_running);
    http_server_free(server);
}

/* A host name binds the IPv4 address it resolves to (#2639). It used to go
 * to inet_pton alone, which failed and left the address zeroed, INADDR_ANY:
 * a server asked to listen on "localhost" listened on every interface. */
TEST(http_server_bind_resolves_a_host_name) {
    HttpServer* server = http_server_create(0);
    ASSERT_NOT_NULL(server);
    /* loopback-ok: "localhost" resolves to loopback, which is what this asserts */
    ASSERT_EQ(0, http_server_bind_raw(server, "localhost", 0));
    struct sockaddr_in got;
    socklen_t len = sizeof(got);
    memset(&got, 0, sizeof(got));
    ASSERT_EQ(0, getsockname(server->socket_fd, (struct sockaddr*)&got, &len));
    ASSERT_EQ(127, (int)(ntohl(got.sin_addr.s_addr) >> 24));
    http_server_free(server);
}

TEST(http_server_bind_refuses_a_name_that_does_not_resolve) {
    HttpServer* server = http_server_create(0);
    ASSERT_NOT_NULL(server);
    /* loopback-ok: refused before any bind */
    ASSERT_EQ(-1, http_server_bind_raw(server, "no-such-host.invalid", 0));
    ASSERT_EQ(-1, server->socket_fd);
    http_server_free(server);
}

TEST(http_server_request_parsing) {
    const char* raw_request =
        "GET /api/users/123 HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: Test\r\n"
        "Content-Type: application/json\r\n"
        "\r\n";

    HttpRequest* req = http_parse_request(raw_request);
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("GET", req->method);
    ASSERT_STREQ("/api/users/123", req->path);
    ASSERT_STREQ("HTTP/1.1", req->http_version);
    http_request_free(req);
}

TEST(http_server_request_headers) {
    const char* raw_request =
        "POST /api/data HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer token123\r\n"
        "\r\n";

    HttpRequest* req = http_parse_request(raw_request);
    ASSERT_NOT_NULL(req);

    const char* content_type = http_get_header(req, "Content-Type");
    const char* auth = http_get_header(req, "Authorization");

    ASSERT_NOT_NULL(content_type);
    ASSERT_STREQ("application/json", content_type);
    ASSERT_NOT_NULL(auth);
    ASSERT_STREQ("Bearer token123", auth);

    http_request_free(req);
}

TEST(http_server_response_building) {
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);

    http_response_set_status(resp, 200);
    ASSERT_EQ(200, resp->status_code);

    http_response_set_header(resp, "Content-Type", "application/json");
    http_response_set_body(resp, "{\"message\":\"Hello\"}");

    char* raw = http_response_serialize(resp);
    ASSERT_NOT_NULL(raw);
    ASSERT_TRUE(strstr(raw, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(raw, "Content-Type: application/json") != NULL);
    ASSERT_TRUE(strstr(raw, "{\"message\":\"Hello\"}") != NULL);

    free(raw);
    http_server_response_free(resp);
}

/* #3297: set_body_n takes its body as bytes. A body that begins like a
 * string header (magic DE C0 57 AE, refcount 0, length and capacity 1, a
 * non-null "data pointer" made of payload bytes) was taken for one, and the
 * copy read from that pointer: a crash, or a wild read of what an upstream
 * or a cached body pointed it at. */
TEST(http_server_response_set_body_n_takes_bytes) {
    unsigned char body[40];
    memset(body, 0x01, sizeof body);
    body[0] = 0xDE; body[1] = 0xC0; body[2] = 0x57; body[3] = 0xAE;
    memset(body + 4, 0, 4);                 /* ref_count 0 */
    memset(body + 8, 0, 16);
    body[8] = 1;                            /* length 1 */
    body[16] = 1;                           /* capacity 1 */
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);
    http_response_set_body_n(resp, (const char*)body, (int)sizeof body);
    ASSERT_EQ((int)sizeof body, (int)resp->body_length);
    ASSERT_TRUE(memcmp(resp->body, body, sizeof body) == 0);
    http_server_response_free(resp);
}

/* #1739: a response reused across requests, and a header overwritten with a
 * value that fits, cost no allocation -- while every header string stays
 * its own allocation, because C outside the library frees them one by one
 * (tests/integration/http_external_ptr). Reuse is asserted by pointer
 * identity: the same string coming back means nothing was allocated. */
TEST(http_server_response_header_reuse) {
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);
    ASSERT_EQ(2, resp->header_count);

    /* A shorter value overwrites the default in place. */
    char* ct_before = resp->header_values[0];
    http_response_set_header(resp, "Content-Type", "text/plain");
    ASSERT_TRUE(resp->header_values[0] == ct_before);
    ASSERT_STREQ("text/plain", resp->header_values[0]);

    /* A longer one needs its own string, and gets the right text. */
    http_response_set_header(resp, "Content-Type",
                             "application/vnd.example+json; charset=utf-8");
    ASSERT_STREQ("application/vnd.example+json; charset=utf-8", resp->header_values[0]);

    /* What a request adds past the defaults... */
    http_response_set_header(resp, "Content-Length", "12");
    http_response_set_header(resp, "Connection", "keep-alive");
    ASSERT_EQ(4, resp->header_count);
    http_response_set_status(resp, 404);

    /* ...is released by reset, and the defaults come back in the strings
     * already there -- identical keys, the long Content-Type value reused
     * for the shorter default, the status text for "OK". */
    char* k0 = resp->header_keys[0];
    char* v0 = resp->header_values[0];
    char* k1 = resp->header_keys[1];
    char* v1 = resp->header_values[1];
    char* st = resp->status_text;
    http_response_reset(resp);

    ASSERT_EQ(2, resp->header_count);
    ASSERT_EQ(200, resp->status_code);
    ASSERT_STREQ("Content-Type", resp->header_keys[0]);
    ASSERT_STREQ("text/html; charset=utf-8", resp->header_values[0]);
    ASSERT_STREQ("Server", resp->header_keys[1]);
    ASSERT_STREQ("Aether/1.0", resp->header_values[1]);
    ASSERT_STREQ("OK", resp->status_text);
    ASSERT_TRUE(resp->header_keys[0] == k0);
    ASSERT_TRUE(resp->header_values[0] == v0);
    ASSERT_TRUE(resp->header_keys[1] == k1);
    ASSERT_TRUE(resp->header_values[1] == v1);
    ASSERT_TRUE(resp->status_text == st);

    /* The ownership convention is intact: each string is freed by itself,
     * exactly as an external caller does it. */
    for (int i = 0; i < resp->header_count; i++) {
        free(resp->header_keys[i]);
        free(resp->header_values[i]);
        resp->header_keys[i] = NULL;
        resp->header_values[i] = NULL;
    }
    resp->header_count = 0;
    http_server_response_free(resp);
}

TEST(http_server_json_response) {
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);

    http_response_json(resp, "{\"status\":\"ok\"}");

    ASSERT_EQ(200, resp->status_code);
    ASSERT_NOT_NULL(resp->body);
    ASSERT_STREQ("{\"status\":\"ok\"}", resp->body);

    http_server_response_free(resp);
}

TEST(http_server_status_text) {
    ASSERT_STREQ("OK", http_status_text(200));
    ASSERT_STREQ("Created", http_status_text(201));
    ASSERT_STREQ("Bad Request", http_status_text(400));
    ASSERT_STREQ("Not Found", http_status_text(404));
    ASSERT_STREQ("Internal Server Error", http_status_text(500));
    // Every registered code has its phrase, not only the common ten (#3147):
    // a 409 or a 429 used to go out as "Unknown".
    ASSERT_STREQ("Conflict", http_status_text(409));
    ASSERT_STREQ("Too Many Requests", http_status_text(429));
    ASSERT_STREQ("Permanent Redirect", http_status_text(308));
    ASSERT_STREQ("Network Authentication Required", http_status_text(511));
    // An unregistered code has no phrase; the status line carries none.
    ASSERT_STREQ("", http_status_text(418));
    ASSERT_STREQ("", http_status_text(599));
}

TEST(http_server_mime_types) {
    // MIME types may include charset
    const char* html = http_mime_type("index.html");
    const char* css = http_mime_type("style.css");
    const char* js = http_mime_type("app.js");
    const char* json = http_mime_type("data.json");
    const char* png = http_mime_type("logo.png");
    const char* jpg = http_mime_type("photo.jpg");

    ASSERT_NOT_NULL(html);
    ASSERT_TRUE(strstr(html, "text/html") != NULL);
    ASSERT_NOT_NULL(css);
    ASSERT_TRUE(strstr(css, "text/css") != NULL);
    ASSERT_NOT_NULL(js);
    ASSERT_TRUE(strstr(js, "javascript") != NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(strstr(json, "json") != NULL);
    ASSERT_NOT_NULL(png);
    ASSERT_TRUE(strstr(png, "image/png") != NULL);
    ASSERT_NOT_NULL(jpg);
    ASSERT_TRUE(strstr(jpg, "image/jpeg") != NULL);
}

TEST(http_server_route_matching_exact) {
    HttpRequest* req = http_parse_request("GET /users HTTP/1.1\r\n\r\n");
    ASSERT_NOT_NULL(req);

    int result = http_route_matches("/users", "/users", req);
    ASSERT_EQ(1, result);

    result = http_route_matches("/other", "/users", req);
    ASSERT_EQ(0, result);

    http_request_free(req);
}

TEST(http_server_route_matching_params) {
    HttpRequest* req = http_parse_request("GET /users/123 HTTP/1.1\r\n\r\n");
    ASSERT_NOT_NULL(req);

    int result = http_route_matches("/users/:id", "/users/123", req);
    ASSERT_EQ(1, result);

    // Check that the param was captured
    const char* id = http_get_path_param(req, "id");
    ASSERT_NOT_NULL(id);
    ASSERT_STREQ("123", id);

    http_request_free(req);
}

TEST(http_server_query_params) {
    const char* raw = "GET /search?q=test&limit=10 HTTP/1.1\r\n\r\n";
    HttpRequest* req = http_parse_request(raw);
    ASSERT_NOT_NULL(req);

    // Query string should be parsed
    ASSERT_NOT_NULL(req->query_string);
    ASSERT_TRUE(strstr(req->query_string, "q=test") != NULL);
    ASSERT_TRUE(strstr(req->query_string, "limit=10") != NULL);

    // Get params if implementation supports it
    const char* q = http_get_query_param(req, "q");
    const char* limit = http_get_query_param(req, "limit");

    // At minimum, one of them should work
    ASSERT_TRUE(q != NULL || limit != NULL);

    http_request_free(req);
}

TEST(http_server_add_routes) {
    HttpServer* server = http_server_create(8080);
    ASSERT_NOT_NULL(server);

    // Add some routes (handlers are NULL for this test)
    http_server_get(server, "/users", NULL, NULL);
    http_server_post(server, "/users", NULL, NULL);
    http_server_put(server, "/users/:id", NULL, NULL);
    http_server_delete(server, "/users/:id", NULL, NULL);

    // Verify routes were added
    ASSERT_NOT_NULL(server->routes);

    http_server_free(server);
}

TEST(http_server_post_with_body) {
    const char* raw_request =
        "POST /api/users HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        "{\"name\":\"John Doe\"}";

    HttpRequest* req = http_parse_request(raw_request);
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("POST", req->method);
    // Note: Body parsing may need additional implementation
    http_request_free(req);
}

TEST(http_server_post_binary_body_with_embedded_nul) {
    /* Regression test (A4, 2026-05-20): an HTTP POST with binary body
     * (Content-Encoding: x-lzf / image upload / any application/octet-
     * stream payload) may contain embedded NUL bytes. The parser used
     * to strdup the body and store strlen() as the length, truncating
     * binary content at the first NUL. With the Content-Length-aware
     * parse, the full body should survive intact.
     *
     * 12-byte body: 'A' 'B' '\0' '\1' 'C' 'D' '\0' '\0' 'E' 'F' '\0' 'G'
     * — three embedded NULs at positions 2, 6, 7, 10. strlen() would
     * return 2; Content-Length is 12. Use http_parse_request_n so the
     * parser sees the authoritative buffer length and doesn't have to
     * rely on strlen (which would stop at the first body NUL).
     */
    static const char body_bytes[12] = {
        'A', 'B', '\0', '\1', 'C', 'D', '\0', '\0', 'E', 'F', '\0', 'G'
    };
    char raw[256];
    int header_len = snprintf(raw, sizeof(raw),
        "POST /upload HTTP/1.1\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 12\r\n"
        "\r\n");
    memcpy(raw + header_len, body_bytes, 12);
    raw[header_len + 12] = '\0';

    HttpRequest* req = http_parse_request_n(raw, (size_t)header_len + 12);
    ASSERT_NOT_NULL(req);
    ASSERT_EQ(12, (int)req->body_length);
    ASSERT_NOT_NULL(req->body);
    ASSERT_EQ(0, memcmp(req->body, body_bytes, 12));
    http_request_free(req);
}

TEST(http_server_post_with_oversized_content_length_clamps_safely) {
    /* Regression test (A4 follow-up, PR #532 ASan failure): if a client
     * sends `Content-Length: <larger-than-actual>`, the parser must not
     * memcpy past the end of the buffered request. Previously the new
     * Content-Length-aware code path trusted the header unconditionally,
     * which ASan caught on the pre-existing `http_server_post_with_body`
     * fixture (Content-Length: 25, body bytes: 19). Clamp body_cl against
     * the bytes actually available in the buffer.
     */
    const char* raw_request =
        "POST /api/users HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        "{\"name\":\"John Doe\"}";  /* 19 bytes — header lies */

    /* The wrapper uses strlen, which gives us the actual buffered byte
     * count (no embedded NULs in the body here). The parser must clamp
     * Content-Length=25 down to the 19 bytes that are actually present. */
    HttpRequest* req = http_parse_request(raw_request);
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("POST", req->method);
    /* body_length must equal the clamp target (19), not the lying 25. */
    ASSERT_EQ(19, (int)req->body_length);
    ASSERT_NOT_NULL(req->body);
    ASSERT_EQ(0, memcmp(req->body, "{\"name\":\"John Doe\"}", 19));
    http_request_free(req);
}

TEST(http_server_request_line_without_version_is_refused_once) {
    /* "GET /x" has a method and a target and no version. The parser released
     * the method it had copied without forgetting it, and
     * http_parse_request_n then freed it again with the rest of the failed
     * request: heap corruption from one request line any client can send
     * (found by tests/fuzz/http_head; ASan reports the double free, a plain
     * build on Windows died with 0xC0000374). */
    ASSERT_NULL(http_parse_request("GET /x\r\n\r\n"));
    ASSERT_NULL(http_parse_request("GET /x\r\nHost: a\r\n\r\n"));

    /* Parsed into a request the caller reuses, the failure leaves nothing
     * of itself behind for the caller's own free. */
    HttpRequest req;
    memset(&req, 0, sizeof req);
    ASSERT_NULL(http_parse_request_into(&req, "GET /x\r\n\r\n", 10));
    ASSERT_NULL(req.method);
    http_request_reset(&req);
    ASSERT_NOT_NULL(http_parse_request_into(&req, "GET /y HTTP/1.1\r\n\r\n", 19));
    ASSERT_STREQ("/y", req.path);
    http_request_reset(&req);
    free(req.header_keys);
    free(req.header_values);
}

/* The head check's answer for `head` followed by the blank line. */
static int head_check_of(const char* head, long* cl, int* chunked) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\r\n\r\n", head);
    const char* end = http_find_header_end(buf, (size_t)n);
    if (!end) return -1;
    return http_request_head_check(buf, end, cl, chunked);
}

TEST(http_request_head_check_refuses_a_bare_cr_or_lf) {
    /* The check split lines at each LF, the parser at each CR LF, and the
     * head ends at the first CR LF CR LF. With a bare LF the two read
     * different headers: here the check framed a 2-byte body while the
     * parser saw one header, Host, whose value held "\nContent-Length: 2".
     * A bare CR stayed in a value the proxy forwards, to an upstream that
     * may end the line there (RFC 9112 2.2; found by tests/fuzz/http_head).
     * Both are a head that is not a request, answered 400 like a NUL. */
    long cl = -1;
    int chunked = -1;
    ASSERT_EQ(HTTP_HEAD_MALFORMED, head_check_of("POST / HTTP/1.1\r\nHost: a\nContent-Length: 2", &cl, &chunked));
    ASSERT_EQ(HTTP_HEAD_MALFORMED, head_check_of("GET / HTTP/1.1\nHost: a", &cl, &chunked));
    ASSERT_EQ(HTTP_HEAD_MALFORMED, head_check_of("GET / HTTP/1.1\r\nX-A: a\rb", &cl, &chunked));
    ASSERT_EQ(HTTP_HEAD_MALFORMED, head_check_of("GET / HTTP/1.1\r\nX-A: a\r", &cl, &chunked));
    ASSERT_EQ(HTTP_HEAD_MALFORMED, head_check_of("\nGET / HTTP/1.1", &cl, &chunked));

    /* CR LF line ends, as always. */
    ASSERT_EQ(HTTP_HEAD_OK, head_check_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2", &cl, &chunked));
    ASSERT_EQ(2, (int)cl);
    ASSERT_EQ(0, chunked);
    ASSERT_EQ(HTTP_HEAD_OK, head_check_of("GET / HTTP/1.1", &cl, &chunked));
    ASSERT_EQ(0, (int)cl);
}

TEST(http_server_header_value_loses_the_whitespace_around_it) {
    /* field-line = field-name ":" OWS field-value OWS (RFC 9112 5), OWS a
     * space or a tab. The parser dropped leading spaces only, so these were
     * "\t tabbed", "trailing \t" and "\t" while the head check, reading the
     * same lines, had "tabbed", "trailing" and "" (found by
     * tests/fuzz/http_head). A space inside a value stays. */
    HttpRequest* req = http_parse_request(
        "GET / HTTP/1.1\r\nX-A:\t tabbed\r\nX-B: trailing \t\r\nX-C:\t\r\nX-D:  in side  \r\n\r\n");
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("tabbed", http_get_header(req, "X-A"));
    ASSERT_STREQ("trailing", http_get_header(req, "X-B"));
    ASSERT_STREQ("", http_get_header(req, "X-C"));
    ASSERT_STREQ("in side", http_get_header(req, "X-D"));
    http_request_free(req);
}

TEST(http_server_request_line_is_method_target_version) {
    /* method SP request-target SP HTTP-version (RFC 9112 3). The parser took
     * whatever lay between the spaces, so these were served: a method that
     * is not a token, an empty method, an empty target, and targets with a
     * tab or a CR, which the reverse proxy wrote into its own request line
     * upstream (found by tests/fuzz/http_head). */
    ASSERT_NULL(http_parse_request("G@T / HTTP/1.1\r\n\r\n"));
    ASSERT_NULL(http_parse_request(" / HTTP/1.1\r\n\r\n"));
    ASSERT_NULL(http_parse_request("GET  HTTP/1.1\r\n\r\n"));
    ASSERT_NULL(http_parse_request("GET /a\tb HTTP/1.1\r\n\r\n"));
    ASSERT_NULL(http_parse_request("GET /a\rb HTTP/1.1\r\n\r\n"));
    ASSERT_NULL(http_parse_request("GET /a\x7f HTTP/1.1\r\n\r\n"));

    /* Every token is a method, and a target may be any visible bytes. */
    HttpRequest* req = http_parse_request("M-SEARCH * HTTP/1.1\r\n\r\n");
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("M-SEARCH", req->method);
    ASSERT_STREQ("*", req->path);
    http_request_free(req);
    req = http_parse_request("GET /caf\xc3\xa9?q=%20 HTTP/1.0\r\n\r\n");
    ASSERT_NOT_NULL(req);
    ASSERT_STREQ("/caf\xc3\xa9", req->path);
    ASSERT_STREQ("q=%20", req->query_string);
    http_request_free(req);
}

TEST(http_server_body_without_content_length_keeps_nul_bytes) {
    /* The worker decodes a chunked upload in place and hands the parser the
     * head and the decoded payload, with no Content-Length, `len` counting
     * both. The parser took that body with strdup, so `a 00 b` arrived as
     * the one byte `a`, body_length 1, and a binary upload was cut at its
     * first NUL (found by tests/fuzz/http_head). It is every byte `len`
     * counts after the head. */
    static const char head[] =
        "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    char raw[128];
    size_t hl = sizeof head - 1;
    memcpy(raw, head, hl);
    memcpy(raw + hl, "a\0b", 3);
    raw[hl + 3] = '\0';
    HttpRequest* req = http_parse_request_n(raw, hl + 3);
    ASSERT_NOT_NULL(req);
    ASSERT_EQ(3, (int)req->body_length);
    ASSERT_EQ(0, memcmp(req->body, "a\0b", 3));
    http_request_free(req);

    /* Nothing after the head is no body, as before. */
    req = http_parse_request_n(raw, hl);
    ASSERT_NOT_NULL(req);
    ASSERT_EQ(0, (int)req->body_length);
    http_request_free(req);
}

/* Combined Log Format timestamps must be locale-independent.
 *
 * The month in CLF is the English three-letter abbreviation; it is a parseable
 * interchange format, not human-facing text. This previously used strftime's
 * %b, which emits the *locale's* month name — so a server embedded in a host
 * that had called setlocale(LC_ALL, "") wrote "08/Mär/2026" and broke every
 * downstream log parser. Regression guard for that (issue #1463, sibling of the
 * LC_NUMERIC float fix in #1459).
 *
 * The locale switch is best-effort: CI images generally ship only C/POSIX, in
 * which case the ambient-locale half is a no-op and the C-locale assertions
 * still run. That is deliberate — an unavailable locale must not fail the test,
 * but it must also not make it vacuous, hence the unconditional checks first. */
TEST(http_clf_time_is_locale_independent) {
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = 126;  /* 2026 */
    tmv.tm_mon  = 2;    /* March — the month that differs in de_DE ("Mär") */
    tmv.tm_mday = 8;
    tmv.tm_hour = 13;
    tmv.tm_min  = 5;
    tmv.tm_sec  = 7;

    char ts[64];
    http_format_clf_time(ts, sizeof ts, &tmv);
    ASSERT_STREQ("08/Mar/2026:13:05:07 +0000", ts);

    /* Every month renders as its English abbreviation. */
    static const char* const expect[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int m = 0; m < 12; m++) {
        char buf[64];
        tmv.tm_mon = m;
        http_format_clf_time(buf, sizeof buf, &tmv);
        ASSERT_EQ(0, memcmp(buf + 3, expect[m], 3));
    }
    tmv.tm_mon = 2;

    /* Now the actual regression: under a comma-decimal / non-English locale the
     * output must be byte-identical. Skipped silently where the locale is not
     * installed (see comment above). */
    const char* had = setlocale(LC_ALL, NULL);
    char saved[128];
    snprintf(saved, sizeof saved, "%s", had ? had : "C");
    if (setlocale(LC_ALL, "de_DE.UTF-8") || setlocale(LC_ALL, "de_DE.utf8")) {
        char under_locale[64];
        http_format_clf_time(under_locale, sizeof under_locale, &tmv);
        ASSERT_STREQ("08/Mar/2026:13:05:07 +0000", under_locale);
        setlocale(LC_ALL, saved);
    }

    /* Defensive: an out-of-range tm_mon must not index off the table. */
    tmv.tm_mon = 99;
    http_format_clf_time(ts, sizeof ts, &tmv);
    ASSERT_EQ(0, memcmp(ts + 3, "Jan", 3));
    tmv.tm_mon = -1;
    http_format_clf_time(ts, sizeof ts, &tmv);
    ASSERT_EQ(0, memcmp(ts + 3, "Jan", 3));
}


/* A wildcard route and the middleware chain: the only two behaviours the
 * since-deleted test_http_server_impl.c covered that this file did not. */
static int mw_probe_calls = 0;
static int mw_probe(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)req; (void)res; (void)user_data;
    mw_probe_calls++;
    return 1;
}

TEST(http_route_wildcard_match) {
    HttpRequest req = {0};
    req.method = "GET";
    req.path = "/static/css/main.css";
    req.param_count = 0;

    ASSERT_EQ(1, http_route_matches("/static/*", "/static/css/main.css", &req));
    ASSERT_EQ(1, http_route_matches("/static/*", "/static/x", &req));
    ASSERT_EQ(0, http_route_matches("/static/*", "/assets/main.css", &req));
    /* The wildcard covers a remainder, not an absent one: `/static/` has
     * nothing left to match and falls through to the next route. */
    ASSERT_EQ(0, http_route_matches("/static/*", "/static/", &req));

    /* The matcher allocates the param arrays on the request even when the
     * pattern captures nothing; the caller owns them. */
    free(req.param_keys);
    free(req.param_values);
}

TEST(http_server_middleware_chain) {
    HttpServer* server = http_server_create(8080);
    ASSERT_NOT_NULL(server);

    http_server_use_middleware(server, mw_probe, NULL);
    ASSERT_NOT_NULL(server->middleware_chain);
    ASSERT_TRUE(server->middleware_chain->middleware == mw_probe);
    ASSERT_TRUE(server->middleware_chain->next == NULL);

    /* A second one chains rather than replacing the first. */
    http_server_use_middleware(server, mw_probe, NULL);
    ASSERT_NOT_NULL(server->middleware_chain->next);

    http_server_free(server);
}

static int bearer_accepts_good(const char* token, void* ud) {
    (void)ud;
    return strcmp(token, "good") == 0;
}

/* The challenge a request's Authorization value gets from bearer_auth. */
static const char* bearer_challenge_for(const char* raw, char* out, size_t cap) {
    AetherBearerAuthOpts* o = aether_bearer_auth_opts_new("api", bearer_accepts_good, NULL);
    HttpRequest* req = http_parse_request(raw);
    HttpServerResponse* res = http_response_create();
    out[0] = '\0';
    if (o && req && res && aether_middleware_bearer_auth(req, res, o) == 0) {
        for (int i = 0; i < res->header_count; i++)
            if (strcasecmp(res->header_keys[i], "WWW-Authenticate") == 0)
                snprintf(out, cap, "%s", res->header_values[i]);
    } else {
        snprintf(out, cap, "passed");
    }
    http_server_response_free(res);
    http_request_free(req);
    aether_bearer_auth_opts_free(o);
    return out;
}

TEST(http_bearer_auth_reads_a_scheme_with_no_token) {
    /* "Authorization: Bearer " is the scheme with no token, a malformed
     * credential answered with error="invalid_token". The parser now trims
     * the space after a value (RFC 9112 5), so the middleware sees "Bearer"
     * and took it for no credential at all. */
    char got[256];
    ASSERT_STREQ("Bearer realm=\"api\", error=\"invalid_token\"",
                 bearer_challenge_for("GET / HTTP/1.1\r\nAuthorization: Bearer \r\n\r\n", got, sizeof got));
    ASSERT_STREQ("Bearer realm=\"api\", error=\"invalid_token\"",
                 bearer_challenge_for("GET / HTTP/1.1\r\nAuthorization: bearer\r\n\r\n", got, sizeof got));
    ASSERT_STREQ("Bearer realm=\"api\", error=\"invalid_token\"",
                 bearer_challenge_for("GET / HTTP/1.1\r\nAuthorization: Bearer bad\r\n\r\n", got, sizeof got));
    ASSERT_STREQ("Bearer realm=\"api\"",
                 bearer_challenge_for("GET / HTTP/1.1\r\n\r\n", got, sizeof got));
    ASSERT_STREQ("Bearer realm=\"api\"",
                 bearer_challenge_for("GET / HTTP/1.1\r\nAuthorization: Bearerx\r\n\r\n", got, sizeof got));
    ASSERT_STREQ("passed",
                 bearer_challenge_for("GET / HTTP/1.1\r\nAuthorization: Bearer good \r\n\r\n", got, sizeof got));
}

TEST(http_response_serialize_long_status_text) {
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);

    /* Sizing the status line from a fixed headroom truncated a status text
     * longer than the guess, and the response went out with a cut status
     * line and no headers at all. */
    const char* long_text =
        "Unprocessable Entity With A Deliberately Very Long Reason Phrase Indeed";
    free(resp->status_text);
    resp->status_text = strdup(long_text);
    ASSERT_NOT_NULL(resp->status_text);
    resp->status_code = 422;
    http_response_set_header(resp, "X-Probe", "kept");
    http_response_set_body(resp, "body-here");

    size_t len = 0;
    char* raw = http_response_serialize_len(resp, &len);
    ASSERT_NOT_NULL(raw);
    ASSERT_TRUE(strstr(raw, long_text) != NULL);
    ASSERT_TRUE(strstr(raw, "X-Probe: kept") != NULL);
    ASSERT_TRUE(strstr(raw, "body-here") != NULL);
    ASSERT_EQ((int)strlen(raw), (int)len);

    free(raw);
    http_server_response_free(resp);
}

TEST(http_response_serialize_into_reuses_buffer) {
    HttpServerResponse* resp = http_response_create();
    ASSERT_NOT_NULL(resp);
    http_response_set_status(resp, 200);
    http_response_set_header(resp, "Content-Type", "text/plain");
    http_response_set_body(resp, "0123456789012345678901234567890123456789");

    char*  buf = NULL;
    size_t cap = 0, len = 0;

    ASSERT_NOT_NULL(http_response_serialize_into(resp, &buf, &cap, &len));
    char*  first_buf = buf;
    size_t first_cap = cap;
    ASSERT_TRUE(first_cap > 0);
    ASSERT_TRUE(strstr(buf, "Content-Type: text/plain") != NULL);
    ASSERT_EQ((int)strlen(buf), (int)len);

    /* A smaller response reuses the same allocation rather than taking a new
     * one: this is the whole point of the entry point. */
    http_response_set_body(resp, "hi");
    ASSERT_NOT_NULL(http_response_serialize_into(resp, &buf, &cap, &len));
    ASSERT_TRUE(buf == first_buf);
    ASSERT_EQ((int)first_cap, (int)cap);
    ASSERT_EQ((int)strlen(buf), (int)len);
    ASSERT_TRUE(strstr(buf, "\r\n\r\nhi") != NULL);

    /* A bigger one grows it, and the content is still right. */
    char big[4096];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    http_response_set_body(resp, big);
    ASSERT_NOT_NULL(http_response_serialize_into(resp, &buf, &cap, &len));
    ASSERT_TRUE(cap > first_cap);
    ASSERT_EQ((int)strlen(buf), (int)len);
    ASSERT_TRUE(strstr(buf, big) != NULL);

    free(buf);
    http_server_response_free(resp);
}

#if !defined(_WIN32)
/* Parsing a request allocated about two dozen strings and freed them a moment
 * later: the method, path, version and a pair per header. With an arena they
 * come from one block and are released by resetting an offset.
 *
 * The rule the reset depends on is that a parse takes ALL of its strings from
 * the arena or none of them, because their origin is one flag. These cases
 * cover both sides of that decision and the reuse across it.
 */
TEST(http_request_parse_arena) {
    HttpArena arena;
    ASSERT_EQ(0, http_arena_init(&arena, 16384));

    HttpRequest req;
    memset(&req, 0, sizeof(req));

    const char* simple =
        "GET /a/b?x=1 HTTP/1.1\r\nHost: h\r\nAccept: */*\r\n\r\n";
    ASSERT_NOT_NULL(http_parse_request_arena(&req, simple, strlen(simple), &arena));
    ASSERT_EQ(1, req.arena_backed);
    ASSERT_STREQ("GET", req.method);
    ASSERT_STREQ("/a/b", req.path);
    ASSERT_STREQ("x=1", req.query_string);
    ASSERT_EQ(2, req.header_count);

    /* A header-heavy request: the reservation must cover a terminator and the
     * alignment rounding for every one of them, or the parse runs out of
     * arena partway and the request is refused. */
    char big[8192];
    int off = snprintf(big, sizeof(big), "POST /p HTTP/1.1\r\n");
    for (int i = 0; i < 40 && off < (int)sizeof(big) - 64; i++)
        off += snprintf(big + off, sizeof(big) - off, "X-H%02d: v%02d\r\n", i, i);
    snprintf(big + off, sizeof(big) - off, "\r\n");

    http_arena_reset(&arena);
    ASSERT_NOT_NULL(http_parse_request_arena(&req, big, strlen(big), &arena));
    ASSERT_EQ(1, req.arena_backed);
    ASSERT_EQ(40, req.header_count);
    ASSERT_STREQ("v39", req.header_values[39]);

    /* Whenever the parse decides to take the arena, the arena must hold
     * everything it then asks for: req_alloc has no fallback, so a
     * reservation that is too small fails the parse and drops the request.
     *
     * Swept across arena sizes rather than aimed at one, because the size
     * where a short reservation starts to bite depends on how many strings
     * the request has and how much each wastes on alignment. Headers are
     * padded to the maximum waste (a length just past the alignment) to make
     * the arena's true need as large as the request allows. */
    {
        char pad[8192];
        int  po = snprintf(pad, sizeof(pad), "GET /p HTTP/1.1\r\n");
        for (int i = 0; i < 50 && po < (int)sizeof(pad) - 64; i++)
            po += snprintf(pad + po, sizeof(pad) - po,
                           "AAAAAAAA%d: bbbbbbbbb\r\n", i % 10);
        snprintf(pad + po, sizeof(pad) - po, "\r\n");
        size_t plen = strlen(pad);

        for (size_t cap = 64; cap <= 4096; cap += 8) {
            HttpArena sweep;
            if (http_arena_init(&sweep, cap) != 0) continue;
            http_request_reset(&req);
            HttpRequest* got = http_parse_request_arena(&req, pad, plen, &sweep);
            /* Whichever way it decided, it has to have produced the request. */
            ASSERT_NOT_NULL(got);
            ASSERT_EQ(50, req.header_count);
            ASSERT_STREQ("bbbbbbbbb", req.header_values[49]);
            http_request_reset(&req);
            http_arena_free(&sweep);
        }
    }

    /* An arena too small for the request must not be used at all: a parse
     * that took some strings from it and the rest from malloc could not be
     * freed correctly afterwards. */
    HttpArena tiny;
    ASSERT_EQ(0, http_arena_init(&tiny, 64));
    http_request_reset(&req);
    ASSERT_NOT_NULL(http_parse_request_arena(&req, big, strlen(big), &tiny));
    ASSERT_EQ(0, req.arena_backed);
    ASSERT_EQ(40, req.header_count);
    ASSERT_STREQ("v39", req.header_values[39]);

    /* Reuse back onto the arena, then off it again: each reset frees under
     * the rule the parse that filled it actually used. */
    http_arena_reset(&arena);
    ASSERT_NOT_NULL(http_parse_request_arena(&req, simple, strlen(simple), &arena));
    ASSERT_EQ(1, req.arena_backed);
    ASSERT_NOT_NULL(http_parse_request_into(&req, simple, strlen(simple)));
    ASSERT_EQ(0, req.arena_backed);
    ASSERT_STREQ("GET", req.method);

    http_request_reset(&req);
    free(req.header_keys);
    free(req.header_values);
    http_arena_free(&tiny);
    http_arena_free(&arena);
}

/* use_real_ip replaces the X-Real-IP a request carries and use_rewrite its
 * path (#3234). Both used free() on the string they replaced, which for a
 * request parsed into an arena is a pointer the allocator never issued, and
 * left their malloc'd replacements to a reset that does not free an
 * arena-backed request's strings. */
TEST(http_request_edit_an_arena_backed_request) {
    HttpArena arena;
    ASSERT_EQ(0, http_arena_init(&arena, 16384));
    HttpRequest req;
    memset(&req, 0, sizeof(req));
    const char* raw =
        "GET /old/x HTTP/1.1\r\nHost: h\r\nX-Real-IP: 198.51.100.1\r\nAccept: */*\r\n\r\n";
    ASSERT_NOT_NULL(http_parse_request_arena(&req, raw, strlen(raw), &arena));
    ASSERT_EQ(1, req.arena_backed);
    req.remote_addr = strdup("127.0.0.1");

    AetherRealIpOpts* real_ip = aether_real_ip_opts_new("", "10.0.0.0/8");
    ASSERT_NOT_NULL(real_ip);
    ASSERT_EQ(1, aether_middleware_real_ip(&req, NULL, real_ip));
    ASSERT_STREQ("127.0.0.1", http_get_header(&req, "X-Real-IP"));

    AetherRewriteOpts* rewrite = aether_rewrite_opts_new();
    ASSERT_NOT_NULL(rewrite);
    ASSERT_EQ(0, aether_rewrite_add_rule(rewrite, "/old/", "/new/"));
    ASSERT_EQ(1, aether_middleware_rewrite(&req, NULL, rewrite));
    ASSERT_STREQ("/new/x", req.path);
    ASSERT_STREQ("GET", req.method);
    ASSERT_STREQ("h", http_get_header(&req, "Host"));
    ASSERT_STREQ("*/*", http_get_header(&req, "Accept"));

    /* The arena can be reset now: nothing the request holds is in it. */
    http_arena_reset(&arena);
    ASSERT_STREQ("/new/x", req.path);
    ASSERT_STREQ("127.0.0.1", http_get_header(&req, "X-Real-IP"));

    http_request_reset(&req);
    free(req.header_keys);
    free(req.header_values);
    free(req.remote_addr);
    aether_rewrite_opts_free(rewrite);
    aether_real_ip_opts_free(real_ip);
    http_arena_free(&arena);
}
#endif  /* !_WIN32 */

#if !defined(_WIN32)
/* A retry's backoff is the exchange's caller's to wait out (#3232). The
 * exchange slept it inside resume, which on the proxy event loop put the
 * driver to sleep with every connection it held. Four retries with a 10 s
 * backoff base, each an upstream that could not be reached: resume has to
 * hand each one back at once, with the wait in backoff_ms. */
TEST(http_proxy_exchange_leaves_the_backoff_to_its_caller) {
    AetherProxyPool* pool = aether_proxy_pool_new(AETHER_PROXY_LB_ROUND_ROBIN, 5, 0, 0);
    ASSERT_NOT_NULL(pool);
    ASSERT_STREQ("", aether_proxy_upstream_add(pool, "http://127.0.0.1:9", 1));
    AetherProxyOpts* opts = aether_proxy_opts_new();
    ASSERT_NOT_NULL(opts);
    opts->pool = pool;   /* the opts free the pool with themselves */
    ASSERT_STREQ("", aether_proxy_opts_set_retry_policy(opts, 4, 10000));

    HttpRequest* req = http_parse_request("GET /x HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_NOT_NULL(req);
    HttpServerResponse* res = http_response_create();
    AetherProxyExchange px;
    int r = aether_proxy_exchange_begin(&px, req, res, opts, NULL);
    ASSERT_EQ(PX_NEED_SEND, r);

    int64_t started = aether_proxy_now_ms();
    int retries = 0;
    while (r == PX_NEED_SEND) {
        http_request_free_raw(px.outbound);
        free(px.upstream_url);
        px.outbound = NULL;
        px.upstream_url = NULL;
        px.resp = http_response_alloc_failed("connection failed");
        r = aether_proxy_exchange_resume(&px, 0);
        if (r == PX_NEED_SEND) retries++;
    }
    int64_t took = aether_proxy_now_ms() - started;
    ASSERT_EQ(4, retries);
    ASSERT_TRUE(took < 1000);
    ASSERT_EQ(502, http_response_get_status(res));

    http_server_response_free(res);
    http_request_free(req);
    aether_proxy_opts_free(opts);
}
#endif

/* The rate limiter keeps a bucket per client address, and addresses are the
 * clients' to have: IPv6 hands one client more than any table holds. Kept
 * without a bound, a client cycling addresses grew the table for good under
 * the lock every request takes (#3266). It holds at most RATE_MAX_KEYS
 * (16384), the least recently seen going first, and still limits the clients
 * it holds. */
static int rate_limit_from(AetherRateLimitOpts* o, const char* ip) {
    HttpRequest* req = http_parse_request("GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    if (!req) return -1;
    req->remote_addr = strdup(ip);
    HttpServerResponse* res = http_response_create();
    int pass = aether_middleware_rate_limit(req, res, o);
    http_server_response_free(res);
    http_request_free(req);
    return pass;
}

TEST(http_rate_limit_holds_a_bounded_set_of_clients) {
    AetherRateLimitOpts* o = aether_rate_limit_opts_new(5, 60000);
    ASSERT_NOT_NULL(o);
    char ip[64];
    for (int i = 0; i < 20000; i++) {
        snprintf(ip, sizeof(ip), "10.%d.%d.%d", (i >> 16) & 255, (i >> 8) & 255, i & 255);
        ASSERT_EQ(1, rate_limit_from(o, ip));
    }
    ASSERT_TRUE(aether_rate_limit_key_count(o) <= 16384);
    /* One client past its five requests is still limited. */
    int passed = 0;
    for (int i = 0; i < 8; i++) passed += rate_limit_from(o, "192.0.2.1") == 1;
    ASSERT_EQ(5, passed);
    aether_rate_limit_opts_free(o);
}

/* X-Real-IP is something a client can send, so use_real_ip leaves none of a
 * client's on the request (#3169). It replaced the first and kept a second,
 * which reached an upstream that reads the last X-Real-IP (#3239). */
TEST(http_real_ip_drops_every_x_real_ip_a_client_sent) {
    const char* raw =
        "GET / HTTP/1.1\r\nX-Real-IP: 1.1.1.1\r\nHost: h\r\nx-real-ip: 2.2.2.2\r\n\r\n";
    HttpRequest* req = http_parse_request(raw);
    ASSERT_NOT_NULL(req);
    req->remote_addr = strdup("127.0.0.1");
    AetherRealIpOpts* real_ip = aether_real_ip_opts_new("", "10.0.0.0/8");
    ASSERT_NOT_NULL(real_ip);
    ASSERT_EQ(1, aether_middleware_real_ip(req, NULL, real_ip));
    int seen = 0;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->header_keys[i], "X-Real-IP") != 0) continue;
        seen++;
        ASSERT_STREQ("127.0.0.1", req->header_values[i]);
    }
    ASSERT_EQ(1, seen);
    ASSERT_STREQ("h", http_get_header(req, "Host"));
    aether_real_ip_opts_free(real_ip);
    http_request_free(req);
}

/* A request object parsed into again keeps its header arrays, which hold
 * HTTP_MAX_HEADERS (#3234). use_real_ip, adding the X-Real-IP a request did
 * not carry, grew them to one past the headers they held, which shrank them,
 * and the next parse into the same object wrote 50 headers into 4 slots. */
TEST(http_request_edit_keeps_room_for_a_reparse) {
    HttpRequest req;
    memset(&req, 0, sizeof(req));
    const char* small = "GET /a HTTP/1.1\r\nHost: h\r\nAccept: */*\r\nX-One: 1\r\n\r\n";
    ASSERT_NOT_NULL(http_parse_request_into(&req, small, strlen(small)));
    req.remote_addr = strdup("127.0.0.1");

    AetherRealIpOpts* real_ip = aether_real_ip_opts_new("", "10.0.0.0/8");
    ASSERT_NOT_NULL(real_ip);
    ASSERT_EQ(1, aether_middleware_real_ip(&req, NULL, real_ip));
    ASSERT_EQ(4, req.header_count);
    ASSERT_STREQ("127.0.0.1", http_get_header(&req, "X-Real-IP"));

    char big[8192];
    int off = snprintf(big, sizeof(big), "GET /b HTTP/1.1\r\n");
    for (int i = 0; i < 50; i++)
        off += snprintf(big + off, sizeof(big) - off, "X-H%02d: value-%02d\r\n", i, i);
    snprintf(big + off, sizeof(big) - off, "\r\n");
    for (int round = 0; round < 3; round++) {
        ASSERT_NOT_NULL(http_parse_request_into(&req, big, strlen(big)));
        ASSERT_EQ(50, req.header_count);
        ASSERT_STREQ("value-00", http_get_header(&req, "X-H00"));
        ASSERT_STREQ("value-49", http_get_header(&req, "X-H49"));
        ASSERT_EQ(1, aether_middleware_real_ip(&req, NULL, real_ip));
        ASSERT_EQ(51, req.header_count);
    }

    http_request_reset(&req);
    free(req.header_keys);
    free(req.header_values);
    free(req.remote_addr);
    aether_real_ip_opts_free(real_ip);
}

/* A cached response lives as long as its TTL, however long that is. The
 * expiry was stored_at_ms + (long)ttl_sec * 1000L: on Windows, where a `long`
 * is 32 bits, a TTL past 24.8 days overflowed, the entry expired before it
 * was stored, and every lookup missed. A 30-day default TTL is stored here
 * and must be found, with its full 30 days ahead of it. */
TEST(http_proxy_cache_keeps_a_long_ttl) {
    AetherProxyCache* cache = aether_proxy_cache_new(4, 65536, 30 * 24 * 3600,
                                                     AETHER_PROXY_CACHE_KEY_METHOD_URL);
    ASSERT_NOT_NULL(cache);
    HttpRequest* req = http_parse_request("GET /long HTTP/1.1\r\n\r\n");
    ASSERT_NOT_NULL(req);
    aether_proxy_cache_store(cache, "GET", "/long", req, 200,
                             "Content-Type: text/plain\r\n", "hello", 5);
    AetherProxyCacheEntry* e = aether_proxy_cache_lookup(cache, "GET", "/long", req);
    int found = e != NULL;
    long long ttl_ms = e ? (long long)(e->expires_at_ms - e->stored_at_ms) : -1;
    http_request_free(req);
    aether_proxy_cache_free(cache);
    ASSERT_TRUE(found);
    ASSERT_TRUE(ttl_ms == 30LL * 24 * 3600 * 1000);
}

#if !defined(_WIN32)
/* #3108: the server's sends went out as one send(2) each, which can take
 * fewer bytes than asked: a signal or a send timeout cuts it short once part
 * of the buffer is out. A response written that way reached the client
 * truncated. Here the server's end of a socketpair has a 4 KiB buffer and a
 * 500 ms send timeout, and the client pauses 750 ms after 64 KiB of a
 * 256 KiB response; the rest goes when it reads on. */
enum { BIG_BODY = 256 * 1024 };

static void big_body_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req;
    const char* body = (const char*)ud;
    http_response_set_status(res, 200);
    http_response_set_body_n(res, body, BIG_BODY);
}

typedef struct {
    int fd;
    char* buf;
    int cap;
    int got;
} PausingReader;

static void* pausing_reader(void* arg) {
    PausingReader* r = (PausingReader*)arg;
    int paused = 0;
    while (r->got < r->cap) {
        int n = (int)recv(r->fd, r->buf + r->got, (size_t)(r->cap - r->got), 0);
        if (n <= 0) break;
        r->got += n;
        if (!paused && r->got >= 64 * 1024) {
            paused = 1;
            usleep(750000);
        }
    }
    return NULL;
}

TEST_CATEGORY(http_server_sends_a_whole_response_through_short_sends, TEST_CATEGORY_NETWORK) {
    char* body = (char*)malloc(BIG_BODY);
    ASSERT_NOT_NULL(body);
    for (int i = 0; i < BIG_BODY; i++) body[i] = (char)('a' + i % 26);

    HttpServer* server = http_server_create(0);
    ASSERT_NOT_NULL(server);
    http_server_get(server, "/big", big_body_handler, body);
    http_server_set_keepalive_raw(server, 0, 0, 0);

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    int small = 4096;
    struct timeval tv = { 0, 500000 };
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    setsockopt(fds[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const char* req = "GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((int)strlen(req), (int)send(fds[1], req, strlen(req), 0));

    PausingReader r = { fds[1], (char*)malloc(BIG_BODY + 4096), BIG_BODY + 4096, 0 };
    ASSERT_NOT_NULL(r.buf);
    pthread_t reader;
    ASSERT_EQ(0, pthread_create(&reader, NULL, pausing_reader, &r));
    http_server_drain_connection(server, fds[0]);   /* closes fds[0] */
    pthread_join(reader, NULL);

    char* head_end = NULL;
    for (int i = 0; i + 4 <= r.got; i++) {
        if (memcmp(r.buf + i, "\r\n\r\n", 4) == 0) { head_end = r.buf + i + 4; break; }
    }
    ASSERT_NOT_NULL(head_end);
    int body_got = r.got - (int)(head_end - r.buf);
    printf("  body received: %d of %d bytes\n", body_got, BIG_BODY);
    ASSERT_EQ(BIG_BODY, body_got);
    ASSERT_TRUE(memcmp(head_end, body, BIG_BODY) == 0);

    close(fds[1]);
    free(r.buf);
    http_server_free(server);
    free(body);
}
#endif
