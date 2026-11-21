#include "aether_os.h"
#include "../mem/aether_grow.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_spawn.h"
#include "../string/aether_string.h"
#include <stdlib.h>

/* The empty string an `@heap` tuple slot holds on an error return: a
 * counted one, which the caller releases like any other, never a
 * literal (#420 v2). */
static char* aether_os_empty_heap(void) {
    return (char*)string_empty();
}

/* os.run_stdio's `stderr_to` (std.os STDERR_INHERIT, STDERR_DISCARD,
 * STDERR_FILE): where the child's stderr goes. */
#define OS_STDERR_INHERIT 0
#define OS_STDERR_DISCARD 1
#define OS_STDERR_FILE 2

/* The reason a stderr choice is refused before anything is spawned, or
 * NULL: an unknown mode, or a file mode with no path. */
static inline const char* os_stdio_stderr_check(int stderr_to, const char* stderr_path) {
    if (stderr_to != OS_STDERR_INHERIT && stderr_to != OS_STDERR_DISCARD &&
        stderr_to != OS_STDERR_FILE)
        return "os.run_stdio: stderr_to is not STDERR_INHERIT, STDERR_DISCARD or STDERR_FILE";
    if (stderr_to == OS_STDERR_FILE && (!stderr_path || !stderr_path[0]))
        return "os.run_stdio: STDERR_FILE needs a stderr_path";
    return NULL;
}

// NOTE on aether_argv0 placement: the implementation lives in
// runtime/aether_runtime.c next to the aether_argc / aether_argv it
// reads. aether_os.c cannot reference those variables directly because
// the compiler binary (aetherc) links std/os/aether_os.c but NOT
// runtime/aether_runtime.c, so a hard reference would break the
// compiler link. Keeping the function next to its state fixes that and
// leaves this file focused on shell/exec helpers.

/* Two independent capabilities gate this file.
 *
 * !AETHER_HAS_FILESYSTEM stubs EVERYTHING (the historical behaviour, used by
 * the Emscripten build, which passes -DAETHER_NO_FILESYSTEM).
 *
 * !AETHER_HAS_PROCESS stubs only the fork/exec/pipe/wait surface, leaving the
 * filesystem and clock functions real. That is the WASI shape: it has a proper
 * capability-based filesystem — preopened directories, sandboxed but real —
 * and no process model whatsoever. Before #1655 the only way to compile out
 * `fork` was to compile out the filesystem with it, because both lived behind
 * one macro; that trade is fine for Emscripten and would have disabled std.fs
 * on the one target that supports it properly.
 */
#if !AETHER_HAS_FILESYSTEM
int os_system(const char* c) { (void)c; return -1; }
char* os_exec_raw(const char* c) { (void)c; return NULL; }
char* os_getenv(const char* n) { (void)n; return NULL; }
int os_execv(const char* p, void* a) { (void)p; (void)a; return -1; }
char* os_which(const char* n) { (void)n; return NULL; }
int os_chdir_raw(const char* p) { (void)p; return -1; }
char* os_getcwd_raw(void) { return NULL; }
void* os_environ_raw(void) { return NULL; }   /* the empty *StringSeq */
typedef struct { void* _0; const char* _1; } _tuple_ptr_string_os;
_tuple_ptr_string_os os_signal_watch_raw(const int* s, int n) {
    (void)s; (void)n; _tuple_ptr_string_os out = { NULL, "os.signal_watch unavailable" }; return out;
}
int os_signal_received_raw(void* w) { (void)w; return 0; }
void os_signal_unwatch_raw(void* w) { (void)w; }
int os_run(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return -1; }
char* os_run_capture_raw(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return NULL; }
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;
_tuple_string_int_string os_run_capture_status_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_capture unavailable" };
    return out;
}
typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;
_tuple_int_int_string os_run_pipe_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_int_string out = { -1, -1, "os.run_pipe unavailable" };
    return out;
}
_tuple_int_int_string os_run_socket_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_int_string out = { -1, -1, "os.run_socket unavailable" };
    return out;
}
_tuple_int_int_string os_socketpair_raw(void) {
    _tuple_int_int_string out = { -1, -1, "os.socketpair unavailable" };
    return out;
}
_tuple_int_string os_wait_pid_raw(int pid) {
    (void)pid;
    _tuple_int_string out = { -1, "os.wait_pid unavailable" };
    return out;
}
_tuple_int_string os_spawn_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_string out = { -1, "os.spawn unavailable" };
    return out;
}
_tuple_int_string os_spawn_to_raw(const char* p, void* a, void* e, const char* path) {
    (void)p; (void)a; (void)e; (void)path;
    _tuple_int_string out = { -1, "os.spawn unavailable" };
    return out;
}
typedef struct { void* _0; const char* _1; } _tuple_ptr_string_tree;
_tuple_ptr_string_tree os_spawn_tree_to_raw(const char* p, void* a, void* e, const char* path, int g) {
    (void)p; (void)a; (void)e; (void)path; (void)g;
    _tuple_ptr_string_tree out = { NULL, "os.spawn_tree_to unavailable" };
    return out;
}
int os_tree_token_raw(void* t) { (void)t; return -1; }
int os_tree_pid_raw(void* t) { (void)t; return -1; }
int os_tree_kill_raw(void* t) { (void)t; return -1; }
void os_tree_free_raw(void* t) { (void)t; }
_tuple_int_string os_wait_raw(int token) {
    (void)token;
    _tuple_int_string out = { -1, "os.wait unavailable" };
    return out;
}
_tuple_int_int_string os_wait_any_raw(void* token_list) {
    (void)token_list;
    _tuple_int_int_string out = { -1, -1, "os.wait_any unavailable" };
    return out;
}
typedef struct { int _0; int _1; int _2; const char* _3; } _tuple_int_int_int_string;
_tuple_int_int_int_string os_wait_any_timeout_raw(void* token_list, int secs) {
    (void)token_list; (void)secs;
    _tuple_int_int_int_string out = { -1, -1, 0, "os.wait_any_timeout unavailable" };
    return out;
}
_tuple_int_int_int_string os_run_stdio_raw(const char* p, void* a, void* e, int stderr_to,
                                           const char* stderr_path, const char* cwd) {
    (void)p; (void)a; (void)e; (void)stderr_to; (void)stderr_path; (void)cwd;
    _tuple_int_int_int_string out = { -1, -1, -1, "os.run_stdio unavailable" };
    return out;
}
int os_kill_raw(int pid, int sig) { (void)pid; (void)sig; return -1; }
_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    (void)pid; (void)secs;
    _tuple_int_int_string out = { -1, 0, "os.wait_pid_timeout unavailable" };
    return out;
}
_tuple_int_string os_run_supervised_raw(const char* p, void* a, void* e,
                                        int g, int f, int t, int r) {
    (void)p; (void)a; (void)e; (void)g; (void)f; (void)t; (void)r;
    _tuple_int_string out = { -1, "os.run_supervised unavailable" };
    return out;
}
typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;
_tuple_string_string_int_string os_run_full_raw(const char* p, void* a, void* e,
                                                const char* in, int in_len) {
    (void)p; (void)a; (void)e; (void)in; (void)in_len;
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "os.run_full unavailable" };
    return out;
}
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_pipe_drain_and_wait unavailable" };
    return out;
}
/* ipc_parent_channel_raw — implemented in std/ipc/aether_ipc.c
 * (its no-FS stub returns -1 there). */
char* os_now_utc_iso8601_raw(void) { return NULL; }
char* os_now_local_iso8601_raw(void) { return NULL; }
void os_now_local_fill_raw(void* out) { (void)out; }
char* os_platform_raw(void) { return NULL; }
char* os_arch_raw(void) { return NULL; }
char* os_temp_dir_raw(void) { return NULL; }
int os_cpu_count_raw(void) { return 1; }
int os_getpid_raw(void) { return 0; }
int os_user_id_raw(void) { return -1; }
int64_t os_wall_seconds_raw(void) { return 0; }
int os_wall_micros_raw(void) { return 0; }
int64_t os_now_monotonic_ms_raw(void) { return 0; }
int64_t os_now_monotonic_ns_raw(void) { return 0; }
int64_t os_now_unix_ms_raw(void) { return 0; }
#elif !AETHER_HAS_PROCESS

/* Filesystem yes, processes no — the WASI shape. Everything the process
 * surface would provide is stubbed here; the filesystem, environment and
 * clock functions fall through to their real implementations below.
 *
 * Return values mirror the !AETHER_HAS_FILESYSTEM stubs above so a caller
 * sees the same "unavailable" shape on either platform. */
int os_system(const char* c) { (void)c; return -1; }
char* os_exec_raw(const char* c) { (void)c; return NULL; }
int os_execv(const char* p, void* a) { (void)p; (void)a; return -1; }
char* os_which(const char* n) { (void)n; return NULL; }
int os_getpid_raw(void) { return 0; }
int os_user_id_raw(void) { return -1; }
int os_kill_raw(int pid, int sig) { (void)pid; (void)sig; return -1; }
void* os_environ_raw(void) { return NULL; }   /* the empty *StringSeq */
typedef struct { void* _0; const char* _1; } _tuple_ptr_string_os;
_tuple_ptr_string_os os_signal_watch_raw(const int* s, int n) {
    (void)s; (void)n; _tuple_ptr_string_os out = { NULL, "os.signal_watch unavailable" }; return out;
}
int os_signal_received_raw(void* w) { (void)w; return 0; }
void os_signal_unwatch_raw(void* w) { (void)w; }

typedef struct { void* _0; const char* _1; } _tuple_ptr_string_tree;
_tuple_ptr_string_tree os_spawn_tree_to_raw(const char* p, void* a, void* e, const char* path, int g) {
    (void)p; (void)a; (void)e; (void)path; (void)g;
    _tuple_ptr_string_tree out = { NULL, "os.spawn_tree_to unavailable" };
    return out;
}
int os_tree_token_raw(void* t) { (void)t; return -1; }
int os_tree_pid_raw(void* t) { (void)t; return -1; }
int os_tree_kill_raw(void* t) { (void)t; return -1; }
void os_tree_free_raw(void* t) { (void)t; }

#define AETHER_OS_PROCESS_STUBBED 1

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Forward declarations for the Aether collections API. aether_os.c sits
// below std/collections in the link order, so we can't include the header
// here without a dependency cycle; the prototypes match
// std/collections/aether_collections.h.
extern int list_size(void* list);
extern void* list_get_raw(void* list, int index);

// String-ABI accessor: list_get_raw returns either an AetherString*
// (heap-built strings — concat, substring, interp, etc.) or a raw
// const char* (string literals, args_get). aether_string_data dispatches
// on the magic header and returns a plain C-string in both cases. Argv
// / envp arrays MUST go through this — the execve(2) family expects
// const char*, and a blind (char*)cast of a magic AetherString* yields
// the struct header bytes, not the payload (issue #688).
extern const char* aether_string_data(const void* s);

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>  /* fstat: which descriptors are the standard streams */
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>  /* fcntl, F_GETFD — used by ipc_parent_channel_raw */
#include <signal.h> /* kill, sigaction — process-supervision primitives */
#include <time.h>   /* nanosleep, struct timespec — bounded waits */
#include <poll.h>   /* poll — deadlock-free 3-way stdio pump in os_run_full_raw */
#else
#include <windows.h>
#include <wchar.h>
#include <process.h>  /* _getpid */
#include <io.h>       /* _open_osfhandle: os.run_stdio's CRT descriptors */
#include <fcntl.h>    /* _O_BINARY, _O_RDONLY, _O_WRONLY */
#endif

// libaether.a list operations — declared extern so this file doesn't have
// to include the collections header (which would create a build cycle).
extern int   list_size(void* list);
extern void* list_get_raw(void* list, int index);
extern const char* aether_string_data(const void* s);

#ifdef _WIN32
// Forward declaration — implementation at the bottom of the file.
// os_execv needs to call into this before the Windows backend block.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture);
#endif

#ifndef _WIN32
/* #3046 review: os.system and os.exec start their shell as every other
 * child here is started, through aether_fork, and os.exec's pipe is made
 * with aether_pipe_cloexec. system() and popen() fork inside libc, outside
 * the fork lock, so where pipe2 is missing (macOS) their shell could take
 * another thread's pipe between its pipe() and its fcntl() and keep it
 * across exec: that capture then waited for the shell (#2917, #3028). What
 * system() and popen() do is kept: `/bin/sh -c` (with `--`, so a command
 * that starts with `-` is no option, as glibc passes it), the wait-status
 * word, the signal handling and the waits retried on EINTR. */
#define AETHER_OS_SHELL AETHER_SANDBOX_SHELL   /* _PATH_BSHELL, or "/bin/sh": aether_sandbox_match.h */

/* This process's environment: what a child execs with when no env is given
 * (#3201). */
#define aether_environ (aether_spawn_environ())

/* A child's exit status, whatever the host did to SIGCHLD.
 *
 * A process whose SIGCHLD is ignored (SIG_IGN, or SA_NOCLDWAIT) has its
 * children reaped by the kernel as they exit, and waitpid on one fails with
 * ECHILD. That disposition is inherited across exec, so a program the Erlang
 * VM starts has it, and a library loaded into such a host has it too:
 * os.run_capture read the child's whole stdout and then returned
 * (stdout, -1, "waitpid failed"), and os.run returned -1.
 *
 * So the calls here make themselves independent of it, as most libraries
 * that wait for their children do. While any child this file started is
 * not yet reaped, SIGCHLD is at its default (no handler, no auto-reap);
 * the disposition the process had is saved by the first fork and put back
 * when the last such child is reaped. A process whose SIGCHLD is not
 * ignored is never touched. The parts of the deal a caller may see:
 *   - while a child of std.os is outstanding (an os.spawn not yet waited
 *     for included), another child of the process that exits is left for
 *     its parent to reap rather than reaped by the kernel;
 *   - a host that changes SIGCHLD's disposition while one is outstanding
 *     has it replaced by the saved one when the last is reaped.
 * The shell os.system and os.exec start gets the saved disposition back in
 * the child, as system() and popen() would have given it; every other child
 * here starts with SIGCHLD at its default anyway (aether_child_reset_signals).
 *
 * The lock is held across the fork, so a pid is in the table before any
 * thread can reap it. The child does not touch the lock, only the copy of
 * the saved disposition (sigaction is async-signal-safe). */
#include "../../runtime/utils/aether_thread.h"
static pthread_mutex_t aether_chld_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t* aether_chld_pids = NULL;   /* children not yet reaped */
static int aether_chld_count = 0, aether_chld_cap = 0;
static int aether_chld_swapped = 0;      /* SIGCHLD was ignored and is now default */
static struct sigaction aether_chld_saved;

/* Called with the lock held, when the table goes from empty to one. */
static void aether_chld_take(void) {
    struct sigaction cur;
    if (sigaction(SIGCHLD, NULL, &cur) != 0) return;
    if (cur.sa_handler != SIG_IGN && !(cur.sa_flags & SA_NOCLDWAIT)) return;
    struct sigaction dfl = cur;
    if (cur.sa_handler == SIG_IGN) dfl.sa_handler = SIG_DFL;
    dfl.sa_flags &= ~SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &dfl, NULL) == 0) {
        aether_chld_saved = cur;
        aether_chld_swapped = 1;
    }
}

/* Called with the lock held, when the table goes from one to empty. */
static void aether_chld_give_back(void) {
    if (!aether_chld_swapped) return;
    sigaction(SIGCHLD, &aether_chld_saved, NULL);
    aether_chld_swapped = 0;
}

/* aether_fork, for every child this file waits for. In the parent the pid is
 * in the table; in the child the saved SIGCHLD disposition is back. */
static pid_t aether_os_fork(void) {
    pthread_mutex_lock(&aether_chld_lock);
    int room = aether_chld_count < aether_chld_cap;
    if (!room) {
        int ncap = aether_chld_cap ? aether_chld_cap * 2 : 16;
        pid_t* grown = (pid_t*)realloc(aether_chld_pids, (size_t)ncap * sizeof(pid_t));
        if (grown) {
            aether_chld_pids = grown;
            aether_chld_cap = ncap;
            room = 1;
        }
    }
    if (!room) {
        pthread_mutex_unlock(&aether_chld_lock);
        errno = ENOMEM;
        return -1;
    }
    if (aether_chld_count == 0) aether_chld_take();
    pid_t pid = aether_fork();
    if (pid == 0) {
        if (aether_chld_swapped) sigaction(SIGCHLD, &aether_chld_saved, NULL);
        return 0;
    }
    if (pid > 0) {
        aether_chld_pids[aether_chld_count++] = pid;
    } else if (aether_chld_count == 0) {
        aether_chld_give_back();
    }
    pthread_mutex_unlock(&aether_chld_lock);
    return pid;
}

/* waitpid, for every wait in this file: a child it reaps leaves the table. */
static pid_t aether_waitpid(pid_t pid, int* st, int flags) {
    pid_t got = waitpid(pid, st, flags);
    if (got <= 0) return got;
    int saved_errno = errno;
    pthread_mutex_lock(&aether_chld_lock);
    for (int i = 0; i < aether_chld_count; i++) {
        if (aether_chld_pids[i] == got) {
            aether_chld_pids[i] = aether_chld_pids[--aether_chld_count];
            if (aether_chld_count == 0) aether_chld_give_back();
            break;
        }
    }
    pthread_mutex_unlock(&aether_chld_lock);
    errno = saved_errno;
    return got;
}

static int aether_child_fd(int fd, int target);   /* below */

/* A program that cannot start (#3170). exec fails in the forked child, so
 * on its own the parent saw only an exit of 127, the same as a program that
 * ran and exited 127. Every spawner here gives the child a close-on-exec
 * pipe: the child writes its errno there before it exits 127, and an exec
 * that succeeds closes the pipe unwritten, so the parent reads EOF. The
 * parent reads it before anything else and reports the failure in the
 * call's own shape, as Windows does, which looks the program up before any
 * child exists. The child moves its end with aether_child_lift_fd. */

/* In a forked child: move `fd` off 0 to 3 and return where it went. A child
 * installs pipe ends and files on 0, 1 and 2 (and 3 for os.run_pipe), and
 * when the parent has some of its own closed, the descriptors it made can
 * sit on those slots. Installed in place, one would overwrite another still
 * to be installed, and the close of an original would close what had been
 * installed there: os.run_full's child lost its stdout when the parent's
 * was closed, os.spawn_proc_to's its output file when the parent's stdin
 * was (#3199). So every descriptor a child installs is moved first, and so
 * is the exec-error pipe's end (#3170), which the wiring would overwrite.
 *
 * The copy closes on exec. The original is closed: a slot from 0 to 2 is
 * one the parent had closed, and aether_child_keep_stdio clears its
 * close-on-exec flag, so left open it would outlive the exec (the
 * exec-error pipe's end would make the parent wait for the program to
 * end). */
static int aether_child_lift_fd(int fd) {
    if (fd > 3) return fd;
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, 4);
    close(fd);
    return moved;
}

/* In the child: report errno on `errfd` and exit 127. */
static void aether_child_exec_failed(int errfd) {
    int e = errno;
    if (errfd >= 0) {
        ssize_t w;
        do { w = write(errfd, &e, sizeof e); } while (w < 0 && errno == EINTR);
    }
    _exit(127);
}

/* In the parent: the errno the child reported, 0 when its exec succeeded. */
static int aether_exec_errno(int errpipe[2]) {
    close(errpipe[1]);
    int e = 0;
    ssize_t n;
    do { n = read(errpipe[0], &e, sizeof e); } while (n < 0 && errno == EINTR);
    close(errpipe[0]);
    if (n != (ssize_t)sizeof e) return 0;
    return e ? e : EIO;
}

/* The err a failed start reads as, the words Windows uses where it has them. */
static const char* aether_exec_failure(int e) {
    switch (e) {
        case ENOENT: case ENOTDIR: return "program not found";
        case EACCES: case EPERM:   return "permission denied";
        case ENOEXEC:              return "not an executable";
        default:                   return "exec failed";
    }
}

/* "<what>: <strerror(err)>" in a per-thread buffer, valid until this
 * thread's next call. strerror_r, not strerror: the runtime runs actor,
 * worker and HTTP threads, and strerror's buffer is shared. */
static const char* aether_os_strerror_msg(const char* what, int err) {
    static __thread char msg[256];
    char buf[128];
    const char* reason = buf;
    buf[0] = '\0';
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    reason = strerror_r(err, buf, sizeof buf);
#else
    if (strerror_r(err, buf, sizeof buf) != 0) snprintf(buf, sizeof buf, "error %d", err);
#endif
    snprintf(msg, sizeof msg, "%s: %s", what, reason);
    return msg;
}

/* Reap a child whose exec failed. */
static void aether_reap_failed(pid_t pid) {
    int st = 0;
    while (aether_waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

/* Wait for `pid`, retried on EINTR; its wait-status word, or -1. */
static int aether_os_shell_wait(pid_t pid) {
    int status = 0;
    while (aether_waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}

/* The argv of `sh -c CMD`, in argv[4]. `sh -c -- CMD` is how POSIX says
 * "CMD is the command string even if it starts with '-'", and dash and bash
 * take it, but FreeBSD's sh takes the `--` itself as the command string and
 * fails every command with "--: not found" (rc 127). A command string that
 * starts with '-' or '+' gets a leading space instead, which every sh reads
 * as the same command and none as an option. Returns the copy that space
 * needed, for the caller to free once the child is forked (NULL when none
 * was), or sets *failed when it could not be made. */
static char* aether_sh_argv(char* argv[4], const char* cmd, int* failed) {
    char* owned = NULL;
    *failed = 0;
    if (cmd[0] == '-' || cmd[0] == '+') {
        size_t n = strlen(cmd);
        owned = (char*)malloc(n + 2);
        if (!owned) { *failed = 1; return NULL; }
        owned[0] = ' ';
        memcpy(owned + 1, cmd, n + 1);
    }
    argv[0] = (char*)"sh";
    argv[1] = (char*)"-c";
    argv[2] = owned ? owned : (char*)cmd;
    argv[3] = NULL;
    return owned;
}

/* popen(cmd, "r"): the shell's stdout is a close-on-exec pipe's write
 * end, made its stdout as every capture here makes it; its stdin, stderr,
 * signal dispositions and mask are this process's, as popen leaves them.
 * NULL where popen fails (no pipe, no fork) and where the shell could not
 * start, which _popen gives on Windows too (#3170). */
static FILE* aether_os_shell_open(const char* cmd, pid_t* out_pid) {
    int pipefd[2];
    if (aether_pipe_cloexec(pipefd) != 0) return NULL;
    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    char* sh_argv[4];
    int sh_failed = 0;
    char* sh_owned = aether_sh_argv(sh_argv, cmd, &sh_failed);
    if (sh_failed) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        return NULL;
    }
    /* environ is read in the child, after the fork: see aether_child_exec. */
    pid_t pid = aether_os_fork();
    if (pid != 0) free(sh_owned);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        return NULL;
    }
    if (pid == 0) {
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int out_fd = aether_child_lift_fd(pipefd[1]);
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        close(out_fd);
        execve(AETHER_OS_SHELL, sh_argv, aether_environ);
        aether_child_exec_failed(ef);
    }
    close(pipefd[1]);
    if (aether_exec_errno(errp)) {
        close(pipefd[0]);
        (void)aether_os_shell_wait(pid);
        return NULL;
    }
    FILE* f = fdopen(pipefd[0], "r");
    if (!f) {
        close(pipefd[0]);
        (void)aether_os_shell_wait(pid);
        return NULL;
    }
    *out_pid = pid;
    return f;
}

/* pclose(): close the stream, then reap the shell. */
static void aether_os_shell_close(FILE* f, pid_t pid) {
    fclose(f);
    (void)aether_os_shell_wait(pid);
}

#if AETHER_HAS_SHELL
#include "../../runtime/utils/aether_thread.h"
/* system(), as POSIX has it and glibc counts it for threads: while any
 * os.system waits, SIGINT and SIGQUIT are ignored in this process (saved
 * by the first caller, restored by the last) and SIGCHLD is blocked in
 * the waiting thread. The shell gets SIGINT and SIGQUIT back as they were
 * (the default, unless they were ignored before) and the caller's mask. */
static pthread_mutex_t aether_system_lock = PTHREAD_MUTEX_INITIALIZER;
static int aether_system_waiting = 0;
static struct sigaction aether_system_intr, aether_system_quit;

static int aether_os_system_posix(const char* cmd) {
    /* A shell that could not start is -1, as Windows's system() gives it,
     * not the 127 POSIX system() reports for it (#3170). */
    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) return -1;
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    pthread_mutex_lock(&aether_system_lock);
    if (aether_system_waiting++ == 0) {
        sigaction(SIGINT, &ign, &aether_system_intr);
        sigaction(SIGQUIT, &ign, &aether_system_quit);
    }
    int intr_ignored = aether_system_intr.sa_handler == SIG_IGN;
    int quit_ignored = aether_system_quit.sa_handler == SIG_IGN;
    pthread_mutex_unlock(&aether_system_lock);

    sigset_t chld, omask;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, &omask);

    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    char* sh_argv[4];
    int sh_failed = 0;
    char* sh_owned = aether_sh_argv(sh_argv, cmd, &sh_failed);
    /* environ is read in the child, after the fork: see aether_child_exec. */
    pid_t pid = sh_failed ? -1 : aether_os_fork();
    if (pid != 0) free(sh_owned);
    if (pid == 0) {
        if (!intr_ignored) sigaction(SIGINT, &dfl, NULL);
        if (!quit_ignored) sigaction(SIGQUIT, &dfl, NULL);
        sigprocmask(SIG_SETMASK, &omask, NULL);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        execve(AETHER_OS_SHELL, sh_argv, aether_environ);
        aether_child_exec_failed(ef);
    }
    int status = -1;
    if (pid > 0) {
        int exec_err = aether_exec_errno(errp);
        status = aether_os_shell_wait(pid);
        if (exec_err) status = -1;
    } else {
        close(errp[0]);
        close(errp[1]);
    }

    pthread_mutex_lock(&aether_system_lock);
    if (--aether_system_waiting == 0) {
        sigaction(SIGINT, &aether_system_intr, NULL);
        sigaction(SIGQUIT, &aether_system_quit, NULL);
    }
    pthread_mutex_unlock(&aether_system_lock);
    sigprocmask(SIG_SETMASK, &omask, NULL);
    return status;
}
#endif
#endif

/* A command string runs through the shell, which runs all of it, not only
 * the part an exec grant matched ("echo *" matches "echo hi; rm -rf ~"):
 * the shell needs an exec grant of its own (#3735), as it does under the
 * LD_PRELOAD library. aether_sandbox_check_shell checks both. */
int os_system(const char* cmd) {
    if (!cmd) return -1;
    if (!aether_sandbox_check_shell(cmd)) return -1;
#if !AETHER_HAS_SHELL
    /* iOS/tvOS/watchOS: no shell to exec into, and system() is marked
     * __API_UNAVAILABLE there, so even naming it fails the compile. Report
     * the same -1 the sandbox-denied and spawn-failure paths use rather
     * than dropping the whole module from the build. os.run / os.run_capture
     * (fork+exec, argv-based) are unaffected and remain the portable way to
     * spawn a child; system() was always the legacy shell-out. */
    return -1;
#else
#ifdef _WIN32
    int status = system(cmd);
    if (status == -1) return -1;
    /* Windows's system() already returns the exit code directly. */
    return status;
#else
    /* system()'s wait-status word, through aether_fork (#3046 review). */
    int status = aether_os_system_posix(cmd);
    if (status == -1) return -1;
    /* POSIX system() returns a wait-status word: low byte = signal,
     * high byte = exit code on normal exit. Aether's contract (per
     * docs/stdlib-reference.md) is "returns exit code" — match the
     * shape of os_run's exit-code handling (line ~493): normal
     * termination → WEXITSTATUS, signal → 128 + signum (shell
     * convention), abnormal → -1. Pre-fix, callers had to `>> 8`
     * by hand on POSIX and not on Windows; that's a cross-platform
     * inconsistency the contract didn't promise. */
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
#endif /* AETHER_HAS_SHELL */
}

