// std.http.middleware — pre-handler middleware primitives.
// See aether_middleware.h for the API contract.
//
// Each middleware ships with:
//   - An options struct (config) the user constructs once at startup.
//   - A C function with the existing HttpMiddleware signature
//     `int(req, res, user_data) -> int`, registered via
//     http_server_use_middleware. Returns 1 to continue the chain,
//     0 to short-circuit.
//
// The hot path is straight C function pointers — no closure
// indirection, no Aether-side dispatch. Aether-side factory
// wrappers in std/http/middleware/module.ae allocate the user_data
// and register the middleware with the server.
#include "aether_middleware.h"
#include "../../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
// pthread types (pthread_mutex_t / lock / unlock / init / destroy) come
// in transitively via aether_middleware.h -> aether_http_server.h ->
// multicore_scheduler.h -> runtime/utils/aether_thread.h. That header
// provides a Win32 shim on MinGW; including <pthread.h> directly here
// would conflict with that shim. Do NOT add #include <pthread.h>.

// Borrow string accessors from the existing http surface.
extern const char* http_get_header(HttpRequest*, const char*);
extern void        http_response_set_status(HttpServerResponse*, int);
extern void        http_response_set_header(HttpServerResponse*, const char*, const char*);
extern void        http_response_set_body  (HttpServerResponse*, const char*);
extern const char* http_request_method(HttpRequest*);
extern const char* http_request_path  (HttpRequest*);

// -----------------------------------------------------------------
// CORS
// -----------------------------------------------------------------
struct AetherCorsOpts {
    char* allow_origin;       /* NULL/"" -> don't emit */
    char* allow_methods;
    char* allow_headers;
    int   allow_credentials;
    int   max_age_seconds;    /* <= 0 -> don't emit Max-Age */
};

static char* dup_or_null(const char* s) {
    if (!s || !*s) return NULL;
    return strdup(s);
}

AetherCorsOpts* aether_cors_opts_new(const char* allow_origin,
                                     const char* allow_methods,
                                     const char* allow_headers,
                                     int allow_credentials,
                                     int max_age_seconds) {
    AetherCorsOpts* o = (AetherCorsOpts*)calloc(1, sizeof(AetherCorsOpts));
    if (!o) return NULL;
    o->allow_origin      = dup_or_null(allow_origin);
    o->allow_methods     = dup_or_null(allow_methods);
    o->allow_headers     = dup_or_null(allow_headers);
    o->allow_credentials = allow_credentials ? 1 : 0;
    o->max_age_seconds   = max_age_seconds;
    return o;
}

void aether_cors_opts_free(AetherCorsOpts* o) {
    if (!o) return;
    free(o->allow_origin);
    free(o->allow_methods);
    free(o->allow_headers);
    free(o);
}

int aether_middleware_cors(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherCorsOpts* o = (AetherCorsOpts*)user_data;
    if (!o) return 1;

    /* CORS headers go on every response, not just preflight. */
    if (o->allow_origin)  http_response_set_header(res, "Access-Control-Allow-Origin",  o->allow_origin);
    if (o->allow_methods) http_response_set_header(res, "Access-Control-Allow-Methods", o->allow_methods);
    if (o->allow_headers) http_response_set_header(res, "Access-Control-Allow-Headers", o->allow_headers);
    if (o->allow_credentials)
        http_response_set_header(res, "Access-Control-Allow-Credentials", "true");

    /* Preflight OPTIONS — answer with 204 + Max-Age and short-circuit
     * the chain. The route handler never runs for preflight requests. */
    const char* method = http_request_method(req);
    if (method && strcasecmp(method, "OPTIONS") == 0) {
        if (o->max_age_seconds > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", o->max_age_seconds);
            http_response_set_header(res, "Access-Control-Max-Age", buf);
        }
        http_response_set_status(res, 204);
        http_response_set_body(res, "");
        return 0;  /* short-circuit */
    }
    return 1;  /* continue chain */
}

// -----------------------------------------------------------------
// Basic Authentication
// -----------------------------------------------------------------
struct AetherBasicAuthOpts {
    char* realm;
    AetherBasicAuthVerifier verify;
    void* verifier_user_data;
};

AetherBasicAuthOpts* aether_basic_auth_opts_new(const char* realm,
                                                AetherBasicAuthVerifier verify,
                                                void* verifier_user_data) {
    if (!verify) return NULL;
    AetherBasicAuthOpts* o = (AetherBasicAuthOpts*)calloc(1, sizeof(AetherBasicAuthOpts));
    if (!o) return NULL;
    o->realm = realm && *realm ? strdup(realm) : strdup("Restricted");
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_basic_auth_opts_free(AetherBasicAuthOpts* o) {
    if (!o) return;
    free(o->realm);
    free(o);
}

/* RFC 4648 base64 alphabet. Decoding tolerant of whitespace + padding. */
static int b64_decode_char(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode a base64 string. Caller frees. Returns NULL on malformed
 * input. *out_len receives the decoded byte count. */
static char* b64_decode(const char* src, size_t* out_len) {
    size_t src_len = strlen(src);
    char* out = (char*)malloc(src_len + 1);
    if (!out) return NULL;
    /* `val` keeps only the bits not yet written, fewer than 8 between
     * characters. An int that kept every bit overflowed after six
     * characters, which C leaves undefined (UBSan, #2488). */
    int bits = 0;
    unsigned int val = 0;
    size_t out_pos = 0;
    for (size_t i = 0; i < src_len; i++) {
        int c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int d = b64_decode_char(c);
        if (d < 0) { free(out); return NULL; }
        val = (val << 6) | (unsigned int)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_pos++] = (char)((val >> bits) & 0xff);
            val &= (1u << bits) - 1u;
        }
    }
    out[out_pos] = '\0';
    *out_len = out_pos;
    return out;
}

