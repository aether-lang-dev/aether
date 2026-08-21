/* aether_http_park.c — the idle keep-alive connection lot (#1663).
 *
 * One poller thread holds every connection that has no request in flight.
 * When a client speaks again the connection goes back to a worker; when it
 * says nothing for the idle timeout it is closed. The cost of an idle
 * connection becomes a descriptor and a table slot rather than a thread,
 * which is the difference between the worker count and the descriptor limit.
 *
 * Ownership is strictly one holder at a time: a worker owns a connection,
 * hands it to the lot, and the lot hands it back through `resume`. The lock
 * covers the table, never the callback.
 */

#include "aether_http_park.h"
#include "../../runtime/scheduler/aether_io_poller.h"
#include "../../runtime/utils/aether_thread.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AETHER_HAS_THREADS

/* How long the poller sleeps between sweeps. Wakeups are event-driven, so
 * this only bounds how late a deadline is noticed, and a keep-alive timeout
 * measured in seconds does not need finer resolution than this. */
#define PARK_POLL_MS      200
#define PARK_MAX_EVENTS   64

typedef struct {
    HttpConn* conn;
    int       fd;
    int64_t   deadline_ms;
} ParkSlot;

/* fd -> slot, open addressing with linear probing. The poller reports a bare
 * descriptor, and finding its connection by walking the table would make every
 * wakeup cost the number of parked connections, which is the one number this
 * whole file exists to stop mattering. */
typedef struct {
    int fd;     /* PARK_IX_FREE, PARK_IX_DEAD, or a live descriptor */
    int slot;
} ParkIndexEntry;

#define PARK_IX_FREE (-1)
#define PARK_IX_DEAD (-2)

struct HttpParkLot {
    HttpServer* server;
    void (*resume)(HttpServer*, HttpConn*);

    AetherIoPoller poller;
    int poller_ready;

    ParkSlot* slots;
    int       capacity;
    int       count;

    ParkIndexEntry* index;
    int             index_mask;   /* index size is a power of two */
    int             index_dead;

    pthread_mutex_t lock;
    pthread_t       thread;
    int             thread_started;
    atomic_int      shutdown;
};

/* Descriptors are handed out low-first on POSIX and in small steps on Windows,
 * so the low bits are the informative ones; the multiply spreads them. */
static inline int park_ix_start(const HttpParkLot* lot, int fd) {
    return (int)(((uint32_t)fd * 2654435761u) >> 16) & lot->index_mask;
}

/* Caller holds the lock. */
static void park_index_set(HttpParkLot* lot, int fd, int slot) {
    int i = park_ix_start(lot, fd);
    int first_dead = -1;
    for (;;) {
        int cur = lot->index[i].fd;
        if (cur == fd) { lot->index[i].slot = slot; return; }
        if (cur == PARK_IX_DEAD && first_dead < 0) first_dead = i;
        if (cur == PARK_IX_FREE) {
            if (first_dead >= 0) { i = first_dead; lot->index_dead--; }
            lot->index[i].fd = fd;
            lot->index[i].slot = slot;
            return;
        }
        i = (i + 1) & lot->index_mask;
    }
}

/* Caller holds the lock. Returns the slot for `fd`, or -1. */
static int park_index_get(const HttpParkLot* lot, int fd) {
    int i = park_ix_start(lot, fd);
    for (;;) {
        int cur = lot->index[i].fd;
        if (cur == fd) return lot->index[i].slot;
        if (cur == PARK_IX_FREE) return -1;
        i = (i + 1) & lot->index_mask;
    }
}

/* Caller holds the lock. */
static void park_index_erase(HttpParkLot* lot, int fd) {
    int i = park_ix_start(lot, fd);
    for (;;) {
        int cur = lot->index[i].fd;
        if (cur == fd) {
            lot->index[i].fd = PARK_IX_DEAD;
            lot->index_dead++;
            return;
        }
        if (cur == PARK_IX_FREE) return;
        i = (i + 1) & lot->index_mask;
    }
}

