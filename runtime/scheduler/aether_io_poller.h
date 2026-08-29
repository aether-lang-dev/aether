// Platform-agnostic I/O poller interface
// Backend selection: epoll (Linux), kqueue (macOS/BSD), poll() (portable fallback)

#ifndef AETHER_IO_POLLER_H
#define AETHER_IO_POLLER_H

#include <stdint.h>

// Portable I/O event flags
#define AETHER_IO_READ  0x001
#define AETHER_IO_WRITE 0x004
#define AETHER_IO_ERROR 0x008

/* Stay registered, and report only on a change.
 *
 * Without this a registration is one-shot: it fires once and has to be armed
 * again, which costs a syscall per wait. That suits a caller that waits on a
 * descriptor occasionally, and it is what the scheduler wants. It does not
 * suit a driver waiting on the same descriptors continuously, which pays the
 * re-arm on every request.
 *
 * A caller asking for this must drain a descriptor until it would block,
 * because nothing will report the same readiness twice. Backends that cannot
 * express it stay level-triggered, which is safe for the same caller: it
 * reports more often, never less. */
#define AETHER_IO_EDGE  0x010

// Single I/O event returned by aether_io_poller_poll
typedef struct {
    int fd;
    uint32_t events;    // AETHER_IO_READ, AETHER_IO_WRITE, AETHER_IO_ERROR
} AetherIoEvent;

// Opaque backend handle (epoll fd, kqueue fd, or poll state pointer)
typedef struct {
    int fd;             // Backend fd (epoll/kqueue) or -1 for poll()
    void* backend_data; // Backend-specific state (used by poll() fallback)
} AetherIoPoller;

// Initialize a poller instance. Returns 0 on success, -1 on failure.
int  aether_io_poller_init(AetherIoPoller* poller);

// Register fd for monitoring. events is a bitmask of AETHER_IO_READ/WRITE.
// actor is opaque user data associated with this fd.
// Returns 0 on success, -1 on failure.
int  aether_io_poller_add(AetherIoPoller* poller, int fd, void* actor, uint32_t events);

/* Whether this backend really does what AETHER_IO_EDGE asks, rather than
 * staying level-triggered. A caller that wants to register write interest
 * once and leave it there has to know: on an edge-triggered backend that
 * costs nothing, and on a level-triggered one a writable descriptor reports
 * ready every single wait. It is a property of the backend compiled in, not
 * of an instance. */
int  aether_io_poller_edge_capable(void);

// Remove fd from monitoring.
void aether_io_poller_remove(AetherIoPoller* poller, int fd);

// Poll for ready events. Fills out[] with up to max_events results.
// timeout_ms: 0 = non-blocking, >0 = wait up to N ms, -1 = block indefinitely.
// Returns number of events written to out[], or 0 on timeout, -1 on error.
int  aether_io_poller_poll(AetherIoPoller* poller, AetherIoEvent* out, int max_events, int timeout_ms);

// Destroy poller and release all resources.
void aether_io_poller_destroy(AetherIoPoller* poller);

#endif // AETHER_IO_POLLER_H
