#include "aether_http.h"
#include "aether_http_internal.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_thread.h"
#include "../../runtime/utils/aether_compiler.h"

/* The helpers below do no networking and other modules link against
 * them (the proxy times its upstreams with the clock and validates the
 * header names it forwards), so they are built whether networking is or
 * not (#2517). */

/* A millisecond clock pinned for one pass of an event loop.
 *
 * A driver reads the clock several times per request: arming a deadline,
 * computing the next timeout, expiring idle pooled connections, recording when
 * an upstream was picked. Each read is a counter the kernel serialises, and on
 * a single core arch_counter_get_cntvct is 4.9% of this path's profile -- the
 * largest entry that is not TCP receive processing.
 *
 * Within one pass those readers do not need different answers: the pass takes
 * microseconds and every deadline they compare against is milliseconds or
 * seconds away. So the driver pins one value for the pass and they share it.
 *
 * Defined here rather than in a header because a static thread-local in a
 * header is a separate variable per translation unit, and the thread that pins
 * it is not in the file that reads it. A thread that never pins reads the real
 * clock, which is every thread but a driver. */
AETHER_TLS_SHARED uint64_t aether_http_pinned_ms = 0;

void http_clock_pin(void) {
    uint64_t ms = aether_now_ns() / 1000000ULL;
    /* Zero means "not pinned", so a clock reading exactly zero pins a
     * millisecond later rather than not at all. */
    aether_http_pinned_ms = ms ? ms : 1;
}

void http_clock_unpin(void) { aether_http_pinned_ms = 0; }

uint64_t http_clock_ms(void) {
    return aether_http_pinned_ms ? aether_http_pinned_ms
                                 : (aether_now_ns() / 1000000ULL);
}

/* A header name has to be a token, and a value has to be free of the bytes
 * that end a line.
 *
 * Without this, a value carrying CR LF is written into the request head
 * verbatim and becomes additional headers: a caller that builds a value out
 * of anything a user supplied hands that user the rest of the request, and a
 * doubled CR LF ends the head entirely and starts a second request the peer
 * will answer (CWE-93). The header is rejected rather than repaired, because
 * silently sending something other than what was asked for is its own bug.
 */
/* RFC 9110 token characters: letters, digits, and "!#$%&'*+-.^_`|~".
 *
 * A table, because the proxy validates every character of every header name it
 * forwards, and the readable spelling of this test costs an isalnum call plus
 * a strchr across sixteen punctuation marks per character. Fixing the set here
 * also pins it to the grammar: isalnum answers for the active locale, and a
 * header name is a token in every locale. */
static const unsigned char http_tchar[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

int http_header_name_ok(const char* name) {
    if (!name || !*name) return 0;
    for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
        if (!http_tchar[*c]) return 0;
    }
    return 1;
}

int http_header_value_ok(const char* value) {
    if (!value) return 0;
    for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
        if (*c == '\r' || *c == '\n') return 0;
    }
    return 1;
}

#if !AETHER_HAS_NETWORKING
HttpResponse* http_get_raw(const char* u) { (void)u; return NULL; }
HttpResponse* http_get_with_timeout_ns_raw(const char* u, int64_t ns) { (void)u; (void)ns; return NULL; }
HttpResponse* http_post_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_put_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_delete_raw(const char* u) { (void)u; return NULL; }
void http_response_free(HttpResponse* r) { (void)r; }
int http_response_status(HttpResponse* r) { (void)r; return 0; }
const char* http_response_body(HttpResponse* r) { (void)r; return (const char*)string_empty(); }
int http_response_body_length(HttpResponse* r) { (void)r; return 0; }
const char* http_response_headers(HttpResponse* r) { (void)r; return ""; }
const char* http_response_error(HttpResponse* r) { (void)r; return "networking disabled at build time"; }
int http_response_ok(HttpResponse* r) { (void)r; return 0; }
struct HttpClientRequest { int unused; };
HttpClientRequest* http_request_raw(const char* m, const char* u) { (void)m; (void)u; return NULL; }
/* No arena to bump-allocate from in a build without networking, so this is
 * the plain constructor, which is itself a stub here. */
HttpClientRequest* http_request_raw_arena(const char* m, const char* u, struct HttpArena* a) { (void)a; return http_request_raw(m, u); }
int http_request_set_header_raw(HttpClientRequest* r, const char* n, const char* v) { (void)r; (void)n; (void)v; return -1; }
int http_request_set_body_raw(HttpClientRequest* r, const char* b, int l, const char* c) { (void)r; (void)b; (void)l; (void)c; return -1; }
int http_request_set_timeout_raw(HttpClientRequest* r, int s) { (void)r; (void)s; return -1; }
int http_request_set_timeout_ns_raw(HttpClientRequest* r, int64_t ns) { (void)r; (void)ns; return -1; }
int http_request_set_follow_redirects_raw(HttpClientRequest* r, int n) { (void)r; (void)n; return -1; }
int http_request_set_insecure_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_request_set_cafile_raw(HttpClientRequest* r, const char* p) { (void)r; (void)p; return -1; }
int http_request_use_env_proxy_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_request_use_http_proxy_raw(HttpClientRequest* r, const char* u) { (void)r; (void)u; return -1; }
int http_request_ignore_http_proxy_raw(HttpClientRequest* r) { (void)r; return -1; }
void http_request_free_raw(HttpClientRequest* r) { (void)r; }
HttpResponse* http_send_raw(HttpClientRequest* r) { (void)r; return NULL; }
const char* http_response_header_raw(HttpResponse* r, const char* n) { (void)r; (void)n; return ""; }
const char* http_response_effective_url_raw(HttpResponse* r) { (void)r; return ""; }
const char* http_response_redirect_error_raw(HttpResponse* r) { (void)r; return ""; }
int http_request_set_stream_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_response_is_stream_raw(HttpResponse* r) { (void)r; return 0; }
const char* http_response_read_chunk_raw(HttpResponse* r, int max) { (void)r; (void)max; return (const char*)string_empty(); }
/* Linked by the proxy, which is built without networking too (#2517). */
const char* http_response_body_str(HttpResponse* r) { (void)r; return ""; }
void http_client_pool_size_for_proxy(void) {}
#else

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <limits.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <stdint.h>   /* INT64_MAX, for the pool expiry watermark */
#include <errno.h>    /* errno and its codes, read on every platform */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    /* The ROOT certificate store (#2675). Before the OpenSSL headers, which
     * undefine the names wincrypt.h shares with them (X509_NAME and others);
     * aether_thread.h's WIN32_LEAN_AND_MEAN keeps windows.h from bringing it. */
    #include <wincrypt.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>  /* TCP_NODELAY on an upstream this driver dials */
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <poll.h>        /* poll: no FD_SETSIZE bound, unlike select   */
    #include <fcntl.h>       /* fcntl, O_NONBLOCK for connect-with-timeout */
    #include <sys/select.h>  /* select(), fd_set                          */
    #include <sys/time.h>    /* struct timeval                            */
#endif
#include "aether_net_addr.h"

/* ---- pure-Aether TLS 1.3 client bridge (#1849) --------------------------
 *
 * HTTPS in this file is otherwise entirely under AETHER_HAS_OPENSSL, so a
 * cross-compiled binary -- which never links OpenSSL, because zig bundles
 * none -- could not make an https request at all. It failed with a named
 * error rather than silently, but a cross-built HTTPS client was simply
 * impossible, which is what blocked shipping an engine as a .dylib/.so built
 * on one box for another.
 *
 * std.cryptography.tls13_client already implements the whole client: full
 * certificate verification, hostname pinning, chain-to-a-trusted-anchor, and
 * RFC 8448 known-answer tests. It is Aether, and this is C, so the two are
 * joined the same way the SERVER side already joins them (#1813): weak stubs
 * here, overridden at link time by the @c_callback exports in the Aether
 * module when a program imports it.
 *
 * Weak rather than a build flag because the C must link and behave whether or
 * not the Aether module is present -- a program that never imports
 * tls13_client gets these stubs, and `pure_tls_available()` reports false so
 * https fails with a message naming the fix instead of crashing.
 */
#if defined(__GNUC__) || defined(__clang__)
#define AETHER_WEAK __attribute__((weak))
#else
#define AETHER_WEAK
#endif

/* Establish TLS over an ALREADY-CONNECTED socket. The socket may be a direct
 * connection or a forward-proxy CONNECT tunnel -- http_dial completes the
 * tunnel before calling this, so proxying needs no separate path.
 *
 * verify_mode: 0 = full verification, 1 = skip it (set_insecure).
 * cafile: a pinned CA bundle path (set_cafile), or NULL for the default store.
 * Returns an opaque connection, or NULL on failure. */
AETHER_WEAK void* aether_pure_tls_client_connect(int fd, const char* host,
                                                 int verify_mode,
                                                 const char* cafile) {
    (void)fd; (void)host; (void)verify_mode; (void)cafile;
    return NULL;
}
AETHER_WEAK int aether_pure_tls_client_send(void* conn, const void* buf, int len) {
    (void)conn; (void)buf; (void)len;
    return -1;
}
AETHER_WEAK int aether_pure_tls_client_recv(void* conn, void* buf, int len) {
    (void)conn; (void)buf; (void)len;
    return -1;
}
AETHER_WEAK void aether_pure_tls_client_close(void* conn) {
    (void)conn;
}
/* Distinguishes "the Aether module is linked" from "a connect attempt failed",
 * so the error can name the actual fix. */
AETHER_WEAK int aether_pure_tls_client_available(void) {
    return 0;
}

/* #2297: the implementation a program registered (aether_callback_lookup),
 * else the symbol above: the weak stub, or, when the runtime is linked into
 * the program, the program's strong override. A shared runtime binds its
 * own calls to its own stubs, so the registration is what reaches the
 * program's tls13_client there. */
#include "../../runtime/aether_callbacks.h"
typedef void* (*pt_client_connect_fn)(int, const char*, int, const char*);
typedef int   (*pt_client_io_fn)(void*, void*, int);
typedef int   (*pt_client_send_fn)(void*, const void*, int);
typedef void  (*pt_client_close_fn)(void*);
typedef int   (*pt_client_available_fn)(void);
static void* pt_client_connect(int fd, const char* host, int verify_mode, const char* cafile) {
    pt_client_connect_fn f = (pt_client_connect_fn)aether_callback_lookup("aether_pure_tls_client_connect");
    return f ? f(fd, host, verify_mode, cafile)
             : aether_pure_tls_client_connect(fd, host, verify_mode, cafile);
}
static int pt_client_send(void* conn, const void* buf, int len) {
    pt_client_send_fn f = (pt_client_send_fn)aether_callback_lookup("aether_pure_tls_client_send");
    return f ? f(conn, buf, len) : aether_pure_tls_client_send(conn, buf, len);
}
static int pt_client_recv(void* conn, void* buf, int len) {
    pt_client_io_fn f = (pt_client_io_fn)aether_callback_lookup("aether_pure_tls_client_recv");
    return f ? f(conn, buf, len) : aether_pure_tls_client_recv(conn, buf, len);
}
static void pt_client_close(void* conn) {
    pt_client_close_fn f = (pt_client_close_fn)aether_callback_lookup("aether_pure_tls_client_close");
    if (f) f(conn); else aether_pure_tls_client_close(conn);
}
static int pt_client_available(void) {
    pt_client_available_fn f = (pt_client_available_fn)aether_callback_lookup("aether_pure_tls_client_available");
    return f ? f() : aether_pure_tls_client_available();
}

/* Why the last pure handshake on this thread failed.
 *
 * The connect callback hands back only a pointer, so before this every pure
 * failure -- an untrusted chain, a wrong hostname, a TLS 1.2-only server, no
 * trust store at all -- reached the caller as one generic sentence that
 * guessed at the cause. tls13_client knows the real reason; it stores it here
 * just before returning NULL, and http_dial copies it into the request's
 * error. Thread-local because the handshake runs synchronously on the
 * dialling thread, so two requests on two threads cannot see each other's
 * reason, and no lock is needed. Cleared before each connect so a stale
 * reason is never reported for a later failure that did not set one. */
static _Thread_local char pt_client_last_err[512];

void aether_pure_tls_client_set_error(const char* msg) {
    if (!msg) { pt_client_last_err[0] = '\0'; return; }
    size_t n = strlen(msg);
    if (n >= sizeof(pt_client_last_err)) n = sizeof(pt_client_last_err) - 1;
    memcpy(pt_client_last_err, msg, n);
    pt_client_last_err[n] = '\0';
}

/* Which TLS client backend an https request uses.
 *
 * A build without OpenSSL has only the pure-Aether client. A build WITH it
 * keeps OpenSSL as the default and switches to the pure client when
 * AETHER_PURE_TLS is 1 or true -- the same variable that already selects the
 * pure TLS server (aether_http_server.c), so one setting moves a process onto
 * the pure stack in both directions. Read per request rather than cached, so
 * the pool key and the dial always agree on the answer for a given request. */
static int http_client_use_pure_tls(void) {
#ifndef AETHER_HAS_OPENSSL
    return 1;
#else
    const char* v = getenv("AETHER_PURE_TLS");
    return v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
#endif
}

#ifdef AETHER_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

static int http_initialized = 0;

static void http_init(void) {
    if (http_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    http_initialized = 1;
}

// -----------------------------------------------------------------
// URL parsing
// -----------------------------------------------------------------

// Parse `url` into (host, port, path, use_tls).
//   https://foo.example/bar  →  host="foo.example" port=443 path="/bar" use_tls=1
//   http://foo:8080/         →  host="foo"         port=8080 path="/"   use_tls=0
// Returns 1 on success, 0 on malformed input.
/* A string copy that refuses rather than cuts: 1 when src fits in cap bytes
 * with its NUL, 0 (and nothing copied) when it does not. snprintf would bring
 * the whole formatting machinery with it, which parse_url paid for on a path
 * that runs once per proxied request; and it would cut. A path cut to the
 * buffer was sent anyway and answered for the wrong thing (#2674). */
static int url_copy(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) return 0;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 1;
}

int parse_url(const char* url, char* host, size_t host_size,
                     int* port, char* path, size_t path_size, int* use_tls) {
    if (!url || !host || !port || !use_tls || host_size == 0 ||
        (path && path_size == 0)) return 0;

    const char* start;
    *use_tls = 0;

    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        *port = 80;
    } else if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
        *port = 443;
        *use_tls = 1;
    } else {
        start = url;
        *port = 80;
    }

    /* The authority ends at the path. An IPv6 host is bracketed
     * ("[::1]:8443") and comes back without the brackets; every user of
     * `host` (the resolver, SNI, the sandbox resource, the Host header) wants
     * it bare or brackets it again itself. */
    const char* slash = strchr(start, '/');
    size_t alen = slash ? (size_t)(slash - start) : strlen(start);
    const char* port_at = NULL;
    size_t port_len = 0;
    if (aether_addr_split_authority(start, alen, host, host_size, &port_at, &port_len) != 0)
        return 0;
    if (port_at) *port = atoi(port_at);
    if (path && !url_copy(path, path_size, slash ? slash : "/")) return 0;

    return 1;
}

int parse_url_alloc(const char* url, char* host, size_t host_size,
                    int* port, char** path_out, int* use_tls) {
    if (!path_out) return 0;
    *path_out = NULL;
    if (!url) return 0;
    /* The path is a tail of the URL, or "/" when it has none. */
    size_t cap = strlen(url) + 2;
    char* path = (char*)malloc(cap);
    if (!path) return 0;
    if (!parse_url(url, host, host_size, port, path, cap, use_tls)) {
        free(path);
        return 0;
    }
    *path_out = path;
    return 1;
}

// -----------------------------------------------------------------
// Windows trust anchors, the same for both HTTPS clients (#2675)
// -----------------------------------------------------------------
//
// Neither client read the Windows certificate store, so a program copied to a
// machine without MSYS2 or Git for Windows could make no HTTPS request at all,
// and the two looked for different bundle files (OpenSSL's list here, the
// pure-Aether client's in std.cryptography.tls13_client), so one could work on
// a machine where the other failed. Both now trust, unless SSL_CERT_FILE or
// SSL_CERT_DIR names the store, every certificate in the system ROOT store
// (what Python's ssl module loads on Windows) together with the first bundle
// file of the one list below. The bundle joins the store rather than standing
// in for it: Windows adds a root to the store on demand, the first time
// CryptoAPI needs it, so a root a server chains to can be missing from the
// store of a machine that has never met that server.
//
// The tls13_client module reaches these through extern declarations, which is
// why they live here, beside the OpenSSL code that uses them too, and why
// they exist (returning nothing) on every platform.

#ifdef _WIN32
/* wincrypt.h is included with winsock2.h at the top, before the OpenSSL
 * headers, which undefine the names it shares with them (X509_NAME...). */

/* The first file of the bundle list that exists, in `out`; "" when none
 * does. In order: the bundle the release archive ships beside the program
 * (<root>\bin\prog.exe -> <root>\share\ssl\ca-bundle.crt, or the program's own
 * directory when it is not in a bin\), MSYS2's ca-certificates for MINGW64
 * and UCRT64 under both names it has used, and Git for Windows'. */
static const char* windows_ca_bundle(char* out, size_t cap) {
    static const char* fixed[] = {
        "C:\\msys64\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\ucrt64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\mingw64\\etc\\ssl\\cert.pem",
        "C:\\msys64\\ucrt64\\etc\\ssl\\cert.pem",
        "C:\\Program Files\\Git\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt",
        NULL
    };
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (n > 0 && n < sizeof(exe_path)) {
        char* p = strrchr(exe_path, '\\');           // ...\bin\prog.exe -> ...\bin
        if (p) {
            *p = '\0';
            char* q = strrchr(exe_path, '\\');        // ...\bin -> ...
            if (q && _stricmp(q, "\\bin") == 0) *q = '\0';
        }
        snprintf(out, cap, "%s\\share\\ssl\\ca-bundle.crt", exe_path);
        DWORD a = GetFileAttributesA(out);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return out;
    }
    for (int i = 0; fixed[i]; i++) {
        DWORD a = GetFileAttributesA(fixed[i]);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(out, cap, "%s", fixed[i]);
            return out;
        }
    }
    if (cap) out[0] = '\0';
    return out;
}

/* Calls `each` with the DER of every certificate in the ROOT system store of
 * the current user, which includes the machine's. Returns how many. */
static int windows_root_store_each(void (*each)(const unsigned char* der, size_t len, void* arg),
                                   void* arg) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return 0;
    int n = 0;
    PCCERT_CONTEXT c = NULL;
    while ((c = CertEnumCertificatesInStore(store, c)) != NULL) {
        if ((c->dwCertEncodingType & X509_ASN_ENCODING) && c->cbCertEncoded > 0) {
            each(c->pbCertEncoded, (size_t)c->cbCertEncoded, arg);
            n++;
        }
    }
    CertCloseStore(store, 0);
    return n;
}

typedef struct { char* buf; size_t len, cap; int oom; } PemOut;

static void pem_out_append(PemOut* o, const char* s, size_t n) {
    if (o->oom) return;
    if (o->len + n + 1 > o->cap) {
        size_t want = o->cap ? o->cap * 2 : 65536;
        while (want < o->len + n + 1) want *= 2;
        char* grown = (char*)aether_caps_realloc(o->buf, o->cap, want);
        if (!grown) { o->oom = 1; return; }
        o->buf = grown;
        o->cap = want;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

/* One certificate as a PEM block, base64 in lines of 64. */
static void pem_out_cert(const unsigned char* der, size_t len, void* arg) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    PemOut* o = (PemOut*)arg;
    pem_out_append(o, "-----BEGIN CERTIFICATE-----\n", 28);
    char line[65];
    size_t col = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)der[i] << 16;
        if (i + 1 < len) v |= (unsigned)der[i + 1] << 8;
        if (i + 2 < len) v |= der[i + 2];
        line[col++] = b64[(v >> 18) & 63];
        line[col++] = b64[(v >> 12) & 63];
        line[col++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
        line[col++] = i + 2 < len ? b64[v & 63] : '=';
        if (col == 64) {
            line[col++] = '\n';
            pem_out_append(o, line, col);
            col = 0;
        }
    }
    if (col) {
        line[col++] = '\n';
        pem_out_append(o, line, col);
    }
    pem_out_append(o, "-----END CERTIFICATE-----\n", 26);
}
#endif  // _WIN32

/* Every certificate of the Windows ROOT system store as PEM text, a fresh
 * counted string the caller releases; "" when there is none, and on any other
 * platform. NULL only when out of memory. */
char* aether_tls_windows_store_pem(void) {
#ifdef _WIN32
    PemOut o = { NULL, 0, 0, 0 };
    windows_root_store_each(pem_out_cert, &o);
    if (o.oom) { aether_caps_free(o.buf, o.cap); return NULL; }
    if (o.buf) return (char*)string_adopt_caps_buffer(o.buf, o.len, o.cap);
#endif
    return (char*)string_empty();
}

/* The first bundle file of the list above that exists, a fresh counted string
 * the caller releases; "" when none does, and on any other platform. NULL only
 * when out of memory. */
char* aether_tls_windows_bundle(void) {
#ifdef _WIN32
    char path[MAX_PATH + 64];
    windows_ca_bundle(path, sizeof(path));
    size_t n = strlen(path);
#else
    const char* path = "";
    size_t n = 0;
#endif
    return (char*)string_new_with_length(path, n);
}

// -----------------------------------------------------------------
// OpenSSL context: lazy init, shared across all HTTPS calls
// -----------------------------------------------------------------

#ifdef AETHER_HAS_OPENSSL

static _Atomic(SSL_CTX*) g_ssl_ctx;

#ifdef _WIN32
/* One certificate of the ROOT store into the context's X509_STORE. One that
 * OpenSSL cannot parse, or that is there already, is passed over. */
static void x509_store_add_der(const unsigned char* der, size_t len, void* arg) {
    const unsigned char* p = der;
    X509* x = d2i_X509(NULL, &p, (long)len);
    if (!x) return;
    X509_STORE_add_cert((X509_STORE*)arg, x);
    X509_free(x);
}

