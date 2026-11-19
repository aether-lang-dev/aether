// Opens a file the way a case of test_sandbox_preload_landlock.sh asks,
// under the LD_PRELOAD library, and reports the outcome in the exit status:
// 0 opened, 3 refused with EACCES, 4 any other failure, 7 no raw syscall
// on this architecture, 2 a usage error.
//
//   probe <raw|libc> <file>
//   probe exec <program> [args...]
//
// `exec` replaces the probe with <program> (execv): its exit status is the
// program's, or 3 when the exec was refused (EACCES, EPERM), 4 otherwise.
//
// `raw` issues openat as a bare syscall instruction, which no libc wrapper
// sees: only the kernel (Landlock) can refuse it. `libc` calls open().
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long raw_openat(int dirfd, const char* path, long flags) {
#if defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(257L), "D"((long)dirfd), "S"(path), "d"(flags), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
#elif defined(__aarch64__)
    register long x8 __asm__("x8") = 56;
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
#else
    (void)dirfd;
    (void)path;
    (void)flags;
    return -100000;
#endif
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "exec") == 0) {
        execv(argv[2], argv + 2);
        return (errno == EACCES || errno == EPERM) ? 3 : 4;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s <raw|libc> <file>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "raw") == 0) {
        long r = raw_openat(AT_FDCWD, argv[2], O_RDONLY | O_CLOEXEC);
        if (r == -100000) return 7;
        if (r >= 0) {
            close((int)r);
            return 0;
        }
        return r == -EACCES ? 3 : 4;
    }
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == EACCES ? 3 : 4;
}