int aether_middleware_basic_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherBasicAuthOpts* o = (AetherBasicAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* auth = http_get_header(req, "Authorization");
    if (!auth || strncasecmp(auth, "Basic ", 6) != 0) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Basic realm=\"%s\"", o->realm);
        http_response_set_header(res, "WWW-Authenticate", hdr);
        http_response_set_status(res, 401);
        http_response_set_body(res, "Unauthorized");
        return 0;
    }

    const char* b64 = auth + 6;
    while (*b64 == ' ' || *b64 == '\t') b64++;

    size_t dec_len = 0;
    char* decoded = b64_decode(b64, &dec_len);
    if (!decoded) {
        http_response_set_status(res, 400);
        http_response_set_body(res, "Bad Authorization header");
        return 0;
    }
    char* sep = strchr(decoded, ':');
    if (!sep) {
        free(decoded);
        http_response_set_status(res, 400);
        http_response_set_body(res, "Bad Authorization payload");
        return 0;
    }
    *sep = '\0';
    const char* user = decoded;
    const char* pass = sep + 1;

    int ok = o->verify(user, pass, o->verifier_user_data);
    free(decoded);

    if (!ok) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Basic realm=\"%s\"", o->realm);
        http_response_set_header(res, "WWW-Authenticate", hdr);
        http_response_set_status(res, 401);
        http_response_set_body(res, "Unauthorized");
        return 0;
    }
    return 1;  /* authorised — continue */
}

// -----------------------------------------------------------------
// Bearer Token Authentication (RFC 6750)
// -----------------------------------------------------------------
struct AetherBearerAuthOpts {
    char* realm;
    AetherBearerAuthVerifier verify;
    void* verifier_user_data;
};

