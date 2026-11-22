// aether_shared_map.c — Token-guarded map of byte strings for host module data exchange
//
// Aether creates the map, puts input values, passes the token to the hosted
// language. The hosted language reads/writes through the token. After the
// hosted code returns, the token is revoked — the map is inaccessible from
// the hosted language side.
//
// Keys and values carry their length (aether_shared_map.h): a NUL in one is
// a byte, in the map and in the shm block, whose layout 2 is length-prefixed.

#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S   /* rand_s, for the token (generate_token) */
#endif
#include "aether_shared_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#if defined(__linux__)
#include <sys/random.h>   /* getrandom, for the token */
#endif
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

#define MAX_ENTRIES AETHER_SHARED_MAP_MAX_ENTRIES
#define MAX_ACTIVE_MAPS 16

typedef struct {
    char* key;          // key_len bytes, then a NUL
    size_t key_len;
    char* value;        // value_len bytes, then a NUL
    size_t value_len;
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

// A map's token is the guard on it: a hosted language reaches the map only
// through its token, and revoking the token is what shuts it out. So two live
// maps must never share one, and one map's token must not be guessable from
// another's. The token used to come from the monotonic clock mixed with a
// stack address: two maps made within one clock tick (a microsecond on macOS)
// from the same call depth got the same token, find_by_token returned the
// first, and revoking the second revoked the first. Now: 64 bits from the OS
// random source, drawn again while any registered map (live or revoked) holds
// them. The fallback, if the OS source fails, is a process counter mixed with
// the clock, still unique by the registry check.
static int os_random64(uint64_t* out) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, sizeof *out);
    return 1;
#elif defined(__linux__)
    size_t got = 0;
    unsigned char* b = (unsigned char*)out;
    while (got < sizeof *out) {
        ssize_t r = getrandom(b + got, sizeof *out - got, 0);
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        got += (size_t)r;
    }
    return 1;
#elif defined(_WIN32)
    unsigned int hi = 0, lo = 0;
    if (rand_s(&hi) != 0 || rand_s(&lo) != 0) return 0;
    *out = ((uint64_t)hi << 32) | lo;
    return 1;
#else
    (void)out;
    return 0;
#endif
}

static int token_registered(uint64_t token) {
    for (int i = 0; i < active_map_count; i++) {
        if (active_maps[i] && active_maps[i]->token == token) return 1;
    }
    return 0;
}

static uint64_t generate_token(void) {
    static uint64_t fallback_counter = 0;
    uint64_t t;
    do {
        if (!os_random64(&t)) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t = ++fallback_counter * 0x9e3779b97f4a7c15ULL;
            t ^= (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32);
            t ^= t >> 31;
        }
    } while (t == 0 || token_registered(t));
    return t;
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

uint64_t aether_shared_map_token(AetherSharedMap* map) { return map ? map->token : 0; }

// n bytes, then a NUL, in a new buffer; NULL without memory.
static char* dup_bytes(const char* d, size_t n) {
    char* b = malloc(n + 1);
    if (!b) return NULL;
    if (n) memcpy(b, d, n);
    b[n] = '\0';
    return b;
}

int aether_shared_map_find(AetherSharedMap* map, const char* key, size_t key_len) {
    if (!map || (!key && key_len)) return -1;
    for (int i = 0; i < map->count; i++) {
        const MapEntry* e = &map->entries[i];
        if (e->key_len == key_len && (key_len == 0 || memcmp(e->key, key, key_len) == 0)) return i;
    }
    return -1;
}

int aether_shared_map_put(AetherSharedMap* map, const char* key, size_t key_len,
                          const char* value, size_t value_len) {
    if (!map || !key) return -1;
    if (!value) { value = ""; value_len = 0; }
    char* vdup = dup_bytes(value, value_len);
    if (!vdup) return -1;

    // Update existing key
    int i = aether_shared_map_find(map, key, key_len);
    if (i >= 0) {
        free(map->entries[i].value);
        map->entries[i].value = vdup;
        map->entries[i].value_len = value_len;
        return 0;
    }

    // Add new entry; on failure the map is unchanged.
    if (map->count >= MAX_ENTRIES) { free(vdup); return -1; }
    char* kdup = dup_bytes(key, key_len);
    if (!kdup) { free(vdup); return -1; }
    MapEntry* e = &map->entries[map->count];
    e->key = kdup;
    e->key_len = key_len;
    e->value = vdup;
    e->value_len = value_len;
    map->count++;
    return 0;
}