// The trust anchors on Windows, which OpenSSL has no useful default for there,
// chosen as the pure-Aether client chooses them (see "Windows trust anchors"
// above): when SSL_CERT_FILE or SSL_CERT_DIR is set, what they name and
// nothing else; otherwise every certificate of the ROOT system store and the
// first bundle file of the shared list. Returns 1 when anything was loaded.
//
// An SSL_CERT_FILE that did not load used to fall through to the bundle
// files, while the pure client failed closed on it; now both fail closed.
static int load_windows_trust(SSL_CTX* ctx) {
    const char* file = getenv("SSL_CERT_FILE");
    const char* dir = getenv("SSL_CERT_DIR");
    if (file && !file[0]) file = NULL;
    if (dir && !dir[0]) dir = NULL;
    int loaded = 0;
    if (file || dir) {
        if (file && SSL_CTX_load_verify_locations(ctx, file, NULL) == 1) loaded = 1;
        if (dir && SSL_CTX_load_verify_locations(ctx, NULL, dir) == 1) loaded = 1;
    } else {
        loaded = windows_root_store_each(x509_store_add_der, SSL_CTX_get_cert_store(ctx)) > 0;
        char bundle[MAX_PATH + 64];
        if (windows_ca_bundle(bundle, sizeof(bundle))[0] &&
            SSL_CTX_load_verify_locations(ctx, bundle, NULL) == 1) loaded = 1;
    }
    /* What failed to load (a duplicate, a missing file) is no error of any
     * request, and must not be reported as the cause of one later. */
    ERR_clear_error();
    return loaded;
}
#endif  // _WIN32

// Get (or lazily create) the shared SSL_CTX. Benign race on first call —
// compare-exchange ensures at most one SSL_CTX is installed even if two
// threads reach here simultaneously. Returns NULL on OpenSSL error.
//
// Not static: the WebSocket client in aether_http_server.c dials wss:// with
// the same trust setup. Sharing this rather than building a second client CTX
// there keeps one place where the trust store is decided -- the Windows trust
// anchors above were hard enough to get right once (#1107, #1110, #2675) that
// a second copy would be a liability. The caller must NOT free it; the CTX is
// process-wide and outlives any one connection.
static SSL_CTX* get_ssl_ctx(void);
SSL_CTX* aether_http_client_ssl_ctx(void) { return get_ssl_ctx(); }

static SSL_CTX* get_ssl_ctx(void) {
    SSL_CTX* ctx = atomic_load(&g_ssl_ctx);
    if (ctx) return ctx;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

#ifdef _WIN32
    // Windows OpenSSL builds typically have no compiled-in CA path: the
    // anchors are the ones load_windows_trust finds. Only when it finds none
    // (an empty store and no bundle file) does the default-paths call get a
    // turn, for a build whose compiled-in OPENSSLDIR does hold certificates.
    if (!load_windows_trust(ctx)) {
        SSL_CTX_set_default_verify_paths(ctx);
    }
#else
    // Linux/macOS: the system trust store lives at the path OpenSSL was
    // compiled with — /etc/ssl/certs and the macOS keychain bridge.
    SSL_CTX_set_default_verify_paths(ctx);
#endif

    SSL_CTX* expected = NULL;
    if (!atomic_compare_exchange_strong(&g_ssl_ctx, &expected, ctx)) {
        SSL_CTX_free(ctx);
        ctx = expected;
    }
    return ctx;
}

// Flush OpenSSL's error queue into a newly allocated string. Used only
// on error paths to surface the underlying OpenSSL reason.
static char* ssl_err_string(const char* prefix) {
    unsigned long err = ERR_get_error();
    const char* detail = err ? ERR_reason_error_string(err) : NULL;
    size_t plen = strlen(prefix);
    size_t dlen = detail ? strlen(detail) : 0;
    char* msg = (char*)malloc(plen + (dlen ? dlen + 3 : 0) + 1);
    if (!msg) return NULL;
    memcpy(msg, prefix, plen);
    if (dlen) {
        memcpy(msg + plen, ": ", 2);
        memcpy(msg + plen + 2, detail, dlen);
        msg[plen + 2 + dlen] = '\0';
    } else {
        msg[plen] = '\0';
    }
    return msg;
}

#endif // AETHER_HAS_OPENSSL

// -----------------------------------------------------------------
// Transport abstraction
//
// `Transport` wraps either a raw socket or an SSL* connection so the
// request/response loop doesn't need to branch on protocol everywhere.
// send/recv callbacks match the BSD socket signatures so plaintext
// paths can use them as-is.
// -----------------------------------------------------------------


static int transport_send(Transport* t, const void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_write(t->ssl, buf, len);
#endif
    if (t->pure_tls) return pt_client_send(t->pure_tls, buf, len);
    return (int)send(t->sockfd, buf, len, 0);
}

static int transport_recv(Transport* t, void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_read(t->ssl, buf, len);
#endif
    if (t->pure_tls) return pt_client_recv(t->pure_tls, buf, len);
    return (int)recv(t->sockfd, buf, len, 0);
}

static void transport_close(Transport* t) {
    /* A retired connection takes its kept receive buffer with it (#2160). */
    if (t->rxbuf) {
        aether_caps_free(t->rxbuf, t->rxcap);
        t->rxbuf = NULL;
        t->rxcap = 0;
    }
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) {
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = NULL;
    }
    /* Free the per-request CTX (set_cafile pin) AFTER the SSL that used it. */
    if (t->owned_ctx) {
        SSL_CTX_free(t->owned_ctx);
        t->owned_ctx = NULL;
    }
#endif
    /* The pure connection owns the socket once the handshake succeeds, so it
     * closes the fd itself; clearing sockfd here stops the close() below from
     * closing a descriptor twice (and, worse, one the process may have already
     * reused). */
    if (t->pure_tls) {
        pt_client_close(t->pure_tls);
        t->pure_tls = NULL;
        t->sockfd = -1;
    }
    if (t->sockfd >= 0) {
        close(t->sockfd);
        t->sockfd = -1;
    }
}

// -----------------------------------------------------------------
// Streaming response bodies (#1004)
//
// A streaming response keeps its transport open after the header block
// and delivers the body incrementally, so a multi-megabyte download never
// materialises whole in memory: peak usage is one read window plus the
// small `pending` buffer, not O(Content-Length). The HttpStream carries
// the open transport, the body framing (Content-Length, chunked, or
// read-until-close), and `pending` — raw socket bytes already read but not
// yet decoded/delivered (the over-read past the header terminator, and
// chunk-framing bytes that straddle recv boundaries).
// -----------------------------------------------------------------

struct HttpStream {
    Transport t;
    int  chunked;             /* Transfer-Encoding: chunked */
    int  read_until_close;    /* no Content-Length and not chunked: body ends at EOF */
    long long content_remaining; /* Content-Length framing: undelivered body bytes */
    /* chunked decode state machine: 0=read size line, 1=deliver chunk data,
     * 2=consume the CRLF that trails a chunk's data. */
    int  chunk_state;
    long long chunk_remaining; /* chunked: undelivered payload bytes in current chunk */
    int  eof;                 /* body fully delivered (nothing more will arrive) */
    int  err;                 /* transport error mid-body */
    char* pending;            /* raw bytes read from transport, not yet consumed */
    int   pending_pos;
    int   pending_len;
    int   pending_cap;
};

// -----------------------------------------------------------------
// Idle connection pool (#1653)
//
// HTTP/1.1 connections are persistent, and a client that closes after every
// response pays a TCP handshake (and a TLS handshake) per request. Measured
// against nginx, that churn was the whole of std.http.server.lb's 3.7x deficit
// and its 8% dropped requests: the proxy dialled its upstream once per
// request.
//
// A connection is only returned here when the response framing was definite
// (Content-Length or chunked) and neither side asked to close, so nothing is
// ever reused when the connection itself was the message delimiter. Entries
// are keyed by everything that makes two connections non-interchangeable:
// origin, the proxy actually dialled, TLS, and the verification the caller
// asked for. A pooled socket the peer closed while idle is indistinguishable
// from a live one until it is used, so the request path retries once on a
// fresh connection when a reused one fails before any response byte arrives.
// -----------------------------------------------------------------

#define HTTP_POOL_KEY_MAX 512

typedef struct HttpIdleConn {
    Transport t;
    char      key[HTTP_POOL_KEY_MAX];
    int64_t   idle_since_ms;
    struct HttpIdleConn* next;
} HttpIdleConn;

/* CRITICAL_SECTION, which the thread shim maps pthread_mutex_t to on Windows,
 * has no static initialiser, so the lock is created once through an atomic
 * gate there. Same shape as runtime/sandbox/aether_audit.c. */
#if defined(_WIN32)
static pthread_mutex_t http_pool_mutex;
static atomic_int      http_pool_mutex_state = 0;   /* 0 unset, 1 setting, 2 ready */
static pthread_mutex_t* http_pool_lock(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&http_pool_mutex_state, &expected, 1)) {
        pthread_mutex_init(&http_pool_mutex, NULL);
        atomic_store(&http_pool_mutex_state, 2);
    } else {
        while (atomic_load(&http_pool_mutex_state) != 2) { /* one-shot spin */ }
    }
    return &http_pool_mutex;
}
#else
static pthread_mutex_t http_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* http_pool_lock(void) { return &http_pool_mutex; }
#endif

static HttpIdleConn*   http_pool_head = NULL;
static int             http_pool_count = 0;
static int             http_pool_enabled = 1;
static int             http_pool_max_idle = 64;
static int             http_pool_max_per_key = 8;
static int64_t         http_pool_idle_ms = 15000;
/* When the oldest pooled connection becomes eligible for expiry, or INT64_MAX
 * when the pool is empty (#1719).
 *
 * http_pool_take and http_pool_put both swept the whole idle list on every
 * request, under the global pool mutex. With the default 15s idle window and a
 * proxy reusing its upstreams continuously, that sweep frees nothing on the
 * overwhelming majority of calls -- it is a list walk per request to discover
 * there is nothing to do. Tracking the earliest deadline lets both callers skip
 * the walk outright until that time actually arrives.
 *
 * Kept deliberately coarse: it is a lower bound, not an exact answer. Entries
 * are only ever added with a fresh idle_since_ms, so the earliest deadline can
 * only move later when the list shrinks, and recomputing it during a sweep we
 * were doing anyway is free. A stale-early value costs one redundant sweep,
 * never a missed expiry. */
static int64_t         http_pool_next_expiry_ms = INT64_MAX;

static int64_t http_now_ms(void) { return (int64_t)http_clock_ms(); }

/* Everything that makes two connections non-interchangeable: the origin, the
 * endpoint actually dialled (proxy or origin), TLS and which backend carries
 * it (`use_tls` is 0 plain, 1 OpenSSL, 2 pure-Aether), and the verification the
 * caller asked for. A connection opened with a pinned CA or with verification
 * off must never be handed to a request that did not ask for that. */
static void http_pool_key(char* out, size_t n, const char* host, int port,
                          int use_tls, const char* dial_host, int dial_port,
                          int insecure, const char* cafile) {
    const char* h  = host      ? host      : "";
    const char* dh = dial_host ? dial_host : "";
    const char* ca = cafile    ? cafile    : "";
    size_t hl = strlen(h), dhl = strlen(dh), cal = strlen(ca);

    /* This string is the pool's identity for a connection, so the bytes have
     * to be exactly what the format produced. Anything that would not fit, or
     * a port that is not a plain number, goes back through snprintf rather
     * than being rendered differently here. */
    if (port < 0 || dial_port < 0 || hl + dhl + cal + 48 > n) {
        snprintf(out, n, "%s:%d|%s:%d|%d|%d|%s",
                 h, port, dh, dial_port,
                 use_tls, insecure ? 1 : 0, ca);
        return;
    }

    char* p = out;
    memcpy(p, h, hl); p += hl;
    *p++ = ':';
    p += http_write_dec(p, (unsigned long long)port);
    *p++ = '|';
    memcpy(p, dh, dhl); p += dhl;
    *p++ = ':';
    p += http_write_dec(p, (unsigned long long)dial_port);
    *p++ = '|';
    *p++ = (char)('0' + (use_tls >= 0 && use_tls <= 9 ? use_tls : 1));
    *p++ = '|';
    *p++ = insecure ? '1' : '0';
    *p++ = '|';
    memcpy(p, ca, cal); p += cal;
    *p = '\0';
}

/* Caller holds the lock. Drops every entry idle past the timeout.
 *
 * Returns immediately when the earliest deadline is still in the future, which
 * is the common case on a busy proxy -- see http_pool_next_expiry_ms. When it
 * does sweep, it recomputes that watermark from the survivors. */
static void http_pool_expire_locked(int64_t now) {
    if (now < http_pool_next_expiry_ms) return;

    int64_t earliest = INT64_MAX;
    HttpIdleConn** link = &http_pool_head;
    while (*link) {
        HttpIdleConn* c = *link;
        if (now - c->idle_since_ms >= http_pool_idle_ms) {
            *link = c->next;
            http_pool_count--;
            transport_close(&c->t);
            free(c);
            continue;
        }
        int64_t due = c->idle_since_ms + http_pool_idle_ms;
        if (due < earliest) earliest = due;
        link = &c->next;
    }
    http_pool_next_expiry_ms = earliest;
}

/* Take an idle connection for `key`, newest first (the most recently used is
 * the one the peer is least likely to have closed). Returns 1 on a hit. */
static int http_pool_take(const char* key, Transport* out) {
    if (!http_pool_enabled) return 0;
    int found = 0;
    pthread_mutex_lock(http_pool_lock());
    http_pool_expire_locked(http_now_ms());
    HttpIdleConn** link = &http_pool_head;
    while (*link) {
        HttpIdleConn* c = *link;
        if (strcmp(c->key, key) == 0) {
            *link = c->next;
            http_pool_count--;
            *out = c->t;
            free(c);
            found = 1;
            break;
        }
        link = &c->next;
    }
    pthread_mutex_unlock(http_pool_lock());
    return found;
}

/* Hand a still-usable connection back. Takes ownership either way: over the
 * caps it is closed here rather than returned to the caller to close. */
static void http_pool_put(const char* key, Transport* t) {
    if (!http_pool_enabled || t->sockfd < 0) {
        transport_close(t);
        return;
    }
    HttpIdleConn* c = (HttpIdleConn*)malloc(sizeof(HttpIdleConn));
    if (!c) {
        transport_close(t);
        return;
    }
    int64_t now = http_now_ms();
    pthread_mutex_lock(http_pool_lock());
    http_pool_expire_locked(now);
    /* Only whether the per-key cap is REACHED matters, not the exact count, so
     * stop at the cap instead of walking to the end -- and skip the walk
     * entirely when the global cap already rejects this connection. */
    int per_key = 0;
    /* The per-key cap cannot bind when it is at or above the global cap: the
     * entries sharing a key are a subset of the pool, so their count is below
     * the global cap already, and the test below would always be false. Under
     * a proxy the two caps are equal, and skipping the walk there is what
     * keeps this loop from growing with the pool: it compares a key against
     * every entry, and sizing the pool for a proxy made it the largest single
     * userspace cost on the path (strcmp, 1.9% of the profile).
     *
     * A client keeps the two caps apart and still walks, breaking early at
     * the cap as before. */
    if (http_pool_count < http_pool_max_idle && http_pool_max_per_key < http_pool_max_idle) {
        for (HttpIdleConn* e = http_pool_head; e; e = e->next) {
            if (strcmp(e->key, key) == 0 && ++per_key >= http_pool_max_per_key)
                break;
        }
    }
    if (http_pool_count >= http_pool_max_idle || per_key >= http_pool_max_per_key) {
        pthread_mutex_unlock(http_pool_lock());
        free(c);
        transport_close(t);
        return;
    }
    c->t = *t;
    /* A bounded copy, not a formatted one. This runs every time a connection
     * goes back to the pool, which is once per proxied request, and asking
     * snprintf to move a string brings the whole formatting machinery with it:
     * the profile put 15% of this path's last-level write misses in vfprintf,
     * reached from here. Truncation behaves as the format did. */
    size_t kl = strlen(key);
    if (kl >= sizeof(c->key)) kl = sizeof(c->key) - 1;
    memcpy(c->key, key, kl);
    c->key[kl] = '\0';
    c->idle_since_ms = now;
    c->next = http_pool_head;
    http_pool_head = c;
    http_pool_count++;
    /* On an empty pool the watermark is INT64_MAX, and this entry is now the
     * only deadline there is. Lowering it here is what re-arms the sweep. */
    if (c->idle_since_ms + http_pool_idle_ms < http_pool_next_expiry_ms)
        http_pool_next_expiry_ms = c->idle_since_ms + http_pool_idle_ms;
    pthread_mutex_unlock(http_pool_lock());
    /* The pool entry owns everything the Transport did now, so the
     * caller's copy must stop pointing at any of it: a later
     * transport_close on the caller's struct would otherwise close the
     * socket or free the kept receive buffer (#2160) out from under
     * the pooled connection -- a double free the next take would
     * then walk into. */
    t->sockfd = -1;
    t->rxbuf = NULL;
    t->rxcap = 0;
#ifdef AETHER_HAS_OPENSSL
    t->ssl = NULL;
    t->owned_ctx = NULL;
#endif
}

/* Close and drop every idle connection. */
void http_client_pool_clear_raw(void) {
    pthread_mutex_lock(http_pool_lock());
    HttpIdleConn* c = http_pool_head;
    http_pool_head = NULL;
    http_pool_count = 0;
    http_pool_next_expiry_ms = INT64_MAX;   /* nothing left to expire */
    pthread_mutex_unlock(http_pool_lock());
    while (c) {
        HttpIdleConn* next = c->next;
        transport_close(&c->t);
        free(c);
        c = next;
    }
}

/* Size the pool for a reverse proxy rather than for a client.
 *
 * The caps below are a client library's: a program fetching pages keeps a few
 * connections per host and holding more would waste descriptors it will never
 * use. A reverse proxy is the opposite. Every request in flight holds one
 * upstream connection and returns it on completion, so a proxy serving N
 * concurrent requests needs about N of them; with the client default of 8 per
 * host, every connection past the eighth was closed on release and dialled
 * again for the next request.
 *
 * That churn was not visible as CPU in any profile. It showed up as TCP
 * segments: 5.61 per request against nginx's 4.02, with a TIME_WAIT socket
 * created every sixth request, because each replacement connection pays a
 * handshake and a shutdown that carry no HTTP at all.
 *
 * The size comes from the descriptor budget rather than a constant, because
 * that is the resource being spent and it is what differs between a container
 * with 256 descriptors and a tuned host with a million. A quarter of the
 * budget leaves the rest for the connections being served. Caps are only ever
 * raised: a caller who has configured the pool deliberately keeps its
 * settings.
 */
/* The pool's current caps, for reporting and for tests that need to know
 * whether mounting a proxy resized it. */
void http_client_pool_caps_raw(int* max_idle, int* max_per_host) {
    pthread_mutex_lock(http_pool_lock());
    if (max_idle)     *max_idle = http_pool_max_idle;
    if (max_per_host) *max_per_host = http_pool_max_per_key;
    pthread_mutex_unlock(http_pool_lock());
}

void http_client_pool_size_for_proxy(void) {
    int budget = 1024;
#if !defined(_WIN32)
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0) {
        budget = (rl.rlim_cur > (rlim_t)INT_MAX) ? INT_MAX : (int)rl.rlim_cur;
    }
#endif
    int want = budget / 4;
    if (want < 64)   want = 64;
    if (want > 1024) want = 1024;

    pthread_mutex_lock(http_pool_lock());
    if (http_pool_enabled) {
        if (http_pool_max_idle < want)    http_pool_max_idle = want;
        /* One backend may legitimately take the whole pool: a proxy with a
         * single upstream is the ordinary case, and a per-host cap below the
         * total would reintroduce exactly the churn this removes. */
        if (http_pool_max_per_key < want) http_pool_max_per_key = want;
    }
    pthread_mutex_unlock(http_pool_lock());
}

/* Reconfigure the pool. `max_idle` 0 turns reuse off (and clears what is
 * held); negative values leave that setting untouched. */
const char* http_client_pool_configure_raw(int max_idle, int max_per_host,
                                           int64_t idle_ns) {
    pthread_mutex_lock(http_pool_lock());
    if (max_idle >= 0) {
        http_pool_max_idle = max_idle;
        http_pool_enabled = max_idle > 0;
    }
    if (max_per_host >= 0) http_pool_max_per_key = max_per_host;
    if (idle_ns >= 0) {
        int64_t ms = idle_ns / 1000000LL;
        http_pool_idle_ms = ms > 0 ? ms : 1;
        /* The expiry watermark was derived from the PREVIOUS window, so
         * shortening the window would leave a deadline further out than the new
         * setting allows and delay every eviction. Force the next pool
         * operation to sweep and recompute it. */
        http_pool_next_expiry_ms = INT64_MIN;
    }
    pthread_mutex_unlock(http_pool_lock());
    if (!http_pool_enabled) http_client_pool_clear_raw();
    return "";
}

/* How many receive buffers the client has had to allocate from nothing
 * (#2160). A response read on a pooled connection that kept its buffer does
 * not count; one read on a fresh connection, or on one whose buffer was not
 * worth keeping, does. It is how the reuse is observed rather than assumed:
 * N requests over one pooled connection allocate once, and a regression that
 * drops the buffer again shows up as N. Monotonic for the process. */
static atomic_llong http_rx_fresh_allocs = 0;

long long http_client_rx_buffer_allocs_raw(void) {
    return (long long)atomic_load(&http_rx_fresh_allocs);
}

int http_client_pool_idle_count_raw(void) {
    pthread_mutex_lock(http_pool_lock());
    int n = http_pool_count;
    pthread_mutex_unlock(http_pool_lock());
    return n;
}


static void http_stream_free(struct HttpStream* s) {
    if (!s) return;
    transport_close(&s->t);
    free(s->pending);
    free(s);
}

/* Compact the unconsumed region to the front of `pending`, then read one more
 * window from the transport onto the end. Returns the unconsumed byte count
 * afterward: >0 = have data, 0 = clean EOF (nothing more will arrive),
 * -1 = transport error (sets s->err). */
static int stream_pump(struct HttpStream* s) {
    int avail = s->pending_len - s->pending_pos;
    if (s->pending_pos > 0) {
        if (avail > 0) memmove(s->pending, s->pending + s->pending_pos, (size_t)avail);
        s->pending_len = avail;
        s->pending_pos = 0;
    }
    if (s->pending_cap - s->pending_len < 8192) {
        int ncap = s->pending_cap ? s->pending_cap * 2 : 16384;
        while (ncap - s->pending_len < 8192) ncap *= 2;
        char* nb = (char*)realloc(s->pending, (size_t)ncap);
        if (!nb) { s->err = 1; return -1; }
        s->pending = nb;
        s->pending_cap = ncap;
    }
    int n = transport_recv(&s->t, s->pending + s->pending_len,
                           s->pending_cap - s->pending_len);
    if (n < 0) { s->err = 1; return -1; }
    if (n > 0) s->pending_len += n;
    return s->pending_len - s->pending_pos;  /* n==0 -> whatever remains buffered */
}

/* Consume a CRLF (or bare LF, tolerated) at the current read position, pumping
 * if needed. Returns 1 on success, 0 if a line terminator can't be obtained
 * (malformed framing at EOF -> sets s->err). */
