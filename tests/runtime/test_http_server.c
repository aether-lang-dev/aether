#include "test_harness.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <time.h>
#include "../../std/net/aether_http_server.h"

// HTTP Server Tests - Real tests for implemented functionality

TEST(http_server_create) {
    HttpServer* server = http_server_create(8080);
    ASSERT_NOT_NULL(server);
    ASSERT_EQ(8080, server->port);
    ASSERT_EQ(0, server->is_running);
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
