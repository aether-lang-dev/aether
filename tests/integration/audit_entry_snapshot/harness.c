#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "aether_audit.h"

#define RING_CAP 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int category_read;
static int entry_overwritten;

static void* overwrite_oldest(void* unused) {
    (void)unused;

    pthread_mutex_lock(&lock);
    while (!category_read) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);

    aether_sandbox_audit("cat-new", "res-new", 1);

    pthread_mutex_lock(&lock);
    entry_overwritten = 1;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void) {
    aether_audit_clear();
    for (int i = 0; i < RING_CAP; i++) {
        char category[32];
        char resource[32];
        snprintf(category, sizeof(category), "cat-%03d", i);
        snprintf(resource, sizeof(resource), "res-%03d", i);
        aether_sandbox_audit(category, resource, i & 1);
    }

    pthread_t writer;
    if (pthread_create(&writer, NULL, overwrite_oldest, NULL) != 0) return 2;

    const char* category = aether_audit_entry_category(0);

    pthread_mutex_lock(&lock);
    category_read = 1;
    pthread_cond_signal(&changed);
    while (!entry_overwritten) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);

    const char* resource = aether_audit_entry_resource(0);
    int allowed = aether_audit_entry_allowed(0);
    pthread_join(writer, NULL);

    if (strcmp(category, "cat-000") != 0 ||
        strcmp(resource, "res-000") != 0 || allowed != 0) {
        fprintf(stderr, "FAIL mixed audit entry: category=%s resource=%s allowed=%d\n",
                category, resource, allowed);
        return 1;
    }

    puts("PASS: audit entry fields come from one snapshot");
    return 0;
}
