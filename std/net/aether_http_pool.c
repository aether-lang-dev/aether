/* aether_http_pool.c — bounded worker pool for accepted connections.
 *
 * Split out of aether_http_server.c: thread-budget management is a
 * separate concern from HTTP protocol handling, and the pool needs
 * nothing from the server internals beyond the public drain entry
 * point.
 */

#include "aether_http_pool.h"
#include "../../runtime/utils/aether_thread.h"

#include <stdlib.h>
#include <stdatomic.h>
#include "../../runtime/utils/aether_cpu_available.h"

#if defined(_WIN32)
    #include <winsock2.h>
    #define close closesocket
#else
    #include <unistd.h>
#endif

/* Connections beyond pool capacity wait in the kernel accept backlog,
 * and the bounded queue applies backpressure to the accept loop rather
 * than buffering client fds without limit. See the header for why this
 * pool is separate from the shared std.worker pool. */

/* Connections accepted and not yet picked up by a worker, across every pool
 * in the process. A worker owns its connection for the connection's life, so
 * a kept-alive connection while this is non-zero starves one that is waiting;
 * the response path reads it to decide (see finish_response). */
static atomic_int http_pool_pending_conns;
/* Workers inside a connection, and workers that exist, across every pool in
 * the process. The response path reads these to decide whether holding an
 * idle connection for a moment costs anyone else their turn (#1663). */
static atomic_int http_pool_busy_workers;
static atomic_int http_pool_total_workers;

int http_pool_pending(void) {
    return atomic_load(&http_pool_pending_conns);
}

int http_pool_has_spare_worker(void) {
    int total = atomic_load(&http_pool_total_workers);
    if (total <= 0) return 0;   /* no pool: the caller is the only worker */
    return atomic_load(&http_pool_busy_workers) < total
        && atomic_load(&http_pool_pending_conns) == 0;
}

#if AETHER_HAS_THREADS

#define HTTP_POOL_QUEUE_CAP  256
#define HTTP_POOL_MIN_WORKERS 8
#define HTTP_POOL_MAX_WORKERS 64

/* A queued item is either a freshly accepted descriptor or a connection the
 * parking lot woke (#1663). They travel the same queue because a worker does
 * the same thing with both: run requests until the connection is done or
 * parks again. */
typedef struct {
    int       fd;      /* valid when conn == NULL */
    HttpConn* conn;    /* a resumed connection, already past its handshake */
} HttpPoolItem;

struct HttpConnectionPool {
    HttpServer* server;
    HttpPoolItem queue[HTTP_POOL_QUEUE_CAP];   // Ring buffer of pending work
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int shutdown;
    int worker_count;                 // threads actually started
    pthread_t workers[HTTP_POOL_MAX_WORKERS];
};

/* Connection handlers block on socket I/O rather than burning CPU, so the
 * pool is sized above the core count. */
/* `evloop_owns` is true when the event driver is running, which means every
 * proxied request is served on a driver thread and never occupies a worker
 * here. The sizing below exists for the opposite case, where a worker is held
 * for a whole upstream round trip, so with the driver present it buys nothing
 * and costs a thread apiece. A pure reverse proxy was starting eight idle
 * workers on two cores, which is where most of its thread count came from. */
static int http_pool_worker_count(int evloop_owns) {
    long cores = aether_cpu_available();

    long want = cores * 2;
    long floor = HTTP_POOL_MIN_WORKERS;
    if (evloop_owns) {
        /* Enough to keep the hand-back path moving: a request the proxy
         * declines, a health or admin route on the same server, a drained
         * connection. None of those is the hot path when a driver is running,
         * and the queue in front of these absorbs a burst. */
        want = 2;
        floor = 2;
    }

    /* Overridable, because cores * 2 is the right shape for handlers that
     * compute and the wrong one for handlers that wait. A worker owns its
     * connection for the whole of a request, so a reverse proxy — whose
     * handler blocks for an upstream round trip — can have no more requests
     * in flight than it has workers, however idle those threads are. Sizing
     * that by core count bounds throughput at workers/latency and looks like
     * a slow server rather than an idle one.
     *
     * The default is unchanged. This exists so the bound can be measured and
     * so a proxy deployment can raise it without a rebuild. */
    const char* env = getenv("AETHER_HTTP_WORKERS");
    if (env && *env) {
        char* end = NULL;
        long from_env = strtol(env, &end, 10);
        if (end && *end == '\0' && from_env > 0) want = from_env;
    }

    if (want < floor) want = floor;
    if (want > HTTP_POOL_MAX_WORKERS) want = HTTP_POOL_MAX_WORKERS;
    return (int)want;
}