static int stream_consume_crlf(struct HttpStream* s) {
    for (;;) {
        int avail = s->pending_len - s->pending_pos;
        if (avail >= 1) {
            char c = s->pending[s->pending_pos];
            if (c == '\r') {
                if (avail >= 2) {
                    s->pending_pos += (s->pending[s->pending_pos + 1] == '\n') ? 2 : 1;
                    return 1;
                }
                /* need the byte after '\r' */
            } else if (c == '\n') {
                s->pending_pos += 1;
                return 1;
            } else {
                /* not a line terminator where one was expected */
                s->err = 1;
                return 0;
            }
        }
        int r = stream_pump(s);
        if (r < 0) return 0;
        if (r == avail) { s->err = 1; return 0; }  /* EOF, no terminator */
    }
}

/* Parse a chunk-size line ("<hex>[;ext]\r\n"), pumping until the CRLF is
 * available. Sets *out to the parsed size and advances past the line.
 * Returns 1 on success, 0 on malformed framing / EOF (sets s->err). */
static int stream_read_chunk_size(struct HttpStream* s, long long* out) {
    for (;;) {
        /* find CRLF in the unconsumed region */
        int start = s->pending_pos, end = s->pending_len, nl = -1;
        for (int i = start; i < end; i++) {
            if (s->pending[i] == '\n') { nl = i; break; }
        }
        if (nl >= 0) {
            long long sz = 0; int any = 0;
            for (int i = start; i < nl; i++) {
                char c = s->pending[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;  /* end of hex digits (CR, ';', or ext) */
                sz = sz * 16 + d;
                any = 1;
            }
            s->pending_pos = nl + 1;  /* consume through the LF */
            if (!any) { s->err = 1; return 0; }
            *out = sz;
            return 1;
        }
        int prev = s->pending_len - s->pending_pos;
        int r = stream_pump(s);
        if (r < 0) return 0;
        if (r == prev) { s->err = 1; return 0; }  /* EOF without a size line */
    }
}

/* Deliver up to `max` decoded body bytes into `out`. Returns bytes delivered
 * (>0), 0 at clean end-of-body, or -1 on transport / framing error. */
static int stream_read_decoded(struct HttpStream* s, char* out, int max) {
    if (s->err) return -1;
    if (s->eof) return 0;
    if (max <= 0) return 0;

    if (!s->chunked) {
        int avail = s->pending_len - s->pending_pos;
        if (avail == 0) {
            int r = stream_pump(s);
            if (r < 0) return -1;
            avail = s->pending_len - s->pending_pos;
            if (avail == 0) { s->eof = 1; return 0; }  /* EOF (or content-length short) */
        }
        int give = avail < max ? avail : max;
        if (!s->read_until_close && (long long)give > s->content_remaining)
            give = (int)s->content_remaining;
        memcpy(out, s->pending + s->pending_pos, (size_t)give);
        s->pending_pos += give;
        if (!s->read_until_close) {
            s->content_remaining -= give;
            if (s->content_remaining <= 0) s->eof = 1;
        }
        return give;
    }

    /* Chunked: run the state machine until `out` fills or the body ends. */
    int produced = 0;
    while (produced < max && !s->eof) {
        if (s->chunk_state == 1) {  /* deliver chunk payload */
            if (s->chunk_remaining == 0) { s->chunk_state = 2; continue; }
            int avail = s->pending_len - s->pending_pos;
            if (avail == 0) {
                int r = stream_pump(s);
                if (r < 0) return produced ? produced : -1;
                avail = s->pending_len - s->pending_pos;
                if (avail == 0) { s->err = 1; return produced ? produced : -1; }  /* truncated */
            }
            int give = avail;
            if ((long long)give > s->chunk_remaining) give = (int)s->chunk_remaining;
            if (give > max - produced) give = max - produced;
            memcpy(out + produced, s->pending + s->pending_pos, (size_t)give);
            s->pending_pos += give;
            s->chunk_remaining -= give;
            produced += give;
            if (s->chunk_remaining == 0) s->chunk_state = 2;
            continue;
        }
        if (s->chunk_state == 2) {  /* trailing CRLF after chunk data */
            if (!stream_consume_crlf(s)) return produced ? produced : -1;
            s->chunk_state = 0;
            continue;
        }
        /* chunk_state == 0: read the next size line */
        long long sz = 0;
        if (!stream_read_chunk_size(s, &sz)) return produced ? produced : -1;
        if (sz == 0) {
            /* terminal chunk: consume the CRLF after "0" (trailers, if any,
             * are ignored) and end the body. */
            stream_consume_crlf(s);
            s->eof = 1;
            break;
        }
        s->chunk_remaining = sz;
        s->chunk_state = 1;
    }
    return produced;
}

// -----------------------------------------------------------------
// v2 request builder — opaque struct + per-field setters. The v1
// one-liners (http_get_raw / http_post_raw / etc.) build a request
// internally and call http_send_raw, so all paths funnel through
// the same socket / TLS code below.
// -----------------------------------------------------------------

/* One allocation per header, not three (#1739).
 *
 * `name` and `value` point into the same block as the node, laid out
 * immediately after it: [HttpHeader][name\0][value\0]. A proxied request
 * forwards on the order of seven headers, and at a calloc plus two strdups
 * each that was 21 of the 76 allocations a proxied request made. They are
 * written once at insertion and never reassigned, so there is nothing to
 * grow and the node's own free() releases all three.
 *
 * The pointers stay rather than being computed from the node, because every
 * reader is `h->name` / `h->value` and this keeps them working unchanged. */
typedef struct HttpHeader {
    /* Set when this node came from a request's arena, so freeing the request
     * leaves it alone: the arena releases it wholesale. */
    int arena_backed;
    char* name;
    char* value;
    struct HttpHeader* next;
} HttpHeader;

struct HttpClientRequest {
    /* Optional. When set, header nodes are bump-allocated from it and freed
     * by whoever owns it, not by http_request_free_raw. */
    struct HttpArena* arena;
    /* The struct came out of that arena, so it must not be handed to free():
     * the allocator never gave it out. `method_owned` and `url_owned` say the
     * same of those two pointers separately, because the redirect path
     * replaces the URL with one it allocated itself. */
    int   arena_backed;
    int   method_owned;
    int   url_owned;
    char* method;        /* "GET", "POST", etc. — owned, NUL-terminated */
    char* url;           /* full URL — owned, NUL-terminated */
    HttpHeader* headers; /* singly-linked, in insertion order */
    HttpHeader* headers_tail; /* last node, so insertion is O(1) (#1739) */
    char* body;          /* may be NULL; binary-safe via body_len */
    int   body_len;      /* explicit length; 0 if no body */
    char* content_type;  /* may be NULL; defaults applied at send time */
    int64_t timeout_ns;  /* 0 = no timeout (block forever). Whole-ns
                          * precision; the platform timeout calls
                          * (select tv, SO_RCVTIMEO timeval, Winsock
                          * DWORD ms) slice this down to whatever
                          * granularity they support. */
    int   max_redirects; /* 0 = don't follow (default); N>0 = follow up to N hops */
    int   insecure;      /* 0 = verify peer cert + hostname (default); 1 = skip
                          * both for THIS connection only (curl -k equivalent).
                          * Relaxed per-SSL, never on the shared SSL_CTX. */
    char* cafile;        /* owned, NUL-terminated PEM path, or NULL. When set
                          * (and !insecure), verify the peer against THIS CA
                          * bundle instead of the system store — pin a private/
                          * self-signed CA while KEEPING peer + hostname
                          * verification on. Applied per-connection via a
                          * per-SSL X509_STORE, never on the shared SSL_CTX
                          * (#1107). */
    int   stream;        /* 0 = buffer the whole body (default); 1 = streaming:
                          * read only the header block and hand the open
                          * transport to an HttpStream for incremental reads (#1004). */
    /* Forward-proxy control. Default is DIRECT (no proxy, env ignored) — the
     * hardened stance that avoids the httpoxy (CVE-2016-5385) ambient-authority
     * footgun. Precedence, highest first:
     *   PROXY_MODE_IGNORE (3)   force direct, ignore everything
     *   PROXY_MODE_EXPLICIT (2) use proxy_url, env ignored entirely
     *   PROXY_MODE_ENV (1)      follow HTTP(S)_PROXY/NO_PROXY, guarded
     *   PROXY_MODE_DIRECT (0)   default: no proxy */
    int   proxy_mode;
    char* proxy_url;     /* owned; set only for PROXY_MODE_EXPLICIT */
};

/* Proxy-mode ladder (see struct HttpClientRequest.proxy_mode). */
#define PROXY_MODE_DIRECT   0
#define PROXY_MODE_ENV      1
#define PROXY_MODE_EXPLICIT 2
#define PROXY_MODE_IGNORE   3

HttpClientRequest* http_request_raw(const char* method, const char* url) {
    if (!url || !*url) return NULL;
    /* The method is a token and the URL cannot carry a line ending, for the
     * same reason a header value cannot: both are written into the request
     * line, so a CR LF in either ends that line early and everything after it
     * is read by the peer as headers of its own (CWE-93). A URL built from
     * anything a user supplied is the ordinary way this happens. */
    if (!http_header_name_ok(method)) return NULL;
    if (!http_header_value_ok(url)) return NULL;
    HttpClientRequest* req = (HttpClientRequest*)calloc(1, sizeof(HttpClientRequest));
    if (!req) return NULL;
    req->method = strdup(method);
    req->url    = strdup(url);
    req->method_owned = 1;
    req->url_owned    = 1;
    if (!req->method || !req->url) {
        http_request_free_raw(req);
        return NULL;
    }
    return req;
}

HttpClientRequest* http_request_raw_arena(const char* method, const char* url,
                                          HttpArena* arena) {
    if (!url || !*url) return NULL;
    if (!http_header_name_ok(method)) return NULL;
    if (!http_header_value_ok(url)) return NULL;

    size_t ml = strlen(method), ul = strlen(url);
    char* block = arena ? (char*)http_arena_alloc(
                      arena, sizeof(HttpClientRequest) + ml + 1 + ul + 1)
                        : NULL;
    if (!block) {
        HttpClientRequest* req = http_request_raw(method, url);
        if (req) req->arena = arena;
        return req;
    }

    HttpClientRequest* req = (HttpClientRequest*)block;
    memset(req, 0, sizeof(*req));
    req->arena = arena;
    req->arena_backed = 1;
    req->method = block + sizeof(HttpClientRequest);
    memcpy(req->method, method, ml + 1);
    req->url = req->method + ml + 1;
    memcpy(req->url, url, ul + 1);
    return req;
}

/* ---- the outbound-request arena ---- */

int http_arena_init(HttpArena* a, size_t cap) {
    if (!a) return -1;
    a->block = (char*)malloc(cap);
    if (!a->block) { a->cap = a->used = 0; a->overflowed = 0; return -1; }
    a->cap = cap;
    a->used = 0;
    a->overflowed = 0;
    return 0;
}

/* Bump, aligned for anything this allocates. Returns NULL when the request
 * does not fit, and the caller falls back to malloc for that one: a header
 * block larger than the arena is a request worth serving, not worth failing. */
void* http_arena_alloc(HttpArena* a, size_t n) {
    if (!a || !a->block) return NULL;
    size_t aligned = (n + (sizeof(void*) - 1)) & ~(sizeof(void*) - 1);
    if (a->used + aligned > a->cap) { a->overflowed = 1; return NULL; }
    void* p = a->block + a->used;
    a->used += aligned;
    return p;
}

size_t http_arena_avail(const HttpArena* a) {
    if (!a || !a->block || a->used > a->cap) return 0;
    return a->cap - a->used;
}

void http_arena_reset(HttpArena* a) {
    if (!a) return;
    a->used = 0;
    a->overflowed = 0;
}

void http_arena_free(HttpArena* a) {
    if (!a) return;
    free(a->block);
    a->block = NULL;
    a->cap = a->used = 0;
}

void http_request_use_arena(HttpClientRequest* req, HttpArena* arena) {
    if (req) req->arena = arena;
}

int http_request_set_header_raw(HttpClientRequest* req, const char* name, const char* value) {
    if (!req) return -1;
    if (!http_header_name_ok(name) || !http_header_value_ok(value)) return -1;
    size_t nl = strlen(name), vl = strlen(value);
    size_t need = sizeof(HttpHeader) + nl + 1 + vl + 1;
    /* One allocation holds the node, the name and the value. From the arena
     * when this request has one, which makes it a pointer bump. */
    HttpHeader* h = req->arena ? (HttpHeader*)http_arena_alloc(req->arena, need) : NULL;
    int from_arena = h != NULL;
    if (!h) h = (HttpHeader*)malloc(need);
    if (!h) return -1;
    h->arena_backed = from_arena;
    h->name = (char*)(h + 1);
    memcpy(h->name, name, nl + 1);
    h->value = h->name + nl + 1;
    memcpy(h->value, value, vl + 1);
    h->next = NULL;
    /* Append at the tail so emission order matches insertion order;
     * keeps tests deterministic and avoids surprises with servers
     * that care about header ordering (rare but exists). The tail is
     * tracked rather than walked to: appending N headers by walking cost
     * N(N+1)/2 traversals for a list that is only ever appended to. */
    if (req->headers_tail) {
        req->headers_tail->next = h;
    } else {
        req->headers = h;
    }
    req->headers_tail = h;
    return 0;
}

int http_request_set_body_raw(HttpClientRequest* req, const char* body, int len, const char* content_type) {
    if (!req || len < 0) return -1;
    /* Replace any prior body. Cap-aware (#343): req->body_len holds
     * the original allocation size and is reset to 0 below before
     * the next aether_caps_malloc. */
    aether_caps_free(req->body, (size_t)req->body_len);
    req->body = NULL; req->body_len = 0;
    free(req->content_type); req->content_type = NULL;
    if (len > 0) {
        if (!body) return -1;
        /* `body` may be either an AetherString* (when the caller
         * passed an Aether string variable — common for binary
         * payloads from fs.read_binary) or a plain char* (string
         * literals). Without this unwrap, a 10-byte AetherString
         * input would copy the 24-byte struct header (magic +
         * refcount + length + capacity + data-ptr) into our body
         * buffer, and the wire would carry that header. Same shape
         * the std.fs / std.cryptography / std.zlib externs use. */
        /* Raw bytes: std.http.client.set_body passes a byte[] slice's data
         * pointer through a `ptr`-typed extern (#2301). This used to check
         * for an AetherString header, but on raw bytes that misreads a
         * payload beginning DE C0 57 AE (the header magic) as a header and
         * copies from a pointer taken out of the bytes that follow. */
        const char* src = body;
        /* Cap-aware (#343): caller-supplied length, untrusted on
         * --emit=lib paths. The matching aether_caps_free passes
         * req->body_len as the size. */
        req->body = (char*)aether_caps_malloc((size_t)len);
        if (!req->body) return -1;
        memcpy(req->body, src, (size_t)len);
        req->body_len = len;
    }
    if (content_type) {
        req->content_type = strdup(content_type);
        if (!req->content_type) return -1;
    }
    return 0;
}

int http_request_set_timeout_raw(HttpClientRequest* req, int seconds) {
    if (!req || seconds < 0) return -1;
    req->timeout_ns = (int64_t)seconds * 1000000000LL;
    return 0;
}

int http_request_set_timeout_ns_raw(HttpClientRequest* req, int64_t timeout_ns) {
    if (!req || timeout_ns < 0) return -1;
    req->timeout_ns = timeout_ns;
    return 0;
}

int http_request_set_follow_redirects_raw(HttpClientRequest* req, int max_hops) {
    if (!req || max_hops < 0) return -1;
    req->max_redirects = max_hops;
    return 0;
}

/* Skip TLS peer + hostname verification for this request only (curl -k /
 * wget --no-check-certificate). `on` non-zero enables it; 0 (the default)
 * verifies. The relaxation is applied per-SSL in the TLS path below, never on
 * the process-wide SSL_CTX singleton — so one insecure request cannot silently
 * downgrade verification for every other request in the process. */
int http_request_set_insecure_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->insecure = on ? 1 : 0;
    return 0;
}

/* Pin a custom CA for THIS request: verify the peer certificate against the PEM
 * bundle at `path` instead of the system trust store, while keeping peer and
 * hostname verification ON (#1107). This is the "verify, but against THIS cert"
 * knob — strictly stronger than set_insecure, for machine-to-machine calls to a
 * host with a private/self-signed CA (e.g. a Proxmox VE API). Applied
 * per-connection in the TLS path via a per-SSL X509_STORE, never on the shared
 * SSL_CTX. Passing NULL/empty clears the pin (revert to the system store).
 * `path` is copied. Returns 0, or -1 on a null request. */
int http_request_set_cafile_raw(HttpClientRequest* req, const char* path) {
    if (!req) return -1;
    free(req->cafile);
    req->cafile = (path && *path) ? strdup(path) : NULL;
    return 0;
}

/* #1004: enable streaming response bodies for THIS request. When on, the
 * response returned by http_send_raw carries an open HttpStream instead of a
 * buffered body; the caller pulls the body via http_response_read_chunk_raw
 * and must free the response (which closes the transport) when done. */
int http_request_set_stream_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->stream = on ? 1 : 0;
    return 0;
}

/* ---- Forward-proxy control (aether#1012 part 2) ------------------------------
 *
 * Default is DIRECT: std.http.client does NOT follow $HTTP_PROXY unless the
 * program opts in. This is the deliberate inverse of Go/PHP/Python's original
 * default-follow, which produced the httpoxy vulnerability class
 * (CVE-2016-5385): a CGI/serverless environment maps an attacker-controlled
 * `Proxy:` request header to $HTTP_PROXY, silently redirecting outbound
 * traffic. Hardened-by-default means there is no ambient env-follow path to
 * exploit; env-following is an explicit verb, not a default anyone falls into.
 *
 * Precedence (highest first): ignore > explicit > env > direct. */

/* use_env_proxy: follow $HTTP_PROXY/$HTTPS_PROXY/$NO_PROXY (Go-compatible),
 * WITH the httpoxy + SSRF guards applied at connect time (see
 * resolve_proxy_for). `on` non-zero enables; 0 reverts to direct. */
int http_request_use_env_proxy_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->proxy_mode = on ? PROXY_MODE_ENV : PROXY_MODE_DIRECT;
    return 0;
}

/* use_http_proxy: pin an explicit proxy (`http://host:port`). This OVERRIDES
 * env entirely — $HTTP_PROXY is never consulted — so a team-controlled proxy
 * (recorder / toxiproxy) is immune to whatever the shell/CI has set. Passing
 * an empty/NULL url reverts to direct. */
int http_request_use_http_proxy_raw(HttpClientRequest* req, const char* proxy_url) {
    if (!req) return -1;
    free(req->proxy_url);
    req->proxy_url = NULL;
    if (!proxy_url || !*proxy_url) {
        req->proxy_mode = PROXY_MODE_DIRECT;
        return 0;
    }
    req->proxy_url = strdup(proxy_url);
    if (!req->proxy_url) return -1;
    req->proxy_mode = PROXY_MODE_EXPLICIT;
    return 0;
}

/* ignore_http_proxy: force a direct connection regardless of env or any proxy
 * a higher layer set. The determinism escape hatch (e.g. VCR record mode that
 * must capture the origin, not a proxy's view). */
int http_request_ignore_http_proxy_raw(HttpClientRequest* req) {
    if (!req) return -1;
    free(req->proxy_url);
    req->proxy_url = NULL;
    req->proxy_mode = PROXY_MODE_IGNORE;
    return 0;
}

void http_request_free_raw(HttpClientRequest* req) {
    if (!req) return;
    if (req->method_owned) free(req->method);
    if (req->url_owned) free(req->url);
    /* Body was alloc'd via aether_caps_malloc in
     * http_request_set_body_raw; pair the free with the recorded
     * length so cap accounting stays at current-usage. */
    aether_caps_free(req->body, (size_t)req->body_len);
    free(req->content_type);
    free(req->proxy_url);
    free(req->cafile);
    HttpHeader* h = req->headers;
    while (h) {
        HttpHeader* next = h->next;
        /* Arena-backed nodes are released by resetting the arena, which the
         * caller that owns it does. Freeing one here would hand the allocator
         * a pointer into the middle of a block it never gave out. */
        if (!h->arena_backed) free(h);
        h = next;
    }
    req->headers = NULL;
    req->headers_tail = NULL;
    if (req->arena_backed) return;
    free(req);
}

/* Forward decls — defined below the static request() function. */
static int header_already_set(const HttpClientRequest* req, const char* name);
static char* http_extract_response_header(const char* hdr_block, const char* name);
/* Decode HTTP/1.1 chunked transfer-encoding. Returns a malloc'd decoded
 * buffer (NUL-terminated; *out_len excludes the NUL) or NULL on malformed
 * framing (caller then keeps the raw body). Binary-safe (copies by
 * length). Defined below. */
static int http_value_has_chunked(const char* v);
static int http_value_has_chunked_n(const char* v, size_t len);
static char* http_extract_response_header(const char* hdr_block, const char* name);

/* Is the chunked body in `buf` complete, i.e. has the terminating zero-size
 * chunk arrived? Walks chunk headers rather than searching for "0\r\n\r\n",
 * which can appear inside chunk data. */
/* How many bytes does the complete chunked body at `buf` occupy, counting its
 * framing and trailers, or 0 when it is not complete yet? A complete body is
 * never zero-length (the terminal chunk alone is five bytes), so 0 is an
 * unambiguous "not yet".
 *
 * The length matters as much as the fact: anything after it belongs to
 * whatever the peer sent next, and a caller that assumes the body runs to the
 * end of what it has read would swallow a pipelined message. */
size_t http_chunked_frame_len(const char* buf, size_t len) {
    size_t off = 0;
    for (;;) {
        const char* line_end = (const char*)memchr(buf + off, '\n', len - off);
        if (!line_end) return 0;
        size_t size_len = (size_t)(line_end - (buf + off));
        char size_buf[32];
        if (size_len >= sizeof(size_buf)) return 0;
        memcpy(size_buf, buf + off, size_len);
        size_buf[size_len] = '\0';
        long long chunk = strtoll(size_buf, NULL, 16);
        if (chunk < 0) return 0;
        off = (size_t)(line_end - buf) + 1;
        if (chunk == 0) {
            /* Trailer section: header lines until an empty one ends it. */
            for (;;) {
                const char* le = (const char*)memchr(buf + off, '\n', len - off);
                if (!le) return 0;
                size_t line_len = (size_t)(le - (buf + off));
                int empty = line_len == 0 || (line_len == 1 && buf[off] == '\r');
                off = (size_t)(le - buf) + 1;
                if (empty) return off;
            }
        }
        off += (size_t)chunk + 2;   /* payload + its trailing CRLF */
        if (off > len) return 0;
    }
}

