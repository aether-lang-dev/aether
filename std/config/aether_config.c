#include "aether_config.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
/* SRWLock — slim reader/writer lock; same shape as pthread rwlock
 * for our purposes (readers don't block readers; writers exclusive). */
#include <windows.h>
typedef SRWLOCK ae_cfg_rwlock_t;
static void ae_cfg_rwlock_rdlock(ae_cfg_rwlock_t* lk) { AcquireSRWLockShared(lk); }
static void ae_cfg_rwlock_rdunlock(ae_cfg_rwlock_t* lk) { ReleaseSRWLockShared(lk); }
static void ae_cfg_rwlock_wrlock(ae_cfg_rwlock_t* lk) { AcquireSRWLockExclusive(lk); }
static void ae_cfg_rwlock_wrunlock(ae_cfg_rwlock_t* lk) { ReleaseSRWLockExclusive(lk); }
#define AE_CFG_RWLOCK_INITIALIZER SRWLOCK_INIT
#else
/* The portable thread shim, not raw <pthread.h> (see its header note).
 * It is real pthreads on POSIX, a CRITICAL_SECTION shim on Windows, and
 * no-op stubs where AETHER_HAS_THREADS is 0. Including <pthread.h> directly
 * broke wasm32-wasi (#1655): wasi-libc SHIPS a pthread.h even though its
 * pthread_create is a stub, so its real typedefs collided with the
 * threadless shim's in the same TU. */
#include "../../runtime/utils/aether_thread.h"
typedef pthread_rwlock_t ae_cfg_rwlock_t;
static void ae_cfg_rwlock_rdlock  (ae_cfg_rwlock_t* lk) { pthread_rwlock_rdlock(lk); }
static void ae_cfg_rwlock_rdunlock(ae_cfg_rwlock_t* lk) { pthread_rwlock_unlock(lk); }
static void ae_cfg_rwlock_wrlock  (ae_cfg_rwlock_t* lk) { pthread_rwlock_wrlock(lk); }
static void ae_cfg_rwlock_wrunlock(ae_cfg_rwlock_t* lk) { pthread_rwlock_unlock(lk); }
#define AE_CFG_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER
#endif

#define AE_CFG_BUCKETS 64  /* power of two; small fixed table fits the
                              "few dozen config keys per program" use
                              case the porter doc described. Chains
                              handle bucket overflow gracefully. */

/* A value is a counted AetherString, published (atomically counted) before
 * it is stored, so a `get` on another thread can take a reference of its
 * own under the read lock. The store holds one reference; a `put` that
 * replaces the value, or a `clear`, drops it after unlocking, and the bytes
 * live on for as long as any reader still holds what `get` gave it. A
 * borrowed `char*` (what `get` returned before) dangled the moment another
 * thread's `put` of the same key freed it, between the read unlock and the
 * caller's own copy. */
typedef struct ae_cfg_entry {
    char* key;
    AetherString* value;
    struct ae_cfg_entry* next;
} ae_cfg_entry;

static ae_cfg_entry* g_buckets[AE_CFG_BUCKETS];
static int g_size = 0;
static ae_cfg_rwlock_t g_lock = AE_CFG_RWLOCK_INITIALIZER;

/* FNV-1a 32-bit. Good enough for short config keys; not crypto. */
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