char* os_exec_raw(const char* cmd) {
    if (!cmd) return NULL;
    if (!aether_sandbox_check_shell(cmd)) return NULL;   /* the shell too, #3735 */

#ifdef _WIN32
    FILE* pipe = _popen(cmd, "r");
#else
    pid_t pid = -1;
    FILE* pipe = aether_os_shell_open(cmd, &pid);   /* popen(), #3046 review */
#endif
    if (!pipe) return NULL;

    size_t capacity = 1024;
    size_t len = 0;
    /* #462: gate the command-output buffer through the capability
     * allocator so a sandboxed plugin can't inflate it without bound.
     * The success path adopts it into the counted string it returns. */
    char* result = (char*)aether_caps_malloc(capacity);
    if (!result) {
#ifdef _WIN32
        _pclose(pipe);
#else
        aether_os_shell_close(pipe, pid);
#endif
        return NULL;
    }

    // fgets reads up to sizeof(buffer)-1 per call, so a single chunk
    // won't need more than one doubling — but use `while` anyway to
    // stay safe against future buffer-size changes.
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t chunk = strlen(buffer);
        if (len + chunk + 1 > capacity) {
            size_t new_capacity = capacity;
            while (new_capacity < len + chunk + 1) new_capacity *= 2;
            char* new_result = (char*)aether_caps_realloc(result, capacity, new_capacity);
            if (!new_result) {
                aether_caps_free(result, capacity);
#ifdef _WIN32
                _pclose(pipe);
#else
                aether_os_shell_close(pipe, pid);
#endif
                return NULL;
            }
            result = new_result;
            capacity = new_capacity;
        }
        memcpy(result + len, buffer, chunk);
        len += chunk;
    }

    result[len] = '\0';

#ifdef _WIN32
    _pclose(pipe);
#else
    aether_os_shell_close(pipe, pid);
#endif
    return (char*)string_adopt_caps_buffer(result, len, capacity);
}

char* os_getenv(const char* name) {
    if (!name) return NULL;
    if (!aether_sandbox_check("env", name)) return NULL;
    char* val = getenv(name);
    if (!val) return NULL;
    /* #462: a counted copy, allocated through the cap (a plugin can't
     * read an unbounded environment value past the limit). */
    return (char*)string_new_with_length(val, strlen(val));
}

/* Format the current UTC time as an ISO-8601 timestamp string:
 * "YYYY-MM-DDThh:mm:ssZ" (20 chars + NUL). Returns a counted
 * string. Uses gmtime_r for thread safety. On any clock / format
 * failure returns an empty string — callers that care about the
 * distinction should check for "" vs a valid timestamp shape.
 *
 * Sub-second precision, timezone offsets, and format flags are
 * out of scope for v1 — keep it to the one shape callers wanting
 * a "timestamp this event happened" field reach for. Additive
 * variants (`now_utc_iso8601_ms`, `format_time`) can land without
 * breaking this one. */
char* os_now_utc_iso8601_raw(void) {
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    if (gmtime_s(&tm_buf, &now) != 0) return (char*)string_empty();
#else
    if (!gmtime_r(&now, &tm_buf)) return (char*)string_empty();
#endif
    char buf[32];
    /* "YYYY-MM-DDThh:mm:ssZ" — 20 bytes + NUL, fits buf easily.
     * Numeric-only conversions, so no locale sensitivity. Do NOT add
     * %b/%a/%p — this is an ISO-8601 wire format, not human-facing text. */
    if (strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0) {
        return (char*)string_empty();
    }
    return (char*)string_new(buf);
}

/* ---- Local wall-clock time ------------------------------------------
 *
 * Aether's `now_local()` allocates a `LocalTime` struct and hands it
 * to `os_now_local_fill_raw` to populate. ONE `gettimeofday` +
 * `localtime_r` call per Aether call — fields are atomic with each
 * other.
 *
 * The companion `os_now_local_iso8601_raw` formats the same data into
 * an RFC-3339 / ISO-8601 string with explicit timezone offset
 * (`"YYYY-MM-DDThh:mm:ss±HH:MM"`, or `"...Z"` when offset == 0).
 *
 * Timezone offset: tm_gmtoff is GNU/BSD; on Windows the offset is
 * derived via `_get_timezone` (note: opposite sign from gmtoff —
 * _timezone is "seconds WEST of UTC", gmtoff is "seconds EAST"). DST
 * adjustment on Windows: `localtime_s` writes `tm_isdst`; we add
 * 3600 seconds when DST is active.
 *
 * The C-side AetherOsLocalTime struct mirrors the Aether-side
 * `struct LocalTime` in std/os/module.ae exactly: eight `int`
 * fields, no padding. Layout is asserted at compile time. */
typedef struct AetherOsLocalTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int nanos;
    int tz_offset_minutes;
} AetherOsLocalTime;

/* Layout sanity — every consumer mallocs sizeof(LocalTime) on the
 * Aether side; this asserts the C struct matches that assumption. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(AetherOsLocalTime) == 8 * sizeof(int),
               "AetherOsLocalTime must be 8 packed ints to match Aether's struct LocalTime");
#endif

#ifdef _WIN32
static int aether_os_local_tz_offset_seconds(const struct tm* tm_buf) {
    long tz_seconds_west = 0;
    _get_timezone(&tz_seconds_west);
    int offset = (int)(-tz_seconds_west);  /* convert "west" to "east" */
    if (tm_buf && tm_buf->tm_isdst > 0) offset += 3600;
    return offset;
}
#endif

/* Forward decl — defined further down next to the wall-clock raw
 * getters. We call it from os_now_local_fill_raw to read seconds +
 * usec in one shot. */
static void aether_os_wall_now(int64_t *psec, int *pusec);

/* Fill an Aether-allocated LocalTime struct with the current local
 * wall-clock time. The Aether wrapper allocates sizeof(LocalTime),
 * passes its ptr here. The void* signature matches the public header
 * (keeps AetherOsLocalTime an internal detail); we cast back inside.
 * On failure (clock read / localtime conversion) the struct is
 * zeroed; the wrapper surfaces year == 0 as the failure signal. */
void os_now_local_fill_raw(void* out_ptr) {
    if (!out_ptr) return;
    AetherOsLocalTime* out = (AetherOsLocalTime*)out_ptr;
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    time_t t = (time_t)sec;
    struct tm tm_buf;
#ifdef _WIN32
    if (localtime_s(&tm_buf, &t) != 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    int tz_seconds = aether_os_local_tz_offset_seconds(&tm_buf);
#else
    if (!localtime_r(&t, &tm_buf)) {
        memset(out, 0, sizeof *out);
        return;
    }
    int tz_seconds = (int)tm_buf.tm_gmtoff;
#endif
    out->year              = tm_buf.tm_year + 1900;
    out->month             = tm_buf.tm_mon + 1;
    out->day               = tm_buf.tm_mday;
    out->hour              = tm_buf.tm_hour;
    out->minute            = tm_buf.tm_min;
    out->second            = tm_buf.tm_sec;
    out->nanos             = usec * 1000;
    out->tz_offset_minutes = tz_seconds / 60;
}

/* RFC-3339 / ISO-8601 with explicit timezone offset:
 *   "YYYY-MM-DDThh:mm:ss+HH:MM"   (positive offset)
 *   "YYYY-MM-DDThh:mm:ss-HH:MM"   (negative offset)
 *   "YYYY-MM-DDThh:mm:ssZ"        (offset == 0, UTC-equivalent)
 * 25 chars + NUL maximum. Returns a counted string; "" on failure. */
char* os_now_local_iso8601_raw(void) {
    AetherOsLocalTime lt;
    os_now_local_fill_raw(&lt);
    if (lt.year == 0) return (char*)string_empty();  /* localtime failed */
    /* 64 bytes is generous — the longest legitimate output is 25 chars
     * + NUL, and oversizing here avoids gcc's -Wformat-truncation
     * cautious worst-case analysis (it can't prove the int args fit
     * in 4 / 2 digits respectively). */
    char buf[64];
    if (lt.tz_offset_minutes == 0) {
        snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second);
    } else {
        int abs_min = lt.tz_offset_minutes < 0
                          ? -lt.tz_offset_minutes
                          : lt.tz_offset_minutes;
        snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                 lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second,
                 lt.tz_offset_minutes < 0 ? '-' : '+',
                 abs_min / 60, abs_min % 60);
    }
    return (char*)string_new(buf);
}

/* Platform name as a flat lowercase string, matching the Go/Rust
 * convention (Go `runtime.GOOS`, Rust `std::env::consts::OS`):
 *
 *   "linux"      Linux kernel (any libc)
 *   "darwin"     macOS / iOS / any Darwin kernel
 *   "windows"    Windows (any toolchain: MSVC, MinGW, MSYS2)
 *   "freebsd"    FreeBSD
 *   "openbsd"    OpenBSD
 *   "netbsd"     NetBSD
 *   "dragonfly"  DragonFly BSD
 *   "solaris"    Solaris / illumos
 *   "wasm"       WebAssembly (Emscripten or wasi)
 *   "unknown"    None of the above matched at compile time
 *
 * Picked at compile time via the toolchain's standard predefined
 * macros (__APPLE__, __linux__, _WIN32, etc.). The returned string
 * is a counted copy the caller owns. */
/* The directory for scratch files.
 *
 * Every caller needing one used to hand-roll TEMP -> TMPDIR -> "/tmp"
 * (23 sites in the tree, #1702), and the ones that skipped it and
 * hardcoded "/tmp" passed on POSIX and on an MSYS2 shell — where /tmp
 * resolves — while failing on the Windows CI runners, which install
 * MSYS2 somewhere else entirely. That is the worst shape for a bug:
 * the local check says yes.
 *
 * Windows uses GetTempPathW, which already performs the documented
 * TMP -> TEMP -> USERPROFILE -> Windows-directory cascade, so there is
 * no hand-rolled precedence to get wrong. It returns a path WITH a
 * trailing separator; we strip it so callers can always write
 * "${dir}/name" without a special case.
 *
 * POSIX reads TMPDIR (which POSIX itself specifies) and falls back to
 * "/tmp". An empty TMPDIR is treated as unset rather than as the
 * current directory.
 *
 * Never returns NULL or an empty string: a caller that cannot write a
 * scratch file has a real problem, but it should not be a null deref.
 */
char* os_temp_dir_raw(void) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wbuf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, wbuf);
    if (n == 0 || n > MAX_PATH) return (char*)string_new("C:\\Windows\\Temp");

    /* Strip the trailing separator GetTempPathW always appends, but
     * never the one in a drive root ("C:\\"), which is part of the
     * name rather than a separator. */
    while (n > 0 && (wbuf[n - 1] == L'\\' || wbuf[n - 1] == L'/')) {
        if (n == 3 && wbuf[1] == L':') break;
        wbuf[--n] = 0;
    }

    /* `need` counts the NUL, which string_alloc_inline adds itself. */
    int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return (char*)string_new("C:\\Windows\\Temp");
    AetherString* out = string_alloc_inline((size_t)need - 1);
    if (!out) return (char*)string_new("C:\\Windows\\Temp");
    if (WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out->data, need, NULL, NULL) <= 0) {
        string_release(out);
        return (char*)string_new("C:\\Windows\\Temp");
    }
    return (char*)out;
#else
    const char* t = getenv("TMPDIR");
    if (t && *t) {
        size_t len = strlen(t);
        /* Strip a trailing slash so the result never doubles one up,
         * except for "/" itself. */
        while (len > 1 && t[len - 1] == '/') len--;
        return (char*)string_new_with_length(t, len);
    }
    return (char*)string_new("/tmp");
#endif
}

/* Online logical CPU count (the `nproc` value), always >= 1. */
int os_cpu_count_raw(void) {
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long n = (long)si.dwNumberOfProcessors;
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

char* os_platform_raw(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char* name = "windows";
#elif defined(__APPLE__)
    const char* name = "darwin";
#elif defined(__linux__)
    const char* name = "linux";
#elif defined(__FreeBSD__)
    const char* name = "freebsd";
#elif defined(__OpenBSD__)
    const char* name = "openbsd";
#elif defined(__NetBSD__)
    const char* name = "netbsd";
#elif defined(__DragonFly__)
    const char* name = "dragonfly";
#elif defined(__sun) && (defined(__SVR4) || defined(__svr4__))
    const char* name = "solaris";
#elif defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(__wasm__)
    const char* name = "wasm";
#else
    const char* name = "unknown";
#endif
    return (char*)string_new(name);
}

/* CPU architecture of the build target, normalized to the ecosystem vocabulary
 * the release assets, aeb, and aeo already use: "x86_64" (never "amd64"),
 * "arm64" (never "aarch64"), "i386", "arm", "riscv64", "ppc64le", "s390x",
 * "wasm32"/"wasm64", else "unknown". The companion to os_platform_raw() —
 * os.platform() + os.arch() reconstruct the "<os>-<arch>" release-asset token.
 *
 * Resolved at COMPILE time from the compiler's arch predefines, deliberately
 * NOT `uname -m`. Two reasons this is better than the shell-out its callers
 * (aeb bldr._host_arch, selaenium's engine-fetch) do today: (1) no process
 * spawn, so it works with no shell and inside a sandbox; (2) it reports the
 * TARGET arch under cross-compilation — `zig cc -target aarch64-linux` defines
 * __aarch64__, so a cross build picks the right asset, whereas `uname -m` on the
 * build host would pick the host's. The macro set covers GCC/Clang/zig
 * (__x86_64__ etc.) and MSVC (_M_*). */
char* os_arch_raw(void) {
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
    const char* name = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    const char* name = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* name = "i386";
#elif defined(__arm__) || defined(_M_ARM)
    const char* name = "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
    const char* name = "riscv64";
#elif defined(__loongarch64) || (defined(__loongarch__) && defined(__loongarch_lp64))
    const char* name = "loongarch64";
#elif defined(__powerpc64__) && (defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN))
    const char* name = "ppc64le";
#elif defined(__s390x__)
    const char* name = "s390x";
#elif defined(__wasm64__)
    const char* name = "wasm64";
#elif defined(__wasm__) || defined(__wasm32__) || defined(__EMSCRIPTEN__) || defined(__wasi__)
    const char* name = "wasm32";
#else
    const char* name = "unknown";
#endif
    return (char*)string_new(name);
}

/* Process identifier for the current process. Useful for tmpfile names
 * (`/tmp/myprog.${pid}.tmp`), per-process locks, log prefixes, and
 * stable tagging across forked children. POSIX uses `getpid(2)`;
 * Windows uses `_getpid()` from <process.h>. Returns an int across
 * both platforms — Windows PIDs fit in 32 bits even though the
 * GetCurrentProcessId() type is DWORD. Sandbox-free: this is a
 * pure-information call, not an action. */
int os_getpid_raw(void) {
#ifdef _WIN32
    return (int)_getpid();
#else
    return (int)getpid();
#endif
}

/* #1368: effective user id of the calling process. POSIX geteuid(). Windows
 * has no numeric uid model, so it returns -1 (callers treat <0 as
 * "unavailable" — e.g. building /run/user/<uid>/ only makes sense on POSIX). */
int os_user_id_raw(void) {
#ifdef _WIN32
    return -1;
#else
    return (int)geteuid();
#endif
}

/* Wall-clock time split into whole seconds since the Unix epoch and
 * the sub-second microsecond fraction (0..999999).  A single
 * gettimeofday/filetime read fills a static cache so the two raw
 * accessors are consistent when called back-to-back. struct timeval /
 * FILETIME stay entirely C-side — Aether only ever sees int64/int. */
#ifdef _WIN32
#include <windows.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* FILETIME: 100-ns ticks since 1601-01-01. Convert to Unix epoch. */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;          /* 1601->1970 offset, 100ns units */
    *psec  = (int64_t)(t / 10000000ULL);
    *pusec = (int)((t / 10ULL) % 1000000ULL);
}
#else
#include <sys/time.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) { *psec = 0; *pusec = 0; return; }
    *psec  = (int64_t)tv.tv_sec;
    *pusec = (int)tv.tv_usec;
}
#endif

int64_t os_wall_seconds_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return sec;
}

int os_wall_micros_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return usec;
}

/* Monotonic clock primitives. Wall-clock time (above) is NTP-jumpable
 * and tied to UTC; useless for animation tick loops, frame-time
 * budgets, or measuring elapsed time across a function call. These
 * use the per-platform monotonic source whose value-domain is opaque
 * (epoch is boot / process-start / arbitrary) but whose *deltas*
 * monotonically track real elapsed time.
 *
 * IMPORTANT: return type is int64_t, NOT C `long`. Aether's `long`
 * lowers to int64_t on every platform, but C `long` is 32-bit on
 * Windows (LLP64). Returning C `long` would truncate the upper 32
 * bits into the Aether 8-byte slot on Windows — the bug PR #562
 * caught and fixed for string_to_long_raw. Same hazard, same fix.
 *
 *   ms — milliseconds since boot/process-start. Wraparound at ~292
 *        million years from epoch; not a real concern.
 *   ns — nanoseconds. Useful for sub-ms timing (animation t-curves,
 *        microbenchmarks). Wraparound at ~292 years; also fine.
 */
#ifndef _WIN32
int64_t os_now_monotonic_ms_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t os_now_monotonic_ns_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/* Wall-clock unix epoch ms. CLOCK_REALTIME because that's what every
 * other timestamp-consuming system on the network is using; the
 * NTP-step caveat is in the header. POSIX guarantees CLOCK_REALTIME;
 * the fallback to time(2) is for the (vanishingly rare) systems
 * where clock_gettime is broken but time(2) works. */
int64_t os_now_unix_ms_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    }
    return (int64_t)time(NULL) * 1000;
}
#else
/* Windows: QueryPerformanceCounter gives a high-resolution monotonic
 * counter; QueryPerformanceFrequency gives its tick rate. Frequency
 * is invariant across the OS lifetime (Win7+), so we cache it on first
 * call. Both functions are documented to always succeed on those
 * versions, so we treat a 0 frequency as the only failure mode. */
/* The frequency, 0 until a call has read it: an atomic, as any thread may
 * read the clock first (#3680). The value is all it publishes, and every
 * thread that reads it computes the same, so two first calls at once only
 * both ask the system. */
static _Atomic int64_t qpc_freq_cache = 0;

static int64_t qpc_freq_ticks_per_sec(void) {
    int64_t freq = qpc_freq_cache;
    if (freq == 0) {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f)) return 0;
        freq = (int64_t)f.QuadPart;
        qpc_freq_cache = freq;
    }
    return freq;
}

int64_t os_now_monotonic_ms_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Compute ms as (ticks * 1000) / freq. Doing the multiply first
     * preserves sub-second precision; on a 10 MHz counter this loses
     * resolution at 2^53 ticks / 10^4 ≈ 28e8 seconds (90 years) which
     * is fine. For very-long-lived processes the ns variant has more
     * headroom because freq divides cleanly into 10^9 on x86. */
    return ((int64_t)counter.QuadPart * 1000) / freq;
}

int64_t os_now_monotonic_ns_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Split-multiply to avoid overflowing int64 on long uptimes:
     * ticks*1e9/freq could overflow at ~2^53/1e9 ≈ 9e15 ticks (≈ 28
     * years on a 10 MHz counter). The split form: (ticks / freq) * 1e9
     * + ((ticks % freq) * 1e9 / freq) — exact, no overflow. */
    int64_t ticks = (int64_t)counter.QuadPart;
    int64_t whole_sec = ticks / freq;
    int64_t frac_ticks = ticks % freq;
    return whole_sec * 1000000000 + (frac_ticks * 1000000000) / freq;
}

/* GetSystemTimeAsFileTime: 100-ns intervals since 1601-01-01 UTC.
 * Convert to unix-ms by subtracting the 116444736000000000 epoch
 * offset and dividing by 10000. */
int64_t os_now_unix_ms_raw(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t ticks = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000LL) / 10000;
}
#endif

int os_execv(const char* prog, void* argv_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

#ifdef _WIN32
    // Windows has no true exec that replaces the process in place. The
    // closest semantic: spawn the child synchronously, inherit stdio,
    // and exit with the child's exit code so the caller sees the same
    // effective behavior as POSIX execvp (the original process is gone
    // after this call returns successfully — but technically returns
    // via exit, not via in-place replacement).
    int exit_code = 0;
    if (win_launch(prog, argv_list, NULL, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    exit(exit_code);
    return -1;  // unreachable
#else
    // Build a NULL-terminated char* array from the Aether list. We copy
    // pointers only — the list owns the string storage and keeps it
    // alive for the duration of the call (which, on success, is the
    // rest of forever — the process image is replaced in place).
    int n = argv_list ? list_size(argv_list) : 0;

    // Guard against pathological sizes before multiplying for malloc.
    if (n < 0) return -1;
    if ((size_t)n > (SIZE_MAX / sizeof(char*)) - 2) return -1;

    /* #462: internal/transient argv scratch — alloc and free both live
     * in this function (or vanish into a successful execvp). Gate it and
     * free through the cap with the exact byte count so accounting is
     * balanced; the execvp-success path never returns, so its bytes are
     * reclaimed by the process-image replacement, not a leak. */
    size_t argv_bytes = sizeof(char*) * (size_t)(n + 2);
    char** argv = (char**)aether_caps_malloc(argv_bytes);
    if (!argv) return -1;

    // Canonical POSIX behaviour: argv[0] is the program name. If the
    // caller passed an empty list we synthesise argv[0] from `prog` so
    // the callee sees a sensible value; otherwise the caller owns the
    // whole argv and we trust their layout.
    int ai = 0;
    if (n == 0) {
        argv[ai++] = (char*)prog;
    } else {
        for (int i = 0; i < n; i++) {
            void* item = list_get_raw(argv_list, i);
            if (!item) {
                // Bail early rather than pass NULL into execvp's
                // variadic-argv contract, which has undefined behaviour
                // on many implementations.
                aether_caps_free(argv, argv_bytes);
                return -1;
            }
            // aether_string_data unwraps a magic AetherString* to its
            // payload pointer; for raw const char* it returns the input
            // verbatim. Required: a blind (char*)cast of a heap-built
            // string would give execvp the struct header (issue #688).
            argv[ai++] = (char*)aether_string_data(item);
        }
    }
    argv[ai] = NULL;

    // Flush stdio before replacing the process image. execvp destroys
    // the caller's stdio buffers, so anything println'd but not yet
    // flushed would be silently lost. Line-buffered output already on
    // a terminal is safe, but redirected / pipe output is typically
    // fully-buffered — without this flush, pre-exec diagnostics vanish.
    fflush(stdout);
    fflush(stderr);

    // The program is found as os.run finds it (aether_spawn_resolve: a
    // name with a '/' as given, a bare name on PATH) and started with
    // execv. It used to be execvp, which runs a file the kernel refuses
    // with ENOEXEC (no #! line) through /bin/sh by itself: the exec grant
    // matched the file, and the shell then ran whatever the file held with
    // no grant of its own (#3762). The shell fallback is made here instead,
    // as execvp makes it (the system shell, path, argv[1..]), and only when the
    // sandbox lets the shell run (grant_shell, the rule os.run follows,
    // #3755). Refused, the call fails with EACCES. Asked only after the
    // kernel refused the file, so the audit names the shell only for a
    // start that needed it. On success this call never returns; on failure
    // the scratch is freed and -1 returned with errno set.
    char* path = aether_spawn_resolve(prog);
    if (!path) {
        aether_caps_free(argv, argv_bytes);
        return -1;
    }
    execv(path, argv);
    if (errno == ENOEXEC) {
        if (!aether_sandbox_check("exec", AETHER_OS_SHELL)) {
            errno = EACCES;
        } else {
            size_t sh_bytes = sizeof(char*) * (size_t)(ai + 2);
            char** sh_argv = (char**)aether_caps_malloc(sh_bytes);
            if (sh_argv) {
                sh_argv[0] = (char*)AETHER_OS_SHELL;
                sh_argv[1] = path;
                for (int i = 1; i <= ai; i++) sh_argv[i + 1] = argv[i];   /* argv[ai] is the NULL */
                execv(AETHER_OS_SHELL, sh_argv);
                int saved = errno;
                aether_caps_free(sh_argv, sh_bytes);
                errno = saved;
            }
        }
    }
    int saved_errno = errno;
    free(path);
    aether_caps_free(argv, argv_bytes);
    errno = saved_errno;
    return -1;
#endif
}

// Search PATH for an executable. POSIX semantics:
//   1. If `name` contains a '/', it's treated as a path (absolute or
//      relative to cwd). Return it as-is if executable, else NULL.
//   2. Otherwise iterate through colon-separated entries in $PATH (or a
//      sensible default if PATH isn't set), looking for `<dir>/<name>`
//      that's executable. Return the first hit.
//
// Caller owns the returned string.
char* os_which(const char* name) {
    if (!name || !*name) return NULL;
    if (!aether_sandbox_check("env", "PATH")) return NULL;

#ifdef _WIN32
    // Windows: PATH separator is ';', and executable extensions are
    // enumerated via PATHEXT (e.g. ".COM;.EXE;.BAT;.CMD"). Absolute or
    // relative paths with backslashes or drive letters are returned
    // as-is after existence check.
    size_t name_len = strlen(name);

    // Name already has a path component? (slash, backslash, or drive letter)
    int has_path = 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/' || (i == 1 && name[i] == ':')) {
            has_path = 1;
            break;
        }
    }

    // Has an extension already?
    const char* dot = strrchr(name, '.');
    const char* last_sep = NULL;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/') last_sep = name + i;
    }
    int has_ext = dot && (!last_sep || dot > last_sep);

    const char* pathext = getenv("PATHEXT");
    if (!pathext || !*pathext) pathext = ".COM;.EXE;.BAT;.CMD";

    // Helper: try a candidate path; return a counted copy of it if the file exists.
    // Inlined via macro since we use it in both the has-path and PATH-search branches.
    #define WIN_WHICH_TRY(candidate) do { \
        DWORD attrs = GetFileAttributesA(candidate); \
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) { \
            return (char*)string_new(candidate); \
        } \
    } while (0)

    char buf[MAX_PATH];

    if (has_path) {
        if (has_ext) {
            WIN_WHICH_TRY(name);
            return NULL;
        }
        // Try name with each PATHEXT extension.
        const char* p = pathext;
        while (*p) {
            const char* end = strchr(p, ';');
            size_t ext_len = end ? (size_t)(end - p) : strlen(p);
            if (name_len + ext_len + 1 < sizeof(buf)) {
                memcpy(buf, name, name_len);
                memcpy(buf + name_len, p, ext_len);
                buf[name_len + ext_len] = '\0';
                WIN_WHICH_TRY(buf);
            }
            if (!end) break;
            p = end + 1;
        }
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) return NULL;

    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ';');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        if (dirlen == 0 || dirlen + 1 + name_len >= sizeof(buf)) {
            if (!end) break;
            p = end + 1;
            continue;
        }
        memcpy(buf, p, dirlen);
        buf[dirlen] = '\\';
        memcpy(buf + dirlen + 1, name, name_len);
        buf[dirlen + 1 + name_len] = '\0';

        if (has_ext) {
            WIN_WHICH_TRY(buf);
        } else {
            char ext_buf[MAX_PATH];
            const char* ep = pathext;
            while (*ep) {
                const char* eend = strchr(ep, ';');
                size_t elen = eend ? (size_t)(eend - ep) : strlen(ep);
                size_t base_len = dirlen + 1 + name_len;
                if (base_len + elen + 1 < sizeof(ext_buf)) {
                    memcpy(ext_buf, buf, base_len);
                    memcpy(ext_buf + base_len, ep, elen);
                    ext_buf[base_len + elen] = '\0';
                    WIN_WHICH_TRY(ext_buf);
                }
                if (!eend) break;
                ep = eend + 1;
            }
        }

        if (!end) break;
        p = end + 1;
    }
    #undef WIN_WHICH_TRY
    return NULL;
