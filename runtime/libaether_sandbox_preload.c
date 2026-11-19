// libaether_sandbox_preload.c — LD_PRELOAD library for cross-process containment
//
// Intercepts libc calls (open, connect, the exec family, posix_spawn,
// system and popen, getenv) and checks
// against an Aether grant list loaded from a file.
//
// Grant file format (one per line): category:pattern
//   tcp:*.example.com
//   fs_read:/app/data/*
//   env:HOME
//   *:*

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>            // PATH_MAX (used by realpath fallback)
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/auxv.h>            // getauxval(AT_BASE): the loader, for dlopen
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <spawn.h>
#include <stddef.h>              // offsetof: scandir's copies
#include <dirent.h>
#include <glob.h>
#include <ftw.h>
#include <utime.h>
#include <sys/time.h>
#if defined(__linux__)
#include <sys/xattr.h>
#include <sys/mount.h>
#endif
// fts: glibc and the BSDs ship it; musl does not.
#if defined(__GLIBC__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__APPLE__)
#include <fts.h>
#define AETHER_PRELOAD_HAS_FTS 1
#else
#define AETHER_PRELOAD_HAS_FTS 0
#endif
#if !defined(__OpenBSD__)
#include <wordexp.h>             // OpenBSD's libc has no wordexp to intercept
#endif
#if defined(__FreeBSD__)
#include <sys/user.h>            // struct kinfo_file, for fexecve's F_KINFO
#include <sys/syscall.h>         // SYS_fork, which libthr's fork issues itself
#endif

// Grant storage
#define MAX_GRANTS 256
#define MAX_GRANT_SOURCE_SIZE 8192
static struct { char cat[64]; char pat[256]; } grants[MAX_GRANTS];
static int grant_count = 0;
static int sandbox_active = 0;

// Logging: file (default), stderr, or none
// Set via AETHER_SANDBOX_LOG=file|stderr|none
#define LOG_FILE   0
#define LOG_STDERR 1
#define LOG_NONE   2
static int log_mode = LOG_FILE;
static FILE* log_file = NULL;

// Real libc functions (resolved via dlsym)
static int (*real_open)(const char*, int, ...) = NULL;
static int (*real_connect)(int, const struct sockaddr*, socklen_t) = NULL;
static char* (*real_getenv)(const char*) = NULL;
static int (*real_execve)(const char*, char*const[], char*const[]) = NULL;
static FILE* (*real_fopen)(const char*, const char*) = NULL;
static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
static int (*real_mprotect)(void*, size_t, int) = NULL;
static void* (*real_dlopen)(const char*, int) = NULL;

// For fs_* categories, the resource is matched where it leads, resolved
// component by component (".." and symlinks) by aether_sandbox_path.h, the
// same code the in-process checks use. A path that cannot be resolved (a
// dangling symlink, a loop) is refused rather than matched as written.
#include "aether_sandbox_path.h"

// Pattern matching: the shared matcher (aether_sandbox_match.h), the same one
// the in-process checker and the host bridges use.
#include "aether_sandbox_match.h"

// The calling thread's in-process grant levels first (embedded mode: an
// Aether enforce block, a host bridge's run_sandboxed), then the file-based
// grants (LD_PRELOAD mode). Weak: a process with no Aether runtime has only
// the file. 1 allowed, 0 refused, -1 no level in force on this thread
// (#2955; it used to read a process-wide checker pointer, which one thread
// leaving its enforce cleared under another).
extern int aether_sandbox_check_resolved(const char* category, const char* resource) __attribute__((weak));
extern int aether_sandbox_levels_here(void) __attribute__((weak));
// The file a temporary std's fs.write_atomic is creating stands for, on this
// thread (aether_sandbox.h, aether_sandbox_stage); NULL for any other name.
extern const char* aether_sandbox_staged_for(const char* tmp) __attribute__((weak));

static void log_deny(const char* category, const char* resource) {
    if (log_mode == LOG_NONE) return;
    if (log_mode == LOG_STDERR) {
        fprintf(stderr, "AETHER_DENIED: %s %s\n", category, resource);
    } else if (log_mode == LOG_FILE) {
        if (!log_file) {
            log_file = real_fopen ? real_fopen("aether-sandbox.log", "a")
                                  : fopen("aether-sandbox.log", "a");
        }
        if (log_file) {
            fprintf(log_file, "AETHER_DENIED: %s %s\n", category, resource);
            fflush(log_file);
        }
    }
}

// The grant check proper, for a resource already resolved (an fs path from
// aether_sandbox_canon_path or aether_sandbox_canon_entry): the calling
// thread's in-process levels, then the grant file's.
static int check_resolved(const char* category, const char* resource) {
    // An embedded policy and preload grants are independent boundaries.
    // When both are active, the operation must satisfy both policies.
    if (aether_sandbox_check_resolved) {
        int result = aether_sandbox_check_resolved(category, resource);
        if (result == 0) {
            log_deny(category, resource);
            return 0;
        }
    }
    // Apply file/shared-memory grants as the second policy when active.
    if (!sandbox_active) return 1;
    for (int i = 0; i < grant_count; i++) {
        if (grants[i].cat[0] == '*' && grants[i].pat[0] == '*') return 1;
        if (strcmp(grants[i].cat, category) == 0) {
            if (aether_grant_match(category, grants[i].pat, resource)) return 1;
        }
    }
    log_deny(category, resource);
    return 0;
}

// Would the policy grant `category` on the resolved `resource`? Asked to
// see what a hard link's new name would reach, so a no is not logged. The
// in-process levels answer through their audit, as for any check.
static int grant_covers(const char* category, const char* resource) {
    if (aether_sandbox_check_resolved && aether_sandbox_check_resolved(category, resource) == 0)
        return 0;
    if (!sandbox_active) return 1;
    for (int i = 0; i < grant_count; i++) {
        if (grants[i].cat[0] == '*' && grants[i].pat[0] == '*') return 1;
        if (strcmp(grants[i].cat, category) == 0 &&
            aether_grant_match(category, grants[i].pat, resource)) return 1;
    }
    return 0;
}

static int check_grant(const char* category, const char* resource) {
    // For fs_* categories, resolve the path BEFORE matching so a grant
    // for `/tmp/*` can't be subverted with `/tmp/../etc/shadow` or a
    // symlink under /tmp. tcp/env/exec resources stay as they are: realpath
    // is meaningful only for filesystem paths.
    if (category && strncmp(category, "fs_", 3) == 0 && resource) {
        char resolved[PATH_MAX];
        if (!aether_sandbox_canon_path(resource, resolved, sizeof(resolved))) {
            int in_process = aether_sandbox_levels_here ? aether_sandbox_levels_here() : 0;
            if (in_process > 0 || sandbox_active) {
                log_deny(category, resource);
                return 0;
            }
            return 1;
        }
        return check_resolved(category, resolved);
    }
    return check_resolved(category, resource);
}

// Parse grant lines from a buffer (shared by file and shm paths)
static void parse_grants(const char* data, int len) {
    char line[320];
    int lpos = 0;
    for (int i = 0; i <= len && grant_count < MAX_GRANTS; i++) {
        int at_end = i == len;
        if (at_end || data[i] == '\n' || data[i] == '\0') {
            line[lpos] = '\0';
            if (lpos > 0 && line[0] != '#') {
                char* colon = strchr(line, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(grants[grant_count].cat, line, 63);
                    grants[grant_count].cat[63] = '\0';
                    strncpy(grants[grant_count].pat, colon + 1, 255);
                    grants[grant_count].pat[255] = '\0';
                    grant_count++;
                }
            }
            lpos = 0;
            if (at_end || data[i] == '\0') break;
        } else if (lpos < 319) {
            line[lpos++] = data[i];
        }
    }
}

extern char** environ;
static void snapshot_sandbox_env(void);

// ---------------------------------------------------------------------------
// The kernel's hold on a permanent policy (#3751)
// ---------------------------------------------------------------------------
//
// A grant file (or the shared memory or descriptor spawn_sandboxed hands
// over) that holds the line "policy:permanent" says the policy is this
// process's for its whole life: nothing will lift it. The constructor then
// asks Landlock to hold the same file grants for the whole process, before
// main runs, so a call that goes round libc (a raw syscall instruction, a
// static helper it starts) meets the kernel too:
//   fs_read, fs_write, fs and exec grants become Landlock rules, as the
//   kernel layer of a sandboxed spawn makes them (aether_landlock.h);
//   tcp grants with a port become connect rules on Landlock ABI 4 and up,
//   and with no tcp grant at all connect is refused; a tcp grant with no
//   port, or a tcp_listen grant, leaves that right to the wrappers;
//   the floor a running program needs is added: the loader's cache and its
//   libraries' directory, the locale archive, /dev/null and /dev/urandom.
// Without the line nothing changes: an in-process enforce block, which is
// entered and left, cannot be held this way (Landlock cannot be lifted), and
// a grant file written before #3751 keeps working as it did.
//
// Where the kernel has no Landlock, or a grant cannot be written as a
// Landlock rule (a suffix pattern such as "*.log"), the policy holds at libc
// only, and the log says so. The restriction needs PR_SET_NO_NEW_PRIVS: a
// set-user-ID program this process starts no longer gains its owner's
// rights.
#if defined(__linux__) && !defined(__ANDROID__)
#include "sandbox/aether_landlock.h"
#include <sys/prctl.h>

static const char* exec_path(void);

// A line in the log about the sandbox itself (the denials' sink).
static void log_note(const char* what) {
    if (log_mode == LOG_NONE) return;
    if (log_mode == LOG_STDERR) {
        fprintf(stderr, "AETHER_SANDBOX: %s\n", what);
        return;
    }
    if (!log_file) log_file = real_fopen ? real_fopen("aether-sandbox.log", "a") : NULL;
    if (log_file) {
        fprintf(log_file, "AETHER_SANDBOX: %s\n", what);
        fflush(log_file);
    }
}

// The program a bare name runs, as execvp finds it on PATH; malloc'd, or
// NULL when there is none.
static char* ll_resolve(const char* word) {
    const char* p = exec_path();
    size_t wl = strlen(word);
    for (;;) {
        const char* colon = strchr(p, ':');
        size_t dl = colon ? (size_t)(colon - p) : strlen(p);
        char* cand = (char*)malloc(dl + 2 + wl + 1);
        if (!cand) return NULL;
        if (dl) {
            memcpy(cand, p, dl);
            cand[dl] = '/';
            memcpy(cand + dl + 1, word, wl + 1);
        } else {
            memcpy(cand, word, wl + 1);
        }
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, X_OK) == 0) return cand;
        free(cand);
        if (!colon) return NULL;
        p = colon + 1;
    }
}

static int grant_is(int i, const char* cat, const char* pat) {
    return strcmp(grants[i].cat, cat) == 0 && strcmp(grants[i].pat, pat) == 0;
}

