// I/O poller backend: kqueue (macOS, FreeBSD, OpenBSD, NetBSD)

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include "aether_io_poller.h"
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int aether_io_poller_init(AetherIoPoller* poller) {
    poller->backend_data = NULL;
    poller->fd = kqueue();
    if (poller->fd < 0) {
        fprintf(stderr, "WARNING: kqueue failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int aether_io_poller_add(AetherIoPoller* poller, int fd, void* actor, uint32_t events) {
    if (poller->fd < 0) return -1;
    (void)actor;

    // EV_ONESHOT matches EPOLLONESHOT; EV_CLEAR matches EPOLLET.
    struct kevent changes[2];
    int nchanges = 0;
    unsigned short mode = (events & AETHER_IO_EDGE) ? EV_CLEAR : EV_ONESHOT;

    if (events & AETHER_IO_READ) {
        EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | mode, 0, 0, NULL);
        nchanges++;
    }
    if (events & AETHER_IO_WRITE) {
        EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | mode, 0, 0, NULL);
        nchanges++;
    }

    if (nchanges == 0) return -1;

    if (kevent(poller->fd, changes, nchanges, NULL, 0, NULL) < 0) {
        // EV_ADD on an existing fd just modifies it — no EEXIST handling needed
        return -1;
    }
    return 0;
}

int aether_io_poller_edge_capable(void) {
    return 1;   /* EV_CLEAR */
}

void aether_io_poller_remove(AetherIoPoller* poller, int fd) {
    if (poller->fd < 0) return;

    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    // Ignore errors — filter may not be registered
    kevent(poller->fd, changes, 2, NULL, 0, NULL);
}

int aether_io_poller_poll(AetherIoPoller* poller, AetherIoEvent* out, int max_events, int timeout_ms) {
    if (poller->fd < 0) return 0;

    struct kevent events[64];
    int cap = max_events < 64 ? max_events : 64;

    struct timespec ts;
    struct timespec* tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(poller->fd, NULL, 0, events, cap, tsp);
    if (n <= 0) return 0;

    for (int i = 0; i < n; i++) {
        out[i].fd = (int)events[i].ident;
        out[i].events = 0;
        if (events[i].filter == EVFILT_READ)  out[i].events |= AETHER_IO_READ;
        if (events[i].filter == EVFILT_WRITE) out[i].events |= AETHER_IO_WRITE;
        if (events[i].flags & EV_ERROR)       out[i].events |= AETHER_IO_ERROR;
        if (events[i].flags & EV_EOF)         out[i].events |= AETHER_IO_READ; // EOF is readable
    }
    return n;
}

void aether_io_poller_destroy(AetherIoPoller* poller) {
    if (poller->fd >= 0) {
        close(poller->fd);
        poller->fd = -1;
    }
}

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__
