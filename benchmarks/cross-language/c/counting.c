// C Counting Actor Benchmark (Savina-style)
// Single counter actor receives N increment messages

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static int MESSAGES = 100000;  // Default for "low" preset

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int done;
} Counter;

static Counter counter;

void* counter_thread(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&counter.mutex);
        while (counter.count == 0 && !counter.done) {
            pthread_cond_wait(&counter.cond, &counter.mutex);
        }
        if (counter.done && counter.count == 0) {
            pthread_mutex_unlock(&counter.mutex);
            break;
        }
        counter.count--;
        pthread_mutex_unlock(&counter.mutex);
    }
    return NULL;
}

int main(void) {
    const char* env = getenv("BENCHMARK_MESSAGES");
    if (env) MESSAGES = atoi(env);

    printf("=== C Counting Actor Benchmark ===\n");
    printf("Messages: %d\n\n", MESSAGES);

    pthread_mutex_init(&counter.mutex, NULL);
    pthread_cond_init(&counter.cond, NULL);
    counter.count = 0;
    counter.done = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, counter_thread, NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Send all messages
    for (int i = 0; i < MESSAGES; i++) {
        pthread_mutex_lock(&counter.mutex);
        counter.count++;
        pthread_cond_signal(&counter.cond);
        pthread_mutex_unlock(&counter.mutex);
    }

    // Signal done
    pthread_mutex_lock(&counter.mutex);
    counter.done = 1;
    pthread_cond_signal(&counter.cond);
    pthread_mutex_unlock(&counter.mutex);

    pthread_join(thread, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double throughput = MESSAGES / elapsed / 1e6;
    double ns_per_msg = elapsed * 1e9 / MESSAGES;

    printf("ns/msg:         %.2f\n", ns_per_msg);
    printf("Throughput:     %.2f M msg/sec\n", throughput);

    pthread_mutex_destroy(&counter.mutex);
    pthread_cond_destroy(&counter.cond);

    return 0;
}