int http_chunked_complete(const char* buf, size_t len) {
    return http_chunked_frame_len(buf, len) > 0;
}

static void http_socket_set_nonblocking(int fd, int on);

/* The status code from a response header block. */
/* The status code from a status line, or 0 when the line does not carry one.
 *
 * It is exactly three digits (RFC 9112 4). Reading it with atoi instead
 * accepts anything that starts with a digit and wraps on overflow, so an
 * upstream answering "HTTP/1.1 999999999999 Weird" handed the caller a status
 * of -727379969, and a proxy copied that onto the response it sent back. */
static int response_status_of(const char* header_block) {
    const char* sp = strchr(header_block, ' ');
    if (!sp) return 0;
    const unsigned char* d = (const unsigned char*)sp + 1;
    if (!isdigit(d[0]) || !isdigit(d[1]) || !isdigit(d[2])) return 0;
    if (isdigit(d[3])) return 0;
    return (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
}

/* RFC 9110: 1xx, 204 and 304 carry no body, and neither does any response to
 * HEAD, whatever the headers say. */
static int no_body_expected(int status, const char* method) {
    if (method && strcmp(method, "HEAD") == 0) return 1;
    if (status >= 100 && status < 200) return 1;
    return status == 204 || status == 304;
}

// -----------------------------------------------------------------
// Forward-proxy resolution (aether#1012 part 2)
// -----------------------------------------------------------------

/* Is `env` a case-insensitive key present and non-empty? Returns its value or
 * NULL. Checks both cases (HTTP_PROXY and http_proxy). */
static const char* proxy_env(const char* upper, const char* lower) {
    const char* v = getenv(lower);          /* lowercase preferred (curl-compat) */
    if (v && *v) return v;
    v = getenv(upper);
    return (v && *v) ? v : NULL;
}

/* httpoxy (CVE-2016-5385) guard: under a CGI/serverless invocation the
 * `Proxy:` request header is mapped to $HTTP_PROXY, so an attacker can inject
 * a proxy. Detect that context and refuse the UPPERCASE HTTP_PROXY there (the
 * lowercase http_proxy is not settable via the CGI header map, so it stays
 * usable — same split Go's net/http/httpproxy adopted). */
static int running_under_cgi(void) {
    return getenv("REQUEST_METHOD") != NULL || getenv("GATEWAY_INTERFACE") != NULL;
}

/* SSRF hardening: reject a proxy that points at a loopback / link-local /
 * cloud-metadata address, so a stray or hostile env var can't redirect
 * outbound traffic to 127.0.0.1, 169.254.169.254 (IMDS), etc. Returns 1 if the
 * literal IP host is dangerous. Hostnames (not IP literals) pass — resolving
 * and re-checking every A record is a heavier follow-up; the common exploit
 * uses a bare IP. */
static int proxy_host_is_dangerous(const char* host) {
    if (!host || !*host) return 1;
    struct in_addr a4;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        uint32_t h = ntohl(a4.s_addr);
        if ((h >> 24) == 127) return 1;                 /* 127.0.0.0/8 loopback */
        if ((h >> 24) == 0)   return 1;                 /* 0.0.0.0/8 */
        if ((h & 0xFFFF0000u) == 0xA9FE0000u) return 1; /* 169.254.0.0/16 link-local (IMDS) */
        return 0;
    }
#ifdef AF_INET6
    struct in6_addr a6;
    if (inet_pton(AF_INET6, host, &a6) == 1) {
        static const unsigned char loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(&a6, loop, 16) == 0) return 1;       /* ::1 */
        if ((a6.s6_addr[0] & 0xFE) == 0xFC) return 1;   /* fc00::/7 ULA */
        if (a6.s6_addr[0] == 0xFE && (a6.s6_addr[1] & 0xC0) == 0x80) return 1; /* fe80::/10 */
        return 0;
    }
#endif
    return 0; /* a hostname; allowed */
}

/* Does `target_host` match any entry of a NO_PROXY list ("host,.dom,10.0.0.1")?
 * Suffix match on a leading dot; exact otherwise; "*" matches all. */
static int host_in_no_proxy(const char* target_host, const char* no_proxy) {
    if (!no_proxy || !*no_proxy || !target_host) return 0;
    size_t tlen = strlen(target_host);
    const char* p = no_proxy;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t elen = (size_t)(end - start);
        if (elen == 1 && start[0] == '*') return 1;
        if (elen > 0) {
            if (start[0] == '.') {              /* ".example.com" → suffix */
                if (tlen >= elen &&
                    strncasecmp(target_host + (tlen - elen), start, elen) == 0) return 1;
            } else if (elen == tlen &&
                       strncasecmp(target_host, start, elen) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Resolve the effective proxy for a request+target, applying the precedence
 * ladder and all guards. On "use a proxy" writes proxy_host/proxy_port and
 * returns 1; returns 0 for a direct connection; returns -1 with *err set on a
 * rejected/malformed proxy (caller surfaces it). */
static int resolve_proxy_for(HttpClientRequest* req, const char* target_host, int use_tls,
                             char* proxy_host, size_t phost_size, int* proxy_port,
                             const char** err) {
    *err = NULL;
    if (req->proxy_mode == PROXY_MODE_IGNORE || req->proxy_mode == PROXY_MODE_DIRECT) {
        return 0;
    }

    const char* proxy_url = NULL;
    if (req->proxy_mode == PROXY_MODE_EXPLICIT) {
        proxy_url = req->proxy_url;             /* env-immune, no guards beyond parse */
    } else { /* PROXY_MODE_ENV */
        const char* no_proxy = proxy_env("NO_PROXY", "no_proxy");
        if (host_in_no_proxy(target_host, no_proxy)) return 0;
        if (use_tls) {
            proxy_url = proxy_env("HTTPS_PROXY", "https_proxy");
        } else {
            /* httpoxy: only the UPPERCASE HTTP_PROXY is CGI-injectable; refuse
             * it under CGI, still honor lowercase http_proxy. */
            const char* lower = getenv("https_proxy"); (void)lower;
            const char* lp = getenv("http_proxy");
            const char* up = getenv("HTTP_PROXY");
            if (lp && *lp)               proxy_url = lp;
            else if (up && *up && !running_under_cgi()) proxy_url = up;
            else                          proxy_url = NULL;
        }
        if (!proxy_url) return 0;
    }

    /* Parse proxy URL: [http://]host[:port]. Default port 3128. */
    const char* s = proxy_url;
    if (strncmp(s, "http://", 7) == 0) s += 7;
    else if (strncmp(s, "https://", 8) == 0) s += 8; /* proxy-over-TLS not supported; treat as host */
    int pport = 3128;
    const char* slash = strchr(s, '/');
    const char* colon = strchr(s, ':');
    size_t hlen;
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - s);
        pport = atoi(colon + 1);
    } else if (slash) {
        hlen = (size_t)(slash - s);
    } else {
        hlen = strlen(s);
    }
    if (hlen == 0 || hlen >= phost_size || pport <= 0 || pport > 65535) {
        *err = "malformed proxy URL";
        return -1;
    }
    memcpy(proxy_host, s, hlen);
    proxy_host[hlen] = '\0';

    /* SSRF guard applies to the ENV path (untrusted); an explicit
     * use_http_proxy is a code-visible grant, so it is trusted as-is. */
    if (req->proxy_mode == PROXY_MODE_ENV && proxy_host_is_dangerous(proxy_host)) {
        *err = "proxy from environment points at a loopback/link-local address (blocked)";
        return -1;
    }
    *proxy_port = pport;
    return 1;
}

// -----------------------------------------------------------------
// Core request, operating on an HttpClientRequest. v1 wrappers build a
// throwaway HttpClientRequest and discard it after send.
// -----------------------------------------------------------------

/* Send / receive timeouts for this request.
 *
 * SO_RCVTIMEO / SO_SNDTIMEO take different shapes on the two families: POSIX
 * a `struct timeval`, Winsock a DWORD of milliseconds. Passing a timeval to
 * Winsock makes it read tv_sec as milliseconds, so `set_timeout(35)` would
 * become a 35ms recv timeout that fires before any upstream can answer.
 * Both shapes keep sub-second precision; Winsock rounds up so a sub-ms value
 * cannot land on 0, which it reads as "infinite". */
static void http_apply_timeouts(int sockfd, int64_t timeout_ns) {
    if (timeout_ns < 0) timeout_ns = 0;   /* 0 clears: block indefinitely */
#ifdef _WIN32
    int64_t ms_total = (timeout_ns + 999999LL) / 1000000LL;
    if (ms_total > 0xFFFFFFFFLL) ms_total = 0xFFFFFFFFLL;
    DWORD rwtv_ms = (DWORD)ms_total;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
#else
    struct timeval rwtv;
    rwtv.tv_sec  = (time_t)(timeout_ns / 1000000000LL);
    rwtv.tv_usec = (long)((timeout_ns / 1000LL) % 1000000LL);
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv, sizeof(rwtv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv, sizeof(rwtv));
#endif
}

/* Apply timeouts only when they differ from what the socket already carries.
 *
 * The unguarded http_apply_timeouts stays for the dial path, where the socket
 * is new and its option state is genuinely unknown. This form is for the reuse
 * path, where the answer is usually "already correct" -- see the comment on
 * Transport::applied_timeout_ns for the measurement. */
static void transport_apply_timeouts(Transport* t, int64_t timeout_ns) {
    int64_t want = timeout_ns < 0 ? 0 : timeout_ns;
    if (t->applied_timeout_ns == want) return;
    http_apply_timeouts(t->sockfd, want);
    t->applied_timeout_ns = want;
}

/* Where the proxy event loop runs (POSIX with threads), and so where its
 * resolver does (#3267). */
#if !defined(_WIN32) && !defined(__wasi__) && !defined(__EMSCRIPTEN__) && AETHER_HAS_THREADS
#define HTTP_RESOLVER 1
#else
#define HTTP_RESOLVER 0
#endif

#if HTTP_RESOLVER
/* The proxy event loop's name lookups (#3267, #3275, #3276, #3277).
 *
 * getaddrinfo blocks, for seconds when a name does not exist or the resolver
 * is slow, and a driver thread holds many connections, so it may not wait on
 * one. The loop asks here instead (http_resolver_answer). A lookup it has to
 * wait for runs on a thread of this resolver's while the driver goes on with
 * its other connections, and the answer wakes it (`answered`) for the dial
 * to go ahead; the connection stays on the loop. Dials waiting on one name
 * share one lookup.
 *
 * Answers are kept, so a name is looked up once and not on every dial. One
 * older than HTTP_RESOLVE_VALID_MS is still used, and looked up again behind
 * the dial, so a changed record is seen about that soon with no dial waiting
 * on it; one older than HTTP_RESOLVE_KEEP_MS is too old to use, and the dial
 * waits for a fresh one, unless the last lookup failed: while lookups of the
 * name fail, the last answer stays in use up to HTTP_RESOLVE_STALE_MS old,
 * a DNS outage served stale as RFC 8767 serves it (#3286). A lookup that
 * says the name does not exist (aether_addr_resolve's -2) is no outage: the
 * answer goes with it. A name with no answer fails its dials for
 * HTTP_RESOLVE_NEGATIVE_MS, then is asked again. At most HTTP_RESOLVE_MAX
 * names are kept, the least recently used given up first, in a hash table:
 * two names never take each other's place. A lookup waits for a thread only
 * while all HTTP_RESOLVE_THREADS are busy (#3284).
 *
 * A resolver belongs to one loop and lives as long as it: its threads start
 * when a lookup first has to wait, and stop with the loop. Nothing else uses
 * it, so a program that does not run the loop starts no thread and makes no
 * lookup it did not ask for (#3276), and adding an upstream looks nothing up
 * (#3277). POSIX only, as the loop is. */
#define HTTP_RESOLVE_VALID_MS    10000
#define HTTP_RESOLVE_KEEP_MS     300000
#define HTTP_RESOLVE_STALE_MS    86400000  /* a day; RFC 8767 suggests one to three */
#define HTTP_RESOLVE_NEGATIVE_MS 1000
#define HTTP_RESOLVE_MAX         1024
#define HTTP_RESOLVE_BUCKETS     1024      /* a power of two */
#define HTTP_RESOLVE_THREADS     4

enum { RESOLVE_IDLE, RESOLVE_QUEUED, RESOLVE_ASKING };

typedef struct HttpResolveName {
    struct HttpResolveName* chain;         /* the rest of its bucket */
    struct HttpResolveName* newer;         /* use order, the newest first */
    struct HttpResolveName* older;
    struct HttpResolveName* queue_next;    /* queued for a thread */
    char*          host;                   /* fixed once made */
    int            port;
    unsigned       hash;
    int            state;                  /* RESOLVE_* */
    int            waited;                 /* a dial waits on this lookup */
    int            failed;                 /* the last lookup failed */
    int64_t        at_ms;                  /* answered; 0: never */
    int64_t        tried_ms;               /* the last lookup ended */
    AetherAddrList addrs;
} HttpResolveName;

struct HttpResolver {
    pthread_mutex_t  lock;
    pthread_cond_t   work;
    HttpResolveName* buckets[HTTP_RESOLVE_BUCKETS];
    HttpResolveName* newest;
    HttpResolveName* oldest;
    int              count;
    HttpResolveName* queue_head;
    HttpResolveName* queue_tail;
    int              queued;          /* names on the queue */
    pthread_t        threads[HTTP_RESOLVE_THREADS];
    int              thread_count;
    int              idle_threads;    /* waiting for work, a signalled one too */
    int              stopping;
    int            (*lookup)(const char* host, int port, AetherAddrList* out);
    int64_t        (*now)(void);     /* http_now_ms; a test's clock (http_resolver_set_clock) */
    void           (*answered)(void* ctx, unsigned name);
    void*            ctx;
};

static int resolve_lookup_default(const char* host, int port, AetherAddrList* out) {
    return aether_addr_resolve(host, port, out);
}

static unsigned resolve_hash(const char* host, int port) {
    unsigned h = 2166136261u;
    for (const char* p = host; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return (h ^ (unsigned)port) * 16777619u;
}

unsigned http_resolver_name(const char* host, int port) {
    return host ? resolve_hash(host, port) : 0;
}

static HttpResolveName** resolve_bucket(HttpResolver* r, unsigned hash) {
    return &r->buckets[hash & (HTTP_RESOLVE_BUCKETS - 1)];
}

static HttpResolveName* resolve_find(HttpResolver* r, const char* host, int port,
                                     unsigned hash) {
    for (HttpResolveName* n = *resolve_bucket(r, hash); n; n = n->chain)
        if (n->hash == hash && n->port == port && strcmp(n->host, host) == 0) return n;
    return NULL;
}

static void resolve_unlink_use(HttpResolver* r, HttpResolveName* n) {
    if (n->newer) n->newer->older = n->older; else r->newest = n->older;
    if (n->older) n->older->newer = n->newer; else r->oldest = n->newer;
    n->newer = n->older = NULL;
}

static void resolve_link_newest(HttpResolver* r, HttpResolveName* n) {
    n->newer = NULL;
    n->older = r->newest;
    if (r->newest) r->newest->newer = n; else r->oldest = n;
    r->newest = n;
}

/* Room for one more name: the least recently used that no lookup holds goes.
 * 0 when every name is held by one. Locked. */
static int resolve_make_room(HttpResolver* r) {
    if (r->count < HTTP_RESOLVE_MAX) return 1;
    for (HttpResolveName* n = r->oldest; n; n = n->newer) {
        if (n->state != RESOLVE_IDLE) continue;
        HttpResolveName** pp = resolve_bucket(r, n->hash);
        while (*pp != n) pp = &(*pp)->chain;
        *pp = n->chain;
        resolve_unlink_use(r, n);
        free(n->host);
        free(n);
        r->count--;
        return 1;
    }
    return 0;
}

static void* resolve_main(void* arg);

/* Queue `n` for a lookup, starting a thread when the names queued would
 * outnumber the threads free to take them: a signalled thread counts as free
 * until it wakes, so "none free" started too few, and a second name waited
 * behind a slow first with thread slots to spare (#3284). 0 when no thread
 * runs to take it. Locked. */
static int resolve_ask(HttpResolver* r, HttpResolveName* n) {
    if (r->queued + 1 > r->idle_threads && r->thread_count < HTTP_RESOLVE_THREADS &&
        pthread_create(&r->threads[r->thread_count], NULL, resolve_main, r) == 0)
        r->thread_count++;
    if (r->thread_count == 0) return 0;
    n->state = RESOLVE_QUEUED;
    n->queue_next = NULL;
    if (r->queue_tail) r->queue_tail->queue_next = n; else r->queue_head = n;
    r->queue_tail = n;
    r->queued++;
    pthread_cond_signal(&r->work);
    return 1;
}

/* A resolver thread: the queued names, one lookup at a time, each outside
 * the lock. A name being looked up is never given up (resolve_make_room) and
 * its host never changes, so the lookup reads it unlocked. */
static void* resolve_main(void* arg) {
    HttpResolver* r = (HttpResolver*)arg;
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->queue_head && !r->stopping) {
            r->idle_threads++;
            pthread_cond_wait(&r->work, &r->lock);
            r->idle_threads--;
        }
        if (r->stopping) break;
        HttpResolveName* n = r->queue_head;
        r->queue_head = n->queue_next;
        if (!r->queue_head) r->queue_tail = NULL;
        r->queued--;
        n->queue_next = NULL;
        n->state = RESOLVE_ASKING;
        int (*lookup)(const char*, int, AetherAddrList*) = r->lookup;
        pthread_mutex_unlock(&r->lock);

        AetherAddrList addrs;
        int rc = lookup(n->host, n->port, &addrs);
        int ok = rc == 0 && addrs.n > 0;

        pthread_mutex_lock(&r->lock);
        int64_t now = r->now();
        if (ok) {
            n->addrs = addrs;
            n->at_ms = now;
        } else if (rc == -2) {
            n->at_ms = 0;           /* the name is gone, and its answer with it */
        }
        n->failed = !ok;
        n->tried_ms = now;
        n->state = RESOLVE_IDLE;
        int wake = n->waited;
        unsigned name = n->hash;
        n->waited = 0;
        if (wake && r->answered) {
            /* Outside the lock: the loop's side takes its own. By its hash,
             * not the name itself, which another thread may give up once
             * the lock is let go (resolve_make_room). */
            pthread_mutex_unlock(&r->lock);
            r->answered(r->ctx, name);
            pthread_mutex_lock(&r->lock);
        }
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

HttpResolver* http_resolver_new(void (*answered)(void* ctx, unsigned name), void* ctx) {
    HttpResolver* r = (HttpResolver*)calloc(1, sizeof(HttpResolver));
    if (!r) return NULL;
    if (pthread_mutex_init(&r->lock, NULL) != 0) { free(r); return NULL; }
    if (pthread_cond_init(&r->work, NULL) != 0) {
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }
    r->lookup = resolve_lookup_default;
    r->now = http_now_ms;
    r->answered = answered;
    r->ctx = ctx;
    return r;
}

void http_resolver_set_lookup(HttpResolver* r,
                              int (*lookup)(const char* host, int port, AetherAddrList* out)) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->lookup = lookup ? lookup : resolve_lookup_default;
    pthread_mutex_unlock(&r->lock);
}

void http_resolver_set_clock(HttpResolver* r, int64_t (*now)(void)) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->now = now ? now : http_now_ms;
    pthread_mutex_unlock(&r->lock);
}