#else
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) return (char*)string_new(name);
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";

    size_t name_len = strlen(name);
    char buf[4096];
    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ':');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        // Empty entry means current directory (POSIX).
        if (dirlen == 0) {
            // Guard: we write "./" (2 bytes) plus name_len+1 bytes
            // (including null terminator) starting at buf+2. The last
            // written byte is at index (2 + name_len), which must be
            // strictly less than sizeof(buf) for validity.
            if (2 + name_len < sizeof(buf)) {
                buf[0] = '.';
                buf[1] = '/';
                memcpy(buf + 2, name, name_len + 1);
                if (access(buf, X_OK) == 0) return (char*)string_new(buf);
            }
        } else if (dirlen + 1 + name_len < sizeof(buf)) {
            memcpy(buf, p, dirlen);
            buf[dirlen] = '/';
            memcpy(buf + dirlen + 1, name, name_len + 1);
            if (access(buf, X_OK) == 0) return (char*)string_new(buf);
        }
        if (!end) break;
        p = end + 1;
    }
    return NULL;
#endif
}


// --- os_run / os_run_capture: argv-based child process launch ---
//
// Both functions take an Aether list as the argv (and optional env)
// rather than a shell-string command line. There is no /bin/sh in the
// loop, so paths-with-spaces, $variables, |, ;, *, and other shell
// metacharacters in argv items are passed verbatim. This eliminates a
// large class of quoting bugs and makes the same Aether code portable
// to platforms without a POSIX shell.
//
// Implementation: POSIX resolves the program on PATH in the parent and
// uses fork + execve + waitpid, with the explicit env or this process's
// (#3201). Windows uses CreateProcessW (see win_launch
// below), building an argv-quoted command line and capturing stdout through
// an inheritable pipe, so the same argv-based contract holds on both.

#ifndef _WIN32

/* Declared at the top of the POSIX block rather than beside the wait family,
 * because the run_capture and run_pipe entry points above that family return
 * its fields through posix_status_to_tuple. */
typedef struct { int _0; const char* _1; } _tuple_int_string;

/* Every waitpid status in this file goes through this one mapper. It used to be
 * copied by hand into three more places, and the copies drifted: two of them
 * had no WIFSIGNALED branch, so the same event -- a child killed by a signal --
 * came back as 137 with no error through os_wait and as -1 with an opaque
 * "child terminated abnormally" through os_run_pipe_drain_and_wait (#2008).
 * Defined further down beside the wait family; declared here so the earlier
 * entry points can share it. */
static _tuple_int_string posix_status_to_tuple(int st);

/* Change the process working directory. POSIX chdir(2). Gated under the
 * "fs" capability bucket: it repositions every subsequent relative path
 * the process touches, so it's a filesystem-scoped action. */
int os_chdir_raw(const char* path) {
    if (!path) return -1;
    if (!aether_sandbox_check("fs", path)) return -1;
    return chdir(path) == 0 ? 0 : -1;
}

/* Current working directory as a fresh counted string (caller owns).
 * Grows the buffer until getcwd(3) stops reporting ERANGE so deep paths
 * past PATH_MAX still resolve. NULL on any other error. */
char* os_getcwd_raw(void) {
    size_t cap = 4096;
    for (;;) {
        /* #462: gate the cwd buffer (a deep path is plugin-influenced).
         * Adopted into the returned string on success; the ERANGE retry
         * frees through the cap before doubling, so the free size matches
         * the live `cap`. */
        char* buf = (char*)aether_caps_malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return (char*)string_adopt_caps_buffer(buf, strlen(buf), cap);
        aether_caps_free(buf, cap);
        if (errno != ERANGE) return NULL;
        if (cap > (size_t)1 << 20) return NULL; /* 1 MiB sanity ceiling */
        cap *= 2;
    }
}

// Build a NULL-terminated argv array from an Aether list. The first
// entry in argv[] is `prog`. Caller must free the returned array (the
// strings inside are pointers into the Aether list and must NOT be
// freed individually). Returns NULL on allocation failure.
static char** build_argv_array(const char* prog, void* argv_list) {
    int n = argv_list ? list_size(argv_list) : 0;
    char** av = (char**)malloc(sizeof(char*) * (size_t)(n + 2));
    if (!av) return NULL;
    av[0] = (char*)prog;
    for (int i = 0; i < n; i++) {
        // Unwrap magic AetherString* to its payload; pass plain char*
        // through unchanged. See aether_os.c header comment + issue #688.
        av[i + 1] = (char*)aether_string_data(list_get_raw(argv_list, i));
    }
    av[n + 1] = NULL;
    return av;
}

// Build a NULL-terminated environ array from an Aether list of
// "KEY=VALUE" strings. Returns NULL if env_list is NULL (caller should
// inherit parent env in that case). Caller must free the returned
// array.
static char** build_envp_array(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    char** envp = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!envp) return NULL;
    for (int i = 0; i < n; i++) {
        envp[i] = (char*)aether_string_data(list_get_raw(env_list, i));
    }
    envp[n] = NULL;
    return envp;
}

/* The program a spawn runs (#3201) is resolved in the parent, by
 * aether_spawn_resolve (runtime/utils/aether_spawn.c): a name with a '/' as
 * given, a bare name on this process's PATH whatever env the child is
 * given, as Windows and Go do. A name not found is "program not found"
 * before any fork.
 *
 * It used to be the child's execvp, and with an explicit env the child's
 * execve, which does no lookup: a bare name was never found by os.run,
 * os.run_capture, os.run_full or os.run_supervised, and os.spawn_proc and
 * os.spawn_proc_to fell back to execvp, which ran it with this process's
 * environment instead of the one given.
 *
 * Resolving in the parent is what lets every child exec with execve and a
 * ready env (the explicit one, or environ). Between fork and exec a child
 * then calls only functions POSIX lists as async-signal-safe: sigemptyset,
 * sigprocmask, sigaction, memset, fcntl, dup2, close, open, setpgid, execve
 * (the program, then /bin/sh for a file with no #! line, as execvp does),
 * write (the exec-error pipe) and _exit. No malloc, no setenv (os.run_pipe's
 * env is built in the parent, #3200), no stdio, no execvp, whose PATH walk
 * POSIX does not promise to be safe there. */

/* What a child execs, all made in the parent before the fork (#3201). */
typedef struct {
    char*  path;     /* the program, resolved: aether_spawn_resolve */
    char** argv;     /* build_argv_array */
    char** sh_argv;  /* /bin/sh, path, argv[1..]: for a file with no #! */
    int    sh_granted; /* the sandbox lets sh_argv[0] run (#3755) */
    char** envp;     /* build_envp_array: the explicit env, or NULL */
    char** env;      /* what execve gets: envp, or NULL for this process's
                      * environ, which the child reads after the fork: taken
                      * in the parent, a setenv on another thread before the
                      * fork could move the array and leave the pointer
                      * dangling */
} AetherExec;

/* Whether `path` is a file execve may refuse with ENOEXEC, so that the child
 * falls back to the shell: one that starts with neither "#!" nor the magic
 * of an executable this kernel family loads (ELF, Mach-O thin or fat, either
 * byte order). A file whose first bytes cannot be read (execute-only) may.
 * Asked so that the sandbox check of the shell, which records a refusal in
 * the audit, is made only for a start that can use the shell: made for every
 * os.run it put a refused "exec /bin/sh" in the audit for programs that never
 * went near one. */
static int aether_exec_may_need_shell(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    unsigned char h[4] = {0, 0, 0, 0};
    ssize_t n;
    do { n = read(fd, h, sizeof h); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n >= 2 && h[0] == '#' && h[1] == '!') return 0;
    if (n < 4) return 1;
    uint32_t be = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                  ((uint32_t)h[2] << 8) | (uint32_t)h[3];
    if (be == 0x7f454c46u) return 0;                         /* ELF */
    if (be == 0xfeedfaceu || be == 0xfeedfacfu ||            /* Mach-O */
        be == 0xcefaedfeu || be == 0xcffaedfeu ||
        be == 0xcafebabeu || be == 0xbebafecau) return 0;    /* fat */
    return 1;
}

/* NULL when `x` is ready, else the err the spawn reports. */
static const char* aether_exec_prepare(AetherExec* x, const char* prog,
                                       void* argv_list, void* env_list) {
    memset(x, 0, sizeof *x);
    x->path = aether_spawn_resolve(prog);
    if (!x->path) return errno == ENOMEM ? "out of memory" : "program not found";
    x->argv = build_argv_array(prog, argv_list);
    size_t n = 0;
    while (x->argv && x->argv[n]) n++;
    x->sh_argv = x->argv ? (char**)malloc((n + 2) * sizeof(char*)) : NULL;
    if (!x->sh_argv) {
        free(x->argv);
        free(x->path);
        memset(x, 0, sizeof *x);
        return "argv build failed";
    }
    x->sh_argv[0] = (char*)AETHER_OS_SHELL;
    /* Whether a file with no #! line may run through the shell: the shell
     * runs whatever the file holds, so it needs an exec grant of its own,
     * as a command string does (#3735) and as the LD_PRELOAD library's
     * exec_or_script requires. Asked here, in the parent: the check is not
     * a call the child may make between fork and exec (#3755). */
    x->sh_granted = aether_exec_may_need_shell(x->path)
                        ? aether_sandbox_check("exec", x->sh_argv[0])
                        : 0;
    x->sh_argv[1] = x->path;
    for (size_t i = 1; i <= n; i++) x->sh_argv[i + 1] = x->argv[i];
    x->envp = build_envp_array(env_list);
    if (env_list && !x->envp) {
        free(x->sh_argv);
        free(x->argv);
        free(x->path);
        memset(x, 0, sizeof *x);
        return "env build failed";
    }
    x->env = x->envp;
    return NULL;
}

static void aether_exec_release(AetherExec* x) {
    free(x->path);
    free(x->argv);
    free(x->sh_argv);
    free(x->envp);
    memset(x, 0, sizeof *x);
}

/* In the child: exec the program. A file with no #! line, which execve
 * refuses with ENOEXEC, runs under /bin/sh with its arguments, as execvp
 * runs it, so a script that never had one keeps working. Returns only when
 * neither could start, errno set: "not an executable" then means /bin/sh
 * could not run it either. Only execve, which is async-signal-safe.
 *
 * Under a sandbox the shell runs such a file only with an exec grant for
 * the shell (grant_shell), checked by the parent in aether_exec_prepare.
 * Without one the start fails with EACCES, "permission denied" (#3755): the
 * grant for the file let the shell run it, whatever it held. */
static void aether_child_exec(const AetherExec* x) {
    char** env = x->env ? x->env : aether_environ;
    execve(x->path, x->argv, env);
    if (errno != ENOEXEC) return;
    if (!x->sh_granted) {
        errno = EACCES;
        return;
    }
    /* sh_argv[0], not the literal: the pointer a spawn hook was told about
     * (AetherSpawnHook.ready). */
    execve(x->sh_argv[0], x->sh_argv, env);
    errno = ENOEXEC;
}

/* Give a freshly-forked child a clean signal slate before exec.
 *
 * POSIX inherits the signal MASK across both fork and execve, and inherits a
 * SIG_IGN disposition across execve (handlers reset to SIG_DFL automatically,
 * but "ignore" does not). So a child spawned from a signal-masking parent — the
 * JVM, .NET, Ruby, and Julia runtimes all block signals in their threads —
 * starts life with, e.g., SIGTERM blocked, and a later os.kill(token, SIGTERM)
 * is delivered but never acted on: the child never dies and an unbounded
 * os.wait on it blocks forever (asks/spawn-child-inherits-blocked-signal-mask).
 *
 * Every spawn path owes the child the clean slate posix_spawn gives with
 * SETSIGMASK|SETSIGDEF, and that Python subprocess (restore_signals) and Go
 * os/exec apply by default: unblock everything, and reset any inherited
 * SIG_IGN back to SIG_DFL. Async-signal-safe (only sigprocmask/sigaction), so
 * it is legal between fork and exec even in a threaded parent. */
static void aether_child_reset_signals(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    for (int sig = 1; sig < NSIG; sig++) {
        struct sigaction cur;
        if (sigaction(sig, NULL, &cur) != 0) continue;   /* skip invalid/uncatchable */
        if (cur.sa_handler == SIG_IGN) {
            struct sigaction dfl;
            memset(&dfl, 0, sizeof dfl);
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(sig, &dfl, NULL);                  /* no-op on SIGKILL/SIGSTOP */
        }
    }
}

/* A child starts with its parent's standard streams, and exec closes any
 * descriptor marked close-on-exec. A parent that marks 0, 1 and 2 that way
 * (Node does) therefore spawned children with no stdin, stdout or stderr, and
 * a program that needs them failed in ways nothing named: chromedriver cannot
 * start Chrome with stdin closed ("Failed to create Chrome process"), and the
 * first file a child opened took descriptor 0. fork leaves the flag set on the
 * child's copies; clear it on those three, which is what "inherits the
 * parent's streams" means. A stream the parent has closed stays closed.
 *
 * Which descriptors are the parent's streams: those that were 0, 1 and 2
 * when the process started (when this library was loaded), known by the
 * file they refer to. One the parent closed since gives its number to the
 * next descriptor opened, a pipe of another thread's spawn among them, and
 * clearing the flag on that one handed another spawn's pipe end to this
 * child as its stdout: that spawn then waited for this child to exit (#3209).
 * A descriptor marked close-on-exec on one of those numbers that is not the
 * file it was at the start is its owner's, and closes at exec as its owner
 * asked. */
/* NetBSD and OpenBSD report every pipe as device 0, inode 0, so there a
 * pipe that took a closed stream's number passed for it. A time each sets
 * when the pipe is made, and never after, tells pipes apart: NetBSD's birth
 * time, OpenBSD's change time. */
#if defined(__NetBSD__)
#define AETHER_STD_STREAM_MADE(st) ((st)->st_birthtimespec)
#elif defined(__OpenBSD__)
#define AETHER_STD_STREAM_MADE(st) ((st)->st_ctim)
#endif

static struct {
    int open; dev_t dev; ino_t ino;
#ifdef AETHER_STD_STREAM_MADE
    struct timespec made;
    int cloexec;   /* close-on-exec when the process started */
#endif
} aether_std_stream[3];

/* Whether `st` is the file stream `fd` was at the start. `fl` is the
 * descriptor's flags now, and the caller only asks about a close-on-exec one. */
static int aether_std_stream_same(int fd, const struct stat* st, int fl) {
    (void)fl;
    if (st->st_dev != aether_std_stream[fd].dev || st->st_ino != aether_std_stream[fd].ino)
        return 0;
#ifdef AETHER_STD_STREAM_MADE
    if (st->st_dev == 0 && st->st_ino == 0) {
        if (AETHER_STD_STREAM_MADE(st).tv_sec != aether_std_stream[fd].made.tv_sec ||
            AETHER_STD_STREAM_MADE(st).tv_nsec != aether_std_stream[fd].made.tv_nsec)
            return 0;
        /* The pipe's time is the kernel's coarse clock (10 ms on OpenBSD),
         * so two pipes made within a tick report the same one: a spawn pipe
         * that took the number of a closed stdout a few milliseconds after
         * the process started looked like the stream itself. Of the two
         * that cannot be told apart, only one is close-on-exec if the
         * stream was not when the process started: the parent that marks its
         * streams (Node) has done so before this library loads. */
        if ((fl & FD_CLOEXEC) && !aether_std_stream[fd].cloexec)
            return 0;
    }
#endif
    return 1;
}

__attribute__((constructor))
static void aether_record_std_streams(void) {
    for (int fd = 0; fd <= 2; fd++) {
        struct stat st;
        if (fstat(fd, &st) != 0) continue;
        aether_std_stream[fd].open = 1;
        aether_std_stream[fd].dev = st.st_dev;
        aether_std_stream[fd].ino = st.st_ino;
#ifdef AETHER_STD_STREAM_MADE
        aether_std_stream[fd].made = AETHER_STD_STREAM_MADE(&st);
        aether_std_stream[fd].cloexec = (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
#endif
    }
}

/* fcntl and fstat only: legal between fork and exec. */
static void aether_child_keep_stdio(void) {
    for (int fd = 0; fd <= 2; fd++) {
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0 || !(fl & FD_CLOEXEC) || !aether_std_stream[fd].open) continue;
        struct stat st;
        if (fstat(fd, &st) != 0 || !aether_std_stream_same(fd, &st, fl))
            continue;
        fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
    }
}

/* Every pipe here is made with aether_pipe_cloexec and every fork is
 * aether_fork (runtime/utils/aether_spawn.h, #2917, #3028, #3046): a pipe
 * closes on exec in every child, the ends a child keeps are moved onto its
 * stdio (or fd 3) with aether_child_fd, which clears the flag on the copy,
 * and where pipe2 is missing no fork runs between a pipe() and its fcntl(). */

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) return -1;
    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(errp[0]);
        close(errp[1]);
        return -1;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        return -1;
    }
    if (pid == 0) {
        // Child
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        aether_child_exec(&x);
        // exec only returns on failure
        aether_child_exec_failed(ef);
    }
    // Parent
    aether_exec_release(&x);
    /* A program that could not start is -1, as on Windows (#3170). */
    int exec_err = aether_exec_errno(errp);
    int status = 0;
    while (aether_waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (exec_err) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}


/* In a forked child: make `fd` the descriptor `target`, open across exec.
 * `fd` comes from aether_child_lift_fd, so it is never another target. */
static int aether_child_fd(int fd, int target) {
    if (fd == target) return fcntl(target, F_SETFD, 0) == -1 ? -1 : target;
    return dup2(fd, target);
}

/* Every wait that reaps a pid tells the process-group table (below). */
static void group_reaped(pid_t pid);

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    int pipefd[2];
    if (aether_pipe_cloexec(pipefd) != 0) return NULL;
    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        return NULL;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        return NULL;
    }
    if (pid == 0) {
        // Child: redirect stdout to pipe write end, close read end
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(pipefd[0]);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int out_fd = aether_child_lift_fd(pipefd[1]);
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        close(out_fd);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    // Parent: close write end, read until EOF
    close(pipefd[1]);
    aether_exec_release(&x);
    /* A program that could not start is NULL, as on Windows (#3170). */
    if (aether_exec_errno(errp)) {
        close(pipefd[0]);
        aether_reap_failed(pid);
        return NULL;
    }

    // Read buffer is page-sized for fewer syscalls on large output;
    // result buffer starts at 4 KB with doubling growth.
    size_t cap = 4096;
    size_t len = 0;
    /* #462: bound the captured child stdout — the prime DoS surface
     * (a sandboxed plugin can spawn a process that floods stdout).
     * Adopted into the returned string; error paths free through the
     * cap with the live size so the counter stays balanced. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(pipefd[0]);
        // Reap the child so we don't leave a zombie
        int st = 0;
        aether_waitpid(pid, &st, 0);
        return NULL;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(pipefd[0]);
            int st = 0;
            aether_waitpid(pid, &st, 0);
            return NULL;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                /* realloc failed: `result` still holds old_cap bytes. */
                aether_caps_free(result, old_cap);
                close(pipefd[0]);
                int st = 0;
                aether_waitpid(pid, &st, 0);
                return NULL;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);
    int st = 0;
    aether_waitpid(pid, &st, 0);
    return (char*)string_adopt_caps_buffer(result, len, cap);
}

/* Tuple-returning sibling of os_run_capture_raw — captures stdout AND
 * exposes the child's exit code. Returns (stdout, status, err):
 *   - stdout: same shape os_run_capture_raw returns. Empty string
 *             when the spawn fails.
 *   - status: WEXITSTATUS(st) on normal exit, -1 when the child was
 *             killed by a signal or the spawn itself failed.
 *   - err:    "" on successful spawn (regardless of exit code);
 *             non-empty only when the fork/exec couldn't run.
 *
 * This is the canonical entry point for callers that need to
 * distinguish "ran cleanly" from "ran but exited non-zero" (diff3,
 * grep, gcc, etc.). The plain `os_run_capture_raw` stays for callers
 * that don't care about status. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int pipefd[2];
    if (aether_pipe_cloexec(pipefd) != 0) {
        out._2 = "pipe failed";
        return out;
    }
    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        out._2 = "pipe failed";
        return out;
    }

    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        out._2 = xerr;
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(pipefd[0]);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int out_fd = aether_child_lift_fd(pipefd[1]);
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        close(out_fd);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    close(pipefd[1]);
    aether_exec_release(&x);
    /* A program that could not start reads as Windows has it (#3170). */
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        close(pipefd[0]);
        aether_reap_failed(pid);
        out._2 = aether_exec_failure(exec_err);
        return out;
    }

    size_t cap = 4096;
    size_t len = 0;
    /* #462: cap the captured child stdout (see os_run_capture_raw).
     * `result` is adopted into out._0; error paths free through the cap. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(pipefd[0]);
        int st = 0;
        aether_waitpid(pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(pipefd[0]);
            int st = 0;
            aether_waitpid(pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                aether_caps_free(result, old_cap);
                close(pipefd[0]);
                int st = 0;
                aether_waitpid(pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);

    string_release(out._0);
    out._0 = (const char*)string_adopt_caps_buffer(result, len, cap);
    int st = 0;
    if (aether_waitpid(pid, &st, 0) < 0) {
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    {
        _tuple_int_string s = posix_status_to_tuple(st);
        out._1 = s._0;
        out._2 = s._1;
    }
    return out;
}

/* ============================================================
 * std.ipc — fd-inheritance back-channel for Aether ↔ Aether spawn.
 *
 * Aeb's `aether.driver_test` pattern: parent spawns a child that
 * needs to send back a structured payload (test results, build
 * telemetry, etc) richer than an exit code. Without this, every
 * consumer reinvents the marker-file pattern.
 *
 * Mechanism: parent opens a pipe, dup2's the write end to a chosen
 * fd (3 by convention, but the value goes into AETHER_IPC_FD so
 * the child reads the env var rather than hardcoding the number),
 * forks, exec's. Child can use ipc.parent_channel() to discover
 * the fd, write a payload, close.
 *
 * Three variants of the parent-side spawn:
 *
 *   os_run_pipe_raw()                  — async; returns (read_fd,
 *                                        pid, err). Caller MUST
 *                                        wait via os_wait_pid_raw,
 *                                        and reads concurrently to
 *                                        avoid pipe-buffer deadlock
 *                                        if the payload exceeds
 *                                        OS pipe size (16K macOS,
 *                                        64K Linux).
 *   os_run_pipe_drain_and_wait_raw()   — sync convenience; reads
 *                                        the channel to EOF and
 *                                        waits for child. Same
 *                                        shape as run_capture but
 *                                        captures the IPC fd
 *                                        instead of stdout. No
 *                                        deadlock regardless of
 *                                        payload size.
 *   os_wait_pid_raw()                  — companion to os_run_pipe;
 *                                        reaps child, returns exit.
 *
 * AETHER_IPC_FD is the contract; fd 3 is the default but the env
 * var is what the child reads. Sidesteps fd-allocation surprises
 * in shell intermediaries (e.g. `bash -c '<driver>'`).
 * ============================================================ */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; int _1; int _2; const char* _3; } _tuple_int_int_int_string;

/* os.run_pipe's child env (#3200): `base` (the env the caller gave, or this
 * process's environ when it gave none) without any AETHER_IPC_FD of its
 * own, plus AETHER_IPC_FD=3, which tells the child that fd 3 carries the
 * parent channel (its ipc.parent_channel() reads it). Built in the parent:
 * the child used to call setenv between fork and exec, which mallocs and
 * takes the environment's lock, and a lock another thread held at the fork
 * is never released in the child. The array is malloc'd; its strings are
 * the base's and a literal. NULL when malloc fails. */
static char** aether_ipc_env(char** base) {
    static const char* const ipc[] = { "AETHER_IPC_FD=3" };
    return aether_spawn_env(base ? base : aether_environ, ipc, 1);
}

/* os.run_socket's child env: as aether_ipc_env, plus AETHER_IPC_KIND=socket,
 * which ipc.parent_channel_kind() reads. Either of the two in `base` is
 * replaced, not duplicated. */
static char** aether_ipc_socket_env(char** base, const char* const* extra, int nextra) {
    static const char* const ipc[] = { "AETHER_IPC_FD=3", "AETHER_IPC_KIND=socket" };
    if (nextra <= 0) return aether_spawn_env(base ? base : aether_environ, ipc, 2);
    const char** set = (const char**)malloc((size_t)(nextra + 2) * sizeof(char*));
    if (!set) return NULL;
    set[0] = ipc[0];
    set[1] = ipc[1];
    for (int i = 0; i < nextra; i++) set[i + 2] = extra[i];
    char** env = aether_spawn_env(base ? base : aether_environ, set, nextra + 2);
    free(set);
    return env;
}

/* os.run_stdio's child env: `base` (or environ) with AETHER_IPC_KIND=stdio,
 * which ipc.parent_channel_kind() reads, and without AETHER_IPC_FD: this
 * child has no fd-3 channel, whatever its parent was given. */
static char** aether_ipc_stdio_env(char** base) {
    static const char* const ipc[] = { "AETHER_IPC_KIND=stdio" };
    char** env = aether_spawn_env(base ? base : aether_environ, ipc, 1);
    if (!env) return NULL;
    size_t k = 0;
    for (size_t i = 0; env[i]; i++) {
        if (strncmp(env[i], "AETHER_IPC_FD=", 14) != 0) env[k++] = env[i];
    }
    env[k] = NULL;
    return env;
}

/* A connected AF_UNIX stream pair, both ends close-on-exec, for a process
 * to use itself (std.ipc's tests do) or to hand one end to a child it
 * spawns some other way. Returns (fd_a, fd_b, err). */
_tuple_int_int_string os_socketpair_raw(void) {
    _tuple_int_int_string out = { -1, -1, "" };
    int fds[2];
    if (aether_socketpair_cloexec(fds) < 0) {
        out._2 = aether_os_strerror_msg("os.socketpair", errno);
        return out;
    }
    out._0 = fds[0];
    out._1 = fds[1];
    return out;
}

