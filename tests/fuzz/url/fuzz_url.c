/* Coverage-guided fuzz target for the HTTP client's URL readers in
 * std/net/aether_http.c: parse_url (a request URL into host, port, target and
 * scheme), http_parse_proxy_url, and http_resolve_location (a redirect's
 * Location resolved against the URL that was asked for).
 *
 * The input is a URL, then optionally a NUL and a Location. Each check is a
 * property, and a failure abort()s with what disagreed:
 *
 *   U1  parse_url gives the pieces a reading of its contract gives
 *       (aether_http_internal.h, RFC 3986 3.2): http:// is port 80,
 *       https:// is 443 and TLS, no scheme is http; the authority runs to
 *       the first '/', '?' or '#'; an IPv6 host is bracketed and comes back
 *       bare; a port is 1 to 5 digits naming 1..65535, read exactly; the
 *       target is the path and query, "/" first when the path is empty, the
 *       fragment dropped, never cut. A host holds no byte a request head
 *       cannot carry (a control byte or a space): it is written into the
 *       Host header and a proxy's request line as it is.
 *   U2  What parse_url accepts, written back as a URL from its pieces,
 *       parses to the same pieces.
 *   U3  http_parse_proxy_url gives a non-empty host and a port in 1..65535
 *       (3128 when none is named), the host parse_url gives.
 *   U4  A resolved Location is an absolute URI with a lower-case scheme that
 *       holds no control byte, and no space or byte past ASCII outside its
 *       authority (they are percent-encoded); resolved again against the
 *       same URL it is unchanged, where that URL's path has no dot segment
 *       (RFC 3986 5.2.2 keeps a base path as it is for an empty reference).
 *   U5  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether_http.h"
#include "aether_http_internal.h"

#define HOST_CAP 256

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_url: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

typedef struct {
    int  ok;
    char host[HOST_CAP];
    int  port;
    int  tls;
    char* path;    /* malloc'd */
} UrlModel;

