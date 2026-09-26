/* Copyright (c) 2026 Aether Developers. */
/* Observable struct models (#2220). See aether_observe.h for the contract.
 *
 * Storage is one process-wide open-addressing hash table keyed by object
 * address; each slot owns a small growable array of observers. A global
 * count of registered observers is the fast path: while it is zero,
 * aether_observe_notify returns after one load, so a program that declares
 * an @observable struct but never observes anything pays nothing measurable
 * per store.
 *
 * Locking: one mutex guards the table. A notification pass copies the
 * object's observer list under the lock and runs the closures with the lock
 * released, so an observer may observe / unobserve (this object or another)
 * without deadlocking. Removal during a pass is deferred: the entry is
 * marked dead, skipped by later passes, and its environment is released by
 * the pass that finds it once notification has finished. */
#include "aether_observe.h"

#include <stdlib.h>
#include <string.h>
#include "utils/aether_thread.h"

/* Live heap and stack addresses are never 0 or 1, so those double as the
 * empty and tombstone slot sentinels (same trick as std/tracking). */
#define OBS_EMPTY ((void*)0)
#define OBS_TOMB  ((void*)1)

typedef struct {
    long token;
    AetherObserverClosure cb;
    int dead;            /* unobserved while a pass on this object was running */
} Observer;

typedef struct {
    void*     key;
    Observer* items;
    int       count;
    int       cap;
    int       notifying;  /* re-entrancy guard, per object */
    int       pending_dead;
} Slot;

static Slot*  g_slots;
static size_t g_cap;
static size_t g_used;     /* live + tombstones */
static size_t g_live;
static long   g_next_token = 1;
static int    g_total_observers;   /* fast-path gate for notify */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* The gate is read without the lock in aether_observe_notify, so it is
 * kept with relaxed atomics where the compiler offers them. */
#if defined(__GNUC__) || defined(__clang__)
static int  obs_total_load(void) { return __atomic_load_n(&g_total_observers, __ATOMIC_RELAXED); }
static void obs_total_add(int d)  { __atomic_add_fetch(&g_total_observers, d, __ATOMIC_RELAXED); }
#else
static int  obs_total_load(void) { return g_total_observers; }
static void obs_total_add(int d)  { g_total_observers += d; }
#endif

extern void aether_closure_env_free(void* env);

static size_t obs_hash(void* p, size_t cap) {
    size_t h = (size_t)p;
    h ^= h >> 33;
    h *= (size_t)0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h & (cap - 1);
}

static Slot* obs_find(void* key) {
    if (!g_cap) return NULL;
    size_t i = obs_hash(key, g_cap);
    for (size_t n = 0; n < g_cap; n++) {
        Slot* s = &g_slots[i];
        if (s->key == OBS_EMPTY) return NULL;
        if (s->key == key) return s;
        i = (i + 1) & (g_cap - 1);
    }
    return NULL;
}

static Slot* obs_place(Slot* slots, size_t cap, void* key) {
    size_t i = obs_hash(key, cap);
    Slot* tomb = NULL;
    for (;;) {
        Slot* s = &slots[i];
        if (s->key == OBS_EMPTY) return tomb ? tomb : s;
        if (s->key == OBS_TOMB) { if (!tomb) tomb = s; }
        else if (s->key == key) return s;
        i = (i + 1) & (cap - 1);
    }
}

static int obs_grow(void) {
    size_t new_cap = g_cap ? g_cap * 2 : 16;
    Slot* fresh = (Slot*)calloc(new_cap, sizeof(Slot));
    if (!fresh) return 0;
    for (size_t i = 0; i < g_cap; i++) {
        Slot* s = &g_slots[i];
        if (s->key == OBS_EMPTY || s->key == OBS_TOMB) continue;
        *obs_place(fresh, new_cap, s->key) = *s;
    }
    free(g_slots);
    g_slots = fresh;
    g_cap = new_cap;
    g_used = g_live;
    return 1;
}

/* Find-or-insert the slot for `key`. Returns NULL on allocation failure. */
static Slot* obs_slot_for(void* key) {
    Slot* s = obs_find(key);
    if (s) return s;
    if ((g_used + 1) * 4 > g_cap * 3 && !obs_grow()) return NULL;
    s = obs_place(g_slots, g_cap, key);
    if (s->key == OBS_EMPTY) g_used++;
    memset(s, 0, sizeof(*s));
    s->key = key;
    g_live++;
    return s;
}

static void obs_release_slot(Slot* s) {
    free(s->items);
    memset(s, 0, sizeof(*s));
    s->key = OBS_TOMB;
    g_live--;
}

/* Drop dead entries and free their environments. Caller holds the lock and
 * has verified no pass is running on this object. */
static void obs_sweep_dead(Slot* s) {
    int w = 0;
    for (int r = 0; r < s->count; r++) {
        if (s->items[r].dead) {
            aether_closure_env_free(s->items[r].cb.env);
            continue;
        }
        s->items[w++] = s->items[r];
    }
    s->count = w;
    s->pending_dead = 0;
    if (s->count == 0) obs_release_slot(s);
}

