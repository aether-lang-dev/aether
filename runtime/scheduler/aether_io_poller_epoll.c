// I/O poller backend: epoll (Linux)

#ifdef __linux__

#include "aether_io_poller.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int aether_io_poller_init(AetherIoPoller* poller) {
    poller->backend_data = NULL;
    poller->fd = epoll_create1(EPOLL_CLOEXEC);
    if (poller->fd < 0) {
        fprintf(stderr, "WARNING: epoll_create1 failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int aether_io_poller_add(AetherIoPoller* poller, int fd, void* actor, uint32_t events) {
    if (poller->fd < 0) return -1;
    (void)actor; // Actor mapping is handled by the caller (io_map)

    struct epoll_event ev;
    /* One-shot unless the caller asked to stay registered. */
    ev.events = (events & AETHER_IO_EDGE) ? EPOLLET : EPOLLONESHOT;
    ev.data.fd = fd;
    if (events & AETHER_IO_READ)  ev.events |= EPOLLIN;
    if (events & AETHER_IO_WRITE) ev.events |= EPOLLOUT;

    if (epoll_ctl(poller->fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (errno == EEXIST) {
            if (epoll_ctl(poller->fd, EPOLL_CTL_MOD, fd, &ev) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

void aether_io_poller_remove(AetherIoPoller* poller, int fd) {
    if (poller->fd >= 0) {
        epoll_ctl(poller->fd, EPOLL_CTL_DEL, fd, NULL);
    }
}

int aether_io_poller_poll(AetherIoPoller* poller, AetherIoEvent* out, int max_events, int timeout_ms) {
    if (poller->fd < 0) return 0;

    struct epoll_event events[64];
    int cap = max_events < 64 ? max_events : 64;

    int n = epoll_wait(poller->fd, events, cap, timeout_ms);
    if (n <= 0) return 0;

    for (int i = 0; i < n; i++) {
        out[i].fd = events[i].data.fd;
        out[i].events = 0;
        if (events[i].events & EPOLLIN)  out[i].events |= AETHER_IO_READ;
        if (events[i].events & EPOLLOUT) out[i].events |= AETHER_IO_WRITE;
        if (events[i].events & (EPOLLERR | EPOLLHUP)) out[i].events |= AETHER_IO_ERROR;
    }
    return n;
}

void aether_io_poller_destroy(AetherIoPoller* poller) {
    if (poller->fd >= 0) {
        close(poller->fd);
        poller->fd = -1;
    }
}

#endif // __linux__