static void landlock_backstop(void) {
    int permanent = 0;
    for (int i = 0; i < grant_count; i++) {
        if (grant_is(i, "*", "*")) return;   // grant_all: nothing to hold
        if (grant_is(i, "policy", "permanent")) permanent = 1;
    }
    if (!permanent) return;
    int abi = aether_landlock_abi();
    if (abi <= 0) {
        log_note("landlock: this kernel has none (Linux 5.13+, landlock in its lsm= list); "
                 "the permanent policy holds at libc only");
        return;
    }
    uint64_t fs = LL_FS_V1 | (abi >= 2 ? LL_FS_REFER : 0) | (abi >= 3 ? LL_FS_TRUNCATE : 0);
    uint64_t net = abi >= 4 ? (LL_NET_BIND_TCP | LL_NET_CONNECT_TCP) : 0;
    for (int i = 0; i < grant_count; i++) {
        char host[256];
        if (strcmp(grants[i].cat, "tcp_listen") == 0) net &= ~LL_NET_BIND_TCP;
        if (strcmp(grants[i].cat, "tcp") == 0 &&
            aether_split_host_port(grants[i].pat, host, sizeof host) < 0)
            net &= ~LL_NET_CONNECT_TCP;
    }
    struct ll_ruleset_attr attr = { .handled_access_fs = fs, .handled_access_net = net, .scoped = 0 };
    long rfd = aether_landlock_syscall(LL_CREATE_RULESET, (long)&attr, abi >= 4 ? 16L : 8L, 0);
    if (rfd < 0) {
        log_note("landlock: cannot make a ruleset; the permanent policy holds at libc only");
        return;
    }
    AetherLandlock l = { (int)rfd, fs };
    char why[600];
    why[0] = '\0';
    for (int i = 0; i < grant_count && !why[0]; i++) {
        const char* cat = grants[i].cat;
        const char* pat = grants[i].pat;
        int rc = 0;
        if (strcmp(cat, "fs_read") == 0) rc = aether_landlock_pattern(&l, pat, LL_READ);
        else if (strcmp(cat, "fs_write") == 0) rc = aether_landlock_pattern(&l, pat, LL_WRITE);
        else if (strcmp(cat, "fs") == 0) rc = aether_landlock_pattern(&l, pat, LL_READ | LL_WRITE);
        else if (strcmp(cat, "exec") == 0) rc = aether_landlock_exec_pattern(&l, pat, ll_resolve);
        else if (strcmp(cat, "tcp") == 0 && (net & LL_NET_CONNECT_TCP)) {
            char host[256];
            int port = aether_split_host_port(pat, host, sizeof host);
            struct ll_net_port_attr np = { .allowed_access = LL_NET_CONNECT_TCP, .port = (uint64_t)port };
            if (port >= 0 && aether_landlock_syscall(LL_ADD_RULE, (long)rfd, LL_RULE_NET_PORT, (long)&np) != 0)
                rc = -1;
        }
        if (rc == 1)
            snprintf(why, sizeof why, "landlock: the %s grant '%s' cannot be a Landlock rule; "
                     "the permanent policy holds at libc only", cat, pat);
        else if (rc < 0)
            snprintf(why, sizeof why, "landlock: no rule for the %s grant '%s' (%s); "
                     "the permanent policy holds at libc only", cat, pat, strerror(errno));
    }
    // The floor a running program needs past its own image
    // (aether_landlock_floor, the spawn's too), with this library and the
    // grant file, which a child it starts reads before its main to keep the
    // policy, and the shared memory the grants came from; the directory of
    // its C library (a later dlopen); with an exec grant, the interpreter
    // this program was started with, which a child built the same way needs
    // (an exec grant naming a program adds that program's own); with a tcp
    // grant that names a host, what the C library reads to resolve it.
    char self_so[PATH_MAX];
    const char* preload_path = NULL;
    Dl_info self_info;
    if (dladdr((void*)landlock_backstop, &self_info) && self_info.dli_fname &&
        realpath(self_info.dli_fname, self_so))
        preload_path = self_so;
    const char* grants_path = real_getenv ? real_getenv("AETHER_SANDBOX_GRANTS") : NULL;
    const char* failed = NULL;
    if (!why[0] && aether_landlock_floor(&l, preload_path, grants_path, &failed) != 0)
        snprintf(why, sizeof why, "landlock: no rule for %s (%s); the permanent policy holds at libc only",
                 failed, strerror(errno));
    const char* shm = real_getenv ? real_getenv("AETHER_SANDBOX_SHM") : NULL;
    if (!why[0] && shm && shm[0]) {
        char shm_path[PATH_MAX];
        int w = snprintf(shm_path, sizeof shm_path, "/dev/shm/%s", shm[0] == '/' ? shm + 1 : shm);
        if (w > 0 && (size_t)w < sizeof shm_path && aether_landlock_rule(&l, shm_path, LL_FS_READ_FILE) != 0)
            snprintf(why, sizeof why, "landlock: no rule for %s; the permanent policy holds at libc only",
                     shm_path);
    }
    int has_exec = 0, names_host = 0;
    for (int i = 0; i < grant_count; i++) {
        if (strcmp(grants[i].cat, "exec") == 0) has_exec = 1;
        if (strcmp(grants[i].cat, "tcp") == 0 && aether_landlock_names_host(grants[i].pat)) names_host = 1;
    }
    if (!why[0] && has_exec && aether_landlock_interp(&l, "/proc/self/exe") < 0)
        snprintf(why, sizeof why, "landlock: no rule for this program's interpreter (%s); the permanent "
                 "policy holds at libc only", strerror(errno));
    if (!why[0] && names_host && aether_landlock_resolver(&l, &failed) != 0)
        snprintf(why, sizeof why, "landlock: no rule for %s (%s); the permanent policy holds at libc only",
                 failed, strerror(errno));
    Dl_info info;
    void* in_libc = (void*)real_open;
    if (!why[0] && real_open && dladdr(in_libc, &info) && info.dli_fname) {
        char real[PATH_MAX];
        if (realpath(info.dli_fname, real)) {
            char* slash = strrchr(real, '/');
            if (slash && slash != real) {
                *slash = '\0';
                if (aether_landlock_rule(&l, real, LL_FS_READ_FILE) != 0)
                    snprintf(why, sizeof why, "landlock: no rule for %s; the permanent policy holds at "
                             "libc only", real);
            }
        }
    }
    // The log is opened now: the ruleset does not let the process make it
    // later.
    if (!why[0] && log_mode == LOG_FILE && !log_file && real_fopen)
        log_file = real_fopen("aether-sandbox.log", "a");
    if (!why[0] && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        snprintf(why, sizeof why, "landlock: PR_SET_NO_NEW_PRIVS refused (%s); the permanent policy "
                 "holds at libc only", strerror(errno));
    if (!why[0] && aether_landlock_syscall(LL_RESTRICT_SELF, rfd, 0, 0) != 0)
        snprintf(why, sizeof why, "landlock: restrict_self refused (%s); the permanent policy holds at "
                 "libc only", strerror(errno));
    close((int)rfd);
    if (why[0]) {
        log_note(why);
        return;
    }
    char done[96];
    snprintf(done, sizeof done, "landlock: the permanent policy is held by the kernel too (ABI %d)", abi);
    log_note(done);
}
#endif

#if defined(__linux__)
// The second stage of the exec fence (runtime/sandbox/
// aether_sandbox_kernel_linux.c, "The program's own execve"). The spawn's
// filter has to let the child's one execve of its program through; once
// that program is running, this constructor, before any of its code, adds
// a filter that refuses every further execve and execveat. The spawn sets
// AETHER_SANDBOX_NO_EXEC=1 when the grants have no exec. Filters stack, and
// the strictest answer wins, so this one cannot be undone. If it cannot be
// installed the process stops (126) rather than run with exec open.
#include <stddef.h>
#include <sys/prctl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
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
static void install_no_exec_filter(void) {
    /* execve, execveat: native (x86_64 59/322 and x32's 520/545 after the
     * x32 bit; generic 221/281), compat (i386 11/358, ARM 11/387). */
#if defined(__x86_64__) && !defined(__ILP32__)
    const unsigned native = AUDIT_ARCH_X86_64, compat = AUDIT_ARCH_I386, mask = ~0x40000000u;
    const unsigned n0 = 59, n1 = 322, n2 = 520, n3 = 545, c0 = 11, c1 = 358;
#elif defined(__aarch64__)
    const unsigned native = AUDIT_ARCH_AARCH64, compat = AUDIT_ARCH_ARM, mask = ~0u;
    const unsigned n0 = 221, n1 = 281, n2 = 221, n3 = 281, c0 = 11, c1 = 387;
#elif defined(__riscv) && __riscv_xlen == 64
    const unsigned native = AUDIT_ARCH_RISCV64, compat = 0, mask = ~0u;
    const unsigned n0 = 221, n1 = 281, n2 = 221, n3 = 281, c0 = 221, c1 = 281;
#elif defined(__loongarch64) || (defined(__loongarch__) && defined(__loongarch_lp64))
    const unsigned native = AUDIT_ARCH_LOONGARCH64, compat = 0, mask = ~0u;
    const unsigned n0 = 221, n1 = 281, n2 = 221, n3 = 281, c0 = 221, c1 = 281;
#else
    fprintf(stderr, "[aether] sandbox: no exec fence for this architecture\n");
    _exit(126);
#endif
#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) || defined(__loongarch__)
    struct sock_filter f[] = {
        /* 0 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        /* 1 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, native, 2, 0),    /* -> 4 */
        /* 2 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, compat, 7, 0),    /* -> 10 */
        /* 3 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* 4 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* 5 */ BPF_STMT(BPF_ALU | BPF_AND | BPF_K, mask),
        /* 6 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, n0, 7, 0),        /* -> 14 */
        /* 7 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, n1, 6, 0),
        /* 8 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, n2, 5, 0),
        /* 9 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, n3, 4, 3),        /* -> 14 : 13 */
        /* 10 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* 11 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, c0, 2, 0),       /* -> 14 */
        /* 12 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, c1, 1, 0),       /* -> 14 : 13 */
        /* 13 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* 14 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
    };
    struct sock_fprog prog = { .len = (unsigned short)(sizeof f / sizeof f[0]), .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        fprintf(stderr, "[aether] sandbox: cannot install the exec fence: %s\n", strerror(errno));
        _exit(126);
    }
#endif
}
#endif

// Load grants from shared memory or file
static int g_preload_constructing = 0;

static void __attribute__((constructor)) sandbox_init(void) {
    g_preload_constructing = 1;
    // Resolve real libc functions FIRST, before we intercept getenv
    real_open = dlsym(RTLD_NEXT, "open");
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_getenv = dlsym(RTLD_NEXT, "getenv");
    real_execve = dlsym(RTLD_NEXT, "execve");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_dlopen = dlsym(RTLD_NEXT, "dlopen");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_mprotect = dlsym(RTLD_NEXT, "mprotect");

    // Capture the sandbox controls before any user code can edit environ.
    snapshot_sandbox_env();

#if defined(__linux__)
    {
        const char* no_exec = real_getenv("AETHER_SANDBOX_NO_EXEC");
        if (no_exec && strcmp(no_exec, "1") == 0) install_no_exec_filter();
    }
#endif

    // Log mode: file (default), stderr, or none
    const char* log_env = real_getenv("AETHER_SANDBOX_LOG");
    if (log_env) {
        if (strcmp(log_env, "stderr") == 0) log_mode = LOG_STDERR;
        else if (strcmp(log_env, "none") == 0) log_mode = LOG_NONE;
        else log_mode = LOG_FILE;
    }
    // Legacy: AETHER_SANDBOX_VERBOSE=1 sets stderr mode
    const char* verbose_env = real_getenv("AETHER_SANDBOX_VERBOSE");
    if (verbose_env && strcmp(verbose_env, "1") == 0) log_mode = LOG_STDERR;

    // An explicitly configured source is a containment request. If every
    // configured source fails to load, leave the sandbox active with zero
    // grants so intercepted operations are denied rather than running
    // unrestricted. With no source configured at all, preload stays inert.
    int source_requested = 0;
    int source_loaded = 0;

    // A descriptor first (sandbox.spawn_socket): a sealed memfd this
    // process inherited, read from its start and left open, so an exec it
    // is granted reads the same grants. When it is named it is the only
    // source: a descriptor that cannot be read leaves zero grants, never the
    // other sources' (fail closed).
    const char* grants_fd = real_getenv("AETHER_SANDBOX_GRANTS_FD");
    if (grants_fd) {
        source_requested = 1;
        char* end = NULL;
        long gfd = strtol(grants_fd, &end, 10);
        struct stat st;
        if (end && *end == '\0' && gfd >= 0 && gfd < INT_MAX &&
            fstat((int)gfd, &st) == 0 && st.st_size >= 0 &&
            st.st_size <= MAX_GRANT_SOURCE_SIZE) {
            char buf[MAX_GRANT_SOURCE_SIZE];
            ssize_t got = 0;
            while (got < st.st_size) {
                ssize_t r = pread((int)gfd, buf + got, (size_t)(st.st_size - got), got);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) break;
                got += r;
            }
            if (got == st.st_size) {
                parse_grants(buf, (int)got);
                source_loaded = 1;
            }
        }
    }

    // Shared memory (set by aether_spawn_sandboxed).
    const char* shm_name = grants_fd ? NULL : real_getenv("AETHER_SANDBOX_SHM");
    if (shm_name) {
        source_requested = 1;
        int fd = shm_open(shm_name, O_RDONLY, 0);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= 0 &&
                st.st_size <= MAX_GRANT_SOURCE_SIZE) {
                if (st.st_size == 0) {
                    source_loaded = 1;
                } else {
                    char* data = mmap(NULL, (size_t)st.st_size, PROT_READ,
                                      MAP_SHARED, fd, 0);
                    if (data != MAP_FAILED) {
                        parse_grants(data, (int)st.st_size);
                        munmap(data, (size_t)st.st_size);
                        source_loaded = 1;
                    }
                }
            }
            close(fd);
        }
    }

    // Fall back to grant file (manual LD_PRELOAD usage)
    if (!source_loaded && !grants_fd) {
        const char* grant_file = real_getenv("AETHER_SANDBOX_GRANTS");
        if (grant_file) {
            source_requested = 1;
            FILE* f = real_fopen(grant_file, "r");
            if (f) {
                char buf[MAX_GRANT_SOURCE_SIZE];
                size_t n = fread(buf, 1, sizeof(buf), f);
                int extra = fgetc(f);
                int read_failed = ferror(f) || extra != EOF;
                fclose(f);
                if (!read_failed) {
                    parse_grants(buf, (int)n);
                    source_loaded = 1;
                }
            }
        }
    }
#if defined(__linux__) && !defined(__ANDROID__)
    // A permanent policy, held by the kernel too, before main runs (#3751).
    if (source_loaded) landlock_backstop();
#endif
    sandbox_active = source_requested;
    g_preload_constructing = 0;

    if (log_mode == LOG_STDERR && sandbox_active) {
        fprintf(stderr, "AETHER_SANDBOX: active with %d grants\n", grant_count);
    }
}

// --- Intercepted functions ---

// getenv
char* getenv(const char* name) {
    if (!real_getenv) real_getenv = dlsym(RTLD_NEXT, "getenv");
    if (!name) return real_getenv(name);
    // The AETHER_* namespace is the sandbox's own control channel —
    // AETHER_SANDBOX_SHM, AETHER_SANDBOX_VERBOSE, AETHER_SANDBOX_LOG,
    // AETHER_CAPSICUM, etc. The Aether runtime inside a sandboxed child
    // must be able to read these; an "env" grant for them would be
    // sandbox plumbing leaking into the user-facing grant list. They
    // carry no secrets, so exempt the whole prefix from interception.
    if (strncmp(name, "AETHER_", 7) == 0) return real_getenv(name);
    if (!check_grant("env", name)) return NULL;
    return real_getenv(name);
}

// fs.write_atomic(path) writes a new file `<path>.tmp.<pid>.<n>` and renames
// it over `path`. std checks `path` and says, on this thread, which file the
// temporary it is about to create stands for (aether_sandbox_stage); the
// open is then checked as a write of that file, under the in-process levels
// and the grant file alike, so a grant on `path` covers write_atomic(path)
// here as it does in std. Only for that shape, with `path` the file staged,
// this process's pid, and an open that creates a new file (O_CREAT|O_EXCL):
// any other open of any other name is checked as it is, so the stage can
// never be used to open an existing file or one that is not beside `path`.
// The file `path` stands for when it is the temporary staged on this thread
// and has exactly its shape, `<target>.tmp.<this pid>.<digits>`; NULL for any
// other name. The rename of it onto that file and its removal are checked
// as writes of the file (#3751), as the open that creates it is.
static const char* staged_sibling_target(const char* path) {
    const char* target = aether_sandbox_staged_for && path ? aether_sandbox_staged_for(path) : NULL;
    if (!target) return NULL;
    size_t n = strlen(target);
    if (strncmp(path, target, n) != 0 || strncmp(path + n, ".tmp.", 5) != 0) return NULL;
    const char* p = path + n + 5;
    char* end = NULL;
    long pid = strtol(p, &end, 10);
    if (end == p || *end != '.' || pid != (long)getpid()) return NULL;
    p = end + 1;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') p++;
    return *p ? NULL : target;
}

static const char* staged_write_target(const char* path, int flags) {
    if ((flags & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL)) return path;
    const char* target = staged_sibling_target(path);
    return target ? target : path;
}

// The same for an fopen() mode (aether_fopen_access in
// aether_sandbox_match.h, which std.fs uses too): "r+", "w+" and "a+" read
// and write, so they need both grants; "r+" used to pass as a read.
static int fopen_mode_allowed(const char* path, const char* mode) {
    int access = aether_fopen_access(mode);
    if ((access & AETHER_ACCESS_READ) && !check_grant("fs_read", path)) return 0;
    if ((access & AETHER_ACCESS_WRITE) && !check_grant("fs_write", path)) return 0;
    return 1;
}

// connect
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    if (addr && addr->sa_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        // Check by IP and port (the hostname was resolved before connect).
        char res[INET_ADDRSTRLEN + 16];
        if (!check_grant("tcp", aether_net_resource(res, sizeof res, ip, ntohs(sin->sin_port)))) {
            errno = EACCES;
            return -1;
        }
    } else if (addr && addr->sa_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        char ip6[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip6, sizeof(ip6));
        char res6[INET6_ADDRSTRLEN + 16];
        if (!check_grant("tcp", aether_net_resource(res6, sizeof res6, ip6, ntohs(sin6->sin6_port)))) {
            errno = EACCES;
            return -1;
        }
    }
    return real_connect(sockfd, addr, addrlen);
}

// bind: block by default, grant with "tcp_listen" category
static int (*real_bind)(int, const struct sockaddr*, socklen_t) = NULL;
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!real_bind) real_bind = dlsym(RTLD_NEXT, "bind");
    if (sandbox_active && addr &&
        (addr->sa_family == AF_INET || addr->sa_family == AF_INET6)) {
        if (!check_grant("tcp_listen", "*")) {
            errno = EACCES;
            return -1;
        }
    }
    return real_bind(sockfd, addr, addrlen);
}

// listen: same restriction as bind
static int (*real_listen)(int, int) = NULL;
int listen(int sockfd, int backlog) {
    if (!real_listen) real_listen = dlsym(RTLD_NEXT, "listen");
    if (sandbox_active && !check_grant("tcp_listen", "*")) {
        errno = EACCES;
        return -1;
    }
    return real_listen(sockfd, backlog);
}

// accept / accept4: block inbound connections unless tcp_listen granted
static int (*real_accept)(int, struct sockaddr*, socklen_t*) = NULL;
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!real_accept) real_accept = dlsym(RTLD_NEXT, "accept");
    if (sandbox_active && !check_grant("tcp_listen", "*")) {
        errno = EACCES;
        return -1;
    }
    return real_accept(sockfd, addr, addrlen);
}

// ---------------------------------------------------------------------------
// The file system (#3751)
// ---------------------------------------------------------------------------
//
// Every libc call that opens, lists or changes something by path comes
// through here. With no policy in force (no grant file, and no level of the
// calling thread's in-process sandbox) each wrapper is the plain call. With
// one, a call is made in three steps:
//
//   1. The path is resolved: where it leads (aether_sandbox_canon_path), or,
//      for a call that acts on a name itself (unlink, rmdir, rename, mkdir,
//      the new name of link and symlink, mknod, an open with O_NOFOLLOW or
//      O_CREAT|O_EXCL), its directory, with the last name kept
//      (aether_sandbox_canon_entry). A path given to an *at call is taken
//      relative to the directory descriptor, whose own path is read from
//      the kernel (fd_path); it used to be taken relative to the working
//      directory, so a descriptor from an opendir reached anything.
//   2. The grant is checked on that resolution: fs_read for what reads or
//      lists, fs_write for what writes, creates, removes or changes.
//   3. The call is made on the resolution, following no symlink: an open
//      by aether_sandbox_walk, a call on a name with its *at variant from
//      the directory aether_sandbox_anchor opens. A symlink swapped into
//      the path after the check (by another thread or process) fails the
//      call instead of being followed out of the grant.
//
// Lookups that read no file's content and change nothing are not wrapped:
// stat and its kin, access, readlink, realpath, getxattr and listxattr,
// statfs and statvfs, pathconf. They say that a name exists and what it
// is, never what it holds, and libc and the program's runtime make them
// for their own needs (a PATH search, the resolution in step 1). The
// in-process checks of std (fs.exists, fs.stat) do ask fs_read for them.
// tests/integration/sandbox_preload_fs_coverage lists every path call of
// the C library and fails on one that is neither wrapped nor listed here.

static int fd_path(int fd, char out[PATH_MAX], char run[PATH_MAX]);
static int policy_in_force(void);

// The function behind a wrapper, looked up the first time it is needed.
#define FS_REAL(name) \
    (real_##name ? real_##name : (real_##name = (__typeof__(real_##name))dlsym(RTLD_NEXT, #name)))

static int fs_real_openat(int dirfd, const char* path, int flags, ...);

#if defined(__linux__)
// The raw system call, for openat2 in aether_sandbox_walk. The syscall()
// wrapper below refuses openat2 under a policy, so not through it.
static long fs_real_syscall(long number, ...) {
    static long (*fn)(long, ...) = NULL;
    if (!fn) fn = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    va_list ap;
    va_start(ap, number);
    long a = va_arg(ap, long);
    long b = va_arg(ap, long);
    long c = va_arg(ap, long);
    long d = va_arg(ap, long);
    va_end(ap);
    if (!fn) { errno = ENOSYS; return -1; }
    return fn(number, a, b, c, d);
}
#endif

static const AetherSandboxFsOps preload_ops = {
    fs_real_openat, close,
#if defined(__linux__)
    fs_real_syscall,
#else
    NULL,
#endif
};

// The in-process runtime finds this to pass a call it checked to the
// wrappers with its resolved path, rather than walk through them itself
// (aether_sandbox.c, fs_preload_here).
__attribute__((visibility("default"))) int aether_sandbox_preload_present(void) { return 1; }

// The functions behind the wrappers, for the in-process runtime's lookups
// (aether_sandbox_stat and its kin): a lookup is not something this
// library holds, and walking through its openat would check each directory
// on the way as an open of it.
__attribute__((visibility("default"))) const AetherSandboxFsOps* aether_sandbox_preload_ops(void) {
    return &preload_ops;
}

// `path` as an absolute name: relative to the directory `dirfd` is open
// on, or to the working directory for AT_FDCWD. An empty path (AT_EMPTY_PATH)
// is the descriptor's own file. 0 when the descriptor names no path.
static int fs_at_path(int dirfd, const char* path, char out[PATH_MAX]) {
    if (!path) return 0;
    if (path[0] == '/' || (dirfd == AT_FDCWD && path[0])) {
        size_t n = strlen(path);
        if (n >= PATH_MAX) return 0;
        memcpy(out, path, n + 1);
        return 1;
    }
    char dir[PATH_MAX];
    char run[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(dir, sizeof dir)) return 0;
    } else if (!fd_path(dirfd, dir, run)) {
        return 0;
    }
    size_t dl = strlen(dir);
    size_t pl = strlen(path);
    if (pl == 0) {
        memcpy(out, dir, dl + 1);
        return 1;
    }
    if (dl + 1 + pl + 1 > PATH_MAX) return 0;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, path, pl + 1);
    return 1;
}

// Steps 1 and 2: `path` (relative to `dirfd`) resolved, the last name kept
// with `entry`, and checked for `access` (AETHER_ACCESS_READ, _WRITE). 1 with
// the resolution in `out`; 0 refused, logged, errno EACCES.
static int fs_allowed(int dirfd, const char* path, int entry, int access, char out[PATH_MAX]) {
    char joined[PATH_MAX];
    int ok = fs_at_path(dirfd, path, joined) &&
             (entry ? aether_sandbox_canon_entry(joined, out, PATH_MAX)
                    : aether_sandbox_canon_path(joined, out, PATH_MAX));
    if (!ok) {
        log_deny((access & AETHER_ACCESS_WRITE) ? "fs_write" : "fs_read", path ? path : "(null)");
        errno = EACCES;
        return 0;
    }
    if ((access & AETHER_ACCESS_READ) && !check_resolved("fs_read", out)) { errno = EACCES; return 0; }
    if ((access & AETHER_ACCESS_WRITE) && !check_resolved("fs_write", out)) { errno = EACCES; return 0; }
    return 1;
}

static int fs_needs_mode(int flags) {
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE) return 1;
#endif
    return (flags & O_CREAT) != 0;
}

#define FS_MODE_ARG(flags)                    \
    unsigned mode = 0;                        \
    if (fs_needs_mode(flags)) {               \
        va_list ap;                           \
        va_start(ap, flags);                  \
        mode = va_arg(ap, unsigned);          \
        va_end(ap);                           \
    }

static int (*real_openat)(int, const char*, int, ...) = NULL;
static int fs_real_openat(int dirfd, const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    return FS_REAL(openat)(dirfd, path, flags, mode);
}

// What an open() asks of the file (#3742): a read when its access mode is
// O_RDONLY or O_RDWR, a write when it is O_WRONLY or O_RDWR or when it
// creates, appends to or truncates the file. O_RDWR needs both grants, as
// fopen's "r+" does. Landlock asks the same of a child that spawn_sandboxed
// starts (READ_FILE and WRITE_FILE).
static int fs_open_access(int flags) {
    int acc = flags & O_ACCMODE;
    int access = 0;
    if (acc == O_RDONLY || acc == O_RDWR) access |= AETHER_ACCESS_READ;
    if (acc != O_RDONLY || (flags & (O_CREAT | O_APPEND | O_TRUNC))) access |= AETHER_ACCESS_WRITE;
    return access;
}

// open and every open of a path, under a policy: steps 1 to 3. O_NOFOLLOW
// and O_CREAT|O_EXCL act on a symlink at the end itself, so for them the
// last name is what is checked. A staged write_atomic temporary is checked
// as a write of the file it stands for (staged_write_target); it is
// opened, as itself, in that file's directory.
static int fs_open_checked(int dirfd, const char* path, int flags, unsigned mode) {
    int entry = (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    if (aether_sandbox_dir_slash(path)) {
        // "name/": the kernel follows the name and needs a directory there;
        // it creates no file by such a path (EISDIR).
        if (flags & O_CREAT) { errno = EISDIR; return -1; }
        entry = 0;
        flags |= O_DIRECTORY;
    }
    int access = fs_open_access(flags);
    char resolved[PATH_MAX];
    const char* target = (dirfd == AT_FDCWD && path) ? staged_write_target(path, flags) : path;
    if (target != path) {
        char rt[PATH_MAX];
        if (!fs_allowed(AT_FDCWD, target, 1, AETHER_ACCESS_WRITE, rt)) return -1;
        if (!fs_at_path(AT_FDCWD, path, rt) ||
            !aether_sandbox_canon_entry(rt, resolved, sizeof resolved)) {
            errno = EACCES;
            return -1;
        }
        if ((access & AETHER_ACCESS_READ) && !check_resolved("fs_read", resolved)) {
            errno = EACCES;
            return -1;
        }
    } else if (!fs_allowed(dirfd, path, entry, access, resolved)) {
        return -1;
    }
    return aether_sandbox_walk(resolved, flags, mode, &preload_ops);
}

int open(const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(open)(path, flags, mode);
    return fs_open_checked(AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(openat)(dirfd, path, flags, mode);
    return fs_open_checked(dirfd, path, flags, mode);
}

static int (*real_creat)(const char*, mode_t) = NULL;
int creat(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(creat)(path, mode);
    return fs_open_checked(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, (unsigned)mode);
}

// fopen under a policy: the open of steps 1 to 3, then a stream on it.
static FILE* fs_fopen_checked(const char* path, const char* mode, int extra_flags) {
    int flags = aether_fopen_flags(mode);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = fs_open_checked(AT_FDCWD, path, flags | extra_flags, 0666);
    if (fd < 0) return NULL;
    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int err = errno;
        close(fd);
        errno = err;
    }
    return fp;
}

FILE* fopen(const char* path, const char* mode) {
    if (!policy_in_force()) return FS_REAL(fopen)(path, mode);
    return fs_fopen_checked(path, mode, 0);
}

// freopen. glibc opens the new file on its internal open, which the open()
// above never sees, so it gets fopen's rule here. With no path it reopens
// the stream's own file in the new mode: glibc does that through
// /proc/self/fd/N, which can widen a read-only stream to "r+", so on Linux
// that path is what is checked (it resolves to the file). The BSD libcs and
// musl only change the flags of the descriptor they have, which cannot
// widen its access, so there nothing new is opened. On a refusal the stream
// is closed, as freopen closes it whatever happens.
//
// With a path, the file is opened by steps 1 to 3 and the stream reopened
// on that descriptor through /proc/self/fd/N, which names the file opened
// and nothing a swap can change. On the BSDs, which have no /proc, the
// descriptor is put in the stream's place (dup2) and the stream reopened
// with no name, which only sets its mode. On Linux with /proc not mounted
// the reopen is by the resolved path, which leaves the directories on its
// way to a swap between the check and the reopen.
//
// fdopen needs no wrapper: it opens nothing, and every libc refuses a mode
// the descriptor's own access does not allow.
static int freopen_allowed(const char* path, const char* mode, FILE* stream) {
    if (path) return fopen_mode_allowed(path, mode);
#if defined(__linux__)
    int fd = stream ? fileno(stream) : -1;
    if (fd < 0) return 1;   // nothing to reopen: libc fails it
    // A reopen of the same descriptor for no more than it already holds
    // gains nothing, so it needs no grant: freopen(NULL, "rb", stdin), the
    // portable way to put stdin in binary mode, reopens a pipe, a socket or
    // a terminal, whose /proc/self/fd link names no path a grant can match
    // (pipe:[123], /dev/pts/N), and was refused, with stdin closed. Asking
    // for more (a read-only descriptor reopened for writing, which the
    // reopen through /proc can grant) is checked against the path.
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) {
        int held = (fl & O_ACCMODE) == O_RDWR   ? AETHER_ACCESS_READ | AETHER_ACCESS_WRITE
                 : (fl & O_ACCMODE) == O_WRONLY ? AETHER_ACCESS_WRITE
                                                : AETHER_ACCESS_READ;
        if ((aether_fopen_access(mode) & ~held) == 0) return 1;
    }
    char self[64];
    snprintf(self, sizeof self, "/proc/self/fd/%d", fd);
    return fopen_mode_allowed(self, mode);
#else
    (void)mode;
    (void)stream;
    return 1;
#endif
}

typedef FILE* (*freopen_fn)(const char*, const char*, FILE*);
static FILE* fs_freopen(freopen_fn real, const char* path, const char* mode, FILE* stream, int extra_flags) {
    if (!policy_in_force()) return real(path, mode, stream);
    if (!path) {
        if (!freopen_allowed(NULL, mode, stream)) {
            if (stream) fclose(stream);
            errno = EACCES;
            return NULL;
        }
        return real(NULL, mode, stream);
    }
    int flags = aether_fopen_flags(mode);
    char resolved[PATH_MAX];
    int fd = -1;
    if (flags < 0) {
        errno = EINVAL;
    } else {
        flags |= extra_flags | O_CLOEXEC;
        int entry = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
        if (fs_allowed(AT_FDCWD, path, entry, fs_open_access(flags), resolved))
            fd = aether_sandbox_walk(resolved, flags, 0666, &preload_ops);
    }
    if (fd < 0) {
        int err = errno;
        if (stream) fclose(stream);
        errno = err;
        return NULL;
    }
    // The reopen is of the file already open, made and truncated as the
    // mode asked: "x" would now find it there.
    char again[16];
    size_t m = 0;
    for (const char* c = mode; *c && m + 1 < sizeof again; c++) {
        if (*c != 'x') again[m++] = *c;
    }
    again[m] = '\0';
    const char* name = resolved;
    FILE* fp = NULL;
    int done = 0;
#if defined(__linux__)
    char self[64];
    snprintf(self, sizeof self, "/proc/self/fd/%d", fd);
    if (access(self, F_OK) == 0) name = self;
#else
    // No /proc: the descriptor the walk opened is put in the stream's
    // place (dup2), and the stream reopened on it with no name: the BSD
    // libcs then only set the descriptor's flags and the stream's mode,
    // refusing a mode the descriptor's access does not allow.
    int sfd = stream ? fileno(stream) : -1;
    if (sfd >= 0) {
        fflush(stream);
        if (dup2(fd, sfd) >= 0) {
            fp = real(NULL, again, stream);
            done = 1;
        }
    }
#endif
    if (!done) fp = real(name, again, stream);
    int err = errno;
    close(fd);
    errno = err;
    return fp;
}

static FILE* (*real_freopen)(const char*, const char*, FILE*) = NULL;
FILE* freopen(const char* path, const char* mode, FILE* stream) {
    return fs_freopen(FS_REAL(freopen), path, mode, stream, 0);
}

static DIR* (*real_opendir)(const char*) = NULL;
DIR* opendir(const char* path) {
    if (!policy_in_force()) return FS_REAL(opendir)(path);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, path, 0, AETHER_ACCESS_READ, resolved)) return NULL;
    int fd = aether_sandbox_walk(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, &preload_ops);
    if (fd < 0) return NULL;
    DIR* d = fdopendir(fd);
    if (!d) {
        int err = errno;
        close(fd);
        errno = err;
    }
    return d;
}

// scandir lists a directory through libc's own opendir, which the wrapper
// above never sees. Under a policy the directory is opened by steps 1 to 3
// and listed here, as scandir lists it: each entry the filter keeps, copied,
// then sorted.
#define FS_SCANDIR(NAME, DIRENT, READDIR)                                                     \
    static int (*real_##NAME)(const char*, struct DIRENT***, int (*)(const struct DIRENT*),   \
                              int (*)(const struct DIRENT**, const struct DIRENT**)) = NULL;   \
    static int fs_list_##NAME(int dirfd, const char* path, struct DIRENT*** list,             \
                              int (*filter)(const struct DIRENT*),                          \
                              int (*compar)(const struct DIRENT**, const struct DIRENT**)) { \
        char resolved[PATH_MAX];                                                              \
        if (!fs_allowed(dirfd, path, 0, AETHER_ACCESS_READ, resolved)) return -1;             \
        int fd = aether_sandbox_walk(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0,         \
                                     &preload_ops);                                           \
        if (fd < 0) return -1;                                                                \
        DIR* d = fdopendir(fd);                                                               \
        if (!d) {                                                                             \
            int err = errno;                                                                  \
            close(fd);                                                                        \
            errno = err;                                                                      \
            return -1;                                                                        \
        }                                                                                     \
        struct DIRENT** out = NULL;                                                           \
        size_t n = 0, cap = 0;                                                                \
        struct DIRENT* e;                                                                     \
        int failed = 0;                                                                       \
        errno = 0;                                                                            \
        while ((e = READDIR(d)) != NULL) {                                                    \
            if (filter && !filter(e)) continue;                                               \
            size_t size = offsetof(struct DIRENT, d_name) + strlen(e->d_name) + 1;            \
            if (size < e->d_reclen) size = e->d_reclen;                                       \
            struct DIRENT* copy = (struct DIRENT*)malloc(size);                               \
            if (n == cap) {                                                                   \
                size_t ncap = cap ? cap * 2 : 16;                                             \
                struct DIRENT** grown =                                                       \
                    copy ? (struct DIRENT**)realloc(out, ncap * sizeof *out) : NULL;          \
                if (grown) { out = grown; cap = ncap; }                                       \
                else { free(copy); copy = NULL; }                                             \
            }                                                                                 \
            if (!copy) { failed = ENOMEM; break; }                                            \
            memcpy(copy, e, size < e->d_reclen ? size : e->d_reclen);                         \
            memcpy(copy->d_name, e->d_name, strlen(e->d_name) + 1);                           \
            out[n++] = copy;                                                                  \
            errno = 0;                                                                        \
        }                                                                                     \
        if (!failed && errno) failed = errno;                                                 \
        closedir(d);                                                                          \
        if (failed || n > INT_MAX) {                                                          \
            for (size_t i = 0; i < n; i++) free(out[i]);                                      \
            free(out);                                                                        \
            errno = failed ? failed : EOVERFLOW;                                              \
            return -1;                                                                        \
        }                                                                                     \
        if (compar && n > 1)                                                                  \
            qsort(out, n, sizeof *out, (int (*)(const void*, const void*))compar);            \
        *list = out;                                                                          \
        return (int)n;                                                                        \
    }                                                                                         \
    int NAME(const char* path, struct DIRENT*** list, int (*filter)(const struct DIRENT*),    \
             int (*compar)(const struct DIRENT**, const struct DIRENT**)) {                   \
        if (!policy_in_force()) return FS_REAL(NAME)(path, list, filter, compar);             \
        return fs_list_##NAME(AT_FDCWD, path, list, filter, compar);                          \
    }
FS_SCANDIR(scandir, dirent, readdir)

// scandirat: the same, its path relative to a directory descriptor.
#define FS_SCANDIRAT(NAME, LIST, DIRENT)                                                       \
    static int (*real_##NAME)(int, const char*, struct DIRENT***,                              \
                              int (*)(const struct DIRENT*),                                   \
                              int (*)(const struct DIRENT**, const struct DIRENT**)) = NULL;    \
    int NAME(int dirfd, const char* path, struct DIRENT*** list,                               \
             int (*filter)(const struct DIRENT*),                                              \
             int (*compar)(const struct DIRENT**, const struct DIRENT**)) {                    \
        if (!policy_in_force()) return FS_REAL(NAME)(dirfd, path, list, filter, compar);       \
        return LIST(dirfd, path, list, filter, compar);                                        \
    }
#if defined(__GLIBC__) || defined(__FreeBSD__)
FS_SCANDIRAT(scandirat, fs_list_scandir, dirent)
#endif

// Calls on a name: unlink, rmdir, remove, mkdir, mknod, mkfifo, symlink.
// Each checks fs_write on the name (step 1 keeps the last name) and makes
// its *at variant from the anchor of its directory (step 3).
static int fs_on_name(int dirfd, const char* path, char name[PATH_MAX]) {
    char resolved[PATH_MAX];
    if (!fs_allowed(dirfd, path, 1, AETHER_ACCESS_WRITE, resolved)) return -1;
    return aether_sandbox_anchor(resolved, name, PATH_MAX, &preload_ops);
}

// fs_on_name for a call that has an answer of its own for "name/"
// (aether_sandbox_slash_call's `op`): the anchor when the call goes on,
// else -1 with the kernel's errno.
static int fs_on_name_op(int dirfd, const char* path, char name[PATH_MAX], int op) {
    int d = fs_on_name(dirfd, path, name);
    if (d < 0 || !aether_sandbox_dir_slash(path)) return d;
    if (aether_sandbox_slash_call(d, name, op) != 0) {
        int err = errno;
        close(d);
        errno = err;
        return -1;
    }
    return d;
}

// fs.write_atomic's temporary (staged_sibling_target): fs_write on the file
// it stands for, then its directory anchored and its last name in `name`.
// -2 when `path` is not that temporary.
static int fs_on_staged(const char* path, char name[PATH_MAX]) {
    const char* target = staged_sibling_target(path);
    if (!target) return -2;
    char rt[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, target, 1, AETHER_ACCESS_WRITE, rt)) return -1;
    char joined[PATH_MAX];
    char resolved[PATH_MAX];
    if (!fs_at_path(AT_FDCWD, path, joined) ||
        !aether_sandbox_canon_entry(joined, resolved, sizeof resolved)) {
        errno = EACCES;
        return -1;
    }
    return aether_sandbox_anchor(resolved, name, PATH_MAX, &preload_ops);
}

// The call `expr`, made from the anchor `d` with `name`; the anchor closed
// after, the call's errno kept.
#define FS_AT_CALL(d, expr)          \
    do {                             \
        int fs_r_ = (expr);          \
        int fs_e_ = errno;           \
        close(d);                    \
        errno = fs_e_;               \
        return fs_r_;                \
    } while (0)

static int (*real_unlinkat)(int, const char*, int) = NULL;
static int (*real_unlink)(const char*) = NULL;
int unlink(const char* path) {
    if (!policy_in_force()) return FS_REAL(unlink)(path);
    char name[PATH_MAX];
    int d = fs_on_staged(path, name);
    if (d == -2) d = fs_on_name_op(AT_FDCWD, path, name, AETHER_SLASH_UNLINK);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(unlinkat)(d, name, 0));
}

int unlinkat(int dirfd, const char* path, int flags) {
    if (!policy_in_force()) return FS_REAL(unlinkat)(dirfd, path, flags);
    char name[PATH_MAX];
    int d = fs_on_name_op(dirfd, path, name,
                          (flags & AT_REMOVEDIR) ? AETHER_SLASH_DIR : AETHER_SLASH_UNLINK);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(unlinkat)(d, name, flags));
}

static int (*real_rmdir)(const char*) = NULL;
int rmdir(const char* path) {
    if (!policy_in_force()) return FS_REAL(rmdir)(path);
    char name[PATH_MAX];
    int d = fs_on_name_op(AT_FDCWD, path, name, AETHER_SLASH_DIR);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(unlinkat)(d, name, AT_REMOVEDIR));
}

// remove: unlink, or rmdir for a directory, as libc's remove decides.
static int (*real_remove)(const char*) = NULL;
int remove(const char* path) {
    if (!policy_in_force()) return FS_REAL(remove)(path);
    char name[PATH_MAX];
    int d = fs_on_staged(path, name);
    if (d == -2) d = fs_on_name_op(AT_FDCWD, path, name, AETHER_SLASH_DIR);
    if (d < 0) return -1;
    struct stat st;
    int dir = fstatat(d, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
    FS_AT_CALL(d, FS_REAL(unlinkat)(d, name, dir ? AT_REMOVEDIR : 0));
}

// mkdir of a name that is already there says EEXIST without a grant: a
// lookup tells as much (stat is not wrapped), and `mkdir -p` meets every
// directory above the one it makes. A refusal would stop it at "/".
static int (*real_mkdirat)(int, const char*, mode_t) = NULL;
static int fs_mkdirat(int dirfd, const char* path, mode_t mode) {
    char joined[PATH_MAX];
    struct stat st;
    if (fs_at_path(dirfd, path, joined) && lstat(joined, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    char name[PATH_MAX];
    int d = fs_on_name(dirfd, path, name);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(mkdirat)(d, name, mode));
}

static int (*real_mkdir)(const char*, mode_t) = NULL;
int mkdir(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(mkdir)(path, mode);
    return fs_mkdirat(AT_FDCWD, path, mode);
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(mkdirat)(dirfd, path, mode);
    return fs_mkdirat(dirfd, path, mode);
}

static int (*real_mknodat)(int, const char*, mode_t, dev_t) = NULL;
static int (*real_mknod)(const char*, mode_t, dev_t) = NULL;
int mknod(const char* path, mode_t mode, dev_t dev) {
    if (!policy_in_force()) return FS_REAL(mknod)(path, mode, dev);
    char name[PATH_MAX];
    int d = fs_on_name_op(AT_FDCWD, path, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(mknodat)(d, name, mode, dev));
}

int mknodat(int dirfd, const char* path, mode_t mode, dev_t dev) {
    if (!policy_in_force()) return FS_REAL(mknodat)(dirfd, path, mode, dev);
    char name[PATH_MAX];
    int d = fs_on_name_op(dirfd, path, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(mknodat)(d, name, mode, dev));
}

static int (*real_mkfifoat)(int, const char*, mode_t) = NULL;
static int (*real_mkfifo)(const char*, mode_t) = NULL;
int mkfifo(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(mkfifo)(path, mode);
    char name[PATH_MAX];
    int d = fs_on_name_op(AT_FDCWD, path, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(mkfifoat)(d, name, mode));
}

int mkfifoat(int dirfd, const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(mkfifoat)(dirfd, path, mode);
    char name[PATH_MAX];
    int d = fs_on_name_op(dirfd, path, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(mkfifoat)(d, name, mode));
}

// symlink: the new name is checked. The target is text the link holds;
// whatever opens the link later is checked where it leads.
static int (*real_symlinkat)(const char*, int, const char*) = NULL;
static int (*real_symlink)(const char*, const char*) = NULL;
int symlink(const char* target, const char* linkpath) {
    if (!policy_in_force()) return FS_REAL(symlink)(target, linkpath);
    char name[PATH_MAX];
    int d = fs_on_name_op(AT_FDCWD, linkpath, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(symlinkat)(target, d, name));
}

int symlinkat(const char* target, int dirfd, const char* linkpath) {
    if (!policy_in_force()) return FS_REAL(symlinkat)(target, dirfd, linkpath);
    char name[PATH_MAX];
    int d = fs_on_name_op(dirfd, linkpath, name, AETHER_SLASH_NEW);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(symlinkat)(target, d, name));
}

// Two names, each checked and anchored: rename's, and link's.
static int fs_two_names(int d1, const char* p1, int d2, const char* p2,
                        int* a1, char n1[PATH_MAX], int* a2, char n2[PATH_MAX]) {
    *a1 = fs_on_name(d1, p1, n1);
    if (*a1 < 0) return -1;
    *a2 = fs_on_name(d2, p2, n2);
    if (*a2 < 0) {
        int err = errno;
        close(*a1);
        errno = err;
        return -1;
    }
    return 0;
}

#define FS_AT_CALL2(a1, a2, expr)    \
    do {                             \
        int fs_r_ = (expr);          \
        int fs_e_ = errno;           \
        close(a1);                   \
        close(a2);                   \
        errno = fs_e_;               \
        return fs_r_;                \
    } while (0)

// A rename spelt with "name/" on either side: the kernel moves a directory only
// (the source not followed: ENOENT, or ENOTDIR for a file or a symlink),
// and onto a directory or nothing (ENOTDIR). 0 to go on.
static int fs_rename_slash(const char* from, int a1, const char* n1, const char* to, int a2,
                           const char* n2) {
    int to_slash = aether_sandbox_dir_slash(to);
    if (!aether_sandbox_dir_slash(from) && !to_slash) return 0;
    if (aether_sandbox_slash_call(a1, n1, AETHER_SLASH_DIR) != 0) return -1;
    struct stat st;
    if (fstatat(a2, n2, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int (*real_renameat)(int, const char*, int, const char*) = NULL;
static int (*real_rename)(const char*, const char*) = NULL;
int rename(const char* from, const char* to) {
    if (!policy_in_force()) return FS_REAL(rename)(from, to);
    int a1, a2;
    char n1[PATH_MAX], n2[PATH_MAX];
    const char* staged = staged_sibling_target(from);
    if (staged && to && strcmp(staged, to) == 0) {
        // fs.write_atomic's temporary onto the file it stands for.
        a1 = fs_on_staged(from, n1);
        if (a1 < 0) return -1;
        a2 = fs_on_name(AT_FDCWD, to, n2);
        if (a2 < 0) {
            int err = errno;
            close(a1);
            errno = err;
            return -1;
        }
    } else if (fs_two_names(AT_FDCWD, from, AT_FDCWD, to, &a1, n1, &a2, n2) != 0) {
        return -1;
    }
    FS_AT_CALL2(a1, a2, fs_rename_slash(from, a1, n1, to, a2, n2) != 0 ? -1 : FS_REAL(renameat)(a1, n1, a2, n2));
}

int renameat(int fromfd, const char* from, int tofd, const char* to) {
    if (!policy_in_force()) return FS_REAL(renameat)(fromfd, from, tofd, to);
    int a1, a2;
    char n1[PATH_MAX], n2[PATH_MAX];
    if (fs_two_names(fromfd, from, tofd, to, &a1, n1, &a2, n2) != 0) return -1;
    FS_AT_CALL2(a1, a2, fs_rename_slash(from, a1, n1, to, a2, n2) != 0 ? -1 : FS_REAL(renameat)(a1, n1, a2, n2));
}
#if defined(__linux__)
static int (*real_renameat2)(int, const char*, int, const char*, unsigned) = NULL;
int renameat2(int fromfd, const char* from, int tofd, const char* to, unsigned flags) {
    if (!policy_in_force()) return FS_REAL(renameat2)(fromfd, from, tofd, to, flags);
    int a1, a2;
    char n1[PATH_MAX], n2[PATH_MAX];
    if (fs_two_names(fromfd, from, tofd, to, &a1, n1, &a2, n2) != 0) return -1;
    FS_AT_CALL2(a1, a2, fs_rename_slash(from, a1, n1, to, a2, n2) != 0 ? -1
                                                                      : FS_REAL(renameat2)(a1, n1, a2, n2, flags));
}
#endif

// A hard link is a second name for the target's file. The new name needs
// fs_write, and may not reach the file more than the old one may: fs_read
// and fs_write on the target for whichever the new name is granted, or a
// link into a writable directory would make any file there writable. With
// AT_SYMLINK_FOLLOW the target is where its symlink leads.
static int (*real_linkat)(int, const char*, int, const char*, int) = NULL;

// The descriptor a link's source is when no path names it: AT_EMPTY_PATH
// with an empty path (the descriptor `d1`), or /proc/self/fd/N (or
// /dev/fd/N, or /proc/<this pid>/fd/N) followed with AT_SYMLINK_FOLLOW,
// the idiom that gives an O_TMPFILE file its name where AT_EMPTY_PATH needs
// a capability. -1 when a path names the source.
static int fs_link_source_fd(int d1, const char* target, int flags) {
    if (!target) return -1;
#ifdef AT_EMPTY_PATH
    if ((flags & AT_EMPTY_PATH) && !target[0]) return d1;
#endif
    if (!(flags & AT_SYMLINK_FOLLOW)) return -1;
    char own[48];
    snprintf(own, sizeof own, "/proc/%ld/fd/", (long)getpid());
    const char* digits = NULL;
    if (strncmp(target, "/proc/self/fd/", 14) == 0) digits = target + 14;
    else if (strncmp(target, "/dev/fd/", 8) == 0) digits = target + 8;
    else if (strncmp(target, own, strlen(own)) == 0) digits = target + strlen(own);
    if (!digits || !*digits) return -1;
    long v = 0;
    for (const char* c = digits; *c; c++) {
        if (*c < '0' || *c > '9' || v > INT_MAX / 10) return -1;
        v = v * 10 + (*c - '0');
    }
    return (int)v;
}

// A link whose source is a descriptor (fs_link_source_fd): the new name is
// checked for fs_write, and may not reach the file more than the path the
// descriptor is open on may (an O_TMPFILE file's "#ino (deleted)" in the
// directory it was made in, the " (deleted)" taken off), so an inherited
// descriptor of a file outside the grants gets no name inside them. The
// link is made with the caller's own spelling of the source, from the new
// name's anchor: it used to be resolved to the descriptor's path and linked
// by that name, which an O_TMPFILE file does not have (ENOENT), or refused
// as a path outside the grants (EACCES).
static int fs_link_from_fd(int src, int d1, const char* target, int d2, const char* linkpath,
                           int flags) {
    char rl[PATH_MAX];
    if (!fs_allowed(d2, linkpath, 1, AETHER_ACCESS_WRITE, rl)) return -1;
    char fp[PATH_MAX], run[PATH_MAX];
    if (!fd_path(src, fp, run)) {
        log_deny("fs_read", target[0] ? target : "(a descriptor)");
        errno = EACCES;
        return -1;
    }
    static const char deleted[] = " (deleted)";
    size_t fl = strlen(fp), dl = sizeof deleted - 1;
    if (fl > dl && strcmp(fp + fl - dl, deleted) == 0) fp[fl - dl] = '\0';
    static const char* const cats[2] = { "fs_read", "fs_write" };
    for (int i = 0; i < 2; i++) {
        if (grant_covers(cats[i], rl) && !check_resolved(cats[i], fp)) {
            errno = EACCES;
            return -1;
        }
    }
    char n2[PATH_MAX];
    int a2 = aether_sandbox_anchor(rl, n2, sizeof n2, &preload_ops);
    if (a2 < 0) return -1;
    int r;
    if (aether_sandbox_dir_slash(linkpath) && aether_sandbox_slash_call(a2, n2, AETHER_SLASH_NEW) != 0)
        r = -1;
    else if (!target[0])
        r = FS_REAL(linkat)(d1, "", a2, n2, flags & ~AT_SYMLINK_FOLLOW);
    else
        r = FS_REAL(linkat)(AT_FDCWD, target, a2, n2, AT_SYMLINK_FOLLOW);
    int err = errno;
    close(a2);
    errno = err;
    return r;
}

static int fs_link_checked(int d1, const char* target, int d2, const char* linkpath, int flags) {
    int src = fs_link_source_fd(d1, target, flags);
    if (src >= 0) return fs_link_from_fd(src, d1, target, d2, linkpath, flags);
    char rt[PATH_MAX], rl[PATH_MAX];
    if (!fs_allowed(d2, linkpath, 1, AETHER_ACCESS_WRITE, rl)) return -1;
    char joined[PATH_MAX];
    int follow = (flags & AT_SYMLINK_FOLLOW) != 0;
    if (aether_sandbox_dir_slash(target)) {
        // "name/" as the source: the kernel follows it and refuses it,
        // a directory (EPERM: no hard link to one) or not (ENOTDIR).
        struct stat st;
        if (!fs_at_path(d1, target, joined) || stat(joined, &st) != 0) errno = ENOENT;
        else errno = S_ISDIR(st.st_mode) ? EPERM : ENOTDIR;
        return -1;
    }
    if (!fs_at_path(d1, target, joined) ||
        !(follow ? aether_sandbox_canon_path(joined, rt, sizeof rt)
                 : aether_sandbox_canon_entry(joined, rt, sizeof rt))) {
        log_deny("fs_read", target ? target : "(null)");
        errno = EACCES;
        return -1;
    }
    static const char* const cats[2] = { "fs_read", "fs_write" };
    for (int i = 0; i < 2; i++) {
        if (grant_covers(cats[i], rl) && !check_resolved(cats[i], rt)) {
            errno = EACCES;
            return -1;
        }
    }
    char n1[PATH_MAX], n2[PATH_MAX];
    int a1 = aether_sandbox_anchor(rt, n1, sizeof n1, &preload_ops);
    if (a1 < 0) return -1;
    int a2 = aether_sandbox_anchor(rl, n2, sizeof n2, &preload_ops);
    if (a2 < 0) {
        int err = errno;
        close(a1);
        errno = err;
        return -1;
    }
    FS_AT_CALL2(a1, a2,
                (aether_sandbox_dir_slash(linkpath) && aether_sandbox_slash_call(a2, n2, AETHER_SLASH_NEW) != 0)
                    ? -1
                    : FS_REAL(linkat)(a1, n1, a2, n2, 0));
}

static int (*real_link)(const char*, const char*) = NULL;
int link(const char* target, const char* linkpath) {
    if (!policy_in_force()) return FS_REAL(link)(target, linkpath);
    return fs_link_checked(AT_FDCWD, target, AT_FDCWD, linkpath, 0);
}

int linkat(int d1, const char* target, int d2, const char* linkpath, int flags) {
    if (!policy_in_force()) return FS_REAL(linkat)(d1, target, d2, linkpath, flags);
    return fs_link_checked(d1, target, d2, linkpath, flags);
}

// Calls that change a file where its path leads: chmod, chown, truncate,
// the times, the extended attributes. fs_write on the resolution, and the
// call made on the file opened by step 3 where an fd call exists; else on
// the name from its anchor, not following it, which fails if the name has
// become a symlink.
static int fs_changed_file(int dirfd, const char* path, int nofollow, char resolved[PATH_MAX]) {
    return fs_allowed(dirfd, path, nofollow, AETHER_ACCESS_WRITE, resolved);
}

// chmod and its kin, by name from the anchor (fchmodat with
// AT_SYMLINK_NOFOLLOW: the name was resolved, so a symlink there now is the
// race; where the C library cannot chmod a name without following it, a
// look first, leaving only that name to a swap between the look and the
// call).
static int (*real_fchmodat)(int, const char*, mode_t, int) = NULL;
static int (*real_chmod)(const char*, mode_t) = NULL;
static int fs_chmod_at(const char* resolved, mode_t mode) {
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    int (*fchmodat_fn)(int, const char*, mode_t, int) = FS_REAL(fchmodat);
    int (*chmod_fn)(const char*, mode_t) = FS_REAL(chmod);
    FS_AT_CALL(d, aether_sandbox_chmod_at(d, name, mode, &preload_ops, fchmodat_fn, chmod_fn));
}

int chmod(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(chmod)(path, mode);
    char resolved[PATH_MAX];
    if (!fs_changed_file(AT_FDCWD, path, 0, resolved)) return -1;
    return fs_chmod_at(resolved, mode);
}

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    if (!policy_in_force()) return FS_REAL(fchmodat)(dirfd, path, mode, flags);
    char resolved[PATH_MAX];
    if (!fs_changed_file(dirfd, path, (flags & AT_SYMLINK_NOFOLLOW) != 0, resolved)) return -1;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        char name[PATH_MAX];
        int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
        if (d < 0) return -1;
        FS_AT_CALL(d, FS_REAL(fchmodat)(d, name, mode, AT_SYMLINK_NOFOLLOW));
    }
    return fs_chmod_at(resolved, mode);
}

#if (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 32))) || \
    (defined(__linux__) && !defined(__GLIBC__)) || defined(__FreeBSD__) || defined(__NetBSD__)
static int (*real_lchmod)(const char*, mode_t) = NULL;
int lchmod(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(lchmod)(path, mode);
    char resolved[PATH_MAX];
    if (!fs_changed_file(AT_FDCWD, path, 1, resolved)) return -1;
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(fchmodat)(d, name, mode, AT_SYMLINK_NOFOLLOW));
}
#endif

// chown follows the name, lchown does not; both made from the anchor with
// AT_SYMLINK_NOFOLLOW on the resolution (chown's resolution has no symlink
// left at its end, so a symlink there now is the race and is not followed).
static int (*real_fchownat)(int, const char*, uid_t, gid_t, int) = NULL;
static int fs_chown_at(int dirfd, const char* path, uid_t uid, gid_t gid, int nofollow) {
    char resolved[PATH_MAX];
    if (!fs_changed_file(dirfd, path, nofollow, resolved)) return -1;
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(fchownat)(d, name, uid, gid, AT_SYMLINK_NOFOLLOW));
}

static int (*real_chown)(const char*, uid_t, gid_t) = NULL;
int chown(const char* path, uid_t uid, gid_t gid) {
    if (!policy_in_force()) return FS_REAL(chown)(path, uid, gid);
    return fs_chown_at(AT_FDCWD, path, uid, gid, 0);
}

static int (*real_lchown)(const char*, uid_t, gid_t) = NULL;
int lchown(const char* path, uid_t uid, gid_t gid) {
    if (!policy_in_force()) return FS_REAL(lchown)(path, uid, gid);
    return fs_chown_at(AT_FDCWD, path, uid, gid, 1);
}

int fchownat(int dirfd, const char* path, uid_t uid, gid_t gid, int flags) {
    if (!policy_in_force()) return FS_REAL(fchownat)(dirfd, path, uid, gid, flags);
#ifdef AT_EMPTY_PATH
    if (path && !*path && (flags & AT_EMPTY_PATH)) {
        // The descriptor's own file: fs_write on its path.
        char resolved[PATH_MAX];
        if (!fs_changed_file(dirfd, "", 0, resolved)) return -1;
        return FS_REAL(fchownat)(dirfd, path, uid, gid, flags);
    }
#endif
    return fs_chown_at(dirfd, path, uid, gid, (flags & AT_SYMLINK_NOFOLLOW) != 0);
}

// truncate: the file opened for writing by step 3, then ftruncate.
static int fs_truncate(const char* path, off_t length) {
    int fd = fs_open_checked(AT_FDCWD, path, O_WRONLY | O_CLOEXEC, 0);
    if (fd < 0) return -1;
    int r = ftruncate(fd, length);
    int err = errno;
    close(fd);
    errno = err;
    return r;
}

static int (*real_truncate)(const char*, off_t) = NULL;
int truncate(const char* path, off_t length) {
    if (!policy_in_force()) return FS_REAL(truncate)(path, length);
    return fs_truncate(path, length);
}

// The times: utimensat by name from the anchor, not following (the
// resolution has no symlink left at its end); utime, utimes and futimesat
// through it.
static int (*real_utimensat)(int, const char*, const struct timespec[2], int) = NULL;
static int fs_utimens(int dirfd, const char* path, const struct timespec ts[2], int nofollow) {
    char resolved[PATH_MAX];
    if (!fs_changed_file(dirfd, path, nofollow, resolved)) return -1;
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(utimensat)(d, name, ts, AT_SYMLINK_NOFOLLOW));
}

int utimensat(int dirfd, const char* path, const struct timespec ts[2], int flags) {
    if (!policy_in_force() || !path) return FS_REAL(utimensat)(dirfd, path, ts, flags);
#ifdef AT_EMPTY_PATH
    if (!*path && (flags & AT_EMPTY_PATH)) {
        char resolved[PATH_MAX];
        if (!fs_changed_file(dirfd, "", 0, resolved)) return -1;
        return FS_REAL(utimensat)(dirfd, path, ts, flags);
    }
#endif
    return fs_utimens(dirfd, path, ts, (flags & AT_SYMLINK_NOFOLLOW) != 0);
}

// struct timeval pairs (utimes, lutimes, futimesat) and a struct utimbuf
// (utime) as utimensat's timespecs; NULL is "now".
static const struct timespec* fs_tv_to_ts(const struct timeval tv[2], struct timespec ts[2]) {
    if (!tv) return NULL;
    for (int i = 0; i < 2; i++) {
        ts[i].tv_sec = tv[i].tv_sec;
        ts[i].tv_nsec = (long)tv[i].tv_usec * 1000;
    }
    return ts;
}

static int (*real_utimes)(const char*, const struct timeval[2]) = NULL;
int utimes(const char* path, const struct timeval tv[2]) {
    if (!policy_in_force()) return FS_REAL(utimes)(path, tv);
    struct timespec ts[2];
    return fs_utimens(AT_FDCWD, path, fs_tv_to_ts(tv, ts), 0);
}

static int (*real_lutimes)(const char*, const struct timeval[2]) = NULL;
int lutimes(const char* path, const struct timeval tv[2]) {
    if (!policy_in_force()) return FS_REAL(lutimes)(path, tv);
    struct timespec ts[2];
    return fs_utimens(AT_FDCWD, path, fs_tv_to_ts(tv, ts), 1);
}

static int (*real_futimesat)(int, const char*, const struct timeval[2]) = NULL;
int futimesat(int dirfd, const char* path, const struct timeval tv[2]) {
    if (!policy_in_force()) return FS_REAL(futimesat)(dirfd, path, tv);
    struct timespec ts[2];
    if (!path) {
        // The descriptor's own file.
        char resolved[PATH_MAX];
        if (!fs_changed_file(dirfd, "", 0, resolved)) return -1;
        return FS_REAL(futimesat)(dirfd, path, tv);
    }
    return fs_utimens(dirfd, path, fs_tv_to_ts(tv, ts), 0);
}

static int (*real_utime)(const char*, const struct utimbuf*) = NULL;
int utime(const char* path, const struct utimbuf* times) {
    if (!policy_in_force()) return FS_REAL(utime)(path, times);
    struct timespec ts[2];
    const struct timespec* t = NULL;
    if (times) {
        ts[0].tv_sec = times->actime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = times->modtime;
        ts[1].tv_nsec = 0;
        t = ts;
    }
    return fs_utimens(AT_FDCWD, path, t, 0);
}

#if defined(__linux__)
// Extended attributes, set or removed: fs_write on the file, the call made
// on it opened by step 3 (O_PATH cannot carry them, so O_RDONLY, or
// O_WRONLY for a file that cannot be read), with the f* variant. lsetxattr
// and lremovexattr act on the name itself, a symlink not followed: it is
// opened O_PATH|O_NOFOLLOW from its anchor, and set through
// /proc/self/fd/N, which setxattr follows to exactly what was opened (an
// O_PATH descriptor takes no fsetxattr). Only without /proc is it set by
// its resolved name, from which a swap of the name alone remains.
static int fs_xattr_fd(const char* path, int nofollow, char resolved[PATH_MAX]) {
    if (!fs_changed_file(AT_FDCWD, path, nofollow, resolved)) return -1;
    if (nofollow) return -2;   // by name, below
    int fd = aether_sandbox_walk(resolved, O_RDONLY | O_CLOEXEC, 0, &preload_ops);
    if (fd < 0 && errno == EACCES) fd = aether_sandbox_walk(resolved, O_WRONLY | O_CLOEXEC, 0, &preload_ops);
    return fd;
}

// The name `resolved` itself, opened O_PATH|O_NOFOLLOW from its anchor, as
// /proc/self/fd/N in `self`: the descriptor, or -1.
static int fs_xattr_entry(const char* resolved, char self[64]) {
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    int fd = fs_real_openat(d, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    int err = errno;
    close(d);
    errno = err;
    if (fd >= 0) snprintf(self, 64, "/proc/self/fd/%d", fd);
    return fd;
}

static int (*real_setxattr)(const char*, const char*, const void*, size_t, int) = NULL;
int setxattr(const char* path, const char* key, const void* value, size_t size, int flags) {
    if (!policy_in_force()) return FS_REAL(setxattr)(path, key, value, size, flags);
    char resolved[PATH_MAX];
    int fd = fs_xattr_fd(path, 0, resolved);
    if (fd < 0) return -1;
    int r = fsetxattr(fd, key, value, size, flags);
    FS_AT_CALL(fd, r);
}

static int (*real_lsetxattr)(const char*, const char*, const void*, size_t, int) = NULL;
int lsetxattr(const char* path, const char* key, const void* value, size_t size, int flags) {
    if (!policy_in_force()) return FS_REAL(lsetxattr)(path, key, value, size, flags);
    char resolved[PATH_MAX];
    if (fs_xattr_fd(path, 1, resolved) == -1) return -1;
    char self[64];
    int fd = fs_xattr_entry(resolved, self);
    if (fd < 0) return -1;
    int r = FS_REAL(setxattr)(self, key, value, size, flags);
    if (r != 0 && errno == ENOENT && access("/proc/self/fd", F_OK) != 0)
        r = FS_REAL(lsetxattr)(resolved, key, value, size, flags);   // no /proc
    FS_AT_CALL(fd, r);
}

static int (*real_removexattr)(const char*, const char*) = NULL;
int removexattr(const char* path, const char* key) {
    if (!policy_in_force()) return FS_REAL(removexattr)(path, key);
    char resolved[PATH_MAX];
    int fd = fs_xattr_fd(path, 0, resolved);
    if (fd < 0) return -1;
    int r = fremovexattr(fd, key);
    FS_AT_CALL(fd, r);
}

static int (*real_lremovexattr)(const char*, const char*) = NULL;
int lremovexattr(const char* path, const char* key) {
    if (!policy_in_force()) return FS_REAL(lremovexattr)(path, key);
    char resolved[PATH_MAX];
    if (fs_xattr_fd(path, 1, resolved) == -1) return -1;
    char self[64];
    int fd = fs_xattr_entry(resolved, self);
    if (fd < 0) return -1;
    int r = FS_REAL(removexattr)(self, key);
    if (r != 0 && errno == ENOENT && access("/proc/self/fd", F_OK) != 0)
        r = FS_REAL(lremovexattr)(resolved, key);   // no /proc
    FS_AT_CALL(fd, r);
}
#endif // __linux__ (xattr)

// Temporary files and directories: libc makes them on its own internal
// open and mkdir, which the wrappers above never see. Under a policy the
// name is made here, as libc makes it (the template's trailing XXXXXX
// replaced, `suffix` bytes after them kept), and created by steps 1 to 3
// with O_CREAT|O_EXCL, or mkdir, on a fresh name until one is free.
static int fs_temp_fill(char* tmpl, int suffix) {
    size_t len = strlen(tmpl);
    if (suffix < 0 || len < (size_t)suffix + 6) { errno = EINVAL; return 0; }
    char* x = tmpl + len - (size_t)suffix - 6;
    for (int i = 0; i < 6; i++) {
        if (x[i] != 'X') { errno = EINVAL; return 0; }
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[6];
    int got = 0;
#if defined(__linux__) || defined(__FreeBSD__)
    int ufd = FS_REAL(openat)(AT_FDCWD, "/dev/urandom", O_RDONLY | O_CLOEXEC, 0);
    if (ufd >= 0) {
        got = read(ufd, r, sizeof r) == (ssize_t)sizeof r;
        close(ufd);
    }
#endif
    if (!got) {
        static unsigned long counter = 0;
        unsigned long v = (unsigned long)getpid() * 2654435761u ^ (unsigned long)(++counter * 40503u) ^
                          (unsigned long)(size_t)&r;
        for (int i = 0; i < 6; i++) { r[i] = (unsigned char)v; v = v * 6364136223846793005ull + 1; }
    }
    for (int i = 0; i < 6; i++) x[i] = chars[r[i] % (sizeof chars - 1)];
    return 1;
}

static int fs_mkstemps_at(int dirfd, char* tmpl, int suffix, int flags) {
    size_t len = tmpl ? strlen(tmpl) : 0;
    if (!tmpl || suffix < 0 || len < (size_t)suffix + 6) { errno = EINVAL; return -1; }
    char keep[6];
    memcpy(keep, tmpl + len - (size_t)suffix - 6, 6);
    for (int attempt = 0; attempt < 100; attempt++) {
        memcpy(tmpl + len - (size_t)suffix - 6, keep, 6);
        if (!fs_temp_fill(tmpl, suffix)) return -1;
        int fd = fs_open_checked(dirfd, tmpl,
                                 O_RDWR | O_CREAT | O_EXCL | (flags & ~O_ACCMODE), 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

static int fs_mkstemps(char* tmpl, int suffix, int flags) {
    return fs_mkstemps_at(AT_FDCWD, tmpl, suffix, flags);
}

static int (*real_mkstemp)(char*) = NULL;
int mkstemp(char* tmpl) {
    if (!policy_in_force()) return FS_REAL(mkstemp)(tmpl);
    return fs_mkstemps(tmpl, 0, 0);
}

static int (*real_mkostemp)(char*, int) = NULL;
int mkostemp(char* tmpl, int flags) {
    if (!policy_in_force()) return FS_REAL(mkostemp)(tmpl, flags);
    return fs_mkstemps(tmpl, 0, flags);
}

static int (*real_mkstemps)(char*, int) = NULL;
int mkstemps(char* tmpl, int suffix) {
    if (!policy_in_force()) return FS_REAL(mkstemps)(tmpl, suffix);
    return fs_mkstemps(tmpl, suffix, 0);
}

static int (*real_mkostemps)(char*, int, int) = NULL;
int mkostemps(char* tmpl, int suffix, int flags) {
    if (!policy_in_force()) return FS_REAL(mkostemps)(tmpl, suffix, flags);
    return fs_mkstemps(tmpl, suffix, flags);
}

static char* (*real_mkdtemp)(char*) = NULL;
char* mkdtemp(char* tmpl) {
    if (!policy_in_force()) return FS_REAL(mkdtemp)(tmpl);
    size_t len = tmpl ? strlen(tmpl) : 0;
    if (len < 6) { errno = EINVAL; return NULL; }
    char keep[6];
    memcpy(keep, tmpl + len - 6, 6);
    for (int attempt = 0; attempt < 100; attempt++) {
        memcpy(tmpl + len - 6, keep, 6);
        if (!fs_temp_fill(tmpl, 0)) return NULL;
        char name[PATH_MAX];
        int d = fs_on_name(AT_FDCWD, tmpl, name);
        if (d < 0) return NULL;
        int r = FS_REAL(mkdirat)(d, name, 0700);
        int err = errno;
        close(d);
        if (r == 0) return tmpl;
        if (err != EEXIST) { errno = err; return NULL; }
    }
    errno = EEXIST;
    return NULL;
}

// tmpfile makes an unnamed file in the temporary directory: fs_write on a
// name there ("<P_tmpdir>/tmpfile", which a grant for "<P_tmpdir>/*"
// covers), then libc's own.
static FILE* (*real_tmpfile)(void) = NULL;
FILE* tmpfile(void) {
    if (!policy_in_force()) return FS_REAL(tmpfile)();
    char resolved[PATH_MAX];
#ifdef P_tmpdir
    const char* dir = P_tmpdir;
#else
    const char* dir = "/tmp";
#endif
    char name[PATH_MAX];
    snprintf(name, sizeof name, "%s/tmpfile", dir);
    if (!fs_allowed(AT_FDCWD, name, 1, AETHER_ACCESS_WRITE, resolved)) return NULL;
    return FS_REAL(tmpfile)();
}

// glob lists directories through libc's own opendir. Under a policy the
// directory its pattern starts in needs fs_read (as std's fs.glob asks),
// and a match the policy may not read is taken out of the result. Where the
// C library allocates each match on its own (glibc, the BSDs) it is freed
// here as globfree would free it; where it does not (musl), a glob that
// matched something the policy refuses fails as a read error (GLOB_ABORTED)
// rather than return it.
static int fs_glob_start_allowed(const char* pattern) {
    char dir[PATH_MAX];
    size_t first = strcspn(pattern, "*?[");
    if (pattern[first] == '\0') return 1;   // no wildcard: the matches are checked
    size_t cut = first;
    while (cut > 0 && pattern[cut - 1] != '/') cut--;
    if (cut == 0) { dir[0] = '.'; dir[1] = '\0'; }
    else {
        if (cut >= sizeof dir) return 0;
        memcpy(dir, pattern, cut);
        dir[cut] = '\0';
    }
    char resolved[PATH_MAX];
    return fs_allowed(AT_FDCWD, dir, 0, AETHER_ACCESS_READ, resolved);
}

// Each match from index `from` the policy may read is kept. Where the C
// library allocates each match on its own (glibc, the BSDs, macOS) a
// refused one is freed as globfree would free it and taken out; elsewhere
// (musl) nothing is changed and 0 says the glob must fail instead.
#if defined(__GLIBC__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__APPLE__)
#define FS_GLOB_FILTER(NAME, GLOB_T)                                              \
    static int NAME(GLOB_T* g, size_t from) {                                     \
        size_t kept = from;                                                       \
        for (size_t i = from; i < g->gl_pathc; i++) {                             \
            char* p = g->gl_pathv[g->gl_offs + i];                                \
            if (p && fs_glob_match_allowed(p)) {                                  \
                g->gl_pathv[g->gl_offs + kept++] = p;                             \
                continue;                                                         \
            }                                                                     \
            free(p);                                                              \
        }                                                                         \
        g->gl_pathc = kept;                                                       \
        g->gl_pathv[g->gl_offs + kept] = NULL;                                    \
        return 1;                                                                 \
    }
#else
#define FS_GLOB_FILTER(NAME, GLOB_T)                                              \
    static int NAME(GLOB_T* g, size_t from) {                                     \
        for (size_t i = from; i < g->gl_pathc; i++) {                             \
            char* p = g->gl_pathv[g->gl_offs + i];                                \
            if (!p || !fs_glob_match_allowed(p)) return 0;                        \
        }                                                                         \
        return 1;                                                                 \
    }
#endif

static int fs_glob_match_allowed(const char* p) {
    char resolved[PATH_MAX];
    return aether_sandbox_canon_path(p, resolved, sizeof resolved) &&
           check_resolved("fs_read", resolved);
}

FS_GLOB_FILTER(fs_glob_filter, glob_t)

typedef int (*glob_errfn)(const char*, int);
static int (*real_glob)(const char*, int, glob_errfn, glob_t*) = NULL;
int glob(const char* pattern, int flags, glob_errfn errfunc, glob_t* g) {
    if (!policy_in_force()) return FS_REAL(glob)(pattern, flags, errfunc, g);
    if (!pattern || !g) return FS_REAL(glob)(pattern, flags, errfunc, g);
    if (!fs_glob_start_allowed(pattern)) return GLOB_ABORTED;
    size_t before = (flags & GLOB_APPEND) ? g->gl_pathc : 0;
    int r = FS_REAL(glob)(pattern, flags, errfunc, g);
    if (r != 0 && r != GLOB_NOMATCH) return r;
    if (g->gl_pathv && !fs_glob_filter(g, before)) return GLOB_ABORTED;
    if (r == 0 && g->gl_pathc == before) return GLOB_NOMATCH;
    return r;
}

// nftw and ftw walk a tree through libc's own opendir. Under a policy the
// root needs fs_read, and the caller's function is called only for entries
// the policy may read; the walk's own descent is libc's, the entries it
// reaches through a symlink are checked where they lead like any other.
typedef int (*nftw_fn)(const char*, const struct stat*, int, struct FTW*);
typedef int (*ftw_fn)(const char*, const struct stat*, int);
static __thread nftw_fn fs_nftw_user;
static __thread ftw_fn fs_ftw_user;

static int fs_tree_entry_allowed(const char* path) {
    char resolved[PATH_MAX];
    return aether_sandbox_canon_entry(path, resolved, sizeof resolved) &&
           check_resolved("fs_read", resolved);
}

static int fs_nftw_call(const char* path, const struct stat* st, int type, struct FTW* f) {
    if (!fs_tree_entry_allowed(path)) return 0;
    return fs_nftw_user(path, st, type, f);
}

static int fs_ftw_call(const char* path, const struct stat* st, int type) {
    if (!fs_tree_entry_allowed(path)) return 0;
    return fs_ftw_user(path, st, type);
}

static int (*real_nftw)(const char*, nftw_fn, int, int) = NULL;
int nftw(const char* root, nftw_fn fn, int fds, int flags) {
    if (!policy_in_force() || !fn) return FS_REAL(nftw)(root, fn, fds, flags);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, root, 0, AETHER_ACCESS_READ, resolved)) return -1;
    nftw_fn outer = fs_nftw_user;
    fs_nftw_user = fn;
    int r = FS_REAL(nftw)(root, fs_nftw_call, fds, flags);
    fs_nftw_user = outer;
    return r;
}

static int (*real_ftw)(const char*, ftw_fn, int) = NULL;
int ftw(const char* root, ftw_fn fn, int fds) {
    if (!policy_in_force() || !fn) return FS_REAL(ftw)(root, fn, fds);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, root, 0, AETHER_ACCESS_READ, resolved)) return -1;
    ftw_fn outer = fs_ftw_user;
    fs_ftw_user = fn;
    int r = FS_REAL(ftw)(root, fs_ftw_call, fds);
    fs_ftw_user = outer;
    return r;
}

#if AETHER_PRELOAD_HAS_FTS
// fts walks a tree through libc's own open: each root needs fs_read, and
// fts_read passes over an entry the policy may not read (and does not
// descend into it). fts_children lists the entry fts_read returned last,
// which was checked.
// FreeBSD's comparison takes its entries as const pointers to const.
#if defined(__FreeBSD__)
typedef int (*fts_compar_fn)(const FTSENT* const*, const FTSENT* const*);
#else
typedef int (*fts_compar_fn)(const FTSENT**, const FTSENT**);
#endif
static FTS* (*real_fts_open)(char* const*, int, fts_compar_fn) = NULL;
FTS* fts_open(char* const* roots, int options, fts_compar_fn compar) {
    if (!policy_in_force() || !roots) return FS_REAL(fts_open)(roots, options, compar);
    for (char* const* r = roots; *r; r++) {
        char resolved[PATH_MAX];
        if (!fs_allowed(AT_FDCWD, *r, 0, AETHER_ACCESS_READ, resolved)) return NULL;
    }
    return FS_REAL(fts_open)(roots, options, compar);
}

static FTSENT* (*real_fts_read)(FTS*) = NULL;
FTSENT* fts_read(FTS* ftsp) {
    for (;;) {
        FTSENT* e = FS_REAL(fts_read)(ftsp);
        if (!e || !policy_in_force() || fs_tree_entry_allowed(e->fts_path)) return e;
        if (e->fts_info == FTS_D) fts_set(ftsp, e, FTS_SKIP);
    }
}
#endif // AETHER_PRELOAD_HAS_FTS

// open_by_handle_at opens by a handle, not a path: the file it opened is
// named (fd_path) and checked, and closed again if the policy refuses it.
#if defined(__linux__)
static int (*real_open_by_handle_at)(int, struct file_handle*, int) = NULL;
int open_by_handle_at(int mountfd, struct file_handle* handle, int flags) {
    int fd = FS_REAL(open_by_handle_at)(mountfd, handle, flags);
    if (fd < 0 || !policy_in_force()) return fd;
    char target[PATH_MAX], run[PATH_MAX], resolved[PATH_MAX];
    int access = fs_open_access(flags);
    if (!fd_path(fd, target, run) || !aether_sandbox_canon_path(target, resolved, sizeof resolved) ||
        ((access & AETHER_ACCESS_READ) && !check_resolved("fs_read", resolved)) ||
        ((access & AETHER_ACCESS_WRITE) && !check_resolved("fs_write", resolved))) {
        if (!fd_path(fd, target, run)) log_deny("fs_read", "open_by_handle_at: a file with no path");
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}
#endif

// mount and its undoing change what a path leads to: fs_write on the
// mount point. A bind mount (MS_BIND) or a move (MS_MOVE) also takes a
// directory's tree from its source and shows it at the mount point, so the
// source is a path read through the mount point: fs_read on it, and
// fs_write unless the bind is read-only (MS_RDONLY), or for a move, which
// takes the source's mount away from it. Checking the mount point alone let
// a bind of a directory outside the grants onto one inside them read it
// (#3751). Both are passed to the kernel as they were resolved.
#if defined(__linux__)
static int (*real_mount)(const char*, const char*, const char*, unsigned long, const void*) = NULL;
int mount(const char* source, const char* target, const char* type, unsigned long flags,
          const void* data) {
    if (!policy_in_force()) return FS_REAL(mount)(source, target, type, flags, data);
    char rt[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, target, 0, AETHER_ACCESS_WRITE, rt)) return -1;
    const char* src = source;
    char rs[PATH_MAX];
    if ((flags & (MS_BIND | MS_MOVE)) && !(flags & MS_REMOUNT)) {
        int access = AETHER_ACCESS_READ;
        if ((flags & MS_MOVE) || !(flags & MS_RDONLY)) access |= AETHER_ACCESS_WRITE;
        if (!fs_allowed(AT_FDCWD, source, 0, access, rs)) return -1;
        src = rs;
    }
    return FS_REAL(mount)(src, rt, type, flags, data);
}

static int (*real_umount2)(const char*, int) = NULL;
int umount2(const char* target, int flags) {
    if (!policy_in_force()) return FS_REAL(umount2)(target, flags);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, target, 0, AETHER_ACCESS_WRITE, resolved)) return -1;
    return FS_REAL(umount2)(resolved, flags);
}

static int (*real_umount)(const char*) = NULL;
int umount(const char* target) {
    if (!policy_in_force()) return FS_REAL(umount)(target);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, target, 0, AETHER_ACCESS_WRITE, resolved)) return -1;
    return FS_REAL(umount)(resolved);
}

// Namespaces. A new mount namespace (with a user namespace, which needs no
// privilege) lets a process bind any directory onto any other where the
// path checks no longer describe what a path reaches; joining another
// process's namespace does the same. Under a policy unshare and clone with
// a CLONE_NEW* flag, and setns, need the native grant their raw system call
// needs ("syscall:unshare", "syscall:setns", as the syscall() wrapper names
// them); clone3 is held below. unshare with no namespace flag (CLONE_FILES,
// CLONE_FS, CLONE_SYSVSEM) changes only this process and goes through.
#define AE_CLONE_NEW_ANY (0x00020000UL /* NEWNS */ | 0x02000000UL /* NEWCGROUP */ | \
                          0x04000000UL /* NEWUTS */ | 0x08000000UL /* NEWIPC */ | \
                          0x10000000UL /* NEWUSER */ | 0x20000000UL /* NEWPID */ | \
                          0x40000000UL /* NEWNET */)
#define AE_CLONE_NEWTIME 0x00000080UL   /* unshare and clone3 only: in clone() it is the signal */

static int ns_allowed(const char* call) {
    if (!policy_in_force() || check_grant("native", call)) return 1;
    errno = EPERM;
    return 0;
}

static int (*real_unshare)(int) = NULL;
int unshare(int flags) {
    if (((unsigned long)(unsigned)flags & (AE_CLONE_NEW_ANY | AE_CLONE_NEWTIME)) &&
        !ns_allowed("syscall:unshare"))
        return -1;
    return FS_REAL(unshare)(flags);
}

static int (*real_setns)(int, int) = NULL;
int setns(int fd, int nstype) {
    if (!ns_allowed("syscall:setns")) return -1;
    return FS_REAL(setns)(fd, nstype);
}

// clone() is variadic: (fn, stack, flags, arg[, parent_tid, tls, child_tid]).
// The three trailing pointers are passed on as read; the kernel reads each
// only when a flag asks for it, which is when the caller passed it.
static int (*real_clone)(int (*)(void*), void*, int, void*, ...) = NULL;
int clone(int (*fn)(void*), void* stack, int flags, void* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    void* ptid = va_arg(ap, void*);
    void* tls = va_arg(ap, void*);
    void* ctid = va_arg(ap, void*);
    va_end(ap);
    if (((unsigned long)(unsigned)flags & AE_CLONE_NEW_ANY) && !ns_allowed("syscall:unshare")) return -1;
    if (!FS_REAL(clone)) { errno = ENOSYS; return -1; }
    return real_clone(fn, stack, flags, arg, ptid, tls, ctid);
}
#endif

// The glibc entry points of the calls above: the large-file variants
// (*64), and the _FORTIFY_SOURCE opens (__open_2 and its kin), which a
// program built with -D_FORTIFY_SOURCE calls instead of open. Each is its
// call above with O_LARGEFILE where that means anything.
#if defined(__linux__)
// glibc names the 64-bit types; on musl they are the plain ones, which are
// 64-bit already (and its headers no longer name the *64 types).
#if defined(__GLIBC__)
#define AE_OFF64 off64_t
#define AE_DIRENT64 dirent64
#define AE_READDIR64 readdir64
#define AE_STAT64 stat64
#define AE_GLOB64 glob64_t
#define AE_FTRUNCATE64 ftruncate64
#else
#define AE_OFF64 off_t
#define AE_DIRENT64 dirent
#define AE_READDIR64 readdir
#define AE_STAT64 stat
#define AE_GLOB64 glob_t
#define AE_FTRUNCATE64 ftruncate
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#ifndef open64
static int (*real_open64)(const char*, int, ...) = NULL;
int open64(const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(open64)(path, flags, mode);
    return fs_open_checked(AT_FDCWD, path, flags | O_LARGEFILE, mode);
}
#endif

#ifndef openat64
static int (*real_openat64)(int, const char*, int, ...) = NULL;
int openat64(int dirfd, const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(openat64)(dirfd, path, flags, mode);
    return fs_open_checked(dirfd, path, flags | O_LARGEFILE, mode);
}
#endif

#ifndef creat64
static int (*real_creat64)(const char*, mode_t) = NULL;
int creat64(const char* path, mode_t mode) {
    if (!policy_in_force()) return FS_REAL(creat64)(path, mode);
    return fs_open_checked(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, (unsigned)mode);
}
#endif

static int (*real___open_2)(const char*, int) = NULL;
int __open_2(const char* path, int flags) {
    if (!policy_in_force()) return FS_REAL(__open_2)(path, flags);
    return fs_open_checked(AT_FDCWD, path, flags, 0);
}

static int (*real___open64_2)(const char*, int) = NULL;
int __open64_2(const char* path, int flags) {
    if (!policy_in_force()) return FS_REAL(__open64_2)(path, flags);
    return fs_open_checked(AT_FDCWD, path, flags | O_LARGEFILE, 0);
}

static int (*real___openat_2)(int, const char*, int) = NULL;
int __openat_2(int dirfd, const char* path, int flags) {
    if (!policy_in_force()) return FS_REAL(__openat_2)(dirfd, path, flags);
    return fs_open_checked(dirfd, path, flags, 0);
}

static int (*real___openat64_2)(int, const char*, int) = NULL;
int __openat64_2(int dirfd, const char* path, int flags) {
    if (!policy_in_force()) return FS_REAL(__openat64_2)(dirfd, path, flags);
    return fs_open_checked(dirfd, path, flags | O_LARGEFILE, 0);
}

#ifndef fopen64
static FILE* (*real_fopen64)(const char*, const char*) = NULL;
FILE* fopen64(const char* path, const char* mode) {
    if (!policy_in_force()) return FS_REAL(fopen64)(path, mode);
    return fs_fopen_checked(path, mode, O_LARGEFILE);
}
#endif

#ifndef freopen64
static FILE* (*real_freopen64)(const char*, const char*, FILE*) = NULL;
FILE* freopen64(const char* path, const char* mode, FILE* stream) {
    return fs_freopen(FS_REAL(freopen64), path, mode, stream, O_LARGEFILE);
}
#endif

#ifndef truncate64
static int (*real_truncate64)(const char*, AE_OFF64) = NULL;
int truncate64(const char* path, AE_OFF64 length) {
    if (!policy_in_force()) return FS_REAL(truncate64)(path, length);
    int fd = fs_open_checked(AT_FDCWD, path, O_WRONLY | O_CLOEXEC | O_LARGEFILE, 0);
    if (fd < 0) return -1;
    int r = AE_FTRUNCATE64(fd, length);
    FS_AT_CALL(fd, r);
}
#endif

#ifndef mkstemp64
static int (*real_mkstemp64)(char*) = NULL;
int mkstemp64(char* tmpl) {
    if (!policy_in_force()) return FS_REAL(mkstemp64)(tmpl);
    return fs_mkstemps(tmpl, 0, O_LARGEFILE);
}
#endif

#ifndef mkostemp64
static int (*real_mkostemp64)(char*, int) = NULL;
int mkostemp64(char* tmpl, int flags) {
    if (!policy_in_force()) return FS_REAL(mkostemp64)(tmpl, flags);
    return fs_mkstemps(tmpl, 0, flags | O_LARGEFILE);
}
#endif

#ifndef mkstemps64
static int (*real_mkstemps64)(char*, int) = NULL;
int mkstemps64(char* tmpl, int suffix) {
    if (!policy_in_force()) return FS_REAL(mkstemps64)(tmpl, suffix);
    return fs_mkstemps(tmpl, suffix, O_LARGEFILE);
}
#endif

#ifndef mkostemps64
static int (*real_mkostemps64)(char*, int, int) = NULL;
int mkostemps64(char* tmpl, int suffix, int flags) {
    if (!policy_in_force()) return FS_REAL(mkostemps64)(tmpl, suffix, flags);
    return fs_mkstemps(tmpl, suffix, flags | O_LARGEFILE);
}
#endif

#ifndef tmpfile64
static FILE* (*real_tmpfile64)(void) = NULL;
FILE* tmpfile64(void) {
    if (!policy_in_force()) return FS_REAL(tmpfile64)();
    char resolved[PATH_MAX];
    char name[PATH_MAX];
    snprintf(name, sizeof name, "%s/tmpfile", P_tmpdir);
    if (!fs_allowed(AT_FDCWD, name, 1, AETHER_ACCESS_WRITE, resolved)) return NULL;
    return FS_REAL(tmpfile64)();
}
#endif

#ifndef scandir64
FS_SCANDIR(scandir64, AE_DIRENT64, AE_READDIR64)
#ifndef scandirat64
FS_SCANDIRAT(scandirat64, fs_list_scandir64, AE_DIRENT64)
#endif
#endif

// __open and __open64: glibc exports them beside open, and a program can
// call them by name.
static int (*real___open)(const char*, int, ...) = NULL;
int __open(const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(__open)(path, flags, mode);
    return fs_open_checked(AT_FDCWD, path, flags, mode);
}

static int (*real___open64)(const char*, int, ...) = NULL;
int __open64(const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(__open64)(path, flags, mode);
    return fs_open_checked(AT_FDCWD, path, flags | O_LARGEFILE, mode);
}

#if defined(__linux__)   // glibc, and musl, which exports them too
// __xmknod and __xmknodat: what mknod and mknodat compiled to before glibc
// 2.33, still exported for those programs.
static int (*real___xmknodat)(int, int, const char*, mode_t, dev_t*) = NULL;
static int (*real___xmknod)(int, const char*, mode_t, dev_t*) = NULL;
int __xmknod(int ver, const char* path, mode_t mode, dev_t* dev) {
    if (!policy_in_force()) return FS_REAL(__xmknod)(ver, path, mode, dev);
    char name[PATH_MAX];
    int d = fs_on_name(AT_FDCWD, path, name);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(__xmknodat)(ver, d, name, mode, dev));
}

int __xmknodat(int ver, int dirfd, const char* path, mode_t mode, dev_t* dev) {
    if (!policy_in_force()) return FS_REAL(__xmknodat)(ver, dirfd, path, mode, dev);
    char name[PATH_MAX];
    int d = fs_on_name(dirfd, path, name);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(__xmknodat)(ver, d, name, mode, dev));
}
#endif

