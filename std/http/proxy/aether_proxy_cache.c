/* aether_proxy_cache.c — in-memory LRU response cache + TTL eviction.
 *
 * Open-chained hash + doubly-linked LRU list (head = most recently
 * used). Lookup walks the bucket chain matching `key_hash + key_repr`,
 * checks `expires_at_ms`, moves the entry to the LRU head on hit,
 * returns NULL on miss.
 *
 * Cacheability gates (RFC 7234 conservative subset):
 *   - GET / HEAD only.
 *   - status in {200, 203, 204, 300, 301, 404, 410}.
 *   - Cache-Control on request: no `no-store`.
 *   - Cache-Control on response: no `no-store` / `private`.
 *   - body_length ≤ max_body_bytes.
 *   - Vary: * → uncacheable.
 *
 * TTL: upstream Cache-Control `max-age` (clamped to 1 hour for v1
 * conservatism) → s-maxage → Expires header → default_ttl_sec.
 *
 * Conditional revalidation is performed by the middleware layer
 * before lookup, by issuing the upstream request with
 * If-None-Match / If-Modified-Since populated from a stale entry
 * — that's not in this file because it's middleware-scope.
 *
 * Single mutex around the whole cache. Sharded locking is the v2
 * follow-up if write contention shows in profiling.
 */

#include "aether_proxy_internal.h"
#include "../../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

int aether_proxy_cache_key_strategy_from_string(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "url")              == 0) return AETHER_PROXY_CACHE_KEY_URL;
    if (strcmp(name, "method_url")       == 0) return AETHER_PROXY_CACHE_KEY_METHOD_URL;
    if (strcmp(name, "method_url_vary")  == 0) return AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY;
    return -1;
}

static int next_pow2_at_least(int n) {
    int v = 1;
    while (v < n) v <<= 1;
    return v;
}

AetherProxyCache* aether_proxy_cache_new(int max_entries,
                                         int max_body_bytes,
                                         int default_ttl_sec,
                                         AetherProxyCacheKeyStrategy key_strategy) {
    if (max_entries <= 0)    return NULL;
    if (max_body_bytes < 0)  return NULL;
    if (default_ttl_sec < 0) return NULL;
    if (key_strategy < 0 || key_strategy > AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY) return NULL;

    AetherProxyCache* c = (AetherProxyCache*)aether_caps_calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->max_entries     = max_entries;
    c->max_body_bytes  = max_body_bytes;
    c->default_ttl_sec = default_ttl_sec;
    c->key_strategy    = key_strategy;
    /* Bucket count: roughly 2x max_entries, rounded up to power of 2.
     * Keeps load factor under 0.5 for fast lookups. */
    c->bucket_count = next_pow2_at_least(max_entries * 2);
    if (c->bucket_count < 16) c->bucket_count = 16;
    c->buckets = (AetherProxyCacheEntry**)calloc((size_t)c->bucket_count,
                                                  sizeof(AetherProxyCacheEntry*));
    if (!c->buckets) { aether_caps_free(c, sizeof(*c)); return NULL; }

    pthread_mutex_init(&c->lock, NULL);
    return c;
}

static void entry_free_one(AetherProxyCacheEntry* e) {
    if (!e) return;
    free(e->key_repr);
    if (e->header_keys) {
        for (int i = 0; i < e->header_count; i++) free(e->header_keys[i]);
        free(e->header_keys);
    }
    if (e->header_values) {
        for (int i = 0; i < e->header_count; i++) free(e->header_values[i]);
        free(e->header_values);
    }
    /* Cap-aware (#343): body was alloc'd via aether_caps_malloc;
     * the recorded length is the byte count for caps accounting. */
    aether_caps_free(e->body, (size_t)e->body_length);
    free(e->etag);
    free(e->last_modified);
    free(e->vary_header);
    aether_caps_free(e, sizeof(*e));
}

