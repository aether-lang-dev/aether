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

int http_pool_pending(void) {
    return atomic_load(&http_pool_pending_conns);
}

#if AETHER_HAS_THREADS

#define HTTP_POOL_QUEUE_CAP  256
#define HTTP_POOL_MIN_WORKERS 8
#define HTTP_POOL_MAX_WORKERS 64

/* A queue entry is either a freshly accepted fd (conn == NULL) or a parked
 * keep-alive connection that woke up (#1663), in which case the worker
 * resumes it instead of starting a new one. */
typedef struct {
    int   fd;
    void* conn;
} HttpPoolItem;

struct HttpConnectionPool {
    HttpServer* server;
    HttpPoolItem queue[HTTP_POOL_QUEUE_CAP];  // Ring buffer of pending work
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int shutdown;
    int worker_count;                 // threads actually started
    pthread_t workers[HTTP_POOL_MAX_WORKERS];
};

/* Connection handlers block on socket I/O rather than burning CPU, so the
 * pool is sized above the core count. aether_cpu_detect lives in the
 * runtime, which the compiler does not link against this file, so the
 * probe is done directly here. */
static int http_pool_worker_count(void) {
    long cores = 0;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores = (long)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cores < 1) cores = 1;

    long want = cores * 2;
    if (want < HTTP_POOL_MIN_WORKERS) want = HTTP_POOL_MIN_WORKERS;
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

        if (item.conn) {
            http_server_resume_connection(pool->server, item.conn);
        } else {
            http_server_drain_connection(pool->server, item.fd);
        }
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

    int want = http_pool_worker_count();
    for (int i = 0; i < want; i++) {
        /* CRITICAL: only count threads that actually started. Joining an
         * uninitialised pthread_t in http_pool_destroy is undefined
         * behaviour, so worker_count, not `want`, bounds that loop. */
        if (pthread_create(&pool->workers[pool->worker_count], NULL,
                           http_pool_worker, pool) != 0) {
            break;
        }
        pool->worker_count++;
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

void http_pool_submit(HttpConnectionPool* pool, int client_fd) {
    pthread_mutex_lock(&pool->lock);
    while (pool->count >= HTTP_POOL_QUEUE_CAP && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        close(client_fd);
        return;
    }
    atomic_fetch_add(&http_pool_pending_conns, 1);
    pool->queue[pool->tail].fd   = client_fd;
    pool->queue[pool->tail].conn = NULL;
    pool->tail = (pool->tail + 1) % HTTP_POOL_QUEUE_CAP;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

int http_pool_submit_conn(HttpConnectionPool* pool, void* conn) {
    if (!pool || !conn) return -1;
    pthread_mutex_lock(&pool->lock);
    /* Never block: the caller is the single park poller thread, and waiting
     * for queue space here would stall every other parked connection. */
    if (pool->shutdown || pool->count >= HTTP_POOL_QUEUE_CAP) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    atomic_fetch_add(&http_pool_pending_conns, 1);
    pool->queue[pool->tail].fd   = -1;
    pool->queue[pool->tail].conn = conn;
    pool->tail = (pool->tail + 1) % HTTP_POOL_QUEUE_CAP;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
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
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool);
}

#endif // AETHER_HAS_THREADS