/* Caller holds the lock. Tombstones lengthen every probe that passes them, and
 * left alone they would eventually leave no free entry at all, which is a probe
 * that never ends rather than a slow one. Rebuilt from the slots, which are the
 * authority, once they reach half the capacity: live + dead then stays under
 * three quarters of the index, so a free entry always exists. Amortised O(1)
 * per removal. */
static void park_index_compact(HttpParkLot* lot) {
    for (int i = 0; i <= lot->index_mask; i++) lot->index[i].fd = PARK_IX_FREE;
    lot->index_dead = 0;
    for (int i = 0; i < lot->count; i++) park_index_set(lot, lot->slots[i].fd, i);
}

static int64_t park_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (int64_t)time(NULL) * 1000;
}

/* Caller holds the lock. Removes slot `i` from the table and returns its
 * connection; the fd is left registered or not per `also_unpoll`. */
static HttpConn* park_take_locked(HttpParkLot* lot, int i, int also_unpoll) {
    HttpConn* conn = lot->slots[i].conn;
    if (also_unpoll) aether_io_poller_remove(&lot->poller, lot->slots[i].fd);
    park_index_erase(lot, lot->slots[i].fd);
    lot->count--;
    if (i != lot->count) {
        lot->slots[i] = lot->slots[lot->count];
        park_index_set(lot, lot->slots[i].fd, i);
    }
    if (lot->index_dead * 2 >= lot->capacity) park_index_compact(lot);
    return conn;
}

/* Close a connection the lot still owns. Declared by the server; kept behind
 * this shim so the lot needs nothing else from it. */
extern void http_conn_close_owned(HttpConn* conn);

static void* park_thread(void* arg) {
    HttpParkLot* lot = (HttpParkLot*)arg;
    AetherIoEvent events[PARK_MAX_EVENTS];

    HttpConn* expired[PARK_MAX_EVENTS];

    while (!atomic_load(&lot->shutdown)) {
        int n = aether_io_poller_poll(&lot->poller, events, PARK_MAX_EVENTS, PARK_POLL_MS);

        for (int e = 0; e < n && !atomic_load(&lot->shutdown); e++) {
            HttpConn* conn = NULL;
            pthread_mutex_lock(&lot->lock);
            int slot = park_index_get(lot, events[e].fd);
            if (slot >= 0) conn = park_take_locked(lot, slot, 1);
            pthread_mutex_unlock(&lot->lock);
            /* The callback runs outside the lock: it hands the connection to a
             * worker pool whose submit can block on a full queue, and holding
             * the table there would stall every other wakeup behind it. */
            if (conn) lot->resume(lot->server, conn);
        }

        /* Deadlines. A client that goes away silently is reaped here rather
         * than by a thread sitting in recv. Swept in batches so a lot that
         * empties all at once costs one pass per batch, not one per
         * connection. */
        int64_t now = park_now_ms();
        for (;;) {
            int found = 0;
            pthread_mutex_lock(&lot->lock);
            for (int i = lot->count - 1; i >= 0 && found < PARK_MAX_EVENTS; i--) {
                if (now >= lot->slots[i].deadline_ms)
                    expired[found++] = park_take_locked(lot, i, 1);
            }
            pthread_mutex_unlock(&lot->lock);
            for (int i = 0; i < found; i++) http_conn_close_owned(expired[i]);
            if (found < PARK_MAX_EVENTS) break;
        }
    }
    return NULL;
}