void aether_proxy_cache_free(AetherProxyCache* cache) {
    if (!cache) return;
    pthread_mutex_lock(&cache->lock);
    for (int i = 0; i < cache->bucket_count; i++) {
        AetherProxyCacheEntry* e = cache->buckets[i];
        while (e) {
            AetherProxyCacheEntry* nx = e->bucket_next;
            entry_free_one(e);
            e = nx;
        }
    }
    free(cache->buckets);
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    aether_caps_free(cache, sizeof(*cache));
}

/* FNV-1a 64-bit. */
static uint64_t fnv1a64(const char* s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void lower_into(const char* in, char* out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = '\0';
}

/* Build the canonical cache key and its hash. Returned key is
 * malloc'd; caller frees. Returns NULL on OOM or uncacheable
 * (Vary: *). When `vary_header` is non-NULL the response's Vary
 * header drives request-header inclusion. */
static char* build_key(AetherProxyCacheKeyStrategy strat,
                       const char* method,
                       const char* url,
                       HttpRequest* req,
                       const char* vary_header,
                       uint64_t* out_hash) {
    /* Reasonable upper bound; rare cases above this are handled by
     * malloc-and-copy. */
    char buf[2048];
    size_t pos = 0;

    if (strat == AETHER_PROXY_CACHE_KEY_METHOD_URL ||
        strat == AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY) {
        size_t ml = strlen(method);
        if (pos + ml + 1 >= sizeof(buf)) return NULL;
        memcpy(buf + pos, method, ml); pos += ml;
        buf[pos++] = ' ';
    }

    size_t ul = strlen(url);
    if (pos + ul >= sizeof(buf)) return NULL;
    memcpy(buf + pos, url, ul); pos += ul;

    if (strat == AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY && vary_header) {
        /* Walk the Vary header (comma-separated header names);
         * append "\0<lower-name>=<value>" segments. Vary: * means
         * uncacheable — caller's gate handles that, we just pass
         * through. */
        const char* p = vary_header;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            const char* name_start = p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
            size_t name_len = (size_t)(p - name_start);
            if (name_len == 0 || name_len > 64) continue;
            char name_buf[80];
            memcpy(name_buf, name_start, name_len);
            name_buf[name_len] = '\0';
            char name_lc[80];
            lower_into(name_buf, name_lc, sizeof(name_lc));

            const char* value = http_get_header(req, name_lc);
            if (!value) value = "";

            if (pos + 1 + name_len + 1 + strlen(value) >= sizeof(buf)) return NULL;
            buf[pos++] = '\0';
            memcpy(buf + pos, name_lc, strlen(name_lc)); pos += strlen(name_lc);
            buf[pos++] = '=';
            size_t vl = strlen(value);
            memcpy(buf + pos, value, vl); pos += vl;
        }
    }

    *out_hash = fnv1a64(buf, pos);
    char* out = (char*)malloc(pos + 1);
    if (!out) return NULL;
    memcpy(out, buf, pos);
    out[pos] = '\0';
    return out;
}

