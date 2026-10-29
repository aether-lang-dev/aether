// spawn_sandboxed_bsd.c — spawn a child process under Aether sandbox (FreeBSD)
//
// Same model as the Linux backend: fork, shm_open, and LD_PRELOAD —
// FreeBSD's rtld-elf honors LD_PRELOAD and dlsym(RTLD_NEXT, ...) just
// as glibc's loader does. The one platform difference is locating the
// running binary: FreeBSD does not mount /proc by default, so instead
// of readlink("/proc/self/exe") we ask the kernel directly via
// sysctl(KERN_PROC_PATHNAME).
//
// Containment here is two-layered: the LD_PRELOAD preload library for
// any child, plus AETHER_CAPSICUM=1 so an Aether child additionally
// self-sandboxes with cap_enter() (kernel-enforced) via its startup
// hook. See the child-branch comment below and capsicum_autosandbox.c.
//
// Self-guarded: compiles to an empty object off FreeBSD. Companion
// backends: spawn_sandboxed_linux.c, spawn_sandboxed_stub.c.

#if defined(__FreeBSD__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

// From libaether.a — list operations
extern int list_size(void*);
extern void* list_get_raw(void*, int);
// String-ABI accessor: list_get_raw may return a magic AetherString* or a
// raw const char*; aether_string_data unwraps both to a plain C-string.
// Required for grant-pattern reads — see issue #688 (the Linux backend hit
// the same corruption when it cast list_get_raw directly to const char*).
extern const char* aether_string_data(const void*);

// The preload library's lookup, shared with the contrib process hosts.
#include "aether_sandbox_preload_path.h"
#include "aether_sandbox_grants.h"
#include "aether_sandbox_shm.h"
// The close-on-exec pipe, the fork, and what a child execs with.
#include "../utils/aether_spawn.h"

// What a child that could not run its program tells the parent, on a
// close-on-exec pipe the exec closes unwritten: the step that failed and its
// errno. The parent prints the reason. The child itself calls nothing
// between fork and exec that is not async-signal-safe (#3200): it used to
// setenv the sandbox's variables, execlp the program and print with perror
// there, and a child of a threaded parent may do none of that,
// since another thread may have held malloc's lock or the environment's at
// the fork. The env, the resolved program and both argvs are made in the
// parent instead (runtime/utils/aether_spawn.h).
enum { SANDBOX_STEP_EXEC = 2 };

static void sandbox_child_failed(int fd, int step, int code) {
    int msg[2] = { step, errno };
    ssize_t w;
    do { w = write(fd, msg, sizeof msg); } while (w < 0 && errno == EINTR);
    _exit(code);
}

// In the child: exec the program with its env. A file with no #! line,
// which execve refuses with ENOEXEC, runs under /bin/sh, as execlp ran it.
static void sandbox_child_exec(const char* path, char** argv, char** sh_argv, char** env) {
    execve(path, argv, env);
    if (errno != ENOEXEC) return;
    execve("/bin/sh", sh_argv, env);
    errno = ENOEXEC;
}

// In the parent: what the child reported, printed as the child used to.
// 1 when it reported a failure: the program never started.
static int sandbox_report(int errfd) {
    int msg[2] = { 0, 0 };
    ssize_t n;
    do { n = read(errfd, msg, sizeof msg); } while (n < 0 && errno == EINTR);
    close(errfd);
    if (n != (ssize_t)sizeof msg) return 0;
    fprintf(stderr, "exec: %s\n", strerror(msg[1]));
    return 1;
}

// Serialize grants from a list to shared memory
// Format: "category:pattern\n" lines, null-terminated
static int serialize_grants(void* grant_list, char* shm_name, size_t shm_name_size) {
    char buf[AETHER_SANDBOX_GRANTS_SIZE];
    size_t data_size = 0;
    if (aether_sandbox_serialize_grants(
            grant_list, buf, sizeof buf, &data_size) != 0) return -1;

    return aether_sandbox_shm_write(
        shm_name, shm_name_size, buf, data_size);
}

// Spawn a sandboxed child process
// Returns: child exit code, or -1 on error
int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg) {
    // Find the preload library
    char preload_path[1024];
    char tried[2048];
    if (!aether_sandbox_find_preload(preload_path, sizeof(preload_path), tried, sizeof(tried))) {
        aether_sandbox_preload_missing("[aether]", tried);
        return -1;
    }

    // Serialize grants to shared memory
    char shm_name[AETHER_SANDBOX_SHM_NAME_SIZE];
    if (serialize_grants(grant_list, shm_name, sizeof shm_name) < 0) {
        fprintf(stderr, "[aether] cannot create shared memory for grants\n");
        return -1;
    }

    // What the child execs with, made here (#3200): the program on this
    // process's PATH, its argv, the /bin/sh argv for a file with no #!
    // line, and the env.
    //
    // Two-layer containment on FreeBSD, both through that env:
    //   1. LD_PRELOAD — intercepts libc for ANY child, including
    //      non-Aether programs (python3, bash). Userspace, cooperative,
    //      bypassable by raw syscalls / static binaries.
    //   2. AETHER_CAPSICUM=1 — if the child is itself an Aether
    //      binary, its startup hook (capsicum_autosandbox.c) calls
    //      cap_enter() after rtld has loaded, giving kernel-enforced,
    //      unbypassable containment. A non-Aether child simply
    //      ignores the variable, falling back to layer 1.
    //
    // We cannot cap_enter() the child from here ourselves:
    // cap_enter() forbids the runtime linker, so a dynamically
    // linked child would fail to exec. Self-sandboxing after exec is
    // the only model that works — see capsicum_autosandbox.c.
    // A program that cannot start is an error (-1), as for std.os (#3170),
    // never an exit of 127, which a program that ran can give.
    char* path = aether_spawn_resolve(program);
    if (!path) {
        fprintf(stderr, "exec: %s\n", strerror(errno));
        shm_unlink(shm_name);
        return -1;
    }
    char* argv[] = { (char*)program, (char*)arg, NULL };
    char* sh_argv[] = { (char*)"/bin/sh", path, (char*)arg, NULL };
    char preload_var[sizeof preload_path + 16];
    char shm_var[sizeof shm_name + 32];
    snprintf(preload_var, sizeof preload_var, "LD_PRELOAD=%s", preload_path);
    snprintf(shm_var, sizeof shm_var, "AETHER_SANDBOX_SHM=%s", shm_name);
    const char* set[4] = { preload_var, shm_var, "AETHER_CAPSICUM=1", NULL };
    int nset = 3;
    if (!getenv("AETHER_SANDBOX_VERBOSE")) set[nset++] = "AETHER_SANDBOX_VERBOSE=0";
    char** env = aether_spawn_env(aether_spawn_environ(), set, nset);
    int errp[2];
    if (!env || aether_pipe_cloexec(errp) != 0) {
        free(env);
        free(path);
        shm_unlink(shm_name);
        return -1;
    }

    pid_t pid = aether_fork();
    if (pid < 0) {
        close(errp[0]);
        close(errp[1]);
        free(env);
        free(path);
        shm_unlink(shm_name);
        return -1;
    }

    if (pid == 0) {
        close(errp[0]);
        sandbox_child_exec(path, argv, sh_argv, env);
        sandbox_child_failed(errp[1], SANDBOX_STEP_EXEC, 127);
    }

    // Parent: report what the child could not do, then wait for it.
    close(errp[1]);
    free(env);
    free(path);
    int not_started = sandbox_report(errp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    // Cleanup shared memory
    shm_unlink(shm_name);

    if (not_started) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

#endif // __FreeBSD__
