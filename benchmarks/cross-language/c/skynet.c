/**
 * C Skynet Benchmark
 * Based on https://github.com/atemerev/skynet
 * Uses pthreads while the subtree is larger than SEQ_THRESHOLD, sequential below.
 * Spawning 1M OS threads is not feasible; limits concurrent threads to ~1000.
 *
 * Compile with: gcc -O3 -march=native skynet.c -o skynet -pthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

/* Same threshold in every language here, so all create 1,111 units for a
 * million leaves. See FAIRNESS.md. */
#define SEQ_THRESHOLD 1000

static long long skynet_seq(long long offset, long long size) {
    long long sum = 0;
    for (long long i = 0; i < size; i++) {
        sum += offset + i;
    }
    return sum;
}

typedef struct {
    long long offset;
    long long size;
    int depth;
    long long result;
} SkynetArg;

static void* skynet_thread(void* arg) {
    SkynetArg* a = (SkynetArg*)arg;
    long long offset = a->offset;
    long long size   = a->size;
    int       depth  = a->depth;

    if (size <= SEQ_THRESHOLD) {
        a->result = skynet_seq(offset, size);
        return NULL;
    }

    long long child_size = size / 10;
    SkynetArg children[10];
    pthread_t threads[10];

    for (int i = 0; i < 10; i++) {
        children[i].offset = offset + (long long)i * child_size;
        children[i].size   = child_size;
        children[i].depth  = depth + 1;
        children[i].result = 0;
        pthread_create(&threads[i], NULL, skynet_thread, &children[i]);
    }

    long long sum = 0;
    for (int i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
        sum += children[i].result;
    }
    a->result = sum;
    return NULL;
}

static long long get_leaves(void) {
    const char* env = getenv("SKYNET_LEAVES");
    if (env) return atoll(env);
    env = getenv("BENCHMARK_MESSAGES");
    if (env) return atoll(env);
    return 1000000LL;
}

int main(void) {
    long long num_leaves = get_leaves();

    /* Divide by units created, not the tree's node count. See FAIRNESS.md. */
    long long total_actors = 1;
    for (long long n = num_leaves; n > SEQ_THRESHOLD; n /= 10) {
        total_actors += n / SEQ_THRESHOLD;
    }
    const long long rate_base = total_actors;

    printf("=== C Skynet Benchmark ===\n");
    printf("Leaves: %lld, concurrency units: %lld (sequential below %d)\n\n",
           num_leaves, total_actors, SEQ_THRESHOLD);

    SkynetArg root = { .offset = 0, .size = num_leaves, .depth = 0, .result = 0 };

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    pthread_t root_thread;
    pthread_create(&root_thread, NULL, skynet_thread, &root);
    pthread_join(root_thread, NULL);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    long long elapsed_ns = (long long)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000LL
                         + (ts_end.tv_nsec - ts_start.tv_nsec);
    long long elapsed_us = elapsed_ns / 1000;

    printf("Sum: %lld\n", root.result);
    if (elapsed_us > 0) {
        long long ns_per_msg   = elapsed_ns / rate_base;
        long long throughput_m = rate_base / elapsed_us;
        long long leftover     = rate_base - (throughput_m * elapsed_us);
        long long frac         = (leftover * 100) / elapsed_us;
        printf("ns/msg:         %lld\n", ns_per_msg);
        printf("Throughput:     %lld.%02lld M msg/sec\n", throughput_m, frac);
    }
    return 0;
}