AetherBearerAuthOpts* aether_bearer_auth_opts_new(const char* realm,
                                                  AetherBearerAuthVerifier verify,
                                                  void* verifier_user_data) {
    if (!verify) return NULL;
    AetherBearerAuthOpts* o = (AetherBearerAuthOpts*)calloc(1, sizeof(AetherBearerAuthOpts));
    if (!o) return NULL;
    o->realm = realm && *realm ? strdup(realm) : strdup("api");
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_bearer_auth_opts_free(AetherBearerAuthOpts* o) {
    if (!o) return;
    free(o->realm);
    free(o);
}

/* Per RFC 6750 §3.1: the Bearer challenge can carry an `error`
 * parameter that distinguishes "no credentials" from "bad
 * credentials". Conforming clients (curl, OAuth libraries) use
 * this to decide whether to retry vs. prompt the user. */
static void bearer_challenge(HttpServerResponse* res,
                             const char* realm,
                             const char* error) {
    char hdr[512];
    if (error && *error) {
        snprintf(hdr, sizeof(hdr),
                 "Bearer realm=\"%s\", error=\"%s\"", realm, error);
    } else {
        snprintf(hdr, sizeof(hdr), "Bearer realm=\"%s\"", realm);
    }
    http_response_set_header(res, "WWW-Authenticate", hdr);
    http_response_set_status(res, 401);
    http_response_set_body(res, "Unauthorized");
}

int aether_middleware_bearer_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherBearerAuthOpts* o = (AetherBearerAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* auth = http_get_header(req, "Authorization");
    /* "Authorization: Bearer " arrives as "Bearer": a field value does not
     * hold the whitespace after it (RFC 9112 5), and the parser trims it.
     * It is the scheme with no token, as before. */
    int scheme_only = auth && strcasecmp(auth, "Bearer") == 0;
    if (!auth || (!scheme_only && strncasecmp(auth, "Bearer ", 7) != 0)) {
        /* No credential — base challenge, no `error` param. */
        bearer_challenge(res, o->realm, NULL);
        return 0;
    }

    /* Skip the "Bearer " prefix and any whitespace. The token itself
     * is RFC-defined as 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" /
     * "+" / "/" ) [ "=" *"=" ] but we don't enforce — verifiers
     * commonly accept tighter or looser shapes (JWTs are dotted,
     * opaque tokens are URL-safe base64, etc.). */
    const char* token = scheme_only ? auth + 6 : auth + 7;
    while (*token == ' ' || *token == '\t') token++;

    if (!*token) {
        /* "Bearer" with no token. Treat as malformed credential. */
        bearer_challenge(res, o->realm, "invalid_token");
        return 0;
    }

    int ok = o->verify(token, o->verifier_user_data);
    if (!ok) {
        bearer_challenge(res, o->realm, "invalid_token");
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------
// Session-Cookie Authentication
// -----------------------------------------------------------------
struct AetherSessionAuthOpts {
    char* cookie_name;
    char* redirect_url;          /* NULL -> 401, non-NULL -> 302 */
    AetherSessionAuthVerifier verify;
    void* verifier_user_data;
};

AetherSessionAuthOpts* aether_session_auth_opts_new(const char* cookie_name,
                                                    const char* redirect_url,
                                                    AetherSessionAuthVerifier verify,
                                                    void* verifier_user_data) {
    if (!verify || !cookie_name || !*cookie_name) return NULL;
    AetherSessionAuthOpts* o = (AetherSessionAuthOpts*)calloc(1, sizeof(AetherSessionAuthOpts));
    if (!o) return NULL;
    o->cookie_name = strdup(cookie_name);
    // cookie_name is compared on every request; a NULL would crash that lookup.
    // (redirect_url is optional, so a NULL there is a valid "no redirect".)
    if (!o->cookie_name) { free(o); return NULL; }
    o->redirect_url = (redirect_url && *redirect_url) ? strdup(redirect_url) : NULL;
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_session_auth_opts_free(AetherSessionAuthOpts* o) {
    if (!o) return;
    free(o->cookie_name);
    free(o->redirect_url);
    free(o);
}

/* Extract the value of cookie `name` from a Cookie: header value.
 * Cookie syntax (RFC 6265 §4.2.1): `name1=val1; name2=val2; ...`.
 * Names match case-sensitively per RFC 6265. Cookie values can
 * contain anything except `;` `,` whitespace or control chars in
 * the unquoted form, or anything except `"` and control chars in
 * the quoted form. Quoted values: surrounding `"` is stripped.
 *
 * Returns a heap-allocated copy of the matched value, or NULL if
 * absent. Caller frees. */
static char* cookie_extract(const char* header_value, const char* name) {
    if (!header_value || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = header_value;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char* eq = strchr(p, '=');
        if (!eq) break;
        size_t key_len = (size_t)(eq - p);
        /* Trim trailing whitespace from key. */
        while (key_len > 0 && (p[key_len - 1] == ' ' || p[key_len - 1] == '\t')) key_len--;

        const char* val_start = eq + 1;
        const char* val_end = strchr(val_start, ';');
        if (!val_end) val_end = val_start + strlen(val_start);

        if (key_len == name_len && strncmp(p, name, name_len) == 0) {
            const char* vs = val_start;
            const char* ve = val_end;
            /* Strip surrounding quotes if present. */
            if (ve > vs && *vs == '"' && ve[-1] == '"') {
                vs++;
                ve--;
            }
            size_t vlen = (size_t)(ve - vs);
            char* out = (char*)malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, vs, vlen);
            out[vlen] = '\0';
            return out;
        }

        p = (*val_end == ';') ? val_end + 1 : val_end;
    }
    return NULL;
}

int aether_middleware_session_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherSessionAuthOpts* o = (AetherSessionAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* cookie_hdr = http_get_header(req, "Cookie");
    char* value = cookie_hdr ? cookie_extract(cookie_hdr, o->cookie_name) : NULL;

    int ok = (value != NULL) && o->verify(value, o->verifier_user_data);
    free(value);

    if (!ok) {
        if (o->redirect_url) {
            http_response_set_header(res, "Location", o->redirect_url);
            http_response_set_status(res, 302);
            http_response_set_body(res, "");
        } else {
            http_response_set_status(res, 401);
            http_response_set_body(res, "Unauthorized");
        }
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------
// Token-bucket rate limiter (per-client-IP)
// -----------------------------------------------------------------
typedef struct RateBucket {
    char* key;          /* the client's address, or "anonymous" */
    double tokens;      /* current bucket level */
    int64_t last_refill; /* monotonic ms */
    struct RateBucket* next;                 /* hash chain */
    struct RateBucket* lru_prev;             /* most recently seen first */
    struct RateBucket* lru_next;
    unsigned int slot;
} RateBucket;

/* The clients a limiter holds a bucket for at most, and the chains they are
 * found by. Addresses are the clients' to have (one IPv6 client holds more
 * than any table), so the table is bounded: at the bound the client seen
 * least recently gives up its bucket, as nginx's limit_req zone does when it
 * is full. Kept without a bound, a client cycling addresses grew the table
 * for good, under the lock every request takes (#3266). */
#define RATE_TABLE_SIZE 4096
#define RATE_MAX_KEYS   16384

struct AetherRateLimitOpts {
    int max_requests;
    int window_ms;
    pthread_mutex_t lock;
    RateBucket* table[RATE_TABLE_SIZE];
    RateBucket* lru_head;    /* seen most recently */
    RateBucket* lru_tail;    /* seen least recently, the next to go */
    int         keys;
};

static void rate_lru_unlink(AetherRateLimitOpts* o, RateBucket* b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else             o->lru_head = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else             o->lru_tail = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}

static void rate_lru_push(AetherRateLimitOpts* o, RateBucket* b) {
    b->lru_prev = NULL;
    b->lru_next = o->lru_head;
    if (o->lru_head) o->lru_head->lru_prev = b;
    o->lru_head = b;
    if (!o->lru_tail) o->lru_tail = b;
}

/* Drop the bucket of the client seen least recently. Called with the lock
 * held, when the table is at RATE_MAX_KEYS. */
static void rate_evict_oldest(AetherRateLimitOpts* o) {
    RateBucket* b = o->lru_tail;
    if (!b) return;
    rate_lru_unlink(o, b);
    RateBucket** at = &o->table[b->slot];
    while (*at && *at != b) at = &(*at)->next;
    if (*at) *at = b->next;
    free(b->key);
    free(b);
    o->keys--;
}

/* In 64 bits. In a `long`, 32 bits on Windows, the product overflowed after
 * 24.8 days of uptime (time since boot there), undefined behaviour that only
 * happened to wrap, and a bucket left idle longer than that read a negative
 * elapsed time and did not refill until as long again had passed (#3096). */
static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int rate_hash(const char* s) {
    unsigned int h = 5381;
    while (*s) { h = ((h << 5) + h) + (unsigned char)*s; s++; }
    return h;
}

AetherRateLimitOpts* aether_rate_limit_opts_new(int max_requests, int window_ms) {
    if (max_requests <= 0 || window_ms <= 0) return NULL;
    AetherRateLimitOpts* o = (AetherRateLimitOpts*)calloc(1, sizeof(AetherRateLimitOpts));
    if (!o) return NULL;
    o->max_requests = max_requests;
    o->window_ms = window_ms;
    pthread_mutex_init(&o->lock, NULL);
    return o;
}

int aether_rate_limit_key_count(AetherRateLimitOpts* o) {
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    int n = o->keys;
    pthread_mutex_unlock(&o->lock);
    return n;
}

void aether_rate_limit_opts_free(AetherRateLimitOpts* o) {
    if (!o) return;
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        RateBucket* b = o->table[i];
        while (b) {
            RateBucket* next = b->next;
            free(b->key);
            free(b);
            b = next;
        }
    }
    pthread_mutex_destroy(&o->lock);
    free(o);
}

int aether_middleware_rate_limit(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherRateLimitOpts* o = (AetherRateLimitOpts*)user_data;
    if (!o) return 1;

    /* The client is the peer the request reports (#3173): the
     * connection's, or, after use_real_ip, the client it resolved from a
     * trusted chain (#3169). Never a header: X-Forwarded-For and
     * X-Real-IP are what the client sends, and keyed on them a client
     * that sent a fresh value on every request got a fresh bucket and was
     * never limited. A request with no IP peer (a Unix-domain socket)
     * shares the "anonymous" bucket. */
    const char* key = http_request_remote_addr(req);
    if (!key || !*key) key = "anonymous";
    char keybuf[128];
    size_t klen = strlen(key);
    if (klen >= sizeof(keybuf)) klen = sizeof(keybuf) - 1;
    memcpy(keybuf, key, klen);
    keybuf[klen] = '\0';

    unsigned int slot = rate_hash(keybuf) % RATE_TABLE_SIZE;
    int64_t now = monotonic_ms();

    pthread_mutex_lock(&o->lock);
    RateBucket* bucket = o->table[slot];
    while (bucket && strcmp(bucket->key, keybuf) != 0) bucket = bucket->next;
    if (bucket) {
        /* Seen now: the last to give up its bucket (#3266). */
        if (o->lru_head != bucket) {
            rate_lru_unlink(o, bucket);
            rate_lru_push(o, bucket);
        }
    } else {
        if (o->keys >= RATE_MAX_KEYS) rate_evict_oldest(o);
        bucket = (RateBucket*)calloc(1, sizeof(RateBucket));
        if (!bucket) {
            pthread_mutex_unlock(&o->lock);
            return 1;  /* fail open on OOM */
        }
        bucket->key = strdup(keybuf);
        if (!bucket->key) {
            // A NULL key would crash the strcmp bucket lookup on the next
            // request; drop the bucket and fail open like the calloc path above.
            free(bucket);
            pthread_mutex_unlock(&o->lock);
            return 1;  /* fail open on OOM */
        }
        bucket->tokens = (double)o->max_requests;
        bucket->last_refill = now;
        bucket->slot = slot;
        bucket->next = o->table[slot];
        o->table[slot] = bucket;
        rate_lru_push(o, bucket);
        o->keys++;
    }

    /* Continuous refill: rate = max_requests / window_ms tokens per ms. */
    int64_t elapsed = now - bucket->last_refill;
    if (elapsed > 0) {
        double refill = (double)elapsed * (double)o->max_requests / (double)o->window_ms;
        bucket->tokens += refill;
        if (bucket->tokens > (double)o->max_requests) {
            bucket->tokens = (double)o->max_requests;
        }
        bucket->last_refill = now;
    }

    if (bucket->tokens < 1.0) {
        /* Out of tokens — compute Retry-After in seconds. */
        double need = 1.0 - bucket->tokens;
        double per_ms = (double)o->max_requests / (double)o->window_ms;
        int retry_sec = (int)(need / per_ms / 1000.0) + 1;
        pthread_mutex_unlock(&o->lock);

        char buf[32];
        snprintf(buf, sizeof(buf), "%d", retry_sec);
        http_response_set_header(res, "Retry-After", buf);
        http_response_set_status(res, 429);
        http_response_set_body(res, "Too Many Requests");
        return 0;
    }
    bucket->tokens -= 1.0;
    pthread_mutex_unlock(&o->lock);
    return 1;
}

// -----------------------------------------------------------------
// Virtual host
// -----------------------------------------------------------------
struct AetherVhostMap {
    char** hosts;
    int    count;
    int    cap;
    int    fallback_status;  /* 0 -> use 404 */
};

AetherVhostMap* aether_vhost_map_new(int fallback_status) {
    AetherVhostMap* m = (AetherVhostMap*)calloc(1, sizeof(AetherVhostMap));
    if (!m) return NULL;
    m->fallback_status = fallback_status > 0 ? fallback_status : 404;
    return m;
}

void aether_vhost_map_free(AetherVhostMap* m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) free(m->hosts[i]);
    free(m->hosts);
    free(m);
}

int aether_vhost_register_host(AetherVhostMap* m, const char* host) {
    if (!m || !host || !*host) return -1;
    if (m->count >= m->cap) {
        int new_cap = m->cap > 0 ? m->cap * 2 : 4;
        char** nh = (char**)realloc(m->hosts, new_cap * sizeof(char*));
        if (!nh) return -1;
        m->hosts = nh;
        m->cap = new_cap;
    }
    // Don't store a NULL on allocation failure and then report success: a NULL
    // entry would crash the strcmp in aether_middleware_vhost on the next
    // request. Leave the map unchanged and signal failure instead.
    char* dup = strdup(host);
    if (!dup) return -1;
    m->hosts[m->count++] = dup;
    return 0;
}

int aether_middleware_vhost(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherVhostMap* m = (AetherVhostMap*)user_data;
    if (!m) return 1;
    const char* host_hdr = http_get_header(req, "Host");
    if (!host_hdr) host_hdr = "";

    /* Strip the optional :port suffix for the comparison. */
    char host[256];
    size_t hlen = 0;
    while (*host_hdr && *host_hdr != ':' && hlen + 1 < sizeof(host)) {
        host[hlen++] = (char)tolower((unsigned char)*host_hdr++);
    }
    host[hlen] = '\0';

    for (int i = 0; i < m->count; i++) {
        if (strcasecmp(m->hosts[i], host) == 0) return 1;  /* allowed */
    }

    http_response_set_status(res, m->fallback_status);
    http_response_set_body(res, "Unknown host");
    return 0;
}

// =================================================================
// D2 — gzip / static_files / rewrite / error_pages
// =================================================================

// -----------------------------------------------------------------
// gzip response transformer
// -----------------------------------------------------------------
#ifdef AETHER_HAS_ZLIB
#include <zlib.h>
#endif

struct AetherGzipOpts {
    int min_size;
    int level;
};

AetherGzipOpts* aether_gzip_opts_new(int min_size, int level) {
    AetherGzipOpts* o = (AetherGzipOpts*)calloc(1, sizeof(AetherGzipOpts));
    if (!o) return NULL;
    o->min_size = min_size > 0 ? min_size : 256;
    if (level < 1 || level > 9) level = 6;
    o->level = level;
    return o;
}

void aether_gzip_opts_free(AetherGzipOpts* o) { free(o); }

#ifdef AETHER_HAS_ZLIB
/* Compress `in_data` (in_len bytes) into a freshly allocated gzip
 * stream. Returns the buffer and writes its compressed length into
 * *out_len and the underlying allocation size into *out_cap. NULL on
 * failure. Uses windowBits=15+16 to produce gzip framing (header +
 * deflate + CRC + ISIZE).
 *
 * Cap-aware (#343): the buffer is allocated via aether_caps_malloc,
 * so the caller MUST free it with aether_caps_free(buf, *out_cap) —
 * passing the allocation size (not out_len) keeps the cap counter
 * balanced. */
static unsigned char* gzip_compress(const unsigned char* in_data, size_t in_len,
                                    int level, size_t* out_len, size_t* out_cap) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* 15+16: max window + gzip wrapper. -15 alone would be raw
     * deflate; +16 tells zlib to emit gzip framing. */
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    uLong bound = deflateBound(&strm, (uLong)in_len);
    /* bound is derived from in_len which is the response body length
     * — caller-controlled but plugin-reachable, so the alloc is
     * gated by the cap. */
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) { deflateEnd(&strm); return NULL; }
    *out_cap = alloc_cap;
    strm.next_in = (Bytef*)in_data;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;
    int rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&strm);
        aether_caps_free(out, alloc_cap);
        return NULL;
    }
    *out_len = strm.total_out;
    deflateEnd(&strm);
    return out;
}
#endif

