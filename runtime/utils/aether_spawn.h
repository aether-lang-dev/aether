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
#endif

#endif
