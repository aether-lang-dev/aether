// I/O poller backend: poll() portable fallback
// Used on platforms without epoll or kqueue (Windows via WSAPoll, WASM, etc.)
//
// epoll and kqueue keep the interest set in the kernel, so one thread can
// register a descriptor while another waits and the wait sees it at once.
// Callers rely on that: the HTTP server's parking lot adds connections from
// its workers while its own thread waits (#1663). poll() has no such set; it
// waits on an array, so this backend keeps the set itself and gives callers
// the same guarantees (#2672):
//
//   - the set is guarded by a lock, and a wait polls a copy of it, so a
//     registration from another thread never touches the array poll() is
//     reading (it used to: an add could realloc it mid-wait);
//   - an add during a wait wakes it, through a descriptor of the poller's own
//     in the copy, so the new descriptor is watched at once rather than when
//     the wait times out;
//   - a wait on an empty set waits out its timeout, as the interface says;
//     it used to return at once, and a caller looping on it (the parking
//     lot's thread, on every Windows HTTP server) spun a core;
//   - each registration has a number, and a result is reported only for the
//     registration it was polled for, so a descriptor removed, closed and
//     reused during a wait cannot fire for its new owner.
//
// One thread waits on a poller at a time, as with every caller here; adds and
// removes may come from any thread. A build without threads keeps the plain
// single-threaded behaviour.

#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)

#ifdef _WIN32
#include <winsock2.h>   // before windows.h, which aether_thread.h includes
#endif
#include "aether_io_poller.h"
#include "../utils/aether_thread.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define poll WSAPoll
typedef ULONG nfds_t;
// On Windows, pollfd.fd is SOCKET (unsigned long long). Cast to avoid
// -Werror=sign-compare when comparing against int fd parameters.
#define AETHER_POLL_FD(fd) ((SOCKET)(fd))
#else
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define AETHER_POLL_FD(fd) (fd)
#endif

#ifndef AETHER_IO_MAX_FDS
#define AETHER_IO_MAX_FDS 4096
#endif

/* Threads are what make a registration concurrent with a wait; without them
 * there is nothing to guard and nothing to wake. */
#define AETHER_POLL_SHARED (AETHER_HAS_THREADS)

// Backend state for poll()-based poller
typedef struct {
    struct pollfd* fds;     // pollfd array
    unsigned*      ids;     // registration number of each entry
    int count;              // Number of active entries
    int capacity;           // Allocated size
    unsigned next_id;
#if AETHER_POLL_SHARED
    pthread_mutex_t lock;
    struct pollfd* snap;    // the waiting thread's copy of the set...
    unsigned*      snap_ids;
    int            snap_cap;
    int            waiting; // a wait is in progress on a copy
    int            wake_tried;
    int            wake_ok;
#ifdef _WIN32
    SOCKET         wake;    // UDP socket connected to itself
    int            wsa_started;
#else
    int            wake_rd, wake_wr;   // pipe
#endif
#endif
} PollBackend;

#if AETHER_POLL_SHARED

/* The wake descriptor, made on the first registration or wait, so a poller
 * that gets neither (every scheduler core of a program without I/O) costs
 * nothing. On failure the poller still works: a new descriptor is then
 * watched from the next wait, at most the caller's timeout later. Called
 * under the lock. */
static void wake_open(PollBackend* pb) {
    if (pb->wake_tried) return;
    pb->wake_tried = 1;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    pb->wsa_started = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = (int)sizeof(a);
    u_long nonblocking = 1;
    if (bind(s, (struct sockaddr*)&a, alen) != 0 ||
        getsockname(s, (struct sockaddr*)&a, &alen) != 0 ||
        connect(s, (struct sockaddr*)&a, alen) != 0 ||
        ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        closesocket(s);
        return;
    }
    pb->wake = s;
#else
    int p[2];
    if (pipe(p) != 0) return;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(p[i], F_GETFL, 0);
        if (fl < 0 || fcntl(p[i], F_SETFL, fl | O_NONBLOCK) != 0) {
            close(p[0]);
            close(p[1]);
            return;
        }
    }
    pb->wake_rd = p[0];
    pb->wake_wr = p[1];
#endif
    pb->wake_ok = 1;
}

static void wake_signal(PollBackend* pb) {
    if (!pb->wake_ok || !pb->waiting) return;
    /* One byte is enough, and a full buffer means a wake-up is already
     * pending, so a failed write loses nothing. */
#ifdef _WIN32
    (void)send(pb->wake, "w", 1, 0);
#else
    ssize_t w = write(pb->wake_wr, "w", 1);
    (void)w;
#endif
}

static void wake_drain(PollBackend* pb) {
    char buf[64];
#ifdef _WIN32
    while (recv(pb->wake, buf, (int)sizeof(buf), 0) > 0) {}
#else
    while (read(pb->wake_rd, buf, sizeof(buf)) > 0) {}
#endif
}