/* Spawn child with a back-channel at fd 3, set AETHER_IPC_FD=3 in child's
 * env. The channel is the write end of a pipe (os_run_pipe_raw) or one end
 * of a socket pair (os_run_socket_raw, which also sets
 * AETHER_IPC_KIND=socket). Returns (parent_fd, child_pid, err). On success,
 * err is "" and caller must:
 *   1. Read from (and, for a socket, write to) parent_fd.
 *   2. Call os_wait_pid_raw(child_pid) to reap.
 *   3. Close parent_fd.
 *
 * Failure modes return (-1, -1, "<reason>") with no child left to reap:
 * a program that could not start reads so too, the reason from its exec
 * (#3170). An explicit env is refused before the fork: the child's
 * AETHER_IPC_FD would have to be spliced into it, and it used to make the
 * child exit 127 without running the program.
 *
 * Pipe-buffer deadlock window: if child writes > OS pipe buffer
 * (typically 16K-64K) without parent reading concurrently, the
 * child blocks forever on its write. For payload-at-exit usage
 * with large payloads, prefer os_run_pipe_drain_and_wait_raw. A socket
 * has the same window in both directions. */
static _tuple_int_int_string aether_spawn_channel_raw(const char* prog, void* argv_list, void* env_list, int is_socket,
                                                      const AetherSpawnHook* hook) {
    _tuple_int_int_string out = { -1, -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int chanfd[2];
    if (is_socket) {
        if (aether_socketpair_cloexec(chanfd) != 0) {
            out._2 = aether_os_strerror_msg("os.run_socket: socketpair", errno);
            return out;
        }
    } else {
        if (aether_pipe_cloexec(chanfd) != 0) {
            out._2 = "pipe failed";
            return out;
        }
    }

    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) {
        close(chanfd[0]);
        close(chanfd[1]);
        out._2 = "pipe failed";
        return out;
    }

    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(chanfd[0]);
        close(chanfd[1]);
        close(errp[0]);
        close(errp[1]);
        out._2 = xerr;
        return out;
    }

    /* The child's env: the one given, or environ, with AETHER_IPC_FD=3 (and
     * for a socket AETHER_IPC_KIND=socket). The spliced array borrows the
     * given one's strings, so only its array goes. */
    char** spliced = is_socket
        ? aether_ipc_socket_env(x.envp, hook ? hook->env : NULL, hook ? hook->nenv : 0)
        : aether_ipc_env(x.envp);
    free(x.envp);
    x.envp = spliced;
    if (!x.envp) {
        close(chanfd[0]);
        close(chanfd[1]);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        out._2 = "env build failed";
        return out;
    }
    x.env = x.envp;

    if (hook && hook->ready && hook->ready(hook->ctx, x.path, x.sh_argv[0]) != 0) {
        close(chanfd[0]);
        close(chanfd[1]);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        out._2 = hook->failed ? hook->failed(hook->ctx, 0, 0) : "spawn refused";
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(chanfd[0]);
        close(chanfd[1]);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child — wire chanfd[1] to fd 3; its env says so. */
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(chanfd[0]);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int chan = aether_child_lift_fd(chanfd[1]);
        if (aether_child_fd(chan, 3) < 0) aether_child_exec_failed(ef);
        close(chan);
        if (hook) {
            /* A hook's step goes up the error pipe in the bits above an
             * errno's, so the parent can tell it from an exec failure. */
            if (hook->keep_fd >= 0 && fcntl(hook->keep_fd, F_SETFD, 0) == -1) {
                errno = (1 << 16) | (errno & 0xffff);
                aether_child_exec_failed(ef);
            }
            int step = hook->child ? hook->child(hook->ctx) : 0;
            if (step != 0) {
                errno = ((step + 1) << 16) | (errno & 0xffff);
                aether_child_exec_failed(ef);
            }
        }
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    close(chanfd[1]);
    aether_exec_release(&x);
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        close(chanfd[0]);
        aether_reap_failed(pid);
        int step = (exec_err >> 16) - 1;
        if (hook && step >= 0) {
            out._2 = hook->failed ? hook->failed(hook->ctx, step, exec_err & 0xffff)
                                  : "the child could not be set up";
        } else {
            out._2 = aether_exec_failure(exec_err);
        }
        return out;
    }

    /* A socket's parent end is marked so std.ipc refuses a descriptor the
     * child sends up it. If it cannot be marked the child is already
     * running with its end, so it is ended rather than left unguarded. */
    if (is_socket && aether_ipc_mark_parent_end(chanfd[0]) != 0) {
        close(chanfd[0]);
        kill(pid, SIGKILL);
        aether_reap_failed(pid);
        out._2 = "os.run_socket: too many open channels";
        return out;
    }

    out._0 = chanfd[0];
    out._1 = pid;
    return out;
}

_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    return aether_spawn_channel_raw(prog, argv_list, env_list, 0, NULL);
}

_tuple_int_int_string os_run_socket_raw(const char* prog, void* argv_list, void* env_list) {
    return aether_spawn_channel_raw(prog, argv_list, env_list, 1, NULL);
}

/* os.run_socket with a hook (runtime/utils/aether_spawn.h): what
 * sandbox.spawn_socket starts its child with. A keep_fd step reads as step
 * 0 to `failed`; the hook's own child steps as themselves. */
int aether_os_run_socket_hooked(const char* prog, void* argv_list, void* env_list,
                                const AetherSpawnHook* hook,
                                int* fd, int* pid, const char** errmsg) {
    _tuple_int_int_string r = aether_spawn_channel_raw(prog, argv_list, env_list, 1, hook);
    if (fd) *fd = r._0;
    if (pid) *pid = r._1;
    if (errmsg) *errmsg = r._2;
    return r._2 && r._2[0] ? -1 : 0;
}

/* os.run_stdio: spawn `prog` with its stdin and stdout each a pipe this
 * process keeps. Its stderr is this process's (stderr_to 0), nowhere
 * (1, /dev/null) or the file stderr_path, created or truncated (2); a
 * non-empty `cwd` is the directory it runs in. Returns (to_child_fd,
 * from_child_fd, pid, "") or (-1, -1, -1, reason) with nothing left to
 * reap. Both of the
 * parent's ends are close-on-exec, so a child started later (by this or
 * another thread) does not hold the conversation open. The env is the one
 * given, or this process's, with AETHER_IPC_KIND=stdio and no AETHER_IPC_FD:
 * the channel is the child's own stdin and stdout, which a program in any
 * language (or a container runtime passing them through) reads and writes
 * as they are.
 *
 * The discipline is aether_spawn_channel_raw's: pipes from
 * aether_pipe_cloexec, the program resolved and argv and env built before
 * aether_fork, only async-signal-safe calls in the child, and a program
 * that cannot start reported through the exec-error pipe in this call's
 * own shape (#3170). */
_tuple_int_int_int_string os_run_stdio_raw(const char* prog, void* argv_list, void* env_list,
                                           int stderr_to, const char* stderr_path,
                                           const char* cwd) {
    _tuple_int_int_int_string out = { -1, -1, -1, "" };
    if (!prog) {
        out._3 = "null prog";
        return out;
    }
    const char* serr = os_stdio_stderr_check(stderr_to, stderr_path);
    if (serr) {
        out._3 = serr;
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._3 = "denied by sandbox";
        return out;
    }
    if (stderr_to == OS_STDERR_FILE && !aether_sandbox_check("fs", stderr_path)) {
        out._3 = "denied by sandbox";
        return out;
    }
    int has_cwd = cwd && cwd[0];
    /* The child's working directory is a chdir, checked as os.chdir is. */
    if (has_cwd && !aether_sandbox_check("fs", cwd)) {
        out._3 = "denied by sandbox";
        return out;
    }

    /* The child's stderr, opened here so a failure is this call's to report:
     * /dev/null, or the file. Close-on-exec; the child installs it on 2. */
    int err_fd = -1;
    if (stderr_to == OS_STDERR_DISCARD) {
        err_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    } else if (stderr_to == OS_STDERR_FILE) {
        err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (stderr_to != OS_STDERR_INHERIT && err_fd < 0) {
        out._3 = aether_os_strerror_msg(stderr_to == OS_STDERR_FILE
                                            ? "os.run_stdio: cannot open the stderr file"
                                            : "os.run_stdio: cannot open /dev/null",
                                        errno);
        return out;
    }

    /* inp: the child reads [0] as its stdin, the parent writes [1].
     * outp: the child writes [1] as its stdout, the parent reads [0]. */
    int inp[2], outp[2], errp[2];
    if (aether_pipe_cloexec(inp) != 0) {
        out._3 = aether_os_strerror_msg("os.run_stdio: pipe", errno);
        if (err_fd >= 0) close(err_fd);
        return out;
    }
    if (aether_pipe_cloexec(outp) != 0) {
        out._3 = aether_os_strerror_msg("os.run_stdio: pipe", errno);
        close(inp[0]); close(inp[1]);
        if (err_fd >= 0) close(err_fd);
        return out;
    }
    if (aether_pipe_cloexec(errp) != 0) {
        out._3 = aether_os_strerror_msg("os.run_stdio: pipe", errno);
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        if (err_fd >= 0) close(err_fd);
        return out;
    }

    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        if (err_fd >= 0) close(err_fd);
        out._3 = xerr;
        return out;
    }
    /* The child's env: the one given, or environ, with AETHER_IPC_KIND=stdio
     * and without AETHER_IPC_FD. Borrowed strings, as run_socket's. */
    char** spliced = aether_ipc_stdio_env(x.envp);
    free(x.envp);
    x.envp = spliced;
    x.env = spliced;
    if (!spliced) {
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        if (err_fd >= 0) close(err_fd);
        aether_exec_release(&x);
        out._3 = "env build failed";
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        if (err_fd >= 0) close(err_fd);
        aether_exec_release(&x);
        out._3 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child: inp[0] onto 0, outp[1] onto 1, and the stderr chosen onto
         * 2 (left as the parent's when inherited). Every end is lifted off
         * 0..3 first (#3199), so installing one cannot overwrite another
         * still to be installed. A chdir that fails goes up the error pipe
         * with 1 in the bits above its errno, told apart from the exec's. */
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(inp[1]);
        close(outp[0]);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int in_fd = aether_child_lift_fd(inp[0]);
        int out_fd = aether_child_lift_fd(outp[1]);
        int e_fd = err_fd >= 0 ? aether_child_lift_fd(err_fd) : -1;
        if (in_fd < 0 || out_fd < 0 || (err_fd >= 0 && e_fd < 0)) aether_child_exec_failed(ef);
        if (aether_child_fd(in_fd, 0) < 0) aether_child_exec_failed(ef);
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        if (e_fd >= 0 && aether_child_fd(e_fd, 2) < 0) aether_child_exec_failed(ef);
        close(in_fd);
        close(out_fd);
        if (e_fd >= 0) close(e_fd);
        if (has_cwd && chdir(cwd) != 0) {
            errno = (1 << 16) | (errno & 0xffff);
            aether_child_exec_failed(ef);
        }
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    close(inp[0]);
    close(outp[1]);
    if (err_fd >= 0) close(err_fd);
    aether_exec_release(&x);
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        close(inp[1]);
        close(outp[0]);
        aether_reap_failed(pid);
        out._3 = (exec_err >> 16) == 1
                     ? aether_os_strerror_msg("os.run_stdio: cannot enter the working directory",
                                              exec_err & 0xffff)
                     : aether_exec_failure(exec_err);
        return out;
    }
    out._0 = inp[1];
    out._1 = outp[0];
    out._2 = (int)pid;
    return out;
}

/* Wait for a child, return (exit_code, err). Reaps the child so
 * it doesn't become a zombie. Companion to os_run_pipe_raw. */
_tuple_int_string os_wait_pid_raw(int pid) {
    _tuple_int_string out = { -1, "" };
    if (pid <= 0) {
        out._1 = "invalid pid";
        return out;
    }
    int st = 0;
    if (aether_waitpid((pid_t)pid, &st, 0) < 0) {
        out._1 = "waitpid failed";
        return out;
    }
    group_reaped((pid_t)pid);
    return posix_status_to_tuple(st);
}

/* ============================================================
 * Non-blocking spawn + reap, WITHOUT the IPC back-channel pipe.
 *
 * These are the fan-out/fan-in pair a parallel build scheduler needs:
 * start up to N ready children, wait for ANY to finish, reap it, start
 * more. Unlike os_run_pipe_raw they create no pipe and set no
 * AETHER_IPC_FD — which is exactly why they are cross-platform (the pipe
 * is the part that needs coordinated _open_osfhandle on Windows). The
 * "token" returned by spawn is the reap handle: on POSIX it is the real
 * pid; on Windows it is an index into a process-handle table.
 *
 *   os_spawn_raw()     -> (token, err)       async spawn, no pipe
 *   os_wait_raw()      -> (exit, err)        reap one token
 *   os_wait_any_raw()  -> (token, exit, err) reap whichever finishes first
 *
 * Each spawned token MUST be reaped exactly once (via wait or wait_any)
 * or it leaks a zombie (POSIX) / handle (Windows). ============ */

/* Map a waitpid status to (exit, err). The ONLY such mapping in this file:
 * wait, wait_any, wait_pid, run_capture_status and run_pipe_drain_and_wait all
 * come through here, so a child killed by a signal reads the same -- 128+signo,
 * no error -- whichever entry point reaped it. */
static _tuple_int_string posix_status_to_tuple(int st) {
    _tuple_int_string out = { -1, "" };
    if (WIFEXITED(st)) {
        out._0 = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        /* Killed by a signal: report 128+signo (the shell convention) with
         * NO error, so a caller that kills a token (e.g. after a wait_any_
         * timeout) can reap a distinguishable status — 137 for SIGKILL,
         * 143 for SIGTERM — rather than an opaque "abnormal" error (#1278). */
        out._0 = 128 + WTERMSIG(st);
    } else {
        out._0 = -1;
        out._1 = "child terminated abnormally";
    }
    return out;
}

_tuple_int_string os_spawn_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) { out._1 = "pipe failed"; return out; }
    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(errp[0]); close(errp[1]);
        out._1 = xerr;
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(errp[0]); close(errp[1]);
        aether_exec_release(&x);
        out._1 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child — no pipe, no fd 3, no AETHER_IPC_FD. Plain exec. */
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    aether_exec_release(&x);
    /* A program that could not start fails the spawn, as on Windows (#3170). */
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        aether_reap_failed(pid);
        out._1 = aether_exec_failure(exec_err);
        return out;
    }
    out._0 = (int)pid;   /* token == pid on POSIX */
    return out;
}

/* The children os_spawn_to_raw started, each the leader of a process group
 * of its own: os_kill_raw signals the whole group for them, so a kill ends
 * the child's tree. A child leaves the table when it is reaped. */
#define AETHER_GROUP_SLOTS 4096
static pid_t g_group_leaders[AETHER_GROUP_SLOTS];
static int g_group_count = 0;
static volatile int g_group_lock = 0;

static void group_lock(void) {
    while (__sync_lock_test_and_set(&g_group_lock, 1)) {
    }
}

static void group_unlock(void) {
    __sync_lock_release(&g_group_lock);
}

static int group_add(pid_t pid) {
    group_lock();
    int ok = g_group_count < AETHER_GROUP_SLOTS;
    if (ok) g_group_leaders[g_group_count++] = pid;
    group_unlock();
    return ok;
}

static int group_has(pid_t pid) {
    int found = 0;
    group_lock();
    for (int i = 0; i < g_group_count; i++) {
        if (g_group_leaders[i] == pid) { found = 1; break; }
    }
    group_unlock();
    return found;
}

/* Called for every pid a wait reaps. */
static void group_reaped(pid_t pid) {
    group_lock();
    for (int i = 0; i < g_group_count; i++) {
        if (g_group_leaders[i] == pid) {
            g_group_leaders[i] = g_group_leaders[--g_group_count];
            break;
        }
    }
    group_unlock();
}

/* Non-blocking spawn with the child's stdout and stderr written to
 * `out_path` (created or truncated, mode 0644) and its stdin /dev/null. The
 * child leads a process group of its own, so os_kill_raw on the token
 * signals the whole group: the child's tree. The file is opened close-on-exec
 * in the parent, so no other child inherits it (#2917). Returns (token, err);
 * reap the token with the wait calls, as an os_spawn_raw one. */
_tuple_int_string os_spawn_to_raw(const char* prog, void* argv_list, void* env_list,
                                  const char* out_path) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!out_path) { out._1 = "null output path"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }
    if (!aether_sandbox_check("fs", out_path)) { out._1 = "denied by sandbox"; return out; }

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { out._1 = "cannot open the output file"; return out; }
    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) { close(fd); out._1 = "pipe failed"; return out; }
    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(fd);
        close(errp[0]);
        close(errp[1]);
        out._1 = xerr;
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(fd);
        close(errp[0]);
        close(errp[1]);
        aether_exec_release(&x);
        out._1 = "fork failed";
        return out;
    }
    if (pid == 0) {
        aether_child_reset_signals();
        aether_child_keep_stdio();
        setpgid(0, 0);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int out_fd = aether_child_lift_fd(fd);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0 && nul != 0) {
            dup2(nul, 0);
            close(nul);
        }
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        if (dup2(1, 2) < 0) aether_child_exec_failed(ef);
        close(out_fd);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    /* Also set here, so the group exists before this call returns whichever
     * of the two runs first. */
    setpgid(pid, pid);
    close(fd);
    aether_exec_release(&x);
    /* A program that could not start fails the spawn, as on Windows (#3170). */
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        aether_reap_failed(pid);
        out._1 = aether_exec_failure(exec_err);
        return out;
    }
    group_add(pid);
    out._0 = (int)pid;   /* token == pid on POSIX */
    return out;
}

/* ---- os.spawn_tree_to: a child whose whole tree ends with this process ----
 *
 * os_spawn_to_raw's child leads a process group, and os.kill signals the
 * group, which ends the tree while this process lives and asks. Two things
 * it cannot do: end the tree when this process dies (by SIGKILL as well),
 * and reach a descendant that left the group with setsid or setpgid
 * (selaenium asks/aether-spawn-tree-dies-with-parent.md, where browsers a
 * WebDriver driver started outlived the test that started the driver).
 *
 * The shape. This process forks a SUPERVISOR, which does not exec; the
 * supervisor forks the program. The token the caller reaps is the
 * supervisor's pid, and the supervisor exits with the program's status, so
 * wait, wait_any and wait_any_timeout work on it as on any token. The
 * supervisor:
 *   - becomes a child subreaper (Linux PR_SET_CHILD_SUBREAPER, FreeBSD
 *     procctl PROC_REAP_ACQUIRE), so a descendant whose parent dies is
 *     re-parented to it, not to init, whatever its process group;
 *   - holds the read end of a pipe whose only write end is this process's,
 *     in the spawn result. End of file on it means the handle was released
 *     (tree_kill, tree_free), or this process ended however it ended: the
 *     kernel closes the write end of a process that dies, SIGKILL included,
 *     and it is the PROCESS's end, not a thread's, so a runtime that spawns
 *     from a pooled thread does not set it off (PR_SET_PDEATHSIG would);
 *   - ends the tree on that, on SIGTERM, SIGINT or SIGHUP sent to the token,
 *     and when the program exits on its own: SIGTERM to the program's group
 *     and to every descendant it can find, up to grace_ms for them to go,
 *     then SIGKILL to the group and to every descendant until none is left.
 *     So once the token reaps, nothing the program started is running.
 *
 * Descendants by platform. Linux: the subreaper, and /proc read for the
 * descendants. FreeBSD: the reaper, PROC_REAP_KILL and PROC_REAP_STATUS.
 * macOS has no subreaper: the supervisor lists the descendants
 * (proc_listpids) every 50 ms while it watches and again at each step of the
 * teardown, and signals a listed one while its start time still matches, so
 * a descendant that left the group is reached even after its parent has
 * exited, unless it left and was orphaned within one 50 ms poll (a
 * double-forked daemon can be that quick); such a one is launchd's child and
 * is not reached. Other POSIX systems, and iOS: the group only.
 *
 * The program also gets the supervisor as its death signal on Linux and
 * FreeBSD (the supervisor is single-threaded, so the thread caveat does not
 * apply): a supervisor killed by SIGKILL takes the program with it, though
 * not what the program started. The write end is close-on-exec, so an exec
 * of this process (os.execv) ends the tree too, and a fork of this process
 * that does not exec keeps it alive while that fork lives.
 *
 * Between its fork and its _exit the supervisor makes only async-signal-safe
 * calls (and the platform's process-listing system calls): this process may
 * be multithreaded. Everything that allocates is done before the fork. */
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <dirent.h>
#elif defined(__FreeBSD__)
#include <sys/procctl.h>
#elif defined(__APPLE__) && defined(__has_include)
#if __has_include(<libproc.h>)   /* macOS; iOS has no libproc: the group only */
#include <libproc.h>
#define AETHER_TREE_LIBPROC 1
#endif
#endif

typedef struct { void* _0; const char* _1; } _tuple_ptr_string_tree;

/* The spawn result's handle: the supervisor (the token), the program, and
 * the write end that keeps the tree alive. */
typedef struct {
    pid_t sup;
    pid_t pid;
    int   ctl;
} AetherTree;

/* What the supervisor tells the spawner once the program has started or
 * failed to: errno of a failed exec (0 when it ran), and the program's pid. */
typedef struct { int err; int pid; } AetherTreeStart;

/* Set by the supervisor's SIGTERM/SIGINT/SIGHUP handler, in the supervisor's
 * own copy of this file's memory; nothing in the spawning process reads it. */
static volatile sig_atomic_t g_tree_stop_asked = 0;
static void tree_on_stop(int sig) { (void)sig; g_tree_stop_asked = 1; }

static int64_t tree_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tree_nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void tree_write_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w;
        n -= (size_t)w;
    }
}

/* Close every descriptor from 3 up but `keep`: the supervisor must not hold
 * this process's other descriptors (another tree's write end above all, which
 * would keep that tree alive as long as this one). */
static void tree_close_from_3(int keep) {
#if defined(__linux__) && defined(SYS_close_range)
    if (keep >= 3) {
        if ((keep == 3 || syscall(SYS_close_range, 3u, (unsigned)keep - 1, 0u) == 0) &&
            syscall(SYS_close_range, (unsigned)keep + 1, ~0u, 0u) == 0) return;
    } else if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) {
        return;
    }
#endif
    long hi = sysconf(_SC_OPEN_MAX);
    if (hi <= 0 || hi > 65536) hi = 65536;
    for (int fd = 3; fd < (int)hi; fd++) {
        if (fd != keep) close(fd);
    }
}

#define AETHER_TREE_MAX 4096

#if defined(__linux__)
/* The pids of every process under /proc and its parent, into `pid`/`ppid`
 * (room for `max`). getdents64 and open/read: no allocation, no stdio. */
struct aether_tree_dirent64 {
    uint64_t d_ino; int64_t d_off; unsigned short d_reclen; unsigned char d_type; char d_name[];
};
static int tree_linux_table(pid_t* pid, pid_t* ppid, int max) {
    int dfd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return 0;
    int n = 0;
    char buf[4096];
    for (;;) {
        long got = syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got;) {
            struct aether_tree_dirent64* d = (struct aether_tree_dirent64*)(buf + off);
            off += d->d_reclen;
            const char* s = d->d_name;
            long p = 0;
            int digits = 1;
            for (const char* c = s; *c; c++) {
                if (*c < '0' || *c > '9') { digits = 0; break; }
                p = p * 10 + (*c - '0');
            }
            if (!digits || p <= 0 || n >= max) continue;
            char path[64] = "/proc/";
            size_t o = 6;
            for (const char* c = s; *c && o < 50; c++) path[o++] = *c;
            memcpy(path + o, "/stat", 6);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char st[512];
            ssize_t r;
            do { r = read(fd, st, sizeof st - 1); } while (r < 0 && errno == EINTR);
            close(fd);
            if (r <= 0) continue;
            st[r] = '\0';
            /* "pid (comm) S ppid ...": comm may hold spaces and parentheses,
             * so the state follows the LAST ')'. */
            char* rp = NULL;
            for (char* c = st; *c; c++) if (*c == ')') rp = c;
            if (!rp || rp[1] != ' ' || !rp[2] || rp[3] != ' ') continue;
            long pp = 0;
            for (char* c = rp + 4; *c >= '0' && *c <= '9'; c++) pp = pp * 10 + (*c - '0');
            pid[n] = (pid_t)p;
            ppid[n] = (pid_t)pp;
            n++;
        }
    }
    close(dfd);
    return n;
}
#endif

#if !defined(__FreeBSD__)   /* FreeBSD's reaper signals them all itself */
/* Every descendant of `root` alive now, into out (room for max): the count. */
static int tree_descendants(pid_t root, pid_t* out, int max) {
    int n = 0;
#if defined(__linux__)
    static pid_t* tab = NULL;   /* the supervisor's own, made once (mmap) */
    if (!tab) {
        void* m = mmap(NULL, sizeof(pid_t) * 2 * 32768, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) return 0;
        tab = (pid_t*)m;
    }
    int t = tree_linux_table(tab, tab + 32768, 32768);
    out[n++] = root;   /* breadth first from root, which is dropped below */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < t && n < max; j++) {
            if (tab[32768 + j] == out[i]) out[n++] = tab[j];
        }
    }
    for (int i = 1; i < n; i++) out[i - 1] = out[i];
    n--;
#elif defined(AETHER_TREE_LIBPROC)
    out[n++] = root;
    for (int i = 0; i < n; i++) {
        pid_t kids[512];
        int bytes = proc_listpids(PROC_PPID_ONLY, (uint32_t)out[i], kids, (int)sizeof kids);
        if (bytes <= 0) continue;
        int k = bytes / (int)sizeof(pid_t);
        for (int j = 0; j < k && n < max; j++) {
            if (kids[j] > 0) out[n++] = kids[j];
        }
    }
    for (int i = 1; i < n; i++) out[i - 1] = out[i];
    n--;
#else
    (void)root; (void)out; (void)max;
#endif
    return n;
}
#endif

#if defined(AETHER_TREE_LIBPROC)
/* A process's start time, to tell a listed descendant from a later process
 * that took its pid. 0 when it is gone. */
static uint64_t tree_started(pid_t p) {
    struct proc_bsdinfo bi;
    if (proc_pidinfo(p, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof bi) != (int)sizeof bi) return 0;
    return (uint64_t)bi.pbi_start_tvsec * 1000000u + (uint64_t)bi.pbi_start_tvusec;
}
#endif

/* Reap every child that has exited, keeping the program's status. Returns 1
 * when the supervisor has no children left. */
