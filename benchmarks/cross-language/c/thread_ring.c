// C Thread Ring Benchmark (Savina-style)
// N threads in a ring pass a token M times

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define RING_SIZE 100
static int NUM_HOPS = 100000;  // Default for "low" preset

typedef struct Node {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int token;
    int has_token;
    struct Node* next;
    int received;
    int done;
} Node;

static Node nodes[RING_SIZE];
static int final_received = 0;
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
static int all_done = 0;

void* node_thread(void* arg) {
    Node* self = (Node*)arg;

    while (1) {
        pthread_mutex_lock(&self->mutex);
        while (!self->has_token && !self->done) {
            pthread_cond_wait(&self->cond, &self->mutex);
        }

        if (self->done) {
            pthread_mutex_unlock(&self->mutex);
            break;
        }

        int token = self->token;
        self->has_token = 0;
        self->received++;
        pthread_mutex_unlock(&self->mutex);

        if (token == 0) {
            // Token exhausted, signal completion
            pthread_mutex_lock(&done_mutex);
            final_received = self->received;
            all_done = 1;
            pthread_cond_signal(&done_cond);
            pthread_mutex_unlock(&done_mutex);
            break;
        }

        // Pass token to next node
        Node* next = self->next;
        pthread_mutex_lock(&next->mutex);
        next->token = token - 1;
        next->has_token = 1;
        pthread_cond_signal(&next->cond);
        pthread_mutex_unlock(&next->mutex);
    }

    return NULL;
}

int main(void) {
    const char* env = getenv("BENCHMARK_MESSAGES");
    if (env) NUM_HOPS = atoi(env);

    printf("=== C Thread Ring Benchmark ===\n");
    printf("Ring size: %d, Hops: %d\n\n", RING_SIZE, NUM_HOPS);

    pthread_t threads[RING_SIZE];

    // Initialize nodes
    for (int i = 0; i < RING_SIZE; i++) {
        pthread_mutex_init(&nodes[i].mutex, NULL);
        pthread_cond_init(&nodes[i].cond, NULL);
        nodes[i].token = 0;
        nodes[i].has_token = 0;
        nodes[i].next = &nodes[(i + 1) % RING_SIZE];
        nodes[i].received = 0;
        nodes[i].done = 0;
    }

    // Start threads
    for (int i = 0; i < RING_SIZE; i++) {
        pthread_create(&threads[i], NULL, node_thread, &nodes[i]);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Inject initial token
    pthread_mutex_lock(&nodes[0].mutex);
    nodes[0].token = NUM_HOPS;
    nodes[0].has_token = 1;
    pthread_cond_signal(&nodes[0].cond);
    pthread_mutex_unlock(&nodes[0].mutex);

    // Wait for completion
    pthread_mutex_lock(&done_mutex);
    while (!all_done) {
        pthread_cond_wait(&done_cond, &done_mutex);
    }
    pthread_mutex_unlock(&done_mutex);

    clock_gettime(CLOCK_MONOTONIC, &end);

    // Signal all threads to exit
    for (int i = 0; i < RING_SIZE; i++) {
        pthread_mutex_lock(&nodes[i].mutex);
        nodes[i].done = 1;
        pthread_cond_signal(&nodes[i].cond);
        pthread_mutex_unlock(&nodes[i].mutex);
    }

    for (int i = 0; i < RING_SIZE; i++) {
        pthread_join(threads[i], NULL);
    }

    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    int total_messages = NUM_HOPS + 1;
    double throughput = total_messages / elapsed / 1e6;
    double ns_per_msg = elapsed * 1e9 / total_messages;

    printf("ns/msg:         %.2f\n", ns_per_msg);
    printf("Throughput:     %.2f M msg/sec\n", throughput);

    // Cleanup
    for (int i = 0; i < RING_SIZE; i++) {
        pthread_mutex_destroy(&nodes[i].mutex);
        pthread_cond_destroy(&nodes[i].cond);
    }

    return 0;
}
