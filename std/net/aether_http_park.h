#ifndef AETHER_HTTP_PARK_H
#define AETHER_HTTP_PARK_H

#include "aether_http_server.h"

/* Idle keep-alive connections, held by a poller instead of by a worker (#1663).
 *
 * A worker owns its connection for that connection's whole life, so without
 * this the number of connections a server can hold open is the worker count:
 * measured on an 8-core box, keep-alive was worth 3.7x at 8 concurrent clients
 * and collapsed to 99 rps at 20, where four connections never reached a worker.
 * #1653 papered over the collapse by refusing to keep a connection while
 * another was queued, which is correct but caps the win at the worker count.
 *
 * Parking removes the cap: a connection with no request in flight costs one
 * file descriptor and one table slot, not a thread. The lot owns the
 * connection while it waits, wakes it when the client speaks again, and closes
 * it when the idle deadline passes with nothing said.
 */

typedef struct HttpParkLot HttpParkLot;
typedef struct HttpConn HttpConn;

/* Start the lot and its poller thread. `resume` is called on the poller
 * thread with a connection that has data waiting; it must take ownership.
 * Returns NULL when threads or a poller are unavailable, in which case the
 * caller keeps the pre-parking behaviour. */
HttpParkLot* http_park_create(HttpServer* server,
                              void (*resume)(HttpServer*, HttpConn*),
                              int capacity);

/* Hand an idle connection to the lot, to be resumed when readable or closed
 * after `idle_ms` of silence. Returns 0 when the lot took it (the caller must
 * not touch `conn` again), -1 when it did not (the caller still owns it). */
int http_park_add(HttpParkLot* lot, HttpConn* conn, int idle_ms);

/* How many connections are parked right now. For tests and for the server's
 * own reporting; not a synchronisation point. */
int http_park_count(HttpParkLot* lot);

/* Stop the poller thread and close every parked connection. Safe on NULL. */
void http_park_destroy(HttpParkLot* lot);

#endif // AETHER_HTTP_PARK_H