#ifndef glob64
#if defined(__GLIBC__)
FS_GLOB_FILTER(fs_glob64_filter, glob64_t)
#else
#define fs_glob64_filter fs_glob_filter
#endif
static int (*real_glob64)(const char*, int, glob_errfn, AE_GLOB64*) = NULL;
int glob64(const char* pattern, int flags, glob_errfn errfunc, AE_GLOB64* g) {
    if (!policy_in_force() || !pattern || !g) return FS_REAL(glob64)(pattern, flags, errfunc, g);
    if (!fs_glob_start_allowed(pattern)) return GLOB_ABORTED;
    size_t before = (flags & GLOB_APPEND) ? g->gl_pathc : 0;
    int r = FS_REAL(glob64)(pattern, flags, errfunc, g);
    if (r != 0 && r != GLOB_NOMATCH) return r;
    if (g->gl_pathv && !fs_glob64_filter(g, before)) return GLOB_ABORTED;
    if (r == 0 && g->gl_pathc == before) return GLOB_NOMATCH;
    return r;
}
#endif

#ifndef nftw64
typedef int (*nftw64_fn)(const char*, const struct AE_STAT64*, int, struct FTW*);
static __thread nftw64_fn fs_nftw64_user;
static int fs_nftw64_call(const char* path, const struct AE_STAT64* st, int type, struct FTW* f) {
    if (!fs_tree_entry_allowed(path)) return 0;
    return fs_nftw64_user(path, st, type, f);
}
static int (*real_nftw64)(const char*, nftw64_fn, int, int) = NULL;
int nftw64(const char* root, nftw64_fn fn, int fds, int flags) {
    if (!policy_in_force() || !fn) return FS_REAL(nftw64)(root, fn, fds, flags);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, root, 0, AETHER_ACCESS_READ, resolved)) return -1;
    nftw64_fn outer = fs_nftw64_user;
    fs_nftw64_user = fn;
    int r = FS_REAL(nftw64)(root, fs_nftw64_call, fds, flags);
    fs_nftw64_user = outer;
    return r;
}
#endif