static void wake_close(PollBackend* pb) {
#ifdef _WIN32
    if (pb->wake_ok) closesocket(pb->wake);
    if (pb->wsa_started) WSACleanup();
#else
    if (pb->wake_ok) {
        close(pb->wake_rd);
        close(pb->wake_wr);
    }
#endif
    pb->wake_ok = 0;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    (void)poll(NULL, 0, ms);
#endif
}

#define POLL_LOCK(pb)   pthread_mutex_lock(&(pb)->lock)
#define POLL_UNLOCK(pb) pthread_mutex_unlock(&(pb)->lock)
#else
#define POLL_LOCK(pb)   ((void)0)
#define POLL_UNLOCK(pb) ((void)0)
#endif

int aether_io_poller_init(AetherIoPoller* poller) {
    PollBackend* pb = calloc(1, sizeof(PollBackend));
    if (!pb) return -1;

    pb->capacity = 64; // Start small, grow as needed
    pb->fds = calloc(pb->capacity, sizeof(struct pollfd));
    pb->ids = calloc(pb->capacity, sizeof(unsigned));
    if (!pb->fds || !pb->ids) {
        free(pb->fds);
        free(pb->ids);
        free(pb);
        return -1;
    }
    pb->count = 0;
#if AETHER_POLL_SHARED
    pthread_mutex_init(&pb->lock, NULL);
#endif

    poller->fd = -1; // No kernel fd for poll() backend
    poller->backend_data = pb;
    return 0;
}

int aether_io_poller_add(AetherIoPoller* poller, int fd, void* actor, uint32_t events) {
    (void)actor;
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb) return -1;

    short want = 0;
    if (events & AETHER_IO_READ)  want |= POLLIN;
    if (events & AETHER_IO_WRITE) want |= POLLOUT;

    POLL_LOCK(pb);
#if AETHER_POLL_SHARED
    wake_open(pb);
#endif
    // Check if fd already registered — update in place
    for (int i = 0; i < pb->count; i++) {
        if (pb->fds[i].fd == AETHER_POLL_FD(fd)) {
            pb->fds[i].events = want;
#if AETHER_POLL_SHARED
            wake_signal(pb);
#endif
            POLL_UNLOCK(pb);
            return 0;
        }
    }

    // Grow if needed
    if (pb->count >= pb->capacity) {
        int new_cap = pb->capacity * 2;
        if (new_cap > AETHER_IO_MAX_FDS) new_cap = AETHER_IO_MAX_FDS;
        if (pb->count >= new_cap) { POLL_UNLOCK(pb); return -1; } // At limit
        struct pollfd* new_fds = realloc(pb->fds, new_cap * sizeof(struct pollfd));
        if (!new_fds) { POLL_UNLOCK(pb); return -1; }
        pb->fds = new_fds;
        unsigned* new_ids = realloc(pb->ids, new_cap * sizeof(unsigned));
        if (!new_ids) { POLL_UNLOCK(pb); return -1; }
        pb->ids = new_ids;
        pb->capacity = new_cap;
    }

    struct pollfd* pfd = &pb->fds[pb->count];
    pfd->fd = AETHER_POLL_FD(fd);
    pfd->events = want;
    pfd->revents = 0;
    pb->ids[pb->count] = ++pb->next_id;
    pb->count++;
#if AETHER_POLL_SHARED
    wake_signal(pb);
#endif
    POLL_UNLOCK(pb);
    return 0;
}

int aether_io_poller_edge_capable(void) {
    return 0;   /* poll() has no edge mode */
}

/* Removes entry i: the last one takes its place. */
static void remove_at(PollBackend* pb, int i) {
    pb->fds[i] = pb->fds[pb->count - 1];
    pb->ids[i] = pb->ids[pb->count - 1];
    pb->count--;
}

void aether_io_poller_remove(AetherIoPoller* poller, int fd) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb) return;

    /* No wake-up: a wait still holding the descriptor in its copy reports
     * nothing for it, its registration being gone. */
    POLL_LOCK(pb);
    for (int i = 0; i < pb->count; i++) {
        if (pb->fds[i].fd == AETHER_POLL_FD(fd)) {
            remove_at(pb, i);
            break;
        }
    }
    POLL_UNLOCK(pb);
}

#if AETHER_POLL_SHARED
/* One wait, on a copy of the set plus the wake descriptor, for up to wait_ms.
 * Returns the events reported; *woken says whether an add ended the wait. */
