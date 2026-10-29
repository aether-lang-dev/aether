// spawn_sandboxed_linux.c — spawn a child process under Aether sandbox (Linux)
//
// Linux backend (companions: spawn_sandboxed_bsd.c, spawn_sandboxed_stub.c).
// Uses fork, shm_open, LD_PRELOAD, and seccomp-bpf (the
// latter for the kernel-level fence on clone/clone3/fork/vfork).
// Other platforms get a stub that returns -1 with a clear message.

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
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
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

// What a child that could not run its program tells the parent, on a
// close-on-exec pipe the exec closes unwritten: the step that failed and its
// errno. The parent prints the reason. The child itself calls nothing
// between fork and exec that is not async-signal-safe (#3200): it used to
// setenv the sandbox's variables, execlp the program and print with perror
// or fprintf there, and a child of a threaded parent may do none of that,
// since another thread may have held malloc's lock or the environment's at
// the fork. The env, the resolved program and both argvs are made in the
// parent instead (runtime/utils/aether_spawn.h).
enum { SANDBOX_STEP_FENCE = 1, SANDBOX_STEP_EXEC = 2 };

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
    if (msg[0] == SANDBOX_STEP_FENCE) {
        fprintf(stderr,
            "[aether] cannot install seccomp clone fence: %s\n"
            "  spawn_sandboxed requires a kernel that supports\n"
            "  PR_SET_NO_NEW_PRIVS + PR_SET_SECCOMP "
            "(Linux 3.5+); or grant 'fork:*' to opt out.\n",
            strerror(msg[1]));
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
            grant_list, buf, sizeof buf, &data_size) != 0) return -1;

    return aether_sandbox_shm_write(
        shm_name, shm_name_size, buf, data_size);
}

// Is the `fork` category granted in the grant list? The LD_PRELOAD layer
// gates clone/fork/vfork at the libc-symbol level, which catches
// cooperative callers but is bypassed by anything that issues the
// underlying syscall directly — including glibc's own __vfork (an
// inline `syscall` instruction with no libc symbol indirection at all)
// and any program calling syscall(SYS_clone3, ...). The seccomp filter
// below is the kernel-level companion; this helper decides whether to
// install it.
//
// Same semantics as the preload's check_grant("fork", "*"): a (fork, *)
// pair anywhere in the list grants fork, anything else denies it. A
// catch-all (*, *) also grants it (matches the preload's special-case).
static int is_fork_granted(void* grant_list) {
    int n = list_size(grant_list);
    if (n <= 0 || n % 2 != 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (!cat || !pat) continue;
        if (cat[0] == '*' && cat[1] == '\0' &&
            pat[0] == '*' && pat[1] == '\0') {
            return 1;
        }
        if (strcmp(cat, "fork") == 0) return 1;
    }
    return 0;
}

