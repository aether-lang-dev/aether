// The one grant matcher. Every place that compares a grant with a resource
// uses it: the generated in-process checker (compiler/codegen/codegen.c), the
// LD_PRELOAD library (runtime/libaether_sandbox_preload.c), and the
// contrib/host/<lang> bridges. They used to carry their own copies, which had
// already drifted (only some normalised IPv4-mapped IPv6 addresses).
//
// Header-only and dependency-free (string.h, stdlib.h), so generated C,
// the preload library and every bridge can include it in any build mode.
//
// Patterns: "*" matches anything, "/etc/*" a prefix, "*.example.com" a
// suffix, anything else exactly. For the "tcp" and "udp" categories a
// pattern may name a port, "db.internal:5432" or "[::1]:8080", and the
// resource is "host:port" (aether_net_resource); a pattern without a port
// matches any port, a pattern with one only that port. The host part
// follows the same glob rules, and "::ffff:a.b.c.d" is compared as a.b.c.d.
#ifndef AETHER_SANDBOX_MATCH_H
#define AETHER_SANDBOX_MATCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The glob comparison, with no port handling.
static inline int aether_grant_glob(const char* pat, const char* resource) {
    if (!pat || !resource) return 0;
    if (strncmp(pat, "::ffff:", 7) == 0) pat += 7;
    if (strncmp(resource, "::ffff:", 7) == 0) resource += 7;
    size_t plen = strlen(pat);
    size_t rlen = strlen(resource);
    if (plen == 1 && pat[0] == '*') return 1;
    if (plen > 1 && pat[plen - 1] == '*' && strncmp(pat, resource, plen - 1) == 0) return 1;
    if (plen > 1 && pat[0] == '*') {
        size_t slen = plen - 1;
        if (rlen >= slen && strcmp(resource + rlen - slen, pat + 1) == 0) return 1;
    }
    return strcmp(pat, resource) == 0;
}

// Split "host:port" / "[v6]:port" / "host" into host (copied into `host`,
// brackets removed) and port (-1 when absent). A bare IPv6 address ("::1",
// two or more colons, no brackets) has no port.
static inline int aether_split_host_port(const char* s, char* host, size_t cap) {
    host[0] = '\0';
    if (!s) return -1;
    const char* end = s + strlen(s);
    const char* colon = strrchr(s, ':');
    int port = -1;
    const char* hend = end;
    if (s[0] == '[') {
        const char* rb = strchr(s, ']');
        if (rb) {
            s++;
            hend = rb;
            if (rb[1] == ':' && rb[2]) port = atoi(rb + 2);
        }
    } else if (colon && strchr(s, ':') == colon && colon[1]) {
        const char* d = colon + 1;
        int digits = 1;
        for (; *d; d++) if (*d < '0' || *d > '9') { digits = 0; break; }
        if (digits) { hend = colon; port = atoi(colon + 1); }
    }
    size_t n = (size_t)(hend - s);
    if (n >= cap) n = cap - 1;
    memcpy(host, s, n);
    host[n] = '\0';
    return port;
}

// Does a grant pattern in `category` cover `resource`?
static inline int aether_grant_match(const char* category, const char* pat, const char* resource) {
    if (!pat || !resource) return 0;
    if (category && (strcmp(category, "tcp") == 0 || strcmp(category, "udp") == 0)) {
        char ph[256], rh[256];
        int pport = aether_split_host_port(pat, ph, sizeof ph);
        int rport = aether_split_host_port(resource, rh, sizeof rh);
        if (pport >= 0 && pport != rport) return 0;
        return aether_grant_glob(ph, rh);
    }
    return aether_grant_glob(pat, resource);
}

// The resource a tcp/udp check names: "host:port", with an IPv6 address in
// brackets so its own colons cannot be read as the port's.
static inline const char* aether_net_resource(char* buf, size_t cap, const char* host, int port) {
    if (!host) host = "";
    if (strchr(host, ':') && host[0] != '[') snprintf(buf, cap, "[%s]:%d", host, port);
    else snprintf(buf, cap, "%s:%d", host, port);
    return buf;
}

#endif // AETHER_SANDBOX_MATCH_H
