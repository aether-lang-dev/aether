/**
 * Zero-Copy Message Passing Benchmark
 * Measures performance improvement from ownership transfer vs copying
 */

#include "test_harness.h"
#include "actor_state_machine.h"
#include "multicore_scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define get_time_us() (GetTickCount64() * 1000)
#else
#include <sys/time.h>
#include <unistd.h>          /* usleep */
static long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}
#endif

typedef struct {
    int id;
    atomic_int active;
    atomic_int assigned_core;
    Mailbox mailbox;
    void (*step)(void*);
    int received_count;
    long total_bytes;
} BenchActor;

void bench_actor_step(BenchActor* self) {
    Message msg;
    while (mailbox_receive(&self->mailbox, &msg)) {
        self->received_count++;
        if (msg.zerocopy.owned && msg.zerocopy.data) {
            self->total_bytes += msg.zerocopy.size;
            message_free(&msg);
        }
    }
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

void benchmark_small_messages() {
    printf("\n=== Benchmark: Small Messages (64 bytes, inline) ===\n");

    scheduler_init(1);

    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->received_count = 0;
    actor->total_bytes = 0;
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    const int COUNT = 10000;
    long start = get_time_us();
    
    for (int i = 0; i < COUNT; i++) {
        Message msg = message_create_simple(1, 0, i);
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for processing
    #ifdef _WIN32
    Sleep(200);
    #else
    usleep(200000);
    #endif
    
    long duration = get_time_us() - start;
    
    scheduler_shutdown();
    
    double throughput = (COUNT * 1000000.0) / duration;
    printf("  Messages: %d\n", COUNT);
    printf("  Duration: %.2f ms\n", duration / 1000.0);
    printf("  Throughput: %.0f msg/sec\n", throughput);
    
    free(actor);
}

void benchmark_large_messages() {
    printf("\n=== Benchmark: Large Messages (1KB, zero-copy) ===\n");
    
    scheduler_init(1);
    
    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->received_count = 0;
    actor->total_bytes = 0;
    mailbox_init(&actor->mailbox);

    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();

    const int COUNT = 5000;
    const int SIZE = 1024;
    long start = get_time_us();
    
    for (int i = 0; i < COUNT; i++) {
        void* data = malloc(SIZE);
        if (data) {
            memset(data, i & 0xFF, SIZE);
            Message msg = message_create_zerocopy(1, 0, data, SIZE);
            scheduler_send_remote((ActorBase*)actor, msg, -1);
        }
    }
    
    // Wait for processing
    #ifdef _WIN32
    Sleep(200);
    #else
    usleep(200000);
    #endif
    
    long duration = get_time_us() - start;
    
    scheduler_shutdown();
    
    double throughput = (actor->received_count * 1000000.0) / duration;
    double bandwidth = (actor->total_bytes * 1000000.0) / duration / (1024 * 1024);
    printf("  Messages: %d\n", actor->received_count);
    printf("  Total bytes: %ld MB\n", actor->total_bytes / (1024 * 1024));
    printf("  Duration: %.2f ms\n", duration / 1000.0);
    printf("  Throughput: %.0f msg/sec\n", throughput);
    printf("  Bandwidth: %.2f MB/sec\n", bandwidth);
    
    free(actor);
}

void benchmark_mixed_messages() {
    printf("\n=== Benchmark: Mixed Messages (33%% large) ===\n");
    
    scheduler_init(1);
    
    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->received_count = 0;
    actor->total_bytes = 0;
    mailbox_init(&actor->mailbox);

    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();

    const int COUNT = 10000;
    const int SIZE = 512;
    long start = get_time_us();
    
    for (int i = 0; i < COUNT; i++) {
        if (i % 3 == 0) {
            void* data = malloc(SIZE);
            if (data) {
                memset(data, i & 0xFF, SIZE);
                Message msg = message_create_zerocopy(1, 0, data, SIZE);
                scheduler_send_remote((ActorBase*)actor, msg, -1);
            }
        } else {
            Message msg = message_create_simple(1, 0, i);
            scheduler_send_remote((ActorBase*)actor, msg, -1);
        }
    }
    
    // Wait for processing
    #ifdef _WIN32
    Sleep(200);
    #else
    usleep(200000);
    #endif
    
    long duration = get_time_us() - start;
    
    scheduler_shutdown();
    
    double throughput = (actor->received_count * 1000000.0) / duration;
    printf("  Messages: %d\n", actor->received_count);
    printf("  Duration: %.2f ms\n", duration / 1000.0);
    printf("  Throughput: %.0f msg/sec\n", throughput);
    
    free(actor);
}

int main() {
    printf("Zero-Copy Message Passing Performance Benchmark\n");
    printf("================================================\n");
    
    benchmark_small_messages();
    benchmark_large_messages();
    benchmark_mixed_messages();
    
    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