// Install a seccomp-bpf filter that refuses the child a new process: fork,
// vfork and a clone that is not a thread get EPERM. Must run AFTER
// prctl(PR_SET_NO_NEW_PRIVS, 1) (required for unprivileged seccomp) and
// BEFORE execve. Returns 0 on success, -1 on failure, and on an
// architecture this filter does not know (the caller fail-closes: the child
// exits 126 rather than running unfenced).
//
// A thread is no new process (#3178): it shares the address space, the
// filter and the preload, and cannot leave them. The fence used to refuse
// every clone, so a child that starts a thread (an Aether program with
// actors, Python with threads, a JVM) failed in pthread_create. Now a clone
// with CLONE_THREAD in its flags (its first argument, on every ABI here)
// passes. clone3 keeps its flags in a struct in memory, which a filter
// cannot read, so it gets ENOSYS, the answer of a kernel without clone3,
// and libc falls back to clone, whose flags the filter can read.
//
// A seccomp filter sees the syscall's ABI (seccomp_data.arch) and its number
// in that ABI, and a process can enter the kernel through more than one ABI.
// So the filter dispatches on arch:
//   - the native ABI traps its own numbers;
//   - on x86_64, i386 (int 0x80, a static 32-bit binary) traps the i386
//     numbers, and x32 calls (native arch, number | 0x40000000) are caught
//     by masking that bit off before comparing;
//   - on aarch64, 32-bit ARM (compat) traps the ARM numbers;
//   - any other arch kills the process: an ABI we did not list is one we
//     cannot fence, and the old behaviour (allow it) let a static i386
//     binary fork freely inside an x86_64 sandbox.
//
// Syscall numbers are ABI literals, not SYS_* macros: those resolve to the
// build host's ABI, and the generic table (arm64, riscv64, loongarch64) has
// no fork/vfork at all. tests/integration/sandbox_clone_fence checks every
// literal below against the kernel headers zig ships for that architecture.
// Each list is clone first, clone3 last, and the plain refusals between.
//   x86_64:  clone 56, fork 57, vfork 58, clone3 435
//   i386 and 32-bit ARM: clone 120, fork 2, vfork 190, clone3 435
//   generic (aarch64, riscv64, loongarch64): clone 220, clone3 435
#ifndef AUDIT_ARCH_AARCH64
#define AUDIT_ARCH_AARCH64     0xC00000B7u
#endif
#ifndef AUDIT_ARCH_ARM
#define AUDIT_ARCH_ARM         0x40000028u
#endif
#ifndef AUDIT_ARCH_RISCV64
#define AUDIT_ARCH_RISCV64     0xC00000F3u
#endif
#ifndef AUDIT_ARCH_LOONGARCH64
#define AUDIT_ARCH_LOONGARCH64 0xC0000102u
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

#define FENCE_MAX 48
#define X32_SYSCALL_BIT 0x40000000u

#define FENCE_CLONE_THREAD 0x00010000u   /* CLONE_THREAD, the same on every ABI */

/* The low 32 bits of a syscall's first argument, where CLONE_THREAD is. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FENCE_ARG0_LOW ((uint32_t)offsetof(struct seccomp_data, args) + 4u)
#else
#define FENCE_ARG0_LOW ((uint32_t)offsetof(struct seccomp_data, args))
#endif

static struct sock_filter fence_jeq(uint32_t k, int pc, int target) {
    /* A jump is relative to the instruction after it. */
    return (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k,
                                        (uint8_t)(target - (pc + 1)), 0);
}

// One arch's block, from `nums` (clone first, clone3 last, plain refusals
// between): load the number (masked when `mask`); a clone goes on to the
// CLONE_THREAD test, clone3 gets ENOSYS, a plain refusal EPERM, anything
// else is allowed.
static int fence_block(struct sock_filter* f, int n, const uint32_t* nums, int count,
                       uint32_t mask) {
    int deny_count = count - 2;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          (uint32_t)offsetof(struct seccomp_data, nr));
    if (mask) f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_AND | BPF_K, mask);
    int p = n;                               /* the first comparison */
    int allow = p + 2 + deny_count;          /* RET ALLOW */
    int check = allow + 1;                   /* LD the clone's flags */
    int jset = check + 1;                    /* JSET CLONE_THREAD */
    int allow_thread = jset + 1;             /* RET ALLOW */
    int deny = allow_thread + 1;             /* RET EPERM */
    int enosys = deny + 1;                   /* RET ENOSYS */
    f[n] = fence_jeq(nums[0], n, check);
    n++;
    f[n] = fence_jeq(nums[count - 1], n, enosys);
    n++;
    for (int i = 1; i <= deny_count; i++) {
        f[n] = fence_jeq(nums[i], n, deny);
        n++;
    }
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FENCE_ARG0_LOW);
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, FENCE_CLONE_THREAD,
                                        (uint8_t)(allow_thread - (n + 1)),
                                        (uint8_t)(deny - (n + 1)));
    n++;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                          SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                          SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA));
    return n;
}

