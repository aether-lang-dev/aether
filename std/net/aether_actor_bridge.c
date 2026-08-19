// aether_actor_bridge.c
//
// Bridges the runtime reactor (scheduler_io_register + MSG_IO_READY) to
// the Aether language surface. Callers use `await_io(fd)` from Aether
// code to suspend an actor until the fd becomes readable, without
// blocking any scheduler thread.
//
// Implementation note: this file intentionally stays in std/net because
// it is a thin shim over the already-public runtime API. No compiler
// changes are needed — the only requirement is that a user message
// named `IoReady` with fields `{ fd: int, events: int }` be registered,
// which the message registry routes to the reserved ID MSG_IO_READY.

#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/scheduler/aether_io_poller.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>

// TLS variables owned by the scheduler + send path. We reach into them
// here to identify the "current" actor and core at the point of call.
//
// g_current_step_actor is set by every generated *_step() function at
// entry (see compiler/codegen/codegen_actor.c), so it is valid in all
// scheduling modes: main-thread sync mode, scheduler-thread dispatch,
// and work-stealing. g_sync_step_actor is kept as a legacy fallback
// for the main-thread-only path.
extern AETHER_TLS int current_core_id;
extern AETHER_TLS void* g_current_step_actor;
extern AETHER_TLS ActorBase* g_sync_step_actor;

// Suspend the current actor until `fd` is readable. Returns 0 on
// success, -1 on failure (no active actor context, invalid fd, or the
// scheduler refused the registration).
//
// The caller must have received or spawned from within an actor's
// receive handler — `g_sync_step_actor` is only set while an actor's
// step() function is on the call stack. Calling await_io from main()
// with no active actor is a programming error and returns -1.
//
// On success, control returns to the caller immediately; the runtime
// does NOT block here. The actor resumes via a fresh step() invocation
// when the scheduler observes the fd is ready, with an IoReady message
// waiting at the front of its mailbox.
int ae_io_await(int fd) {
    if (fd < 0) return -1;

    ActorBase* actor = (ActorBase*)g_current_step_actor;
    if (!actor) actor = g_sync_step_actor;
    if (!actor) return -1;

    int core = current_core_id;
    if (core < 0) {
        // Main-thread actor mode: step() runs on the main thread with
        // current_core_id == -1, but the actor is still assigned to a
        // scheduler core. Route the registration through that core.
        core = 0;
    }

    return scheduler_io_register(core, fd, actor, AETHER_IO_READ);
}

// Cancel a prior await. Rarely needed — the scheduler's one-shot mode
// auto-unregisters after delivery — but exposed for completeness so
// callers can abandon an fd they decided not to wait on.
void ae_io_cancel(int fd) {
    if (fd < 0) return;
    int core = current_core_id;
    if (core < 0) core = 0;
    scheduler_io_unregister(core, fd);
}

// -----------------------------------------------------------------------------
// Minimal fd primitives
// -----------------------------------------------------------------------------
// These are thin wrappers over POSIX pipe/read/write/close. They exist
// primarily to make the reactor testable from Aether code and to give
// integration scaffolding a way to produce a readable fd without
// opening a real socket. Production code should prefer the TCP/HTTP
// APIs, which expose proper server/client handles.

// Create a pipe. Returns the read fd on success, -1 on failure.
// Stores the write fd in a per-process slot retrievable via
// ae_pipe_write_fd(). Single-pipe scope is sufficient for the one
// caller that needs it (the await_io end-to-end test); production code
// that needs multiple pipes should use its own syscall wrapper.
/* `_ae_pipe_read_fd` is consumed only on the POSIX path (ae_pipe_open
 * returns it directly), so guard it out on Windows — there's no pipe()
 * and no read-fd accessor, and an always-declared static would warn as
 * unused (-Wunused-variable). `_ae_pipe_write_fd` stays declared on every
 * platform because ae_pipe_write_fd() reads it unconditionally (it just
 * returns -1 on Windows). */
#ifndef _WIN32
static int _ae_pipe_read_fd = -1;
#endif
static int _ae_pipe_write_fd = -1;

int ae_pipe_open(void) {
#if defined(_WIN32) || !AETHER_HAS_PROCESS
    /* pipe() is POSIX; Windows callers must use sockets. WASI reaches the same
     * arm via AETHER_HAS_PROCESS: it has no pipe(), no fork, and no process
     * model to plumb one between (#1655). */
    return -1;
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    _ae_pipe_read_fd = fds[0];
    _ae_pipe_write_fd = fds[1];
    return _ae_pipe_read_fd;
#endif
}

int ae_pipe_write_fd(void) {
    return _ae_pipe_write_fd;
}

// Write a NUL-terminated string to an fd. Returns bytes written
// on success, -1 on failure.
int ae_fd_write(int fd, const char* data) {
    if (fd < 0 || !data) return -1;
    size_t len = strlen(data);
    ssize_t n = write(fd, data, len);
    return (int)n;
}

// Close an fd. No-op on invalid fd.
void ae_fd_close(int fd) {
    if (fd < 0) return;
    close(fd);
}
