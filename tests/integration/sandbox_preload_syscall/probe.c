// The preload's syscall() interposer: what it forwards and what it polices.
//
//   probe <none|deny> <file>
//
// `none` runs with no in-process level; `deny` stands in for an Aether
// enforce block that grants nothing (the preload asks the two functions
// below, as it asks the runtime's in an Aether program). <file> is two
// pages, the second filled with 'B'; it is opened, and the scratch files
// the unlink and rename checks remove are made beside it, before the level
// is set, so those checks do not depend on the fs grants.
//
// Each line is "<check> ok", or "<check> <ERRNO>" for a failure.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int level = -1;   // -1 no level on this thread, 0 a level refusing all

int aether_sandbox_check_resolved(const char* category, const char* resource) {
    (void)category;
    (void)resource;
    return level;
}

int aether_sandbox_levels_here(void) {
    return level >= 0 ? 1 : 0;
}

static const char* err_name(int e) {
    switch (e) {
    case EPERM: return "EPERM";
    case ENOSYS: return "ENOSYS";
    case EACCES: return "EACCES";
    case EAGAIN: return "EAGAIN";
    case EINVAL: return "EINVAL";
    case ETIMEDOUT: return "ETIMEDOUT";
    default: return "EOTHER";
    }
}

static void line(const char* check, int ok, int e) {
    if (ok) printf("%s ok\n", check);
    else printf("%s %s\n", check, err_name(e));
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <none|deny> <file>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }
    // Scratch files beside <file>, made before any level is in force.
    char a1[4096], a2[4096], b1[4096], b2[4096];
    snprintf(a1, sizeof a1, "%s.unlink-raw", argv[2]);
    snprintf(a2, sizeof a2, "%s.unlink-libc", argv[2]);
    snprintf(b1, sizeof b1, "%s.rename-from", argv[2]);
    snprintf(b2, sizeof b2, "%s.rename-to", argv[2]);
    const char* scratch[] = { a1, a2, b1 };
    for (int i = 0; i < 3; i++) {
        int s = open(scratch[i], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (s < 0) {
            perror(scratch[i]);
            return 2;
        }
        close(s);
    }
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (strcmp(argv[1], "deny") == 0) level = 0;

    // A futex wait on a word that holds the expected value sleeps until
    // its timeout: GLib's g_cond_wait and g_mutex_lock, through syscall().
    // A wait that returns at once is the spin.
    int word = 0;
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    double t0 = now();
    long rc = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
    int e = errno;
    double slept = now() - t0;
    line("futex_wait", rc == -1 && e == ETIMEDOUT && slept >= 0.09, rc == 0 ? 0 : e);

    rc = syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    line("futex_wake", rc >= 0, errno);

    // The Aether scheduler's CPU count.
    cpu_set_t set;
    CPU_ZERO(&set);
    rc = syscall(SYS_sched_getaffinity, 0, sizeof(set), &set);
    line("sched_getaffinity", rc > 0 && CPU_COUNT(&set) > 0, errno);

#ifdef SYS_membarrier
    // MEMBARRIER_CMD_QUERY (0): the commands this kernel has, never an error
    // on a kernel that has membarrier at all.
    rc = syscall(SYS_membarrier, 0, 0);
    line("membarrier", rc >= 0, errno);
#else
    printf("membarrier ok\n");
#endif

    rc = syscall(SYS_gettid);
    line("gettid", rc == (long)getpid(), errno);

    // Six arguments, the last at a page offset: the forwarding must hand
    // every one to the kernel where the caller put it (on x86_64 the sixth
    // is on the stack).
    long page = sysconf(_SC_PAGESIZE);
    rc = syscall(SYS_mmap, NULL, page, PROT_READ, MAP_PRIVATE, fd, page);
    int ok = rc != -1 && ((const char*)rc)[0] == 'B' && ((const char*)rc)[page - 1] == 'B';
    line("mmap_six_args", ok, errno);
    if (rc != -1) munmap((void*)rc, (size_t)page);

    // A raw open goes round the open() wrapper's check: policed.
    rc = syscall(SYS_openat, AT_FDCWD, argv[2], O_RDONLY, 0);
    line("raw_openat", rc >= 0, errno);
    if (rc >= 0) close((int)rc);

    // The file system past the wrappers: raw unlinkat and renameat.
    rc = syscall(SYS_unlinkat, AT_FDCWD, a1, 0);
    line("raw_unlinkat", rc == 0, errno);
    rc = syscall(SYS_renameat, AT_FDCWD, b1, AT_FDCWD, b2);
    line("raw_renameat", rc == 0, errno);
    // libc's unlink(): held to the fs grants since #3751 (a level granting
    // nothing refuses it with EACCES), where the raw call is held by name.
    rc = unlink(a2);
    line("libc_unlink", rc == 0, errno);

    // A signal to another process (signal 0: only asks whether it may),
    // and to this one.
    rc = syscall(SYS_kill, (long)getppid(), 0);
    line("raw_kill_other", rc == 0, errno);
    rc = syscall(SYS_kill, (long)getpid(), 0);
    line("raw_kill_self", rc == 0, errno);
    rc = syscall(SYS_tgkill, (long)getpid(), syscall(SYS_gettid), 0);
    line("raw_tgkill_self", rc == 0, errno);

    // A datagram to an address of the caller's choosing (UDP, port 9).
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = udp < 0 ? -1 : syscall(SYS_sendto, udp, "x", 1L, 0L, &to, (long)sizeof to);
    line("raw_sendto", rc == 1, errno);

    // A call this preload has no name for (an unassigned number) is refused
    // under a policy like any other unlisted one; ENOSYS from the kernel
    // otherwise.
    rc = syscall(4000L, 0L);
    line("raw_unknown", rc == 0 || errno == ENOSYS, errno);

    if (udp >= 0) close(udp);
    unlink(b1);
    unlink(b2);
    unlink(a1);
    close(fd);
    return 0;
}
