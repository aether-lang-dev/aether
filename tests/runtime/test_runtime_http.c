#include "test_harness.h"
#include "../../std/net/aether_http.h"
#include "../../std/string/aether_string.h"
/* The connect-completion contract below is exercised only where the driver
 * that depends on it is built. That driver needs a poller and a pipe, so it
 * is POSIX-only, and on Windows http_upstream_connected has no caller. */
#if !defined(_WIN32)
#include "../../std/net/aether_http_internal.h"
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
