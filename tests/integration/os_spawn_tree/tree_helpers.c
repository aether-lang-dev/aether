/* Helpers for test_os_spawn_tree.ae: what std.os has no call for, because
 * only a test wants it. */
/* A handle the program means never to free (the case where the parent
 * exits and the tree must end anyway): held here so LeakSanitizer finds it
 * reachable, as it would a handle a long-lived program kept in a global. */
void* volatile tree_test_kept;
void tree_test_keep(void* p) { tree_test_kept = p; }

#ifdef _WIN32
#include <windows.h>
/* Windows has no sessions to leave; the job holds every descendant. */
int tree_test_setsid(void) { return -1; }
int tree_test_alive(int pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;
}
#else
#include <errno.h>
#include <signal.h>
#include <unistd.h>
int tree_test_setsid(void) { return (int)setsid(); }
/* Alive and not a zombie waiting for its parent: a zombie answers kill(pid,
 * 0) too, so where the status can be read it is checked. */
#if defined(__linux__)
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
int tree_test_alive(int pid) {
    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char* rp = strrchr(buf, ')');
    return !(rp && rp[1] == ' ' && rp[2] == 'Z');
}
#else
int tree_test_alive(int pid) { return kill((pid_t)pid, 0) == 0 || errno == EPERM; }
#endif
#endif