void http_resolver_free(HttpResolver* r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    r->stopping = 1;
    pthread_cond_broadcast(&r->work);
    pthread_mutex_unlock(&r->lock);
    /* A lookup under way ends first: getaddrinfo cannot be cut short. Nobody
     * asks any more (the loop's drivers stopped first), so the count holds. */
    for (int i = 0; i < r->thread_count; i++) pthread_join(r->threads[i], NULL);
    for (int b = 0; b < HTTP_RESOLVE_BUCKETS; b++) {
        HttpResolveName* n = r->buckets[b];
        while (n) {
            HttpResolveName* next = n->chain;
            free(n->host);
            free(n);
            n = next;
        }
    }
    pthread_cond_destroy(&r->work);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static int resolve_numeric(const char* host, int port, AetherAddrList* out);

int http_resolver_answer(HttpResolver* r, const char* host, int port,
                         AetherAddrList* out) {
    if (!r || !host || !out) return -1;
    if (resolve_numeric(host, port, out)) return 1;
    unsigned hash = resolve_hash(host, port);
    int got;
    pthread_mutex_lock(&r->lock);
    int64_t now = r->now();
    HttpResolveName* n = resolve_find(r, host, port, hash);
    if (n) {
        if (r->newest != n) {
            resolve_unlink_use(r, n);
            resolve_link_newest(r, n);
        }
    } else {
        char* copy = strdup(host);
        if (copy && resolve_make_room(r)) n = (HttpResolveName*)calloc(1, sizeof(*n));
        if (!n) {
            free(copy);
            pthread_mutex_unlock(&r->lock);
            return -1;
        }
        n->host = copy;
        n->port = port;
        n->hash = hash;
        HttpResolveName** b = resolve_bucket(r, hash);
        n->chain = *b;
        *b = n;
        resolve_link_newest(r, n);
        r->count++;
    }
    /* The last answer, unless it is too old: past HTTP_RESOLVE_KEEP_MS while
     * lookups go through, past HTTP_RESOLVE_STALE_MS while they fail
     * (#3286). A name that does not exist has none (resolve_main). */
    int64_t age = now - n->at_ms;
    if (n->at_ms && (age < HTTP_RESOLVE_KEEP_MS ||
                     (n->failed && age < HTTP_RESOLVE_STALE_MS))) {
        *out = n->addrs;
        got = 1;
        /* Due again: looked up behind this dial, which goes ahead. */
        if (n->state == RESOLVE_IDLE && now - n->tried_ms >= HTTP_RESOLVE_VALID_MS)
            resolve_ask(r, n);
    } else if (n->state == RESOLVE_IDLE && n->failed &&
               now - n->tried_ms < HTTP_RESOLVE_NEGATIVE_MS) {
        got = -1;
    } else if (n->state == RESOLVE_IDLE && !resolve_ask(r, n)) {
        got = -1;                        /* no thread to look it up on */
    } else {
        n->waited = 1;
        got = 0;
    }
    pthread_mutex_unlock(&r->lock);
    return got;
}

/* `host`:`port` into `out` without asking a resolver: a lookup with
 * AI_NUMERICHOST only parses. 1 for a numeric address, else 0. */
static int resolve_numeric(const char* host, int port, AetherAddrList* out) {
    char bare[256];
    out->n = 0;
    if (!aether_addr_unbracket(host, bare, sizeof bare) || !bare[0]) return 0;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST;
    if (getaddrinfo(bare, port_str, &hints, &res) == 0 && res) {
        for (struct addrinfo* r = res; r && out->n < AETHER_ADDR_MAX; r = r->ai_next) {
            if (r->ai_family != AF_INET && r->ai_family != AF_INET6) continue;
            if ((size_t)r->ai_addrlen > sizeof out->ss[0]) continue;
            memset(&out->ss[out->n], 0, sizeof out->ss[0]);
            memcpy(&out->ss[out->n], r->ai_addr, r->ai_addrlen);
            out->len[out->n] = (socklen_t)r->ai_addrlen;
            out->n++;
        }
        freeaddrinfo(res);
    }
    return out->n > 0;
}
#else
HttpResolver* http_resolver_new(void (*answered)(void* ctx, unsigned name), void* ctx) {
    (void)answered; (void)ctx;
    return NULL;
}
void http_resolver_set_lookup(HttpResolver* r,
                              int (*lookup)(const char* host, int port, AetherAddrList* out)) {
    (void)r; (void)lookup;
}
void http_resolver_free(HttpResolver* r) { (void)r; }
void http_resolver_set_clock(HttpResolver* r, int64_t (*now)(void)) { (void)r; (void)now; }
unsigned http_resolver_name(const char* host, int port) {
    (void)host; (void)port;
    return 0;
}
int http_resolver_answer(HttpResolver* r, const char* host, int port,
                         AetherAddrList* out) {
    (void)r; (void)host; (void)port;
    if (out) out->n = 0;
    return -1;
}
#endif

/* Resolve `host`:`port` into `out`, at most once per request (#1719).
 *
 * `*resolved` is the caller's once-flag: 0 on entry means "not yet", and it is
 * set on success so a second call is free. Returns 1 on success (including the
 * cached case), 0 when the name does not resolve.
 *
 * Resolve via getaddrinfo, NOT gethostbyname: gethostbyname returns a pointer
 * into a shared, process-static `struct hostent`, so two client calls resolving
 * at once on different threads -- e.g. a request handler that dials out while
 * serving (serve-and-dial), where the inner call runs on a server worker thread
 * -- race on that static buffer and can corrupt each other's resolved address.
 * getaddrinfo is thread-safe and returns caller-owned memory freed with
 * freeaddrinfo. Every address the name has is kept, IPv6 and IPv4 alike, in
 * getaddrinfo's preference order, so the dial can try each in turn.
 *
 * A failed resolve leaves *resolved at 0, so a later caller retries rather than
 * reading an uninitialised address. */
static int resolve_dial_addr(const char* host, int port,
                             AetherAddrList* out, int* resolved) {
    if (*resolved) return 1;
    if (aether_addr_resolve(host, port, out) != 0) return 0;
    *resolved = 1;
    return 1;
}

/* Set *err to a heap copy of `msg`, for the dial helper's error returns. */
static void ae_set_err(char** out_err, const char* msg) {
    if (!out_err) return;
    *out_err = msg ? strdup(msg) : NULL;
}

/* Open one connection to one of `addrs`: socket, connect (with the request's
 * timeout when set), the forward-proxy CONNECT tunnel for HTTPS, and the TLS
 * handshake. On success `out` holds a ready transport and 0 is returned; on
 * failure nothing is left open, *err holds a message the caller frees, and
 * the return is -1.
 *
 * Split out of http_request_internal so a pooled connection that turns out to
 * be dead can be redialled without duplicating any of this (#1653). */
/* Why HTTPS cannot run in this program, or NULL when it can: the
 * pure-Aether client is selected (AETHER_PURE_TLS=1, or a build without
 * OpenSSL) but not linked. The program's to fix, and known before any
 * network is touched. */
static const char* http_tls_backend_missing(void) {
    if (!http_client_use_pure_tls() || pt_client_available()) return NULL;
#ifdef AETHER_HAS_OPENSSL
    return "AETHER_PURE_TLS selects the pure-Aether TLS client, but it is "
           "not linked into this program. Add "
           "`import std.cryptography.tls13_client` to the program, or "
           "unset AETHER_PURE_TLS to use OpenSSL.";
#else
    return "HTTPS requested but this build has no TLS backend: it was "
           "built without OpenSSL (every --target= cross-build is), and "
           "the pure-Aether client is not linked. Add "
           "`import std.cryptography.tls13_client` to the program.";
#endif
}

static int http_dial(HttpClientRequest* req, AetherAddrList* addrs,
                     const char* dial_host, int dial_port,
                     const char* host, int port, int use_tls, int via_proxy,
                     Transport* out, char** out_err) {
    /* Each address the dial host resolved to, as aether_net_dial_addrs
     * describes: a short first try on all but the last, the request's
     * timeout across all of them, and the family that connected tried first
     * next time. */
    const char* why = NULL;
    int sockfd = aether_net_dial_addrs(dial_host, dial_port, addrs,
                                       req->timeout_ns > 0 ? req->timeout_ns : 0, 0, &why);
    if (sockfd < 0) {
        ae_set_err(out_err, why);
        return -1;
    }

    if (req->timeout_ns > 0) {
        /* Apply send/recv timeouts equal to the configured value.
         *
         * SO_RCVTIMEO / SO_SNDTIMEO take different shapes on the two
         * families:
         *   - POSIX: pointer to `struct timeval` (seconds + microseconds)
         *   - Winsock: pointer to a 32-bit DWORD in milliseconds
         *
         * Passing a struct timeval to Winsock causes it to interpret
         * the first 4 bytes (tv_sec) as a millisecond count — so
         * `set_timeout(35)` would degrade to a 35-millisecond recv
         * timeout, which fires almost instantly and surfaces as
         * "recv timeout or I/O error" before any sane upstream can
         * even respond. Use the right type per platform.
         *
         * Both shapes carry sub-second precision: POSIX gets the full
         * microsecond resolution via tv_usec; Winsock rounds up to
         * the next whole millisecond (sub-ms DWORD=0 is ambiguous
         * with "infinite"). */
        http_apply_timeouts(sockfd, req->timeout_ns);
    }

    out->sockfd = sockfd;
    /* The dial path below applies the timeouts unconditionally on a fresh
     * socket; record the value so a later reuse can skip re-applying it. */
    out->applied_timeout_ns = req->timeout_ns < 0 ? 0 : req->timeout_ns;

    /* HTTPS via a forward proxy: establish a CONNECT tunnel over the raw socket
     * BEFORE the TLS handshake, so TLS runs end-to-end through the proxy (the
     * proxy is a blind pipe; it does not terminate TLS). HTTP via proxy needs no
     * tunnel — it uses an absolute-form request line, handled below. */
    if (via_proxy && use_tls) {
        char creq[512];
        char authority[300];
        aether_addr_authority(authority, sizeof authority, host, port);
        int cn = snprintf(creq, sizeof(creq),
                          "CONNECT %s HTTP/1.1\r\nHost: %s\r\n"
                          "Proxy-Connection: keep-alive\r\n\r\n",
                          authority, authority);
        if (cn <= 0 || cn >= (int)sizeof(creq) ||
            send(sockfd, creq, (size_t)cn, 0) != cn) {
            close(sockfd);
            ae_set_err(out_err, "proxy CONNECT: send failed");
            return -1;
        }
        /* Read the proxy's response headers up to the terminating CRLFCRLF.
         * A 2xx means the tunnel is open; anything else is a proxy refusal. */
        char cbuf[1024];
        size_t clen = 0;
        int saw_end = 0;
        while (clen < sizeof(cbuf) - 1) {
            ssize_t rn = recv(sockfd, cbuf + clen, sizeof(cbuf) - 1 - clen, 0);
            if (rn <= 0) break;
            clen += (size_t)rn;
            cbuf[clen] = '\0';
            if (strstr(cbuf, "\r\n\r\n")) { saw_end = 1; break; }
        }
        if (!saw_end) {
            close(sockfd);
            ae_set_err(out_err, "proxy CONNECT: no response from proxy");
            return -1;
        }
        /* Parse "HTTP/1.x NNN ..." status. */
        int pstatus = 0;
        const char* sp = strchr(cbuf, ' ');
        if (sp) pstatus = atoi(sp + 1);
        if (pstatus < 200 || pstatus >= 300) {
            close(sockfd);
            char emsg[128];
            snprintf(emsg, sizeof(emsg),
                     "proxy CONNECT refused (status %d)", pstatus);
            ae_set_err(out_err, emsg);
            return -1;
        }
        /* Tunnel open. TLS handshake below runs against `host` end-to-end. */
    }

    out->pure_tls = NULL;

    /* The pure-Aether TLS 1.3 client (#1849). Always the backend in a build
     * without OpenSSL -- every `ae build --target=` cross-compile, because zig
     * bundles no TLS -- and selectable with AETHER_PURE_TLS=1 in a build that
     * has it (see http_client_use_pure_tls). The socket is already connected,
     * and already tunnelled when a forward proxy applies, so the handshake
     * runs end-to-end against `host` either way and proxying needs no
     * separate path.
     *
     * Verification is the pure client's own: certificate chain to a trusted
     * anchor, validity window, and hostname/SAN pinned to `host`. set_insecure
     * skips it; set_cafile pins a couriered bundle. */
    if (use_tls && http_client_use_pure_tls()) {
        /* http_request_internal has said so before dialling; a dial from
         * anywhere else must not reach the handshake without a client. */
        const char* missing = http_tls_backend_missing();
        if (missing) {
            close(sockfd);
            ae_set_err(out_err, missing);
            return -1;
        }
        aether_pure_tls_client_set_error(NULL);
        void* pc = pt_client_connect(sockfd, host,
                                     req && req->insecure ? 1 : 0,
                                     req ? req->cafile : NULL);
        if (!pc) {
            /* Not close(sockfd): the descriptor was handed over, and the pure
             * client closes it on every failure path (connect_full's error
             * returns, the adopt itself). Closing it again here closed a
             * NUMBER, which another thread may already have been given for a
             * fresh socket of its own.
             *
             * tls13_client leaves the reason in pt_client_last_err. Only a
             * failure before it got that far (out of memory, an unusable
             * socket) arrives without one. */
            char emsg[640];
            if (pt_client_last_err[0]) {
                snprintf(emsg, sizeof(emsg), "TLS handshake failed (pure-Aether TLS 1.3): %s",
                         pt_client_last_err);
            } else {
                snprintf(emsg, sizeof(emsg),
                         "TLS handshake failed (pure-Aether TLS 1.3): the connection "
                         "could not be set up");
            }
            ae_set_err(out_err, emsg);
            return -1;
        }
        /* The pure connection owns the socket from here; transport_close
         * releases both through aether_pure_tls_client_close. */
        out->pure_tls = pc;
        out->sockfd   = sockfd;
        out->nonblocking = -1;
        out->applied_timeout_ns = -1;
        return 0;
    }

#ifdef AETHER_HAS_OPENSSL
    out->ssl = NULL;
    out->owned_ctx = NULL;

    if (use_tls) {
        /* Custom-CA pin (set_cafile): build a DEDICATED SSL_CTX whose trust
         * store is loaded from the couriered CA, instead of the shared
         * system-store CTX. Loading the CA onto the CTX's own verify store via
         * SSL_CTX_load_verify_locations is the portable, version-agnostic trust
         * idiom — it's exactly what `openssl s_client -CAfile` does, and it
         * verifies identically across OpenSSL 1.1/3.x and LibreSSL. (The prior
         * per-SSL SSL_set1_verify_cert_store approach verified on OpenSSL 3.x
         * but did not reliably become the *trust* store on every TLS library,
         * so a couriered CA that `openssl -CAfile` accepted still failed
         * `certificate verify failed` on some builds — #1110.) The per-request
         * CTX is owned by the transport and freed in transport_close after the
         * SSL. When no cafile is set we keep the shared process-wide CTX. */
        SSL_CTX* ctx;
        if (req->cafile) {
            ctx = SSL_CTX_new(TLS_client_method());
            if (ctx) {
                SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                if (SSL_CTX_load_verify_locations(ctx, req->cafile, NULL) != 1) {
                    SSL_CTX_free(ctx);
                    close(sockfd);
                    char* msg = ssl_err_string("custom CA (set_cafile) load failed");
                    ae_set_err(out_err, msg ? msg : "custom CA load failed");
                    free(msg);
                    return -1;
                }
            }
            out->owned_ctx = ctx;   /* transport frees it (NULL is a no-op) */
        } else {
            ctx = get_ssl_ctx();
        }
        if (!ctx) {
            close(sockfd);
            char* msg = ssl_err_string("TLS context init failed");
            ae_set_err(out_err, msg ? msg : "TLS context init failed");
            free(msg);
            return -1;
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            if (out->owned_ctx) { SSL_CTX_free(out->owned_ctx); out->owned_ctx = NULL; }
            close(sockfd);
            char* msg = ssl_err_string("SSL_new failed");
            ae_set_err(out_err, msg ? msg : "SSL_new failed");
            free(msg);
            return -1;
        }

        // SNI: server-name indication so virtual-hosted TLS services
        // return the right cert.
        SSL_set_tlsext_host_name(ssl, host);

        if (req->insecure) {
            // Per-request opt-out (set_insecure): skip peer verification AND
            // the hostname pin for THIS connection only. The shared SSL_CTX
            // (which set SSL_VERIFY_PEER) is untouched, so every other request
            // still verifies. Mirrors curl -k / wget --no-check-certificate.
            SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
        } else {
            // Verify the cert's CN/SAN matches the host we connected to. For an
            // IP literal (e.g. a Proxmox API at https://192.168.0.204:8006),
            // set1_ip_asc pins the IP SAN; for a DNS name, set1_host pins the
            // DNS SAN/CN. Using the IP-specific call for IP literals is correct
            // across OpenSSL versions (older set1_host did not auto-detect IPs).
            // The trust anchor for THIS connection is either the shared system
            // store or, when set_cafile was called, the couriered CA loaded onto
            // the per-request CTX above (#1107/#1110) — peer + hostname
            // verification stay ON either way, so a pinned request is strictly
            // stronger than set_insecure and fails closed if the CA doesn't
            // cover the presented cert.
            X509_VERIFY_PARAM* vpm = SSL_get0_param(ssl);
            X509_VERIFY_PARAM_set_hostflags(vpm, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            struct in_addr in4;
            struct in6_addr in6;
            if (inet_pton(AF_INET, host, &in4) == 1 ||
                inet_pton(AF_INET6, host, &in6) == 1) {
                X509_VERIFY_PARAM_set1_ip_asc(vpm, host);
            } else {
                X509_VERIFY_PARAM_set1_host(vpm, host, 0);
            }
        }

        SSL_set_fd(ssl, sockfd);
        int connect_result = SSL_connect(ssl);
        if (connect_result != 1) {
            int ssl_err = SSL_get_error(ssl, connect_result);
            (void)ssl_err;
            SSL_free(ssl);
            if (out->owned_ctx) { SSL_CTX_free(out->owned_ctx); out->owned_ctx = NULL; }
            close(sockfd);
            char* msg = ssl_err_string("TLS handshake failed");
            ae_set_err(out_err, msg ? msg : "TLS handshake failed");
            free(msg);
            return -1;
        }

        out->ssl = ssl;
    }
#endif
    return 0;
}


/* Find a header by name in a header block, anchored to the start of each line.
 *
 * Anchoring matters: a substring search over the whole block also matches
 * inside another header's value, so a message carrying
 * `X-Note: Content-Length: 9` would have had that read as its framing.
 *
 * Shared by the client and the server: a response frames its body the same way
 * a request does, and two scanners would be two chances to disagree.
 *
 * Returns how many times the header appears, writes the first value into
 * `out`, and sets *differing when two of them disagree. A framing header that
 * appears twice with two answers has no single answer at all.
 */
int http_find_header_span(const char* block, const char* end,
                          const char* name, char* out, size_t out_cap,
                          int* differing,
                          const char** out_v, size_t* out_vl) {
    size_t name_len = strlen(name);
    int count = 0;
    if (differing) *differing = 0;
    if (out && out_cap) out[0] = '\0';

    for (const char* line = block; line && line < end; ) {
        const char* eol = (const char*)memchr(line, '\n', (size_t)(end - line));
        const char* line_end = eol ? eol : end;
        size_t line_len = (size_t)(line_end - line);
        if (line_len && line[line_len - 1] == '\r') line_len--;

        /* Where the colon has to fall, and the first letter, reject almost
         * every line for two loads. strncasecmp is a call that lowercases as
         * it walks, and this scan runs over every header of every response.
         * The first-letter test can only be over-permissive, never wrong: two
         * bytes equal ignoring case always agree on it. */
        if (line_len > name_len && line[name_len] == ':'
            && (((unsigned char)line[0] | 0x20)
                == ((unsigned char)name[0] | 0x20))
            && strncasecmp(line, name, name_len) == 0) {
            const char* v = line + name_len + 1;
            const char* v_end = line + line_len;
            while (v < v_end && (*v == ' ' || *v == '\t')) v++;
            while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) v_end--;
            size_t vl = (size_t)(v_end - v);

            /* The span is the value where it lies, so a caller that only
             * needs to look at it is not limited by any buffer size. */
            if (count == 0) {
                if (out_v)  *out_v  = v;
                if (out_vl) *out_vl = vl;
            }

            if (out || differing) {
                char value[256];
                size_t cv = vl < sizeof(value) - 1 ? vl : sizeof(value) - 1;
                memcpy(value, v, cv);
                value[cv] = '\0';

                if (count == 0) {
                    if (out && out_cap) {
                        size_t copy = cv < out_cap - 1 ? cv : out_cap - 1;
                        memcpy(out, value, copy);
                        out[copy] = '\0';
                    }
                } else if (differing && out && strcmp(out, value) != 0) {
                    *differing = 1;
                }
            }
            count++;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return count;
}

int http_find_header_in_block(const char* block, const char* end,
                            const char* name, char* out, size_t out_cap,
                            int* differing) {
    return http_find_header_span(block, end, name, out, out_cap, differing,
                                 NULL, NULL);
}

/* Has a complete HTTP/1.1 response accumulated in `buf`?
 *
 * Reading to end of connection only works when the connection is the
 * delimiter, which is what made every request cost a fresh one (#1653). This
 * reads to the end of THIS response: the header block, then exactly the body
 * its framing declares. `f` carries what has been worked out so far across
 * calls and starts zeroed.
 *
 * Shared deliberately. A caller that cannot block reads the same bytes in a
 * different order, and a second copy of response framing would be a second
 * place for chunked, Content-Length and the no-body statuses to disagree.
 */

const char* http_find_header_end(const char* buf, size_t len) {
    if (!buf || len < 4) return NULL;
    const char* p = buf;
    /* A match has to start at or before this, so that all four bytes exist. */
    const char* last = buf + len - 3;
    while (p < last) {
        const char* cr = (const char*)memchr(p, '\r', (size_t)(last - p));
        if (!cr) return NULL;
        if (cr[1] == '\n' && cr[2] == '\r' && cr[3] == '\n') return cr;
        p = cr + 1;
    }
    return NULL;
}

static int http_response_is_complete(HttpRespFraming* f, char* buf, size_t len,
                                     const char* method) {
    if (!f->header_bytes) {
        char* hend = (char*)http_find_header_end(buf, len);
        if (!hend) return 0;
        f->header_bytes = (size_t)((hend + 4) - buf);

        /* Read Transfer-Encoding where it lies. Copying it out meant an
         * allocation and a free per response, and any buffer it was copied
         * into would have put a length limit on a header whose contents
         * decide how the body is framed. */
        const char* te = NULL;
        size_t te_len = 0;
        http_find_header_span(buf, hend, "Transfer-Encoding",
                              NULL, 0, NULL, &te, &te_len);
        if (te && http_value_has_chunked_n(te, te_len)) {
            f->chunked = 1;
            f->definite = 1;
        } else {
            /* Anchored, and counted: a response naming two different lengths
             * has not said where its body ends, and picking one leaves the
             * bytes of the other in a connection this client may hand to the
             * next request. */
            char cl[64];
            int differing = 0;
            int count = http_find_header_in_block(buf, hend, "Content-Length",
                                                  cl, sizeof(cl), &differing);
            if (count > 0) {
                if (differing) {
                    f->invalid = 1;
                } else {
                    char* endp = NULL;
                    long long declared = strtoll(cl, &endp, 10);
                    if (endp && *endp == '\0' && endp != cl && declared >= 0) {
                        f->body_target = f->header_bytes + (size_t)declared;
                        f->definite = 1;
                    } else {
                        f->invalid = 1;
                    }
                }
            }
        }
        if (no_body_expected(response_status_of(buf), method)) {
            f->body_target = f->header_bytes;
            f->definite = 1;
            f->chunked = 0;
        }
    }
    if (f->invalid) return 1;      /* stop reading; the driver reports it */
    if (!f->definite) return 0;
    if (f->chunked)
        return http_chunked_complete(buf + f->header_bytes, len - f->header_bytes);
    return len >= f->body_target;
}

/* Serialise a request head into a fresh capability-gated buffer.
 *
 * Shared so that a driver which sends without blocking puts exactly the same
 * bytes on the wire as the blocking one. Returns NULL on allocation failure,
 * with nothing to free; on success the caller owns the buffer and frees it
 * with aether_caps_free(buf, *out_cap).
 */

char* http_build_request_head(const HttpReqHead* p,
                                     size_t* out_len, size_t* out_cap) {
    char*  buf = NULL;
    size_t cap = 0;
    char*  r = http_build_request_head_into(p, &buf, &cap, out_len);
    if (out_cap) *out_cap = cap;
    return r;
}

/* The same builder, over a buffer the caller keeps.
 *
 * A connection serving many requests built this head into a fresh allocation
 * every time and released it a moment later. The head of one request is the
 * same size as the head of the next, so after the first there is nothing to
 * allocate: the buffer is reused at whatever size it reached.
 *
 * `*buf` is NULL on the first call and the caller's afterwards, released with
 * aether_caps_free and `*cap` when the connection ends -- through the cap,
 * because that is how it was taken. */
char* http_build_request_head_into(const HttpReqHead* p,
                                     char** io_buf, size_t* io_cap,
                                     size_t* out_len) {
    size_t hdr_cap = *io_cap;
    char*  hdr     = *io_buf;
    if (!hdr) {
        hdr_cap = 1024;
        /* #461: gate the request-header build buffer through the cap. */
        hdr = (char*)aether_caps_malloc(hdr_cap);
        if (!hdr) return NULL;
    }
    size_t hdr_len = 0;

    /* Helper: append a NUL-terminated string into hdr, growing as
     * needed. Returns 0 on success, -1 on OOM. */
    #define HDR_APPEND_N(s, len) do { \
        const char* _s = (s); \
        size_t _slen = (len); \
        if (hdr_len + _slen + 1 > hdr_cap) { \
            size_t _nc = hdr_cap; \
            while (_nc < hdr_len + _slen + 1) _nc *= 2; \
            char* _nh = (char*)aether_caps_realloc(hdr, hdr_cap, _nc); \
            if (!_nh) { aether_caps_free(hdr, hdr_cap); \
                        *io_buf = NULL; *io_cap = 0; return NULL; } \
            hdr = _nh; hdr_cap = _nc; \
        } \
        memcpy(hdr + hdr_len, _s, _slen); \
        hdr_len += _slen; \
        hdr[hdr_len] = '\0'; \
    } while (0)

    /* A literal's length is known while compiling; measuring it again at run
     * time is work this path takes once per header emitted. */
    #define HDR_APPEND_LIT(s) HDR_APPEND_N("" s, sizeof(s) - 1)
    #define HDR_APPEND_STR(s) do { const char* _t = (s); HDR_APPEND_N(_t, strlen(_t)); } while (0)

    /* Request line. For plain HTTP through a forward proxy, use the absolute
     * form (`GET http://p->host[:p->port]/p->path HTTP/1.1`) so the proxy knows the
     * origin. Direct requests, and HTTPS-through-a-CONNECT-tunnel (which talks
     * end-to-end to the origin), use the origin form (`GET /p->path`). */
    HDR_APPEND_STR(p->method); HDR_APPEND_LIT(" ");
    if (p->via_proxy && !p->use_tls) {
        char absline[1408];
        char authority[300];
        aether_addr_authority(authority, sizeof authority, p->host, p->port == 80 ? -1 : p->port);
        snprintf(absline, sizeof(absline), "http://%s%s", authority, p->path);
        HDR_APPEND_STR(absline);
    } else {
        HDR_APPEND_STR(p->path);
    }
    HDR_APPEND_LIT(" HTTP/1.1\r\n");

    /* Built-in Host (overridable via set_header).
     *
     * The authority, which includes the port whenever it is not the default
     * for the scheme (RFC 9110 7.2). Sending the bare host to a server on
     * another port names a different authority than the one being addressed,
     * which a virtual-hosted server routes on. An IPv6 literal host is
     * bracketed, as in the URL. */
    if (!header_already_set(p->req, "Host")) {
        HDR_APPEND_LIT("Host: ");
        int host_v6 = strchr(p->host, ':') != NULL;
        if (host_v6) HDR_APPEND_LIT("[");
        HDR_APPEND_STR(p->host);
        if (host_v6) HDR_APPEND_LIT("]");
        if (p->port != (p->use_tls ? 443 : 80)) {
            char port_suffix[24];
            size_t pn = 0;
            port_suffix[pn++] = ':';
            pn += http_write_dec(port_suffix + pn, (unsigned long long)p->port);
            HDR_APPEND_N(port_suffix, pn);
        }
        HDR_APPEND_LIT("\r\n");
    }

    /* Built-in Content-Length when p->body present (overridable, but
     * setting it manually is almost always a bug — we still emit
     * ours unless the caller explicitly overrode it). */
    if (p->body && p->body_len > 0 && !header_already_set(p->req, "Content-Length")) {
        char clen[48];
        size_t cn = 16;
        memcpy(clen, "Content-Length: ", 16);
        cn += http_write_dec(clen + cn, (unsigned long long)p->body_len);
        clen[cn++] = '\r'; clen[cn++] = '\n';
        HDR_APPEND_N(clen, cn);
    }

    /* Built-in Content-Type when p->body present, only if neither the
     * builder's p->content_type nor an explicit Content-Type header is set. */
    if (p->body && p->body_len > 0 && p->content_type
        && !header_already_set(p->req, "Content-Type")) {
        HDR_APPEND_LIT("Content-Type: "); HDR_APPEND_STR(p->content_type); HDR_APPEND_LIT("\r\n");
    } else if (p->body && p->body_len > 0 && !p->content_type
        && !header_already_set(p->req, "Content-Type")) {
        HDR_APPEND_LIT("Content-Type: application/x-www-form-urlencoded\r\n");
    }

    /* Persistent by default: the connection goes back to the idle pool when
     * the response framing is definite. A caller who sets their own
     * Connection header still gets exactly that. */
    if (!header_already_set(p->req, "Connection")) {
        HDR_APPEND_STR(p->keep_alive ? "Connection: keep-alive\r\n"
                                 : "Connection: close\r\n");
    }

    /* Caller-provided headers, in insertion order. */
    for (HttpHeader* h = p->req->headers; h; h = h->next) {
        HDR_APPEND_STR(h->name); HDR_APPEND_LIT(": "); HDR_APPEND_STR(h->value); HDR_APPEND_LIT("\r\n");
    }

    /* End-of-headers blank line. */
    HDR_APPEND_LIT("\r\n");

    #undef HDR_APPEND_STR
    #undef HDR_APPEND_LIT
    #undef HDR_APPEND_N

    *out_len = hdr_len;
    /* The buffer goes back to the caller at whatever size it reached, so the
     * next request on this connection builds into it without allocating. */
    *io_buf = hdr;
    *io_cap = hdr_cap;
    return hdr;
}

/* Fill status, headers and body from a complete response buffer.
 *
 * Shared with any driver that accumulates the same bytes without blocking, so
 * de-chunking and the header/body split cannot come out differently depending
 * on who read the socket. `buf` is modified in place: the header terminator is
 * NUL-terminated so the header block can be handed out as a string.
 */
void http_response_fill_from_bytes(HttpResponse* response,
                                          char* buf, size_t len) {
    char* header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        response->body = string_new_with_length(buf, len);
        return;
    }
    size_t header_bytes = (size_t)((header_end + 4) - buf);
    size_t body_bytes = len >= header_bytes ? len - header_bytes : 0;
    *header_end = '\0';
    char* status_line = buf;
    response->status_code = response_status_of(status_line);
    if (response->status_code == 0 && !response->error) {
        /* Headers arrived, but the first line does not carry a status, so
         * this is not a response to hand back as a success: a proxy would
         * copy the absent status straight onto its own reply. */
        response->error = string_new("malformed status line in response");
    }

    response->headers = string_new(buf);

    /* De-chunk a `Transfer-Encoding: chunked` body so consumers see
     * the decoded payload, not the raw chunk framing
     * (`13\r\n…\r\n0\r\n\r\n`). Without this, any unknown-length /
     * streamed upstream response (no Content-Length) came back
     * framed — corrupting e.g. VCR record-mode tapes
     * (vcr_record_chunked_dechunk_wish.md). Gated on the header, so
     * only chunked bodies — already garbled today — change shape;
     * on malformed framing we keep the raw bytes. */
    const char* body_start = header_end + 4;
    char* te = http_extract_response_header(buf, "Transfer-Encoding");
    char* dechunked = NULL;
    size_t dechunked_len = 0;
    if (te && http_value_has_chunked(te)) {
        dechunked = http_dechunk(body_start, body_bytes, &dechunked_len);
    }
    free(te);
    if (dechunked) {
        response->body = string_new_with_length(dechunked, dechunked_len);
        free(dechunked);
    } else {
        response->body = string_new_with_length(body_start, body_bytes);
    }
}

/* An empty response object, the shape every path starts from. Shared so a
 * driver filling one in from bytes it read itself starts from exactly what
 * the blocking path starts from. */
HttpResponse* http_response_alloc_empty(void) {
    HttpResponse* response = (HttpResponse*)malloc(sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = 0;
    response->body = NULL;
    response->headers = NULL;
    response->error = NULL;
    response->redirect_error = NULL;
    response->effective_url = NULL;
    response->stream = NULL;
    return response;
}

/* A response that says the exchange failed before an answer came, as the
 * blocking path's does for a refused dial, a failed send or a connection
 * closed mid-answer: no status, `why` as its error. A driver that gave up on
 * the upstream hands this to the proxy exchange, which counts the failure,
 * retries where it may and otherwise turns it into the 502 or 504 the client
 * is owed. */
HttpResponse* http_response_alloc_failed(const char* why) {
    HttpResponse* response = http_response_alloc_empty();
    if (!response) return NULL;
    response->error = string_new(why ? why : "upstream failed");
    return response;
}

/* ---- What a driver needs to read from an outbound request ---- */

const char* http_request_method_of(const HttpClientRequest* req) {
    return req ? req->method : NULL;
}

const char* http_request_url_of(const HttpClientRequest* req) {
    return req ? req->url : NULL;
}

const char* http_request_body_of(const HttpClientRequest* req, int* out_len) {
    if (out_len) *out_len = req ? req->body_len : 0;
    return req ? req->body : NULL;
}

const char* http_request_content_type_of(const HttpClientRequest* req) {
    return req ? req->content_type : NULL;
}

/* ---- Upstream connections for a driver that cannot block ----
 *
 * The blocking client dials inside its own call and waits there. A driver
 * running many connections on one thread cannot: it needs the descriptor
 * before the connection is established, so it can wait for that the same way
 * it waits for everything else.
 *
 * The idle pool is the same pool the blocking path uses, so a connection one
 * of them finishes with is available to the other, and a reused connection is
 * ready to write immediately.
 */
/* The idle pool is shared with the blocking client, which reads its sockets
 * expecting them to block. A driver that cannot block needs the opposite, so
 * the mode belongs to the borrower and is set when it takes one.
 *
 * It is only set when it differs. A pooled connection remembers the mode it
 * carries, so a proxy, where the same borrower takes the same connections
 * over and over, changes it once and never again; setting it unconditionally
 * cost two fcntl calls per borrow in each direction, four per request, on
 * sockets that were already right. */
static void http_socket_set_nonblocking(int fd, int on) {
    if (fd < 0) return;
#ifdef _WIN32
    u_long nb = on ? 1 : 0;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (want != flags) fcntl(fd, F_SETFL, want);
#endif
}

int http_upstream_acquire(const char* host, int port, HttpUpstreamConn* out) {
    return http_upstream_acquire_ex(host, port, 1, out);
}

/* A fresh transport on `out`, the pool key kept. No descriptor until one is
 * dialled or taken from the pool: -1, which is what every caller tests
 * (sockfd >= 0). It was 0, so a dial that failed before it made a socket (a
 * name that does not resolve) left descriptor 0 behind as this connection's,
 * and the proxy event loop closed it when the connection ended: the
 * program's stdin, or whatever connection held 0 by then (#3248). */
static void upstream_reset(HttpUpstreamConn* out) {
    out->t.sockfd = -1;
    out->t.nonblocking = 0;
    out->t.applied_timeout_ns = -1;
    /* Set by hand like its neighbours: transport_close frees whatever is
     * here, so an uninitialised pointer would be freed. */
    out->t.rxbuf = NULL;
    out->t.rxcap = 0;
#ifdef AETHER_HAS_OPENSSL
    out->t.ssl = NULL;
    out->t.owned_ctx = NULL;
#endif
    out->reused = 0;
    out->connecting = 0;
    out->addr_n_tried = 0;
    out->addr_total = 0;
    out->dial_host[0] = '\0';
    out->dial_port = 0;
    out->resolver = NULL;
    out->lookup = 0;
}

/* FNV-1a over an address's family, port and bytes: what tells one resolved
 * address from another. */
static unsigned upstream_addr_hash(const struct sockaddr_storage* a) {
    const unsigned char* p;
    size_t n;
    unsigned h = 2166136261u;
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)a;
        p = (const unsigned char*)&a6->sin6_addr; n = 16;
        h = (h ^ 6u) * 16777619u;
        h = (h ^ (unsigned)a6->sin6_port) * 16777619u;
    } else {
        const struct sockaddr_in* a4 = (const struct sockaddr_in*)a;
        p = (const unsigned char*)&a4->sin_addr; n = 4;
        h = (h ^ 4u) * 16777619u;
        h = (h ^ (unsigned)a4->sin_port) * 16777619u;
    }
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Dial `host` over the resolved addresses this connection has not tried yet.
 * Non-blocking, so only an address that fails at once (no route to that
 * family, a refusal the kernel already knows) moves on here; one still
 * connecting is the answer, and http_upstream_redial carries on from the
 * rest if it then fails -- the usual case being a name whose IPv6 address
 * comes first and whose server listens on IPv4 alone. */
static int upstream_dial(const char* host, int port, HttpUpstreamConn* out) {
    AetherAddrList addrs;
    int resolved = 0;
#if HTTP_RESOLVER
    /* A caller that may not wait dials from its resolver's answer, and is
     * told when the answer is still to come or the name does not resolve
     * (#3267). */
    if (out->resolver) {
        int got = http_resolver_answer(out->resolver, host, port, &addrs);
        if (got <= 0) {
            out->lookup = got == 0 ? 1 : -1;
            return -1;
        }
        resolved = 1;
    }
#endif
    if (!resolve_dial_addr(host, port, &addrs, &resolved)) return -1;
    /* The family that last connected to this upstream first: a connect in
     * flight is waited on, not cut short, so the order is what keeps a slow
     * refusal (Windows takes about two seconds over ::1) to the first
     * connection rather than every one. */
    aether_net_order(host, port, &addrs);
    out->addr_total = addrs.n;
    out->dial_host[0] = '\0';
    if (addrs.n > 1 && strlen(host) < sizeof(out->dial_host)) {
        strcpy(out->dial_host, host);
        out->dial_port = port;
    }

    for (int i = 0; i < addrs.n; i++) {
        unsigned h = upstream_addr_hash(&addrs.ss[i]);
        int seen = 0;
        for (int k = 0; k < out->addr_n_tried; k++) if (out->addr_tried[k] == h) seen = 1;
        if (seen) continue;
        if (out->addr_n_tried < (int)(sizeof(out->addr_tried) / sizeof(out->addr_tried[0])))
            out->addr_tried[out->addr_n_tried++] = h;

        int fd = (int)socket(addrs.ss[i].ss_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        out->t.sockfd = fd;
        out->t.nonblocking = 1;      /* dialled non-blocking just above */
        out->reused = 0;
        if (connect(fd, (struct sockaddr*)&addrs.ss[i], addrs.len[i]) == 0) {
            out->connecting = 0;
            if (out->dial_host[0])
                aether_net_note_family(out->dial_host, out->dial_port, addrs.ss[i].ss_family);
            return fd;
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        int in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        int in_progress = (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
        if (in_progress) {
            out->connecting = 1;      /* finished when the descriptor is writable */
            return fd;
        }
        close(fd);
        out->t.sockfd = -1;
    }
    out->addr_n_tried = out->addr_total;   /* nothing left to redial to */
    return -1;
}

/* `allow_pool` is 0 when the caller has just been burned by a pooled
 * connection the upstream had closed. Taking another one from the same pool
 * could hand it a second corpse, and the retry after that is the one the
 * client never gets an answer from. */
static int upstream_acquire(const char* host, int port, int allow_pool,
                            HttpResolver* resolver, HttpUpstreamConn* out) {
    if (!out || !host || port <= 0) return -1;
    /* Field by field, not memset over the struct. Most of this object is a
     * 512 byte pool key, and blanking it wrote nine cache lines per request
     * that the very next line overwrites with about forty bytes. Those lines
     * are cold once more than a handful of connections are in flight, which
     * is where this path's last-level misses were coming from. Every field the
     * code goes on to read is still given the value the memset gave it. */
    upstream_reset(out);
    out->pool_key[0] = '\0';

    http_pool_key(out->pool_key, sizeof(out->pool_key), host, port, 0,
                  host, port, 0, NULL);
    if (allow_pool && http_pool_enabled && http_pool_take(out->pool_key, &out->t)) {
        out->reused = 1;
        out->connecting = 0;
        if (out->t.nonblocking != 1) {
            http_socket_set_nonblocking(out->t.sockfd, 1);
            out->t.nonblocking = 1;
        }
        return out->t.sockfd;
    }
    out->resolver = resolver;
    return upstream_dial(host, port, out);
}

int http_upstream_acquire_ex(const char* host, int port, int allow_pool,
                             HttpUpstreamConn* out) {
    return upstream_acquire(host, port, allow_pool, NULL, out);
}

int http_upstream_has_next(HttpUpstreamConn* c) {
    return c && !c->reused && c->addr_n_tried < c->addr_total;
}

int http_upstream_redial(const char* host, int port, HttpUpstreamConn* c) {
    if (!http_upstream_has_next(c) || !host) return -1;
    /* A fresh transport; the tried set carries over, and so does the
     * resolver a dial that may not wait asks (#3267). */
    unsigned tried[8];
    int n_tried = c->addr_n_tried;
    HttpResolver* resolver = c->resolver;
    memcpy(tried, c->addr_tried, sizeof tried);
    upstream_reset(c);
    memcpy(c->addr_tried, tried, sizeof tried);
    c->addr_n_tried = n_tried;
    c->resolver = resolver;
    return upstream_dial(host, port, c);
}

int http_upstream_acquire_nb(const char* host, int port, int allow_pool,
                             HttpResolver* resolver, HttpUpstreamConn* out) {
    return upstream_acquire(host, port, allow_pool, resolver, out);
}

int http_upstream_lookup(const HttpUpstreamConn* c) {
    return c ? c->lookup : 0;
}

/* Did the connect this driver started actually succeed? A refused connection
 * makes the descriptor writable too, with the reason in SO_ERROR, so
 * writability alone is not the answer. */
int http_upstream_connected(HttpUpstreamConn* c) {
    if (!c || c->t.sockfd < 0) return -1;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->t.sockfd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0)
        return -1;
    if (err != 0) return -1;

    /* SO_ERROR is 0 for a connect that has finished and for one still in
     * flight alike, so on its own it cannot tell them apart. A socket with no
     * peer yet fails getpeername with ENOTCONN, which does.
     *
     * This matters because a caller is woken by a poller, and a poller may
     * wake it for another descriptor, or for no reason at all: both epoll and
     * kqueue are allowed to report spurious readiness. Reading any wakeup as
     * "the connect finished" writes the request into a socket that has no
     * peer, which fails with ENOTCONN and looks like the upstream refusing. */
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    if (getpeername(c->t.sockfd, (struct sockaddr*)&peer, &peerlen) != 0)
        return errno == ENOTCONN ? 0 : -1;

    c->connecting = 0;
    if (c->dial_host[0]) {
        aether_net_note_family(c->dial_host, c->dial_port, peer.ss_family);
        c->dial_host[0] = '\0';
    }
    return 1;
}

/* Does `v`, `vl` bytes not NUL-terminated, hold `needle`, ignoring case? */
static int http_span_has_ci(const char* v, size_t vl, const char* needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= vl; i++)
        if (strncasecmp(v + i, needle, nl) == 0) return 1;
    return 0;
}

/* One rule for both clients, so the proxy event loop stops pooling a
 * connection the upstream said it would close (#3259). The Connection value
 * is read where it lies: the loop asks this of every response it pools a
 * connection after, and copying the value out to look at it was an
 * allocation on each one that had the header (#3281). */
int http_response_head_keeps_conn(const char* head, size_t header_bytes) {
    if (!head || header_bytes < 4) return 0;
    const char* end = head + header_bytes - 4;
    const char* status_end = (const char*)memchr(head, '\n', (size_t)(end - head));
    const char* v = NULL;
    size_t vl = 0;
    if (status_end &&
        http_find_header_span(status_end + 1, end, "Connection", NULL, 0, NULL, &v, &vl) > 0)
        return !http_span_has_ci(v, vl, "close");
    return strncmp(head, "HTTP/1.0", 8) != 0;   /* 1.0 closes unless it says otherwise */
}

/* Hand the connection back to the idle pool, or close it. A connection is
 * only worth keeping when the response that came over it ended where its own
 * framing said it would, which is the caller's decision to make. */
void http_upstream_release(HttpUpstreamConn* c, int keep) {
    if (!c || c->t.sockfd < 0) return;
    if (keep && http_pool_enabled) {
        /* Left as it is: whoever takes it next sets what it needs, and in a
         * proxy that is this driver again. */
        http_pool_put(c->pool_key, &c->t);
    } else {
        transport_close(&c->t);
    }
    c->t.sockfd = -1;
}

/* ---- The upstream exchange: write the request, read one response ----
 *
 * Everything on a client call that waits for the peer happens here, and
 * nothing else does. A driver that owns its thread loops until the exchange
 * says DONE, and never sees a WANT because a blocking transport does not
 * produce one. A driver that cannot block hands the descriptor to a poller on
 * a WANT and comes back. Both run this code, so there is one implementation
 * of what a request looks like on the wire and one of when a response has
 * finished arriving.
 *
 * The exchange does not own the transport and does not close it: whether a
 * failure is worth a redial is the driver's decision, and only the driver
 * knows whether the connection came from the pool.
 */


void http_exchange_init(HttpExchange* x, Transport* t,
                               const char* head, size_t head_len,
                               const char* body, int body_len,
                               const char* method) {
    memset(x, 0, sizeof(*x));
    x->t = t;
    x->head = head;
    x->head_len = head_len;
    x->body = body;
    x->body_len = body_len;
    x->method = method;
}

void http_exchange_set_timeout(HttpExchange* x, int64_t timeout_ns) {
    if (!x) return;
    x->deadline_ms = timeout_ns > 0
        ? http_now_ms() + (timeout_ns + 999999LL) / 1000000LL
        : 0;
}

/* Would this send/recv have blocked, rather than failed? */
static int http_io_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* Write the request head, then the body. Bodies are written raw so embedded
 * NULs survive. */
int http_exchange_send(HttpExchange* x) {
    while (x->head_sent < x->head_len) {
        int n = transport_send(x->t, x->head + x->head_sent,
                               (int)(x->head_len - x->head_sent));
        if (n > 0) { x->head_sent += (size_t)n; continue; }
        return http_io_would_block() ? AE_X_WANT_WRITE : AE_X_ERROR;
    }
    while (x->body && x->body_sent < x->body_len) {
        int n = transport_send(x->t, x->body + x->body_sent,
                               x->body_len - x->body_sent);
        if (n > 0) { x->body_sent += n; continue; }
        return http_io_would_block() ? AE_X_WANT_WRITE : AE_X_ERROR;
    }
    return AE_X_DONE;
}

/* Read until this response ends where its own framing says it does, or until
 * the peer closes, which is the framing when nothing else declares one. */
int http_exchange_recv(HttpExchange* x) {
    char chunk[8192];
    for (;;) {
        /* The caller's budget covers the whole response, not each recv: a
         * peer dripping one byte per interval never trips SO_RCVTIMEO. */
        if (x->deadline_ms && http_now_ms() >= x->deadline_ms) {
            x->timed_out = 1;
            return AE_X_ERROR;
        }
        int n = transport_recv(x->t, chunk, sizeof(chunk) - 1);
        if (n == 0) { x->peer_closed = 1; return AE_X_DONE; }
        if (n < 0)  return http_io_would_block() ? AE_X_WANT_READ : AE_X_ERROR;

        if (x->len + (size_t)n + 1 > x->cap) {
            size_t new_cap = x->cap ? x->cap * 2 : 16384;
            if (x->cap == 0) atomic_fetch_add(&http_rx_fresh_allocs, 1);
            while (new_cap < x->len + (size_t)n + 1) new_cap *= 2;
            /* #461: the response body is attacker-controlled (a malicious
             * server can flood it), so the doubling buffer is gated through
             * the capability allocator. `cap` carries the old size for the
             * realloc delta and for the error-path free. */
            char* grown = (char*)aether_caps_realloc(x->buf, x->cap, new_cap);
            if (!grown) { x->oom = 1; return AE_X_ERROR; }
            x->buf = grown;
            x->cap = new_cap;
        }
        memcpy(x->buf + x->len, chunk, (size_t)n);
        x->len += (size_t)n;
        x->buf[x->len] = '\0';

        if (http_response_is_complete(&x->framing, x->buf, x->len, x->method)) {
            x->complete = 1;
            return AE_X_DONE;
        }
    }
}

/* Finish with a connection the blocking path used: back to the pool with its
 * receive buffer, or closed and the buffer freed. The one place a request's
 * connection is disposed of once its response has been read. */
static void http_conn_finish(int keep, const char* key, Transport* t,
                             char* buf, size_t cap) {
    if (keep && buf && cap <= HTTP_RX_KEEP_MAX && !t->rxbuf) {
        t->rxbuf = buf;
        t->rxcap = cap;
        buf = NULL;
    }
    if (buf) aether_caps_free(buf, cap);
    if (keep) http_pool_put(key, t);
    else      transport_close(t);
}

static HttpResponse* http_request_internal(HttpClientRequest* req) {
    const char* method = req->method;
    const char* url    = req->url;
    const char* body   = req->body;          /* may be NULL */
    int   body_len     = req->body_len;
    const char* content_type = req->content_type;
    http_init();

    HttpResponse* response = (HttpResponse*)malloc(sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = 0;
    response->body = NULL;
    response->headers = NULL;
    response->error = NULL;
    response->redirect_error = NULL;
    response->effective_url = NULL;
    response->stream = NULL;

    char host[256];
    /* Sized from the URL, as long as it needs: a fixed path[1024] cut a
     * longer path and sent the request anyway (#2674). Freed once the request
     * head is built, its only reader, and on each return before that. */
    char* path = NULL;
    int port;
    int use_tls;

    if (!parse_url_alloc(url, host, sizeof(host), &port, &path, &use_tls)) {
        response->error = string_new("malformed URL");
        return response;
    }

    /* An https request the program cannot make (the pure-Aether client
     * selected but not linked, #1849) is the program's to fix: say so
     * before resolving or dialling, so the answer does not depend on the
     * network. It was found only after the TCP connect, and when the
     * connect failed first (a server not up yet, a loaded machine) the
     * request reported "connection failed" instead. */
    if (use_tls) {
        const char* missing = http_tls_backend_missing();
        if (missing) {
            free(path);
            response->error = string_new(missing);
            return response;
        }
    }

    /* Forward-proxy resolution (aether#1012). Default is direct. When a proxy
     * applies, we CONNECT to the proxy's host/port instead of the target's;
     * the original target host/port stay in `host`/`port` for the Host header,
     * the CONNECT line (https), and the absolute-form request line (http). */
    char connect_host[256];
    int  connect_port;
    int  via_proxy = 0;
    {
        char phost[256];
        int  pport = 0;
        const char* perr = NULL;
        int pr = resolve_proxy_for(req, host, use_tls,
                                   phost, sizeof(phost), &pport, &perr);
        if (pr < 0) {
            free(path);
            response->error = string_new(perr ? perr : "proxy error");
            return response;
        }
        if (pr == 1) {
            via_proxy = 1;
            snprintf(connect_host, sizeof(connect_host), "%s", phost);
            connect_port = pport;
        } else {
            snprintf(connect_host, sizeof(connect_host), "%s", host);
            connect_port = port;
        }
    }

    /* From here the socket connects to connect_host:connect_port (the proxy
     * when via_proxy, else the origin). Rebind the resolve/connect variables. */
    const char* dial_host = connect_host;
    int         dial_port = connect_port;

    /* The dial address, resolved lazily (#1719).
     *
     * Resolution used to run unconditionally, above the pool lookup, so every
     * request resolved the backend host and threw the answer away on a pooled
     * hit -- a lock-taking call per request for a result nobody used. Only the
     * two http_dial sites consume it, and the pool key is built from
     * dial_host/dial_port rather than the resolved address, so nothing before
     * a dial needs it.
     *
     * Both dial sites go through resolve_dial_addr, which resolves at most
     * once per request: the send-failure retry below re-dials a connection
     * that came from the pool, and that is precisely the path where the first
     * resolve was skipped.
     *
     * One deliberate behaviour change falls out of this: a request that hits a
     * live pooled connection now succeeds even if the host has since stopped
     * resolving, where before it failed with "could not resolve host". An open
     * connection does not need DNS, and holding an established socket hostage
     * to a resolver blip is the less useful behaviour -- it is also what nginx
     * does. A request that has to dial still fails exactly as it did. */
    AetherAddrList serv_addr;
    int serv_addr_resolved = 0;

    /* Zero-initialised so applied_timeout_ns starts at the "nothing applied"
     * sentinel rather than stack garbage, which the reuse guard would read as
     * a real value and wrongly skip the setsockopt. */
    Transport t = {0};
    t.applied_timeout_ns = -1;
    char pool_key[HTTP_POOL_KEY_MAX];
    /* The TLS backend is part of a connection's identity: 1 = OpenSSL,
     * 2 = pure-Aether. A pooled OpenSSL connection must never serve a request
     * that asked for the pure client, or the reverse. */
    int tls_kind = use_tls ? (http_client_use_pure_tls() ? 2 : 1) : 0;
    http_pool_key(pool_key, sizeof(pool_key), host, port, tls_kind,
                  dial_host, dial_port, req->insecure, req->cafile);
    /* A streaming response hands the transport to the caller, who may abandon
     * it mid-body, so those connections are never pooled in either direction.
     * A caller who set their own Connection header gets exactly that, and a
     * connection the peer is about to close is not worth keeping. */
    int pool_this = http_pool_enabled && !req->stream &&
                    !header_already_set(req, "Connection");
    int reused = 0;
    if (pool_this && http_pool_take(pool_key, &t)) {
        /* This path reads with a timeout on the socket and expects it to
         * block. A driver may have left this connection non-blocking, where
         * a read that would wait returns EAGAIN, which this path reads as a
         * timeout. Put it back, and only when it differs. */
        if (t.nonblocking != 0) {
            http_socket_set_nonblocking(t.sockfd, 0);
            t.nonblocking = 0;
        }
        /* Taken without probing it first (#1719).
         *
         * This used to poll the socket before reusing it, one syscall on
         * every request through the pool, to catch a peer that had closed
         * during the idle window. The read path already handles that case and
         * handles it better: nothing coming back on a pooled connection means
         * the request went into the void, and it redials and resends. The
         * poll only moved the discovery earlier at the cost of asking the
         * kernel every time.
         *
         * The other thing it caught — bytes waiting on a supposedly idle
         * connection, which would be read as the head of this response — is
         * caught below instead, by checking that what came back actually
         * starts a response. That costs nothing and is a stronger check: the
         * poll only knew that something was readable, not what it was. */
        reused = 1;
        transport_apply_timeouts(&t, req->timeout_ns);
    }
    if (!reused) {
        if (!resolve_dial_addr(dial_host, dial_port, &serv_addr,
                               &serv_addr_resolved)) {
            response->error = string_new(via_proxy ? "could not resolve proxy host"
                                                   : "could not resolve host");
            return response;
        }

        char* dial_err = NULL;
        if (http_dial(req, &serv_addr, dial_host, dial_port, host, port, use_tls, via_proxy, &t, &dial_err) != 0) {
            response->error = string_new(dial_err ? dial_err : "connection failed");
            free(dial_err);
            return response;
        }
    }

    /* Build the header block in a heap-allocated growing buffer so
     * we're not bounded by a 4K stack array. Body goes out as a
     * separate transport_send so binary payloads with embedded NULs
     * survive (the previous "%s" snprintf path would have truncated
     * at the first NUL — wasn't a problem in practice because the
     * v1 wrappers only sent textual JSON, but v2 takes body+len). */
    size_t hdr_cap = 0, hdr_len = 0;
    HttpReqHead head_params = {
        req, method, path, host, port, via_proxy, use_tls,
        body, body_len, content_type, pool_this
    };
    char* hdr = http_build_request_head(&head_params, &hdr_len, &hdr_cap);
    free(path);   /* the head holds the request line now */
    path = NULL;
    if (!hdr) {
        transport_close(&t);
        response->error = string_new("out of memory building request");
        return response;
    }

    /* Accumulator for the buffered read below, declared here because the
     * reuse retry rewinds to `send_request` and has to reset it. */
    char*  full_response = NULL;
    size_t total_len = 0;
    size_t cap = 0;
    int    n = 0;
    int    recv_err = 0;
    int    timed_out = 0;   /* the whole-request budget ran out mid-response */
    int    rx_complete_at_deadline = 0;  /* ... but the response had already ended */
    int    truncated = 0;
    HttpRespFraming framing = {0};
    /* A pooled connection the peer closed while it sat idle is
     * indistinguishable from a live one until it is used: the liveness probe
     * catches almost all of them, and the rest fail with nothing received.
     * Measured against an upstream that closes after every response, 4% of
     * requests landed in that window. One redial covers it. The retry is
     * bounded to a connection that came from the pool and produced no
     * response byte, so the server cannot have acted on the request. */
    int retried = 0;
    HttpExchange tx;

send_request:
    http_exchange_init(&tx, &t, hdr, hdr_len, body, body_len, method);
    /* A WANT here is SO_SNDTIMEO firing on a transport this driver owns
     * outright, which is a failed write like any other. */
    if (http_exchange_send(&tx) != AE_X_DONE) {
        if (reused && !retried) {
            retried = 1;
            reused = 0;
            transport_close(&t);
            char* rd_err = NULL;
            /* This connection came from the pool, so the resolve above was
             * skipped; do it now. A failure here just means no retry. */
            if (resolve_dial_addr(dial_host, dial_port, &serv_addr,
                                  &serv_addr_resolved)
                && http_dial(req, &serv_addr, dial_host, dial_port, host, port, use_tls, via_proxy, &t, &rd_err) == 0) {
                free(rd_err);
                goto send_request;
            }
            free(rd_err);
        }
        aether_caps_free(hdr, hdr_cap);
        transport_close(&t);
        response->error = string_new("send failed");
        return response;
    }

    /* Streaming mode (#1004): read only the header block, then hand the
     * still-open transport to an HttpStream so the caller pulls the body
     * incrementally (peak memory = one read window, not O(Content-Length)).
     * The buffered read-until-EOF path below is skipped. */
    if (req->stream) {
        /* A stream is never taken from the pool, so it never retries and the
         * request buffer is done with here. */
        aether_caps_free(hdr, hdr_cap);
        hdr = NULL;
        char   sbuf[8192];
        char*  hb = NULL;        /* headers + any over-read body bytes */
        size_t hlen = 0, hcap = 0;
        char*  hend = NULL;
        int    sn, serr = 0;
        while ((sn = transport_recv(&t, sbuf, sizeof(sbuf))) > 0) {
            if (hlen + (size_t)sn + 1 > hcap) {
                size_t nc = hcap ? hcap * 2 : 16384;
                while (nc < hlen + (size_t)sn + 1) nc *= 2;
                char* nb = (char*)realloc(hb, nc);
                if (!nb) {
                    free(hb);
                    transport_close(&t);
                    response->error = string_new("out of memory reading response headers");
                    return response;
                }
                hb = nb; hcap = nc;
            }
            memcpy(hb + hlen, sbuf, (size_t)sn);
            hlen += (size_t)sn;
            hb[hlen] = '\0';
            /* The header terminator is NUL-free ASCII and precedes any body
             * byte, so strstr finds it before any body NUL. */
            hend = strstr(hb, "\r\n\r\n");
            if (hend) break;
        }
        if (sn < 0) serr = 1;
        if (!hend) {
            free(hb);
            transport_close(&t);
            response->error = string_new(
                serr ? "recv timeout or I/O error"
                     : "connection closed before response headers");
            return response;
        }

        size_t header_bytes = (size_t)((hend + 4) - hb);
        size_t over_len     = hlen - header_bytes;   /* body bytes already read */
        *hend = '\0';                                 /* isolate the header block */

        response->status_code = response_status_of(hb);
        response->headers = string_new(hb);

        /* Framing: chunked wins over Content-Length; neither => read-until-close. */
        int       is_chunked = 0;
        long long clen = -1;
        char* te = http_extract_response_header(hb, "Transfer-Encoding");
        if (te && http_value_has_chunked(te)) is_chunked = 1;
        free(te);
        if (!is_chunked) {
            char* cl = http_extract_response_header(hb, "Content-Length");
            if (cl) { clen = strtoll(cl, NULL, 10); free(cl); }
        }

        struct HttpStream* st = (struct HttpStream*)calloc(1, sizeof(struct HttpStream));
        if (!st) {
            free(hb);
            transport_close(&t);
            response->error = string_new("out of memory");
            return response;
        }
        st->t = t;                 /* transfer ownership of the open transport */
        st->chunked = is_chunked;
        if (is_chunked) {
            st->chunk_state = 0;   /* start by reading a chunk-size line */
        } else if (clen >= 0) {
            st->content_remaining = clen;
            if (clen == 0) st->eof = 1;   /* declared empty body */
        } else {
            st->read_until_close = 1;
        }
        if (over_len > 0) {
            st->pending = (char*)malloc(over_len);
            if (!st->pending) {
                free(hb);
                http_stream_free(st);   /* closes the transport it now owns */
                response->error = string_new("out of memory");
                return response;
            }
            memcpy(st->pending, hend + 4, over_len);
            st->pending_len = (int)over_len;
            st->pending_cap = (int)over_len;
        }
        free(hb);
        response->stream = st;
        return response;           /* transport stays open, owned by the stream */
    }

    /* Read to the end of THIS response, not to end of connection: the header
     * block, then exactly the body its framing declares. Reading to EOF works
     * only when the connection is the delimiter, which is what made every
     * request cost a fresh connection (#1653). `body_target` is the total
     * byte count that ends the response, or 0 while it is still unknown.
     * The accumulator grows by doubling; a realloc per recv was quadratic on
     * large responses. */
    {
        HttpExchange rx;
        http_exchange_init(&rx, &t, NULL, 0, NULL, 0, method);
        /* A connection from the pool may carry the buffer its last response
         * was read into (#2160). Hand it to the exchange empty, and let it
         * grow from there instead of from nothing. Ownership moves: the
         * connection no longer frees it. */
        if (t.rxbuf) {
            rx.buf = t.rxbuf;
            rx.cap = t.rxcap;
            rx.len = 0;
            t.rxbuf = NULL;
            t.rxcap = 0;
        }
        /* The caller's timeout is a budget for the whole response, not for
         * each recv: SO_RCVTIMEO alone let a peer that dribbles a byte just
         * inside the limit hold the request open indefinitely. */
        http_exchange_set_timeout(&rx, req->timeout_ns);
        int rc = http_exchange_recv(&rx);
        timed_out = rx.timed_out;
        rx_complete_at_deadline = rx.complete || rx.peer_closed;
        full_response = rx.buf;
        total_len     = rx.len;
        cap           = rx.cap;
        framing       = rx.framing;
        /* A response that never said where its body ends is not one to hand
         * back: the bytes it did not account for belong to nothing, and on a
         * pooled connection they would be read as the head of the next
         * response. */
        if (rx.framing.invalid) {
            aether_caps_free(full_response, cap);
            transport_close(&t);
            response->error = string_new(
                "response framing is ambiguous: the declared body length is not usable");
            return response;
        }
        if (rx.oom) {
            aether_caps_free(full_response, cap);
            transport_close(&t);
            response->error = string_new("out of memory reading response");
            return response;
        }
        /* A WANT on a blocking transport is SO_RCVTIMEO firing, not a socket
         * with more to say later: this driver owns its thread and never asked
         * for a non-blocking one. Treating it as anything but a failed read
         * is what once let a timed-out request return an empty 0-status
         * response that the caller could not tell from a silent server. */
        if (rc == AE_X_ERROR || rc == AE_X_WANT_READ || rc == AE_X_WANT_WRITE)
            recv_err = 1;
        /* The response declared its own length and stopped short of it. The
         * body we hold is a prefix of the real one, and handing that back as
         * a successful response makes a truncated payload indistinguishable
         * from a complete one: a proxy forwards a short body as if it were
         * whole, and a caller parses whatever arrived. A response with no
         * declared framing is not this case, because there the close IS the
         * framing. */
        truncated = rx.framing.definite && !rx.complete;
        n = 0;
    }
    /* Nothing at all came back on a connection that came from the pool: the
     * peer had closed it and our request went into the void. Redial once and
     * send again; the server never saw this request, so repeating it is safe
     * whatever the method. The desync case below is the same reasoning: what
     * came back was not an answer to this request. */
    /* A pooled connection that answers with something that is not a response
     * was carrying bytes nobody asked for, and reading them as this
     * response's head would desynchronise everything after it. Same treatment
     * as no answer at all: retire the connection and ask again on a fresh
     * one. Only for a connection that came from the pool — a freshly dialled
     * one returning nonsense is the upstream's answer, not a stale socket. */
    int desynced = reused && total_len > 0
                && (total_len < 5 || strncmp(full_response, "HTTP/", 5) != 0);

    if ((total_len == 0 || desynced) && reused && !retried) {
        retried = 1;
        reused = 0;
        transport_close(&t);
        char* rd_err = NULL;
        /* Pooled connection, so the resolve was skipped; do it now. */
        if (resolve_dial_addr(dial_host, dial_port, &serv_addr,
                              &serv_addr_resolved)
            && http_dial(req, &serv_addr, dial_host, dial_port, host, port, use_tls, via_proxy, &t, &rd_err) == 0) {
            free(rd_err);
            aether_caps_free(full_response, cap);
            full_response = NULL;
            total_len = 0;
            cap = 0;
            recv_err = 0;
            framing.header_bytes = 0;
            framing.body_target = 0;
            framing.chunked = 0;
            framing.definite = 0;
            goto send_request;
        }
        /* The redial failed too, so the upstream is gone rather than the
         * pooled connection being stale. Say that instead of handing back an
         * empty status-0 response. */
        aether_caps_free(hdr, hdr_cap);
        aether_caps_free(full_response, cap);
        response->error = string_new(rd_err ? rd_err : "connection failed");
        free(rd_err);
        return response;
    }

    aether_caps_free(hdr, hdr_cap);
    hdr = NULL;

    /* n < 0 means transport_recv hit an error; the most common one
     * (when the caller set a timeout) is recv-side EAGAIN/EWOULDBLOCK
     * from SO_RCVTIMEO firing. Without this, a timed-out request
     * silently returned an empty 0-status response — caller couldn't
     * tell timeout from a happy server returning nothing. */
    if (n < 0) {
        recv_err = 1;
    }

    // Zero-byte response: still need a valid empty string so strstr
    // below is safe. Tiny allocation, done only on the empty path.
    if (!full_response) {
        full_response = (char*)aether_caps_malloc(1);
        if (!full_response) {
            transport_close(&t);
            response->error = string_new("out of memory");
            return response;
        }
        full_response[0] = '\0';
        cap = 1;  /* #461: record the 1-byte size so the frees below balance */
    }

    /* Whether the connection goes back to the pool is decided here, but it is
     * DONE by http_conn_finish at each exit below, after the body has been
     * copied out: the receive buffer goes back with it (#2160), and it is
     * still being read until then. */
    int keep_conn = 0;
    /* Hand the connection back only when this response ended where its own
     * framing said it would, and neither side asked to close. Anything else
     * (read-until-EOF framing, a truncated body, an I/O error) leaves the
     * next response's start position unknown, so the connection is retired. */
    {
        int keep = pool_this && !recv_err && framing.definite &&
                   (framing.chunked || total_len == framing.body_target);
        if (keep && framing.header_bytes > 0) {
            keep = http_response_head_keeps_conn(full_response, framing.header_bytes);
        } else {
            keep = 0;
        }
        keep_conn = keep;
    }

    /* If transport_recv reported an error AND we didn't get a complete
     * header block, this was a timeout / I/O error mid-recv. Tell the
     * caller — otherwise they'd see status=0 + empty body and not
     * know whether the request even reached the server. */
    char* header_end = strstr(full_response, "\r\n\r\n");
    /* A response framed by the close (no Content-Length, no chunked) that
     * the deadline cut off is NOT complete: `truncated` cannot see it
     * (nothing declared a length) and its headers did arrive, so without
     * this it came back as a successful partial body with no error — the
     * silent truncation the deadline exists to make visible. */
    if (timed_out && !rx_complete_at_deadline) {
        http_conn_finish(keep_conn, pool_key, &t, full_response, cap);
        response->error = string_new(
            "request timed out before the response ended");
        return response;
    }
    if (recv_err && !header_end) {
        http_conn_finish(keep_conn, pool_key, &t, full_response, cap);
        response->error = string_new(
            timed_out ? "request timed out before the response arrived"
                      : "recv timeout or I/O error");
        return response;
    }
    /* A response that stopped short of the length it declared is an error
     * even though its headers arrived intact. Reporting only the no-headers
     * case meant a read that died mid-body came back as a successful short
     * response, which a caller cannot tell from a complete one. */
    if (truncated) {
        http_conn_finish(keep_conn, pool_key, &t, full_response, cap);
        response->error = string_new(
            timed_out ? "request timed out: the response stopped short of its declared body length"
            : recv_err ? "response truncated: read failed before the declared body length"
                       : "response truncated: peer closed before the declared body length");
        return response;
    }
    /* Hand over exactly the body the response declared. A read can return
     * more than that (a pipelined reply, or bytes a mis-framed response left
     * behind), and counting those as body makes the payload something the
     * sender never described. Chunked framing carries its own end, so it is
     * left alone. */
    size_t deliver_len = total_len;
    if (framing.definite && !framing.chunked && framing.body_target > 0
        && framing.body_target < deliver_len)
        deliver_len = framing.body_target;
    http_response_fill_from_bytes(response, full_response, deliver_len);

    /* The body is a copy now, so the buffer is free to go back with the
     * connection for the next request on it. */
    http_conn_finish(keep_conn, pool_key, &t, full_response, cap);
    return response;
}

/* Case-insensitive header-name match. Used both when emitting the
 * built-in headers (skip if caller already set one) and when looking
 * up a response header by name. */
static int http_strcaseeq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int header_already_set(const HttpClientRequest* req, const char* name) {
    if (!req) return 0;
    for (HttpHeader* h = req->headers; h; h = h->next) {
        if (http_strcaseeq(h->name, name)) return 1;
    }
    return 0;
}

/* Look up the case-folded value of a response header from the raw
 * header block. Lighter than http_response_header_raw because it
 * works directly on the string buffer rather than the response
 * struct (so the redirect loop below can use it without releasing
 * intermediate responses). Returns a freshly-allocated copy the
 * caller must free, or NULL if the header isn't present. */
static char* http_extract_response_header(const char* hdr_block, const char* name) {
    if (!hdr_block || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = hdr_block;
    /* Skip the status line. */
    const char* nl = strchr(p, '\n');
    if (nl) p = nl + 1;
    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        const char* colon = memchr(p, ':', line_len);
        if (colon && (size_t)(colon - p) == name_len) {
            int match = 1;
            for (size_t i = 0; i < name_len; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match) {
                const char* v = colon + 1;
                while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)((p + line_len) - v);
                while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ' ||
                                    v[vlen - 1] == '\t')) vlen--;
                char* out = (char*)malloc(vlen + 1);
                if (!out) return NULL;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return out;
            }
        }
        if (!line_end) break;
        p = line_end + 1;
    }
    return NULL;
}