static int tree_reap(pid_t prog, int* prog_status, int* prog_reaped) {
    for (;;) {
        int st = 0;
        pid_t r = waitpid(-1, &st, WNOHANG);
        if (r > 0) {
            if (r == prog) { *prog_status = st; *prog_reaped = 1; }
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return r < 0 && errno == ECHILD;
    }
}

/* `sig` to the program's group and to every descendant there is to find:
 * those alive now (Linux, FreeBSD, macOS) and, on macOS, those listed when
 * the teardown began whose start time still matches. */
static void tree_signal(pid_t prog, int sig, const pid_t* early, const uint64_t* early_start, int nearly) {
    kill(-prog, sig);
#if defined(__FreeBSD__)
    struct procctl_reaper_kill rk;
    memset(&rk, 0, sizeof rk);
    rk.rk_sig = sig;
    procctl(P_PID, getpid(), PROC_REAP_KILL, &rk);
#else
    pid_t now[AETHER_TREE_MAX];
    int n = tree_descendants(getpid(), now, AETHER_TREE_MAX);
    for (int i = 0; i < n; i++) kill(now[i], sig);
#endif
#if defined(AETHER_TREE_LIBPROC)
    for (int i = 0; i < nearly; i++) {
        if (early_start[i] && tree_started(early[i]) == early_start[i]) kill(early[i], sig);
    }
#else
    (void)early; (void)early_start; (void)nearly;
#endif
}

/* Whether nothing of the tree is left. With a reaper (Linux, FreeBSD) every
 * descendant is a child or a child's descendant, so no children means none.
 * Without one (macOS), the group and the early list are checked too. */
static int tree_gone(pid_t prog, int no_children, const pid_t* early, const uint64_t* early_start, int nearly) {
    if (!no_children) return 0;
#if defined(AETHER_TREE_LIBPROC)
    if (kill(-prog, 0) == 0) return 0;
    for (int i = 0; i < nearly; i++) {
        if (early_start[i] && tree_started(early[i]) == early_start[i]) return 0;
    }
#elif !defined(__linux__) && !defined(__FreeBSD__)
    if (kill(-prog, 0) == 0) return 0;
#else
    (void)prog;
#endif
    (void)early; (void)early_start; (void)nearly;
    return 1;
}

/* macOS: every descendant seen while the supervisor watched, with its start
 * time; kept up to date at each poll, so one that leaves the group and is
 * then orphaned (its parent exits, the program included) is still known when
 * the teardown comes. Supervisor-only memory, like g_tree_stop_asked. */
static pid_t g_tree_seen[AETHER_TREE_MAX];
static uint64_t g_tree_seen_start[AETHER_TREE_MAX];
static int g_tree_nseen = 0;

static void tree_track(void) {
#if defined(AETHER_TREE_LIBPROC)
    int keep = 0;
    for (int i = 0; i < g_tree_nseen; i++) {
        if (tree_started(g_tree_seen[i]) == g_tree_seen_start[i]) {
            g_tree_seen[keep] = g_tree_seen[i];
            g_tree_seen_start[keep] = g_tree_seen_start[i];
            keep++;
        }
    }
    g_tree_nseen = keep;
    pid_t now[AETHER_TREE_MAX];
    int n = tree_descendants(getpid(), now, AETHER_TREE_MAX);
    for (int i = 0; i < n && g_tree_nseen < AETHER_TREE_MAX; i++) {
        int known = 0;
        for (int j = 0; j < g_tree_nseen; j++) {
            if (g_tree_seen[j] == now[i]) { known = 1; break; }
        }
        if (known) continue;
        uint64_t st = tree_started(now[i]);
        if (!st) continue;
        g_tree_seen[g_tree_nseen] = now[i];
        g_tree_seen_start[g_tree_nseen] = st;
        g_tree_nseen++;
    }
#endif
}

static void tree_teardown(pid_t prog, int grace_ms, int* prog_status, int* prog_reaped) {
    tree_track();
    const pid_t* early = g_tree_seen;
    const uint64_t* early_start = g_tree_seen_start;
    int nearly = g_tree_nseen;
    tree_signal(prog, SIGTERM, early, early_start, nearly);
    int64_t deadline = tree_now_ms() + grace_ms;
    for (;;) {
        int none = tree_reap(prog, prog_status, prog_reaped);
        if (tree_gone(prog, none, early, early_start, nearly)) return;
        if (tree_now_ms() >= deadline) break;
        tree_nap_ms(10);
    }
    /* Bounded: a process stuck in an uninterruptible sleep cannot hold the
     * supervisor, and so the token, for ever. */
    int64_t until = tree_now_ms() + 10000;
    for (;;) {
        tree_signal(prog, SIGKILL, early, early_start, nearly);
        int none = tree_reap(prog, prog_status, prog_reaped);
        if (tree_gone(prog, none, early, early_start, nearly)) return;
        if (tree_now_ms() >= until) return;
        tree_nap_ms(10);
    }
}

/* The supervisor's life; ends in _exit with the program's status (128+signo
 * when a signal ended it, as the wait calls read a signalled child). */
__attribute__((noreturn))
static void tree_supervise(int ctl, int start_w, int out_fd, const AetherExec* x, int grace_ms) {
    aether_child_reset_signals();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = tree_on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    /* A group of its own: a Ctrl-C meant for this process's foreground group
     * does not end the tree; only this process going, or asking, does. */
    setpgid(0, 0);
#if defined(__linux__)
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#elif defined(__FreeBSD__)
    procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
#endif
    ctl = aether_child_lift_fd(ctl);
    start_w = aether_child_lift_fd(start_w);
    out_fd = aether_child_lift_fd(out_fd);
    AetherTreeStart start = { 0, 0 };
    int ep[2];
    if (ctl < 0 || start_w < 0 || out_fd < 0 || pipe(ep) != 0) {
        start.err = errno ? errno : EIO;
        tree_write_all(start_w, &start, sizeof start);
        _exit(127);
    }
    fcntl(ep[0], F_SETFD, FD_CLOEXEC);
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);
    pid_t me = getpid();
    pid_t prog = fork();
    if (prog < 0) {
        start.err = errno;
        tree_write_all(start_w, &start, sizeof start);
        _exit(127);
    }
    if (prog == 0) {
#if defined(__linux__)
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#elif defined(__FreeBSD__) && defined(PROC_PDEATHSIG_CTL)
        {
            int ds = SIGKILL;
            procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &ds);
        }
#endif
        if (getppid() != me) _exit(127);   /* the supervisor is gone already */
        aether_child_reset_signals();
        setpgid(0, 0);
        close(ep[0]);
        close(ctl);
        close(start_w);
        int ef = aether_child_lift_fd(ep[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0 && nul != 0) {
            dup2(nul, 0);
            close(nul);
        }
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        if (dup2(1, 2) < 0) aether_child_exec_failed(ef);
        close(out_fd);
        aether_child_exec(x);
        aether_child_exec_failed(ef);
    }
    setpgid(prog, prog);
    close(ep[1]);
    close(out_fd);
    int e = 0;
    ssize_t r;
    do { r = read(ep[0], &e, sizeof e); } while (r < 0 && errno == EINTR);
    close(ep[0]);
    start.err = r == (ssize_t)sizeof e ? (e ? e : EIO) : 0;
    start.pid = (int)prog;
    tree_write_all(start_w, &start, sizeof start);
    close(start_w);
    int status = 0, reaped = 0;
    if (start.err) {
        while (waitpid(prog, &status, 0) < 0 && errno == EINTR) {}
        _exit(127);
    }

    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        if (nul > 2) close(nul);
    }
    if (ctl < 3) {   /* /dev/null went onto it: cannot happen, ctl was lifted */
        _exit(127);
    }
    tree_close_from_3(ctl);

    /* Until the handle is released or this process's parent is gone (end of
     * file), it is asked to stop, or the program exits. */
    for (;;) {
        if (g_tree_stop_asked) break;
        struct pollfd p = { ctl, POLLIN, 0 };
        int pr = poll(&p, 1, 50);
        if (pr > 0) {
            char b[16];
            ssize_t n = read(ctl, b, sizeof b);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) break;
        }
        tree_track();
        tree_reap(prog, &status, &reaped);
        if (reaped) break;
    }
    tree_teardown(prog, grace_ms, &status, &reaped);
    int code = 128 + SIGKILL;
    if (reaped) {
        if (WIFEXITED(status)) code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    }
    _exit(code);
}

_tuple_ptr_string_tree os_spawn_tree_to_raw(const char* prog, void* argv_list, void* env_list,
                                            const char* out_path, int grace_ms) {
    _tuple_ptr_string_tree out = { NULL, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!out_path) { out._1 = "null output path"; return out; }
    if (grace_ms < 0) grace_ms = 0;
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }
    if (!aether_sandbox_check("fs", out_path)) { out._1 = "denied by sandbox"; return out; }
#if defined(__linux__)
    /* The supervisor finds the descendants that left the program's group by
     * reading /proc, with open() as this process does, so a sandbox that
     * refuses that read (the preload, and Landlock under it) leaves it with
     * none to find: tree_kill and the exit would end the group and leave a
     * setsid'd descendant running, with the token reading as if all had
     * ended. Refuse the spawn instead, as the sandbox's own spawns fail
     * closed. The same two reads the supervisor makes: the directory, and a
     * process's stat by its pid (not "self", which a grant may name alone). */
    {
        char probe[64];
        snprintf(probe, sizeof probe, "/proc/%ld/stat", (long)getpid());
        int d = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int f = d < 0 ? -1 : open(probe, O_RDONLY | O_CLOEXEC);
        if (d >= 0) close(d);
        if (f >= 0) close(f);
        if (d < 0 || f < 0) {
            out._1 = "cannot read /proc, which ending the tree needs (a sandbox may refuse it: grant_fs_read(\"/proc/*\"))";
            return out;
        }
    }
#endif

    AetherTree* t = (AetherTree*)malloc(sizeof(AetherTree));
    if (!t) { out._1 = "out of memory"; return out; }
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { free(t); out._1 = "cannot open the output file"; return out; }
    int ctl[2], sp[2];
    if (aether_pipe_cloexec(ctl) != 0) {
        close(fd);
        free(t);
        out._1 = "pipe failed";
        return out;
    }
    if (aether_pipe_cloexec(sp) != 0) {
        close(fd);
        close(ctl[0]); close(ctl[1]);
        free(t);
        out._1 = "pipe failed";
        return out;
    }
    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(fd);
        close(ctl[0]); close(ctl[1]);
        close(sp[0]); close(sp[1]);
        free(t);
        out._1 = xerr;
        return out;
    }
    pid_t sup = aether_os_fork();
    if (sup < 0) {
        close(fd);
        close(ctl[0]); close(ctl[1]);
        close(sp[0]); close(sp[1]);
        aether_exec_release(&x);
        free(t);
        out._1 = "fork failed";
        return out;
    }
    if (sup == 0) {
        close(ctl[1]);
        close(sp[0]);
        tree_supervise(ctl[0], sp[1], fd, &x, grace_ms);
    }
    close(ctl[0]);
    close(sp[1]);
    close(fd);
    aether_exec_release(&x);
    AetherTreeStart start = { 0, 0 };
    size_t got = 0;
    while (got < sizeof start) {
        ssize_t r = read(sp[0], (char*)&start + got, sizeof start - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(sp[0]);
    if (got < sizeof start || start.err || start.pid <= 0) {
        close(ctl[1]);
        aether_reap_failed(sup);
        free(t);
        out._1 = got < sizeof start ? "the supervisor failed to start"
                                    : aether_exec_failure(start.err ? start.err : EIO);
        return out;
    }
    t->sup = sup;
    t->pid = (pid_t)start.pid;
    t->ctl = ctl[1];
    out._0 = t;
    return out;
}

int os_tree_token_raw(void* tree) {
    return tree ? (int)((AetherTree*)tree)->sup : -1;
}

int os_tree_pid_raw(void* tree) {
    return tree ? (int)((AetherTree*)tree)->pid : -1;
}

/* Release the write end: the supervisor ends the tree and exits, and the
 * token is then reaped with the wait calls. 0, or -1 when already asked. */
int os_tree_kill_raw(void* tree) {
    AetherTree* t = (AetherTree*)tree;
    if (!t || t->ctl < 0) return -1;
    if (!aether_sandbox_check("exec", "kill")) return -1;
    close(t->ctl);
    t->ctl = -1;
    return 0;
}

/* Free the handle. A tree still running is ended, as tree_kill ends it. */
void os_tree_free_raw(void* tree) {
    AetherTree* t = (AetherTree*)tree;
    if (!t) return;
    if (t->ctl >= 0) close(t->ctl);
    free(t);
}

_tuple_int_string os_wait_raw(int token) {
    /* On POSIX the token IS the pid, so reap semantics match wait_pid. */
    _tuple_int_string out = { -1, "" };
    if (token <= 0) { out._1 = "invalid token"; return out; }
    int st = 0;
    if (aether_waitpid((pid_t)token, &st, 0) < 0) { out._1 = "waitpid failed"; return out; }
    group_reaped((pid_t)token);
    return posix_status_to_tuple(st);
}

_tuple_int_int_string os_wait_any_raw(void* token_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._2 = "no tokens"; return out; }

    /* On POSIX the token IS the pid, and waitpid(-1) already reports which
     * child finished — so the returned pid is itself the finished token. We
     * don't unbox the list here (int-list element boxing is codegen-private
     * and not part of this file's ABI); the Windows backend, which cannot
     * wait on "any child" generically, is the one that consumes the list to
     * build its HANDLE array. A scheduler that spawns only via os_spawn_raw
     * has no foreign children, so the reaped pid is always one of its
     * tokens. */
    int st = 0;
    pid_t got = aether_waitpid(-1, &st, 0);
    if (got < 0) { out._2 = "waitpid failed"; return out; }
    group_reaped(got);

    _tuple_int_string s = posix_status_to_tuple(st);
    out._0 = (int)got;      /* which token finished */
    out._1 = s._0;          /* its exit code */
    out._2 = s._1;          /* err (abnormal-termination note, if any) */
    return out;
}

/* Bounded wait-any: reap whichever token finishes first, or give up after
 * `secs`. Returns (token, exit, timed_out, err):
 *   - timed_out == 1 → no child finished within `secs`; token is -1 and the
 *     caller decides what to kill. Live children are left running.
 *   - secs <= 0 → block indefinitely (identical to os_wait_any_raw, with the
 *     timed_out slot always 0).
 * As with os_wait_any_raw, on POSIX the token IS the pid and waitpid(-1)
 * reports which child finished, so the list is not unboxed here. Poll cadence
 * is 20ms against a CLOCK_MONOTONIC deadline, matching os_wait_pid_timeout. */
_tuple_int_int_int_string os_wait_any_timeout_raw(void* token_list, int secs) {
    _tuple_int_int_int_string out = { -1, -1, 0, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._3 = "no tokens"; return out; }

    int st = 0;
    if (secs <= 0) {
        pid_t got;
        while ((got = aether_waitpid(-1, &st, 0)) < 0) {
            if (errno != EINTR) { out._3 = "waitpid failed"; return out; }
        }
        group_reaped(got);
        _tuple_int_string s = posix_status_to_tuple(st);
        out._0 = (int)got;
        out._1 = s._0;
        out._3 = s._1;
        return out;
    }

    int64_t deadline = os_now_monotonic_ms_raw() + (int64_t)secs * 1000;
    for (;;) {
        pid_t got = aether_waitpid(-1, &st, WNOHANG);
        if (got > 0) {
            group_reaped(got);
            _tuple_int_string s = posix_status_to_tuple(st);
            out._0 = (int)got;
            out._1 = s._0;
            out._3 = s._1;
            return out;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            out._3 = "waitpid failed";   /* e.g. ECHILD — no children left */
            return out;
        }
        /* got == 0: no child ready yet. */
        if (os_now_monotonic_ms_raw() >= deadline) { out._2 = 1; return out; }
        struct timespec ts = { 0, 20 * 1000 * 1000 }; /* 20ms */
        nanosleep(&ts, NULL);
    }
}

