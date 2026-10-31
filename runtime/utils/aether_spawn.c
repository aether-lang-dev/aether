/* The process's close-on-exec pipe and its fork (aether_spawn.h, #3046).
 * Moved here from std/os (#2917, #3028), so the runtime's own pipes and
 * contrib/host's take the same rules and the same lock as std.os's. */
#include "aether_spawn.h"
#include "../config/aether_optimization_config.h"

#if !defined(_WIN32) && !defined(__wasi__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#if AETHER_HAS_NETWORKING
#include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h> /* _NSGetEnviron: environ is not visible to a dylib */
#else
extern char** environ;
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
/* Declared here as well: glibc declares it only under _GNU_SOURCE. */
int pipe2(int pipefd[2], int flags);
#define AETHER_HAVE_PIPE2 1
#endif

#ifndef AETHER_HAVE_PIPE2
#include "aether_thread.h"
/* #3028: one per process. Pipe creation holds it shared and every fork holds
 * it exclusively, so no fork sees a pipe that is not yet close-on-exec. */
static pthread_rwlock_t aether_fork_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif

/* Can this process execute `path`, by its effective ids as execve judges? */
static int aether_spawn_can_exec(const char* path) {
#ifdef AT_EACCESS
    return faccessat(AT_FDCWD, path, X_OK, AT_EACCESS) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

/* Moved here from std/os (#3201) for spawn_sandboxed (#3200). */
char* aether_spawn_resolve(const char* prog) {
    if (!prog || !*prog) { errno = ENOENT; return NULL; }
    if (strchr(prog, '/')) return strdup(prog);
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    size_t plen = strlen(prog);
    const char* p = path;
    for (;;) {
        const char* end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        const char* dir = dlen ? p : ".";
        if (!dlen) dlen = 1;
        char* cand = (char*)malloc(dlen + 1 + plen + 1);
        if (!cand) return NULL;
        memcpy(cand, dir, dlen);
        cand[dlen] = '/';
        memcpy(cand + dlen + 1, prog, plen + 1);
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && aether_spawn_can_exec(cand)) return cand;
        free(cand);
        if (!end) break;
        p = end + 1;
    }
    errno = ENOENT;
    return NULL;
}

char** aether_spawn_environ(void) {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

/* Is `entry` ("KEY=..." from an env) one of the keys `set` gives? The whole
 * name before the `=` on both sides, so `PATH` is not `PATHEXT`. */
static int aether_spawn_env_has(const char* entry, const char* const* set, int nset) {
    const char* eeq = strchr(entry, '=');
    size_t elen = eeq ? (size_t)(eeq - entry) : strlen(entry);
    for (int i = 0; i < nset; i++) {
        const char* eq = strchr(set[i], '=');
        size_t klen = eq ? (size_t)(eq - set[i]) : strlen(set[i]);
        if (klen == elen && strncmp(entry, set[i], klen) == 0) return 1;
    }
    return 0;
}

char** aether_spawn_env(char** base, const char* const* set, int nset) {
    size_t n = 0;
    while (base && base[n]) n++;
    char** out = (char**)malloc((n + (size_t)nset + 1) * sizeof(char*));
    if (!out) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!aether_spawn_env_has(base[i], set, nset)) out[k++] = base[i];
    }
    for (int i = 0; i < nset; i++) out[k++] = (char*)set[i];
    out[k] = NULL;
    return out;
}

int aether_pipe_cloexec(int fds[2]) {
#ifdef AETHER_HAVE_PIPE2
    return pipe2(fds, O_CLOEXEC);
#else
    pthread_rwlock_rdlock(&aether_fork_lock);
    int rc = pipe(fds);
    if (rc == 0) {
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    pthread_rwlock_unlock(&aether_fork_lock);
    return rc == 0 ? 0 : -1;
#endif
}

/* Where std.os runs processes: a build without a filesystem has none to run
 * (its std.os is stubbed whole, Emscripten's and the embedded ones). */
#if AETHER_HAS_PROCESS && AETHER_HAS_FILESYSTEM
pid_t aether_fork(void) {
#ifdef AETHER_HAVE_PIPE2
    return fork();
#else
    pthread_rwlock_wrlock(&aether_fork_lock);
    pid_t pid = fork();
    if (pid != 0) pthread_rwlock_unlock(&aether_fork_lock);
    return pid;
#endif
}

/* std.os's socket channel (os.run_socket, os.socketpair). Every platform
 * with pipe2 has SOCK_CLOEXEC too; macOS has neither and takes the lock as
 * aether_pipe_cloexec does. */
int aether_socketpair_cloexec(int fds[2]) {
#if !AETHER_HAS_NETWORKING
    /* No sockets on this target (AETHER_NO_NETWORKING: embedded builds). */
    (void)fds;
    errno = ENOSYS;
    return -1;
#else
#ifdef AETHER_HAVE_PIPE2
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return -1;
#else
    pthread_rwlock_rdlock(&aether_fork_lock);
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if (rc == 0) {
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    pthread_rwlock_unlock(&aether_fork_lock);
    if (rc != 0) return -1;
#endif
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return 0;
#endif
}
#endif

#endif
