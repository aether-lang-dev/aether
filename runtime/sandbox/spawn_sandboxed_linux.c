// spawn_sandboxed_linux.c — spawn a child process under Aether sandbox (Linux)
//
// Linux backend (companions: spawn_sandboxed_bsd.c, spawn_sandboxed_stub.c).
// Uses fork, shm_open, LD_PRELOAD, and the kernel layer in
// aether_sandbox_kernel_linux.c (seccomp-bpf).
// Other platforms get a stub that returns -1 with a clear message.
//
// sandbox.spawn_socket's Linux half is here too: os.run_socket's channel
// (std/os/aether_os.c, through aether_os_run_socket_hooked) with this
// file's containment.

// Android: bionic has no shm_open, and an app has no LD_PRELOAD to fence;
// it takes the stub in spawn_sandboxed_stub.c.
#if defined(__linux__) && !defined(__ANDROID__)

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>

// From libaether.a — list operations
extern int list_size(void*);
extern void* list_get_raw(void*, int);
// String-ABI accessor: list_get_raw may return a magic AetherString* or
// a raw const char*; aether_string_data unwraps both to a plain
// C-string. Required for argv / grant-pattern reads — see issue #688.
extern const char* aether_string_data(const void*);

// The preload library's lookup, shared with the contrib process hosts.
#include "aether_sandbox_preload_path.h"
#include "aether_sandbox_grants.h"
#include "aether_sandbox_shm.h"
// The close-on-exec pipe, the fork, and what a child execs with.
#include "../utils/aether_spawn.h"
// The kernel layer both spawns share.
#include "aether_sandbox_kernel.h"

// What a child that could not run its program tells the parent, on a
// close-on-exec pipe the exec closes unwritten: the step that failed and its
// errno. The parent prints the reason. The child itself calls nothing
// between fork and exec that is not async-signal-safe (#3200): it used to
// setenv the sandbox's variables, execlp the program and print with perror
// or fprintf there, and a child of a threaded parent may do none of that,
// since another thread may have held malloc's lock or the environment's at
// the fork. The env, the resolved program and both argvs are made in the
// parent instead (runtime/utils/aether_spawn.h).
// A kernel-layer step is reported as SANDBOX_STEP_KERNEL + its
// AETHER_SBX_STEP_*.
enum { SANDBOX_STEP_EXEC = 2, SANDBOX_STEP_KERNEL = 100 };

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
    if (msg[0] > SANDBOX_STEP_KERNEL) {
        char why[256];
        fprintf(stderr, "[aether] spawn_sandboxed: %s\n",
                aether_sandbox_kernel_failure(msg[0] - SANDBOX_STEP_KERNEL, msg[1],
                                              why, sizeof why));
    } else {
        fprintf(stderr, "exec: %s\n", strerror(msg[1]));
    }
    return 1;
}