/* Convenience: spawn + drain pipe + wait. Reads the back-channel
 * to EOF (driven by the child closing fd 3 or exiting) while the
 * child runs, then waits for the child to exit. Returns the
 * channel payload as a string, the exit code, and an error string.
 *
 * No deadlock risk: the read loop drains continuously, so the
 * pipe buffer never fills regardless of payload size. */
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    /* Delegate to os_run_pipe_raw for the spawn, then drain + wait. */
    _tuple_int_int_string spawn = os_run_pipe_raw(prog, argv_list, env_list);
    if (spawn._2 && spawn._2[0]) {
        out._2 = spawn._2;
        return out;
    }
    int read_fd = spawn._0;
    int pid = spawn._1;

    size_t cap = 4096;
    size_t len = 0;
    /* #462: cap the drained IPC-channel payload (see os_run_capture_raw).
     * `result` is adopted into out._0; error paths free through the cap. */
    char* result = (char*)aether_caps_malloc(cap);
    if (!result) {
        close(read_fd);
        int st = 0;
        aether_waitpid((pid_t)pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(result, cap);
            close(read_fd);
            int st = 0;
            aether_waitpid((pid_t)pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t old_cap = cap;
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)aether_caps_realloc(result, old_cap, cap);
            if (!bigger) {
                aether_caps_free(result, old_cap);
                close(read_fd);
                int st = 0;
                aether_waitpid((pid_t)pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(read_fd);

    string_release(out._0);
    out._0 = (const char*)string_adopt_caps_buffer(result, len, cap);
    int st = 0;
    if (aether_waitpid((pid_t)pid, &st, 0) < 0) {
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    {
        _tuple_int_string s = posix_status_to_tuple(st);
        out._1 = s._0;
        out._2 = s._1;
    }
    return out;
}

/* ============================================================
 * Process-supervision primitives — process groups, killpg, bounded
 * waits, and a one-call supervised run. Filed by aeb
 * (aeb-process-supervision-primitives.md): the floor any build/test
 * orchestrator hits — run a child as its own process group, forward
 * interactive signals to it, enforce a wall-clock timeout, then
 * group-reap anything the child leaked. This is the POSIX branch
 * (fork + setpgid + killpg + waitpid); the Windows branch further
 * down maps the same shape onto a Job Object.
 * ============================================================ */

/* Send `sig` to `pid`. Thin POSIX kill(2) wrapper, returning 0 on
 * success and -1 on failure. The two non-obvious conventions are
 * intentional and documented in the header / module.ae:
 *   - negative pid → signal the whole group abs(pid);
 *   - sig == 0     → existence/permission probe, no delivery.
 * Gated under the "exec" capability bucket: sending a signal is a
 * process-control action of the same blast radius as spawning one. */
int os_kill_raw(int pid, int sig) {
    if (!aether_sandbox_check("exec", "kill")) return -1;
    /* A child os_spawn_to_raw started leads a group: signal all of it. */
    if (pid > 0 && group_has((pid_t)pid)) return kill(-(pid_t)pid, sig) == 0 ? 0 : -1;
    return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}

/* Bounded wait for a single child. Returns (status, timed_out, err):
 *   - status:    WEXITSTATUS on normal exit, 128+signo when killed by
 *                a signal, -1 on timeout or error.
 *   - timed_out: 1 if `secs` elapsed before the child exited (the
 *                child is left running — the caller decides whether to
 *                kill it), 0 otherwise.
 *   - err:       "" on a clean reap or a clean timeout; non-empty on a
 *                waitpid error / invalid pid.
 * secs <= 0 means "block indefinitely" (equivalent to os_wait_pid_raw
 * but with the timed_out slot always 0). Poll cadence is 20ms against
 * a CLOCK_MONOTONIC deadline so a wall-clock jump can't distort it. */
_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    _tuple_int_int_string out = { -1, 0, "" };
    if (pid <= 0) { out._2 = "invalid pid"; return out; }

    int st = 0;
    if (secs <= 0) {
        while (aether_waitpid((pid_t)pid, &st, 0) < 0) {
            if (errno != EINTR) { out._2 = "waitpid failed"; return out; }
        }
    } else {
        int64_t deadline = os_now_monotonic_ms_raw() + (int64_t)secs * 1000;
        int reaped = 0;
        for (;;) {
            pid_t r = aether_waitpid((pid_t)pid, &st, WNOHANG);
            if (r == (pid_t)pid) { reaped = 1; break; }
            if (r < 0) {
                if (errno == EINTR) continue;
                out._2 = "waitpid failed";
                return out;
            }
            if (os_now_monotonic_ms_raw() >= deadline) { out._1 = 1; return out; }
            struct timespec ts = { 0, 20 * 1000 * 1000 }; /* 20ms */
            nanosleep(&ts, NULL);
        }
        (void)reaped;
    }
    group_reaped((pid_t)pid);

    {
        _tuple_int_string s = posix_status_to_tuple(st);
        out._0 = s._0;
        out._2 = s._1;
    }
    return out;
}

/* Signal-forwarding state for os_run_supervised_raw. A supervised run
 * installs SIGINT/SIGTERM handlers that forward the signal to the
 * child's process group, so a Ctrl-C at the controlling terminal tears
 * down the whole build rather than orphaning the parent's children.
 * Single-slot global: v1 supports one supervised run forwarding at a
 * time (nested/concurrent supervised runs in threads would race this).
 * The handler does nothing but kill(2), which is async-signal-safe. */
static volatile sig_atomic_t g_supervised_pgid = 0;

static void aether_os_forward_signal(int sig) {
    pid_t pg = (pid_t)g_supervised_pgid;
    if (pg > 0) kill(-pg, sig);
}

/* One-call supervised run. Encodes the bash job-control pattern that
 * every build/test orchestrator reinvents:
 *
 *   - new_process_group: run the child in its own process group
 *     (pgid == child pid) so signals and reaping can target the whole
 *     subtree, not just the immediate child.
 *   - forward_signals: install parent SIGINT/SIGTERM handlers that
 *     forward to the child's group (requires new_process_group).
 *   - timeout_secs > 0: TERM the group on deadline, grace 5s, then
 *     KILL; report exit_code 124 (GNU `timeout` convention).
 *   - reap_group: after the child exits, TERM→grace→KILL the group to
 *     clean up anything the build leaked (a stray test server, a
 *     backgrounded helper), then reap the now-dead members (requires
 *     new_process_group).
 *
 * Returns (exit_code, outcome) where outcome is one of:
 *   "exited"    — child exited normally; exit_code is its status.
 *   "signalled" — child was killed by a signal; exit_code is 128+signo.
 *   "timeout"   — deadline hit, group torn down; exit_code is 124.
 *   "error"     — wait failure; exit_code is -1.
 * A child that could not be started is exit_code -1 with the reason as the
 * outcome, as on Windows: "program not found", "permission denied", "not an
 * executable" for a failed exec (#3170), "fork failed" and the like.
 */
_tuple_int_string os_run_supervised_raw(const char* prog, void* argv_list, void* env_list,
                                        int new_process_group, int forward_signals,
                                        int timeout_secs, int reap_group) {
    _tuple_int_string out = { -1, "error" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    int errp[2];
    if (aether_pipe_cloexec(errp) != 0) { out._1 = "pipe failed"; return out; }
    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(errp[0]); close(errp[1]);
        out._1 = xerr;
        return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(errp[0]); close(errp[1]);
        aether_exec_release(&x);
        out._1 = "fork failed";
        return out;
    }
    if (pid == 0) {
        aether_child_reset_signals();
        aether_child_keep_stdio();
        /* Child: become our own process-group leader before exec so the
         * parent can address the whole subtree as -pid. Setting it in
         * both child and parent (below) closes the fork/exec race. */
        if (new_process_group) setpgid(0, 0);
        close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    /* Parent. */
    if (new_process_group) setpgid(pid, pid); /* harmless if child won the race */
    aether_exec_release(&x);
    /* A child that could not start is (-1, reason), as on Windows (#3170). */
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        aether_reap_failed(pid);
        out._1 = aether_exec_failure(exec_err);
        return out;
    }

    /* The group we can address with a negative pid. 0 means "no group"
     * (new_process_group was off) — signal/reap-group steps are skipped. */
    pid_t group = new_process_group ? pid : 0;

    struct sigaction old_int, old_term, sa;
    int installed = 0;
    if (forward_signals && group) {
        g_supervised_pgid = pid;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = aether_os_forward_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART: let waitpid return EINTR so we re-loop */
        sigaction(SIGINT, &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);
        installed = 1;
    }

    int st = 0;
    const char* outcome = "exited";
    int exit_code = -1;
    int handled = 0; /* 1 once outcome/exit_code are final (timeout path) */

    if (timeout_secs > 0) {
        int64_t deadline = os_now_monotonic_ms_raw() + (int64_t)timeout_secs * 1000;
        int reaped = 0;
        for (;;) {
            pid_t r = aether_waitpid(pid, &st, WNOHANG);
            if (r == pid) { reaped = 1; break; }
            if (r < 0) {
                if (errno == EINTR) continue;
                break; /* ECHILD or similar — fall through to status decode */
            }
            if (os_now_monotonic_ms_raw() >= deadline) break;
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            /* Deadline hit: TERM the group (or the lone child), give it
             * 5s to unwind, then KILL and definitely reap. */
            pid_t target = group ? -group : pid;
            kill(target, SIGTERM);
            int64_t grace = os_now_monotonic_ms_raw() + 5000;
            while (os_now_monotonic_ms_raw() < grace) {
                if (aether_waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
                struct timespec ts = { 0, 20 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            if (!reaped) {
                kill(target, SIGKILL);
                aether_waitpid(pid, &st, 0);
            }
            outcome = "timeout";
            exit_code = 124;
            handled = 1;
        }
    } else {
        while (aether_waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR) { outcome = "error"; exit_code = -1; handled = 1; break; }
        }
    }

    if (!handled) {
        if (WIFEXITED(st)) {
            exit_code = WEXITSTATUS(st);
            outcome = "exited";
        } else if (WIFSIGNALED(st)) {
            exit_code = 128 + WTERMSIG(st);
            outcome = "signalled";
        } else {
            exit_code = -1;
            outcome = "error";
        }
    }

    /* Group-reap anything the build leaked into the group. The child
     * itself is already reaped; these waitpid(-group) calls collect any
     * other group members we just signalled so they don't linger as
     * zombies. */
    if (reap_group && group) {
        kill(-group, SIGTERM);
        struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms grace */
        nanosleep(&ts, NULL);
        kill(-group, SIGKILL);
        for (;;) {
            int st2 = 0;
            pid_t r = aether_waitpid(-group, &st2, WNOHANG);
            if (r <= 0) break;
        }
    }

    if (installed) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        g_supervised_pgid = 0;
    }

    out._0 = exit_code;
    out._1 = outcome;
    return out;
}

typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;

/* Append `n` bytes to a doubling buffer, keeping room for a trailing
 * NUL. Returns 1 on success, 0 on allocation failure (buffer left
 * intact for the caller to free). Used by os_run_full_raw's poll
 * loop to accumulate stdout and stderr independently. */
static int aether_os_buf_append(char** buf, size_t* len, size_t* cap,
                                const char* data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (*len + n + 1 > nc) nc *= 2;
        /* #462: cap the captured stream (caller os_run_full_raw bounds
         * stdout/stderr through this). realloc(NULL, ...) on the first
         * grow behaves as malloc; the old size is *cap (0 initially). */
        char* bigger = (char*)aether_caps_realloc(*buf, *cap, nc);
        if (!bigger) return 0;
        *buf = bigger;
        *cap = nc;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 1;
}

static void aether_os_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Full three-way child stdio: feed `stdin_data` (exactly `stdin_len`
 * bytes — binary-safe, NUL bytes included) to the child's stdin, and
 * capture its stdout and stderr SEPARATELY. Returns
 * (stdout, stderr, exit_code, err):
 *   - stdout / stderr: @heap counted captures the caller owns; "" if
 *     empty. They carry their length, so an embedded NUL is kept.
 *   - exit_code: WEXITSTATUS on normal exit, 128+signo if the child
 *     was killed by a signal, -1 when it could not be started or on an
 *     IO error (err non-empty then).
 *   - err: "" on a successful spawn (regardless of exit code);
 *     non-empty only when the fork/exec/pipe couldn't run; a failed
 *     exec gives its reason, "program not found" for one (#3170).
 *
 * A poll(2) loop pumps all three fds at once, so the child can write
 * unbounded stdout/stderr while we stream its stdin — no classic
 * "fill the pipe buffer and deadlock" hazard. `stdin_len <= 0` gives
 * the child an immediately-closed (empty) stdin rather than the
 * parent's, so it can't block reading a terminal. */
_tuple_string_string_int_string os_run_full_raw(const char* prog, void* argv_list, void* env_list,
                                                const char* stdin_data, int stdin_len) {
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "" };
    if (!prog) { out._3 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._3 = "denied by sandbox"; return out; }

    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (aether_pipe_cloexec(in_pipe) != 0) { out._3 = "pipe failed"; return out; }
    if (aether_pipe_cloexec(out_pipe) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        out._3 = "pipe failed"; return out;
    }
    if (aether_pipe_cloexec(err_pipe) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        out._3 = "pipe failed"; return out;
    }
    int errp[2] = { -1, -1 };
    if (aether_pipe_cloexec(errp) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        out._3 = "pipe failed"; return out;
    }

    AetherExec x;
    const char* xerr = aether_exec_prepare(&x, prog, argv_list, env_list);
    if (xerr) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        close(errp[0]); close(errp[1]);
        out._3 = xerr; return out;
    }

    pid_t pid = aether_os_fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        close(errp[0]); close(errp[1]);
        aether_exec_release(&x);
        out._3 = "fork failed"; return out;
    }
    if (pid == 0) {
        /* Child: wire stdin/stdout/stderr to the pipes, close the rest. */
        aether_child_reset_signals();
        aether_child_keep_stdio();
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]); close(errp[0]);
        int ef = aether_child_lift_fd(errp[1]);
        int in_fd = aether_child_lift_fd(in_pipe[0]);
        int out_fd = aether_child_lift_fd(out_pipe[1]);
        int err_fd = aether_child_lift_fd(err_pipe[1]);
        if (aether_child_fd(in_fd, 0) < 0)  aether_child_exec_failed(ef);
        if (aether_child_fd(out_fd, 1) < 0) aether_child_exec_failed(ef);
        if (aether_child_fd(err_fd, 2) < 0) aether_child_exec_failed(ef);
        close(in_fd); close(out_fd); close(err_fd);
        aether_child_exec(&x);
        aether_child_exec_failed(ef);
    }
    /* Parent. */
    aether_exec_release(&x);
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    /* A program that could not start reads as Windows has it (#3170). */
    int exec_err = aether_exec_errno(errp);
    if (exec_err) {
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        aether_reap_failed(pid);
        out._3 = aether_exec_failure(exec_err);
        return out;
    }
    int in_fd = in_pipe[1], o_fd = out_pipe[0], e_fd = err_pipe[0];

    size_t in_total = (stdin_data && stdin_len > 0) ? (size_t)stdin_len : 0;
    size_t in_off = 0;
    int in_open = 1;
    if (in_total == 0) { close(in_fd); in_open = 0; } /* empty stdin → immediate EOF */

    aether_os_set_nonblock(o_fd);
    aether_os_set_nonblock(e_fd);
    if (in_open) aether_os_set_nonblock(in_fd);

    char* obuf = NULL; size_t olen = 0, ocap = 0;
    char* ebuf = NULL; size_t elen = 0, ecap = 0;
    int o_open = 1, e_open = 1, oom = 0;
    char tmp[4096];

    while (o_open || e_open || in_open) {
        struct pollfd pfds[3];
        int nf = 0, oi = -1, ei = -1, ii = -1;
        if (o_open)  { pfds[nf].fd = o_fd;  pfds[nf].events = POLLIN;  pfds[nf].revents = 0; oi = nf++; }
        if (e_open)  { pfds[nf].fd = e_fd;  pfds[nf].events = POLLIN;  pfds[nf].revents = 0; ei = nf++; }
        if (in_open) { pfds[nf].fd = in_fd; pfds[nf].events = POLLOUT; pfds[nf].revents = 0; ii = nf++; }

        if (poll(pfds, nf, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (oi >= 0 && pfds[oi].revents) {
            for (;;) {
                ssize_t n = read(o_fd, tmp, sizeof tmp);
                if (n > 0) { if (!aether_os_buf_append(&obuf, &olen, &ocap, tmp, (size_t)n)) { oom = 1; o_open = 0; break; } continue; }
                if (n == 0) { o_open = 0; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { if (pfds[oi].revents & (POLLHUP | POLLERR)) o_open = 0; break; }
                o_open = 0; break;
            }
        }
        if (ei >= 0 && pfds[ei].revents) {
            for (;;) {
                ssize_t n = read(e_fd, tmp, sizeof tmp);
                if (n > 0) { if (!aether_os_buf_append(&ebuf, &elen, &ecap, tmp, (size_t)n)) { oom = 1; e_open = 0; break; } continue; }
                if (n == 0) { e_open = 0; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { if (pfds[ei].revents & (POLLHUP | POLLERR)) e_open = 0; break; }
                e_open = 0; break;
            }
        }
        if (ii >= 0 && pfds[ii].revents) {
            size_t remain = in_total - in_off;
            size_t chunk = remain > 65536 ? 65536 : remain;
            ssize_t w = write(in_fd, stdin_data + in_off, chunk);
            if (w > 0) {
                in_off += (size_t)w;
                if (in_off >= in_total) { close(in_fd); in_open = 0; }
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                /* EPIPE etc — child closed stdin early. Stop feeding. */
                close(in_fd); in_open = 0;
            }
            if (oom) break;
        }
        if (oom) break;
    }

    close(o_fd);
    close(e_fd);
    if (in_open) close(in_fd);

    int st = 0;
    while (aether_waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            /* #462: free the capped capture buffers with their live
             * sizes (caps_free(NULL,0) is a no-op when never grown). */
            aether_caps_free(obuf, ocap); aether_caps_free(ebuf, ecap);
            out._2 = -1;
            out._3 = "waitpid failed";
            return out;
        }
    }

    if (oom) {
        aether_caps_free(obuf, ocap); aether_caps_free(ebuf, ecap);
        out._2 = -1;
        out._3 = "alloc failed";
        return out;
    }

    /* NUL-terminate the captures and hand them to the caller as counted
     * strings, releasing the empty placeholders (#420 v2). */
    if (obuf) {
        obuf[olen] = '\0';
        string_release(out._0);
        out._0 = (const char*)string_adopt_caps_buffer(obuf, olen, ocap);
    }
    if (ebuf) {
        ebuf[elen] = '\0';
        string_release(out._1);
        out._1 = (const char*)string_adopt_caps_buffer(ebuf, elen, ecap);
    }

    if (WIFEXITED(st))        out._2 = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) out._2 = 128 + WTERMSIG(st);
    else                      out._2 = -1;
    return out;
}

/* Child side: returns the parent-channel fd if the parent spawned
 * us via os_run_pipe* (i.e. AETHER_IPC_FD is set and the named fd
 * is open writable); -1 otherwise.
 *
 * Implemented in std/ipc/aether_ipc.c — declared here only so the
 * Aether module.ae's `extern ipc_parent_channel_raw()` resolves
 * during link. */

#else // _WIN32

// -----------------------------------------------------------------
// Windows process-exec backend: CreateProcessW with argv-style launch.
//
// The POSIX branch uses execvp directly. On Windows, CreateProcessW
// takes a single UTF-16 command-line string with the very particular
// escaping rules consumed by the C runtime's argv parser. The helpers
// below build that string correctly — the classic "Everyone quotes
// command line arguments the wrong way" rules are applied verbatim.
// -----------------------------------------------------------------

// UTF-8 → UTF-16. Caller frees. Returns NULL on conversion failure.
static wchar_t* utf8_to_wide(const char* utf8) {
    if (!utf8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wlen) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

// Simple growing wide-string buffer for assembling a command line.
typedef struct {
    wchar_t* data;
    size_t   len;
    size_t   cap;
    int      oom;
} WBuf;

static void wbuf_init(WBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }

static int wbuf_reserve(WBuf* b, size_t extra) {
    if (b->oom) return 0;
    size_t need = b->len + extra + 1;
    if (need > b->cap) {
        size_t new_cap = aether_buf_grow_capacity(b->cap, 256, need, sizeof(wchar_t));
        if (!new_cap) { b->oom = 1; return 0; }
        wchar_t* nd = (wchar_t*)realloc(b->data, sizeof(wchar_t) * new_cap);
        if (!nd) { b->oom = 1; return 0; }
        b->data = nd;
        b->cap = new_cap;
    }
    return 1;
}

static void wbuf_append(WBuf* b, const wchar_t* s) {
    if (!s) return;
    size_t slen = wcslen(s);
    if (!wbuf_reserve(b, slen)) return;
    memcpy(b->data + b->len, s, sizeof(wchar_t) * slen);
    b->len += slen;
    b->data[b->len] = L'\0';
}

static void wbuf_append_char(WBuf* b, wchar_t c) {
    if (!wbuf_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = L'\0';
}

// Escape and append `arg` for CRT argv parsing. Rules from MSFT:
// https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
//   - If the arg contains no whitespace and no '"', append verbatim.
//   - Otherwise wrap in quotes. Inside the quotes:
//       * Any run of N backslashes followed by a '"' becomes 2N+1
//         backslashes followed by \".
//       * Any run of N backslashes at the very end of the arg (right
//         before the closing quote) becomes 2N backslashes.
//       * Other backslashes are passed through verbatim.
static void append_escaped_arg(WBuf* b, const wchar_t* arg) {
    int needs_quote = 0;
    if (*arg == L'\0') {
        needs_quote = 1;
    } else {
        for (const wchar_t* p = arg; *p; p++) {
            if (*p == L' ' || *p == L'\t' || *p == L'"') {
                needs_quote = 1;
                break;
            }
        }
    }

    if (!needs_quote) {
        wbuf_append(b, arg);
        return;
    }

    wbuf_append_char(b, L'"');

    const wchar_t* p = arg;
    while (*p) {
        size_t backslashes = 0;
        while (*p == L'\\') { backslashes++; p++; }

        if (*p == L'\0') {
            // Trailing backslashes before closing quote — double them.
            for (size_t i = 0; i < backslashes * 2; i++) wbuf_append_char(b, L'\\');
            break;
        } else if (*p == L'"') {
            // N backslashes + '"' → 2N+1 backslashes + \"
            for (size_t i = 0; i < backslashes * 2 + 1; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, L'"');
            p++;
        } else {
            for (size_t i = 0; i < backslashes; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, *p);
            p++;
        }
    }

    wbuf_append_char(b, L'"');
}

// Build the command-line string for CreateProcessW. argv[0] is the
// program name — we use `prog` if the caller's list is empty, else the
// first list entry. Caller frees.
static wchar_t* build_command_line(const char* prog, void* argv_list) {
    WBuf b; wbuf_init(&b);

    /* Match the POSIX argv convention used by build_argv_array: the program
     * is always prog, and argv_list holds the caller's arguments starting at
     * position 0. The earlier shape — treating argv_list[0] as the program
     * when the list was non-empty — meant os.run("cmd", ["/c","echo","x"])
     * produced the command line `/c echo x` and tried to spawn a program
     * named `/c`. Discovered chasing the second half of issue #706 once
     * lpApplicationName=NULL exposed cmdline[0] to CreateProcessW's parser. */
    wchar_t* wprog = utf8_to_wide(prog);
    if (!wprog) { free(b.data); return NULL; }
    append_escaped_arg(&b, wprog);
    free(wprog);

    int n = argv_list ? list_size(argv_list) : 0;
    for (int i = 0; i < n; i++) {
        const char* item = aether_string_data(list_get_raw(argv_list, i));
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append_char(&b, L' ');
        append_escaped_arg(&b, w);
        free(w);
    }

    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

/* Does env entry `e` ("NAME=...") name `name`? Windows names ignore case. */
static int win_env_names(const wchar_t* e, const wchar_t* name) {
    size_t n = wcslen(name);
    return _wcsnicmp(e, name, n) == 0 && e[n] == L'=';
}

/* os.run_stdio's environment block: the list given, or this process's
 * environment, without AETHER_IPC_KIND or AETHER_IPC_FD, then
 * AETHER_IPC_KIND=stdio. Never NULL for "inherit": the marker has to be
 * added either way. NULL only when out of memory. */
static wchar_t* build_stdio_environ_block(void* env_list) {
    WBuf b; wbuf_init(&b);
    if (env_list) {
        int n = list_size(env_list);
        for (int i = 0; i < n; i++) {
            const char* item = aether_string_data(list_get_raw(env_list, i));
            if (!item) continue;
            wchar_t* w = utf8_to_wide(item);
            if (!w) { free(b.data); return NULL; }
            if (!win_env_names(w, L"AETHER_IPC_KIND") && !win_env_names(w, L"AETHER_IPC_FD")) {
                wbuf_append(&b, w);
                wbuf_append_char(&b, L'\0');
            }
            free(w);
        }
    } else {
        wchar_t* all = GetEnvironmentStringsW();
        if (!all) return NULL;
        for (const wchar_t* e = all; *e; e += wcslen(e) + 1) {
            if (win_env_names(e, L"AETHER_IPC_KIND") || win_env_names(e, L"AETHER_IPC_FD")) continue;
            wbuf_append(&b, e);
            wbuf_append_char(&b, L'\0');
        }
        FreeEnvironmentStringsW(all);
    }
    wbuf_append(&b, L"AETHER_IPC_KIND=stdio");
    wbuf_append_char(&b, L'\0');
    wbuf_append_char(&b, L'\0');
    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

// Build a UTF-16 environment block from a list of "KEY=VALUE" strings.
// Format: key1=val1\0key2=val2\0\0. Returns NULL if env_list is NULL
// (inherit parent env) or on allocation failure.
static wchar_t* build_environ_block(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    WBuf b; wbuf_init(&b);

    for (int i = 0; i < n; i++) {
        const char* item = aether_string_data(list_get_raw(env_list, i));
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append(&b, w);
        wbuf_append_char(&b, L'\0');
        free(w);
    }
    wbuf_append_char(&b, L'\0');
    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

/* ============================================================
 * #2171: resolving the program, and launching a batch file safely.
 *
 * Every launch used to pass lpApplicationName = NULL and let CreateProcessW
 * find cmdline[0] itself (the #706 fix for PATH lookup). That search is
 * not POSIX's: it tries the application's own directory and the CURRENT
 * directory before PATH, so os.run_capture("git", ...) run from a directory
 * someone else can write to ran their .\git.exe. And a .bat / .cmd it found
 * ran under cmd.exe, which reads the command line by its own rules, not the
 * C runtime's that append_escaped_arg quotes for, so an argument such as
 * `x" & calc & "` ran calc (the BatBadBut class, CVE-2024-24576).
 *
 * Now the program is resolved here. A name with a path separator is used as
 * written. A bare name is looked up everywhere CreateProcessW looked EXCEPT
 * the current directory -- the application's own directory, the system
 * directory, the Windows directory, then PATH -- which is Microsoft's own
 * mitigation (NoDefaultCurrentDirectoryInExePath) and keeps an app that
 * ships helpers beside itself working. A name without an extension takes
 * PATHEXT's, limited to the four CreateProcessW can start (.com .exe .bat
 * .cmd): PATHEXT also lists .js, .vbs, .py and the like, and a tool.py
 * earlier on PATH must not hide the tool.exe after it. The absolute result
 * is passed as lpApplicationName, so CreateProcessW searches nothing. A
 * batch file is launched through the system cmd.exe explicitly, with every
 * argument quoted for cmd -- and refused when it holds a character cmd
 * would still interpret inside quotes (`"`, `%`, a line break).
 * ============================================================ */

/* Why the last win_prepare_launch on this thread failed, for the error
 * slot callers return. Static strings only. */
static _Thread_local const char* g_win_launch_err = NULL;

static const char* win_launch_error(void) {
    return g_win_launch_err ? g_win_launch_err : "spawn failed";
}

static int win_is_file(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* `dir` + `\` + `name` + `ext` if it names a file, else NULL. */
static wchar_t* win_try_candidate(const wchar_t* dir, size_t dir_len,
                                  const wchar_t* name, const wchar_t* ext,
                                  size_t ext_len) {
    size_t name_len = wcslen(name);
    size_t n = dir_len + 1 + name_len + ext_len + 1;
    wchar_t* c = (wchar_t*)malloc(sizeof(wchar_t) * n);
    if (!c) return NULL;
    size_t k = 0;
    if (dir_len) {
        memcpy(c, dir, sizeof(wchar_t) * dir_len);
        k = dir_len;
        if (c[k - 1] != L'\\' && c[k - 1] != L'/') c[k++] = L'\\';
    }
    memcpy(c + k, name, sizeof(wchar_t) * name_len);
    k += name_len;
    if (ext_len) { memcpy(c + k, ext, sizeof(wchar_t) * ext_len); k += ext_len; }
    c[k] = L'\0';
    if (win_is_file(c)) return c;
    free(c);
    return NULL;
}

/* Can CreateProcessW start a file with this extension (this launcher
 * running .bat / .cmd through cmd.exe)? */
static int win_launchable_ext(const wchar_t* ext, size_t len) {
    static const wchar_t* ok[] = { L".com", L".exe", L".bat", L".cmd", NULL };
    for (int i = 0; ok[i]; i++) {
        if (len == 4 && _wcsnicmp(ext, ok[i], 4) == 0) return 1;
    }
    return 0;
}

/* Try `name` in one directory: as written when it already has an
 * extension, else with each launchable extension PATHEXT lists, in its
 * order. */
static wchar_t* win_try_dir(const wchar_t* dir, size_t dir_len,
                            const wchar_t* name, int has_ext,
                            const wchar_t* pathext) {
    if (has_ext) return win_try_candidate(dir, dir_len, name, L"", 0);
    const wchar_t* e = pathext;
    while (*e) {
        const wchar_t* end = wcschr(e, L';');
        size_t len = end ? (size_t)(end - e) : wcslen(e);
        if (len && win_launchable_ext(e, len)) {
            wchar_t* hit = win_try_candidate(dir, dir_len, name, e, len);
            if (hit) return hit;
        }
        if (!end) break;
        e = end + 1;
    }
    return NULL;
}

/* The directory this process's executable is in, or NULL. */
static wchar_t* win_app_dir(void) {
    DWORD cap = MAX_PATH;
    for (int tries = 0; tries < 6; tries++) {
        wchar_t* buf = (wchar_t*)malloc(sizeof(wchar_t) * cap);
        if (!buf) return NULL;
        DWORD got = GetModuleFileNameW(NULL, buf, cap);
        if (got == 0) { free(buf); return NULL; }
        if (got < cap) {
            wchar_t* slash = wcsrchr(buf, L'\\');
            if (!slash) { free(buf); return NULL; }
            *slash = L'\0';
            return buf;
        }
        free(buf);
        cap *= 2;
    }
    return NULL;
}

/* A copy of environment variable `name`, or NULL when it is unset. */
static wchar_t* win_getenv_copy(const wchar_t* name) {
    DWORD need = GetEnvironmentVariableW(name, NULL, 0);
    if (need == 0) return NULL;
    wchar_t* v = (wchar_t*)malloc(sizeof(wchar_t) * need);
    if (!v) return NULL;
    DWORD got = GetEnvironmentVariableW(name, v, need);
    if (got == 0 || got >= need) { free(v); return NULL; }
    return v;
}

/* The absolute path of the program `prog` names, resolved as execvp does
 * (see the block comment above). NULL, with g_win_launch_err set, when
 * there is none. */
static wchar_t* win_resolve_program(const char* prog) {
    wchar_t* wprog = utf8_to_wide(prog);
    if (!wprog) { g_win_launch_err = "out of memory"; return NULL; }

    const wchar_t* base = wprog;
    for (const wchar_t* p = wprog; *p; p++) {
        if (*p == L'\\' || *p == L'/' || *p == L':') base = p + 1;
    }
    int has_sep = (base != wprog);
    int has_ext = wcschr(base, L'.') != NULL;

    wchar_t* pathext = win_getenv_copy(L"PATHEXT");
    const wchar_t* exts = (pathext && *pathext) ? pathext : L".COM;.EXE;.BAT;.CMD";

    wchar_t* found = NULL;
    if (has_sep) {
        /* A path the caller named (relative ones against the current
         * directory). Without an extension, PATHEXT's come first, as
         * CreateProcessW appended .exe; then the file exactly as named, so
         * an extensionless program at an explicit path still starts (Wine
         * runs a Unix binary that way, os_execv("/bin/echo", ...)). */
        found = win_try_dir(L"", 0, wprog, has_ext, exts);
        if (!found && !has_ext) found = win_try_candidate(L"", 0, wprog, L"", 0);
    } else {
        /* CreateProcessW's own order, less the current directory. */
        wchar_t* app = win_app_dir();
        if (app) {
            found = win_try_dir(app, wcslen(app), wprog, has_ext, exts);
            free(app);
        }
        wchar_t sysdir[MAX_PATH];
        UINT sl = found ? 0 : GetSystemDirectoryW(sysdir, MAX_PATH);
        if (sl > 0 && sl < MAX_PATH) found = win_try_dir(sysdir, sl, wprog, has_ext, exts);
        UINT wl = found ? 0 : GetWindowsDirectoryW(sysdir, MAX_PATH);
        if (wl > 0 && wl < MAX_PATH) found = win_try_dir(sysdir, wl, wprog, has_ext, exts);
    }
    if (!found && !has_sep) {
        wchar_t* path = win_getenv_copy(L"PATH");
        if (path) {
            const wchar_t* d = path;
            while (!found) {
                const wchar_t* end = wcschr(d, L';');
                size_t len = end ? (size_t)(end - d) : wcslen(d);
                /* A quoted entry ("C:\Program Files\x") is legal in PATH. */
                const wchar_t* dir = d;
                if (len >= 2 && dir[0] == L'"' && dir[len - 1] == L'"') { dir++; len -= 2; }
                if (len) found = win_try_dir(dir, len, wprog, has_ext, exts);
                if (!end) break;
                d = end + 1;
            }
            free(path);
        }
    }
    free(pathext);
    free(wprog);
    if (!found) { g_win_launch_err = "program not found"; return NULL; }

    /* Absolute, so CreateProcessW has nothing left to search. */
    DWORD need = GetFullPathNameW(found, 0, NULL, NULL);
    if (need == 0) { free(found); g_win_launch_err = "program not found"; return NULL; }
    wchar_t* full = (wchar_t*)malloc(sizeof(wchar_t) * need);
    if (!full) { free(found); g_win_launch_err = "out of memory"; return NULL; }
    DWORD got = GetFullPathNameW(found, need, full, NULL);
    free(found);
    if (got == 0 || got >= need) { free(full); g_win_launch_err = "program not found"; return NULL; }
    return full;
}

static int win_is_batch(const wchar_t* path) {
    size_t n = wcslen(path);
    if (n < 4) return 0;
    return _wcsicmp(path + n - 4, L".bat") == 0 || _wcsicmp(path + n - 4, L".cmd") == 0;
}

/* Append one value for cmd.exe: quoted when it holds anything cmd splits
 * or acts on outside quotes. Returns 0 when it holds something cmd still
 * interprets INSIDE quotes -- `"` ends the quoting, `%` expands a variable,
 * a line break ends the command -- which no quoting makes safe. */
static int win_append_cmd_arg(WBuf* b, const wchar_t* v) {
    for (const wchar_t* p = v; *p; p++) {
        if (*p == L'"' || *p == L'%' || *p == L'\r' || *p == L'\n') return 0;
    }
    int quote = (*v == L'\0') || wcspbrk(v, L" \t&|<>^(),;=@!") != NULL;
    if (quote) wbuf_append_char(b, L'"');
    wbuf_append(b, v);
    if (quote) wbuf_append_char(b, L'"');
    return 1;
}

/* Resolve `prog` and build what CreateProcessW is given: the absolute
 * program in *out_app (caller frees) and the command line (returned,
 * caller frees). NULL, with g_win_launch_err set, on failure. */
static wchar_t* win_prepare_launch(const char* prog, void* argv_list, wchar_t** out_app) {
    g_win_launch_err = NULL;
    *out_app = NULL;
    wchar_t* app = win_resolve_program(prog);
    if (!app) return NULL;

    if (!win_is_batch(app)) {
        wchar_t* cmdline = build_command_line(prog, argv_list);
        if (!cmdline) { free(app); g_win_launch_err = "out of memory"; return NULL; }
        *out_app = app;
        return cmdline;
    }

    /* cmd.exe from the system directory, never from PATH or %ComSpec%. */
    wchar_t sysdir[MAX_PATH];
    UINT sl = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (sl == 0 || sl + 9 >= MAX_PATH) { free(app); g_win_launch_err = "cannot locate cmd.exe"; return NULL; }
    wchar_t* cmd = (wchar_t*)malloc(sizeof(wchar_t) * (sl + 9));
    if (!cmd) { free(app); g_win_launch_err = "out of memory"; return NULL; }
    memcpy(cmd, sysdir, sizeof(wchar_t) * sl);
    memcpy(cmd + sl, L"\\cmd.exe", sizeof(wchar_t) * 9);

    /* "<cmd>" /d /e:ON /v:OFF /s /c ""<script>" arg1 arg2"
     * /d skips AutoRun, /v:OFF keeps `!` literal, and /s makes /c strip
     * exactly the outermost pair of quotes whatever is inside. */
    WBuf b; wbuf_init(&b);
    wbuf_append_char(&b, L'"');
    wbuf_append(&b, cmd);
    wbuf_append(&b, L"\" /d /e:ON /v:OFF /s /c \"");
    /* The script's own path goes through cmd's parser too. */
    for (const wchar_t* p = app; *p; p++) {
        if (*p == L'"' || *p == L'%') {
            free(app); free(cmd); free(b.data);
            g_win_launch_err = "batch file path cannot be passed to cmd.exe safely: "
                               "it contains a quote or %";
            return NULL;
        }
    }
    int ok = 1;
    wbuf_append_char(&b, L'"');
    wbuf_append(&b, app);
    wbuf_append_char(&b, L'"');
    int n = argv_list ? list_size(argv_list) : 0;
    for (int i = 0; ok && i < n; i++) {
        const char* item = aether_string_data(list_get_raw(argv_list, i));
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { b.oom = 1; break; }
        wbuf_append_char(&b, L' ');
        ok = win_append_cmd_arg(&b, w);
        free(w);
    }
    wbuf_append_char(&b, L'"');
    free(app);
    if (!ok) {
        free(cmd); free(b.data);
        g_win_launch_err = "argument cannot be passed to a batch file safely: "
                           "it contains a quote, % or a line break";
        return NULL;
    }
    if (b.oom) { free(cmd); free(b.data); g_win_launch_err = "out of memory"; return NULL; }
    *out_app = cmd;
    return b.data;
}

/* CreateProcessW for a child that inherits exactly `handles` and nothing else
 * (#2917). bInheritHandles = TRUE alone hands the child every inheritable
 * handle the process holds at that moment: a pipe end another thread has
 * just made for its own child is one, and the capture reading that pipe
 * then waits for EOF until this unrelated child exits too.
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST names the handles to pass. Handles that
 * are NULL, invalid, not inheritable or repeated are left out (the list
 * refuses them); with none left the child inherits nothing. */
static BOOL win_create_process_inheriting(wchar_t* app, wchar_t* cmdline, DWORD flags, void* wenv,
                                          const wchar_t* wcwd, STARTUPINFOW* si,
                                          const HANDLE* handles, int n,
                                          PROCESS_INFORMATION* pi) {
    HANDLE list[8];
    int m = 0;
    for (int i = 0; i < n && m < (int)(sizeof list / sizeof list[0]); i++) {
        HANDLE h = handles[i];
        DWORD info = 0;
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        if (!GetHandleInformation(h, &info) || !(info & HANDLE_FLAG_INHERIT)) continue;
        int seen = 0;
        for (int j = 0; j < m; j++) if (list[j] == h) seen = 1;
        if (!seen) list[m++] = h;
    }
    if (m == 0) {
        return CreateProcessW(app, cmdline, NULL, NULL, FALSE, flags, wenv, wcwd, si, pi);
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
    if (!attrs) return FALSE;
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        free(attrs);
        return FALSE;
    }
    BOOL ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        list, (SIZE_T)m * sizeof(HANDLE), NULL, NULL);
    if (ok) {
        STARTUPINFOEXW six;
        memset(&six, 0, sizeof six);
        six.StartupInfo = *si;
        six.StartupInfo.cb = sizeof six;
        six.lpAttributeList = attrs;
        ok = CreateProcessW(app, cmdline, NULL, NULL, TRUE, flags | EXTENDED_STARTUPINFO_PRESENT,
                            wenv, wcwd, &six.StartupInfo, pi);
    }
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    return ok;
}

// Shared launch path. If `capture_stdout` is non-zero we redirect the
// child's stdout to a pipe and read it to completion, into a counted
// string. `out_exit_code` and `out_capture` are optional outputs.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture) {
    /* prog seeds the command line's argv[0] (matching POSIX
     * build_argv_array); the program itself is resolved by
     * win_prepare_launch (#2171). */
    wchar_t* app = NULL;
    wchar_t* cmdline = win_prepare_launch(prog, argv_list, &app);
    if (!cmdline) return -1;

    wchar_t* wenv = build_environ_block(env_list);
    // NULL from build_environ_block when env_list is NULL means "inherit" —
    // that's the correct semantic, not an error.

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES sa;

    if (capture_stdout) {
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            free(app); free(cmdline); free(wenv);
            return -1;
        }
        // The read end must NOT be inherited by the child.
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = write_pipe;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    /* The absolute program win_prepare_launch resolved (#2171): a bare name
     * handed to lpApplicationName would be a literal file in the CWD (#706),
     * and NULL let CreateProcessW search the CWD before PATH. When capturing,
     * the child inherits its own stdio and nothing else (#2917); otherwise
     * nothing. wenv NULL inherits the parent's environment. */
    HANDLE inherit[3] = { si.hStdInput, write_pipe, si.hStdError };
    BOOL ok = win_create_process_inheriting(app, cmdline, flags, wenv, NULL, &si,
                                            inherit, capture_stdout ? 3 : 0, &pi);

    free(app);
    free(cmdline);
    free(wenv);

    if (capture_stdout) {
        // Parent closes the write end so EOF happens when the child exits.
        CloseHandle(write_pipe);
    }

    if (!ok) {
        if (capture_stdout) CloseHandle(read_pipe);
        return -1;
    }

    char* capture = NULL;

    if (capture_stdout) {
        size_t cap = 1024;
        size_t capture_len = 0;
        char* capture_buf = (char*)aether_caps_malloc(cap);
        if (!capture_buf) {
            CloseHandle(read_pipe);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return -1;
        }
        char tmp[1024];
        DWORD got = 0;
        while (ReadFile(read_pipe, tmp, sizeof(tmp), &got, NULL) && got > 0) {
            if (capture_len + got + 1 > cap) {
                size_t old_cap = cap;
                while (capture_len + got + 1 > cap) cap *= 2;
                char* bigger = (char*)aether_caps_realloc(capture_buf, old_cap, cap);
                if (!bigger) {
                    aether_caps_free(capture_buf, old_cap);
                    CloseHandle(read_pipe);
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return -1;
                }
                capture_buf = bigger;
            }
            memcpy(capture_buf + capture_len, tmp, got);
            capture_len += got;
        }
        capture_buf[capture_len] = '\0';
        CloseHandle(read_pipe);
        capture = (char*)string_adopt_caps_buffer(capture_buf, capture_len, cap);
        if (!capture) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return -1;
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (out_exit_code) *out_exit_code = (int)exit_code;
    if (out_capture) *out_capture = capture;
    else string_release(capture);

    return 0;
}

/* ============================================================
 * Non-blocking spawn + reap on Windows (no IPC pipe).
 *
 * win_spawn is win_launch's front half: build the command line (reusing
 * append_escaped_arg's CreateProcessW quoting), CreateProcessW, return the
 * live process HANDLE without waiting. The wait/exit-code/close half lives
 * in os_wait_raw / os_wait_any_raw.
 *
 * The Aether "token" ABI is int, but a HANDLE is a pointer — and a raw
 * dwProcessId can be recycled after exit, so reaping by PID could race onto
 * a different process. We therefore hand out a monotonically-growing int
 * token and keep the live HANDLE in a table keyed by that token. The token
 * is never recycled within a run.
 *
 * os.kill and os.wait_pid_timeout also take a real pid, so a token must not
 * be mistaken for one (#2609). Tokens start at WINPROC_TOKEN_BASE, above any
 * pid Windows hands out (pids come from a handle table, well under 2^26), and
 * a number in that range that the table does not hold is a reaped token:
 * "no such process", never opened as a pid. Counted from 1, a reaped token
 * reached whatever process held that pid now, on an elevated CI runner
 * another test's or the runner's own. */

#define WINPROC_TOKEN_BASE (1 << 30)

/* `job` is the Job Object of a child os_spawn_to_raw started (NULL for
 * os_spawn_raw's): os.kill ends the job, the child's whole tree. */
/* `tree`: an os.spawn_tree_to child, whose job is ended when the token
 * reaps, so nothing it left running outlives the reap. */
typedef struct { int token; HANDLE h; HANDLE job; int tree; } WinProc;
static WinProc  g_winprocs[4096];
static int      g_winproc_count = 0;
static int      g_winproc_next_token = WINPROC_TOKEN_BASE;
/* Initialized statically, so two threads spawning at once cannot both make
 * it, as they could the critical section made on first use that it replaces
 * (#3346). It allocates nothing either: the critical section took a debug
 * record from the heap the first time a spawn had to wait on it. Never taken
 * while held. */
static SRWLOCK g_winproc_srw = SRWLOCK_INIT;

static void winproc_lock(void) { AcquireSRWLockExclusive(&g_winproc_srw); }
static void winproc_unlock(void) { ReleaseSRWLockExclusive(&g_winproc_srw); }

/* Register a live handle, and the child's Job Object if it has one; return
 * its token (or -1 if the table is full). */
static int winproc_register_job(HANDLE h, HANDLE job, int tree) {
    winproc_lock();
    if (g_winproc_count >= (int)(sizeof(g_winprocs)/sizeof(g_winprocs[0]))) {
        winproc_unlock();
        return -1;
    }
    int tok = g_winproc_next_token;
    /* A billion spawns later the range starts over; the table holds 4096
     * live tokens, so the one handed out again is long reaped. */
    g_winproc_next_token = tok == INT_MAX ? WINPROC_TOKEN_BASE : tok + 1;
    g_winprocs[g_winproc_count].token = tok;
    g_winprocs[g_winproc_count].h = h;
    g_winprocs[g_winproc_count].job = job;
    g_winprocs[g_winproc_count].tree = tree;
    g_winproc_count++;
    winproc_unlock();
    return tok;
}

static int winproc_register(HANDLE h) {
    return winproc_register_job(h, NULL, 0);
}

/* The Job Object of a live token, or NULL. Borrowed: the slot owns it. */
static HANDLE winproc_find_job(int token) {
    HANDLE job = NULL;
    winproc_lock();
    for (int i = 0; i < g_winproc_count; i++) {
        if (g_winprocs[i].token == token) { job = g_winprocs[i].job; break; }
    }
    winproc_unlock();
    return job;
}

/* Look up a token's handle without removing it. Returns NULL if absent. */
static HANDLE winproc_find(int token) {
    HANDLE h = NULL;
    winproc_lock();
    for (int i = 0; i < g_winproc_count; i++) {
        if (g_winprocs[i].token == token) { h = g_winprocs[i].h; break; }
    }
    winproc_unlock();
    return h;
}

/* Remove a token's slot (after it has been reaped + CloseHandle'd), closing
 * its Job Object. The job has no KILL_ON_JOB_CLOSE, so anything the child
 * left running goes on, as it does after os_spawn_raw's child exits. */
static void winproc_remove(int token) {
    winproc_lock();
    for (int i = 0; i < g_winproc_count; i++) {
        if (g_winprocs[i].token == token) {
            if (g_winprocs[i].job) {
                if (g_winprocs[i].tree) TerminateJobObject(g_winprocs[i].job, 1);
                CloseHandle(g_winprocs[i].job);
            }
            g_winprocs[i] = g_winprocs[g_winproc_count - 1];
            g_winproc_count--;
            break;
        }
    }
    winproc_unlock();
}

/* Front half of win_launch: spawn without waiting. Returns the process
 * HANDLE, or NULL on failure. hThread is closed here (never needed by the
 * caller); hProcess is the caller's to wait on and close.
 *
 * `stdio` is NULL for a child that inherits nothing (os_spawn_raw), or the
 * child's stdin, stdout and stderr (os_run_stdio_raw): each must be
 * inheritable, and the child inherits those three and nothing else (#2917). */
static HANDLE win_spawn_stdio(const char* prog, void* argv_list, void* env_list,
                              const HANDLE* stdio, const wchar_t* wcwd) {
    wchar_t* app = NULL;
    wchar_t* cmdline = win_prepare_launch(prog, argv_list, &app);
    if (!cmdline) return NULL;
    /* os.run_stdio's child is told so (AETHER_IPC_KIND=stdio). */
    wchar_t* wenv = stdio ? build_stdio_environ_block(env_list) : build_environ_block(env_list);
    if (stdio && !wenv) {
        free(app);
        free(cmdline);
        g_win_launch_err = "env build failed";
        return NULL;
    }

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (stdio) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = stdio[0];
        si.hStdOutput = stdio[1];
        si.hStdError  = stdio[2];
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* Same CreateProcessW contract as win_launch: the resolved absolute
     * program (#2171); inherit parent env when wenv is NULL. Without stdio
     * the handle list is empty and the child inherits no handle at all. */
    BOOL ok = win_create_process_inheriting(app, cmdline, CREATE_UNICODE_ENVIRONMENT, wenv,
                                            wcwd, &si, stdio, stdio ? 3 : 0, &pi);

    free(app);
    free(cmdline);
    free(wenv);

    if (!ok) {
        if (!g_win_launch_err) g_win_launch_err = "spawn failed";
        return NULL;
    }
    CloseHandle(pi.hThread);   /* thread handle never used */
    return pi.hProcess;
}

static HANDLE win_spawn(const char* prog, void* argv_list, void* env_list) {
    return win_spawn_stdio(prog, argv_list, env_list, NULL, NULL);
}

/* Working-directory accessors — Windows. UTF-8 path in / UTF-8 path out,
 * converted through the wide-char helpers so non-ASCII paths survive. */
int os_chdir_raw(const char* path) {
    if (!path) return -1;
    if (!aether_sandbox_check("fs", path)) return -1;
    wchar_t* w = utf8_to_wide(path);
    if (!w) return -1;
    BOOL ok = SetCurrentDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

char* os_getcwd_raw(void) {
    /* GetCurrentDirectoryW(0, NULL) returns the buffer size (incl. NUL). */
    DWORD need = GetCurrentDirectoryW(0, NULL);
    if (need == 0) return NULL;
    /* #462: transient wide-char cwd scratch — alloc'd and freed locally,
     * sized by the (plugin-influenced) path length. */
    size_t w_bytes = sizeof(wchar_t) * (size_t)need;
    wchar_t* w = (wchar_t*)aether_caps_malloc(w_bytes);
    if (!w) return NULL;
    DWORD got = GetCurrentDirectoryW(need, w);
    if (got == 0 || got >= need) { aether_caps_free(w, w_bytes); return NULL; }
    /* `len` counts the NUL, which string_alloc_inline adds itself. */
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    AetherString* out = len > 0 ? string_alloc_inline((size_t)len - 1) : NULL;
    if (out && WideCharToMultiByte(CP_UTF8, 0, w, -1, out->data, len, NULL, NULL) <= 0) {
        string_release(out);
        out = NULL;
    }
    aether_caps_free(w, w_bytes);
    return (char*)out;
}

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    return exit_code;
}

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        return NULL;
    }
    // Caller conventions: on non-zero exit the POSIX branch still
    // returns the captured stdout so the caller can inspect it.
    (void)exit_code;
    return capture;
}

/* Tuple sibling — exposes exit status. See the POSIX branch for the
 * full contract. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) { out._2 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._2 = "denied by sandbox"; return out; }

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        out._2 = win_launch_error();
        return out;
    }
    string_release(out._0);
    out._0 = capture;
    out._1 = exit_code;
    /* err stays "" — Win32 win_launch already returned 0 here, so
     * the spawn itself succeeded. */
    return out;
}

/* std.ipc on Windows: stubs returning unsupported.
 *
 * Windows handle inheritance via STARTUPINFOEX +
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST is the right approach for a
 * future port, but mapping "child sees this at fd 3" requires
 * coordinated _open_osfhandle on both sides plus careful handling
 * of which std handles get inherited. v1 ships POSIX-only;
 * Windows consumers fall back to the file-marker pattern. */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;
typedef struct { int _0; int _1; int _2; const char* _3; } _tuple_int_int_int_string;

/* Reap one token: wait on its handle, read exit code, close, free slot.
 * Shared by os_wait_raw and (per-handle) os_wait_any_raw. */
static _tuple_int_string winproc_reap(int token, HANDLE h) {
    _tuple_int_string out = { -1, "" };
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    winproc_remove(token);
    out._0 = (int)code;
    return out;
}

/* Non-blocking spawn (no pipe). Returns (token, err). */
_tuple_int_string os_spawn_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }
    HANDLE h = win_spawn(prog, argv_list, env_list);
    if (!h) { out._1 = win_launch_error(); return out; }
    int tok = winproc_register(h);
    if (tok < 0) {
        /* Table full — don't leak the process: wait+close it and error. */
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        out._1 = "process table full";
        return out;
    }
    out._0 = tok;
    return out;
}

/* Non-blocking spawn with the child's stdout and stderr written to
 * `out_path` (created or truncated) and its stdin the null device. The
 * child starts suspended, joins a Job Object of its own and then runs, so
 * everything it starts belongs to the job: os_kill_raw on the token ends
 * the whole tree. The file handle and the null device are the only handles
 * it inherits (#2917). Returns (token, err); reap the token with the wait
 * calls, as an os_spawn_raw one. */
static _tuple_int_string win_spawn_to_job(const char* prog, void* argv_list, void* env_list,
                                          const char* out_path, int tree, int* pid_out,
                                          HANDLE* tree_job) {
    _tuple_int_string out = { -1, "" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!out_path) { out._1 = "null output path"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }
    if (!aether_sandbox_check("fs", out_path)) { out._1 = "denied by sandbox"; return out; }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    wchar_t* wpath = utf8_to_wide(out_path);
    if (!wpath) { out._1 = "bad output path"; return out; }
    HANDLE fh = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (fh == INVALID_HANDLE_VALUE) { out._1 = "cannot open the output file"; return out; }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);

    wchar_t* app = NULL;
    wchar_t* cmdline = win_prepare_launch(prog, argv_list, &app);
    if (!cmdline) {
        CloseHandle(fh);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        out._1 = win_launch_error();
        return out;
    }
    wchar_t* wenv = build_environ_block(env_list);

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = fh;
    si.hStdError = fh;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    HANDLE inherit[2] = { fh, nul };
    BOOL ok = win_create_process_inheriting(app, cmdline, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                            wenv, NULL, &si, inherit, 2, &pi);
    free(app);
    free(cmdline);
    free(wenv);
    CloseHandle(fh);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { out._1 = "spawn failed"; return out; }

    /* Joined before it runs a single instruction, so nothing it starts can
     * escape the job. Should the job fail (it does not on Windows 8 and
     * later, where jobs nest), the child still runs, and a kill ends it alone. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (tree && !job) {
        TerminateProcess(pi.hProcess, 1);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        out._1 = "cannot make a job object";
        return out;
    }
    if (job) {
        /* A crash ends the child, or anything it starts, with its exception
         * code: the job sets SEM_NOGPFAULTERRORBOX in every process it holds,
         * so no error-reporting dialog keeps a crashed child alive until a
         * runner's deadline. A child inherits the error mode of the program
         * that starts it otherwise, and that is often 0, the dialog. */
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        /* os.spawn_tree_to: the job ends every process in it when its last
         * handle closes, so when this process ends, however it ends. */
        if (tree) li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
        if (tree) {
            TerminateProcess(pi.hProcess, 1);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            out._1 = "cannot put the child in a job object";
            return out;
        }
    }
    /* The tree's handle holds a job handle of its own, so the slot's can
     * close at reap while the handle still reaches the job. */
    if (tree && !DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), tree_job,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        TerminateJobObject(job, 1);
        CloseHandle(job);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        out._1 = "cannot duplicate the job handle";
        return out;
    }
    if (pid_out) *pid_out = (int)pi.dwProcessId;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    int tok = winproc_register_job(pi.hProcess, job, tree);
    if (tok < 0) {
        if (tree) CloseHandle(*tree_job);
        if (job) { TerminateJobObject(job, 1); CloseHandle(job); }
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        out._1 = "process table full";
        return out;
    }
    out._0 = tok;
    return out;
}

