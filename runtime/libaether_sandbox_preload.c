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

static int check_grant(const char* category, const char* resource) {
    // For fs_* categories, resolve the path BEFORE matching so a grant
    // for `/tmp/*` can't be subverted with `/tmp/../etc/shadow` or a
    // symlink under /tmp. tcp/env/exec resources stay as-is — realpath
    // is meaningful only for filesystem paths.
    char resolved[PATH_MAX];
    const char* match_target = resource;
    if (category && strncmp(category, "fs_", 3) == 0 && resource) {
        if (!aether_sandbox_canon_path(resource, resolved, sizeof(resolved))) {
            int in_process = aether_sandbox_levels_here ? aether_sandbox_levels_here() : 0;
            if (in_process > 0 || sandbox_active) {
                log_deny(category, resource);
                return 0;
            }
            return 1;
        }
        match_target = resolved;
    }

    // An embedded policy and preload grants are independent boundaries.
    // When both are active, the operation must satisfy both policies.
    if (aether_sandbox_check_resolved) {
        int result = aether_sandbox_check_resolved(category, match_target);
        if (result == 0) {
            log_deny(category, match_target);
            return 0;
        }
    }
    // Apply file/shared-memory grants as the second policy when active.
    if (!sandbox_active) return 1;
    for (int i = 0; i < grant_count; i++) {
        if (grants[i].cat[0] == '*' && grants[i].pat[0] == '*') return 1;
        if (strcmp(grants[i].cat, category) == 0) {
            if (aether_grant_match(category, grants[i].pat, match_target)) return 1;
        }
    }
    log_deny(category, match_target);
    return 0;
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
static void __attribute__((constructor)) sandbox_init(void) {
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
    sandbox_active = source_requested;

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
static const char* staged_write_target(const char* path, int flags) {
    if ((flags & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL)) return path;
    const char* target = aether_sandbox_staged_for ? aether_sandbox_staged_for(path) : NULL;
    if (!target) return path;
    size_t n = strlen(target);
    if (strncmp(path, target, n) != 0 || strncmp(path + n, ".tmp.", 5) != 0) return path;
    const char* p = path + n + 5;
    char* end = NULL;
    long pid = strtol(p, &end, 10);
    if (end == p || *end != '.' || pid != (long)getpid()) return path;
    p = end + 1;
    if (*p < '0' || *p > '9') return path;
    while (*p >= '0' && *p <= '9') p++;
    return *p ? path : target;
}

// The category and resource an open of `path` with `flags` is checked as.
static int open_allowed(const char* path, int flags) {
    if (!(flags & (O_WRONLY | O_RDWR | O_CREAT | O_APPEND))) return check_grant("fs_read", path);
    return check_grant("fs_write", staged_write_target(path, flags));
}

// open
int open(const char* path, int flags, ...) {
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (path) {
        if (!open_allowed(path, flags)) {
            errno = EACCES;
            return -1;
        }
    }
    // Handle variadic mode argument
    if (flags & O_CREAT) {
        va_list ap;
        __builtin_va_start(ap, flags);
        int mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}

// fopen
FILE* fopen(const char* path, const char* mode) {
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    if (path && mode) {
        const char* cat = (mode[0] == 'r') ? "fs_read" : "fs_write";
        if (!check_grant(cat, path)) {
            errno = EACCES;
            return NULL;
        }
    }
    return real_fopen(path, mode);
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

// bind — block by default, grant with "tcp_listen" category
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

// listen — same restriction as bind
static int (*real_listen)(int, int) = NULL;
int listen(int sockfd, int backlog) {
    if (!real_listen) real_listen = dlsym(RTLD_NEXT, "listen");
    if (sandbox_active && !check_grant("tcp_listen", "*")) {
        errno = EACCES;
        return -1;
    }
    return real_listen(sockfd, backlog);
}

// accept / accept4 — block inbound connections unless tcp_listen granted
static int (*real_accept)(int, struct sockaddr*, socklen_t*) = NULL;
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!real_accept) real_accept = dlsym(RTLD_NEXT, "accept");
    if (sandbox_active && !check_grant("tcp_listen", "*")) {
        errno = EACCES;
        return -1;
    }
    return real_accept(sockfd, addr, addrlen);
}

// openat (modern Linux uses this instead of open)
static int (*real_openat)(int, const char*, int, ...) = NULL;
int openat(int dirfd, const char* path, int flags, ...) {
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    if (path) {
        if (!open_allowed(path, flags)) {
            errno = EACCES;
            return -1;
        }
    }
    if (flags & O_CREAT) {
        va_list ap;
        __builtin_va_start(ap, flags);
        int mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return real_openat(dirfd, path, flags, mode);
    }
    return real_openat(dirfd, path, flags);
}

// open64 / openat64 / fopen64 — glibc large-file variants. These symbols
// exist only on glibc/Linux; FreeBSD's off_t is 64-bit natively and has
// no *64 suffixed entry points, so the whole block is Linux-gated.
#if defined(__linux__)

// open64 — glibc large file variant (skip if glibc already redirects open→open64)
#ifndef open64
int open64(const char* path, int flags, ...) {
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open64");
    if (path) {
        if (!open_allowed(path, flags)) { errno = EACCES; return -1; }
    }
    if (flags & O_CREAT) {
        va_list ap; __builtin_va_start(ap, flags);
        int mode = __builtin_va_arg(ap, int); __builtin_va_end(ap);
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}
#endif

#ifndef openat64
int openat64(int dirfd, const char* path, int flags, ...) {
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat64");
    if (path) {
        if (!open_allowed(path, flags)) {
            errno = EACCES;
            return -1;
        }
    }
    if (flags & O_CREAT) {
        va_list ap;
        __builtin_va_start(ap, flags);
        int mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return real_openat(dirfd, path, flags, mode);
    }
    return real_openat(dirfd, path, flags);
}
#endif

// fopen64 — glibc large file variant
#ifndef fopen64
FILE* fopen64(const char* path, const char* mode) {
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen64");
    if (path && mode) {
        const char* cat = (mode[0] == 'r') ? "fs_read" : "fs_write";
        if (!check_grant(cat, path)) { errno = EACCES; return NULL; }
    }
    return real_fopen(path, mode);
}
#endif

#endif // __linux__ (open64 / openat64 / fopen64)

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
#define SANDBOX_SHELL "/bin/sh"

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
// fork and execve it amounts to would. The shell inherits the live
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
void* mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap64");
    if (sandbox_active && (prot & 0x4) && fd == -1) {
        log_deny("native", "mmap_exec_anonymous");
        errno = EACCES;
        return (void*)-1;
    }
    return real_mmap(addr, length, prot, flags, fd, offset);
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
