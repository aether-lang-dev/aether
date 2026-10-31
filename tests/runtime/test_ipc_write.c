// std.ipc's write loop (std/ipc/aether_ipc.c): ipc.write must deliver every
// byte through a send that a signal cuts short, and retry one a signal
// interrupts before it sent anything (EINTR). A blocking send on a socket
// never comes back short by itself, so a test that only writes a large
// payload passes with or without the loop; this one makes the kernel return
// early. The sending thread is signalled over and over while its send is
// blocked on a full socket, its reader stalled.
#include "test_harness.h"
#ifndef _WIN32
#include "../../runtime/utils/aether_spawn.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char* ipc_write_raw(int fd, const void* bytes);

#define IPC_WRITE_LEN (1 << 20) /* far past a socket's buffer: the send blocks */
#define IPC_STALL_AT (256 * 1024)

static pthread_t ipc_write_sender;
static int ipc_write_stop;

static void ipc_write_on_signal(int sig) { (void)sig; }

/* Signals the sending thread, not the process, so the one blocked in send
 * is the one interrupted. */
static void* ipc_write_pester(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&ipc_write_stop, __ATOMIC_ACQUIRE)) {
        pthread_kill(ipc_write_sender, SIGUSR1);
        usleep(5000);
    }
    return NULL;
}

static char ipc_write_byte(size_t i) { return (char)('a' + (i * 7 + i / 4093) % 26); }

/* The reader: stalls before reading and again a quarter of the way in,
 * then checks it got every byte, in order. Exits 0 when it did. */
static void ipc_write_reader(int fd) {
    char buf[4096];
    size_t got = 0;
    int stalled = 0;
    usleep(200 * 1000);
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != ipc_write_byte(got + (size_t)i)) _exit(2);
        }
        got += (size_t)n;
        if (!stalled && got >= IPC_STALL_AT) {
            stalled = 1;
            usleep(200 * 1000);
        }
    }
    _exit(got == IPC_WRITE_LEN ? 0 : 1);
}

TEST_CATEGORY(ipc_write_finishes_sends_signals_cut_short, TEST_CATEGORY_RUNTIME) {
    int sv[2];
    ASSERT_EQ(0, aether_socketpair_cloexec(sv));
    char* data = (char*)malloc(IPC_WRITE_LEN + 1);
    ASSERT_NOT_NULL(data);
    for (size_t i = 0; i < IPC_WRITE_LEN; i++) data[i] = ipc_write_byte(i);
    data[IPC_WRITE_LEN] = '\0';

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        ipc_write_reader(sv[1]);
    }
    close(sv[1]);

    /* No SA_RESTART: an interrupted send returns what it sent, or EINTR. */
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ipc_write_on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old);
    ipc_write_sender = pthread_self();
    __atomic_store_n(&ipc_write_stop, 0, __ATOMIC_RELEASE);
    pthread_t pester;
    int started = pthread_create(&pester, NULL, ipc_write_pester, NULL) == 0;

    const char* err = ipc_write_raw(sv[0], data);

    __atomic_store_n(&ipc_write_stop, 1, __ATOMIC_RELEASE);
    if (started) pthread_join(pester, NULL);
    sigaction(SIGUSR1, &old, NULL);
    close(sv[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    free(data);

    ASSERT_TRUE(started);
    ASSERT_STREQ("", err);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
}
#endif