_tuple_int_string os_spawn_to_raw(const char* prog, void* argv_list, void* env_list,
                                  const char* out_path) {
    return win_spawn_to_job(prog, argv_list, env_list, out_path, 0, NULL, NULL);
}

/* os.spawn_tree_to on Windows: os_spawn_to_raw's child in a job that ends
 * every process in it when its last handle closes (this process ending,
 * however it ends, closes them all), when the handle asks (tree_kill), and
 * when the token reaps, which ends whatever the child left running. A
 * process in the job cannot leave it (no BREAKAWAY_OK), so this covers the
 * whole tree. The token is the process table's, as for os_spawn_to_raw. */
typedef struct { void* _0; const char* _1; } _tuple_ptr_string_tree;
typedef struct { int token; int pid; HANDLE job; int asked; } AetherTree;

_tuple_ptr_string_tree os_spawn_tree_to_raw(const char* prog, void* argv_list, void* env_list,
                                            const char* out_path, int grace_ms) {
    (void)grace_ms;   /* Windows has no SIGTERM to give a grace period to */
    _tuple_ptr_string_tree out = { NULL, "" };
    AetherTree* t = (AetherTree*)malloc(sizeof(AetherTree));
    if (!t) { out._1 = "out of memory"; return out; }
    t->job = NULL;
    t->pid = -1;
    t->asked = 0;
    _tuple_int_string r = win_spawn_to_job(prog, argv_list, env_list, out_path, 1, &t->pid, &t->job);
    if (r._0 < 0) {
        free(t);
        out._1 = r._1;
        return out;
    }
    t->token = r._0;
    out._0 = t;
    return out;
}

int os_tree_token_raw(void* tree) { return tree ? ((AetherTree*)tree)->token : -1; }
int os_tree_pid_raw(void* tree) { return tree ? ((AetherTree*)tree)->pid : -1; }

/* End every process in the job; reap the token after. 143, as a POSIX child
 * ended by SIGTERM reads. */
int os_tree_kill_raw(void* tree) {
    AetherTree* t = (AetherTree*)tree;
    if (!t || !t->job || t->asked) return -1;
    if (!aether_sandbox_check("exec", "kill")) return -1;
    if (!TerminateJobObject(t->job, 128 + 15)) return -1;
    t->asked = 1;
    return 0;
}

void os_tree_free_raw(void* tree) {
    AetherTree* t = (AetherTree*)tree;
    if (!t) return;
    if (t->job) {
        TerminateJobObject(t->job, 128 + 15);
        CloseHandle(t->job);
    }
    free(t);
}

/* Reap a single token. Returns (exit, err). */
_tuple_int_string os_wait_raw(int token) {
    _tuple_int_string out = { -1, "" };
    if (token <= 0) { out._1 = "invalid token"; return out; }
    HANDLE h = winproc_find(token);
    if (!h) { out._1 = "unknown token"; return out; }
    return winproc_reap(token, h);
}

/* Reap whichever token finishes first. Returns (token, exit, err).
 * WaitForMultipleObjects gives a true wait-any over the live handles. */
_tuple_int_int_string os_wait_any_raw(void* token_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._2 = "no tokens"; return out; }
    if (n > MAXIMUM_WAIT_OBJECTS) { out._2 = "too many tokens for one wait_any"; return out; }

    /* Snapshot (token, handle) pairs for the live tokens the caller named.
     * Tokens are small ints stored INLINE as the list's pointer slot (the
     * caller boxes them with mem.long_to_ptr(token)); read them back as the
     * integer value of the pointer, NOT as a pointer-to-int. Only tokens
     * still in the table (not yet reaped) contribute a handle. */
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    int    toks[MAXIMUM_WAIT_OBJECTS];
    int    m = 0;
    for (int i = 0; i < n; i++) {
        void* item = list_get_raw(token_list, i);
        int tok = (int)(intptr_t)item;
        HANDLE h = winproc_find(tok);
        if (h) { handles[m] = h; toks[m] = tok; m++; }
    }
    if (m == 0) { out._2 = "no live tokens"; return out; }

    DWORD r = WaitForMultipleObjects((DWORD)m, handles, FALSE, INFINITE);
    /* WAIT_OBJECT_0 is 0, so the lower bound (r >= WAIT_OBJECT_0) is always
     * true for the unsigned DWORD and -Wtype-limits flags it; only the upper
     * bound distinguishes a signaled index from WAIT_TIMEOUT/WAIT_FAILED. */
    if (r < WAIT_OBJECT_0 + (DWORD)m) {
        int idx = (int)(r - WAIT_OBJECT_0);
        _tuple_int_string s = winproc_reap(toks[idx], handles[idx]);
        out._0 = toks[idx];
        out._1 = s._0;
        out._2 = s._1;
        return out;
    }
    out._2 = "wait_any failed";
    return out;
}

/* Bounded wait-any: like os_wait_any_raw but gives up after `secs`.
 * Returns (token, exit, timed_out, err). WaitForMultipleObjects already
 * takes a dwMilliseconds argument, so the timeout is the value we were
 * passing INFINITE to. secs <= 0 blocks indefinitely. On timeout, token is
 * -1, timed_out is 1, and no handle is reaped (children left running). */
_tuple_int_int_int_string os_wait_any_timeout_raw(void* token_list, int secs) {
    _tuple_int_int_int_string out = { -1, -1, 0, "" };
    int n = token_list ? list_size(token_list) : 0;
    if (n <= 0) { out._3 = "no tokens"; return out; }
    if (n > MAXIMUM_WAIT_OBJECTS) { out._3 = "too many tokens for one wait_any"; return out; }

    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    int    toks[MAXIMUM_WAIT_OBJECTS];
    int    m = 0;
    for (int i = 0; i < n; i++) {
        void* item = list_get_raw(token_list, i);
        int tok = (int)(intptr_t)item;
        HANDLE h = winproc_find(tok);
        if (h) { handles[m] = h; toks[m] = tok; m++; }
    }
    if (m == 0) { out._3 = "no live tokens"; return out; }

    DWORD ms = (secs <= 0) ? INFINITE : (DWORD)secs * 1000;
    DWORD r = WaitForMultipleObjects((DWORD)m, handles, FALSE, ms);
    if (r == WAIT_TIMEOUT) { out._2 = 1; return out; }
    if (r < WAIT_OBJECT_0 + (DWORD)m) {
        int idx = (int)(r - WAIT_OBJECT_0);
        _tuple_int_string s = winproc_reap(toks[idx], handles[idx]);
        out._0 = toks[idx];
        out._1 = s._0;
        out._3 = s._1;
        return out;
    }
    out._3 = "wait_any failed";
    return out;
}

/* run_pipe on Windows: the SPAWN works (no back-channel pipe — that half
 * needs coordinated _open_osfhandle and stays POSIX-only). Returns a -1
 * pipe fd, the token, and ""; reap the token via os_wait_pid_raw. */
_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    _tuple_int_string sp = os_spawn_raw(prog, argv_list, env_list);
    if (sp._1[0]) { out._2 = sp._1; return out; }   /* err */
    out._0 = -1;        /* no back-channel pipe on Windows */
    out._1 = sp._0;     /* token doubles as the wait_pid handle */
    return out;
}

_tuple_int_int_string os_run_socket_raw(const char* prog, void* argv_list, void* env_list) {
    (void)prog; (void)argv_list; (void)env_list;
    _tuple_int_int_string out = { -1, -1, "unsupported on Windows" };
    return out;
}

/* os.run_stdio on Windows: the child's stdin and stdout are anonymous pipes
 * whose other ends this process keeps; its stderr this process's
 * (stderr_to 0), NUL (1) or the file stderr_path, created or truncated (2),
 * and a non-empty `cwd` its working directory (CreateProcessW's
 * lpCurrentDirectory). The
 * parent's ends are made non-inheritable before the spawn and handed back
 * as binary CRT descriptors (_open_osfhandle), which std.ipc's write, read
 * and frame calls take as they take any; the child inherits its two pipe
 * ends and stderr and nothing else (#2917). The pid is a spawn token, as
 * os_spawn_raw's: os_wait_pid_raw, os_wait_pid_timeout_raw and os_kill_raw
 * take it. Returns (to_child_fd, from_child_fd, token, "") or
 * (-1, -1, -1, reason) with nothing left to reap. */