#ifndef ftw64
typedef int (*ftw64_fn)(const char*, const struct AE_STAT64*, int);
static __thread ftw64_fn fs_ftw64_user;
static int fs_ftw64_call(const char* path, const struct AE_STAT64* st, int type) {
    if (!fs_tree_entry_allowed(path)) return 0;
    return fs_ftw64_user(path, st, type);
}
static int (*real_ftw64)(const char*, ftw64_fn, int) = NULL;
int ftw64(const char* root, ftw64_fn fn, int fds) {
    if (!policy_in_force() || !fn) return FS_REAL(ftw64)(root, fn, fds);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, root, 0, AETHER_ACCESS_READ, resolved)) return -1;
    ftw64_fn outer = fs_ftw64_user;
    fs_ftw64_user = fn;
    int r = FS_REAL(ftw64)(root, fs_ftw64_call, fds);
    fs_ftw64_user = outer;
    return r;
}
#endif

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)
// glibc 2.34's fts64 (fts with _FILE_OFFSET_BITS=64 on 32-bit systems).
static FTS64* (*real_fts64_open)(char* const*, int, int (*)(const FTSENT64**, const FTSENT64**)) = NULL;
FTS64* fts64_open(char* const* roots, int options, int (*compar)(const FTSENT64**, const FTSENT64**)) {
    if (!policy_in_force() || !roots) return FS_REAL(fts64_open)(roots, options, compar);
    for (char* const* r = roots; *r; r++) {
        char resolved[PATH_MAX];
        if (!fs_allowed(AT_FDCWD, *r, 0, AETHER_ACCESS_READ, resolved)) return NULL;
    }
    return FS_REAL(fts64_open)(roots, options, compar);
}

