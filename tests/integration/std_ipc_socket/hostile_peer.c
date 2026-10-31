/* A peer that breaks std.ipc's descriptor rules, for probe.ae. Spawned with
 * os.run_socket, so its channel is fd AETHER_IPC_FD. Each message waits for
 * the probe's "next" first, so no two arrive in one recvmsg:
 *   1. two descriptors in one message
 *   2. ten descriptors in one message (more than the eight recv_fd once
 *      made room for: macOS then installed all ten but showed eight, and
 *      the two it hid stayed open in the probe)
 *   3. one descriptor, as the rules allow, to show the channel still works
 * Each descriptor is a fresh open of /dev/null, so every one the probe is
 * handed is a new entry in its descriptor table. Exits 0 when the probe
 * closes the channel. Built by test_std_ipc_socket.sh with $CC: the public
 * API cannot send more than one descriptor, which is the point. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int ch;

static void await_next(void) {
    char buf[4];
    size_t got = 0;
    while (got < sizeof buf) {
        ssize_t n = read(ch, buf + got, sizeof buf - got);
        if (n <= 0) exit(0);
        got += (size_t)n;
    }
}

static void send_fds(int count, const char* payload) {
    int fds[16];
    for (int i = 0; i < count; i++) {
        fds[i] = open("/dev/null", O_RDONLY);
        if (fds[i] < 0) { perror("hostile_peer: open"); exit(2); }
    }
    union {
        char buf[CMSG_SPACE(sizeof(int) * 16)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);
    struct iovec iov = { (void*)payload, strlen(payload) };
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)count);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)count);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)count);
    if (sendmsg(ch, &msg, 0) < 0) { perror("hostile_peer: sendmsg"); exit(2); }
    for (int i = 0; i < count; i++) close(fds[i]);
}

int main(void) {
    const char* s = getenv("AETHER_IPC_FD");
    if (!s) { fprintf(stderr, "hostile_peer: no AETHER_IPC_FD\n"); return 2; }
    ch = atoi(s);
    await_next();
    send_fds(2, "two");
    await_next();
    send_fds(10, "ten");
    await_next();
    send_fds(1, "one");
    await_next();
    return 0;
}
