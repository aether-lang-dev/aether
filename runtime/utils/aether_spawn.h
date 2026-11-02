#ifndef AETHER_SPAWN_H
#define AETHER_SPAWN_H

/* One discipline for the pipes and forks of a process (#2917, #3028, #3040,
 * #3046), for every part of it that makes either: std.os's process calls,
 * contrib/host's bridges, the scheduler's I/O poller and std.net's and
 * std.http's wake pipes.
 *
 * A pipe made with aether_pipe_cloexec closes on exec in every child. A
 * plain pipe() is copied into every child another thread starts while it is
 * open, and the child keeps it across exec: a capture reading the pipe then
 * waits for EOF until that unrelated child exits, and a wake pipe's write end
 * lives on in it. A child that is meant to keep an end moves it onto its
 * stdio with dup2, which clears the flag on the copy.
 *
 * Where pipe2 is missing (macOS) the flag is set by fcntl after pipe(), and a
 * fork between the two would copy the pipe anyway. Pipe creation then holds
 * the process's fork lock shared and aether_fork holds it exclusively, as the
 * Go runtime's ForkLock does. aether_fork's child inherits the lock held and
 * must exec or _exit without making a pipe.
 *
 * POSIX only: Windows makes HANDLE pipes and has no fork. aether_pipe_cloexec
 * is defined wherever pipe() is (not WASI), aether_fork where std.os runs
 * processes (AETHER_HAS_PROCESS and AETHER_HAS_FILESYSTEM).
 * aether_pipe_cloexec returns 0, or -1 with errno set; aether_fork returns
 * what fork() does.
 *
 * A child of a threaded parent may call only async-signal-safe functions
 * between fork and exec: another thread may have held malloc's lock or the
 * environment's at the fork, and the child never gets it back. So what a
 * child execs with is made in the parent, with the three below, and the
 * child calls execve (#3200, #3201): std.os's spawners and the runtime's
 * spawn_sandboxed do. */

#ifndef _WIN32
#include <sys/types.h>

int aether_pipe_cloexec(int fds[2]);
pid_t aether_fork(void);

/* A connected AF_UNIX stream pair under the same rules as
 * aether_pipe_cloexec: both ends close on exec (SOCK_CLOEXEC, or fcntl under
 * the fork lock where that is missing), and on macOS and the BSDs both carry
 * SO_NOSIGPIPE, so a write to a closed peer fails with EPIPE rather than
 * raising SIGPIPE. Defined where aether_fork is. Returns 0, or -1 with errno
 * set. */
int aether_socketpair_cloexec(int fds[2]);

/* The program a child runs. A name with a '/' is taken as given. A bare
 * name is looked up on this process's PATH, whatever env the child is
 * given, as Windows and Go do: the first entry where it is a regular file
 * this process may execute. An empty entry is the current directory, as
 * execvp and os.which have it, and an unset PATH is os.which's default.
 * malloc'd; NULL with errno ENOENT when nothing is found, ENOMEM when a
 * malloc fails. */
char* aether_spawn_resolve(const char* prog);

/* This process's environ, the env a child execs with when it is given none
 * (through _NSGetEnviron on macOS, where a dylib cannot name environ). */
char** aether_spawn_environ(void);

/* `base` with each of the `nset` "KEY=VALUE" strings in `set` in place of
 * any KEY= entry of its own, the set ones last. The array is malloc'd and
 * borrows its strings from `base` and `set`; NULL when malloc fails. */
char** aether_spawn_env(char** base, const char* const* set, int nset);

/* os.run_socket with more done to the child (sandbox.spawn_socket's
 * containment, runtime/sandbox/spawn_sandboxed_linux.c). The channel, the
 * env, the program's lookup and the exec-error pipe are os.run_socket's own
 * (std/os/aether_os.c); the hook adds:
 *   env/nenv  "KEY=VALUE" strings the child's env has in place of its own;
 *   keep_fd   a descriptor (4 or above) the child keeps open across exec,
 *             at the same number; -1 for none;
 *   ready     in the parent once the program is resolved, before the fork:
 *             `path` and `sh_path` are the pointers the child will pass to
 *             execve (the program, then /bin/sh for a file with no #!
 *             line). 0, or -1 to start nothing, with the reason left for
 *             `failed` to give (step 0);
 *   child     in the child, after fd 3 is in place and before exec: only
 *             async-signal-safe calls. 0, or a step above 0 with errno set:
 *             the child then exits without running the program;
 *   failed    in the parent: the err the call returns for a step that
 *             failed (`err` is the errno the child reported).
 * Any of the functions may be NULL. Returns 0 with *fd and *pid set, or -1
 * with *errmsg set and nothing left to reap. Defined where os.run_socket
 * is (not Windows). */
typedef struct AetherSpawnHook {
    const char* const* env;
    int nenv;
    int keep_fd;
    int (*ready)(void* ctx, const char* path, const char* sh_path);
    int (*child)(void* ctx);
    const char* (*failed)(void* ctx, int step, int err);
    void* ctx;
} AetherSpawnHook;

int aether_os_run_socket_hooked(const char* prog, void* argv_list, void* env_list,
                                const AetherSpawnHook* hook,
                                int* fd, int* pid, const char** errmsg);
#endif

/* The parent ends of std.ipc's channels, so std.ipc can refuse what a child
 * sends up them. aether_spawn_channel (os.run_socket, and any other spawner
 * that hands its caller one end of a socket pair for a child) marks the end
 * it returns; std.ipc asks before accepting a descriptor from it, and
 * io.fd_close forgets the number as it closes it, so a later socket that
 * reuses the number is not taken for a parent end. A fixed table, lock-free:
 * marking fails (-1, errno ENOSPC) rather than letting a channel go
 * unmarked, and the caller then does not hand the channel out. Defined on
 * every platform; where there are no channels nothing is ever marked. */
int aether_ipc_mark_parent_end(int fd);
void aether_ipc_forget_fd(int fd);
int aether_ipc_is_parent_end(int fd);

#endif