void aether_xform_gzip(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherGzipOpts* o = (AetherGzipOpts*)user_data;
    if (!o || !res || !res->body || res->body_length < (size_t)o->min_size) return;
    /* Don't double-encode if the handler already set Content-Encoding. */
    for (int i = 0; i < res->header_count; i++) {
        if (res->header_keys[i] &&
            strcasecmp(res->header_keys[i], "Content-Encoding") == 0) {
            return;
        }
    }
    /* Client must accept gzip. Tolerant matching — Accept-Encoding can
     * carry multiple tokens with q-values; a substring search for
     * "gzip" is good enough for the common case. */
    const char* ae = http_get_header(req, "Accept-Encoding");
    if (!ae) return;
    int wants_gzip = 0;
    for (const char* p = ae; *p; p++) {
        if ((p == ae || !isalnum((unsigned char)p[-1])) &&
            strncasecmp(p, "gzip", 4) == 0) {
            wants_gzip = 1;
            break;
        }
    }
    if (!wants_gzip) return;

#ifdef AETHER_HAS_ZLIB
    size_t out_len = 0;
    size_t comp_cap = 0;
    unsigned char* compressed = gzip_compress((const unsigned char*)res->body,
                                              res->body_length, o->level,
                                              &out_len, &comp_cap);
    if (!compressed) return;  /* leave response untouched on compression failure */
    if (out_len >= res->body_length) {
        /* Compression didn't help (already-compressed body, e.g.
         * already-gzipped image). Discard and leave alone. Free the
         * caps-allocated buffer with its allocation size. */
        aether_caps_free(compressed, comp_cap);
        return;
    }
    /* Replace body in place. The response struct owns the body
     * string via its own free path (http_server_response_free),
     * which calls aether_caps_free(res->body, res->body_cap) — so
     * the replacement must go through the cap allocator and keep
     * res->body_cap in sync, otherwise the cap counter drifts and
     * the eventual free passes a wrong size. Free the old body the
     * same way the server does (caps-aware with the tracked cap;
     * heap-safe even when the body was a libc strdup from
     * http_response_set_body, where body_cap is 0).
     * Body is binary (deflate stream); the trailing NUL is
     * defensive only — the wire path uses res->body_length, not
     * strlen(res->body). */
    aether_caps_free(res->body, res->body_cap);
    size_t body_cap = out_len + 1;
    res->body = (char*)aether_caps_malloc(body_cap);
    if (!res->body) {
        aether_caps_free(compressed, comp_cap);
        res->body_length = 0;
        res->body_cap = 0;
        return;
    }
    memcpy(res->body, compressed, out_len);
    res->body[out_len] = '\0';
    res->body_length = out_len;
    res->body_cap = body_cap;
    aether_caps_free(compressed, comp_cap);

    /* Sync Content-Length to the post-compression size, replacing
     * the value the route handler set via http_response_set_body.
     * Without this, the wire response advertises the original
     * length and the client either truncates or errors out. */
    char clbuf[32];
    snprintf(clbuf, sizeof(clbuf), "%zu", out_len);
    http_response_set_header(res, "Content-Length",   clbuf);
    http_response_set_header(res, "Content-Encoding", "gzip");
    http_response_set_header(res, "Vary",             "Accept-Encoding");
#else
    (void)res;
#endif
}

