/* aether_intmap.c — a hash map with integer keys and values (#1986).
 * The design is described in aether_intmap.h. */
#include "aether_intmap.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

enum { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_TOMB = 2 };

struct AetherIntMap {
    int64_t* keys;
    int64_t* vals;
    uint8_t* state;
    size_t cap;        /* a power of two */
    size_t count;      /* live entries */
    size_t tombs;      /* removed entries still occupying a probe chain */
    int add_failed;    /* the last add could not allocate */
};

static uint64_t spread(int64_t key) {
    uint64_t z = (uint64_t)key + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int alloc_table(AetherIntMap* m, size_t cap) {
    int64_t* keys = (int64_t*)aether_caps_malloc(cap * sizeof(int64_t));
    int64_t* vals = (int64_t*)aether_caps_malloc(cap * sizeof(int64_t));
    uint8_t* state = (uint8_t*)aether_caps_calloc(cap, 1);
    if (!keys || !vals || !state) {
        if (keys) aether_caps_free(keys, cap * sizeof(int64_t));
        if (vals) aether_caps_free(vals, cap * sizeof(int64_t));
        if (state) aether_caps_free(state, cap);
        return 0;
    }
    m->keys = keys;
    m->vals = vals;
    m->state = state;
    m->cap = cap;
    m->count = 0;
    m->tombs = 0;
    return 1;
}

static void free_table(int64_t* keys, int64_t* vals, uint8_t* state, size_t cap) {
    aether_caps_free(keys, cap * sizeof(int64_t));
    aether_caps_free(vals, cap * sizeof(int64_t));
    aether_caps_free(state, cap);
}

/* The slot holding `key`, or the slot to insert it at (the first tombstone
 * on its chain when there is one). `*found` says which. */
static size_t probe(const AetherIntMap* m, int64_t key, int* found) {
    size_t mask = m->cap - 1;
    size_t i = (size_t)spread(key) & mask;
    size_t first_tomb = (size_t)-1;
    for (;;) {
        uint8_t st = m->state[i];
        if (st == SLOT_EMPTY) {
            *found = 0;
            return first_tomb != (size_t)-1 ? first_tomb : i;
        }
        if (st == SLOT_FULL && m->keys[i] == key) {
            *found = 1;
            return i;
        }
        if (st == SLOT_TOMB && first_tomb == (size_t)-1) first_tomb = i;
        i = (i + 1) & mask;
    }
}

/* Rebuild into a table of `cap` slots, dropping tombstones. */
static int rebuild(AetherIntMap* m, size_t cap) {
    int64_t* old_keys = m->keys;
    int64_t* old_vals = m->vals;
    uint8_t* old_state = m->state;
    size_t old_cap = m->cap;
    if (!alloc_table(m, cap)) {
        m->keys = old_keys; m->vals = old_vals; m->state = old_state; m->cap = old_cap;
        return 0;
    }
    for (size_t i = 0; i < old_cap; i++) {
        if (old_state[i] != SLOT_FULL) continue;
        int found;
        size_t j = probe(m, old_keys[i], &found);
        m->keys[j] = old_keys[i];
        m->vals[j] = old_vals[i];
        m->state[j] = SLOT_FULL;
        m->count++;
    }
    free_table(old_keys, old_vals, old_state, old_cap);
    return 1;
}

/* Make room for one more entry before an insert. */
static int reserve_one(AetherIntMap* m) {
    if ((m->count + m->tombs + 1) * 10 <= m->cap * 7) return 1;
    size_t cap = m->cap;
    if ((m->count + 1) * 2 > cap) {
        if (cap > ((size_t)-1) / 2) return 0;
        cap *= 2;
    }
    return rebuild(m, cap);
}

AetherIntMap* aether_intmap_new(int capacity_hint) {
    AetherIntMap* m = (AetherIntMap*)aether_caps_calloc(1, sizeof(AetherIntMap));
    if (!m) return NULL;
    size_t cap = 16;
    size_t want = capacity_hint > 0 ? (size_t)capacity_hint : 0;
    while (cap * 7 < want * 10) cap *= 2;   /* room for the hint under 70% */
    if (!alloc_table(m, cap)) {
        aether_caps_free(m, sizeof(AetherIntMap));
        return NULL;
    }
    return m;
}

void aether_intmap_free(AetherIntMap* m) {
    if (!m) return;
    free_table(m->keys, m->vals, m->state, m->cap);
    aether_caps_free(m, sizeof(AetherIntMap));
}

int aether_intmap_put(AetherIntMap* m, int64_t key, int64_t value) {
    if (!m) return -1;
    int found;
    size_t i = probe(m, key, &found);
    if (found) {
        m->vals[i] = value;
        return 0;
    }
    if (!reserve_one(m)) return -1;
    i = probe(m, key, &found);
    if (m->state[i] == SLOT_TOMB) m->tombs--;
    m->keys[i] = key;
    m->vals[i] = value;
    m->state[i] = SLOT_FULL;
    m->count++;
    return 1;
}

int64_t aether_intmap_get(AetherIntMap* m, int64_t key, int64_t missing) {
    if (!m) return missing;
    int found;
    size_t i = probe(m, key, &found);
    return found ? m->vals[i] : missing;
}

int aether_intmap_has(AetherIntMap* m, int64_t key) {
    if (!m) return 0;
    int found;
    probe(m, key, &found);
    return found;
}

int64_t aether_intmap_add(AetherIntMap* m, int64_t key, int64_t delta) {
    if (!m) return 0;
    m->add_failed = 0;
    int found;
    size_t i = probe(m, key, &found);
    if (!found) {
        if (!reserve_one(m)) {
            m->add_failed = 1;
            return INT64_MIN;
        }
        i = probe(m, key, &found);
        if (m->state[i] == SLOT_TOMB) m->tombs--;
        m->keys[i] = key;
        m->vals[i] = 0;
        m->state[i] = SLOT_FULL;
        m->count++;
    }
    m->vals[i] += delta;
    return m->vals[i];
}

int aether_intmap_add_failed(AetherIntMap* m) {
    return m ? m->add_failed : 0;
}

int aether_intmap_remove(AetherIntMap* m, int64_t key) {
    if (!m) return 0;
    int found;
    size_t i = probe(m, key, &found);
    if (!found) return 0;
    m->state[i] = SLOT_TOMB;
    m->count--;
    m->tombs++;
    return 1;
}

int aether_intmap_size(AetherIntMap* m) {
    return m ? (int)m->count : 0;
}

void aether_intmap_clear(AetherIntMap* m) {
    if (!m) return;
    memset(m->state, SLOT_EMPTY, m->cap);
    m->count = 0;
    m->tombs = 0;
}

int aether_intmap_next(AetherIntMap* m, int slot) {
    if (!m || slot < 0) return -1;
    for (size_t i = (size_t)slot; i < m->cap; i++) {
        if (m->state[i] == SLOT_FULL) return (int)i;
    }
    return -1;
}

int64_t aether_intmap_key_at(AetherIntMap* m, int slot) {
    if (!m || slot < 0 || (size_t)slot >= m->cap || m->state[slot] != SLOT_FULL) return 0;
    return m->keys[slot];
}

int64_t aether_intmap_value_at(AetherIntMap* m, int slot) {
    if (!m || slot < 0 || (size_t)slot >= m->cap || m->state[slot] != SLOT_FULL) return 0;
    return m->vals[slot];
}