static FTSENT64* (*real_fts64_read)(FTS64*) = NULL;
FTSENT64* fts64_read(FTS64* ftsp) {
    for (;;) {
        FTSENT64* e = FS_REAL(fts64_read)(ftsp);
        if (!e || !policy_in_force() || fs_tree_entry_allowed(e->fts_path)) return e;
        if (e->fts_info == FTS_D) fts64_set(ftsp, e, FTS_SKIP);
    }
}
#endif

#endif // __linux__ (large-file and fortify entry points)


#define MAX_EXEC_ENV_ENTRIES 4096
extern char** environ;

static int env_key_is(const char* entry, const char* key) {
    size_t n;
    if (!entry || !key) return 0;
    n = strlen(key);
    return strncmp(entry, key, n) == 0 && entry[n] == '=';
}

static int same_env_key(const char* a, const char* b) {
    const char* ae;
    const char* be;
    if (!a || !b) return 0;
    ae = strchr(a, '=');
    be = strchr(b, '=');
    if (!ae || !be) return 0;
    return (ae - a) == (be - b) && strncmp(a, b, (size_t)(ae - a)) == 0;
}

static int is_sandbox_control_env(const char* entry) {
    static const char prefix[] = "AETHER_SANDBOX_";
    if (!entry) return 0;
    return env_key_is(entry, "LD_PRELOAD") ||
           env_key_is(entry, "AETHER_CAPSICUM") ||
           strncmp(entry, prefix, sizeof(prefix) - 1) == 0;
}

// Snapshot of the sandbox controls taken once in sandbox_init, from the
// environment as it was before user code ran. execve rebuilds the child's
// controls from this, never from live environ, which the contained process can
// edit (setenv/unsetenv). A key absent from the snapshot was absent at init;
// an empty value ("KEY=") is recorded as present. Fixed buffers, no malloc,
// so execve stays async-signal-safe.
#define SNAP_MAX_ENTRIES 32
#define SNAP_BUF_SIZE 16384
static char snap_buf[SNAP_BUF_SIZE];
static char* snap_entries[SNAP_MAX_ENTRIES];
static size_t snap_count = 0;
static int snap_overflow = 0;

static void snapshot_sandbox_env(void) {
    size_t used = 0;
    for (size_t i = 0; environ && environ[i]; i++) {
        const char* e = environ[i];
        if (!is_sandbox_control_env(e)) continue;
        size_t len = strlen(e);
        if (snap_count >= SNAP_MAX_ENTRIES || len + 1 > SNAP_BUF_SIZE - used) {
            snap_overflow = 1;   // cannot preserve controls: execve fails closed
            return;
        }
        memcpy(snap_buf + used, e, len);
        snap_buf[used + len] = '\0';
        snap_entries[snap_count++] = snap_buf + used;
        used += len + 1;
    }
}

static int env_has_nonempty(char* const env[], size_t count, const char* key) {
    for (size_t i = 0; i < count; i++) {
        if (env_key_is(env[i], key)) {
            const char* value = strchr(env[i], '=');
            return value && value[1] != '\0';
        }
    }
    return 0;
}