static void* http_pool_worker(void* arg) {
    HttpConnectionPool* pool = (HttpConnectionPool*)arg;
    while (1) {
        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        HttpPoolItem item = pool->queue[pool->head];
        pool->head = (pool->head + 1) % HTTP_POOL_QUEUE_CAP;
        pool->count--;
        atomic_fetch_sub(&http_pool_pending_conns, 1);
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        atomic_fetch_add(&http_pool_busy_workers, 1);
        if (item.conn) http_server_resume_connection(pool->server, item.conn);
        else           http_server_drain_connection(pool->server, item.fd);
        atomic_fetch_sub(&http_pool_busy_workers, 1);
    }
    return NULL;
}

HttpConnectionPool* http_pool_create(HttpServer* server) {
    HttpConnectionPool* pool = calloc(1, sizeof(HttpConnectionPool));
    if (!pool) return NULL;
    pool->server = server;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    int want = http_pool_worker_count(server->evloop != NULL);
    for (int i = 0; i < want; i++) {
        /* CRITICAL: only count threads that actually started. Joining an
         * uninitialised pthread_t in http_pool_destroy is undefined
         * behaviour, so worker_count, not `want`, bounds that loop. */
        if (pthread_create(&pool->workers[pool->worker_count], NULL,
                           http_pool_worker, pool) != 0) {
            break;
        }
        pool->worker_count++;
        atomic_fetch_add(&http_pool_total_workers, 1);
    }

    if (pool->worker_count == 0) {
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->not_empty);
        pthread_cond_destroy(&pool->not_full);
        free(pool);
        return NULL;
    }
    return pool;
}

/* Add one worker, if the pool is still short of its ceiling.
 *
 * A worker owns its connection for the whole of a request, so a handler that
 * waits — a reverse proxy waiting on an upstream is the ordinary case — holds
 * a thread without using a core. Sizing the pool at cores * 2 then caps
 * requests in flight at twice the core count however idle those threads are.
 * Measured on a 2-core-pinned proxy against two backends: 8 workers gave
 * 18,621 rps, 16 gave 38,866, 32 gave 42,798 with p99 falling from 96ms to
 * 11ms. The work was not CPU-bound at any of those points; it was waiting.
 *
 * Growing one at a time rather than in batches: each new worker changes the
 * condition that asked for it, and a proxy that briefly queues two items does
 * not need sixteen more threads.
 *
 * The ceiling stays HTTP_POOL_MAX_WORKERS. Past the point where waiting stops
 * being the constraint, more threads cost more than they carry — the same
 * measurement had 64 workers slower than 32. */
static void http_pool_grow(HttpConnectionPool* pool) {
    pthread_mutex_lock(&pool->lock);
    if (pool->shutdown || pool->worker_count >= HTTP_POOL_MAX_WORKERS) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    int slot = pool->worker_count;
    /* Claimed before the thread exists so two submitters cannot take the same
     * slot; rolled back below if the thread does not start. */
    pool->worker_count++;
    pthread_mutex_unlock(&pool->lock);

    if (pthread_create(&pool->workers[slot], NULL, http_pool_worker, pool) != 0) {
        pthread_mutex_lock(&pool->lock);
        /* Only safe to give the slot back if nobody has claimed one after it;
         * otherwise leave the count alone and let the gap stay unused rather
         * than have http_pool_destroy join a pthread_t that was never
         * initialised. */
        if (pool->worker_count == slot + 1) pool->worker_count--;
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    atomic_fetch_add(&http_pool_total_workers, 1);
}

static void http_pool_submit_item(HttpConnectionPool* pool, HttpPoolItem item) {
    pthread_mutex_lock(&pool->lock);
    while (pool->count >= HTTP_POOL_QUEUE_CAP && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        if (item.conn) http_conn_close_owned(item.conn);
        else           close(item.fd);
        return;
    }
    atomic_fetch_add(&http_pool_pending_conns, 1);
    pool->queue[pool->tail] = item;
    pool->tail = (pool->tail + 1) % HTTP_POOL_QUEUE_CAP;
    pool->count++;
    /* Every worker busy and work still arriving means the pool is the
     * bottleneck, not the machine. Decided under the lock, acted on outside
     * it: pthread_create takes long enough that holding the queue lock across
     * it would stall the submitters this is meant to unblock. */
    int grow = (pool->worker_count < HTTP_POOL_MAX_WORKERS)
            && (atomic_load(&http_pool_busy_workers) >= pool->worker_count)
            && (pool->count > 1);
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    if (grow) http_pool_grow(pool);
}

void http_pool_submit(HttpConnectionPool* pool, int client_fd) {
    HttpPoolItem item = { .fd = client_fd, .conn = NULL };
    http_pool_submit_item(pool, item);
}

void http_pool_submit_conn(HttpConnectionPool* pool, HttpConn* conn) {
    HttpPoolItem item = { .fd = -1, .conn = conn };
    http_pool_submit_item(pool, item);
}

void http_pool_destroy(HttpConnectionPool* pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i], NULL);
    }
    atomic_fetch_sub(&http_pool_total_workers, pool->worker_count);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool);
}

#endif // AETHER_HAS_THREADS

