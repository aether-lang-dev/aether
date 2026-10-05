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
// failure (the caller fail-closes).
//
// The filter is multi-arch-aware: it checks seccomp_data.arch and only
// applies the trap on x86_64; other architectures fall through to
// SECCOMP_RET_ALLOW so this never breaks ARM/RISC-V/etc. (where the
// LD_PRELOAD-level libc wrappers remain the only fence — a known and
// documented gap). The x86_64 fence is the value-add: that's the only
// arch where the issue reporter's repro lives (gcc/clone3).
//
// Syscall numbers below are **x86_64 ABI literals**, intentionally not
// the symbolic SYS_* names. Two reasons:
//   1. We're filtering syscalls *of the x86_64 ABI* (the arch check
//      above gates this), so the architecturally correct numbers are
//      x86_64's regardless of what arch we're compiling on.
//   2. The portable SYS_* macros would resolve to the *build host's*
//      ABI numbers — broken when cross-compiling for an arch that uses
//      different numbers, AND on architectures like RISC-V that simply
//      have no SYS_fork / SYS_vfork (only clone/clone3) the code
//      doesn't even compile.
// x86_64 ABI: clone=56, fork=57, vfork=58, clone3=435. These are stable
// kernel ABI and will not change.
static int install_clone_fence_seccomp(void) {
    // SECCOMP_RET_ERRNO returns an errno in the low 16 bits.
    #define DENY_EPERM (SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
    #define ALLOW      SECCOMP_RET_ALLOW

    #define NR_X86_64_CLONE   56
    #define NR_X86_64_FORK    57
    #define NR_X86_64_VFORK   58
    #define NR_X86_64_CLONE3  435

    struct sock_filter filter[] = {
        // Load arch from seccomp_data; if not x86_64, allow.
        BPF_STMT(BPF_LD  | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, ALLOW),

        // Load syscall number.
        BPF_STMT(BPF_LD  | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, nr)),

        // Four traps, fall-through allows everything else.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_X86_64_CLONE,  4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_X86_64_CLONE3, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_X86_64_FORK,   2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_X86_64_VFORK,  1, 0),
        BPF_STMT(BPF_RET | BPF_K, ALLOW),
        BPF_STMT(BPF_RET | BPF_K, DENY_EPERM),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) return -1;
    return 0;

    #undef DENY_EPERM
    #undef ALLOW
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