// -----------------------------------------------------------------
// Static file serving
// -----------------------------------------------------------------
struct AetherStaticOpts {
    char* url_prefix;   /* e.g. "/assets" or "" */
    char* root;         /* e.g. "/var/www/static" */
    size_t prefix_len;
};

AetherStaticOpts* aether_static_opts_new(const char* url_prefix, const char* root) {
    if (!root || !*root) return NULL;
    AetherStaticOpts* o = (AetherStaticOpts*)calloc(1, sizeof(AetherStaticOpts));
    if (!o) return NULL;
    o->url_prefix = strdup(url_prefix ? url_prefix : "");
    o->root = strdup(root);
    // A NULL url_prefix would crash the strlen just below; a NULL root would
    // crash path resolution when serving. Bail cleanly instead.
    if (!o->url_prefix || !o->root) {
        free(o->url_prefix);
        free(o->root);
        free(o);
        return NULL;
    }
    o->prefix_len = strlen(o->url_prefix);
    return o;
}

void aether_static_opts_free(AetherStaticOpts* o) {
    if (!o) return;
    free(o->url_prefix);
    free(o->root);
    free(o);
}

extern const char* http_mime_type(const char* path);
extern void http_serve_file(HttpServerResponse* res, const char* filepath);

int aether_middleware_static(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherStaticOpts* o = (AetherStaticOpts*)user_data;
    if (!o) return 1;
    const char* path = http_request_path(req);
    if (!path) return 1;

    /* Path must start with the configured prefix. Prefix "" matches
     * everything (mount the static directory at root). */
    if (o->prefix_len > 0 &&
        strncmp(path, o->url_prefix, o->prefix_len) != 0) {
        return 1;  /* not ours, continue chain */
    }
    const char* tail = path + o->prefix_len;
    if (*tail == '\0') tail = "/";

    /* Block path traversal — refuse any "/.." segment. */
    if (strstr(tail, "/..") || strstr(tail, "../") ||
        strcmp(tail, "..") == 0) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "Forbidden");
        return 0;
    }

    /* Build the filesystem path: <root> + <tail>. */
    char fspath[1024];
    int n = snprintf(fspath, sizeof(fspath), "%s%s%s",
                     o->root,
                     (tail[0] == '/' ? "" : "/"),
                     tail);
    if (n <= 0 || n >= (int)sizeof(fspath)) {
        http_response_set_status(res, 414);
        http_response_set_body(res, "URI too long");
        return 0;
    }

    /* http_serve_file populates the response on success or sets a
     * 404 / 500 on failure. Either way, short-circuit the chain
     * once we've taken responsibility for the path. */
    http_serve_file(res, fspath);
    return 0;
}

