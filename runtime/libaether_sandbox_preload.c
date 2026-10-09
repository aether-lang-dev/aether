// libaether_sandbox_preload.c — LD_PRELOAD library for cross-process containment
//
// Intercepts libc calls (open, connect, execve, getenv) and checks
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// Grant storage
#define MAX_GRANTS 256
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

// Check the in-process Aether sandbox checker first (embedded mode),
// then fall back to file-based grants (LD_PRELOAD mode)
typedef int (*aether_sandbox_check_fn)(const char*, const char*);
extern aether_sandbox_check_fn _aether_sandbox_checker __attribute__((weak));

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
            if ((&_aether_sandbox_checker && _aether_sandbox_checker) || sandbox_active) {
                log_deny(category, resource);
                return 0;
            }
            return 1;
        }
        match_target = resolved;
    }

    // In-process checker takes priority (embedded Python mode)
    if (&_aether_sandbox_checker && _aether_sandbox_checker) {
        int result = _aether_sandbox_checker(category, match_target);
        if (!result) log_deny(category, match_target);
        return result;
    }
    // Fall back to file-based grants (LD_PRELOAD mode)
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
        if (i == len || data[i] == '\n' || data[i] == '\0') {
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
            if (data[i] == '\0') break;
        } else if (lpos < 319) {
            line[lpos++] = data[i];
        }
    }
}

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

    // Log mode: file (default), stderr, or none
    const char* log_env = real_getenv("AETHER_SANDBOX_LOG");
    if (log_env) {
        if (strcmp(log_env, "stderr") == 0) log_mode = LOG_STDERR;
        else if (strcmp(log_env, "none") == 0) log_mode = LOG_NONE;
        else log_mode = LOG_FILE;
    }
    // Legacy: AETHER_SANDBOX_VERBOSE=1 sets stderr mode
    if (real_getenv("AETHER_SANDBOX_VERBOSE")) log_mode = LOG_STDERR;

    // Try shared memory first (set by aether_spawn_sandboxed)
    const char* shm_name = real_getenv("AETHER_SANDBOX_SHM");
    if (shm_name) {
        int fd = shm_open(shm_name, O_RDONLY, 0);
        if (fd >= 0) {
            struct stat st;
            fstat(fd, &st);
            char* data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
            if (data != MAP_FAILED) {
                parse_grants(data, st.st_size);
                munmap(data, st.st_size);
                sandbox_active = 1;
            }
            close(fd);
        }
    }

    // Fall back to grant file (manual LD_PRELOAD usage)
    if (!sandbox_active) {
        const char* grant_file = real_getenv("AETHER_SANDBOX_GRANTS");
        if (!grant_file) return;
        FILE* f = real_fopen(grant_file, "r");
        if (!f) return;
        // Read entire file
        char buf[8192];
        int n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        parse_grants(buf, n);
        sandbox_active = 1;
    }

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

// open
int open(const char* path, int flags, ...) {
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (path) {
        const char* cat = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_APPEND)) ? "fs_write" : "fs_read";
        if (!check_grant(cat, path)) {
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
        const char* cat = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_APPEND)) ? "fs_write" : "fs_read";
        if (!check_grant(cat, path)) {
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
        const char* cat = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_APPEND)) ? "fs_write" : "fs_read";
        if (!check_grant(cat, path)) { errno = EACCES; return -1; }
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
        const char* cat = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_APPEND)) ? "fs_write" : "fs_read";
        if (!check_grant(cat, path)) {
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

// execve
int execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (!real_execve) real_execve = dlsym(RTLD_NEXT, "execve");
    if (pathname && !check_grant("exec", pathname)) {
        errno = EPERM;
        return -1;
    }
    return real_execve(pathname, argv, envp);
}

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
    if (sandbox_active && !check_grant("fork", "*")) {
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

// dlopen — block loading of native libraries unless granted
// This prevents Python ctypes, Ruby Fiddle, Perl DynaLoader,
// Java Panama/JNI from loading arbitrary .so files that bypass the sandbox.
void* dlopen(const char* filename, int flags) {
    if (!real_dlopen) real_dlopen = dlsym(RTLD_NEXT, "dlopen");
    if (filename && sandbox_active) {
        // Allow NULL (self), system libraries, and runtime paths
        // Block explicit loading of libc (ctypes/Fiddle escape vector)
        int is_escape_target = 0;
        if (strcmp(filename, "libc.so.6") == 0 || strcmp(filename, "libc.so") == 0) {
            is_escape_target = 1;
        }
        if (is_escape_target && !check_grant("native", filename)) {
            log_deny("native", filename);
            errno = EACCES;
            return NULL;
        }
        // Everything else (Python .so modules, Ruby .so extensions, etc.)
        // is allowed — the sandbox enforces at the libc call level, not
        // at the module loading level. The loaded module's calls still
        // go through our intercepted open/connect/getenv.
    }
    return real_dlopen(filename, flags);
}

// syscall — block direct syscall() which bypasses all libc interception
// This is the nuclear escape hatch: Perl's syscall(), Python's os.syscall()
//
// The prototype differs by platform and must match the libc declaration
// exactly or the override is a compile error: glibc declares
// `long syscall(long, ...)`, FreeBSD declares `int syscall(int, ...)`.
#if defined(__FreeBSD__)
typedef int aether_syscall_arg_t;
#else
typedef long aether_syscall_arg_t;
#endif
static aether_syscall_arg_t (*real_syscall)(aether_syscall_arg_t, ...) = NULL;
aether_syscall_arg_t syscall(aether_syscall_arg_t number, ...) {
    if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
    if (sandbox_active) {
#if defined(__linux__)
        // Block dangerous syscalls: open(2), openat(257), connect(42),
        // execve(59), socket(41). Numbers are the Linux x86-64 ABI;
        // FreeBSD's syscall table differs, so this named-deny branch is
        // Linux-only. The catch-all block below still denies every raw
        // syscall() when sandboxed, on every platform.
        if (number == 2 || number == 257 || number == 42 ||
            number == 59 || number == 41) {
            char num_str[32];
            snprintf(num_str, sizeof(num_str), "syscall_%ld", (long)number);
            log_deny("native", num_str);
            errno = EPERM;
            return -1;
        }
#endif // __linux__
    }
    // For allowed syscalls, we can't easily forward varargs.
    // Block all when sandboxed as a safety measure.
    if (sandbox_active) {
        errno = EPERM;
        return -1;
    }
    // Not sandboxed — we can't forward varargs generically from here
    // either. Policy: raw syscall() always returns -1/ENOSYS through
    // this interception point. Every syscall must go through a libc
    // wrapper (open, connect, execve, getenv, ...) that this preload
    // library can see. Bypassing libc with syscall(SYS_xxx) is out of
    // scope for cooperative containment.
    errno = ENOSYS;
    return -1;
}
