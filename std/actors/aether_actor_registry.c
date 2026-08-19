#include "aether_actor_registry.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK ae_reg_rwlock_t;
static void ae_reg_rwlock_rdlock  (ae_reg_rwlock_t* lk) { AcquireSRWLockShared(lk); }
static void ae_reg_rwlock_rdunlock(ae_reg_rwlock_t* lk) { ReleaseSRWLockShared(lk); }
static void ae_reg_rwlock_wrlock  (ae_reg_rwlock_t* lk) { AcquireSRWLockExclusive(lk); }
static void ae_reg_rwlock_wrunlock(ae_reg_rwlock_t* lk) { ReleaseSRWLockExclusive(lk); }
#define AE_REG_RWLOCK_INITIALIZER SRWLOCK_INIT
#else
/* The portable thread shim, not raw <pthread.h> (see its header note).
 * It is real pthreads on POSIX, a CRITICAL_SECTION shim on Windows, and
 * no-op stubs where AETHER_HAS_THREADS is 0. Including <pthread.h> directly
 * broke wasm32-wasi (#1655): wasi-libc SHIPS a pthread.h even though its
 * pthread_create is a stub, so its real typedefs collided with the
 * threadless shim's in the same TU. */
#include "../../runtime/utils/aether_thread.h"
typedef pthread_rwlock_t ae_reg_rwlock_t;
static void ae_reg_rwlock_rdlock  (ae_reg_rwlock_t* lk) { pthread_rwlock_rdlock(lk); }
static void ae_reg_rwlock_rdunlock(ae_reg_rwlock_t* lk) { pthread_rwlock_unlock(lk); }
static void ae_reg_rwlock_wrlock  (ae_reg_rwlock_t* lk) { pthread_rwlock_wrlock(lk); }
static void ae_reg_rwlock_wrunlock(ae_reg_rwlock_t* lk) { pthread_rwlock_unlock(lk); }
#define AE_REG_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER
#endif

/* Smaller bucket count than std.config because the actor registry
 * is for "a handful of named long-lived actors per program" — the
 * config registry's headcount can grow to dozens of CLI flags. */
#define AE_REG_BUCKETS 32

typedef struct ae_reg_entry {
    char* name;
    void* ref;
    struct ae_reg_entry* next;
} ae_reg_entry;

static ae_reg_entry* g_buckets[AE_REG_BUCKETS];
static int g_size = 0;
static ae_reg_rwlock_t g_lock = AE_REG_RWLOCK_INITIALIZER;

static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static char* dup_str(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* d = (char*)aether_caps_malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static ae_reg_entry* find_in_bucket(ae_reg_entry* head, const char* name) {
    for (ae_reg_entry* e = head; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

void aether_actor_register(const char* name, void* ref) {
    if (!name || !ref) return;
    uint32_t h = fnv1a(name);
    int idx = (int)(h & (AE_REG_BUCKETS - 1));

    ae_reg_rwlock_wrlock(&g_lock);
    ae_reg_entry* existing = find_in_bucket(g_buckets[idx], name);
    if (existing) {
        existing->ref = ref;  /* overwrite — actors don't own their
                                  registry slot, the caller does */
        ae_reg_rwlock_wrunlock(&g_lock);
        return;
    }
    ae_reg_entry* e = (ae_reg_entry*)aether_caps_malloc(sizeof(ae_reg_entry));
    if (!e) {
        ae_reg_rwlock_wrunlock(&g_lock);
        return;
    }
    e->name = dup_str(name);
    if (!e->name) {
        aether_caps_free(e, sizeof(ae_reg_entry));
        ae_reg_rwlock_wrunlock(&g_lock);
        return;
    }
    e->ref = ref;
    e->next = g_buckets[idx];
    g_buckets[idx] = e;
    g_size++;
    ae_reg_rwlock_wrunlock(&g_lock);
}

void* aether_actor_whereis(const char* name) {
    if (!name) return NULL;
    uint32_t h = fnv1a(name);
    int idx = (int)(h & (AE_REG_BUCKETS - 1));

    ae_reg_rwlock_rdlock(&g_lock);
    ae_reg_entry* e = find_in_bucket(g_buckets[idx], name);
    void* r = e ? e->ref : NULL;
    ae_reg_rwlock_rdunlock(&g_lock);
    return r;
}

int aether_actor_unregister(const char* name) {
    if (!name) return 0;
    uint32_t h = fnv1a(name);
    int idx = (int)(h & (AE_REG_BUCKETS - 1));

    ae_reg_rwlock_wrlock(&g_lock);
    ae_reg_entry** prev = &g_buckets[idx];
    while (*prev) {
        if (strcmp((*prev)->name, name) == 0) {
            ae_reg_entry* dead = *prev;
            *prev = dead->next;
            aether_caps_free(dead->name, strlen(dead->name) + 1);
            aether_caps_free(dead, sizeof(ae_reg_entry));
            g_size--;
            ae_reg_rwlock_wrunlock(&g_lock);
            return 1;
        }
        prev = &(*prev)->next;
    }
    ae_reg_rwlock_wrunlock(&g_lock);
    return 0;
}

int aether_actor_is_registered(const char* name) {
    if (!name) return 0;
    uint32_t h = fnv1a(name);
    int idx = (int)(h & (AE_REG_BUCKETS - 1));

    ae_reg_rwlock_rdlock(&g_lock);
    ae_reg_entry* e = find_in_bucket(g_buckets[idx], name);
    int result = e ? 1 : 0;
    ae_reg_rwlock_rdunlock(&g_lock);
    return result;
}

int aether_actor_registry_size(void) {
    ae_reg_rwlock_rdlock(&g_lock);
    int n = g_size;
    ae_reg_rwlock_rdunlock(&g_lock);
    return n;
}

void aether_actor_registry_clear(void) {
    ae_reg_rwlock_wrlock(&g_lock);
    for (int i = 0; i < AE_REG_BUCKETS; i++) {
        ae_reg_entry* e = g_buckets[i];
        while (e) {
            ae_reg_entry* next = e->next;
            aether_caps_free(e->name, strlen(e->name) + 1);
            aether_caps_free(e, sizeof(ae_reg_entry));
            e = next;
        }
        g_buckets[i] = NULL;
    }
    g_size = 0;
    ae_reg_rwlock_wrunlock(&g_lock);
}