static int head_safe(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static void model_url(const char* url, UrlModel* m) {
    memset(m, 0, sizeof *m);
    const char* s = url;
    m->port = 80;
    if (strncmp(url, "http://", 7) == 0) s = url + 7;
    else if (strncmp(url, "https://", 8) == 0) { s = url + 8; m->port = 443; m->tls = 1; }
    size_t alen = strcspn(s, "/?#");
    const char *h = s, *h_end, *port = NULL;
    size_t port_len = 0;
    if (alen > 0 && s[0] == '[') {
        const char* close = (const char*)memchr(s, ']', alen);
        if (!close) return;
        h = s + 1;
        h_end = close;
        if (close + 1 < s + alen) {
            if (close[1] != ':') return;
            port = close + 2;
        }
    } else {
        const char* colon = (const char*)memchr(s, ':', alen);
        h_end = colon ? colon : s + alen;
        if (colon) port = colon + 1;
    }
    if (port) port_len = (size_t)(s + alen - port);
    size_t hlen = (size_t)(h_end - h);
    if (hlen >= HOST_CAP || !head_safe(h, hlen)) return;
    if (port) {
        if (port_len == 0 || port_len > 5) return;
        long v = 0;
        for (size_t i = 0; i < port_len; i++) {
            if (port[i] < '0' || port[i] > '9') return;
            v = v * 10 + (port[i] - '0');
        }
        if (v < 1 || v > 65535) return;
        m->port = (int)v;
    }
    memcpy(m->host, h, hlen);
    m->host[hlen] = '\0';
    const char* rest = s + alen;
    size_t n = strcspn(rest, "#");
    int lead = rest[0] != '/';
    m->path = (char*)malloc(n + 2);
    CHECK(m->path != NULL, "out of memory");
    if (lead) m->path[0] = '/';
    memcpy(m->path + lead, rest, n);
    m->path[lead + n] = '\0';
    m->ok = 1;
}

static void check_parse_url(const char* url) {
    UrlModel m;
    model_url(url, &m);
    char host[HOST_CAP];
    int port = -1, tls = -1;
    char* path = NULL;
    int ok = parse_url_alloc(url, host, sizeof host, &port, &path, &tls);
    CHECK(ok == m.ok, "parse_url %s '%s', its contract %s it", ok ? "accepted" : "refused", url,
          m.ok ? "accepts" : "refuses");
    if (ok) {
        CHECK(strcmp(host, m.host) == 0, "host '%s', the URL names '%s'", host, m.host);
        CHECK(port == m.port, "port %d, the URL names %d", port, m.port);
        CHECK(tls == m.tls, "TLS %d for '%s'", tls, url);
        CHECK(path && strcmp(path, m.path) == 0, "target '%s', the URL's is '%s'", path, m.path);

        /* U2 */
        int v6 = strchr(host, ':') != NULL;
        size_t cap = strlen(host) + strlen(path) + 32;
        char* again = (char*)malloc(cap);
        CHECK(again != NULL, "out of memory");
        snprintf(again, cap, "%s://%s%s%s:%d%s", tls ? "https" : "http", v6 ? "[" : "", host,
                 v6 ? "]" : "", port, path);
        char host2[HOST_CAP];
        int port2 = -1, tls2 = -1;
        char* path2 = NULL;
        int ok2 = parse_url_alloc(again, host2, sizeof host2, &port2, &path2, &tls2);
        /* A host holding '[', ']', '/', '?' or '#' cannot be written back
         * into an authority; only those may fail to round-trip. */
        if (strpbrk(host, "[]/?#") == NULL) {
            CHECK(ok2, "'%s' written back as '%s' does not parse", url, again);
            CHECK(strcmp(host2, host) == 0 && port2 == port && tls2 == tls && strcmp(path2, path) == 0,
                  "'%s' written back as '%s' parses to %s %d %s", url, again, host2, port2, path2);
        }
        free(path2);
        free(again);
    }
    free(path);

    /* U3 */
    char phost[HOST_CAP];
    int pport = -1;
    if (http_parse_proxy_url(url, phost, sizeof phost, &pport) == 0) {
        CHECK(phost[0] != '\0', "a proxy URL with no host was accepted: '%s'", url);
        CHECK(pport >= 1 && pport <= 65535, "proxy port %d", pport);
        if (ok) CHECK(strcmp(phost, host) == 0, "proxy host '%s', parse_url's '%s'", phost, host);
    }
    free(m.path);
}

static void check_location(const char* base, const char* ref) {
    char* r = http_resolve_location(base, ref);
    if (!r) return;
    size_t n = strlen(r);
    size_t colon = strcspn(r, ":");
    CHECK(colon > 0 && colon < n, "'%s' against '%s' resolved to '%s', which has no scheme", ref, base, r);
    for (size_t i = 0; i < colon; i++)
        CHECK(!(r[i] >= 'A' && r[i] <= 'Z'), "the scheme of '%s' is not lower case", r);
    size_t auth_at = 0, auth_end = 0;
    if (r[colon + 1] == '/' && r[colon + 2] == '/') {
        auth_at = colon + 3;
        auth_end = auth_at + strcspn(r + auth_at, "/?#");
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)r[i];
        CHECK(c >= 0x20 && c != 0x7f, "'%s' against '%s' resolved to a URI holding byte 0x%02x", ref, base, c);
        if (i < auth_at || i >= auth_end)
            CHECK(c != ' ' && c < 0x80, "'%s' against '%s' resolved to a URI holding an unescaped 0x%02x", ref, base, c);
    }
    /* A reference with an empty path takes the base's path as it is, dot
     * segments and all (RFC 3986 5.2.2), and resolved again as an absolute
     * URI it loses them; so the fixed point holds where the base has none. */
    if (!strstr(base, "/.")) {
        char* again = http_resolve_location(base, r);
        CHECK(again && strcmp(again, r) == 0, "'%s' against '%s' resolved to '%s', and that to '%s'",
              ref, base, r, again ? again : "(refused)");
        free(again);
    }
    free(r);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* url = (char*)malloc(size + 1);
    CHECK(url != NULL, "out of memory");
    memcpy(url, data, size);
    url[size] = '\0';
    check_parse_url(url);
    size_t ulen = strlen(url);
    if (ulen < size) check_location(url, url + ulen + 1);
    free(url);
    return 0;
}
