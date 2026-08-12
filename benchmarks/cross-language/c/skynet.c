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

/* Sequential below SEQ_THRESHOLD. Same threshold in every language in this
 * suite, so all of them create the same 1,111 concurrency units and perform the
 * same 1,000,000 leaf additions. */
#define SEQ_THRESHOLD 1000

static long long skynet_seq(long long offset, long long size) {
    if (size == 1) return offset;
    long long child_size = size / 10;
    long long sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += skynet_seq(offset + (long long)i * child_size, child_size);
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

    /* Divide by the concurrency units created, not by the tree's node count.
     *
     * Every language in this suite now uses the same SEQ_THRESHOLD, so every
     * one creates the same 1,111 units and performs the same num_leaves leaf
     * additions. A per-unit cost is therefore directly comparable, and it is
     * what skynet is for: the price of creating a unit, passing its result up
     * and aggregating.
     *
     * It used to divide by the full 1,111,111-node tree while creating between
     * 1,111 and 1,111,111 units depending on the language, so whichever
     * implementation created the fewest scored highest. */
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
