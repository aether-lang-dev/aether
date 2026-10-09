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

// Find libaether_sandbox.so next to the running binary
static int find_preload_path(char* buf, int bufsize) {
    // Try /proc/self/exe to find our binary's directory
    // Leave room for suffix ("build/libaether_sandbox.so" = 26 chars)
    char exe[512];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) {
        // Fallback: look in current directory
        snprintf(buf, bufsize, "./libaether_sandbox.so");
        return access(buf, F_OK) == 0;
    }
    exe[len] = '\0';

    // Strip binary name, keep directory
    char* slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';

    snprintf(buf, bufsize, "%slibaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    // Try ../build/
    if (slash) *slash = '\0';
    slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';
    snprintf(buf, bufsize, "%sbuild/libaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    return 0;
}

// Serialize grants from a list to shared memory
// Format: "category:pattern\n" lines, null-terminated
static char shm_name[64];

static int serialize_grants(void* grant_list) {
    int n = list_size(grant_list);
    if (n <= 0 || n % 2 != 0) return -1;

    // Build the grant string
    char buf[8192];
    int pos = 0;
    for (int i = 0; i < n && pos < 8000; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (!cat || !pat) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s:%s\n", cat, pat);
    }

    // Write to shared memory
    snprintf(shm_name, sizeof(shm_name), "/aether_sandbox_%d", getpid());
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;

    if (ftruncate(fd, pos + 1) != 0) {
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }
    void* mem = mmap(NULL, pos + 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return -1; }

    memcpy(mem, buf, pos + 1);
    munmap(mem, pos + 1);
    close(fd);

    return 0;
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

// Install a seccomp-bpf filter that traps clone/clone3/fork/vfork with
// EPERM. Must run AFTER prctl(PR_SET_NO_NEW_PRIVS, 1) (required for
// unprivileged seccomp) and BEFORE execve. Returns 0 on success, -1 on
// failure, and on an architecture this filter does not know (the caller
// fail-closes: the child exits 126 rather than running unfenced).
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

// One arch's block: load the number (masked when `mask`), deny on a match,
// otherwise allow.
static int fence_block(struct sock_filter* f, int n, const uint32_t* nums, int count,
                       uint32_t mask) {
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          (uint32_t)offsetof(struct seccomp_data, nr));
    if (mask) f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_AND | BPF_K, mask);
    for (int i = 0; i < count; i++)
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nums[i],
                                              (uint8_t)(count - i), 0);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                          SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
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
    if (!find_preload_path(preload_path, sizeof(preload_path))) {
        fprintf(stderr, "[aether] cannot find libaether_sandbox.so\n");
        return -1;
    }

    // Serialize grants to shared memory
    if (serialize_grants(grant_list) < 0) {
        fprintf(stderr, "[aether] cannot create shared memory for grants\n");
        return -1;
    }

    // Pre-fork: decide whether the kernel-level clone fence applies.
    // (Reading the list post-fork would race with the child if we ever
    // moved to a model where the parent mutates grants concurrently;
    // doing it once here keeps the contract obvious.)
    int fork_granted = is_fork_granted(grant_list);

    pid_t pid = fork();
    if (pid < 0) {
        shm_unlink(shm_name);
        return -1;
    }

    if (pid == 0) {
        // Child: set up LD_PRELOAD and grant source, then exec
        setenv("LD_PRELOAD", preload_path, 1);
        setenv("AETHER_SANDBOX_SHM", shm_name, 1);
        setenv("AETHER_SANDBOX_VERBOSE", "0", 0);

        // Kernel-level clone fence: when fork:* is not granted, install
        // a seccomp filter that traps clone/clone3/fork/vfork with
        // EPERM regardless of how they're invoked. Closes the gap
        // where glibc's __vfork (inline `syscall` instruction) and any
        // raw `syscall(SYS_clone3,…)` would otherwise bypass the
        // LD_PRELOAD libc-symbol fence. Fail-closed: if seccomp setup
        // fails (kernel too old, etc.), refuse to exec — the caller
        // asked for containment and we cannot deliver.
        if (!fork_granted) {
            if (install_clone_fence_seccomp() != 0) {
                fprintf(stderr,
                    "[aether] cannot install seccomp clone fence: %s\n"
                    "  spawn_sandboxed requires a kernel that supports\n"
                    "  PR_SET_NO_NEW_PRIVS + PR_SET_SECCOMP "
                    "(Linux 3.5+); or grant 'fork:*' to opt out.\n",
                    strerror(errno));
                _exit(126);
            }
        }

        if (arg) {
            execlp(program, program, arg, NULL);
        } else {
            execlp(program, program, NULL);
        }
        perror("exec");
        _exit(127);
    }

    // Parent: wait for child
    int status = 0;
    waitpid(pid, &status, 0);

    // Cleanup shared memory
    shm_unlink(shm_name);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

#endif // __linux__
// Non-Linux backends live in spawn_sandboxed_bsd.c / spawn_sandboxed_stub.c.