/* Move entry to LRU head (caller holds cache->lock). */
static void lru_promote(AetherProxyCache* c, AetherProxyCacheEntry* e) {
    if (c->lru_head == e) return;
    /* Unlink from current position. */
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (c->lru_tail == e) c->lru_tail = e->lru_prev;
    /* Insert at head. */
    e->lru_prev = NULL;
    e->lru_next = c->lru_head;
    if (c->lru_head) c->lru_head->lru_prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

static void unlink_entry(AetherProxyCache* c, AetherProxyCacheEntry* e) {
    /* Remove from LRU list. */
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (c->lru_head == e) c->lru_head = e->lru_next;
    if (c->lru_tail == e) c->lru_tail = e->lru_prev;
    /* Remove from hash bucket. */
    int b = (int)(e->key_hash & (uint64_t)(c->bucket_count - 1));
    AetherProxyCacheEntry** pp = &c->buckets[b];
    while (*pp && *pp != e) pp = &(*pp)->bucket_next;
    if (*pp) *pp = e->bucket_next;
}

AetherProxyCacheEntry* aether_proxy_cache_lookup(AetherProxyCache* cache,
                                                 const char* method,
                                                 const char* url,
                                                 HttpRequest* req) {
    if (!cache || !method || !url) return NULL;

    /* For non-Vary strategies the lookup key fully determines the
     * stored representation. For VARY mode we don't know the
     * relevant Vary headers up front — they're stored on each
     * cache entry from the upstream's prior response — so we
     * compute the URL-only key for hash bucketing, then walk the
     * bucket recomputing each entry's "what the request would
     * look like under THIS entry's Vary spec" key and matching
     * that against the entry's stored key. Standard CDN pattern:
     * O(N) on bucket length (typically 1-2), no extra storage
     * beyond `vary_header` on the entry.
     *
     * For URL-only and METHOD_URL strategies we still hash the
     * canonical key so the bucket lookup is direct. */
    int64_t now = aether_proxy_now_ms();

    pthread_mutex_lock(&cache->lock);

    /* Compute a base hash (without Vary). For URL / METHOD_URL
     * strategies this IS the full key; the hash directly indexes
     * the bucket. For VARY mode we still bucket by URL+method so
     * all variants of one URL share a bucket — then the per-entry
     * Vary recompute narrows down. */
    uint64_t base_hash;
    char* base_key = build_key(cache->key_strategy, method, url, req, NULL,
                                &base_hash);
    if (!base_key) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    int b = (int)(base_hash & (uint64_t)(cache->bucket_count - 1));
    AetherProxyCacheEntry* e = cache->buckets[b];
    while (e) {
        AetherProxyCacheEntry* nx = e->bucket_next;
        int matches;

        if (cache->key_strategy == AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY &&
            e->vary_header && *e->vary_header) {
            /* Recompute the request's key under this entry's Vary
             * spec. If it produces the same canonical key as the
             * stored one, the request's Vary-named header values
             * match what was stored — same representation, hit. */
            uint64_t variant_hash;
            char* variant_key = build_key(cache->key_strategy, method, url, req,
                                          e->vary_header, &variant_hash);
            if (!variant_key) { e = nx; continue; }
            matches = (variant_hash == e->key_hash &&
                       strcmp(variant_key, e->key_repr) == 0);
            free(variant_key);
        } else {
            /* No Vary: direct key match. */
            matches = (e->key_hash == base_hash &&
                       strcmp(e->key_repr, base_key) == 0);
        }

        if (matches) {
            if (e->expires_at_ms <= now) {
                unlink_entry(cache, e);
                cache->entry_count--;
                entry_free_one(e);
                free(base_key);
                pthread_mutex_unlock(&cache->lock);
                return NULL;
            }
            lru_promote(cache, e);
            free(base_key);
            pthread_mutex_unlock(&cache->lock);
            return e;
        }
        e = nx;
    }
    free(base_key);
    pthread_mutex_unlock(&cache->lock);
    return NULL;
}

/* Parse "Cache-Control: max-age=N" (crude, no validation beyond the
 * digits). Returns -1 if the directive isn't present. */
static int parse_max_age(const char* cc) {
    if (!cc) return -1;
    const char* p = strstr(cc, "max-age=");
    if (!p) return -1;
    p += 8;
    int n = 0;
    int seen = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; seen = 1; }
    return seen ? n : -1;
}

static int cc_has_directive(const char* cc, const char* dir) {
    if (!cc || !dir) return 0;
    return strstr(cc, dir) != NULL;
}

/* Helper — locate one header value in a `Name: value\r\n` block.
 * Case-insensitive name match. Returns NULL on miss. Caller frees
 * the returned malloc'd string. */