const char* aether_shared_map_get(AetherSharedMap* map, const char* key, size_t key_len,
                                  size_t* value_len) {
    if (value_len) *value_len = 0;
    int i = aether_shared_map_find(map, key, key_len);
    if (i < 0) return NULL;
    if (value_len) *value_len = map->entries[i].value_len;
    return map->entries[i].value;
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

const char* aether_shared_map_get_by_token(uint64_t token, const char* key, size_t key_len,
                                           size_t* value_len) {
    if (value_len) *value_len = 0;
    AetherSharedMap* map = find_by_token(token);
    if (!map) return NULL;
    return aether_shared_map_get(map, key, key_len, value_len);
}

int aether_shared_map_put_by_token(uint64_t token, const char* key, size_t key_len,
                                   const char* value, size_t value_len) {
    AetherSharedMap* map = find_by_token(token);
    if (!map || !key) return -1;
    // Reject writes to frozen input keys
    int i = aether_shared_map_find(map, key, key_len);
    if (i >= 0 && i < map->frozen_count) return -1;  // input key: read-only
    return aether_shared_map_put(map, key, key_len, value, value_len);
}

void aether_shared_map_revoke_token(uint64_t token) {
    AetherSharedMap* map = find_by_token(token);
    if (map) map->token_valid = 0;
}

void aether_shared_map_freeze_inputs(AetherSharedMap* map) {
    if (map) map->frozen_count = map->count;
}

void aether_shared_map_freeze_inputs_by_token(uint64_t token) {
    aether_shared_map_freeze_inputs(find_by_token(token));
}

int aether_shared_map_count(AetherSharedMap* map) { return map ? map->count : 0; }

const char* aether_shared_map_key_at(AetherSharedMap* map, int i, size_t* len) {
    if (len) *len = 0;
    if (!map || i < 0 || i >= map->count) return NULL;
    if (len) *len = map->entries[i].key_len;
    return map->entries[i].key;
}
const char* aether_shared_map_value_at(AetherSharedMap* map, int i, size_t* len) {
    if (len) *len = 0;
    if (!map || i < 0 || i >= map->count) return NULL;
    if (len) *len = map->entries[i].value_len;
    return map->entries[i].value;
}
int64_t aether_shared_map_key_len_at(AetherSharedMap* map, int i) {
    return (map && i >= 0 && i < map->count) ? (int64_t)map->entries[i].key_len : -1;
}
int64_t aether_shared_map_value_len_at(AetherSharedMap* map, int i) {
    return (map && i >= 0 && i < map->count) ? (int64_t)map->entries[i].value_len : -1;
}
int aether_shared_map_count_by_token(uint64_t token) {
    return aether_shared_map_count(find_by_token(token));
}
const char* aether_shared_map_key_at_by_token(uint64_t token, int i, size_t* len) {
    return aether_shared_map_key_at(find_by_token(token), i, len);
}
const char* aether_shared_map_value_at_by_token(uint64_t token, int i, size_t* len) {
    return aether_shared_map_value_at(find_by_token(token), i, len);
}

// ---- The block, layout 2 (aether_shared_map.h has the table) ----------------

static void put_u32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static uint32_t get_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

unsigned char* aether_shared_map_encode(AetherSharedMap* map, size_t* n) {
    if (n) *n = 0;
    if (!map) { errno = EINVAL; return NULL; }
    size_t total = AETHER_SHARED_MAP_SHM_HEADER;
    for (int i = 0; i < map->count; i++) {
        const MapEntry* e = &map->entries[i];
        // Each length is a uint32 and the whole block is capped far below
        // that, so a too-large entry is caught by the cap.
        if (e->key_len > AETHER_SHARED_MAP_SHM_MAX_WRITE || e->value_len > AETHER_SHARED_MAP_SHM_MAX_WRITE) {
            errno = E2BIG;
            return NULL;
        }
        total += 8 + e->key_len + e->value_len;
        if (total > AETHER_SHARED_MAP_SHM_MAX_WRITE) { errno = E2BIG; return NULL; }
    }
    unsigned char* b = malloc(total);
    if (!b) { errno = ENOMEM; return NULL; }
    memcpy(b, AETHER_SHARED_MAP_SHM_MAGIC, 4);
    put_u32(b + 4, 0);
    put_u32(b + 8, AETHER_SHARED_MAP_SHM_VERSION);
    put_u32(b + 12, (uint32_t)map->frozen_count);
    put_u32(b + 16, (uint32_t)map->count);
    size_t pos = AETHER_SHARED_MAP_SHM_HEADER;
    for (int i = 0; i < map->count; i++) {
        const MapEntry* e = &map->entries[i];
        put_u32(b + pos, (uint32_t)e->key_len);
        put_u32(b + pos + 4, (uint32_t)e->value_len);
        pos += 8;
        if (e->key_len) memcpy(b + pos, e->key, e->key_len);
        pos += e->key_len;
        if (e->value_len) memcpy(b + pos, e->value, e->value_len);
        pos += e->value_len;
    }
    if (n) *n = total;
    return b;
}

int aether_shared_map_decode_outputs(AetherSharedMap* map, const unsigned char* data, size_t n) {
    if (!map || !data) return -1;
    // Refused whole unless it is a layout-2 block: a layout-1 block starts
    // with its frozen count, not the magic, and is never read as one.
    if (n < AETHER_SHARED_MAP_SHM_HEADER || memcmp(data, AETHER_SHARED_MAP_SHM_MAGIC, 4) != 0 ||
        get_u32(data + 4) != 0 || get_u32(data + 8) != AETHER_SHARED_MAP_SHM_VERSION)
        return -1;
    uint32_t count = get_u32(data + 16);
    if (count > MAX_ENTRIES) return -1;
    // Walk it once to check every entry lies inside the block, then apply:
    // a malformed block changes nothing.
    size_t pos = AETHER_SHARED_MAP_SHM_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        if (n - pos < 8) return -1;
        size_t kl = get_u32(data + pos), vl = get_u32(data + pos + 4);
        pos += 8;
        if (kl > n - pos) return -1;
        pos += kl;
        if (vl > n - pos) return -1;
        pos += vl;
    }
    // After the last entry only zero bytes: macOS rounds a shm object's size
    // up to a page and fills it with zeros. Anything else is not a block a
    // layout-2 writer made.
    for (size_t z = pos; z < n; z++) if (data[z] != 0) return -1;
    pos = AETHER_SHARED_MAP_SHM_HEADER;
    int rc = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t kl = get_u32(data + pos), vl = get_u32(data + pos + 4);
        const char* k = (const char*)data + pos + 8;
        const char* v = k + kl;
        pos += 8 + kl + vl;
        // The map's own frozen inputs stay as the host put them, whatever
        // the block (which a hosted process wrote) says about them.
        int at = aether_shared_map_find(map, k, kl);
        if (at >= 0 && at < map->frozen_count) continue;
        if (aether_shared_map_put(map, k, kl, v, vl) != 0) rc = -1;
    }
    return rc;
}

