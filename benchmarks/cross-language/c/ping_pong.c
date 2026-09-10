// Pure C ping-pong benchmark with pthreads
// Baseline comparison showing raw pthread performance without Aether optimizations
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

// Configurable via BENCHMARK_MESSAGES env var, default 100K for "low" preset
static int MESSAGES = 100000;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int value;
} Channel;

Channel *chan_a, *chan_b;

void* ping_thread(void* arg) {
    for (int i = 0; i < MESSAGES; i++) {
        // Send to A
        pthread_mutex_lock(&chan_a->mutex);
        chan_a->value = i;
        chan_a->ready = 1;
        pthread_cond_signal(&chan_a->cond);
        pthread_mutex_unlock(&chan_a->mutex);

        // Wait for B
        pthread_mutex_lock(&chan_b->mutex);
        while (!chan_b->ready) {
            pthread_cond_wait(&chan_b->cond, &chan_b->mutex);
        }
        int received_value = chan_b->value;
        chan_b->ready = 0;
        pthread_mutex_unlock(&chan_b->mutex);

        // VALIDATE: Must receive echo of what we sent
        if (received_value != i) {
            fprintf(stderr, "ERROR: Ping sent %d but got back %d\n", i, received_value);
        }
    }
    return NULL;
}

void* pong_thread(void* arg) {
    for (int i = 0; i < MESSAGES; i++) {
        // Wait for A
        pthread_mutex_lock(&chan_a->mutex);
        while (!chan_a->ready) {
            pthread_cond_wait(&chan_a->cond, &chan_a->mutex);
        }
        int received_value = chan_a->value;
        chan_a->ready = 0;
        pthread_mutex_unlock(&chan_a->mutex);

        // VALIDATE: Must receive expected sequence
        if (received_value != i) {
            fprintf(stderr, "ERROR: Pong expected %d but got %d\n", i, received_value);
        }

        // Send to B - echo back what we received
        pthread_mutex_lock(&chan_b->mutex);
        chan_b->value = received_value;
        chan_b->ready = 1;
        pthread_cond_signal(&chan_b->cond);
        pthread_mutex_unlock(&chan_b->mutex);
    }
    return NULL;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void) {
    // Read message count from environment
    const char* env = getenv("BENCHMARK_MESSAGES");
    if (env) MESSAGES = atoi(env);

    chan_a = calloc(1, sizeof(Channel));
    chan_b = calloc(1, sizeof(Channel));

    pthread_mutex_init(&chan_a->mutex, NULL);
    pthread_cond_init(&chan_a->cond, NULL);
    pthread_mutex_init(&chan_b->mutex, NULL);
    pthread_cond_init(&chan_b->cond, NULL);

    printf("=== C (pthread) Ping-Pong Benchmark ===\n");
    printf("Messages: %d\n", MESSAGES);
    printf("Using pthread mutexes and condition variables\n\n");

    uint64_t start = get_time_ns();

    pthread_t t1, t2;
    pthread_create(&t1, NULL, ping_thread, NULL);
    pthread_create(&t2, NULL, pong_thread, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    uint64_t end = get_time_ns();
    double elapsed_sec = (double)(end - start) / 1e9;
    double throughput = MESSAGES / elapsed_sec;
    double ns_per_msg = elapsed_sec * 1e9 / MESSAGES;

    printf("ns/msg:         %.2f\n", ns_per_msg);
    printf("Throughput:     %.2f M msg/sec\n", throughput / 1e6);

    pthread_mutex_destroy(&chan_a->mutex);
    pthread_cond_destroy(&chan_a->cond);
    pthread_mutex_destroy(&chan_b->mutex);
    pthread_cond_destroy(&chan_b->cond);
    free(chan_a);
    free(chan_b);

    return 0;
}