HttpParkLot* http_park_create(HttpServer* server,
                              void (*resume)(HttpServer*, HttpConn*),
                              int capacity) {
    if (!server || !resume || capacity <= 0) return NULL;

    HttpParkLot* lot = (HttpParkLot*)calloc(1, sizeof(HttpParkLot));
    if (!lot) return NULL;
    lot->server = server;
    lot->resume = resume;
    lot->capacity = capacity;
    lot->slots = (ParkSlot*)calloc((size_t)capacity, sizeof(ParkSlot));
    if (!lot->slots) { free(lot); return NULL; }

    /* Index sized to at least twice the slots and rounded up to a power of
     * two, so linear probing never runs past a half-full table. */
    int index_size = 1;
    while (index_size < capacity * 2) index_size <<= 1;
    lot->index = (ParkIndexEntry*)malloc((size_t)index_size * sizeof(ParkIndexEntry));
    if (!lot->index) { free(lot->slots); free(lot); return NULL; }
    lot->index_mask = index_size - 1;
    for (int i = 0; i < index_size; i++) lot->index[i].fd = PARK_IX_FREE;

    if (aether_io_poller_init(&lot->poller) != 0) {
        free(lot->index);
        free(lot->slots);
        free(lot);
        return NULL;
    }
    lot->poller_ready = 1;
    pthread_mutex_init(&lot->lock, NULL);

    if (pthread_create(&lot->thread, NULL, park_thread, lot) != 0) {
        /* No thread, no lot: the caller keeps connections on their workers,
         * which is what happened before parking existed. */
        pthread_mutex_destroy(&lot->lock);
        aether_io_poller_destroy(&lot->poller);
        free(lot->index);
        free(lot->slots);
        free(lot);
        return NULL;
    }
    lot->thread_started = 1;
    return lot;
}

int http_park_add(HttpParkLot* lot, HttpConn* conn, int idle_ms) {
    if (!lot || !conn || atomic_load(&lot->shutdown)) return -1;
    int fd = http_conn_fd(conn);
    if (fd < 0) return -1;

    pthread_mutex_lock(&lot->lock);
    if (lot->count >= lot->capacity) {
        pthread_mutex_unlock(&lot->lock);
        return -1;
    }
    /* Register before publishing the slot: an event that arrives between the
     * two would find no slot and be dropped, and the connection would wait
     * for its deadline instead of its client. */
    if (aether_io_poller_add(&lot->poller, fd, conn, AETHER_IO_READ) != 0) {
        pthread_mutex_unlock(&lot->lock);
        return -1;
    }
    lot->slots[lot->count].conn = conn;
    lot->slots[lot->count].fd = fd;
    lot->slots[lot->count].deadline_ms = park_now_ms() + (idle_ms > 0 ? idle_ms : 30000);
    park_index_set(lot, fd, lot->count);
    lot->count++;
    pthread_mutex_unlock(&lot->lock);
    return 0;
}

int http_park_count(HttpParkLot* lot) {
    if (!lot) return 0;
    pthread_mutex_lock(&lot->lock);
    int n = lot->count;
    pthread_mutex_unlock(&lot->lock);
    return n;
}

void http_park_destroy(HttpParkLot* lot) {
    if (!lot) return;
    atomic_store(&lot->shutdown, 1);
    if (lot->thread_started) pthread_join(lot->thread, NULL);

    pthread_mutex_lock(&lot->lock);
    while (lot->count > 0) {
        HttpConn* conn = park_take_locked(lot, 0, 1);
        pthread_mutex_unlock(&lot->lock);
        http_conn_close_owned(conn);
        pthread_mutex_lock(&lot->lock);
    }
    pthread_mutex_unlock(&lot->lock);

    pthread_mutex_destroy(&lot->lock);
    if (lot->poller_ready) aether_io_poller_destroy(&lot->poller);
    free(lot->index);
    free(lot->slots);
    free(lot);
}

#else /* !AETHER_HAS_THREADS */

HttpParkLot* http_park_create(HttpServer* server,
                              void (*resume)(HttpServer*, HttpConn*),
                              int capacity) {
    (void)server; (void)resume; (void)capacity;
    return NULL;   /* no threads: connections stay on their caller, as before */
}
int  http_park_add(HttpParkLot* lot, HttpConn* conn, int idle_ms) {
    (void)lot; (void)conn; (void)idle_ms; return -1;
}
int  http_park_count(HttpParkLot* lot) { (void)lot; return 0; }
void http_park_destroy(HttpParkLot* lot) { (void)lot; }

#endif /* AETHER_HAS_THREADS */
