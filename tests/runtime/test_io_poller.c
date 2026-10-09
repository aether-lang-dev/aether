// The I/O poller's contract, held for whichever backend is compiled in
// (#2679): a wait on an empty set lasts its timeout, and a descriptor added
// from another thread during a wait is reported by that wait. epoll and kqueue
// keep the set in the kernel and give both. The poll() fallback, which is
// what Windows runs, returned at once on an empty set, so the HTTP server's
// parking-lot thread spun a core on every idle server, and it watched a
// descriptor added mid-wait only from the next wait.
#include "test_harness.h"
#include "../../runtime/scheduler/aether_io_poller.h"
#include "../../runtime/utils/aether_thread.h"
#include "../../std/udp/aether_udp.h"

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* A build without threads keeps the single-threaded poller, which answers an
 * empty wait at once: nothing could add to the set while it waited. */
#if AETHER_HAS_THREADS

TEST_CATEGORY(io_poller_empty_wait_lasts_its_timeout, TEST_CATEGORY_RUNTIME) {
    AetherIoPoller p;
    ASSERT_EQ(0, aether_io_poller_init(&p));
    AetherIoEvent ev[4];
    uint64_t t0 = aether_now_ns();
    int n = aether_io_poller_poll(&p, ev, 4, 150);
    uint64_t ms = (aether_now_ns() - t0) / 1000000ULL;
    aether_io_poller_destroy(&p);
    ASSERT_EQ(0, n);
    /* The slack is the system timer's resolution, not the contract's. */
    ASSERT_TRUE(ms >= 120);
}

#if AETHER_HAS_NETWORKING

typedef struct {
    AetherIoPoller* poller;
    int n;
    int fd;
    uint64_t ms;
} PollerWait;

static void* poller_wait_once(void* arg) {
    PollerWait* w = (PollerWait*)arg;
    AetherIoEvent ev[4];
    uint64_t t0 = aether_now_ns();
    w->n = aether_io_poller_poll(w->poller, ev, 4, 3000);
    w->ms = (aether_now_ns() - t0) / 1000000ULL;
    w->fd = w->n > 0 ? ev[0].fd : -1;
    return NULL;
}

TEST_CATEGORY(io_poller_sees_an_add_during_a_wait, TEST_CATEGORY_RUNTIME) {
    /* A datagram socket with a datagram waiting: readable from the start. */
    UdpSocket* s = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(s);
    int port = udp_local_port_raw(s);
    ASSERT_EQ(1, udp_send_to_raw(s, "127.0.0.1", port, "x", 1));
    int fd = udp_fd_raw(s);

    AetherIoPoller p;
    ASSERT_EQ(0, aether_io_poller_init(&p));
    PollerWait w = { &p, -1, -1, 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, poller_wait_once, &w));
    sleep_ms(100);   /* the wait has started, on an empty set */
    int added = aether_io_poller_add(&p, fd, NULL, AETHER_IO_READ);
    pthread_join(t, NULL);
    aether_io_poller_destroy(&p);
    udp_close(s);

    ASSERT_EQ(0, added);
    ASSERT_EQ(1, w.n);
    ASSERT_EQ(fd, w.fd);
    /* Reported by the wait it was added during, not after its 3 s timeout. */
    ASSERT_TRUE(w.ms < 1500);
}

#endif /* AETHER_HAS_NETWORKING */
#endif /* AETHER_HAS_THREADS */
