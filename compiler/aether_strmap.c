#include "aether_strmap.h"
#include <stdlib.h>
#include <string.h>

unsigned strmap_hash(const char* s) {
    unsigned h = 2166136261u;                 /* FNV-1a */
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void strmap_init(StrMap* m) {
    memset(m, 0, sizeof(*m));
}

void strmap_free(StrMap* m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) free(m->entries[i].key);
    free(m->entries);
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

/* The slot holding `key`, or the empty slot where it would go. */
static int strmap_probe(const StrMap* m, const char* key) {
    unsigned mask = (unsigned)(m->slot_count - 1);
    unsigned i = strmap_hash(key) & mask;
    while (m->slots[i]) {
        if (strcmp(m->entries[m->slots[i] - 1].key, key) == 0) return (int)i;
        i = (i + 1) & mask;
    }
    return (int)i;
}

static void strmap_rehash(StrMap* m, int slot_count) {
    int* slots = calloc((size_t)slot_count, sizeof(int));
    if (!slots) return;                       /* keep the old table: still correct, just fuller */
    free(m->slots);
    m->slots = slots;
    m->slot_count = slot_count;
    for (int k = 0; k < m->count; k++) {
        int i = strmap_probe(m, m->entries[k].key);
        m->slots[i] = k + 1;
    }
}

void* strmap_get(const StrMap* m, const char* key) {
    if (!m || !key || !m->slots) return NULL;
    int i = strmap_probe(m, key);
    return m->slots[i] ? m->entries[m->slots[i] - 1].value : NULL;
}

int strmap_has(const StrMap* m, const char* key) {
    if (!m || !key || !m->slots) return 0;
    return m->slots[strmap_probe(m, key)] != 0;
}

void* strmap_put(StrMap* m, const char* key, void* value) {
    if (!m || !key) return NULL;
    if (m->slots) {
        int i = strmap_probe(m, key);
        if (m->slots[i]) {
            StrMapEntry* e = &m->entries[m->slots[i] - 1];
            void* old = e->value;
            e->value = value;
            return old;
        }
    }
    if (m->count >= m->capacity) {
        int cap = m->capacity ? m->capacity * 2 : 32;
        StrMapEntry* ne = realloc(m->entries, sizeof(StrMapEntry) * (size_t)cap);
        if (!ne) return NULL;
        m->entries = ne;
        m->capacity = cap;
    }
    char* dup = strdup(key);
    if (!dup) return NULL;
    m->entries[m->count].key = dup;
    m->entries[m->count].value = value;
    m->count++;
    if (!m->slots || m->count * 2 > m->slot_count) {
        strmap_rehash(m, m->slot_count ? m->slot_count * 2 : 64);
        if (!m->slots) { m->count--; free(dup); return NULL; }
    } else {
        int i = strmap_probe(m, key);
        m->slots[i] = m->count;
    }
    return NULL;
}

int strmap_count(const StrMap* m) { return m ? m->count : 0; }
const char* strmap_key_at(const StrMap* m, int i) { return m->entries[i].key; }
void* strmap_value_at(const StrMap* m, int i) { return m->entries[i].value; }
