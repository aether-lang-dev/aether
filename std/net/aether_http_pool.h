#ifndef AETHER_HTTP_POOL_H
#define AETHER_HTTP_POOL_H

#include "aether_http_server.h"

// Bounded worker pool that runs accepted connections off the accept loop.
//
// Deliberately separate from the shared std.worker pool. A worker task
// runs one unit of work and returns; a connection is owned for its whole
// lifetime, and with keep-alive enabled (keep_alive_max == 0 meaning
// unlimited) that is unbounded in time. Routing connections onto the
// shared pool would let idle keep-alive clients occupy every thread in
// it and starve worker.run jobs and HTTP/2 stream dispatch, which share
// that pool. Separate lifetimes need separate thread budgets.

typedef struct HttpConnectionPool HttpConnectionPool;

// Starts the worker threads. Returns NULL when no worker could be
// started, in which case the caller should handle connections inline.
HttpConnectionPool* http_pool_create(HttpServer* server);

// Hands `client_fd` to a worker. Blocks while the queue is full, which
// is the backpressure that keeps the accept loop from buffering client
// fds without limit. Takes ownership of the fd: it is closed here if the
// pool is shutting down.
void http_pool_submit(HttpConnectionPool* pool, int client_fd);

/* Submit an already-established connection (a parked keep-alive connection
 * that became readable again, #1663). Unlike http_pool_submit this never
 * blocks waiting for queue space — the caller is the park poller thread,
 * and stalling it would hold up every other parked connection. Returns 0
 * when queued, -1 when the pool is absent, shutting down, or full; the
 * caller still owns the connection on -1. */
int http_pool_submit_conn(HttpConnectionPool* pool, void* conn);

// Drains the queue, joins every worker and frees the pool. Safe on NULL.
void http_pool_destroy(HttpConnectionPool* pool);

// Connections accepted and still waiting for a worker, across every pool in
// the process. Non-zero means holding a connection open costs another one its
// turn, which is what the keep-alive decision needs to know.
int http_pool_pending(void);

/* Workers currently running a connection, and the pool's worker count.
 * Parking (#1663) reads these to decide whether releasing a worker is
 * worth its cost: when the pool is not near saturation, looping on the
 * same worker is strictly cheaper than a poller round trip. Both return
 * 0 when no pool exists (inline / no-threads paths). */
int http_pool_busy(void);
int http_pool_workers(void);

#endif // AETHER_HTTP_POOL_H
