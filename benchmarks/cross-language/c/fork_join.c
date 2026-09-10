// C Fork-Join Benchmark (Savina-style)
// K worker threads, M messages distributed round-robin

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define NUM_WORKERS 8
static int MESSAGES_PER_WORKER = 12500;  // Default for "low" preset (100000 / 8)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int* queue;
    int queue_head;
    int queue_tail;
    int queue_size;
    int queue_capacity;
    int processed;
    int done;
} Worker;

static Worker workers[NUM_WORKERS];

void worker_enqueue(Worker* w, int value) {
    pthread_mutex_lock(&w->mutex);
    if (w->queue_size >= w->queue_capacity) {
        w->queue_capacity *= 2;
        w->queue = realloc(w->queue, w->queue_capacity * sizeof(int));
    }
    w->queue[w->queue_tail] = value;
    w->queue_tail = (w->queue_tail + 1) % w->queue_capacity;
    w->queue_size++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

void* worker_thread(void* arg) {
    Worker* self = (Worker*)arg;

    while (1) {
        pthread_mutex_lock(&self->mutex);
        while (self->queue_size == 0 && !self->done) {
            pthread_cond_wait(&self->cond, &self->mutex);
        }

        if (self->done && self->queue_size == 0) {
            pthread_mutex_unlock(&self->mutex);
            break;
        }

        // Dequeue
        self->queue_head = (self->queue_head + 1) % self->queue_capacity;
        self->queue_size--;
        self->processed++;
        pthread_mutex_unlock(&self->mutex);
    }

    return NULL;
}

int main(void) {
    const char* env = getenv("BENCHMARK_MESSAGES");
    if (env) MESSAGES_PER_WORKER = atoi(env) / NUM_WORKERS;

    int total = NUM_WORKERS * MESSAGES_PER_WORKER;
    printf("=== C Fork-Join Throughput Benchmark ===\n");
    printf("Workers: %d, Messages: %d\n\n", NUM_WORKERS, total);

    pthread_t threads[NUM_WORKERS];

    // Initialize workers
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_mutex_init(&workers[i].mutex, NULL);
        pthread_cond_init(&workers[i].cond, NULL);
        workers[i].queue_capacity = 1024;
        workers[i].queue = malloc(workers[i].queue_capacity * sizeof(int));
        workers[i].queue_head = 0;
        workers[i].queue_tail = 0;
        workers[i].queue_size = 0;
        workers[i].processed = 0;
        workers[i].done = 0;
    }

    // Start worker threads
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &workers[i]);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Send messages round-robin
    for (int i = 0; i < total; i++) {
        worker_enqueue(&workers[i % NUM_WORKERS], i);
    }

    // Signal workers to finish
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_mutex_lock(&workers[i].mutex);
        workers[i].done = 1;
        pthread_cond_signal(&workers[i].cond);
        pthread_mutex_unlock(&workers[i].mutex);
    }

    // Wait for all workers
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    // Collect results
    int total_processed = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        total_processed += workers[i].processed;
    }

    if (total_processed != total) {
        printf("VALIDATION FAILED: expected %d, got %d\n", total, total_processed);
    }

    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double throughput = total / elapsed / 1e6;
    double ns_per_msg = elapsed * 1e9 / total;

    printf("ns/msg:         %.2f\n", ns_per_msg);
    printf("Throughput:     %.2f M msg/sec\n", throughput);

    // Cleanup
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_mutex_destroy(&workers[i].mutex);
        pthread_cond_destroy(&workers[i].cond);
        free(workers[i].queue);
    }

    return 0;
}