// -----------------------------------------------------------------
// URL rewriting
// -----------------------------------------------------------------
typedef struct RewriteRule {
    char* from_prefix;
    char* to_prefix;
    size_t from_len;
} RewriteRule;

struct AetherRewriteOpts {
    RewriteRule* rules;
    int count;
    int cap;
};

AetherRewriteOpts* aether_rewrite_opts_new(void) {
    return (AetherRewriteOpts*)calloc(1, sizeof(AetherRewriteOpts));
}

void aether_rewrite_opts_free(AetherRewriteOpts* o) {
    if (!o) return;
    for (int i = 0; i < o->count; i++) {
        free(o->rules[i].from_prefix);
        free(o->rules[i].to_prefix);
    }
    free(o->rules);
    free(o);
}

int aether_rewrite_add_rule(AetherRewriteOpts* o,
                            const char* from_prefix,
                            const char* to_prefix) {
    if (!o || !from_prefix || !to_prefix) return -1;
    if (o->count >= o->cap) {
        int new_cap = o->cap > 0 ? o->cap * 2 : 4;
        RewriteRule* nr = (RewriteRule*)realloc(o->rules, new_cap * sizeof(RewriteRule));
        if (!nr) return -1;
        o->rules = nr;
        o->cap = new_cap;
    }
    // Allocate both strings up front; on failure store neither (a half-added
    // rule with a NULL prefix would crash the rewrite path) and report failure.
    char* fp = strdup(from_prefix);
    char* tp = strdup(to_prefix);
    if (!fp || !tp) { free(fp); free(tp); return -1; }
    o->rules[o->count].from_prefix = fp;
    o->rules[o->count].to_prefix = tp;
    o->rules[o->count].from_len = strlen(from_prefix);
    o->count++;
    return 0;
}

/* Replace req->path through http_request_set_path, which knows how the
 * request holds it (#3234): a free() of the old path handed an arena-backed
 * request's path to the allocator that never issued it. */
int aether_middleware_rewrite(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)res;
    AetherRewriteOpts* o = (AetherRewriteOpts*)user_data;
    if (!o || !req || !req->path) return 1;

    for (int i = 0; i < o->count; i++) {
        RewriteRule* r = &o->rules[i];
        if (strncmp(req->path, r->from_prefix, r->from_len) == 0) {
            const char* tail = req->path + r->from_len;
            size_t need = strlen(r->to_prefix) + strlen(tail) + 1;
            char* np = (char*)malloc(need);
            if (!np) return 1;  /* fail open on OOM */
            snprintf(np, need, "%s%s", r->to_prefix, tail);
            http_request_set_path(req, np);   /* unchanged on OOM: fail open */
            free(np);
            return 1;  /* first match wins; continue chain */
        }
    }
    return 1;
}

// -----------------------------------------------------------------
// Custom error pages
// -----------------------------------------------------------------
typedef struct ErrorPage {
    int    status;
    char*  body;
    char*  content_type;
} ErrorPage;

struct AetherErrorPagesOpts {
    ErrorPage* pages;
    int count;
    int cap;
};

AetherErrorPagesOpts* aether_error_pages_opts_new(void) {
    return (AetherErrorPagesOpts*)calloc(1, sizeof(AetherErrorPagesOpts));
}

void aether_error_pages_opts_free(AetherErrorPagesOpts* o) {
    if (!o) return;
    for (int i = 0; i < o->count; i++) {
        free(o->pages[i].body);
        free(o->pages[i].content_type);
    }
    free(o->pages);
    free(o);
}

int aether_error_pages_register(AetherErrorPagesOpts* o,
                                int status_code,
                                const char* body,
                                const char* content_type) {
    if (!o || status_code < 100 || status_code > 599 || !body) return -1;
    if (o->count >= o->cap) {
        int new_cap = o->cap > 0 ? o->cap * 2 : 4;
        ErrorPage* np = (ErrorPage*)realloc(o->pages, new_cap * sizeof(ErrorPage));
        if (!np) return -1;
        o->pages = np;
        o->cap = new_cap;
    }
    // Allocate both strings up front; on failure store neither (a NULL body or
    // content_type would crash when serving the custom error page) and report
    // failure rather than leaving a corrupt page entry.
    char* body_dup = strdup(body);
    char* ct_dup = (content_type && *content_type)
                       ? strdup(content_type) : strdup("text/html; charset=utf-8");
    if (!body_dup || !ct_dup) { free(body_dup); free(ct_dup); return -1; }
    o->pages[o->count].status = status_code;
    o->pages[o->count].body = body_dup;
    o->pages[o->count].content_type = ct_dup;
    o->count++;
    return 0;
}

void aether_xform_error_pages(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)req;
    AetherErrorPagesOpts* o = (AetherErrorPagesOpts*)user_data;
    if (!o || !res) return;
    for (int i = 0; i < o->count; i++) {
        if (o->pages[i].status == res->status_code) {
            http_response_set_body(res, o->pages[i].body);
            http_response_set_header(res, "Content-Type", o->pages[i].content_type);
            return;
        }
    }
}

// -----------------------------------------------------------------
// Real-IP / X-Forwarded-For
// -----------------------------------------------------------------
/* An address or a network of them (#3212): 16 bytes, an IPv4 address as
 * ::ffff:a.b.c.d, and how many leading bits a match shares with it. */
typedef struct {
    unsigned char addr[16];
    int bits;
} RealIpNet;

struct AetherRealIpOpts {
    char* header_name;      /* e.g. "X-Forwarded-For" */
    int forwarded;          /* RFC 7239 `Forwarded`: the for= of each element */
    RealIpNet* trusted;     /* the proxies whose word is taken */
    int trusted_count;
};

/* Addresses are read and written here, not with inet_pton and inet_ntop:
 * this is text, and the socket headers those come with are not on every
 * target std builds for (WASI has none). The rules are inet_pton's. */

/* An IPv4 address, the `len` bytes at `s`: 1, its four bytes in `out`.
 * Four decimal parts of 0 to 255, none with a leading zero. */
static int real_ip_parse_v4(const char* s, size_t len, unsigned char out[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (i >= len || s[i] != '.') return 0;
            i++;
        }
        if (i >= len || !isdigit((unsigned char)s[i])) return 0;
        int v = 0, digits = 0;
        while (i < len && isdigit((unsigned char)s[i])) {
            if (digits > 0 && v == 0) return 0;   /* a leading zero */
            v = v * 10 + (s[i] - '0');
            digits++;
            i++;
            if (v > 255) return 0;
        }
        out[part] = (unsigned char)v;
    }
    return i == len;
}