// --- Cross-process shared memory (Linux/macOS only) ---
// Android's bionic has no POSIX shared memory (shm_open), so it takes the
// stubs below, as Windows and the BSDs do.
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)

char* aether_shared_map_to_shm(AetherSharedMap* map) {
    if (!map) return NULL;
    size_t pos = 0;
    unsigned char* buf = aether_shared_map_encode(map, &pos);
    if (!buf) return NULL;

    // Write to shared memory
    // At most 31 characters: macOS refuses a longer shm name
    // (ENAMETOOLONG, PSHMNAMLEN), and "/aether_map_<pid>_<token in
    // decimal>" was up to 43, so a map never reached a child there. The pid
    // and the token in hex: 29 at most, unique by the pid across processes
    // and by the token (drawn unique, aether_shared_map_new) within one.
    char* shm_name = malloc(32);
    if (!shm_name) { free(buf); return NULL; }
    snprintf(shm_name, 32, "/aem%x_%016llx", (unsigned)getpid(), (unsigned long long)map->token);

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { free(buf); free(shm_name); return NULL; }

    if (ftruncate(fd, (off_t)pos) != 0) {
        close(fd);
        shm_unlink(shm_name);
        free(buf);
        free(shm_name);
        return NULL;
    }
    void* mem = mmap(NULL, pos, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); shm_unlink(shm_name); free(buf); free(shm_name); return NULL; }

    memcpy(mem, buf, pos);
    munmap(mem, pos);
    close(fd);
    free(buf);

    return shm_name;
}

int aether_shared_map_read_outputs_from_shm(AetherSharedMap* map, const char* shm_name) {
    if (!map || !shm_name) return -1;

    int fd = shm_open(shm_name, O_RDONLY, 0);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < AETHER_SHARED_MAP_SHM_HEADER ||
        st.st_size > AETHER_SHARED_MAP_SHM_MAX_READ) {
        close(fd);
        return -1;
    }
    size_t n = (size_t)st.st_size;
    void* data = mmap(NULL, n, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return -1;
    // The hosted process can still write to the block while it is read, so
    // it is copied first and the copy checked and read.
    unsigned char* copy = malloc(n);
    int rc = -1;
    if (copy) {
        memcpy(copy, data, n);
        rc = aether_shared_map_decode_outputs(map, copy, n);
        free(copy);
    }
    munmap(data, n);
    return rc;
}

void aether_shared_map_unlink_shm(const char* shm_name) {
    if (shm_name) shm_unlink(shm_name);
}

char* aether_shared_map_to_shm_by_token(uint64_t token) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return NULL;
    return aether_shared_map_to_shm(map);
}

int aether_shared_map_read_outputs_from_shm_by_token(uint64_t token, const char* shm_name) {
    AetherSharedMap* map = find_by_token(token);
    if (!map) return -1;
    return aether_shared_map_read_outputs_from_shm(map, shm_name);
}
#else
// No POSIX shared memory block here: the map works in process, and
// encode/decode give the block's bytes; to_shm says ENOSYS.
char* aether_shared_map_to_shm(AetherSharedMap* map) { (void)map; errno = ENOSYS; return NULL; }
int aether_shared_map_read_outputs_from_shm(AetherSharedMap* map, const char* shm_name) { (void)map; (void)shm_name; return -1; }
void aether_shared_map_unlink_shm(const char* shm_name) { (void)shm_name; }
char* aether_shared_map_to_shm_by_token(uint64_t token) { (void)token; errno = ENOSYS; return NULL; }
int aether_shared_map_read_outputs_from_shm_by_token(uint64_t token, const char* shm_name) { (void)token; (void)shm_name; return -1; }
#endif
