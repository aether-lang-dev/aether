#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

// Under --emit=obj the exported Aether symbol is aether_<name>.
extern void aether_fuzz_target(const void* data, int32_t len);

// The writer thread the harness uses to deliver a chunk while the reader is
// already waiting: one at a time, started by harness_write_late and reaped
// by harness_write_late_join. The delay gives the reader time to reach
// poll() before the bytes land; whether it did is not what the model
// checks, so a slow start costs nothing but the path.
static pthread_t writer;
static int writer_live;
static int writer_fd;
static const unsigned char* writer_data;
static size_t writer_len;

static void* writer_run(void* arg) {
    (void)arg;
    usleep(1000);
    size_t off = 0;
    while (off < writer_len) {
        ssize_t w = write(writer_fd, writer_data + off, writer_len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            abort();
        }
        off += (size_t)w;
    }
    return NULL;
}

void harness_write_late(int32_t fd, const void* data, int32_t len) {
    if (writer_live || len < 0) abort();
    writer_fd = fd;
    writer_data = (const unsigned char*)data;
    writer_len = (size_t)len;
    if (pthread_create(&writer, NULL, writer_run, NULL) != 0) abort();
    writer_live = 1;
}

void harness_write_late_join(void) {
    if (!writer_live) return;
    if (pthread_join(writer, NULL) != 0) abort();
    writer_live = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 0x7fffffff) return 0;
    aether_fuzz_target((const void*)data, (int32_t)size);
    harness_write_late_join();
    return 0;
}
