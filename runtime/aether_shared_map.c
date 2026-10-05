// aether_shared_map.c — Token-guarded string:string map for host module data exchange
//
// Aether creates the map, puts input values, passes the token to the hosted
// language. The hosted language reads/writes through the token. After the
// hosted code returns, the token is revoked — the map is inaccessible from
// the hosted language side.

#include "aether_shared_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#elif defined(_WIN32)
/* generate_token() below uses clock_gettime(CLOCK_MONOTONIC); Windows has no
 * POSIX clocks, but the thread shim supplies both via QueryPerformanceCounter.
 * (The shm map itself is a POSIX-only stub on Windows — see the guards below.) */
#include "utils/aether_thread.h"
#endif

#define MAX_ENTRIES 256
#define MAX_ACTIVE_MAPS 16

typedef struct {
    char* key;
    char* value;
} MapEntry;

struct AetherSharedMap {
    MapEntry entries[MAX_ENTRIES];
    int count;
    int frozen_count;   // entries 0..frozen_count-1 are inputs (read-only to hosted code)
    uint64_t token;
    int token_valid;
};

// Global registry of active maps (for token lookup)
static AetherSharedMap* active_maps[MAX_ACTIVE_MAPS];
static int active_map_count = 0;

// Simple random token
static uint64_t generate_token(void) {
    uint64_t t = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32);
    t ^= (uint64_t)(uintptr_t)&t;  // mix in stack address for entropy
    t ^= t >> 17;
    t *= 0xbf58476d1ce4e5b9ULL;
    t ^= t >> 31;
    return t | 1;  // never zero
}

static AetherSharedMap* find_by_token(uint64_t token) {
    if (token == 0) return NULL;
    for (int i = 0; i < active_map_count; i++) {
        if (active_maps[i] && active_maps[i]->token == token &&
            active_maps[i]->token_valid) {
            return active_maps[i];
        }
    }
    return NULL;
}

AetherSharedMap* aether_shared_map_new(uint64_t* out_token) {
    AetherSharedMap* map = calloc(1, sizeof(AetherSharedMap));
    if (!map) return NULL;

    map->token = generate_token();
    map->token_valid = 1;
    map->count = 0;

    // Register in active maps
    if (active_map_count < MAX_ACTIVE_MAPS) {
        active_maps[active_map_count++] = map;
    }

    if (out_token) *out_token = map->token;
    return map;
}

void aether_shared_map_put(AetherSharedMap* map, const char* key, const char* value) {
    if (!map || !key) return;

    // Update existing key
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            free(map->entries[i].value);
            map->entries[i].value = value ? strdup(value) : NULL;
            return;
        }
    }

    // Add new entry. A NULL key (allocation failure) would crash the strcmp in
    // every later get/put, so on failure leave the map unchanged. A NULL value
    // is fine (get legitimately returns NULL for a present key with no value).
    if (map->count >= MAX_ENTRIES) return;
    char* kdup = strdup(key);
    if (!kdup) return;
    map->entries[map->count].key = kdup;
    map->entries[map->count].value = value ? strdup(value) : NULL;
    map->count++;
}

const char* aether_shared_map_get(AetherSharedMap* map, const char* key) {
    if (!map || !key) return NULL;
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return map->entries[i].value;
        }
    }
    return NULL;
}

void aether_shared_map_free(AetherSharedMap* map) {
    if (!map) return;

    // Remove from active registry
    for (int i = 0; i < active_map_count; i++) {
        if (active_maps[i] == map) {
            active_maps[i] = active_maps[--active_map_count];
            break;
        }
    }

    // Free entries
    for (int i = 0; i < map->count; i++) {
        free(map->entries[i].key);
        free(map->entries[i].value);
    }
    free(map);
}

// Token-guarded access (called from hosted language native bindings)

const char* aether_shared_map_get_by_token(uint64_t token, const char* key) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return NULL;
    return aether_shared_map_get(map, key);
}

int aether_shared_map_put_by_token(uint64_t token, const char* key, const char* value) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return -1;
    // Reject writes to frozen input keys
    for (int i = 0; i < map->frozen_count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return -1;  // input key — read-only
        }
    }
    aether_shared_map_put(map, key, value);
    return 0;
}

void aether_shared_map_revoke_token(uint64_t token) {
    AetherSharedMap* map = find_by_token(token);
    if (map) map->token_valid = 0;
}

void aether_shared_map_freeze_inputs(AetherSharedMap* map) {
    if (map) map->frozen_count = map->count;
}