static char* extract_header(const char* block, const char* name) {
    if (!block || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = block;
    while (*p) {
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char* v = p + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char* eol = strstr(v, "\r\n");
            size_t vl = eol ? (size_t)(eol - v) : strlen(v);
            char* out = (char*)malloc(vl + 1);
            if (!out) return NULL;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return out;
        }
        const char* eol = strstr(p, "\r\n");
        if (!eol) break;
        p = eol + 2;
    }
    return NULL;
}

void aether_proxy_cache_store(AetherProxyCache* cache,
                              const char* method,
                              const char* url,
                              HttpRequest* req,
                              int status_code,
                              const char* response_headers,
                              const char* body,
                              int body_length) {
    if (!cache || !method || !url) return;
    if (body_length < 0 || body_length > cache->max_body_bytes) return;

    /* Cacheability gate — RFC 7234 conservative subset. */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) return;
    int cacheable_status =
        status_code == 200 || status_code == 203 || status_code == 204 ||
        status_code == 300 || status_code == 301 || status_code == 404 ||
        status_code == 410;
    if (!cacheable_status) return;

    const char* req_cc = http_get_header(req, "Cache-Control");
    if (req_cc && cc_has_directive(req_cc, "no-store")) return;

    char* resp_cc = extract_header(response_headers, "Cache-Control");
    if (resp_cc && (cc_has_directive(resp_cc, "no-store") ||
                    cc_has_directive(resp_cc, "private"))) {
        free(resp_cc);
        return;
    }

    char* vary = extract_header(response_headers, "Vary");
    if (vary && strchr(vary, '*')) {
        /* Vary: * means the response varies on every header — refuse. */
        free(resp_cc);
        free(vary);
        return;
    }

    /* TTL resolution: upstream max-age (clamped 1h) → default. */
    int ttl_sec = -1;
    int max_age = parse_max_age(resp_cc);
    if (max_age >= 0) {
        if (max_age > 3600) max_age = 3600;
        ttl_sec = max_age;
    }
    if (ttl_sec < 0) {
        ttl_sec = cache->default_ttl_sec;
    }
    free(resp_cc);

    /* Two keys for VARY mode: a base key (URL+method only) drives
     * bucket placement so all variants of one URL hash to the same
     * bucket — that's how lookup finds them with only the request
     * in hand. The full key (with Vary suffix) is what entries are
     * matched against once the bucket is walked. For URL and
     * METHOD_URL modes the two collapse to the same value because
     * `vary` is unused. */
    uint64_t base_hash, full_hash;
    char* base_key = build_key(cache->key_strategy, method, url, req, NULL,
                               &base_hash);
    char* full_key = build_key(cache->key_strategy, method, url, req, vary,
                               &full_hash);
    if (!base_key || !full_key) {
        free(base_key);
        free(full_key);
        free(vary);
        return;
    }

    pthread_mutex_lock(&cache->lock);

    /* Replace any existing entry with the same full key. Bucket walk
     * uses base_hash so we hit the same bucket as a future lookup. */
    int b = (int)(base_hash & (uint64_t)(cache->bucket_count - 1));
    AetherProxyCacheEntry** pp = &cache->buckets[b];
    while (*pp) {
        if ((*pp)->key_hash == full_hash &&
            strcmp((*pp)->key_repr, full_key) == 0) {
            AetherProxyCacheEntry* old = *pp;
            *pp = old->bucket_next;
            /* Unlink from LRU. */
            if (old->lru_prev) old->lru_prev->lru_next = old->lru_next;
            if (old->lru_next) old->lru_next->lru_prev = old->lru_prev;
            if (cache->lru_head == old) cache->lru_head = old->lru_next;
            if (cache->lru_tail == old) cache->lru_tail = old->lru_prev;
            cache->entry_count--;
            entry_free_one(old);
            break;
        }
        pp = &(*pp)->bucket_next;
    }

    /* Evict LRU tail if at capacity. */
    while (cache->entry_count >= cache->max_entries && cache->lru_tail) {
        AetherProxyCacheEntry* victim = cache->lru_tail;
        unlink_entry(cache, victim);
        cache->entry_count--;
        entry_free_one(victim);
    }

    /* Allocate the new entry. */
    AetherProxyCacheEntry* e = (AetherProxyCacheEntry*)aether_caps_calloc(1, sizeof(*e));
    if (!e) {
        free(base_key);
        free(full_key);
        free(vary);
        pthread_mutex_unlock(&cache->lock);
        return;
    }
    /* base_key was only needed for bucketing; the entry stores the
     * full key + full hash for matching. */
    free(base_key);
    e->key_hash     = full_hash;
    e->key_repr     = full_key;
    e->vary_header  = vary;     /* ownership transferred; may be NULL */
    e->status_code  = status_code;
    e->stored_at_ms  = aether_proxy_now_ms();
    /* In 64 bits: (long)ttl_sec * 1000L overflowed on Windows, where a
     * `long` is 32 bits, for a TTL past 24.8 days, and the entry expired
     * before it was stored (#3096). */
    e->expires_at_ms = e->stored_at_ms + (int64_t)ttl_sec * 1000;
    e->etag          = extract_header(response_headers, "ETag");
    e->last_modified = extract_header(response_headers, "Last-Modified");

    if (body && body_length > 0) {
        /* Cap-aware (#343): cached upstream response body. The
         * body_length is the upstream's Content-Length — untrusted.
         * Cache entry's e->body_length is the byte count
         * aether_caps_free needs at the matching free site (see
         * the cache-entry destructor a few lines above). */
        e->body = (char*)aether_caps_malloc((size_t)body_length);
        if (e->body) {
            memcpy(e->body, body, (size_t)body_length);
            e->body_length = body_length;
        }
    }

    /* Parse the response header block into parallel arrays so the
     * lookup path can replay them onto HttpServerResponse. */
    int hdr_count = 0;
    const char* p = response_headers ? response_headers : "";
    /* The block's last field has no CRLF after it (#3714). */
    while (*p) {
        const char* eol;
        const char* next = aether_proxy_block_line(p, &eol);
        if (memchr(p, ':', (size_t)(eol - p))) hdr_count++;
        p = next;
    }
    if (hdr_count > 0) {
        e->header_keys   = (char**)calloc((size_t)hdr_count, sizeof(char*));
        e->header_values = (char**)calloc((size_t)hdr_count, sizeof(char*));
        if (e->header_keys && e->header_values) {
            int idx = 0;
            p = response_headers;
            while (*p && idx < hdr_count) {
                const char* eol;
                const char* next = aether_proxy_block_line(p, &eol);
                const char* colon = memchr(p, ':', (size_t)(eol - p));
                if (colon) {
                    size_t kl = (size_t)(colon - p);
                    const char* v = colon + 1;
                    while (v < eol && (*v == ' ' || *v == '\t')) v++;
                    size_t vl = (size_t)(eol - v);
                    e->header_keys[idx]   = (char*)malloc(kl + 1);
                    e->header_values[idx] = (char*)malloc(vl + 1);
                    if (e->header_keys[idx] && e->header_values[idx]) {
                        memcpy(e->header_keys[idx], p, kl);
                        e->header_keys[idx][kl] = '\0';
                        memcpy(e->header_values[idx], v, vl);
                        e->header_values[idx][vl] = '\0';
                        idx++;
                    }
                }
                p = next;
            }
            e->header_count = idx;
        }
    }

    /* Insert into bucket + LRU head. */
    e->bucket_next = cache->buckets[b];
    cache->buckets[b] = e;
    e->lru_prev = NULL;
    e->lru_next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->lru_prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail) cache->lru_tail = e;
    cache->entry_count++;

    pthread_mutex_unlock(&cache->lock);
}