// The environment a contained process's child gets: the caller's entries
// minus every sandbox control, plus the controls from the init-time snapshot.
// So an explicit environment cannot drop or replace the preload and the grant
// source. On the stack, no malloc: it runs between fork (or vfork) and exec.
// Returns 0 with errno set when it cannot.
static int child_env(char* const envp[], char* merged_env[MAX_EXEC_ENV_ENTRIES + 1]) {
    size_t count = 0;
    size_t scanned = 0;
    while (envp && envp[scanned]) {
        if (scanned >= MAX_EXEC_ENV_ENTRIES) {
            errno = E2BIG;
            return 0;
        }
        if (!is_sandbox_control_env(envp[scanned])) {
            if (count >= MAX_EXEC_ENV_ENTRIES) {
                errno = E2BIG;
                return 0;
            }
            merged_env[count++] = envp[scanned];
        }
        scanned++;
    }

    if (snap_overflow) {
        errno = EPERM;
        return 0;
    }
    for (size_t i = 0; i < snap_count; i++) {
        if (count >= MAX_EXEC_ENV_ENTRIES) {
            errno = E2BIG;
            return 0;
        }
        merged_env[count++] = snap_entries[i];
    }
    merged_env[count] = NULL;

    if (!env_has_nonempty(merged_env, count, "LD_PRELOAD") ||
        (!env_has_nonempty(merged_env, count, "AETHER_SANDBOX_SHM") &&
         !env_has_nonempty(merged_env, count, "AETHER_SANDBOX_GRANTS") &&
         !env_has_nonempty(merged_env, count, "AETHER_SANDBOX_GRANTS_FD"))) {
        errno = EPERM;
        return 0;
    }
    return 1;
}

// Run `pathname` once its exec grant has been checked: with the child's
// environment rebuilt under a grant file, as is otherwise.
static int exec_checked(const char* pathname, char* const argv[], char* const envp[]) {
    if (!real_execve) real_execve = dlsym(RTLD_NEXT, "execve");
    if (!sandbox_active) return real_execve(pathname, argv, envp);
    char* merged_env[MAX_EXEC_ENV_ENTRIES + 1];
    if (!child_env(envp, merged_env)) return -1;
    return real_execve(pathname, argv, merged_env);
}

// execve
int execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (pathname && !check_grant("exec", pathname)) {
        errno = EPERM;
        return -1;
    }
    return exec_checked(pathname, argv, envp);
}

// The rest of the exec family. libc implements each of these on its own
// internal execve, never the exported one, so the wrapper above did not see
// them: under a grant file with no exec grant, execv, execvp, execl and the
// rest ran their program. Each comes here and goes through execve above.
// execvp, execvpe and execlp search PATH here, the way glibc does, so the
// grant is checked against each file actually run.
#define MAX_EXEC_ARGS 4096
#define SANDBOX_SHELL AETHER_SANDBOX_SHELL   // "/bin/sh": the one name std.os checks too

// A file that is not an executable format (no #! line) is run as a shell
// script by execvp and execlp, as glibc and FreeBSD do; through execve above,
// so the shell needs its exec grant.
static int exec_or_script(const char* path, char* const argv[], char* const envp[]) {
    execve(path, argv, envp);
    if (errno != ENOEXEC) return -1;
    size_t argc = 0;
    while (argv && argv[argc]) argc++;
    if (argc + 2 > MAX_EXEC_ARGS) {
        errno = E2BIG;
        return -1;
    }
    // Sized to the arguments: this can run in a vfork child, on the
    // parent's stack.
    char* sh_argv[argc + 2];
    sh_argv[0] = (char*)SANDBOX_SHELL;
    sh_argv[1] = (char*)path;
    size_t n = 2;
    for (size_t i = 1; i < argc; i++) sh_argv[n++] = argv[i];
    sh_argv[n] = NULL;
    return execve(SANDBOX_SHELL, sh_argv, envp);
}

// PATH search as glibc's execvpe does it: a name with a slash is run as it
// is; otherwise each PATH entry in turn (an empty entry is the current
// directory), going on past a missing or unreadable one and stopping at any
// other failure, a refused grant included.
static int exec_search(const char* file, const char* path, char* const argv[], char* const envp[]) {
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/')) return exec_or_script(file, argv, envp);
    size_t flen = strlen(file);
    char buf[PATH_MAX];
    int got_eacces = 0;
    const char* p = path;
    for (;;) {
        const char* colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen + 1 + flen + 1 <= sizeof(buf)) {
            memcpy(buf, p, dlen);
            size_t at = dlen;
            if (dlen) buf[at++] = '/';
            memcpy(buf + at, file, flen + 1);
            exec_or_script(buf, argv, envp);
            switch (errno) {
                case EACCES: got_eacces = 1; break;
                case ENOENT: case ESTALE: case ENOTDIR: case ENODEV: case ETIMEDOUT: break;
                default: return -1;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    errno = got_eacces ? EACCES : ENOENT;
    return -1;
}

static const char* exec_path(void) {
    if (!real_getenv) real_getenv = dlsym(RTLD_NEXT, "getenv");
    const char* path = real_getenv ? real_getenv("PATH") : NULL;
    return path ? path : "/bin:/usr/bin";
}

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char* file, char* const argv[]) {
    return exec_search(file, exec_path(), argv, environ);
}

int execvpe(const char* file, char* const argv[], char* const envp[]) {
    return exec_search(file, exec_path(), argv, envp);
}

#if defined(__FreeBSD__)
int execvP(const char* file, const char* search_path, char* const argv[]) {
    return exec_search(file, search_path ? search_path : "", argv, environ);
}
#endif

// The variadic forms: their arguments into an argv on the stack, sized to
// them (this can run in a vfork child, on the parent's stack). Counted
// first, up to and including the terminating NULL.
static size_t count_args(const char* arg0, va_list ap) {
    size_t n = 1;
    for (const char* a = arg0; a && n <= MAX_EXEC_ARGS; n++) a = va_arg(ap, const char*);
    return n;
}

static void fill_args(const char* arg0, va_list ap, char* argv[], size_t n) {
    argv[0] = (char*)arg0;
    for (size_t i = 1; i < n; i++) argv[i] = va_arg(ap, char*);
}

int execl(const char* path, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    size_t n = count_args(arg, ap);
    va_end(ap);
    if (n > MAX_EXEC_ARGS) {
        errno = E2BIG;
        return -1;
    }
    char* argv[n];
    va_start(ap, arg);
    fill_args(arg, ap, argv, n);
    va_end(ap);
    return execve(path, argv, environ);
}

int execle(const char* path, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    size_t n = count_args(arg, ap);
    va_end(ap);
    if (n > MAX_EXEC_ARGS) {
        errno = E2BIG;
        return -1;
    }
    char* argv[n];
    va_start(ap, arg);
    fill_args(arg, ap, argv, n);
    char** envp = va_arg(ap, char**);   // after the terminating NULL
    va_end(ap);
    return execve(path, argv, envp);
}

int execlp(const char* file, const char* arg, ...) {
    va_list ap;
    va_start(ap, arg);
    size_t n = count_args(arg, ap);
    va_end(ap);
    if (n > MAX_EXEC_ARGS) {
        errno = E2BIG;
        return -1;
    }
    char* argv[n];
    va_start(ap, arg);
    fill_args(arg, ap, argv, n);
    va_end(ap);
    return exec_search(file, exec_path(), argv, environ);
}

// The absolute path of the file `fd` is open on, into `out`, and in `run` a
// name execve can run that same file by (/proc/self/fd/N on Linux, the path
// itself on FreeBSD). 0 when the descriptor's file cannot be named.
static int fd_path(int fd, char out[PATH_MAX], char run[PATH_MAX]) {
    if (fd < 0) return 0;
#if defined(__linux__)
    char link[32] = "/proc/self/fd/";
    char digits[16];
    int d = 0;
    unsigned int v = (unsigned int)fd;
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < (int)sizeof(digits));
    size_t at = strlen(link);
    while (d > 0) link[at++] = digits[--d];
    link[at] = '\0';
    ssize_t n = readlink(link, out, PATH_MAX - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    if (out[0] != '/') return 0;
    memcpy(run, link, at + 1);
    return 1;
#elif defined(__FreeBSD__) && defined(F_KINFO)
    struct kinfo_file kf;
    memset(&kf, 0, sizeof(kf));
    kf.kf_structsize = sizeof(kf);
    if (fcntl(fd, F_KINFO, &kf) != 0 || kf.kf_path[0] != '/') return 0;
    size_t n = strnlen(kf.kf_path, PATH_MAX - 1);
    memcpy(out, kf.kf_path, n);
    out[n] = '\0';
    memcpy(run, out, n + 1);
    return 1;
#else
    (void)out;
    (void)run;
    return 0;
#endif
}

// Is any policy in force here: a grant file, or this thread's in-process
// levels (an enforce block, a host bridge's sandboxed run)?
static int policy_in_force(void) {
    // The constructor's own calls (the Landlock rules it makes) are not
    // held, and the runtime is not asked before its own constructors ran.
    if (g_preload_constructing) return 0;
    int in_process = aether_sandbox_levels_here ? aether_sandbox_levels_here() : 0;
    return sandbox_active || in_process > 0;
}

// fexecve runs whatever file the descriptor is open on: the grant is checked
// against that file's path, and the program is run through execve by a name
// for it. A descriptor whose file cannot be named is refused whenever a
// policy is in force.
int fexecve(int fd, char* const argv[], char* const envp[]) {
    char target[PATH_MAX];
    char run[PATH_MAX];
    if (!fd_path(fd, target, run)) {
        if (policy_in_force()) {
            log_deny("exec", "fexecve: a descriptor with no path");
            errno = EPERM;
            return -1;
        }
        static int (*real_fexecve)(int, char* const[], char* const[]) = NULL;
        if (!real_fexecve) real_fexecve = dlsym(RTLD_NEXT, "fexecve");
        return real_fexecve(fd, argv, envp);
    }
    if (!check_grant("exec", target)) {
        errno = EPERM;
        return -1;
    }
    return exec_checked(run, argv, envp);
}

#if defined(__linux__) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
// execveat (glibc 2.34): execve of a name relative to a directory
// descriptor, or of the descriptor itself (AT_EMPTY_PATH). The file it
// names is worked out here, its grant checked, and the call made with the
// environment execve would give.
static int (*real_execveat)(int, const char*, char* const[], char* const[], int) = NULL;
int execveat(int dirfd, const char* pathname, char* const argv[], char* const envp[], int flags) {
    if (!real_execveat) real_execveat = dlsym(RTLD_NEXT, "execveat");
    char target[PATH_MAX];
    char run[PATH_MAX];
    int named = 0;
    if (pathname[0] == '/' || (dirfd == AT_FDCWD && pathname[0])) {
        // As execve sees it: an absolute name, or one relative to the
        // working directory.
        size_t n = strlen(pathname);
        if (n < PATH_MAX) {
            memcpy(target, pathname, n + 1);
            named = 1;
        }
    } else if (!pathname[0] && (flags & AT_EMPTY_PATH)) {
        named = fd_path(dirfd, target, run);
    } else if (pathname[0] && fd_path(dirfd, target, run)) {
        size_t dn = strlen(target);
        size_t pn = strlen(pathname);
        if (dn + 1 + pn < PATH_MAX) {
            target[dn] = '/';
            memcpy(target + dn + 1, pathname, pn + 1);
            named = 1;
        }
    }
    if (!named) {
        if (policy_in_force()) {
            log_deny("exec", "execveat: a file with no path");
            errno = EPERM;
            return -1;
        }
        return real_execveat(dirfd, pathname, argv, envp, flags);
    }
    if (!check_grant("exec", target)) {
        errno = EPERM;
        return -1;
    }
    if (!sandbox_active) return real_execveat(dirfd, pathname, argv, envp, flags);
    char* merged_env[MAX_EXEC_ENV_ENTRIES + 1];
    if (!child_env(envp, merged_env)) return -1;
    return real_execveat(dirfd, pathname, argv, merged_env, flags);
}
#endif

// stat/access/fstatat — NOT intercepted. Modern glibc inlines these as
// direct syscalls via asm, bypassing any LD_PRELOAD override. Documented
// as a known gap. File CONTENT access (open/read) is still intercepted.

// fork — block by default, grant with "fork" category
static pid_t (*real_fork)(void) = NULL;
pid_t fork(void) {
    if (!real_fork) real_fork = dlsym(RTLD_NEXT, "fork");
    if (sandbox_active && !check_grant("fork", "*")) {
        errno = EPERM;
        return -1;
    }
    return real_fork();
}

// clone3 — block by default (same as fork)
// clone() is variadic and arch-specific, hard to intercept portably.
// clone3() is the modern replacement (Linux 5.3+), clean signature.
// Linux-only: <linux/sched.h> and struct clone_args do not exist on
// FreeBSD, which also has no clone3 syscall — process creation there
// goes through fork/vfork (both intercepted above).
#if defined(__linux__)
#include <linux/sched.h>
static long (*real_clone3)(struct clone_args*, size_t) = NULL;
long clone3(struct clone_args* args, size_t size) {
    if (!real_clone3) real_clone3 = dlsym(RTLD_NEXT, "clone3");
    /* A thread is no new process (#3178): it passes here, as it passes the
     * kernel fence spawn_sandboxed installs. */
    int thread = args && (args->flags & CLONE_THREAD);
    if (sandbox_active && !thread && !check_grant("fork", "*")) {
        errno = EPERM;
        return -1;
    }
    /* A new namespace needs what unshare needs (see unshare above). */
    if (args && (args->flags & (AE_CLONE_NEW_ANY | AE_CLONE_NEWTIME)) && !ns_allowed("syscall:unshare"))
        return -1;
    return real_clone3(args, size);
}
#endif // __linux__ (clone3)

// vfork — NOT intercepted. A C-language wrapper around vfork() is unsafe:
// vfork's contract requires the child to call only _exit() or execve()
// before returning from the function that called vfork(). Wrapping vfork
// in our own C function means the child returns from *our* frame on the
// parent's shared stack, corrupting it — which manifests as the calling
// program (gcc driver, etc.) segfaulting after the wait4. We rely on the
// fork() and execve() interceptors to gate process creation: a vfork()
// without a follow-up execve() is virtually unheard of outside of glibc
// internals (where it's harmless), and any exec by a vfork'd child is
// caught by the execve interceptor below. See
// sandbox-preload-toolchain-segfault.md for the bug this fixes.

// The process starters that are not fork plus execve. libc starts the child
// inside itself (clone or vfork, then its internal execve), so neither the
// fork nor the execve wrapper sees it: under a grant file with no exec or
// fork grant, posix_spawn, posix_spawnp, system and popen all ran their
// program, and so did os.execute and io.popen in a contained-in-process Lua.
// Each applies here the two rules the wrappers above would have applied had
// libc gone through them: a new process needs what fork() needs, and the
// program run needs its exec grant.

// fork()'s rule, for a starter that makes a new process: under a grant
// file, the fork grant.
static int new_process_allowed(void) {
    return !sandbox_active || check_grant("fork", "*");
}

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
// _Fork (glibc 2.34): fork without the atfork handlers, a public symbol of
// its own that fork() does not go through, so it gets fork()'s rule.
static pid_t (*real__Fork)(void) = NULL;
pid_t _Fork(void) {
    if (!real__Fork) real__Fork = dlsym(RTLD_NEXT, "_Fork");
    if (!new_process_allowed()) {
        errno = EPERM;
        return -1;
    }
    return real__Fork();
}
#endif

typedef int (*spawn_fn)(void*, const char*, const posix_spawn_file_actions_t*,
                        const posix_spawnattr_t*, char* const[], char* const[]);

// posix_spawn's checks and environment, for the real starter `real`: its
// first argument a pid_t* (posix_spawn) or an int* pidfd (pidfd_spawn).
// Returns an error number, as posix_spawn does.
static int spawn_checked(spawn_fn real, void* who, const char* path,
                         const posix_spawn_file_actions_t* fa,
                         const posix_spawnattr_t* attr,
                         char* const argv[], char* const envp[]) {
    if (!new_process_allowed()) return EPERM;
    if (path && !check_grant("exec", path)) return EPERM;
    if (!sandbox_active) return real(who, path, fa, attr, argv, envp);
    char* merged_env[MAX_EXEC_ENV_ENTRIES + 1];
    if (!child_env(envp, merged_env)) return errno;
    return real(who, path, fa, attr, argv, merged_env);
}

// posix_spawnp's PATH search, done here so the grant is checked against the
// file that runs: the first PATH entry holding a regular file this process
// may execute, which is then started by its full path. A name with a slash
// is used as it is. Fills `out`; 0 or an error number.
static int spawn_search(const char* file, char out[PATH_MAX]) {
    if (!file || !*file) return ENOENT;
    if (strchr(file, '/')) {
        if (strlen(file) >= PATH_MAX) return ENAMETOOLONG;
        memcpy(out, file, strlen(file) + 1);
        return 0;
    }
    size_t flen = strlen(file);
    int got_eacces = 0;
    const char* p = exec_path();
    for (;;) {
        const char* colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen + 1 + flen + 1 <= PATH_MAX) {
            memcpy(out, p, dlen);
            size_t at = dlen;
            if (dlen) out[at++] = '/';
            memcpy(out + at, file, flen + 1);
            struct stat st;
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
                if (faccessat(AT_FDCWD, out, X_OK, AT_EACCESS) == 0) return 0;
                got_eacces = 1;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    return got_eacces ? EACCES : ENOENT;
}

static spawn_fn real_posix_spawn = NULL;

int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa,
                const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
    if (!real_posix_spawn) real_posix_spawn = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    return spawn_checked(real_posix_spawn, pid, path, fa, attr, argv, envp);
}

int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
                 const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
    if (!real_posix_spawn) real_posix_spawn = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    char found[PATH_MAX];
    int err = spawn_search(file, found);
    if (err) return err;
    return spawn_checked(real_posix_spawn, pid, found, fa, attr, argv, envp);
}

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 39))
// pidfd_spawn and pidfd_spawnp (glibc 2.39): posix_spawn's starters that
// hand back a pidfd.
static spawn_fn real_pidfd_spawn = NULL;
int pidfd_spawn(int* pidfd, const char* path, const posix_spawn_file_actions_t* fa,
                const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
    if (!real_pidfd_spawn) real_pidfd_spawn = (spawn_fn)dlsym(RTLD_NEXT, "pidfd_spawn");
    return spawn_checked(real_pidfd_spawn, pidfd, path, fa, attr, argv, envp);
}
int pidfd_spawnp(int* pidfd, const char* file, const posix_spawn_file_actions_t* fa,
                 const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
    if (!real_pidfd_spawn) real_pidfd_spawn = (spawn_fn)dlsym(RTLD_NEXT, "pidfd_spawn");
    char found[PATH_MAX];
    int err = spawn_search(file, found);
    if (err) return err;
    return spawn_checked(real_pidfd_spawn, pidfd, found, fa, attr, argv, envp);
}
#endif

// The shell: system(), popen() and wordexp()'s command substitution run
// /bin/sh. It needs fork()'s rule and an exec grant for /bin/sh, as the
// fork and execve it amounts to would. In-process, std.os's os.system and
// os.exec apply the same exec rule (aether_sandbox_check_shell, #3735).
// The shell inherits the live
// environment, which (unlike execve's) cannot be rebuilt here, so under a
// grant file it must still carry exactly the controls the process started
// with: one that has dropped or changed them is refused, as execve would
// replace them.
static int live_env_keeps_controls(void) {
    if (snap_overflow) return 0;
    size_t live = 0;
    for (size_t i = 0; environ && environ[i]; i++) {
        if (!is_sandbox_control_env(environ[i])) continue;
        int found = 0;
        for (size_t j = 0; j < snap_count; j++) {
            if (strcmp(environ[i], snap_entries[j]) == 0) { found = 1; break; }
        }
        if (!found) return 0;
        live++;
    }
    return live == snap_count;
}

static int shell_allowed(void) {
    if (!new_process_allowed()) return 0;
    if (!check_grant("exec", SANDBOX_SHELL)) return 0;
    if (sandbox_active && !live_env_keeps_controls()) {
        log_deny("exec", "a shell whose environment lost the sandbox controls");
        return 0;
    }
    return 1;
}

static int (*real_system)(const char*) = NULL;
int system(const char* command) {
    if (!real_system) real_system = dlsym(RTLD_NEXT, "system");
    if (!shell_allowed()) {
        errno = EPERM;
        return command ? -1 : 0;   // system(NULL): 0, no shell to be had
    }
    return real_system(command);
}

static FILE* (*real_popen)(const char*, const char*) = NULL;
FILE* popen(const char* command, const char* mode) {
    if (!real_popen) real_popen = dlsym(RTLD_NEXT, "popen");
    if (!shell_allowed()) {
        errno = EPERM;
        return NULL;
    }
    return real_popen(command, mode);
}

// wordexp. glibc runs only a command substitution ($(...) or `...`) through
// the shell, and not at all under WRDE_NOCMD: where the shell is not
// allowed, WRDE_NOCMD is added, so a substitution fails with WRDE_CMDSUB and
// plain words still expand. FreeBSD's wordexp runs /bin/sh for every call,
// WRDE_NOCMD or not: there it is refused outright, with WRDE_CMDSUB, an
// error after which POSIX leaves `we` as it was (after WRDE_NOSPACE a
// caller would wordfree it, and it was never filled). OpenBSD's libc has
// no wordexp at all (deliberately), so there is nothing to wrap there.
#if !defined(__OpenBSD__)
static int (*real_wordexp)(const char*, wordexp_t*, int) = NULL;
int wordexp(const char* words, wordexp_t* we, int flags) {
    if (!real_wordexp) real_wordexp = dlsym(RTLD_NEXT, "wordexp");
#if defined(__GLIBC__)
    if (!(flags & WRDE_NOCMD) && !shell_allowed()) flags |= WRDE_NOCMD;
#else
    if (!shell_allowed()) return WRDE_CMDSUB;
#endif
    return real_wordexp(words, we, flags);
}
#endif // !__OpenBSD__ (wordexp)

// mmap — block PROT_EXEC to prevent shellcode execution from byte arrays
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    if (sandbox_active && (prot & 0x4)) {  // PROT_EXEC = 0x4
        // Allow the dynamic linker and JIT runtimes to map their own code.
        // Block anonymous executable mappings (fd == -1) which are
        // the shellcode vector: mmap(-1, ..., PROT_EXEC, MAP_ANONYMOUS, ...)
        if (fd == -1) {
            log_deny("native", "mmap_exec_anonymous");
            errno = EACCES;
            return (void*)-1;  // MAP_FAILED
        }
    }
    return real_mmap(addr, length, prot, flags, fd, offset);
}