static int real_ip_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* An IPv6 address: 1, its sixteen bytes. Groups of up to four hex digits,
 * one `::` standing for one or more zero groups, and an IPv4 address in
 * the last 32 bits. */
static int real_ip_parse_v6(const char* s, size_t len, unsigned char out[16]) {
    unsigned char buf[16] = { 0 };
    int n = 0;      /* bytes read so far */
    int gap = -1;   /* where the `::` stands, in bytes */
    size_t i = 0;
    if (len >= 1 && s[0] == ':') {
        if (len < 2 || s[1] != ':') return 0;
        gap = 0;
        i = 2;
    }
    while (i < len) {
        if (n >= 16) return 0;
        size_t j = i;
        int dotted = 0;
        while (j < len && s[j] != ':') {
            if (s[j] == '.') dotted = 1;
            j++;
        }
        if (dotted) {
            if (j != len || n > 12 || !real_ip_parse_v4(s + i, len - i, buf + n)) return 0;
            n += 4;
            break;
        }
        int v = 0, digits = 0;
        while (i < len && real_ip_hex(s[i]) >= 0) {
            if (++digits > 4) return 0;
            v = v * 16 + real_ip_hex(s[i]);
            i++;
        }
        if (digits == 0) return 0;
        buf[n++] = (unsigned char)(v >> 8);
        buf[n++] = (unsigned char)(v & 0xff);
        if (i == len) break;
        if (s[i] != ':') return 0;
        i++;
        if (i < len && s[i] == ':') {
            if (gap >= 0) return 0;   /* a second `::` */
            gap = n;
            i++;
        } else if (i == len) {
            return 0;                 /* a trailing single `:` */
        }
    }
    if (gap >= 0) {
        if (n > 14) return 0;         /* `::` stands for at least one group */
        memmove(buf + 16 - (n - gap), buf + gap, (size_t)(n - gap));
        memset(buf + gap, 0, (size_t)(16 - n));
    } else if (n != 16) {
        return 0;
    }
    memcpy(out, buf, 16);
    return 1;
}

/* The `len` bytes at `text` as an address: 1, with its 16 bytes in `out`
 * (an IPv4 address mapped, ::ffff:a.b.c.d) and *is_v4 saying which it was
 * written as; 0 when it is none. */
static int real_ip_parse(const char* text, size_t len, unsigned char out[16], int* is_v4) {
    if (len == 0) return 0;
    if (real_ip_parse_v4(text, len, out + 12)) {
        memset(out, 0, 10);
        out[10] = 0xff;
        out[11] = 0xff;
        if (is_v4) *is_v4 = 1;
        return 1;
    }
    if (real_ip_parse_v6(text, len, out)) {
        if (is_v4) *is_v4 = 0;
        return 1;
    }
    return 0;
}

static void real_ip_trim(const char** s, const char** e) {
    while (*s < *e && (**s == ' ' || **s == '\t')) (*s)++;
    while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t')) (*e)--;
}

/* `list` ("127.0.0.1, 10.0.0.0/8, ::1, fd00::/8") into o->trusted: 0, or
 * -1 when an entry is no address or network. */
static int real_ip_parse_trusted(AetherRealIpOpts* o, const char* list) {
    int cap = 1;
    for (const char* c = list; *c; c++) if (*c == ',') cap++;
    o->trusted = (RealIpNet*)calloc((size_t)cap, sizeof(RealIpNet));
    if (!o->trusted) return -1;
    const char* p = list;
    while (1) {
        const char* end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char* s = p;
        const char* e = end;
        real_ip_trim(&s, &e);
        const char* slash = memchr(s, '/', (size_t)(e - s));
        RealIpNet* net = &o->trusted[o->trusted_count];
        int is_v4 = 0;
        if (!real_ip_parse(s, (size_t)((slash ? slash : e) - s), net->addr, &is_v4)) return -1;
        int width = is_v4 ? 32 : 128;
        int prefix = width;
        if (slash) {
            const char* d = slash + 1;
            if (d == e) return -1;
            prefix = 0;
            for (; d < e; d++) {
                if (*d < '0' || *d > '9') return -1;
                prefix = prefix * 10 + (*d - '0');
                if (prefix > width) return -1;
            }
        }
        net->bits = is_v4 ? 96 + prefix : prefix;
        o->trusted_count++;
        if (!*end) break;
        p = end + 1;
    }
    return 0;
}

static int real_ip_is_trusted(const AetherRealIpOpts* o, const unsigned char a[16]) {
    for (int i = 0; i < o->trusted_count; i++) {
        const RealIpNet* n = &o->trusted[i];
        int whole = n->bits / 8, rest = n->bits % 8;
        if (memcmp(a, n->addr, (size_t)whole) != 0) continue;
        if (rest && ((a[whole] ^ n->addr[whole]) & (unsigned char)(0xff << (8 - rest)))) continue;
        return 1;
    }
    return 0;
}

AetherRealIpOpts* aether_real_ip_opts_new(const char* header_name, const char* trusted) {
    if (!trusted || !*trusted) return NULL;
    AetherRealIpOpts* o = (AetherRealIpOpts*)calloc(1, sizeof(AetherRealIpOpts));
    if (!o) return NULL;
    if (!header_name || !*header_name) header_name = "X-Forwarded-For";
    o->header_name = strdup(header_name);
    o->forwarded = strcasecmp(header_name, "Forwarded") == 0;
    if (!o->header_name || real_ip_parse_trusted(o, trusted) != 0) {
        aether_real_ip_opts_free(o);
        return NULL;
    }
    return o;
}

void aether_real_ip_opts_free(AetherRealIpOpts* o) {
    if (!o) return;
    free(o->header_name);
    free(o->trusted);
    free(o);
}

/* One entry of the forwarding header as an address: trimmed, unquoted, the
 * for= parameter of a `Forwarded` element, a [v6]:port or a v4:port
 * reduced to its address. 0 when it is none ("unknown", an obfuscated
 * name, garbage). */
