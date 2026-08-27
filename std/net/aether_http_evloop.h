#ifndef AETHER_HTTP_EVLOOP_H
#define AETHER_HTTP_EVLOOP_H

/* A proxy driver that runs many connections on one thread (#1758).
 *
 * The server gives a connection its own worker for that connection's whole
 * life, so whenever it waits, the thread waits: measured at 1.93 voluntary
 * context switches per proxied request against nginx's 0.00, from two sleeps
 * that have one cause between them. A thread with one connection and nothing
 * else to run can only sleep.
 *
 * This driver owns a poller and a set of connections, and never waits on any
 * single descriptor. The work is the same work: it drives the same proxy
 * exchange and the same upstream exchange the blocking path uses, so the two
 * cannot disagree about what a request means. What changes is who waits.
 *
 * It handles plain HTTP proxying. TLS, HTTP/2, upgrades and streaming bodies
 * stay on the worker path, which is why this is a driver and not a rewrite.
 */

typedef struct HttpEvLoop HttpEvLoop;
typedef struct HttpServer HttpServer;

/* Start `threads` drivers, each with its own poller. Returns NULL when the
 * platform has no poller or threads, and the caller keeps the worker path. */
HttpEvLoop* http_evloop_start(HttpServer* server, int threads);

/* Hand an accepted connection to a driver. Returns 0 when a driver took it
 * (the caller must not touch the descriptor again), -1 when none did. */
int http_evloop_submit(HttpEvLoop* loop, int client_fd);

/* How many connections the drivers are holding, for tests and reporting. */
int http_evloop_active(HttpEvLoop* loop);

void http_evloop_stop(HttpEvLoop* loop);

#endif // AETHER_HTTP_EVLOOP_H