static ae_cfg_entry* find_in_bucket(ae_cfg_entry* head, const char* key) {
    for (ae_cfg_entry* e = head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

void aether_config_store_put(const char* key, const char* value) {
    if (!key || !value) return;

    AetherString* new_value = string_new(value);
    if (!new_value) return;  /* OOM — silent (matches stdlib's other
                                infallible-in-practice paths) */
    /* Reachable from every thread once stored: count it atomically. */
    aether_string_publish(new_value);

    uint32_t h = fnv1a(key);
    int idx = (int)(h & (AE_CFG_BUCKETS - 1));

    ae_cfg_rwlock_wrlock(&g_lock);
    ae_cfg_entry* existing = find_in_bucket(g_buckets[idx], key);
    if (existing) {
        AetherString* old = existing->value;
        existing->value = new_value;
        ae_cfg_rwlock_wrunlock(&g_lock);
        string_release(old);   /* the store's reference; a reader may hold more */
        return;
    }
    /* New entry. Allocate under the write lock to keep insertion
     * atomic with the bucket-head update. */
    ae_cfg_entry* e = (ae_cfg_entry*)aether_caps_malloc(sizeof(ae_cfg_entry));
    if (!e) {
        ae_cfg_rwlock_wrunlock(&g_lock);
        string_release(new_value);
        return;
    }
    e->key = dup_str(key);
    if (!e->key) {
        aether_caps_free(e, sizeof(ae_cfg_entry));
        ae_cfg_rwlock_wrunlock(&g_lock);
        string_release(new_value);
        return;
    }
    e->value = new_value;
    e->next = g_buckets[idx];
    g_buckets[idx] = e;
    g_size++;
    ae_cfg_rwlock_wrunlock(&g_lock);
}

const char* aether_config_store_get(const char* key) {
    if (!key) return (const char*)string_empty();
    uint32_t h = fnv1a(key);
    int idx = (int)(h & (AE_CFG_BUCKETS - 1));

    ae_cfg_rwlock_rdlock(&g_lock);
    ae_cfg_entry* e = find_in_bucket(g_buckets[idx], key);
    AetherString* v = NULL;
    if (e) {
        v = e->value;
        string_retain(v);   /* under the lock: no put can drop it first */
    }
    ae_cfg_rwlock_rdunlock(&g_lock);
    return v ? (const char*)v : (const char*)string_empty();
}

const char* aether_config_store_get_or(const char* key, const char* default_value) {
    if (!key) return (const char*)string_new(default_value ? default_value : "");
    uint32_t h = fnv1a(key);
    int idx = (int)(h & (AE_CFG_BUCKETS - 1));

    ae_cfg_rwlock_rdlock(&g_lock);
    ae_cfg_entry* e = find_in_bucket(g_buckets[idx], key);
    AetherString* v = NULL;
    if (e) {
        v = e->value;
        string_retain(v);
    }
    ae_cfg_rwlock_rdunlock(&g_lock);
    if (v) return (const char*)v;
    return (const char*)string_new(default_value ? default_value : "");
}

int aether_config_store_has(const char* key) {
    if (!key) return 0;
    uint32_t h = fnv1a(key);
    int idx = (int)(h & (AE_CFG_BUCKETS - 1));

    ae_cfg_rwlock_rdlock(&g_lock);
    ae_cfg_entry* e = find_in_bucket(g_buckets[idx], key);
    int result = e ? 1 : 0;
    ae_cfg_rwlock_rdunlock(&g_lock);
    return result;
}

int aether_config_store_size(void) {
    ae_cfg_rwlock_rdlock(&g_lock);
    int n = g_size;
    ae_cfg_rwlock_rdunlock(&g_lock);
    return n;
}

void aether_config_store_clear(void) {
    /* Unlink every chain under the lock, release outside it: a value's last
     * reference may be a reader's, and either way the lock is not held over
     * the frees. */
    ae_cfg_entry* chains[AE_CFG_BUCKETS];
    ae_cfg_rwlock_wrlock(&g_lock);
    for (int i = 0; i < AE_CFG_BUCKETS; i++) {
        chains[i] = g_buckets[i];
        g_buckets[i] = NULL;
    }
    g_size = 0;
    ae_cfg_rwlock_wrunlock(&g_lock);
    for (int i = 0; i < AE_CFG_BUCKETS; i++) {
        ae_cfg_entry* e = chains[i];
        while (e) {
            ae_cfg_entry* next = e->next;
            aether_caps_free(e->key, strlen(e->key) + 1);
            string_release(e->value);
            aether_caps_free(e, sizeof(ae_cfg_entry));
            e = next;
        }
    }
}
