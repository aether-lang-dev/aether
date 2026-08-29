// I/O poller backend: poll() portable fallback
// Used on platforms without epoll or kqueue (Windows via WSAPoll, WASM, etc.)

#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)

#include "aether_io_poller.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#define poll WSAPoll
typedef ULONG nfds_t;
// On Windows, pollfd.fd is SOCKET (unsigned long long). Cast to avoid
// -Werror=sign-compare when comparing against int fd parameters.
#define AETHER_POLL_FD(fd) ((SOCKET)(fd))
#else
#include <poll.h>
#include <unistd.h>
#define AETHER_POLL_FD(fd) (fd)
#endif

#ifndef AETHER_IO_MAX_FDS
#define AETHER_IO_MAX_FDS 4096
#endif

// Backend state for poll()-based poller
typedef struct {
    struct pollfd* fds;     // pollfd array
    int count;              // Number of active entries
    int capacity;           // Allocated size
} PollBackend;

int aether_io_poller_init(AetherIoPoller* poller) {
    PollBackend* pb = calloc(1, sizeof(PollBackend));
    if (!pb) return -1;

    pb->capacity = 64; // Start small, grow as needed
    pb->fds = calloc(pb->capacity, sizeof(struct pollfd));
    if (!pb->fds) {
        free(pb);
        return -1;
    }
    pb->count = 0;

    poller->fd = -1; // No kernel fd for poll() backend
    poller->backend_data = pb;
    return 0;
}

int aether_io_poller_add(AetherIoPoller* poller, int fd, void* actor, uint32_t events) {
    (void)actor;
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb) return -1;

    // Check if fd already registered — update in place
    for (int i = 0; i < pb->count; i++) {
        if (pb->fds[i].fd == AETHER_POLL_FD(fd)) {
            pb->fds[i].events = 0;
            if (events & AETHER_IO_READ)  pb->fds[i].events |= POLLIN;
            if (events & AETHER_IO_WRITE) pb->fds[i].events |= POLLOUT;
            return 0;
        }
    }

    // Grow if needed
    if (pb->count >= pb->capacity) {
        int new_cap = pb->capacity * 2;
        if (new_cap > AETHER_IO_MAX_FDS) new_cap = AETHER_IO_MAX_FDS;
        if (pb->count >= new_cap) return -1; // At limit
        struct pollfd* new_fds = realloc(pb->fds, new_cap * sizeof(struct pollfd));
        if (!new_fds) return -1;
        pb->fds = new_fds;
        pb->capacity = new_cap;
    }

    struct pollfd* pfd = &pb->fds[pb->count];
    pfd->fd = AETHER_POLL_FD(fd);
    pfd->events = 0;
    pfd->revents = 0;
    if (events & AETHER_IO_READ)  pfd->events |= POLLIN;
    if (events & AETHER_IO_WRITE) pfd->events |= POLLOUT;
    pb->count++;
    return 0;
}

int aether_io_poller_edge_capable(void) {
    return 0;   /* poll() has no edge mode */
}

void aether_io_poller_remove(AetherIoPoller* poller, int fd) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb) return;

    for (int i = 0; i < pb->count; i++) {
        if (pb->fds[i].fd == AETHER_POLL_FD(fd)) {
            // Swap with last element for O(1) removal
            pb->fds[i] = pb->fds[pb->count - 1];
            pb->count--;
            return;
        }
    }
}

int aether_io_poller_poll(AetherIoPoller* poller, AetherIoEvent* out, int max_events, int timeout_ms) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (!pb || pb->count == 0) return 0;

    int n = poll(pb->fds, (nfds_t)pb->count, timeout_ms);
    if (n <= 0) return 0;

    int count = 0;
    for (int i = 0; i < pb->count && count < max_events; i++) {
        if (pb->fds[i].revents == 0) continue;

        out[count].fd = pb->fds[i].fd;
        out[count].events = 0;
        if (pb->fds[i].revents & POLLIN)                out[count].events |= AETHER_IO_READ;
        if (pb->fds[i].revents & POLLOUT)               out[count].events |= AETHER_IO_WRITE;
        if (pb->fds[i].revents & (POLLERR | POLLHUP))   out[count].events |= AETHER_IO_ERROR;
        count++;

        // Emulate one-shot: remove fired fd (same as EPOLLONESHOT)
        pb->fds[i] = pb->fds[pb->count - 1];
        pb->count--;
        i--; // Re-check swapped entry
    }
    return count;
}

void aether_io_poller_destroy(AetherIoPoller* poller) {
    PollBackend* pb = (PollBackend*)poller->backend_data;
    if (pb) {
        free(pb->fds);
        free(pb);
        poller->backend_data = NULL;
    }
    poller->fd = -1;
}

#endif // portable fallback