int aether_shared_map_count(AetherSharedMap* map) { return map ? map->count : 0; }
const char* aether_shared_map_key_at(AetherSharedMap* map, int i) {
    return (map && i >= 0 && i < map->count) ? map->entries[i].key : NULL;
}
const char* aether_shared_map_value_at(AetherSharedMap* map, int i) {
    return (map && i >= 0 && i < map->count) ? map->entries[i].value : NULL;
}
int aether_shared_map_count_by_token(uint64_t token) {
    AetherSharedMap* m = find_by_token(token); return m ? m->count : 0;
}
const char* aether_shared_map_key_at_by_token(uint64_t token, int i) {
    AetherSharedMap* m = find_by_token(token);
    return (m && i >= 0 && i < m->count) ? m->entries[i].key : NULL;
}
const char* aether_shared_map_value_at_by_token(uint64_t token, int i) {
    AetherSharedMap* m = find_by_token(token);
    return (m && i >= 0 && i < m->count) ? m->entries[i].value : NULL;
}

void aether_shared_map_freeze_inputs_by_token(uint64_t token) {
    AetherSharedMap* map = find_by_token(token);
    if (map) map->frozen_count = map->count;
}

// --- Cross-process shared memory (Linux/macOS only) ---
// Format: frozen_count(4 bytes) + entries as key\0value\0...key\0value\0\0
// Android's bionic has no POSIX shared memory (shm_open), so it takes the
// stubs below, as Windows does.
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)

char* aether_shared_map_to_shm(AetherSharedMap* map) {
    if (!map) return NULL;

    // Build buffer
    char buf[65536];
    int pos = 0;

    // First 4 bytes: frozen_count (so Java knows which keys are inputs)
    buf[pos++] = (map->frozen_count >> 0) & 0xff;
    buf[pos++] = (map->frozen_count >> 8) & 0xff;
    buf[pos++] = (map->frozen_count >> 16) & 0xff;
    buf[pos++] = (map->frozen_count >> 24) & 0xff;

    for (int i = 0; i < map->count && pos < 65000; i++) {
        const char* k = map->entries[i].key;
        const char* v = map->entries[i].value;
        if (!k) continue;
        int klen = strlen(k);
        int vlen = v ? strlen(v) : 0;
        memcpy(buf + pos, k, klen + 1); pos += klen + 1;  // key + null
        if (v) { memcpy(buf + pos, v, vlen + 1); pos += vlen + 1; }
        else { buf[pos++] = '\0'; }
    }
    buf[pos++] = '\0';  // double-null terminator

    // Write to shared memory
    char* shm_name = malloc(64);
    if (!shm_name) return NULL;
    snprintf(shm_name, 64, "/aether_map_%d_%llu", getpid(), (unsigned long long)map->token);

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { free(shm_name); return NULL; }

    if (ftruncate(fd, pos) != 0) {
        close(fd);
        shm_unlink(shm_name);
        free(shm_name);
        return NULL;
    }
    void* mem = mmap(NULL, pos, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); shm_unlink(shm_name); free(shm_name); return NULL; }

    memcpy(mem, buf, pos);
    munmap(mem, pos);
    close(fd);

    return shm_name;
}

void aether_shared_map_read_outputs_from_shm(AetherSharedMap* map, const char* shm_name) {
    if (!map || !shm_name) return;

    int fd = shm_open(shm_name, O_RDONLY, 0);
    if (fd < 0) return;

    struct stat st;
    fstat(fd, &st);
    if (st.st_size <= 4) { close(fd); return; }

    char* data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return; }

    // Skip frozen_count header
    int frozen = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    int pos = 4;

    // Parse key\0value\0 pairs — skip first 'frozen' entries (inputs),
    // read the rest as outputs written by Java
    int entry_idx = 0;
    while (pos < st.st_size - 1 && data[pos] != '\0') {
        const char* k = data + pos;
        pos += strlen(k) + 1;
        const char* v = data + pos;
        pos += strlen(v) + 1;

        if (entry_idx >= frozen) {
            // This is an output — add to map
            aether_shared_map_put(map, k, v);
        }
        entry_idx++;
    }

    munmap(data, st.st_size);
    close(fd);
}

void aether_shared_map_unlink_shm(const char* shm_name) {
    if (shm_name) shm_unlink(shm_name);
}

char* aether_shared_map_to_shm_by_token(uint64_t token) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return NULL;
    return aether_shared_map_to_shm(map);
}

void aether_shared_map_read_outputs_from_shm_by_token(uint64_t token, const char* shm_name) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return;
    aether_shared_map_read_outputs_from_shm(map, shm_name);
}
#else
// Non-POSIX stubs
char* aether_shared_map_to_shm(AetherSharedMap* map) { (void)map; return NULL; }
void aether_shared_map_read_outputs_from_shm(AetherSharedMap* map, const char* shm_name) { (void)map; (void)shm_name; }
void aether_shared_map_unlink_shm(const char* shm_name) { (void)shm_name; }
char* aether_shared_map_to_shm_by_token(uint64_t token) { (void)token; return NULL; }
void aether_shared_map_read_outputs_from_shm_by_token(uint64_t token, const char* shm_name) { (void)token; (void)shm_name; }
#endif