/* Case-insensitive: does the Transfer-Encoding value name `chunked`?
 * `chunked` is the final (and in practice only) transfer coding we
 * decode; a comma-list like "gzip, chunked" still matches. */
static int http_value_has_chunked_n(const char* v, size_t len) {
    if (!v || len < 7) return 0;
    /* Only the seven letters can produce these values with 0x20 set, so this
     * is an exact case-insensitive compare and not a loose one. */
    for (size_t i = 0; i + 7 <= len; i++) {
        const char* p = v + i;
        if ((p[0] | 0x20) == 'c' && (p[1] | 0x20) == 'h' &&
            (p[2] | 0x20) == 'u' && (p[3] | 0x20) == 'n' &&
            (p[4] | 0x20) == 'k' && (p[5] | 0x20) == 'e' &&
            (p[6] | 0x20) == 'd') {
            return 1;
        }
    }
    return 0;
}

static int http_value_has_chunked(const char* v) {
    return v ? http_value_has_chunked_n(v, strlen(v)) : 0;
}

/* Decode HTTP/1.1 chunked transfer-encoding (RFC 7230 §4.1). `in` /
 * `in_len` is the raw chunk-framed body; writes the decoded length to
 * *out_len and returns a malloc'd buffer (NUL-terminated for
 * convenience; *out_len excludes the NUL). Returns NULL on malformed
 * framing so the caller can fall back to the raw bytes. Binary-safe:
 * the payload is copied by length, never scanned for NUL. Chunk
 * extensions (`<size>;name=val`) are skipped; trailing trailers after
 * the terminating `0` chunk are ignored. */
