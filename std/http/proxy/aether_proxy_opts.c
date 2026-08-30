/* aether_proxy_opts.c — per-mount options.
 *
 * Opts hold a single AetherProxyPool* (refcount-incremented on
 * bind) and an optional AetherProxyCache*. Multiple mounts can
 * share the same pool; bind/release ratios match
 * aether_proxy_pool_retain / aether_proxy_pool_free.
 */

#include "aether_proxy_internal.h"
#include "../../../runtime/aether_resource_caps.h"
#include "../../net/aether_http.h"

#include <stdlib.h>
#include <string.h>

AetherProxyOpts* aether_proxy_opts_new(void) {
    AetherProxyOpts* o = (AetherProxyOpts*)aether_caps_calloc(1, sizeof(*o));
    if (!o) return NULL;
    /* Sensible defaults — all X-Forwarded-* on, host rewritten to
     * upstream, no path-prefix chop, 8 MiB body cap. */
    o->add_xff = 1;
    o->add_xfp = 1;
    o->add_xfh = 1;
    o->preserve_host  = 0;
    o->max_body_bytes = 8 * 1024 * 1024;

    /* Mounting a proxy changes what the connection pool is for: its upstream
     * connections are held for one request and handed back, where a client's
     * are kept for a few hosts. Left at the client's caps, every connection
     * past the eighth to a backend was closed on release and redialled. */
    /* Read once here rather than per request: a mount happens on one thread
     * at start-up, and a getenv on the request path would be both a cost and
     * a race. */
    const char* direct = getenv("AETHER_PROXY_DIRECT");
    o->direct_disabled = (direct && direct[0] == '0' && direct[1] == '\0');

    http_client_pool_size_for_proxy();
    return o;
}

void aether_proxy_opts_free(AetherProxyOpts* opts) {
    if (!opts) return;
    if (opts->pool) aether_proxy_pool_free(opts->pool);
    /* cache is owned by the caller; we never own it here */
    free(opts->path_prefix);
    free(opts->route_pattern);
    free(opts->methods);
    free(opts->strip_path_prefix);
    aether_caps_free(opts, sizeof(*opts));
}

const char* aether_proxy_opts_set_strip_prefix(AetherProxyOpts* opts,
                                               const char* prefix) {
    if (!opts) return "opts is null";
    free(opts->strip_path_prefix);
    opts->strip_path_prefix = (prefix && *prefix) ? strdup(prefix) : NULL;
    return "";
}

const char* aether_proxy_opts_set_preserve_host(AetherProxyOpts* opts, int on) {
    if (!opts) return "opts is null";
    opts->preserve_host = on ? 1 : 0;
    return "";
}

const char* aether_proxy_opts_set_xforwarded(AetherProxyOpts* opts,
                                             int xff, int xfp, int xfh) {
    if (!opts) return "opts is null";
    opts->add_xff = xff ? 1 : 0;
    opts->add_xfp = xfp ? 1 : 0;
    opts->add_xfh = xfh ? 1 : 0;
    return "";
}

const char* aether_proxy_opts_bind_cache(AetherProxyOpts* opts,
                                         AetherProxyCache* cache) {
    if (!opts) return "opts is null";
    /* The caller retains ownership of the cache; we just record a
     * pointer. Multiple mounts sharing one cache is fine and
     * intended. */
    opts->cache = cache;
    return "";
}

const char* aether_proxy_opts_set_body_cap(AetherProxyOpts* opts,
                                           int max_body_bytes) {
    if (!opts) return "opts is null";
    if (max_body_bytes < 0) return "max_body_bytes must be >= 0";
    opts->max_body_bytes = max_body_bytes;
    return "";
}

const char* aether_proxy_opts_set_retry_policy(AetherProxyOpts* opts,
                                               int max_retries,
                                               int backoff_base_ms) {
    if (!opts) return "opts is null";
    if (max_retries < 0)      return "max_retries must be >= 0";
    if (backoff_base_ms < 0)  return "backoff_base_ms must be >= 0";
    opts->retry_max_retries     = max_retries;
    opts->retry_backoff_base_ms = backoff_base_ms > 0 ? backoff_base_ms
                                                      : (max_retries > 0 ? 100 : 0);
    return "";
}

const char* aether_proxy_opts_set_trace_inject(AetherProxyOpts* opts, int inject) {
    if (!opts) return "opts is null";
    opts->trace_context_inject = inject ? 1 : 0;
    return "";
}