long aether_observe(void* obj, AetherObserverClosure cb) {
    if (!obj || !cb.fn) {
        /* The environment is ours from the call on, rejected or not: the
         * caller crossed an extern boundary and will not release it. */
        aether_closure_env_free(cb.env);
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    Slot* s = obs_slot_for(obj);
    long token = 0;
    if (s) {
        if (s->count == s->cap) {
            int new_cap = s->cap ? s->cap * 2 : 4;
            Observer* bigger = (Observer*)realloc(s->items, sizeof(Observer) * (size_t)new_cap);
            if (bigger) { s->items = bigger; s->cap = new_cap; }
        }
        if (s->count < s->cap) {
            token = g_next_token++;
            s->items[s->count].token = token;
            s->items[s->count].cb = cb;
            s->items[s->count].dead = 0;
            s->count++;
            obs_total_add(1);
        } else if (s->count == 0) {
            obs_release_slot(s);
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!token) aether_closure_env_free(cb.env);
    return token;
}

/* Mark or remove one observer. Caller holds the lock. Returns 1 if found. */
static int obs_retire(Slot* s, int idx) {
    if (s->items[idx].dead) return 0;
    obs_total_add(-1);
    if (s->notifying) {
        s->items[idx].dead = 1;
        s->pending_dead = 1;
        return 1;
    }
    aether_closure_env_free(s->items[idx].cb.env);
    memmove(&s->items[idx], &s->items[idx + 1],
            sizeof(Observer) * (size_t)(s->count - idx - 1));
    s->count--;
    if (s->count == 0) obs_release_slot(s);
    return 1;
}

int aether_unobserve(void* obj, long token) {
    if (!obj || token <= 0) return 0;
    int found = 0;
    pthread_mutex_lock(&g_lock);
    Slot* s = obs_find(obj);
    if (s) {
        for (int i = 0; i < s->count; i++) {
            if (s->items[i].token == token) { found = obs_retire(s, i); break; }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int aether_unobserve_all(void* obj) {
    if (!obj) return 0;
    int removed = 0;
    pthread_mutex_lock(&g_lock);
    Slot* s = obs_find(obj);
    if (s) {
        for (int i = 0; i < s->count; i++) {
            if (s->items[i].dead) continue;
            removed++;
            if (s->notifying) {
                s->items[i].dead = 1;
                s->pending_dead = 1;
            } else {
                aether_closure_env_free(s->items[i].cb.env);
            }
        }
        obs_total_add(-removed);
        if (!s->notifying) obs_release_slot(s);
    }
    pthread_mutex_unlock(&g_lock);
    return removed;
}

int aether_observer_count(void* obj) {
    if (!obj) return 0;
    int n = 0;
    pthread_mutex_lock(&g_lock);
    Slot* s = obs_find(obj);
    if (s) {
        for (int i = 0; i < s->count; i++) n += !s->items[i].dead;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int aether_observe_is_notifying(void* obj) {
    if (!obj) return 0;
    pthread_mutex_lock(&g_lock);
    Slot* s = obs_find(obj);
    int r = s ? s->notifying : 0;
    pthread_mutex_unlock(&g_lock);
    return r;
}

void aether_observe_notify(void* obj) {
    /* Fast path: nothing observed anywhere. A stale read here can only miss
     * an observer registered concurrently with this very store, which no
     * caller can distinguish from the store having happened first. */
    if (!obj || obs_total_load() == 0) return;

    pthread_mutex_lock(&g_lock);
    Slot* s = obs_find(obj);
    if (!s || s->notifying || s->count == 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    int n = s->count;
    Observer* snapshot = (Observer*)malloc(sizeof(Observer) * (size_t)n);
    if (!snapshot) { pthread_mutex_unlock(&g_lock); return; }
    memcpy(snapshot, s->items, sizeof(Observer) * (size_t)n);
    s->notifying = 1;
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < n; i++) {
        if (snapshot[i].dead) continue;
        /* An observer removed by an earlier observer in this same pass must
         * not run: re-check its liveness against the table. */
        int live = 1;
        pthread_mutex_lock(&g_lock);
        Slot* cur = obs_find(obj);
        if (cur) {
            live = 0;
            for (int j = 0; j < cur->count; j++) {
                if (cur->items[j].token == snapshot[i].token) { live = !cur->items[j].dead; break; }
            }
        } else {
            live = 0;
        }
        pthread_mutex_unlock(&g_lock);
        if (!live) continue;
        ((void (*)(void*, void*))snapshot[i].cb.fn)(snapshot[i].cb.env, obj);
    }
    free(snapshot);

    pthread_mutex_lock(&g_lock);
    s = obs_find(obj);
    if (s) {
        s->notifying = 0;
        if (s->pending_dead) obs_sweep_dead(s);
    }
    pthread_mutex_unlock(&g_lock);
}