// mmap64 — glibc may redirect mmap to mmap64. Linux-only: FreeBSD's
// mmap is already 64-bit and exposes no mmap64 symbol; the plain mmap
// interception above covers it.
#if defined(__linux__)
// Its own function behind it, and glibc's 64-bit offset: on a 32-bit
// system off_t is 32 bits, so the old shape (off_t, and plain mmap behind
// it) did not compile there and cut the offset.
#if defined(__GLIBC__)
typedef off64_t aether_mmap64_off;
#else
typedef off_t aether_mmap64_off;
#endif
static void* (*real_mmap64)(void*, size_t, int, int, int, aether_mmap64_off) = NULL;
void* mmap64(void* addr, size_t length, int prot, int flags, int fd, aether_mmap64_off offset) {
    if (!real_mmap64) real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    if (sandbox_active && (prot & 0x4) && fd == -1) {
        log_deny("native", "mmap_exec_anonymous");
        errno = EACCES;
        return (void*)-1;
    }
    return real_mmap64(addr, length, prot, flags, fd, offset);
}
#endif // __linux__ (mmap64)

// mprotect — block adding PROT_EXEC to existing mappings
int mprotect(void* addr, size_t len, int prot) {
    if (!real_mprotect) real_mprotect = dlsym(RTLD_NEXT, "mprotect");
    if (sandbox_active && (prot & 0x4)) {  // PROT_EXEC
        log_deny("native", "mprotect_exec");
        errno = EACCES;
        return -1;
    }
    return real_mprotect(addr, len, prot);
}

// dlopen of libc or the dynamic loader: refused without a native grant.
//
// A handle on libc is the ctypes / Fiddle / DynaLoader escape: dlsym on it
// finds the real open, connect and execve, past this library's. The check
// used to compare the name with "libc.so.6" and "libc.so", so
// CDLL("/usr/lib/libc.so.6") got round it. Now a name is libc's or the
// loader's when any of these is:
//   - the basename of the name as given ("libc.so.6", ".../libc.so.6");
//   - the basename of where a path leads, ".." and symlinks resolved, so a
//     symlink called libplain.so is seen for what it points at;
//   - the object the loader opened: after a load is let through, a handle
//     that is libc's or the loader's own (the ones this process is running
//     on) is closed again and refused, which catches any spelling the loader
//     resolves by its own search (a hard link, an LD_LIBRARY_PATH entry).
// /lib and /usr/lib, multiarch directories and lib64 need nothing of their
// own: the basename and the handle are the same through all of them.
//
// This is the preload layer, a guest's libc calls. On Linux the wall is
// Landlock (sandbox.spawn_socket), which refuses the open of a file outside
// the grants however the guest reaches the syscall; a guest that copies
// libc somewhere it can write, or issues the syscall itself, is stopped
// there, not here.

// glibc "libc.so", "libc.so.6", "libc-2.31.so"; musl "libc.musl-x86_64.so.1";
// FreeBSD "libc.so.7"; the loaders "ld-linux-x86-64.so.2",
// "ld-linux-aarch64.so.1", "ld-2.31.so", "ld-musl-x86_64.so.1", "ld64.so.2"
// (ppc64, s390x), "ld.so.1", "ld-elf.so.1" (FreeBSD).
// Only these shapes: "libc-client.so" (UW IMAP) and the like are not libc.
static int aether_is_libc_name(const char* base) {
    if (!base || !*base) return 0;
    if (strcmp(base, "libc.so") == 0 || strncmp(base, "libc.so.", 8) == 0) return 1;
    if (strncmp(base, "libc.musl-", 10) == 0) return 1;
    if (strncmp(base, "libc-", 5) == 0 && base[5] >= '0' && base[5] <= '9') return 1;
    if (strncmp(base, "ld-linux", 8) == 0 || strncmp(base, "ld-musl-", 8) == 0 ||
        strncmp(base, "ld-elf", 6) == 0) {
        return 1;
    }
    if (strncmp(base, "ld-", 3) == 0 && base[3] >= '0' && base[3] <= '9') return 1;
    if (strncmp(base, "ld64.so", 7) == 0 || strncmp(base, "ld.so", 5) == 0) return 1;
    return 0;
}

static const char* aether_basename(const char* p) {
    const char* slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static int aether_names_libc(const char* filename) {
    if (aether_is_libc_name(aether_basename(filename))) return 1;
    if (strchr(filename, '/')) {
        char resolved[PATH_MAX];
        if (aether_sandbox_canon_path(filename, resolved, sizeof(resolved)) &&
            aether_is_libc_name(aether_basename(resolved))) {
            return 1;
        }
    }
    return 0;
}

// The handles of the libc and loader this process runs on, found once by
// asking the loader which object holds a libc function (and, on Linux, the
// loader's base address), opened with RTLD_NOLOAD so nothing new is mapped.
// Each stays open: it pins nothing that is not loaded for good anyway.
static void* aether_libc_handle = NULL;
static void* aether_loader_handle = NULL;
static int aether_core_handles_found = 0;

static void aether_find_core_handles(void) {
    if (aether_core_handles_found) return;
    Dl_info info;
    if (dladdr((void*)strlen, &info) && info.dli_fname && info.dli_fname[0]) {
        aether_libc_handle = real_dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    }
#if defined(__linux__)
    unsigned long base = getauxval(AT_BASE);
    if (base && dladdr((void*)base, &info) && info.dli_fname && info.dli_fname[0]) {
        aether_loader_handle = real_dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    }
#endif
    aether_core_handles_found = 1;
}

// A refused dlopen's dlerror() string. dlopen's contract is that a NULL
// return leaves a message for dlerror(), and hosts rely on it: Tcl 8.6's
// `load` formats dlerror() with %s, so a refusal that left none (the loader
// had nothing to report, it was never asked) crashed the interpreter on a
// NULL, and Ruby's Fiddle reported a NULL handle with no reason. The
// message is per thread and is returned once, then cleared, as glibc's own
// is. dlopen calls that reach the loader clear it first, so a later
// loader error is never shadowed by an earlier refusal.
static char* (*real_dlerror)(void) = NULL;
static __thread char aether_dlerror_buf[PATH_MAX + 96];
static __thread int aether_dlerror_pending = 0;

static void aether_dlopen_refused(const char* filename) {
    if (!real_dlerror) real_dlerror = dlsym(RTLD_NEXT, "dlerror");
    // Drop any older loader message, so the next dlerror() is this one and
    // the one after it is NULL.
    if (real_dlerror) (void)real_dlerror();
    snprintf(aether_dlerror_buf, sizeof(aether_dlerror_buf),
             "%s: refused by the sandbox: dlopen of libc or the loader needs a native grant",
             filename);
    aether_dlerror_pending = 1;
    errno = EACCES;
}

char* dlerror(void) {
    if (aether_dlerror_pending) {
        aether_dlerror_pending = 0;
        return aether_dlerror_buf;
    }
    if (!real_dlerror) real_dlerror = dlsym(RTLD_NEXT, "dlerror");
    return real_dlerror ? real_dlerror() : NULL;
}

void* dlopen(const char* filename, int flags) {
    if (!real_dlopen) real_dlopen = dlsym(RTLD_NEXT, "dlopen");
    aether_dlerror_pending = 0;
    // NULL (the program itself) is not a load. Everything other than libc
    // and the loader (Python's .so modules, Ruby's extensions) loads: the
    // sandbox holds at the libc calls those make, which come through here.
    if (!filename || !sandbox_active) return real_dlopen(filename, flags);
    if (aether_names_libc(filename)) {
        if (!check_grant("native", filename)) {
            aether_dlopen_refused(filename);
            return NULL;
        }
        return real_dlopen(filename, flags);
    }
    void* h = real_dlopen(filename, flags);
    if (!h) return NULL;
    aether_find_core_handles();
    if ((h == aether_libc_handle || h == aether_loader_handle) &&
        !check_grant("native", filename)) {
        dlclose(h);
        aether_dlopen_refused(filename);
        return NULL;
    }
    return h;
}

#if defined(__GLIBC__)
// dlmopen loads into a link-map namespace. Into the base one it is dlopen.
// Into any other it brings a copy of the C library of its own, whose calls
// no wrapper here sees, so under a policy that needs the "native" grant for
// the file, as a dlopen of libc itself does.
static void* (*real_dlmopen)(Lmid_t, const char*, int) = NULL;
void* dlmopen(Lmid_t lmid, const char* filename, int flags) {
    if (!real_dlmopen) real_dlmopen = dlsym(RTLD_NEXT, "dlmopen");
    if (lmid == LM_ID_BASE) return dlopen(filename, flags);
    aether_dlerror_pending = 0;
    if (filename && policy_in_force() && !check_grant("native", filename)) {
        aether_dlopen_refused(filename);
        return NULL;
    }
    return real_dlmopen(lmid, filename, flags);
}
#endif

// posix_spawn_file_actions_addopen: the child opens the file between its
// fork and its exec, on libc's own open, which no wrapper sees. Under a
// policy the path is resolved and checked here, for what the flags ask, and
// the action is given the resolved path.
static int (*real_posix_spawn_file_actions_addopen)(posix_spawn_file_actions_t*, int, const char*,
                                                    int, mode_t) = NULL;
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* fa, int fd, const char* path,
                                     int oflag, mode_t mode) {
    if (!policy_in_force() || !path)
        return FS_REAL(posix_spawn_file_actions_addopen)(fa, fd, path, oflag, mode);
    int entry = (oflag & O_NOFOLLOW) || (oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, path, entry, fs_open_access(oflag), resolved)) return EACCES;
    return FS_REAL(posix_spawn_file_actions_addopen)(fa, fd, resolved, oflag, mode);
}

// syscall(): the raw escape hatch (Perl's syscall(), Python's ctypes
// syscall, a library that issues a call libc has no wrapper for).
//
// The interposer is there so syscall() cannot be used to go round this
// library: a raw call skips the argument checks the wrappers make, and most
// calls have no wrapper here at all (unlink, rename, mkdir, sendto, kill,
// ptrace, ... are not intercepted as libc calls). So:
//
//   - With nothing in force (no grant source, no in-process level on the
//     calling thread) every call is forwarded.
//
//   - With a policy in force it is default-deny. Forwarded are only the calls
//     on the allowlist below, each of which reaches no file, socket, program
//     or other process (each entry says why), and which the code a sandbox
//     runs makes through syscall() as a matter of course: GLib's futex
//     waits, gettid and sched_getattr/sched_setattr; the Aether scheduler's
//     sched_getaffinity and membarrier; FreeBSD libthr's getpid and
//     sigreturn (lib/libthr/thread/thr_rtld.c, thr_sig.c). Traced on an idle
//     GTK4 program, sae-driver and Aether actor programs (glibc 2.35 and
//     2.42), those are all the calls that arrive here.
//
//   - Calls that create a process (fork, vfork, a clone or clone3 that is
//     not a thread, FreeBSD's rfork with RFPROC and pdfork) get fork()'s
//     rule: the fork grant under a grant file. A thread passes. libthr makes
//     every threaded FreeBSD program's fork() and system() this way.
//   - mmap and mprotect / pkey_mprotect get mmap()'s and mprotect()'s rule:
//     executable memory is refused under a grant file, the rest forwarded.
//
//   - Anything else is refused (EPERM) and logged
//     (`AETHER_DENIED: native syscall:<name>`, or the number for a call this
//     table does not name), unless a native grant names it:
//     native:syscall:unlinkat, or native:* / grant_all for all of them.
//     A conditional allowlist entry whose condition fails (a kill of another
//     process) falls through to the same rule.
//
// The prototype differs by platform and must match the libc declaration
// exactly or the override is a compile error: glibc declares
// `long syscall(long, ...)`, FreeBSD and NetBSD declare
// `int syscall(int, ...)`. OpenBSD's libc no longer has syscall(), so the
// definition there is the only one, and it can forward nothing (ENOSYS).
#if defined(__FreeBSD__) || defined(__NetBSD__)
typedef int aether_syscall_arg_t;
#define AETHER_SYSCALL_NARGS 8     /* the most a BSD syscall takes */
#else
typedef long aether_syscall_arg_t;
#define AETHER_SYSCALL_NARGS 6     /* the most a Linux syscall takes */
#endif

#if !defined(__FreeBSD__)
#include <sys/syscall.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>            /* PR_SET_NAME, PR_GET_NAME */
#endif

/* Allowed with a policy in force: */
#define SC_FREE      1   /* reaches nothing outside this process's own state */
#define SC_SELF      2   /* the same, when the first argument names this process
                            or thread (0, our pid, our tid) */
#define SC_KILL_SELF 3   /* a signal to this process (pid or tgid argument) */
#define SC_TKILL_SELF 4  /* a signal to the calling thread */
#define SC_PRCTL_NAME 5  /* prctl of the thread's name only */
/* Held to a wrapper's rule: */
#define SC_PROCESS   6   /* a new process unless a thread: fork()'s rule */
#define SC_MMAP      7   /* anonymous PROT_EXEC: mmap()'s rule */
#define SC_MPROTECT  8   /* adds PROT_EXEC: mprotect()'s rule */
/* Named so the log and the grant read the same on every architecture; refused
 * under a policy without a native grant, as is every call not listed. */
#define SC_GRANT     9

#define SC(name, kind) { SYS_##name, "syscall:" #name, kind },
static const struct { long nr; const char* name; int kind; } aether_raw_calls[] = {
    /* Waiting and waking on this process's own memory (GLib's GCond, GMutex,
     * GThreadPool; libc's own locks). */
#ifdef SYS_futex
    SC(futex, SC_FREE)
#endif
#ifdef SYS_futex_time64
    SC(futex_time64, SC_FREE)
#endif
#ifdef SYS_futex_waitv
    SC(futex_waitv, SC_FREE)
#endif
#ifdef SYS_futex_wake
    SC(futex_wake, SC_FREE)
#endif
#ifdef SYS_futex_wait
    SC(futex_wait, SC_FREE)
#endif
#ifdef SYS_futex_requeue
    SC(futex_requeue, SC_FREE)
#endif
#ifdef SYS__umtx_op
    SC(_umtx_op, SC_FREE)               /* FreeBSD's futex */
#endif
#ifdef SYS_set_robust_list
    SC(set_robust_list, SC_FREE)        /* this thread's futex list */
#endif
#ifdef SYS_get_robust_list
    SC(get_robust_list, SC_SELF)
#endif
#ifdef SYS_rseq
    SC(rseq, SC_FREE)                   /* this thread's restartable sequences */
#endif
    /* Memory ordering across this process's own threads (the Aether
     * scheduler's membarrier). */
#ifdef SYS_membarrier
    SC(membarrier, SC_FREE)
#endif
    /* Scheduling of this process's own threads (the Aether scheduler's CPU
     * count; GLib's thread attributes). */
#ifdef SYS_sched_yield
    SC(sched_yield, SC_FREE)
#endif
#ifdef SYS_getcpu
    SC(getcpu, SC_FREE)
#endif
#ifdef SYS_sched_getaffinity
    SC(sched_getaffinity, SC_SELF)
#endif
#ifdef SYS_sched_setaffinity
    SC(sched_setaffinity, SC_SELF)
#endif
#ifdef SYS_sched_getattr
    SC(sched_getattr, SC_SELF)
#endif
#ifdef SYS_sched_setattr
    SC(sched_setattr, SC_SELF)
#endif
    /* Who this process is: no resource reached. */
#ifdef SYS_gettid
    SC(gettid, SC_FREE)
#endif
#ifdef SYS_getpid
    SC(getpid, SC_FREE)
#endif
#ifdef SYS_getppid
    SC(getppid, SC_FREE)
#endif
#ifdef SYS_getuid
    SC(getuid, SC_FREE)
#endif
#ifdef SYS_geteuid
    SC(geteuid, SC_FREE)
#endif
#ifdef SYS_getgid
    SC(getgid, SC_FREE)
#endif
#ifdef SYS_getegid
    SC(getegid, SC_FREE)
#endif
#ifdef SYS_getresuid
    SC(getresuid, SC_FREE)
#endif
#ifdef SYS_getresgid
    SC(getresgid, SC_FREE)
#endif
#ifdef SYS_thr_self
    SC(thr_self, SC_FREE)
#endif
    /* Randomness and time: no resource reached. */
#ifdef SYS_getrandom
    SC(getrandom, SC_FREE)
#endif
#ifdef SYS_clock_gettime
    SC(clock_gettime, SC_FREE)
#endif
#ifdef SYS_clock_gettime64
    SC(clock_gettime64, SC_FREE)
#endif
#ifdef SYS_clock_getres
    SC(clock_getres, SC_FREE)
#endif
#ifdef SYS_clock_nanosleep
    SC(clock_nanosleep, SC_FREE)
#endif
#ifdef SYS_nanosleep
    SC(nanosleep, SC_FREE)
#endif
#ifdef SYS_gettimeofday
    SC(gettimeofday, SC_FREE)
#endif
    /* This process's own mappings, no new executable memory: mremap keeps
     * a mapping's protection, madvise and munmap only drop or hint. */
#ifdef SYS_munmap
    SC(munmap, SC_FREE)
#endif
#ifdef SYS_mremap
    SC(mremap, SC_FREE)
#endif
#ifdef SYS_madvise
    SC(madvise, SC_FREE)
#endif
    /* I/O on a descriptor this process already holds. Holding it is the
     * authority: it was opened through the wrappers (or handed over by the
     * host that started this process), and reading or writing it reaches
     * nothing new. A socket's peer was checked at connect; sendto and
     * sendmsg, which name a peer of their own, are not here. */
#ifdef SYS_read
    SC(read, SC_FREE)
#endif
#ifdef SYS_write
    SC(write, SC_FREE)
#endif
#ifdef SYS_pread64
    SC(pread64, SC_FREE)
#endif
#ifdef SYS_pwrite64
    SC(pwrite64, SC_FREE)
#endif
#ifdef SYS_readv
    SC(readv, SC_FREE)
#endif
#ifdef SYS_writev
    SC(writev, SC_FREE)
#endif
#ifdef SYS_lseek
    SC(lseek, SC_FREE)
#endif
#ifdef SYS_close
    SC(close, SC_FREE)
#endif
#ifdef SYS_close_range
    SC(close_range, SC_FREE)
#endif
    /* Waiting on descriptors already held, and descriptors that name no
     * resource (an event counter, a timer, an epoll set). */
#ifdef SYS_epoll_create1
    SC(epoll_create1, SC_FREE)
#endif
#ifdef SYS_epoll_ctl
    SC(epoll_ctl, SC_FREE)
#endif
#ifdef SYS_epoll_wait
    SC(epoll_wait, SC_FREE)
#endif
#ifdef SYS_epoll_pwait
    SC(epoll_pwait, SC_FREE)
#endif
#ifdef SYS_epoll_pwait2
    SC(epoll_pwait2, SC_FREE)
#endif
#ifdef SYS_poll
    SC(poll, SC_FREE)
#endif
#ifdef SYS_ppoll
    SC(ppoll, SC_FREE)
#endif
#ifdef SYS_eventfd2
    SC(eventfd2, SC_FREE)
#endif
#ifdef SYS_timerfd_create
    SC(timerfd_create, SC_FREE)
#endif
#ifdef SYS_timerfd_settime
    SC(timerfd_settime, SC_FREE)
#endif
#ifdef SYS_timerfd_gettime
    SC(timerfd_gettime, SC_FREE)
#endif
    /* This thread's signal state, and signals to itself. */
#ifdef SYS_rt_sigaction
    SC(rt_sigaction, SC_FREE)
#endif
#ifdef SYS_rt_sigprocmask
    SC(rt_sigprocmask, SC_FREE)
#endif
#ifdef SYS_rt_sigreturn
    SC(rt_sigreturn, SC_FREE)
#endif
#ifdef SYS_rt_sigpending
    SC(rt_sigpending, SC_FREE)
#endif
#ifdef SYS_rt_sigtimedwait
    SC(rt_sigtimedwait, SC_FREE)
#endif
#ifdef SYS_rt_sigsuspend
    SC(rt_sigsuspend, SC_FREE)
#endif
#ifdef SYS_sigaltstack
    SC(sigaltstack, SC_FREE)
#endif
#ifdef SYS_sigaction
    SC(sigaction, SC_FREE)
#endif
#ifdef SYS_sigprocmask
    SC(sigprocmask, SC_FREE)
#endif
#ifdef SYS_sigreturn
    SC(sigreturn, SC_FREE)              /* libthr's signal return */
#endif
#ifdef SYS_tgkill
    SC(tgkill, SC_KILL_SELF)
#endif
#ifdef SYS_rt_tgsigqueueinfo
    SC(rt_tgsigqueueinfo, SC_KILL_SELF)
#endif
#ifdef SYS_kill
    SC(kill, SC_KILL_SELF)
#endif
#ifdef SYS_tkill
    SC(tkill, SC_TKILL_SELF)
#endif
    /* The thread's own name, and on x86_64 its own TLS registers. */
#ifdef SYS_prctl
    SC(prctl, SC_PRCTL_NAME)
#endif
#ifdef SYS_arch_prctl
    SC(arch_prctl, SC_FREE)
#endif
#ifdef SYS_exit
    SC(exit, SC_FREE)
#endif
#ifdef SYS_exit_group
    SC(exit_group, SC_FREE)
#endif
#ifdef SYS_restart_syscall
    SC(restart_syscall, SC_FREE)
#endif

    /* New processes: fork()'s rule. */
#ifdef SYS_fork
    SC(fork, SC_PROCESS)
#endif
#ifdef SYS_vfork
    SC(vfork, SC_PROCESS)
#endif
#ifdef SYS_clone
    SC(clone, SC_PROCESS)
#endif
#ifdef SYS_clone3
    SC(clone3, SC_PROCESS)
#endif
#ifdef SYS_rfork
    SC(rfork, SC_PROCESS)
#endif
#ifdef SYS_pdfork
    SC(pdfork, SC_PROCESS)
#endif
    /* Memory protection: mmap()'s and mprotect()'s rules. */
#ifdef SYS_mmap
    SC(mmap, SC_MMAP)
#endif
#ifdef SYS_mmap2
    SC(mmap2, SC_MMAP)
#endif
#ifdef SYS_mprotect
    SC(mprotect, SC_MPROTECT)
#endif
#ifdef SYS_pkey_mprotect
    SC(pkey_mprotect, SC_MPROTECT)
#endif

    /* Named only for the log and the grant: refused under a policy. Files and
     * sockets past the wrappers' checks, and the rest of the file system,
     * other processes, the kernel. */
#ifdef SYS_open
    SC(open, SC_GRANT)
#endif
#ifdef SYS_openat
    SC(openat, SC_GRANT)
#endif
#ifdef SYS_openat2
    SC(openat2, SC_GRANT)
#endif
#ifdef SYS_creat
    SC(creat, SC_GRANT)
#endif
#ifdef SYS_open_by_handle_at
    SC(open_by_handle_at, SC_GRANT)
#endif
#ifdef SYS_name_to_handle_at
    SC(name_to_handle_at, SC_GRANT)
#endif
#ifdef SYS_unlink
    SC(unlink, SC_GRANT)
#endif
#ifdef SYS_unlinkat
    SC(unlinkat, SC_GRANT)
#endif
#ifdef SYS_rename
    SC(rename, SC_GRANT)
#endif
#ifdef SYS_renameat
    SC(renameat, SC_GRANT)
#endif
#ifdef SYS_renameat2
    SC(renameat2, SC_GRANT)
#endif
#ifdef SYS_mkdir
    SC(mkdir, SC_GRANT)
#endif
#ifdef SYS_mkdirat
    SC(mkdirat, SC_GRANT)
#endif
#ifdef SYS_rmdir
    SC(rmdir, SC_GRANT)
#endif
#ifdef SYS_link
    SC(link, SC_GRANT)
#endif
#ifdef SYS_linkat
    SC(linkat, SC_GRANT)
#endif
#ifdef SYS_symlink
    SC(symlink, SC_GRANT)
#endif
#ifdef SYS_symlinkat
    SC(symlinkat, SC_GRANT)
#endif
#ifdef SYS_truncate
    SC(truncate, SC_GRANT)
#endif
#ifdef SYS_chmod
    SC(chmod, SC_GRANT)
#endif
#ifdef SYS_fchmodat
    SC(fchmodat, SC_GRANT)
#endif
#ifdef SYS_chown
    SC(chown, SC_GRANT)
#endif
#ifdef SYS_fchownat
    SC(fchownat, SC_GRANT)
#endif
#ifdef SYS_mount
    SC(mount, SC_GRANT)
#endif
#ifdef SYS_socket
    SC(socket, SC_GRANT)
#endif
#ifdef SYS_socketcall
    SC(socketcall, SC_GRANT)
#endif
#ifdef SYS_connect
    SC(connect, SC_GRANT)
#endif
#ifdef SYS_bind
    SC(bind, SC_GRANT)
#endif
#ifdef SYS_listen
    SC(listen, SC_GRANT)
#endif
#ifdef SYS_accept
    SC(accept, SC_GRANT)
#endif
#ifdef SYS_accept4
    SC(accept4, SC_GRANT)
#endif
#ifdef SYS_sendto
    SC(sendto, SC_GRANT)
#endif
#ifdef SYS_sendmsg
    SC(sendmsg, SC_GRANT)
#endif
#ifdef SYS_sendmmsg
    SC(sendmmsg, SC_GRANT)
#endif
#ifdef SYS_execve
    SC(execve, SC_GRANT)
#endif
#ifdef SYS_execveat
    SC(execveat, SC_GRANT)
#endif
#ifdef SYS_fexecve
    SC(fexecve, SC_GRANT)
#endif
#ifdef SYS_memfd_create
    SC(memfd_create, SC_GRANT)
#endif
#ifdef SYS_io_uring_setup
    SC(io_uring_setup, SC_GRANT)
#endif
#ifdef SYS_io_uring_enter
    SC(io_uring_enter, SC_GRANT)
#endif
#ifdef SYS_io_uring_register
    SC(io_uring_register, SC_GRANT)
#endif
#ifdef SYS_ptrace
    SC(ptrace, SC_GRANT)
#endif
#ifdef SYS_process_vm_readv
    SC(process_vm_readv, SC_GRANT)
#endif
#ifdef SYS_process_vm_writev
    SC(process_vm_writev, SC_GRANT)
#endif
#ifdef SYS_pidfd_open
    SC(pidfd_open, SC_GRANT)
#endif
#ifdef SYS_pidfd_send_signal
    SC(pidfd_send_signal, SC_GRANT)
#endif
#ifdef SYS_pidfd_getfd
    SC(pidfd_getfd, SC_GRANT)
#endif
#ifdef SYS_setns
    SC(setns, SC_GRANT)
#endif
#ifdef SYS_unshare
    SC(unshare, SC_GRANT)
#endif
#ifdef SYS_bpf
    SC(bpf, SC_GRANT)
#endif
#ifdef SYS_keyctl
    SC(keyctl, SC_GRANT)
#endif
#ifdef SYS_init_module
    SC(init_module, SC_GRANT)
#endif
#ifdef SYS_finit_module
    SC(finit_module, SC_GRANT)
#endif
};
#undef SC