_tuple_int_int_int_string os_run_stdio_raw(const char* prog, void* argv_list, void* env_list,
                                           int stderr_to, const char* stderr_path,
                                           const char* cwd) {
    _tuple_int_int_int_string out = { -1, -1, -1, "" };
    if (!prog) { out._3 = "null prog"; return out; }
    const char* serr = os_stdio_stderr_check(stderr_to, stderr_path);
    if (serr) { out._3 = serr; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._3 = "denied by sandbox"; return out; }
    if (stderr_to == OS_STDERR_FILE && !aether_sandbox_check("fs", stderr_path)) {
        out._3 = "denied by sandbox";
        return out;
    }
    /* The child's working directory is a chdir, checked as os.chdir is. */
    if (cwd && cwd[0] && !aether_sandbox_check("fs", cwd)) {
        out._3 = "denied by sandbox";
        return out;
    }

    /* The working directory, checked here so a missing one is reported as
     * that rather than as a spawn failure. */
    wchar_t* wcwd = NULL;
    if (cwd && cwd[0]) {
        wcwd = utf8_to_wide(cwd);
        DWORD attrs = wcwd ? GetFileAttributesW(wcwd) : INVALID_FILE_ATTRIBUTES;
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            free(wcwd);
            out._3 = "os.run_stdio: cannot enter the working directory: not a directory";
            return out;
        }
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    /* The child's stderr: NUL, or the file, opened inheritable for it. */
    HANDLE err_h = NULL;
    if (stderr_to != OS_STDERR_INHERIT) {
        wchar_t* wpath = stderr_to == OS_STDERR_FILE ? utf8_to_wide(stderr_path) : NULL;
        if (stderr_to == OS_STDERR_FILE && !wpath) {
            free(wcwd);
            out._3 = "os.run_stdio: bad stderr path";
            return out;
        }
        err_h = CreateFileW(wpath ? wpath : L"NUL", GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                            stderr_to == OS_STDERR_FILE ? CREATE_ALWAYS : OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        free(wpath);
        if (err_h == INVALID_HANDLE_VALUE) {
            free(wcwd);
            out._3 = stderr_to == OS_STDERR_FILE ? "os.run_stdio: cannot open the stderr file"
                                                 : "os.run_stdio: cannot open NUL";
            return out;
        }
    }

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
        if (err_h) CloseHandle(err_h);
        free(wcwd);
        out._3 = "os.run_stdio: pipe failed";
        return out;
    }
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w);
        if (err_h) CloseHandle(err_h);
        free(wcwd);
        out._3 = "os.run_stdio: pipe failed";
        return out;
    }
    /* The parent's ends are never inherited, by this child or any other. */
    if (!SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w);
        if (err_h) CloseHandle(err_h);
        free(wcwd);
        out._3 = "os.run_stdio: pipe failed";
        return out;
    }

    HANDLE stdio[3] = { in_r, out_w, err_h ? err_h : GetStdHandle(STD_ERROR_HANDLE) };
    HANDLE h = win_spawn_stdio(prog, argv_list, env_list, stdio, wcwd);
    free(wcwd);
    /* The child's ends are its own now: closed here, so the parent's read
     * sees end of stream when the child exits and the child's sees it when
     * the parent closes to_child_fd. */
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (err_h) CloseHandle(err_h);
    if (!h) {
        CloseHandle(in_w); CloseHandle(out_r);
        out._3 = win_launch_error();
        return out;
    }

    int to_fd = _open_osfhandle((intptr_t)in_w, _O_WRONLY | _O_BINARY);
    int from_fd = to_fd < 0 ? -1 : _open_osfhandle((intptr_t)out_r, _O_RDONLY | _O_BINARY);
    if (to_fd < 0 || from_fd < 0) {
        /* The child is running with its ends: end it rather than leave it
         * talking to nobody. _close on a descriptor closes its handle. */
        if (to_fd >= 0) _close(to_fd); else CloseHandle(in_w);
        CloseHandle(out_r);
        TerminateProcess(h, 1);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        out._3 = "os.run_stdio: too many open descriptors";
        return out;
    }

    int tok = winproc_register(h);
    if (tok < 0) {
        _close(to_fd);
        _close(from_fd);
        TerminateProcess(h, 1);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        out._3 = "process table full";
        return out;
    }
    out._0 = to_fd;
    out._1 = from_fd;
    out._2 = tok;
    return out;
}

_tuple_int_int_string os_socketpair_raw(void) {
    _tuple_int_int_string out = { -1, -1, "unsupported on Windows" };
    return out;
}

_tuple_int_string os_wait_pid_raw(int pid) {
    /* pid here is the token minted by os_run_pipe_raw / os_spawn_raw. */
    return os_wait_raw(pid);
}

_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    /* The back-channel pipe is the POSIX-only part; no Windows equivalent. */
    (void)prog; (void)argv_list; (void)env_list;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "unsupported on Windows" };
    return out;
}

/* Process-supervision primitives on Windows.
 *
 * POSIX process groups + killpg + sigaction don't exist here; the clean
 * Win32 mapping is:
 *   - process *group* lifecycle (timeout, group-reap of leaked children)
 *     → a Job Object. JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE makes closing
 *     the job tear down the whole tree, which IS group-reap;
 *     TerminateJobObject is the group kill;
 *   - signal forwarding → a console control handler that terminates the
 *     job on Ctrl-C / Ctrl-Break (console apps only; there are no
 *     catchable POSIX signals to forward);
 *   - waiting / timeout → WaitForSingleObject on the process handle.
 * There is no "killed by signal" outcome on Windows, so run_supervised
 * reports "exited" (or "timeout"); os.kill maps any non-zero signal to a
 * forced TerminateProcess (no graceful SIGTERM) and rejects negative-pid
 * group signalling (no pgid-by-negative-pid concept). */

int os_kill_raw(int pid, int sig) {
    if (!aether_sandbox_check("exec", "kill")) return -1;
    /* Negative pid = signal a whole group: no Win32 equivalent. */
    if (pid < 0) return -1;

    /* Resolve a spawn token through the process table first, so kill() takes
     * the same identity os_spawn_raw / os_wait_* hand out on every platform
     * (issue #1278). A token is a table key, NOT an OS pid, so
     * OpenProcess((DWORD)token) would hit an unrelated process. When the int
     * is not a live token and is below the token range we fall back to
     * treating it as a real pid, which keeps kill() working for pids
     * obtained by other means; in the range it is a reaped token (#2609). The table HANDLE
     * is borrowed (do NOT CloseHandle it here — the owning wait reaps it). */
    HANDLE tok_h = winproc_find(pid);
    if (tok_h) {
        if (sig == 0) {
            DWORD w = WaitForSingleObject(tok_h, 0);
            return (w == WAIT_TIMEOUT) ? 0 : -1;   /* still running -> alive */
        }
        /* A child os_spawn_to_raw started: end its whole job, the tree. */
        HANDLE job = winproc_find_job(pid);
        if (job) return TerminateJobObject(job, (UINT)(128 + sig)) ? 0 : -1;
        return TerminateProcess(tok_h, (UINT)(128 + sig)) ? 0 : -1;
    }
    if (pid >= WINPROC_TOKEN_BASE) return -1;   /* a reaped token (#2609) */

    if (sig == 0) {
        /* Existence probe: open + a zero-timeout wait. WAIT_TIMEOUT means
         * the process is still running (alive); WAIT_OBJECT_0 means it has
         * already exited (gone) — matches POSIX kill -0 semantics. */
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        if (!h) return -1;
        DWORD w = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return (w == WAIT_TIMEOUT) ? 0 : -1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return -1;
    /* Exit code mirrors POSIX 128+signo so a waiter can tell it was a kill. */
    BOOL ok = TerminateProcess(h, (UINT)(128 + sig));
    CloseHandle(h);
    return ok ? 0 : -1;
}

_tuple_int_int_string os_wait_pid_timeout_raw(int pid, int secs) {
    _tuple_int_int_string out = { -1, 0, "" };
    if (pid <= 0) { out._2 = "invalid pid"; return out; }

    /* Resolve a spawn token through the process table first (issue #1278),
     * so a bounded wait takes the same identity os_spawn_raw hands out. On a
     * clean exit we reap the token here (close its handle + free the slot),
     * just like winproc_reap, so a later os_wait on the same token cleanly
     * misses rather than double-closing. On timeout the child is left running
     * and its table slot untouched. */
    HANDLE tok_h = winproc_find(pid);
    if (tok_h) {
        DWORD ms = (secs <= 0) ? INFINITE : (DWORD)secs * 1000;
        DWORD w = WaitForSingleObject(tok_h, ms);
        if (w == WAIT_TIMEOUT) { out._1 = 1; return out; }
        if (w != WAIT_OBJECT_0) { out._2 = "wait failed"; return out; }
        DWORD code = 0;
        GetExitCodeProcess(tok_h, &code);
        CloseHandle(tok_h);
        winproc_remove(pid);
        out._0 = (int)code;
        return out;
    }
    /* A reaped token (#2609): not a pid. */
    if (pid >= WINPROC_TOKEN_BASE) { out._2 = "no such process"; return out; }

    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h) { out._2 = "no such process"; return out; }
    DWORD ms = (secs <= 0) ? INFINITE : (DWORD)secs * 1000;
    DWORD w = WaitForSingleObject(h, ms);
    if (w == WAIT_TIMEOUT) { out._1 = 1; CloseHandle(h); return out; }
    if (w != WAIT_OBJECT_0) { out._2 = "wait failed"; CloseHandle(h); return out; }
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    out._0 = (int)code;
    return out;
}

/* Forward-signals state: the job to tear down on Ctrl-C / Ctrl-Break.
 * Single supervised run at a time (mirrors the POSIX g_supervised_pgid). */
static HANDLE g_supervised_job = NULL;

static BOOL WINAPI aether_os_ctrl_handler(DWORD ctrl) {
    if ((ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) && g_supervised_job) {
        TerminateJobObject(g_supervised_job, (UINT)(128 + 2)); /* SIGINT-ish */
        return TRUE;
    }
    return FALSE;
}

_tuple_int_string os_run_supervised_raw(const char* prog, void* argv_list, void* env_list,
                                        int new_process_group, int forward_signals,
                                        int timeout_secs, int reap_group) {
    _tuple_int_string out = { -1, "error" };
    if (!prog) { out._1 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._1 = "denied by sandbox"; return out; }

    wchar_t* app = NULL;
    wchar_t* cmdline = win_prepare_launch(prog, argv_list, &app);
    if (!cmdline) { out._1 = win_launch_error(); return out; }
    wchar_t* wenv = build_environ_block(env_list); /* NULL = inherit */

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) { free(app); free(cmdline); free(wenv); out._1 = "job create failed"; return out; }

    /* Arm kill-on-close only when reap_group is set: closing the job then
     * tears down the whole tree (the Windows group-reap). Without it,
     * leaked children outlive the job-handle close. */
    if (reap_group) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
    }

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);

    /* CREATE_SUSPENDED so we can assign to the job before the child runs —
     * otherwise a fast child could spawn grandchildren that escape it. */
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (new_process_group) flags |= CREATE_NEW_PROCESS_GROUP;

    /* The resolved absolute program, as in win_launch (#2171). */
    BOOL ok = CreateProcessW(app, cmdline, NULL, NULL, FALSE, flags, wenv, NULL, &si, &pi);
    free(app); free(cmdline); free(wenv);
    if (!ok) { CloseHandle(job); out._1 = "spawn failed"; return out; }

    AssignProcessToJobObject(job, pi.hProcess);

    HANDLE old_job = g_supervised_job;
    int installed = 0;
    if (forward_signals) {
        g_supervised_job = job;
        SetConsoleCtrlHandler(aether_os_ctrl_handler, TRUE);
        installed = 1;
    }

    ResumeThread(pi.hThread);

    const char* outcome = "exited";
    int exit_code = -1;
    DWORD ms = (timeout_secs > 0) ? (DWORD)timeout_secs * 1000 : INFINITE;
    DWORD w = WaitForSingleObject(pi.hProcess, ms);
    if (w == WAIT_TIMEOUT) {
        TerminateJobObject(job, 124);             /* GNU timeout convention */
        WaitForSingleObject(pi.hProcess, 5000);
        outcome = "timeout";
        exit_code = 124;
    } else if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        exit_code = (int)code;
        outcome = "exited";
    } else {
        outcome = "error";
        exit_code = -1;
    }

    if (installed) {
        SetConsoleCtrlHandler(aether_os_ctrl_handler, FALSE);
        g_supervised_job = old_job;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job); /* fires KILL_ON_JOB_CLOSE when reap_group was set */

    out._0 = exit_code;
    out._1 = outcome;
    return out;
}

/* os.run_full on Windows. The POSIX side uses a poll(2) loop over the
 * three pipes; Win32 anonymous pipes aren't pollable, so we drain stdout
 * and stderr on dedicated reader threads while the main thread writes
 * stdin — same deadlock-free guarantee, different mechanism. */
typedef struct { const char* _0; const char* _1; int _2; const char* _3; } _tuple_string_string_int_string;

typedef struct {
    HANDLE pipe;
    char*  buf;
    size_t len;
    size_t cap;
    int    oom;
} aether_win_drain_ctx;

/* Read `pipe` to EOF into a doubling buffer. EOF on a Win32 pipe surfaces
 * as ReadFile returning FALSE with ERROR_BROKEN_PIPE once the child's
 * write end is closed — treated as a clean end-of-stream. */
static unsigned __stdcall aether_win_drain_thread(void* arg) {
    aether_win_drain_ctx* c = (aether_win_drain_ctx*)arg;
    char tmp[4096];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(c->pipe, tmp, sizeof tmp, &got, NULL)) break;
        if (got == 0) break;
        if (c->len + got + 1 > c->cap) {
            size_t nc = c->cap ? c->cap : 4096;
            while (c->len + got + 1 > nc) nc *= 2;
            char* b = (char*)aether_caps_realloc(c->buf, c->cap, nc);
            if (!b) { c->oom = 1; break; }
            c->buf = b; c->cap = nc;
        }
        memcpy(c->buf + c->len, tmp, got);
        c->len += got;
    }
    return 0;
}

_tuple_string_string_int_string os_run_full_raw(const char* prog, void* argv_list, void* env_list,
                                                const char* stdin_data, int stdin_len) {
    _tuple_string_string_int_string out = {
        aether_os_empty_heap(), aether_os_empty_heap(), -1, "" };
    if (!prog) { out._3 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._3 = "denied by sandbox"; return out; }

    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa; sa.bInheritHandle = TRUE; sa.lpSecurityDescriptor = NULL;

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) { out._3 = "pipe failed"; return out; }
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w);
        out._3 = "pipe failed"; return out;
    }
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w);
        out._3 = "pipe failed"; return out;
    }
    /* Parent-side ends must not be inherited by the child. */
    SetHandleInformation(in_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    wchar_t* app = NULL;
    wchar_t* cmdline = win_prepare_launch(prog, argv_list, &app);
    if (!cmdline) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r);
        CloseHandle(out_w); CloseHandle(err_r); CloseHandle(err_w);
        out._3 = win_launch_error(); return out;
    }
    wchar_t* wenv = build_environ_block(env_list); /* NULL = inherit */

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_r;
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);

    /* The resolved absolute program, as in win_launch (#2171). The child
     * inherits its three pipe ends and nothing else (#2917). */
    HANDLE inherit[3] = { in_r, out_w, err_w };
    BOOL ok = win_create_process_inheriting(app, cmdline, CREATE_UNICODE_ENVIRONMENT, wenv,
                                            NULL, &si, inherit, 3, &pi);
    free(app); free(cmdline); free(wenv);
    /* Close the child-side ends in the parent so EOF propagates correctly. */
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r); CloseHandle(err_r);
        out._3 = "spawn failed"; return out;
    }

    aether_win_drain_ctx octx; memset(&octx, 0, sizeof octx); octx.pipe = out_r;
    aether_win_drain_ctx ectx; memset(&ectx, 0, sizeof ectx); ectx.pipe = err_r;
    HANDLE ot = (HANDLE)_beginthreadex(NULL, 0, aether_win_drain_thread, &octx, 0, NULL);
    HANDLE et = (HANDLE)_beginthreadex(NULL, 0, aether_win_drain_thread, &ectx, 0, NULL);

    size_t total = (stdin_data && stdin_len > 0) ? (size_t)stdin_len : 0;
    size_t off = 0;
    while (off < total) {
        DWORD chunk = (DWORD)((total - off) > 65536 ? 65536 : (total - off));
        DWORD wrote = 0;
        if (!WriteFile(in_w, stdin_data + off, chunk, &wrote, NULL)) break; /* child closed stdin */
        off += wrote;
    }
    CloseHandle(in_w); /* deliver EOF to the child's stdin */

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (ot) { WaitForSingleObject(ot, INFINITE); CloseHandle(ot); }
    if (et) { WaitForSingleObject(et, INFINITE); CloseHandle(et); }
    CloseHandle(out_r); CloseHandle(err_r);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    if (octx.oom || ectx.oom) {
        aether_caps_free(octx.buf, octx.cap); aether_caps_free(ectx.buf, ectx.cap);
        out._3 = "alloc failed"; return out;
    }
    if (octx.buf) {
        octx.buf[octx.len] = '\0';
        string_release(out._0);
        out._0 = (const char*)string_adopt_caps_buffer(octx.buf, octx.len, octx.cap);
    }
    if (ectx.buf) {
        ectx.buf[ectx.len] = '\0';
        string_release(out._1);
        out._1 = (const char*)string_adopt_caps_buffer(ectx.buf, ectx.len, ectx.cap);
    }
    out._2 = (int)code;
    return out;
}

/* ipc_parent_channel_raw on Windows — implemented in
 * std/ipc/aether_ipc.c (returns -1 unconditionally; Windows is
 * stub-first per the v1 design). */

#endif // !_WIN32

/* os.environ(): this process's environment as a *StringSeq of "NAME=value"
 * strings, in the order the OS keeps them (nib-code ask
 * aether-fs-stat-flock-exclusive, item 5: a child's environment built
 * without some variables needs every one; std.os had getenv only). Each
 * string is a counted copy, so a later setenv does not change the list. A
 * name the sandbox does not grant `env` for is left out, as os.getenv
 * refuses it. Windows: GetEnvironmentStringsW in UTF-8, without the
 * "=C:=C:\..." per-drive directory entries, which are no variables. */
struct StringSeq;
extern struct StringSeq* string_seq_cons_adopt(const char* head, struct StringSeq* tail);
extern void string_seq_free(struct StringSeq* s);

static int os_environ_entry_granted(const char* entry, size_t len) {
    const char* eq = memchr(entry, '=', len);
    size_t nlen = eq ? (size_t)(eq - entry) : len;
    char small[256];
    char* name = nlen < sizeof small ? small : (char*)malloc(nlen + 1);
    if (!name) return 0;
    memcpy(name, entry, nlen);
    name[nlen] = '\0';
    int ok = aether_sandbox_check("env", name);
    if (name != small) free(name);
    return ok;
}

/* Cons `entries[0..n)` onto an empty seq back to front, so the seq reads in
 * their order. Each entry is copied into a counted string the cell adopts. */
static void* os_environ_seq(const char* const* entries, const size_t* lens, int n) {
    struct StringSeq* seq = NULL;
    for (int i = n - 1; i >= 0; i--) {
        if (!os_environ_entry_granted(entries[i], lens[i])) continue;
        AetherString* s = string_new_with_length(entries[i], lens[i]);
        if (!s) continue;
        struct StringSeq* cell = string_seq_cons_adopt((const char*)s, seq);
        if (!cell) { string_release(s); continue; }
        seq = cell;
    }
    return seq;
}

#ifdef _WIN32
void* os_environ_raw(void) {
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return NULL;
    int n = 0;
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) n++;
    char** entries = n ? (char**)calloc((size_t)n, sizeof(char*)) : NULL;
    size_t* lens = n ? (size_t*)calloc((size_t)n, sizeof(size_t)) : NULL;
    int k = 0;
    if (n && entries && lens) {
        for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (p[0] == L'=') continue;
            int bytes = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
            if (bytes <= 0) continue;
            char* u = (char*)malloc((size_t)bytes);
            if (!u) continue;
            WideCharToMultiByte(CP_UTF8, 0, p, -1, u, bytes, NULL, NULL);
            entries[k] = u;
            lens[k] = (size_t)bytes - 1;
            k++;
        }
    }
    FreeEnvironmentStringsW(block);
    void* seq = os_environ_seq((const char* const*)entries, lens, k);
    for (int i = 0; i < k; i++) free(entries[i]);
    free(entries);
    free(lens);
    return seq;
}
#else
void* os_environ_raw(void) {
    char** env = aether_spawn_environ();
    int n = 0;
    while (env && env[n]) n++;
    if (n == 0) return NULL;
    size_t* lens = (size_t*)calloc((size_t)n, sizeof(size_t));
    if (!lens) return NULL;
    for (int i = 0; i < n; i++) lens[i] = strlen(env[i]);
    void* seq = os_environ_seq((const char* const*)env, lens, n);
    free(lens);
    return seq;
}
#endif


/* os.signal_watch / signal_received / signal_unwatch (nib-code ask
 * aether-signal-flag-and-tty-echo): SIGINT and SIGTERM, and the other
 * catchable signals, as something a program polls instead of the default
 * action ending it.
 *
 * Where the state lives, and why any of it is process-wide: the kernel calls
 * a handler with the signal number and nothing else (no user pointer), so
 * whatever it records has to be reachable from the number alone. That is
 * one delivery counter per signal, ae_sig_count, and nothing more: the
 * handler only adds one to it (an atomic add, async-signal-safe), and no
 * other code writes it. Everything a caller sees is in its own watch: the
 * signals it asked for and the count it last saw of each. A watch reports a
 * signal when its count moved since that watch last looked, so two watches
 * on SIGINT both see a Ctrl-C and neither takes it from the other; there is
 * no shared "last signal" slot to consume.
 *
 * The other process-wide state is the bookkeeping that puts the handler in
 * and takes it out: how many watches hold each signal, and the disposition
 * the first one replaced, restored when the last one goes. It is touched
 * only by watch and unwatch, under a lock. */
#define AE_SIG_LIMIT 65

typedef struct {
    int n;
    int sigs[AE_SIG_LIMIT];
    long seen[AE_SIG_LIMIT];
} AeSignalWatch;

typedef struct { void* _0; const char* _1; } _tuple_ptr_string_os;

#ifdef _WIN32
#include <signal.h>
static volatile LONG ae_sig_count[AE_SIG_LIMIT];
static volatile LONG ae_sig_watchers[AE_SIG_LIMIT];
static SRWLOCK ae_sig_lock = SRWLOCK_INIT;
static int ae_sig_ctrl_installed = 0;

static long ae_sig_load(int sig) { return (long)InterlockedCompareExchange(&ae_sig_count[sig], 0, 0); }

/* Console control events stand in for the two signals Windows has for a
 * console program: Ctrl-C and Ctrl-Break are SIGINT; closing the console,
 * logging off and shutting down are SIGTERM. TRUE (handled) only while a
 * watch holds the signal, so the default ending applies otherwise. Windows
 * ends the process as soon as a close, logoff or shutdown handler returns,
 * so for those the handler waits (up to the five seconds Windows allows)
 * while the program polls and finishes up. */
static BOOL WINAPI ae_sig_ctrl_handler(DWORD event) {
    int sig;
    switch (event) {
        case CTRL_C_EVENT: case CTRL_BREAK_EVENT: sig = SIGINT; break;
        case CTRL_CLOSE_EVENT: case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT: sig = SIGTERM; break;
        default: return FALSE;
    }
    if (InterlockedCompareExchange(&ae_sig_watchers[sig], 0, 0) == 0) return FALSE;
    InterlockedIncrement(&ae_sig_count[sig]);
    if (sig == SIGTERM) Sleep(5000);
    return TRUE;
}

static const char* ae_sig_install(int sig) {
    if (sig != SIGINT && sig != SIGTERM) return "os.signal_watch: only SIGINT and SIGTERM on Windows";
    if (!ae_sig_ctrl_installed) {
        if (!SetConsoleCtrlHandler(ae_sig_ctrl_handler, TRUE)) return "os.signal_watch: SetConsoleCtrlHandler failed";
        ae_sig_ctrl_installed = 1;
    }
    return NULL;
}

static void ae_sig_restore(int sig) {
    (void)sig;
    for (int i = 1; i < AE_SIG_LIMIT; i++) if (ae_sig_watchers[i] > 0) return;
    if (ae_sig_ctrl_installed) {
        SetConsoleCtrlHandler(ae_sig_ctrl_handler, FALSE);
        ae_sig_ctrl_installed = 0;
    }
}
#define AE_SIG_LOCK() AcquireSRWLockExclusive(&ae_sig_lock)
#define AE_SIG_UNLOCK() ReleaseSRWLockExclusive(&ae_sig_lock)
#define AE_SIG_WATCHERS_ADD(sig, d) InterlockedExchangeAdd(&ae_sig_watchers[(sig)], (d))

#else
#include <signal.h>
#include <pthread.h>
static volatile int ae_sig_count[AE_SIG_LIMIT];
static int ae_sig_watchers[AE_SIG_LIMIT];
static struct sigaction ae_sig_prev[AE_SIG_LIMIT];
static pthread_mutex_t ae_sig_lock = PTHREAD_MUTEX_INITIALIZER;

static void ae_sig_handler(int sig) {
    if (sig > 0 && sig < AE_SIG_LIMIT) __atomic_fetch_add(&ae_sig_count[sig], 1, __ATOMIC_SEQ_CST);
}

static long ae_sig_load(int sig) { return (long)__atomic_load_n(&ae_sig_count[sig], __ATOMIC_SEQ_CST); }

static const char* ae_sig_install(int sig) {
    if (sig == SIGKILL || sig == SIGSTOP) return "os.signal_watch: SIGKILL and SIGSTOP cannot be caught";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ae_sig_handler;
    sigemptyset(&sa.sa_mask);
    /* Interrupted reads and waits carry on: the watch is polled, so nothing
     * needs a blocking call to fail with EINTR to notice the signal. */
    sa.sa_flags = SA_RESTART;
    if (sigaction(sig, &sa, &ae_sig_prev[sig]) != 0) return "os.signal_watch: sigaction failed";
    return NULL;
}

static void ae_sig_restore(int sig) { (void)sigaction(sig, &ae_sig_prev[sig], NULL); }
#define AE_SIG_LOCK() pthread_mutex_lock(&ae_sig_lock)
#define AE_SIG_UNLOCK() pthread_mutex_unlock(&ae_sig_lock)
#define AE_SIG_WATCHERS_ADD(sig, d) (ae_sig_watchers[(sig)] += (d))
#endif

/* Drop one hold on `sig`; the last puts back what was there before. */
static void ae_sig_release(int sig) {
    AE_SIG_WATCHERS_ADD(sig, -1);
    if (ae_sig_watchers[sig] == 0) ae_sig_restore(sig);
}

/* A watch on the `n` signals in `sigs`: (watch, "") or (NULL, why). A
 * number outside 1..64, or a signal that cannot be caught, refuses the whole
 * watch and leaves every disposition as it was. */
_tuple_ptr_string_os os_signal_watch_raw(const int* sigs, int n) {
    _tuple_ptr_string_os out = { NULL, "" };
    if (!sigs || n <= 0 || n >= AE_SIG_LIMIT) { out._1 = "os.signal_watch: give one to 64 signals"; return out; }
    for (int i = 0; i < n; i++) {
        if (sigs[i] <= 0 || sigs[i] >= AE_SIG_LIMIT) { out._1 = "os.signal_watch: not a signal number"; return out; }
    }
    AeSignalWatch* w = (AeSignalWatch*)calloc(1, sizeof *w);
    if (!w) { out._1 = "os.signal_watch: out of memory"; return out; }
    AE_SIG_LOCK();
    for (int i = 0; i < n; i++) {
        int sig = sigs[i];
        int dup = 0;
        for (int j = 0; j < w->n; j++) dup |= w->sigs[j] == sig;
        if (dup) continue;
        const char* why = ae_sig_watchers[sig] == 0 ? ae_sig_install(sig) : NULL;
        if (why) {
            for (int j = 0; j < w->n; j++) ae_sig_release(w->sigs[j]);
            AE_SIG_UNLOCK();
            free(w);
            out._1 = why;
            return out;
        }
        AE_SIG_WATCHERS_ADD(sig, 1);
        w->sigs[w->n] = sig;
        w->seen[w->n] = ae_sig_load(sig);
        w->n++;
    }
    AE_SIG_UNLOCK();
    out._0 = w;
    return out;
}

/* The number of a watched signal delivered since this watch last reported
 * it, or 0. One per call: when SIGINT and SIGTERM both came, the next call
 * gives the other. */
int os_signal_received_raw(void* watch) {
    AeSignalWatch* w = (AeSignalWatch*)watch;
    if (!w) return 0;
    for (int i = 0; i < w->n; i++) {
        long now = ae_sig_load(w->sigs[i]);
        if (now != w->seen[i]) {
            w->seen[i] = now;
            return w->sigs[i];
        }
    }
    return 0;
}

/* End the watch and free it. */
void os_signal_unwatch_raw(void* watch) {
    AeSignalWatch* w = (AeSignalWatch*)watch;
    if (!w) return;
    AE_SIG_LOCK();
    for (int i = 0; i < w->n; i++) ae_sig_release(w->sigs[i]);
    AE_SIG_UNLOCK();
    free(w);
}


#endif // AETHER_HAS_FILESYSTEM