char* http_dechunk(const char* in, size_t in_len, size_t* out_len) {
    if (!in || !out_len) return NULL;
    char* out = (char*)malloc(in_len + 1);   /* decoded payload <= input */
    if (!out) return NULL;
    size_t oi = 0;
    size_t i = 0;
    while (i < in_len) {
        size_t sz = 0;
        int saw_hex = 0;
        while (i < in_len) {
            char c = in[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (size_t)d;
            saw_hex = 1;
            i++;
        }
        if (!saw_hex) { free(out); return NULL; }   /* expected a hex size */
        /* Skip the rest of the size line (chunk extensions) to the LF. */
        while (i < in_len && in[i] != '\n') i++;
        if (i >= in_len) { free(out); return NULL; } /* no CRLF after size */
        i++;                                          /* consume the '\n' */
        if (sz == 0) break;                           /* terminating chunk */
        if (sz > in_len - i) { free(out); return NULL; } /* truncated chunk */
        memcpy(out + oi, in + i, sz);
        oi += sz;
        i += sz;
        /* Consume the CRLF (or bare LF) that follows the chunk data. */
        if (i < in_len && in[i] == '\r') i++;
        if (i < in_len && in[i] == '\n') i++;
    }
    out[oi] = '\0';
    *out_len = oi;
    return out;
}

/* Resolve a Location-header value against a base URL. The Location
 * may be absolute (`http://other.host/x`), scheme-relative
 * (`//other.host/x`), root-relative (`/x`), or relative
 * (`x`). Returns a malloc'd absolute URL on success, NULL on
 * malformed input. */
static char* http_resolve_location(const char* base_url, const char* location) {
    if (!base_url || !location || !*location) return NULL;
    /* Absolute URL — Location starts with a scheme. */
    if (strstr(location, "://")) return strdup(location);
    /* Need to extract scheme + host[:port] from base_url.
     * parse_url returns 1 on success, 0 on failure — bail when it
     * fails. The earlier round-1 implementation had the check
     * inverted, which produced a bogus "malformed Location header"
     * for every well-formed base URL. Caught by the cases-8-10
     * end-to-end test in test_http_client_v2.ae. */
    /* Both paths are sized to fit: fixed 1024-byte buffers failed a redirect
     * from, or to, a long path (#2674). */
    char base_host[256];
    char* base_path = NULL;
    int base_port = 0, base_use_tls = 0;
    if (parse_url_alloc(base_url, base_host, sizeof(base_host),
                        &base_port, &base_path, &base_use_tls) == 0) {
        return NULL;
    }
    const char* scheme = base_use_tls ? "https" : "http";
    char* out = NULL;
    /* Scheme-relative: //host/x → keep base scheme. */
    if (location[0] == '/' && location[1] == '/') {
        size_t need = strlen(scheme) + 1 + strlen(location) + 1;
        out = (char*)malloc(need);
        if (out) snprintf(out, need, "%s:%s", scheme, location);
        free(base_path);
        return out;
    }
    /* Root-relative: /x → keep base scheme + host[:port]; a relative path
     * replaces the last segment of base_path. */
    char* joined_path = NULL;
    const char* tail = location;
    if (location[0] != '/') {
        char* last_slash = strrchr(base_path, '/');
        size_t prefix_len = last_slash ? (size_t)(last_slash - base_path) + 1 : 0;
        joined_path = (char*)malloc(prefix_len + strlen(location) + 2);
        if (!joined_path) { free(base_path); return NULL; }
        if (last_slash) {
            memcpy(joined_path, base_path, prefix_len);
            strcpy(joined_path + prefix_len, location);
        } else {
            joined_path[0] = '/';
            strcpy(joined_path + 1, location);
        }
        tail = joined_path;
    }
    size_t need = strlen(scheme) + 3 + strlen(base_host) + 16 + strlen(tail) + 1;
    out = (char*)malloc(need);
    if (out) {
        if ((base_use_tls && base_port == 443) || (!base_use_tls && base_port == 80)) {
            snprintf(out, need, "%s://%s%s", scheme, base_host, tail);
        } else {
            snprintf(out, need, "%s://%s:%d%s", scheme, base_host, base_port, tail);
        }
    }
    free(joined_path);
    free(base_path);
    return out;
}

/* Strip headers that should not be forwarded across a host change
 * (Authorization, Cookie, Proxy-Authorization). Modifies req in
 * place. Called on each redirect hop where the target host differs
 * from the previous host. */
static void http_strip_cross_host_headers(HttpClientRequest* req) {
    if (!req) return;
    HttpHeader** link = &req->headers;
    while (*link) {
        HttpHeader* h = *link;
        int strip = http_strcaseeq(h->name, "Authorization") ||
                    http_strcaseeq(h->name, "Cookie") ||
                    http_strcaseeq(h->name, "Proxy-Authorization");
        if (strip) {
            *link = h->next;
            free(h);      /* name and value live in this same block */
        } else {
            link = &h->next;
        }
    }
    /* Any of those unlinked nodes could have been the tail. Recomputed
     * once here rather than tracked through the loop: this runs only on a
     * redirect whose host differs, while the insert path it keeps O(1) runs
     * for every header of every request. */
    req->headers_tail = NULL;
    for (HttpHeader* t = req->headers; t; t = t->next) req->headers_tail = t;
}

/* v2 entry point. The v1 wrappers below build a throwaway request
 * and call this. Handles redirect-following when the request was
 * configured with max_redirects > 0 (issue #239). */
HttpResponse* http_send_raw(HttpClientRequest* req) {
    if (!req) return NULL;

    HttpResponse* resp = http_request_internal(req);
    if (!resp) return NULL;

    /* Stash the URL of the originating request as the effective URL.
     * Overwritten below if redirects are followed. */
    if (req->url) resp->effective_url = string_new(req->url);

    /* If redirects aren't enabled, return the first response as-is. */
    if (req->max_redirects <= 0) return resp;

    /* No URL to resolve redirects against (strdup(NULL) below would be UB),
     * so there is nothing to follow: return the first response as-is. */
    if (!req->url) return resp;

    /* Track visited URLs for loop detection. Bounded by max_redirects
     * + 1 (the original URL). Static array is fine — max_redirects is
     * expected to be small (typically 5-10). */
    char* visited[64];
    int visited_count = 0;
    int max_track = req->max_redirects + 1;
    if (max_track > 64) max_track = 64;
    visited[visited_count++] = strdup(req->url);

    int hops_remaining = req->max_redirects;
    char* current_url = strdup(req->url);
    if (!current_url || !visited[0]) {
        // Can't follow redirects without a copy of the starting URL; return the
        // first response as-is rather than dereferencing a NULL current_url below.
        free(current_url);
        free(visited[0]);
        return resp;
    }

    while (hops_remaining > 0 && resp && resp->status_code >= 300 && resp->status_code < 400) {
        const char* hdrs = resp->headers ? string_to_cstr(resp->headers) : "";
        char* location = http_extract_response_header(hdrs, "Location");
        if (!location) break;  /* 3xx with no Location → return as-is. */

        char* next_url = http_resolve_location(current_url, location);
        free(location);
        if (!next_url) {
            /* Redirect-class error — record on redirect_error so the
             * v2 send_request wrapper preserves the response and the
             * caller can still inspect the terminal 3xx. Issue #239. */
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new("malformed Location header");
            break;
        }

        /* Reject scheme downgrade: HTTPS origin → HTTP target. */
        int curr_https = strncmp(current_url, "https://", 8) == 0;
        int next_https = strncmp(next_url, "https://", 8) == 0;
        if (curr_https && !next_https) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect rejected: scheme downgrade (https -> http)");
            free(next_url);
            break;
        }

        /* Loop detection: refuse to revisit a URL within this chain. */
        int looped = 0;
        for (int i = 0; i < visited_count; i++) {
            if (visited[i] && strcmp(visited[i], next_url) == 0) {
                looped = 1;
                break;
            }
        }
        if (looped) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect loop detected (hop limit may have been exceeded)");
            free(next_url);
            break;
        }

        /* Strip credentials when the redirect leaves the ORIGIN, which is
         * scheme + host + port together, not the host alone (#1741). The port
         * and TLS flag were parsed here and then never compared, so a redirect
         * from http://host:8080/ to http://host:9090/ replayed the caller's
         * Authorization and Cookie headers at a different service on the same
         * machine, owned by whoever holds that port. An http -> https upgrade
         * changes the origin the same way. This is curl's rule: any of the
         * three changing drops the credentials.
         *
         * parse_url always fills the port from the scheme (80 / 443) and lets
         * an explicit :port override it, so http://host and http://host:80
         * compare equal and only a real origin change strips.
         *
         * parse_url returns 1 on success — same inverted-check bug as
         * http_resolve_location had above; without that fix the strip path
         * never fired in practice (because parse_url always succeeds for
         * well-formed URLs). */
        /* Only the origins are compared, so no path is parsed; and the
         * headers stay only when both origins parse and match. With a
         * 1024-byte path buffer a long path failed the parse, which skipped
         * the strip and sent the credentials to the new host (#2674). */
        char curr_host[256], next_host[256];
        int curr_port = 0, next_port = 0, curr_tls = 0, next_tls = 0;
        int same_origin =
            parse_url(current_url, curr_host, sizeof(curr_host), &curr_port,
                      NULL, 0, &curr_tls) != 0 &&
            parse_url(next_url, next_host, sizeof(next_host), &next_port,
                      NULL, 0, &next_tls) != 0 &&
            strcmp(curr_host, next_host) == 0 &&
            curr_port == next_port && curr_tls == next_tls;
        if (!same_origin) http_strip_cross_host_headers(req);

        /* Move on. Record the new URL, swap the request URL, send,
         * release the prior response. */
        if (visited_count < max_track) {
            visited[visited_count++] = strdup(next_url);
        }
        free(current_url);
        current_url = strdup(next_url);

        if (req->url_owned) free(req->url);
        req->url = next_url;  /* takes ownership */
        req->url_owned = 1;

        http_response_free(resp);
        resp = http_request_internal(req);
        if (!resp) break;

        hops_remaining--;
    }

    /* If we exited the loop because we ran out of hops while still
     * looking at a 3xx response, surface that as a redirect_error so
     * the caller can inspect the terminal 3xx status / body without
     * the v2 wrapper auto-freeing the response. */
    if (resp && hops_remaining == 0 && resp->status_code >= 300 && resp->status_code < 400) {
        if (resp->redirect_error) string_release(resp->redirect_error);
        resp->redirect_error = string_new("redirect hop limit reached");
    }

    /* Stash the final URL as the effective URL on the response. */
    if (resp) {
        if (resp->effective_url) string_release(resp->effective_url);
        resp->effective_url = string_new(current_url);
    }

    free(current_url);
    for (int i = 0; i < visited_count; i++) free(visited[i]);

    return resp;
}