static int install_clone_fence_seccomp(void) {
    static const uint32_t x86_64_nums[]  = { 56, 57, 58, 435 };
    static const uint32_t legacy32_nums[] = { 120, 2, 190, 435 };   /* i386, ARM */
    static const uint32_t generic_nums[] = { 220, 435 };
    (void)x86_64_nums; (void)legacy32_nums; (void)generic_nums;

#if defined(__x86_64__) && !defined(__ILP32__)
    uint32_t native = AUDIT_ARCH_X86_64, compat = AUDIT_ARCH_I386;
    const uint32_t* nnums = x86_64_nums; int ncount = 4; uint32_t nmask = ~X32_SYSCALL_BIT;
    const uint32_t* cnums = legacy32_nums; int ccount = 4;
#elif defined(__aarch64__)
    uint32_t native = AUDIT_ARCH_AARCH64, compat = AUDIT_ARCH_ARM;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = legacy32_nums; int ccount = 4;
#elif defined(__riscv) && __riscv_xlen == 64
    uint32_t native = AUDIT_ARCH_RISCV64, compat = 0;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = NULL; int ccount = 0;
#elif defined(__loongarch64) || (defined(__loongarch__) && defined(__loongarch_lp64))
    uint32_t native = AUDIT_ARCH_LOONGARCH64, compat = 0;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = NULL; int ccount = 0;
#else
    /* No fence for this architecture: refuse, rather than run the child with
     * only the libc-level fence while the caller asked for containment. */
    return -1;
#endif

#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) || defined(__loongarch__)
    /* Assemble: [0] LD arch, [1] JEQ native, [2] JEQ compat (if any),
     * then KILL, then the native block, then the compat block. */
    struct sock_filter nb[FENCE_MAX], cb[FENCE_MAX];
    int nn = fence_block(nb, 0, nnums, ncount, nmask);
    int cn = cnums ? fence_block(cb, 0, cnums, ccount, 0) : 0;

    struct sock_filter f[FENCE_MAX * 2 + 4];
    int n = 0;
    int head = compat ? 4 : 3;            /* LD, JEQ, [JEQ], KILL */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          (uint32_t)offsetof(struct seccomp_data, arch));
    /* jt is relative to the next instruction. */
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, native,
                                        (uint8_t)(head - (n + 1)), 0);
    n++;
    if (compat) {
        f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, compat,
                                            (uint8_t)(head + nn - (n + 1)), 0);
        n++;
    }
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    memcpy(f + n, nb, (size_t)nn * sizeof(nb[0]));
    n += nn;
    if (cn) {
        memcpy(f + n, cb, (size_t)cn * sizeof(cb[0]));
        n += cn;
    }

    struct sock_fprog prog = { .len = (unsigned short)n, .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) return -1;
    return 0;
#endif
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

    // Pre-fork: decide whether the kernel-level clone fence applies.
    // (Reading the list post-fork would race with the child if we ever
    // moved to a model where the parent mutates grants concurrently;
    // doing it once here keeps the contract obvious.)
    int fork_granted = is_fork_granted(grant_list);

    // What the child execs with, made here (#3200): the program on this
    // process's PATH, its argv, the /bin/sh argv for a file with no #!
    // line, and the env with LD_PRELOAD and the grant source in it.
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
    const char* set[3] = { preload_var, shm_var, NULL };
    int nset = 2;
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
        // Kernel-level clone fence: when fork:* is not granted, install
        // a seccomp filter that traps clone/clone3/fork/vfork with
        // EPERM regardless of how they're invoked. Closes the gap
        // where glibc's __vfork (inline `syscall` instruction) and any
        // raw `syscall(SYS_clone3,…)` would otherwise bypass the
        // LD_PRELOAD libc-symbol fence. Fail-closed: if seccomp setup
        // fails (kernel too old, etc.), refuse to exec — the caller
        // asked for containment and we cannot deliver. Building the
        // filter is arithmetic on the stack, and installing it is two
        // prctl calls.
        if (!fork_granted && install_clone_fence_seccomp() != 0)
            sandbox_child_failed(errp[1], SANDBOX_STEP_FENCE, 126);
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

#endif // __linux__
// Non-Linux backends live in spawn_sandboxed_bsd.c / spawn_sandboxed_stub.c.