static int real_ip_entry(const AetherRealIpOpts* o, const char* s, const char* e,
                         unsigned char out[16]) {
    if (o->forwarded) {
        /* for=...; proto=...; by=...: the for= value, up to its ';'. */
        const char* v = NULL;
        const char* p = s;
        while (p < e) {
            const char* q = memchr(p, ';', (size_t)(e - p));
            if (!q) q = e;
            const char* ps = p;
            const char* pe = q;
            real_ip_trim(&ps, &pe);
            if (pe - ps > 4 && strncasecmp(ps, "for=", 4) == 0) {
                v = ps + 4;
                s = v;
                e = pe;
                break;
            }
            p = q + 1;
        }
        if (!v) return 0;
    }
    real_ip_trim(&s, &e);
    if (e - s >= 2 && *s == '"' && e[-1] == '"') { s++; e--; }
    if (s < e && *s == '[') {
        const char* close = memchr(s, ']', (size_t)(e - s));
        if (!close) return 0;
        return real_ip_parse(s + 1, (size_t)(close - s - 1), out, NULL);
    }
    const char* colon = memchr(s, ':', (size_t)(e - s));
    if (colon && !memchr(colon + 1, ':', (size_t)(e - colon - 1))) e = colon;   /* v4:port */
    return real_ip_parse(s, (size_t)(e - s), out, NULL);
}

/* `a` as text: dotted for an IPv4 address, else IPv6 as RFC 5952 writes it
 * (lowercase, no leading zeros, the longest run of two or more zero groups
 * as `::`, the first of equal ones). */
static void real_ip_text(const unsigned char a[16], char* buf, size_t cap) {
    static const unsigned char mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
    if (memcmp(a, mapped, 12) == 0) {
        snprintf(buf, cap, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        return;
    }
    unsigned g[8];
    for (int k = 0; k < 8; k++) g[k] = ((unsigned)a[2 * k] << 8) | a[2 * k + 1];
    int best = -1, best_len = 0;
    for (int k = 0; k < 8;) {
        if (g[k] != 0) { k++; continue; }
        int run = k;
        while (run < 8 && g[run] == 0) run++;
        if (run - k > best_len && run - k >= 2) { best = k; best_len = run - k; }
        k = run;
    }
    size_t used = 0;
    buf[0] = '\0';
    for (int k = 0; k < 8 && used < cap; k++) {
        if (k == best) {
            used += (size_t)snprintf(buf + used, cap - used, "::");
            k += best_len - 1;
            continue;
        }
        int sep = k > 0 && k != best + best_len;
        used += (size_t)snprintf(buf + used, cap - used, "%s%x", sep ? ":" : "", g[k]);
    }
}

/* X-Real-IP is `ip`, the client resolved here, or gone when there is none:
 * a client can send that header itself, so one on the request is never
 * kept (#3169): none of them, a second one included, which used to stay and
 * reach an upstream that reads the last X-Real-IP (#3239). Through the
 * server's request edits (#3234): this replaced strings with free() and grew
 * the header arrays to one past what they held, which an arena-backed request
 * and a reused one cannot take. */
static void real_ip_set_header(HttpRequest* req, const char* ip) {
    http_request_remove_header(req, "X-Real-IP");
    if (ip) http_request_set_header(req, "X-Real-IP", ip);
}

/* The client a request came from, through the proxies `trusted` names
 * (#3212): what nginx's set_real_ip_from with real_ip_recursive gives. A
 * forwarding header is believed only from a trusted peer, since anything
 * else that connects can send one. Its entries are read from the right,
 * each the address the hop before it saw: the first that is not a trusted
 * proxy is the client, and a client cannot get past it by writing entries
 * of its own to the left. The leftmost entry used to be taken from any
 * peer, so a client picked its own address, and with it its rate-limit
 * bucket, behind every proxy that appends to the header (std.http.proxy,
 * nginx's default, cloud load balancers). */
int aether_middleware_real_ip(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)res;
    AetherRealIpOpts* o = (AetherRealIpOpts*)user_data;
    if (!o || !req) return 1;

    unsigned char peer[16];
    int peer_known = req->remote_addr &&
                     real_ip_parse(req->remote_addr, strlen(req->remote_addr), peer, NULL);
    if (!peer_known || !real_ip_is_trusted(o, peer)) {
        real_ip_set_header(req, peer_known ? req->remote_addr : NULL);
        return 1;
    }

    const char* fwd = NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (req->header_keys[i] && strcasecmp(req->header_keys[i], o->header_name) == 0) {
            fwd = req->header_values[i];
            break;
        }
    }

    /* The entries' spans, the last REAL_IP_MAX of them: the walk starts
     * at the right. */
    enum { REAL_IP_MAX = 64 };
    const char* starts[REAL_IP_MAX];
    const char* ends[REAL_IP_MAX];
    int n = 0;
    for (const char* p = fwd; p && *p;) {
        const char* end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        if (n == REAL_IP_MAX) {
            memmove(starts, starts + 1, sizeof starts[0] * (REAL_IP_MAX - 1));
            memmove(ends, ends + 1, sizeof ends[0] * (REAL_IP_MAX - 1));
            n--;
        }
        starts[n] = p;
        ends[n] = end;
        n++;
        p = *end ? end + 1 : end;
    }

    unsigned char client[16];
    memcpy(client, peer, 16);
    int moved = 0;
    for (int i = n - 1; i >= 0; i--) {
        unsigned char hop[16];
        if (!real_ip_entry(o, starts[i], ends[i], hop)) break;   /* the chain is broken here */
        memcpy(client, hop, 16);
        moved = 1;
        if (!real_ip_is_trusted(o, hop)) break;
    }

    char text[64];
    real_ip_text(client, text, sizeof text);
    if (moved && text[0]) {
        /* The peer the request reports from here on (#3169):
         * http_request_remote_addr answers it to every handler and
         * middleware after this one, std.http.proxy's X-Forwarded-For
         * entry among them. The proxy connection's port says nothing of
         * the client, so the port reads as unavailable. */
        char* resolved = strdup(text);
        if (resolved) {
            free(req->remote_addr);
            req->remote_addr = resolved;
            req->remote_port = 0;
        }
    }
    real_ip_set_header(req, req->remote_addr);
    return 1;  /* always continue the chain — middleware is
                * informational, never short-circuits */
}