// Serialize grants from a list to shared memory
// Format: "category:pattern\n" lines, null-terminated
static int serialize_grants(void* grant_list, char* shm_name, size_t shm_name_size) {
    char buf[AETHER_SANDBOX_GRANTS_SIZE];
    size_t data_size = 0;
    if (aether_sandbox_serialize_grants(
            grant_list, buf, sizeof buf, &data_size) != 0)
        return aether_sandbox_grants_err_code(errno);

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
    int grants_rc = serialize_grants(grant_list, shm_name, sizeof shm_name);
    if (grants_rc < 0) {
        fprintf(stderr, "[aether] %s\n", aether_sandbox_grants_err_msg(grants_rc));
        return -1;
    }

    // Pre-fork: the kernel layer, built from the grants (the child only
    // installs it). Reading the list post-fork would race with a parent
    // that mutates it on another thread.
    AetherSandboxKernel* kernel = (AetherSandboxKernel*)malloc(sizeof *kernel);
    char kerr[512];
    if (!kernel) {
        shm_unlink(shm_name);
        return -1;
    }
    if (aether_sandbox_kernel_prepare(kernel, grant_list, 0, kerr, sizeof kerr) != 0) {
        fprintf(stderr, "[aether] spawn_sandboxed: %s\n", kerr);
        free(kernel);
        shm_unlink(shm_name);
        return -1;
    }

    // What the child execs with, made here (#3200): the program on this
    // process's PATH, its argv, the /bin/sh argv for a file with no #!
    // line, and the env with LD_PRELOAD and the grant source in it.
    // A program that cannot start is an error (-1), as for std.os (#3170),
    // never an exit of 127, which a program that ran can give.
    char* path = aether_spawn_resolve(program);
    if (!path) {
        fprintf(stderr, "exec: %s\n", strerror(errno));
        aether_sandbox_kernel_release(kernel);
        free(kernel);
        shm_unlink(shm_name);
        return -1;
    }
    char* argv[] = { (char*)program, (char*)arg, NULL };
    char* sh_argv[] = { (char*)"/bin/sh", path, (char*)arg, NULL };
    char preload_var[sizeof preload_path + 16];
    char shm_var[sizeof shm_name + 32];
    snprintf(preload_var, sizeof preload_var, "LD_PRELOAD=%s", preload_path);
    snprintf(shm_var, sizeof shm_var, "AETHER_SANDBOX_SHM=%s", shm_name);
    const char* set[3] = { preload_var, shm_var, NULL };
    int nset = 2;
    if (!getenv("AETHER_SANDBOX_VERBOSE")) set[nset++] = "AETHER_SANDBOX_VERBOSE=0";
    char** env = aether_spawn_env(aether_spawn_environ(), set, nset);
    int errp[2];
    if (!env || aether_pipe_cloexec(errp) != 0) {
        free(env);
        free(path);
        aether_sandbox_kernel_release(kernel);
        free(kernel);
        shm_unlink(shm_name);
        return -1;
    }

    pid_t pid = aether_fork();
    if (pid < 0) {
        close(errp[0]);
        close(errp[1]);
        free(env);
        free(path);
        aether_sandbox_kernel_release(kernel);
        free(kernel);
        shm_unlink(shm_name);
        return -1;
    }

    if (pid == 0) {
        close(errp[0]);
        // The kernel layer (aether_sandbox_kernel_linux.c), built in the
        // parent: the seccomp filter that holds what the preload cannot
        // see, a raw syscall instruction among it (glibc's own __vfork is
        // one). Fail-closed: if it cannot be installed, refuse to exec, since
        // the caller asked for containment and we cannot deliver it.
        // Installing it is prctl calls only.
        int kstep = aether_sandbox_kernel_apply(kernel);
        if (kstep != 0)
            sandbox_child_failed(errp[1], SANDBOX_STEP_KERNEL + kstep, 126);
        sandbox_child_exec(path, argv, sh_argv, env);
        sandbox_child_failed(errp[1], SANDBOX_STEP_EXEC, 127);
    }

    // Parent: report what the child could not do, then wait for it.
    close(errp[1]);
    free(env);
    free(path);
    aether_sandbox_kernel_release(kernel);
    free(kernel);
    int not_started = sandbox_report(errp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    // Cleanup shared memory
    shm_unlink(shm_name);

    if (not_started) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// ---- sandbox.spawn_socket ----
//
// os.run_socket's channel (fd 3, AETHER_IPC_FD=3, AETHER_IPC_KIND=socket),
// through aether_os_run_socket_hooked, with this file's containment: the
// preload library and the kernel layer, held to the grants. Unlike
// spawn_sandboxed it returns at once, with the parent's end of the socket
// and the child's pid, which the caller reaps (os.wait_pid).
//
// It fails closed. Anything that stops the child being contained as asked
// (no preload library, a grant list that cannot be sent, a kernel layer
// that cannot be built or installed) is an error, and no child is started.
//
// The grants travel on a sealed memfd the child keeps open, not on a named
// shared-memory object as spawn_sandboxed's do: spawn_sandboxed removes its
// object once the child exits, and this call does not wait for that. A memfd
// has no name to remove, its seals stop the child writing to it, and an
// exec the child is granted reads the same grants from it again.

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;

static __thread char spawn_socket_err[1024];

static const char* spawn_socket_fail(const char* fmt, const char* detail) {
    snprintf(spawn_socket_err, sizeof spawn_socket_err, fmt, detail);
    return spawn_socket_err;
}

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (1024 + 9)
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008
#endif

// The grants on a sealed memfd, at a descriptor of 10 or above (clear of the
// child's 0 to 3 and of what the spawn lifts there), close-on-exec here; the
// spawn's keep_fd clears that in the child. -1 with errno set.
static int grants_memfd(const char* data, size_t len) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "aether-sandbox-grants",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    int fd = -1;
    errno = ENOSYS;
#endif
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { int e = errno; close(fd); errno = e ? e : EIO; return -1; }
        off += (size_t)w;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    int e = errno;
    close(fd);
    errno = e;
    return high;
}

typedef struct {
    AetherSandboxKernel kernel;
    char reason[512];
} SpawnSocketCtx;

static int spawn_socket_child(void* ctx) {
    return aether_sandbox_kernel_apply(&((SpawnSocketCtx*)ctx)->kernel);
}

static const char* spawn_socket_failed(void* ctx, int step, int err) {
    SpawnSocketCtx* c = (SpawnSocketCtx*)ctx;
    if (step == 0) {
        if (err == 0) return spawn_socket_fail("sandbox.spawn_socket: %s", c->reason);
        return spawn_socket_fail("sandbox.spawn_socket: cannot hand the child its grants: %s",
                                 strerror(err));
    }
    char why[512];
    return spawn_socket_fail("sandbox.spawn_socket: %s; nothing was started",
                             aether_sandbox_kernel_failure(step, err, why, sizeof why));
}

_tuple_int_int_string aether_sandbox_spawn_socket_raw(void* grant_list, const char* prog,
                                                      void* argv_list, void* env_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    if (!grant_list) {
        out._2 = "sandbox.spawn_socket: no grant list (make one with sandbox.new)";
        return out;
    }
    if (!prog || !prog[0]) {
        out._2 = "sandbox.spawn_socket: no program";
        return out;
    }

    char preload_path[1024];
    char tried[2048];
    if (!aether_sandbox_find_preload(preload_path, sizeof preload_path, tried, sizeof tried)) {
        out._2 = spawn_socket_fail(
            "sandbox.spawn_socket: cannot find " AETHER_SANDBOX_PRELOAD_NAME
            " (looked in %s); nothing was started", tried);
        return out;
    }

    char buf[AETHER_SANDBOX_GRANTS_SIZE];
    size_t data_size = 0;
    if (aether_sandbox_serialize_grants(grant_list, buf, sizeof buf, &data_size) != 0) {
        out._2 = spawn_socket_fail("sandbox.spawn_socket: %s",
                                   aether_sandbox_grants_err_msg(aether_sandbox_grants_err_code(errno)));
        return out;
    }

    SpawnSocketCtx* ctx = (SpawnSocketCtx*)calloc(1, sizeof *ctx);
    if (!ctx) {
        out._2 = "sandbox.spawn_socket: out of memory";
        return out;
    }
    if (aether_sandbox_kernel_prepare(&ctx->kernel, grant_list, AETHER_SBX_STRICT,
                                      ctx->reason, sizeof ctx->reason) != 0) {
        out._2 = spawn_socket_fail("sandbox.spawn_socket: %s; nothing was started", ctx->reason);
        free(ctx);
        return out;
    }

    // The serialized form ends in its NUL (data_size counts it); the
    // preload reads up to it.
    int gfd = grants_memfd(buf, data_size);
    if (gfd < 0) {
        out._2 = spawn_socket_fail("sandbox.spawn_socket: cannot make the grants' memfd: %s",
                                   strerror(errno));
        aether_sandbox_kernel_release(&ctx->kernel);
        free(ctx);
        return out;
    }

    char preload_var[sizeof preload_path + 16];
    char fd_var[48];
    snprintf(preload_var, sizeof preload_var, "LD_PRELOAD=%s", preload_path);
    snprintf(fd_var, sizeof fd_var, "AETHER_SANDBOX_GRANTS_FD=%d", gfd);
    const char* set[4] = { preload_var, fd_var, NULL, NULL };
    int nset = 2;
    if (!getenv("AETHER_SANDBOX_VERBOSE")) set[nset++] = "AETHER_SANDBOX_VERBOSE=0";

    AetherSpawnHook hook;
    memset(&hook, 0, sizeof hook);
    hook.env = set;
    hook.nenv = nset;
    hook.keep_fd = gfd;
    hook.child = spawn_socket_child;
    hook.failed = spawn_socket_failed;
    hook.ctx = ctx;

    int fd = -1, pid = -1;
    const char* err = "";
    aether_os_run_socket_hooked(prog, argv_list, env_list, &hook, &fd, &pid, &err);
    close(gfd);
    aether_sandbox_kernel_release(&ctx->kernel);
    free(ctx);
    out._0 = fd;
    out._1 = pid;
    out._2 = err ? err : "";
    return out;
}

#endif // __linux__
// Non-Linux backends live in spawn_sandboxed_bsd.c / spawn_sandboxed_stub.c.