/* v1 wrappers — thin sugar over the v2 builder. timeout=0 preserves
 * the original "block forever" behaviour callers had before v2. */

HttpResponse* http_get_raw(const char* url) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

// http_get_raw + per-call timeout. Closes the polling-loop pain
// where one hung site held the actor's whole AnalyzeNext handler
// hostage. Default `http_get_raw` blocks forever for backward
// compatibility; new callers should reach for this variant any time
// the URL is third-party or non-local.
HttpResponse* http_get_with_timeout_raw(const char* url, int timeout_ms) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (timeout_ms > 0) {
        // SO_RCVTIMEO / SO_SNDTIMEO storage is integer seconds; round
        // up so callers asking for 50ms get a 1s timeout rather than
        // accidentally disabling the timeout (0 = block forever). The
        // sub-second granularity gap is documented; ms-precision
        // timeouts are a separate change to the underlying field.
        int secs = (timeout_ms + 999) / 1000;
        http_request_set_timeout_raw(req, secs);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_get_with_timeout_ns_raw(const char* url, int64_t timeout_ns) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (http_request_set_timeout_ns_raw(req, timeout_ns) != 0) {
        http_request_free_raw(req);
        return NULL;
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_post_raw(const char* url, const char* body, const char* content_type) {
    HttpClientRequest* req = http_request_raw("POST", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_put_raw(const char* url, const char* body, const char* content_type) {
    HttpClientRequest* req = http_request_raw("PUT", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_delete_raw(const char* url) {
    HttpClientRequest* req = http_request_raw("DELETE", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

void http_response_free(HttpResponse* response) {
    if (!response) return;
    if (response->body) string_release(response->body);
    if (response->headers) string_release(response->headers);
    if (response->error) string_release(response->error);
    if (response->redirect_error) string_release(response->redirect_error);
    if (response->effective_url) string_release(response->effective_url);
    /* #1004: a streaming response owns its still-open transport; freeing the
     * response closes the socket/SSL and releases the pending buffer. This
     * also makes redirect-following safe for streaming requests: the redirect
     * loop frees each intermediate response before the next hop. */
    if (response->stream) http_stream_free(response->stream);
    free(response);
}

// Response accessors. All NULL-safe: callers can pass a NULL response
// (e.g. from an out-of-memory path) without crashing.

int http_response_status(HttpResponse* response) {
    if (!response) return 0;
    return response->status_code;
}

/* Returns an OWNED string: the response's body AetherString, retained so it
 * outlives http_response_free. Declared `@heap` on the Aether side, so codegen
 * releases the caller's ref at scope exit (aether_heap_str_free dispatches on
 * the magic header → string_release). This is what makes reading the body
 * AFTER response_free safe — a borrowed `->data` pointer would dangle, the
 * classic footgun in http-serve-and-dial-reentrancy-ask.md. On the empty/NULL
 * path we hand back a fresh empty AetherString rather than a literal "", so the
 * caller's uniform `@heap` release is always well-typed. */
const char* http_response_body(HttpResponse* response) {
    if (!response || !response->body) return (const char*)string_empty();
    string_retain(response->body);
    return (const char*)response->body;
}

int http_response_body_length(HttpResponse* response) {
    if (!response || !response->body) return 0;
    return (int)aether_string_length(response->body);
}

/* #1004: 1 if this response is streaming (body pulled incrementally via
 * http_response_read_chunk_raw), 0 if the body was buffered. */
int http_response_is_stream_raw(HttpResponse* response) {
    return (response && response->stream) ? 1 : 0;
}

/* #1004: pull the next decoded body window from a streaming response. Returns
 * a freshly-minted AetherString of up to `max` decoded body bytes (binary-safe
 * via its length). An EMPTY string means end-of-body OR a mid-stream transport
 * error; the two are disambiguated by http_response_error (set on error). For a
 * non-streaming or NULL response, returns an empty string. The caller owns the
 * returned string (`@heap` on the Aether side releases it at scope exit). */
const char* http_response_read_chunk_raw(HttpResponse* response, int max) {
    if (!response || !response->stream) return (const char*)string_empty();
    if (max <= 0) max = 65536;
    char* buf = (char*)malloc((size_t)max);
    if (!buf) {
        if (!response->error) response->error = string_new("out of memory");
        return (const char*)string_empty();
    }
    int n = stream_read_decoded(response->stream, buf, max);
    if (n < 0) {
        /* Mid-stream transport/framing error: surface via response->error so a
         * caller that sees an empty chunk can tell failure from clean EOF. */
        if (!response->error) response->error = string_new("stream read error");
        free(buf);
        return (const char*)string_empty();
    }
    AetherString* s = (n > 0) ? string_new_with_length(buf, (size_t)n) : string_empty();
    free(buf);
    return (const char*)s;
}

const char* http_response_headers(HttpResponse* response) {
    if (!response || !response->headers) return "";
    const char* s = string_to_cstr(response->headers);
    return s ? s : "";
}

const char* http_response_error(HttpResponse* response) {
    if (!response || !response->error) return "";
    const char* s = string_to_cstr(response->error);
    return s ? s : "";
}

int http_response_ok(HttpResponse* response) {
    if (!response) return 0;
    if (response->error) return 0;
    return response->status_code >= 200 && response->status_code < 300;
}

// Legacy accessor aliases — thin wrappers over the short names above.
int http_response_status_code(HttpResponse* response) {
    return http_response_status(response);
}

/* Borrowed-pointer variant, kept for the legacy `_str` accessor whose callers
 * copy-on-use (v1 wrappers, tinyweb). Unlike http_response_body (now `@heap`
 * owned), this returns a pointer into the response and does NOT retain, so it
 * must not be read after http_response_free. */
const char* http_response_body_str(HttpResponse* response) {
    if (!response || !response->body) return "";
    const char* s = string_to_cstr(response->body);
    return s ? s : "";
}

const char* http_response_headers_str(HttpResponse* response) {
    return http_response_headers(response);
}

/* Case-insensitive header lookup. Walks the raw header block stored
 * in response->headers (which still includes the HTTP status line as
 * the first "header"), splits each line at the first `:`, and matches
 * the name. Returns "" when the header isn't found.
 *
 * The returned pointer is into a per-response cache so it remains
 * valid until http_response_free(). Multiple values for the same
 * header are joined with ", " (RFC 7230 §3.2.2).
 *
 * Implementation: lazy single-pass. The cache is a singly-linked
 * list of (name, value) hung off the response — we don't pre-parse
 * into a hashmap because typical responses have <30 headers and the
 * lookup count per response is tiny. */
typedef struct HttpHeaderCache {
    char* name;
    char* value;
    struct HttpHeaderCache* next;
} HttpHeaderCache;

const char* http_response_header_raw(HttpResponse* response, const char* name) {
    if (!response || !name || !*name) return "";
    if (!response->headers) return "";

    /* The response struct doesn't have a parsed-headers field; we
     * cache by attaching to a per-thread arena. Simpler and adequate:
     * just walk the raw block on every call. The cost is O(n) in
     * header bytes, but typical headers are well under 4 KB and the
     * call count per response is small (callers grab the few headers
     * they care about and move on). If a profiler ever shows this in
     * the hot path, swap in a per-response cache.
     *
     * Joining duplicate-named headers into ", "-separated value is
     * done in a thread-local accumulator below. */
    static _Thread_local char tls_joined[8192];
    tls_joined[0] = '\0';
    size_t joined_len = 0;

    const char* hdr = string_to_cstr(response->headers);
    if (!hdr) return "";

    /* Skip the status line (first line — "HTTP/1.1 200 OK"). */
    const char* p = strchr(hdr, '\n');
    if (!p) return "";
    p++;

    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        /* Trim trailing \r if present. */
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        const char* colon = (const char*)memchr(p, ':', line_len);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            /* Match the header name case-insensitively. */
            if (nlen == strlen(name)) {
                int eq = 1;
                for (size_t i = 0; i < nlen; i++) {
                    char ca = p[i], cb = name[i];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) { eq = 0; break; }
                }
                if (eq) {
                    /* Skip ": " then trim leading spaces. */
                    const char* val = colon + 1;
                    size_t vlen = (size_t)((p + line_len) - val);
                    while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
                    /* Append to the joined accumulator. */
                    if (joined_len > 0 && joined_len + 2 < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, ", ", 2);
                        joined_len += 2;
                    }
                    if (joined_len + vlen < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, val, vlen);
                        joined_len += vlen;
                        tls_joined[joined_len] = '\0';
                    }
                }
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }

    return tls_joined;
}

const char* http_response_effective_url_raw(HttpResponse* response) {
    if (!response || !response->effective_url) return "";
    const char* s = string_to_cstr(response->effective_url);
    return s ? s : "";
}

const char* http_response_redirect_error_raw(HttpResponse* response) {
    if (!response || !response->redirect_error) return "";
    const char* s = string_to_cstr(response->redirect_error);
    return s ? s : "";
}

#endif // AETHER_HAS_NETWORKING