static int poll_once(PollBackend* pb, AetherIoEvent* out, int max_events,
                     int wait_ms, int* woken) {
    *woken = 0;
    POLL_LOCK(pb);
    /* A wait on an empty set needs the wake descriptor as much as an add
     * does: it is what the wait blocks on, and what an add ends it with. */
    wake_open(pb);
    int n = pb->count;
    if (n == 0 && !pb->wake_ok) {
        /* No wake descriptor could be made: nothing to wait on, and an add
         * from here on is seen by the next call. A wait without a timeout
         * sleeps in bounded steps, so its caller's loop does not spin. */
        POLL_UNLOCK(pb);
        sleep_ms(wait_ms < 0 ? 100 : wait_ms);
        return 0;
    }
    int total = n + (pb->wake_ok ? 1 : 0);
    if (total > pb->snap_cap) {
        struct pollfd* s = realloc(pb->snap, (size_t)total * sizeof(struct pollfd));
        if (!s) { POLL_UNLOCK(pb); return 0; }
        pb->snap = s;
        unsigned* si = realloc(pb->snap_ids, (size_t)total * sizeof(unsigned));
        if (!si) { POLL_UNLOCK(pb); return 0; }
        pb->snap_ids = si;
        pb->snap_cap = total;
    }
    memcpy(pb->snap, pb->fds, (size_t)n * sizeof(struct pollfd));
    memcpy(pb->snap_ids, pb->ids, (size_t)n * sizeof(unsigned));
    if (pb->wake_ok) {
#ifdef _WIN32
        pb->snap[n].fd = pb->wake;
#else
        pb->snap[n].fd = pb->wake_rd;
#endif
        pb->snap[n].events = POLLIN;
    }
    for (int i = 0; i < total; i++) pb->snap[i].revents = 0;
    pb->waiting = 1;
    POLL_UNLOCK(pb);

    int r = poll(pb->snap, (nfds_t)total, wait_ms);

    POLL_LOCK(pb);
    pb->waiting = 0;
    int count = 0;
    if (r > 0) {
        if (pb->wake_ok && pb->snap[n].revents) {
            wake_drain(pb);
            *woken = 1;
        }
        for (int i = 0; i < n && count < max_events; i++) {
            if (pb->snap[i].revents == 0) continue;
            /* The registration polled, if it is still there: where it was
             * copied from unless the set changed since, else found by its
             * number. */
            int at = -1;
            if (i < pb->count && pb->ids[i] == pb->snap_ids[i]) {
                at = i;
            } else {
                for (int j = 0; j < pb->count; j++) {
                    if (pb->ids[j] == pb->snap_ids[i]) { at = j; break; }
                }
            }
            if (at < 0) continue;   // removed during the wait

            short rev = pb->snap[i].revents;
            out[count].fd = (int)pb->snap[i].fd;
            out[count].events = 0;
            if (rev & POLLIN)                out[count].events |= AETHER_IO_READ;
            if (rev & POLLOUT)               out[count].events |= AETHER_IO_WRITE;
            if (rev & (POLLERR | POLLHUP))   out[count].events |= AETHER_IO_ERROR;
            count++;

            // Emulate one-shot: remove fired fd (same as EPOLLONESHOT)
            remove_at(pb, at);
        }
    }
    POLL_UNLOCK(pb);
    return count;
}
#endif

int aether_io_poller_poll(AetherIoPoller* poller, AetherIoEvent* out, int max_events, int timeout_ms) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb) return 0;

#if AETHER_POLL_SHARED
    /* A wait an add woke takes a new copy and waits again for whatever is
     * left of its timeout, so the descriptor added is reported by this call,
     * as epoll and kqueue report it. */
    uint64_t deadline_ns = timeout_ms > 0
        ? aether_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0;
    int wait_ms = timeout_ms;
    for (;;) {
        int woken = 0;
        int count = poll_once(pb, out, max_events, wait_ms, &woken);
        if (count > 0 || !woken || timeout_ms == 0) return count;
        if (timeout_ms > 0) {
            uint64_t now = aether_now_ns();
            if (now >= deadline_ns) return 0;
            wait_ms = (int)((deadline_ns - now + 999999ULL) / 1000000ULL);
        }
    }
#else
    if (pb->count == 0) return 0;

    int n = poll(pb->fds, (nfds_t)pb->count, timeout_ms);
    if (n <= 0) return 0;

    int count = 0;
    for (int i = 0; i < pb->count && count < max_events; i++) {
        if (pb->fds[i].revents == 0) continue;

        out[count].fd = (int)pb->fds[i].fd;
        out[count].events = 0;
        if (pb->fds[i].revents & POLLIN)                out[count].events |= AETHER_IO_READ;
        if (pb->fds[i].revents & POLLOUT)               out[count].events |= AETHER_IO_WRITE;
        if (pb->fds[i].revents & (POLLERR | POLLHUP))   out[count].events |= AETHER_IO_ERROR;
        count++;

        // Emulate one-shot: remove fired fd (same as EPOLLONESHOT)
        remove_at(pb, i);
        i--; // Re-check swapped entry
    }
    return count;
#endif
}

void aether_io_poller_destroy(AetherIoPoller* poller) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (pb) {
#if AETHER_POLL_SHARED
        wake_close(pb);
        pthread_mutex_destroy(&pb->lock);
        free(pb->snap);
        free(pb->snap_ids);
#endif
        free(pb->fds);
        free(pb->ids);
        free(pb);
        poller->backend_data = NULL;
    }
    poller->fd = -1;
}

#endif // portable fallback