static aether_syscall_arg_t (*real_syscall)(aether_syscall_arg_t, ...) = NULL;

static long aether_own_tid(void) {
#if defined(__linux__) && defined(SYS_gettid)
    if (real_syscall) return real_syscall(SYS_gettid);
#endif
    return -1;
}

// Is a process-creating raw call a thread? clone's flags are its first
// argument on every Linux ABI; clone3's are in the struct it points at;
// rfork makes a process only with RFPROC.
static int aether_raw_call_is_thread(long nr, const long a[]) {
#if defined(__linux__)
#ifdef SYS_clone
    if (nr == SYS_clone) return (a[0] & CLONE_THREAD) != 0;
#endif
#ifdef SYS_clone3
    if (nr == SYS_clone3) {
        const struct clone_args* ca = (const struct clone_args*)a[0];
        return ca && (ca->flags & CLONE_THREAD);
    }
#endif
#endif
#if defined(__FreeBSD__) && defined(SYS_rfork) && defined(RFPROC)
    if (nr == SYS_rfork) return (a[0] & RFPROC) == 0;
#endif
    (void)nr;
    (void)a;
    return 0;
}

// 1 when the raw call may go to the kernel, 0 when it is refused (logged,
// errno EPERM).
static int aether_raw_call_allowed(long nr, const long a[]) {
    if (!policy_in_force()) return 1;
    const char* name = NULL;
    int kind = 0;
    size_t n = sizeof(aether_raw_calls) / sizeof(aether_raw_calls[0]);
    for (size_t i = 0; i < n; i++) {
        if (aether_raw_calls[i].nr == nr) {
            name = aether_raw_calls[i].name;
            kind = aether_raw_calls[i].kind;
            break;
        }
    }
    long pid = (long)getpid();
    switch (kind) {
    case SC_FREE:
        return 1;
    case SC_SELF:
        if (a[0] == 0 || a[0] == pid || a[0] == aether_own_tid()) return 1;
        break;
    case SC_KILL_SELF:
        if (a[0] == pid) return 1;
        break;
    case SC_TKILL_SELF:
        if (a[0] == aether_own_tid()) return 1;
        break;
    case SC_PRCTL_NAME:
#if defined(__linux__)
        if (a[0] == PR_SET_NAME || a[0] == PR_GET_NAME) return 1;
#endif
        break;
    case SC_PROCESS:
        if (aether_raw_call_is_thread(nr, a) || new_process_allowed()) return 1;
        log_deny("native", name);
        return 0;
    case SC_MMAP:
        if (!sandbox_active || !(a[2] & PROT_EXEC) || (int)a[4] != -1) return 1;
        log_deny("native", name);
        return 0;
    case SC_MPROTECT:
        if (!sandbox_active || !(a[2] & PROT_EXEC)) return 1;
        log_deny("native", name);
        return 0;
    default:
        break;
    }
    // Not on the allowlist (or its condition failed): a native grant naming
    // it, by name where the table has one, else by number.
    char numbered[40];
    if (!name) {
        snprintf(numbered, sizeof numbered, "syscall:%ld", nr);
        name = numbered;
    }
    return check_grant("native", name);   // logs a refusal itself
}

aether_syscall_arg_t syscall(aether_syscall_arg_t number, ...) {
    // The arguments as the caller passed them, every one a full register or
    // stack slot wide (a long: a BSD libc declares the arguments int, but a
    // pointer argument is still passed whole), as libc's own syscall()
    // reads them; the call is made with the same count, so each reaches the
    // kernel where it was. Reading a slot the caller left unset reads
    // whatever is there, which libc's own syscall() also hands the kernel.
    long args[AETHER_SYSCALL_NARGS];
    va_list ap;
    va_start(ap, number);
    for (int i = 0; i < AETHER_SYSCALL_NARGS; i++) args[i] = va_arg(ap, long);
    va_end(ap);

    if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
    if (!aether_raw_call_allowed((long)number, args)) {
        errno = EPERM;
        return -1;
    }
    if (!real_syscall) {
        errno = ENOSYS;
        return -1;
    }
#if AETHER_SYSCALL_NARGS == 8
    return real_syscall(number, args[0], args[1], args[2], args[3], args[4], args[5],
                        args[6], args[7]);
#else
    return real_syscall(number, args[0], args[1], args[2], args[3], args[4], args[5]);
#endif
}

#if defined(__FreeBSD__)
// FreeBSD's own path calls (#3751). Each is checked as the calls above are:
// what it changes needs fs_write, what it reads or lists fs_read.

// The blocks forms of scandir (clang's blocks; this file is not built with
// -fblocks, so a block is taken by its ABI: a struct whose fourth member is
// the function that runs it, called with the block first). Listed as
// scandir is, its filter and comparison run through the block.
struct fs_block { void* isa; int flags; int reserved; void (*invoke)(void); };
static __thread void* fs_select_block;
static __thread void* fs_compar_block;
static int fs_select_tramp(const struct dirent* e) {
    struct fs_block* b = (struct fs_block*)fs_select_block;
    int (*run)(void*, const struct dirent*);
    memcpy(&run, &b->invoke, sizeof run);
    return run(b, e);
}
static int fs_compar_tramp(const struct dirent** a, const struct dirent** z) {
    struct fs_block* b = (struct fs_block*)fs_compar_block;
    int (*run)(void*, const struct dirent**, const struct dirent**);
    memcpy(&run, &b->invoke, sizeof run);
    return run(b, a, z);
}
static int fs_scandir_blocks(int dirfd, const char* path, struct dirent*** list, void* sel, void* cmp) {
    void* outer_sel = fs_select_block;
    void* outer_cmp = fs_compar_block;
    fs_select_block = sel;
    fs_compar_block = cmp;
    int r = fs_list_scandir(dirfd, path, list, sel ? fs_select_tramp : NULL, cmp ? fs_compar_tramp : NULL);
    fs_select_block = outer_sel;
    fs_compar_block = outer_cmp;
    return r;
}

static int (*real_scandir_b)(const char*, struct dirent***, void*, void*) = NULL;
int scandir_b(const char* path, struct dirent*** list, void* sel, void* cmp) {
    if (!policy_in_force()) return FS_REAL(scandir_b)(path, list, sel, cmp);
    return fs_scandir_blocks(AT_FDCWD, path, list, sel, cmp);
}

static int (*real_scandirat_b)(int, const char*, struct dirent***, void*, void*) = NULL;
int scandirat_b(int dirfd, const char* path, struct dirent*** list, void* sel, void* cmp) {
    if (!policy_in_force()) return FS_REAL(scandirat_b)(dirfd, path, list, sel, cmp);
    return fs_scandir_blocks(dirfd, path, list, sel, cmp);
}

// mkostempsat: mkostemps relative to a directory descriptor.
static int (*real_mkostempsat)(int, char*, int, int) = NULL;
int mkostempsat(int dirfd, char* tmpl, int suffix, int flags) {
    if (!policy_in_force()) return FS_REAL(mkostempsat)(dirfd, tmpl, suffix, flags);
    return fs_mkstemps_at(dirfd, tmpl, suffix, flags);
}

// File flags: chflagsat from the anchor, the name not followed (lchflags
// and chflagsat's AT_SYMLINK_NOFOLLOW act on a link itself; chflags acts on
// the resolution, which has no symlink at its end).
static int (*real_chflagsat)(int, const char*, unsigned long, int) = NULL;
static int fs_chflags_at(int dirfd, const char* path, unsigned long fl, int nofollow) {
    char resolved[PATH_MAX];
    if (!fs_changed_file(dirfd, path, nofollow, resolved)) return -1;
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &preload_ops);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(chflagsat)(d, name, fl, AT_SYMLINK_NOFOLLOW));
}

static int (*real_chflags)(const char*, unsigned long) = NULL;
int chflags(const char* path, unsigned long fl) {
    if (!policy_in_force()) return FS_REAL(chflags)(path, fl);
    return fs_chflags_at(AT_FDCWD, path, fl, 0);
}

static int (*real_lchflags)(const char*, unsigned long) = NULL;
int lchflags(const char* path, unsigned long fl) {
    if (!policy_in_force()) return FS_REAL(lchflags)(path, fl);
    return fs_chflags_at(AT_FDCWD, path, fl, 1);
}

int chflagsat(int dirfd, const char* path, unsigned long fl, int atflag) {
    if (!policy_in_force()) return FS_REAL(chflagsat)(dirfd, path, fl, atflag);
    return fs_chflags_at(dirfd, path, fl, (atflag & AT_SYMLINK_NOFOLLOW) != 0);
}

// funlinkat: unlinkat that removes the name only while it is still the
// file `fd` is open on. The name is checked and anchored as unlinkat's.
static int (*real_funlinkat)(int, const char*, int, int) = NULL;
int funlinkat(int dirfd, const char* path, int fd, int flags) {
    if (!policy_in_force()) return FS_REAL(funlinkat)(dirfd, path, fd, flags);
    char name[PATH_MAX];
    int d = fs_on_name(dirfd, path, name);
    if (d < 0) return -1;
    FS_AT_CALL(d, FS_REAL(funlinkat)(d, name, fd, flags));
}

// Calls with no form relative to a directory: checked on the resolution
// and made on it, the resolved path given in place of the one written (a
// swap between the check and the call is in the directories on the way
// only, the last name having been resolved).
#define FS_BY_NAME(RET, NAME, PARAMS, ARGS, PATHARG, NOFOLLOW, ACCESS, ARGS_RESOLVED) \
    static RET (*real_##NAME) PARAMS = NULL;                                    \
    RET NAME PARAMS {                                                           \
        if (!policy_in_force()) return FS_REAL(NAME) ARGS;                      \
        char resolved[PATH_MAX];                                                \
        if (!fs_allowed(AT_FDCWD, PATHARG, NOFOLLOW, ACCESS, resolved)) return (RET)-1; \
        return FS_REAL(NAME) ARGS_RESOLVED;                                     \
    }

FS_BY_NAME(int, revoke, (const char* path), (path), path, 0, AETHER_ACCESS_WRITE, (resolved))
FS_BY_NAME(int, undelete, (const char* path), (path), path, 1, AETHER_ACCESS_WRITE, (resolved))
FS_BY_NAME(int, unmount, (const char* path, int flags), (path, flags), path, 0, AETHER_ACCESS_WRITE,
           (resolved, flags))
FS_BY_NAME(long, extattr_set_file,
           (const char* path, int ns, const char* name, const void* data, size_t n),
           (path, ns, name, data, n), path, 0, AETHER_ACCESS_WRITE, (resolved, ns, name, data, n))
FS_BY_NAME(long, extattr_set_link,
           (const char* path, int ns, const char* name, const void* data, size_t n),
           (path, ns, name, data, n), path, 1, AETHER_ACCESS_WRITE, (resolved, ns, name, data, n))
FS_BY_NAME(int, extattr_delete_file, (const char* path, int ns, const char* name), (path, ns, name),
           path, 0, AETHER_ACCESS_WRITE, (resolved, ns, name))
FS_BY_NAME(int, extattr_delete_link, (const char* path, int ns, const char* name), (path, ns, name),
           path, 1, AETHER_ACCESS_WRITE, (resolved, ns, name))
FS_BY_NAME(int, acl_set_file, (const char* path, int type, void* acl), (path, type, acl), path, 0,
           AETHER_ACCESS_WRITE, (resolved, type, acl))
FS_BY_NAME(int, acl_set_link_np, (const char* path, int type, void* acl), (path, type, acl), path,
           1, AETHER_ACCESS_WRITE, (resolved, type, acl))
FS_BY_NAME(int, acl_delete_def_file, (const char* path), (path), path, 0, AETHER_ACCESS_WRITE,
           (resolved))
FS_BY_NAME(int, acl_delete_def_link_np, (const char* path), (path), path, 1, AETHER_ACCESS_WRITE,
           (resolved))
FS_BY_NAME(int, acl_delete_file_np, (const char* path, int type), (path, type), path, 0,
           AETHER_ACCESS_WRITE, (resolved, type))
FS_BY_NAME(int, acl_delete_link_np, (const char* path, int type), (path, type), path, 1,
           AETHER_ACCESS_WRITE, (resolved, type))
FS_BY_NAME(int, __acl_set_file, (const char* path, int type, void* acl), (path, type, acl), path, 0,
           AETHER_ACCESS_WRITE, (resolved, type, acl))
FS_BY_NAME(int, __acl_set_link, (const char* path, int type, void* acl), (path, type, acl), path, 1,
           AETHER_ACCESS_WRITE, (resolved, type, acl))
FS_BY_NAME(int, __acl_delete_file, (const char* path, int type), (path, type), path, 0,
           AETHER_ACCESS_WRITE, (resolved, type))
FS_BY_NAME(int, __acl_delete_link, (const char* path, int type), (path, type), path, 1,
           AETHER_ACCESS_WRITE, (resolved, type))
FS_BY_NAME(int, extattrctl, (const char* path, int cmd, const char* file, int ns, const char* name),
           (path, cmd, file, ns, name), path, 0, AETHER_ACCESS_WRITE, (resolved, cmd, file, ns, name))

// exect: execve with tracing on, checked as execve is.
static int (*real_exect)(const char*, char* const[], char* const[]) = NULL;
int exect(const char* path, char* const argv[], char* const envp[]) {
    if (!real_exect) real_exect = dlsym(RTLD_NEXT, "exect");
    if (policy_in_force()) {
        if (!path || !check_grant("exec", path)) {
            errno = EACCES;
            return -1;
        }
        if (sandbox_active) {
            char* merged_env[MAX_EXEC_ENV_ENTRIES + 1];
            if (!child_env(envp, merged_env)) return -1;
            return real_exect(path, argv, merged_env);
        }
    }
    return real_exect(path, argv, envp);
}

// nmount: the mount point is the iovec pair named "fspath", fs_write on
// it. A nullfs (or unionfs) mount shows the directory named "target" (or
// "from") at the mount point, so that is read through it: fs_read on it,
// and fs_write unless the mount is read-only (an "ro" or "rdonly" option, or
// MNT_RDONLY). The call is made on a copy of the vector with each of those
// paths as it was resolved.
#ifndef MNT_RDONLY
#define MNT_RDONLY 0x00000001   /* <sys/mount.h> */
#endif
struct iovec;
static int (*real_nmount)(struct iovec*, unsigned int, int) = NULL;
static int nmount_opt_is(const void* base, size_t len, const char* name) {
    size_t nl = strlen(name) + 1;
    return base && len == nl && memcmp(base, name, nl) == 0;
}
int nmount(struct iovec* iov, unsigned int n, int flags) {
    if (!policy_in_force()) return FS_REAL(nmount)(iov, n, flags);
    struct ae_iov { void* base; size_t len; };
    const struct ae_iov* v = (const void*)iov;
    if (n > 128 || (n & 1)) { errno = EINVAL; return -1; }
    int fspath_at = -1, layered = 0, ro = (flags & MNT_RDONLY) != 0;
    for (unsigned i = 0; i + 1 < n; i += 2) {
        if (nmount_opt_is(v[i].base, v[i].len, "fspath")) fspath_at = (int)i + 1;
        if (nmount_opt_is(v[i].base, v[i].len, "fstype") &&
            (nmount_opt_is(v[i + 1].base, v[i + 1].len, "nullfs") ||
             nmount_opt_is(v[i + 1].base, v[i + 1].len, "unionfs")))
            layered = 1;
        if (nmount_opt_is(v[i].base, v[i].len, "ro") || nmount_opt_is(v[i].base, v[i].len, "rdonly"))
            ro = 1;
    }
    if (fspath_at < 0 || !v[fspath_at].base) { errno = EACCES; return -1; }
    struct ae_iov copy[128];
    memcpy(copy, v, n * sizeof copy[0]);
    char paths[3][PATH_MAX];
    int used = 0;
    if (!fs_allowed(AT_FDCWD, (const char*)v[fspath_at].base, 0, AETHER_ACCESS_WRITE, paths[used])) return -1;
    copy[fspath_at].base = paths[used];
    copy[fspath_at].len = strlen(paths[used]) + 1;
    used++;
    for (unsigned i = 0; layered && i + 1 < n && used < 3; i += 2) {
        if (!nmount_opt_is(v[i].base, v[i].len, "target") && !nmount_opt_is(v[i].base, v[i].len, "from"))
            continue;
        if (!v[i + 1].base) continue;
        int access = AETHER_ACCESS_READ | (ro ? 0 : AETHER_ACCESS_WRITE);
        if (!fs_allowed(AT_FDCWD, (const char*)v[i + 1].base, 0, access, paths[used])) return -1;
        copy[i + 1].base = paths[used];
        copy[i + 1].len = strlen(paths[used]) + 1;
        used++;
    }
    return FS_REAL(nmount)((struct iovec*)(void*)copy, n, flags);
}

// mount(2) on FreeBSD: (type, dir, flags, data).
static int (*real_mount)(const char*, const char*, int, void*) = NULL;
// A layered file system (nullfs, unionfs) takes its source through nmount
// only, where it is checked above; mount(2) is refused for one under a
// policy rather than mount a source it cannot see.
int mount(const char* type, const char* dir, int flags, void* data) {
    if (!policy_in_force()) return FS_REAL(mount)(type, dir, flags, data);
    if (type && (strcmp(type, "nullfs") == 0 || strcmp(type, "unionfs") == 0)) {
        log_deny("fs_read", type);
        errno = EACCES;
        return -1;
    }
    char resolved[PATH_MAX];
    if (!fs_allowed(AT_FDCWD, dir, 0, AETHER_ACCESS_WRITE, resolved)) return -1;
    return FS_REAL(mount)(type, resolved, flags, data);
}

// _open and _openat: libc's own names for open and openat, exported, which
// a program can call by name.
int _open(const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(openat)(AT_FDCWD, path, flags, mode);
    return fs_open_checked(AT_FDCWD, path, flags, mode);
}

int _openat(int dirfd, const char* path, int flags, ...) {
    FS_MODE_ARG(flags)
    if (!policy_in_force()) return FS_REAL(openat)(dirfd, path, flags, mode);
    return fs_open_checked(dirfd, path, flags, mode);
}
#endif // __FreeBSD__
