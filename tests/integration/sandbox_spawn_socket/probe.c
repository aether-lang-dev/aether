/* sandbox.spawn_socket's test child. It writes one line per probe to the
 * socket named by AETHER_IPC_FD, then exits 0. The raw probes issue the
 * syscall instruction themselves, past libc and so past LD_PRELOAD: only the
 * kernel layer can refuse them.
 *
 *   probe hello <file>        channel, argv, env, libc fopen of /etc/passwd and <file>
 *   probe raw_openat <path>   openat by a raw syscall
 *   probe raw_passwd          raw_openat /etc/passwd (for spawn_sandboxed, which passes one arg)
 *   probe raw_socket <family> socket(AF_INET|AF_INET6|AF_UNIX, SOCK_STREAM) by a raw syscall
 *   probe raw_execve          execve of this program ("execed") by a raw syscall
 *   probe execed              what raw_execve runs, when it is let through
 *   probe raw_ptrace          ptrace(PTRACE_TRACEME) by a raw syscall
 *   probe raw_iouring         io_uring_setup by a raw syscall
 *   probe raw_unshare         unshare(CLONE_NEWUSER) by a raw syscall
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ch = -1;

static void say(const char* fmt, const char* a, const char* b) {
    char line[1024];
    int n = snprintf(line, sizeof line, fmt, a, b);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    if (ch >= 0) {
        ssize_t w = write(ch, line, (size_t)n);
        (void)w;
    } else {
        fputs(line, stdout);
    }
}

#if defined(__linux__) && defined(__x86_64__)
#define NR_OPENAT 257
#define NR_SOCKET 41
#define NR_EXECVE 59
#define NR_PTRACE 101
#define NR_IOURING 425
#define NR_UNSHARE 272
static long raw3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}
static long raw4(long nr, long a, long b, long c, long d) {
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
#define HAVE_RAW 1
#elif defined(__linux__) && defined(__aarch64__)
#define NR_OPENAT 56
#define NR_SOCKET 198
#define NR_EXECVE 221
#define NR_PTRACE 117
#define NR_IOURING 425
#define NR_UNSHARE 97
static long raw4(long nr, long a, long b, long c, long d) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
static long raw3(long nr, long a, long b, long c) { return raw4(nr, a, b, c, 0); }
#define HAVE_RAW 1
#endif

static const char* result(long r, char* buf, size_t n) {
    if (r >= 0) return "ALLOWED";
    snprintf(buf, n, "refused (%s)", strerror((int)-r));
    return buf;
}

int main(int argc, char** argv) {
    const char* fdv = getenv("AETHER_IPC_FD");
    if (fdv) ch = atoi(fdv);
    const char* mode = argc > 1 ? argv[1] : "hello";
    char buf[256];

    if (strcmp(mode, "hello") == 0) {
        const char* kind = getenv("AETHER_IPC_KIND");
        const char* t = getenv("SPAWN_TEST");
        say("channel fd=%s kind=%s\n", fdv ? fdv : "none", kind ? kind : "none");
        snprintf(buf, sizeof buf, "%d", argc);
        say("argc=%s argv1=%s\n", buf, argc > 1 ? argv[1] : "");
        say("env SPAWN_TEST=%s%s\n", t ? t : "(unset)", "");
        FILE* f = fopen("/etc/passwd", "r");
        say("libc fopen /etc/passwd: %s%s\n", f ? "ALLOWED" : "refused", "");
        if (f) fclose(f);
        if (argc > 2) {
            f = fopen(argv[2], "r");
            char line[128] = "";
            if (f && fgets(line, sizeof line, f)) line[strcspn(line, "\n")] = '\0';
            say("libc fopen fixture: %s %s\n", f ? "ALLOWED" : "refused", line);
            if (f) fclose(f);
        }
        return 0;
    }
#ifdef HAVE_RAW
    if (strcmp(mode, "raw_passwd") == 0) {
        argv[2] = (char*)"/etc/passwd";
        mode = "raw_openat";
        argc = 3;
    }
    if (strcmp(mode, "raw_openat") == 0 && argc > 2) {
        long r = raw4(NR_OPENAT, -100 /* AT_FDCWD */, (long)argv[2], O_RDONLY, 0);
        if (r >= 0) close((int)r);
        say("raw openat %s: %s\n", argv[2], result(r, buf, sizeof buf));
        return 0;
    }
    if (strcmp(mode, "raw_socket") == 0 && argc > 2) {
        long fam = strcmp(argv[2], "AF_INET6") == 0 ? 10 : strcmp(argv[2], "AF_UNIX") == 0 ? 1 : 2;
        long r = raw3(NR_SOCKET, fam, 1 /* SOCK_STREAM */, 0);
        if (r >= 0) close((int)r);
        say("raw socket %s: %s\n", argv[2], result(r, buf, sizeof buf));
        return 0;
    }
    if (strcmp(mode, "raw_execve") == 0) {
        char self[1024];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) { say("raw execve: cannot find myself%s%s\n", "", ""); return 0; }
        self[n] = '\0';
        char* nargv[] = { self, (char*)"execed", NULL };
        extern char** environ;
        long r = raw3(NR_EXECVE, (long)self, (long)nargv, (long)environ);
        say("raw execve: %s%s\n", result(r, buf, sizeof buf), "");
        return 0;
    }
    if (strcmp(mode, "raw_ptrace") == 0) {
        long r = raw4(NR_PTRACE, 0 /* PTRACE_TRACEME */, 0, 0, 0);
        say("raw ptrace: %s%s\n", result(r, buf, sizeof buf), "");
        return 0;
    }
    if (strcmp(mode, "raw_iouring") == 0) {
        char params[120];
        memset(params, 0, sizeof params);
        long r = raw3(NR_IOURING, 4, (long)params, 0);
        if (r >= 0) close((int)r);
        say("raw io_uring_setup: %s%s\n", result(r, buf, sizeof buf), "");
        return 0;
    }
    if (strcmp(mode, "raw_unshare") == 0) {
        long r = raw3(NR_UNSHARE, 0x10000000 /* CLONE_NEWUSER */, 0, 0);
        say("raw unshare: %s%s\n", result(r, buf, sizeof buf), "");
        return 0;
    }
#endif
    if (strcmp(mode, "execed") == 0) {
        say("execed: ALLOWED (a new program ran)%s%s\n", "", "");
        return 0;
    }
    say("probe: unknown or unsupported mode %s%s\n", mode, "");
    return 3;
}
