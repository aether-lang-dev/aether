/* ae_cache.c — build cache: content-hashed keys, publish, GC, and the
 * cache management command (#1221 split). Moved verbatim out of ae.c; the
 * fnv64 hashers and the lib-dir walk stay static to this file, the entry
 * points the driver uses are declared in ae_internal.h.
 */

#include "ae_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <sys/utime.h>
#  define PATH_SEP "\\"
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <utime.h>
#  define PATH_SEP "/"
#endif

// --------------------------------------------------------------------------
// Cache infrastructure
// --------------------------------------------------------------------------



char s_cache_dir[512] = "";

// Portable home-directory lookup.
// On Windows: USERPROFILE (native shell) → HOME (MSYS2) → fallback.
// On POSIX:   HOME → /tmp fallback.
const char* get_home_dir(void) {
#ifdef _WIN32
    const char* h = getenv("USERPROFILE");
    if (!h || !h[0]) h = getenv("HOME");
    return h ? h : "C:\\Users\\Public";
#else
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
#endif
}

/* Atomic cache publish (#1032). Writers produce `<slot>.tmp.<pid>` in the
 * cache directory and move it onto the slot, so a concurrent reader only ever
 * sees a complete file: old, new, or a miss, never a partial one.
 *
 * A taken slot is LEFT ALONE rather than replaced. The key is derived from the
 * sources and the flags, so a slot that already exists holds a binary built
 * from the same inputs: rewriting it is a megabyte of I/O for a file that is
 * already what the writer was about to put there. Under a parallel build every
 * loser of the race did that write. link(2) is the primitive that says "only
 * if absent" and is atomic; a filesystem without hard links (EPERM/EXDEV/
 * ENOSYS: FAT, some network mounts) falls back to the rename this always did. */
int cache_publish(const char* tmp_path, const char* final_path) {
#ifdef _WIN32
    /* Without MOVEFILE_REPLACE_EXISTING this fails when the slot is taken. */
    if (MoveFileExA(tmp_path, final_path, 0)) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) {
        DeleteFileA(tmp_path);
        return 0;
    }
    return -1;
#else
    if (link(tmp_path, final_path) == 0) { unlink(tmp_path); return 0; }
    if (errno == EEXIST) { unlink(tmp_path); return 0; }
    if (errno == EPERM || errno == EXDEV || errno == ENOSYS) {
        return rename(tmp_path, final_path);
    }
    return -1;
#endif
}


/* ---- Size cap ---------------------------------------------------------------
 *
 * The cache had no bound: every cold `ae run` / `ae build` added a static
 * binary (~1-2 MB) and nothing ever left, so a machine that runs the test
 * sweep reached 26,000 entries and 12.7 GB with `ae cache clear` as the only
 * remedy. It is bounded now, ccache-style: AETHER_CACHE_MAX_MB (default
 * 5120, 0 = unlimited) and least-recently-USED eviction. A hit touches the
 * slot's mtime, so "recently used" means what it says rather than "recently
 * built"; after a publish that takes the cache over the cap, the oldest
 * slots go until it is under 90% of it (hysteresis, so one publish does not
 * mean one scan-and-evict per build). The scan is rate-limited by a stamp
 * file to once per ten minutes per cache directory, and the slot just
 * published is never evicted, so a single binary larger than the cap still
 * runs. Depfiles (`*.deps`, keyed by SOURCE path, a few hundred bytes) and
 * in-flight `*.tmp.*` slots are not counted or evicted. */
#define AETHER_CACHE_DEFAULT_MAX_MB 5120ULL

unsigned long long cache_max_bytes(void) {
    const char* v = getenv("AETHER_CACHE_MAX_MB");
    if (v && v[0]) {
        char* end = NULL;
        unsigned long long mb = strtoull(v, &end, 10);
        if (end && *end == '\0') return mb * 1024ULL * 1024ULL;   /* 0 = unlimited */
        fprintf(stderr, "warning: AETHER_CACHE_MAX_MB='%s' is not a number; using %llu\n",
                v, AETHER_CACHE_DEFAULT_MAX_MB);
    }
    return AETHER_CACHE_DEFAULT_MAX_MB * 1024ULL * 1024ULL;
}

void cache_touch(const char* path) {
    if (!path || !path[0]) return;
#ifdef _WIN32
    _utime(path, NULL);
#else
    utime(path, NULL);
#endif
}

void cache_touch_depfile(const char* ae_file) {
    char depfile[1200];
    cache_depfile_path(ae_file, depfile, sizeof(depfile));
    if (depfile[0]) cache_touch(depfile);
}

/* mtime in nanoseconds: the eviction order is least-recently-used, and a
 * fast machine publishes several builds within one second, so a
 * seconds-resolution mtime made their order arbitrary (by name), and the
 * oldest-used build could outlive a newer one. Windows' FILETIME is 100 ns;
 * POSIX stat carries st_mtim (Linux) / st_mtimespec (macOS). */
typedef struct { char name[256]; unsigned long long size; unsigned long long mtime_ns; } CacheSlot;

#ifndef _WIN32
static unsigned long long stat_mtime_ns(const struct stat* st) {
#if defined(__APPLE__)
    return (unsigned long long)st->st_mtimespec.tv_sec * 1000000000ULL +
           (unsigned long long)st->st_mtimespec.tv_nsec;
#else
    return (unsigned long long)st->st_mtim.tv_sec * 1000000000ULL +
           (unsigned long long)st->st_mtim.tv_nsec;
#endif
}
#endif

static int cache_slot_is_countable(const char* name) {
    if (name[0] == '.') return 0;
    if (strstr(name, ".tmp.")) return 0;
    size_t n = strlen(name);
    if (n > 5 && strcmp(name + n - 5, ".deps") == 0) return 0;
    if (strcmp(name, "gc.stamp") == 0 || strcmp(name, "latest_release") == 0) return 0;
    return 1;
}

static int cache_slot_older(const void* a, const void* b) {
    const CacheSlot* x = (const CacheSlot*)a;
    const CacheSlot* y = (const CacheSlot*)b;
    if (x->mtime_ns != y->mtime_ns) return x->mtime_ns < y->mtime_ns ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* Every countable slot with its size and mtime; malloc'd, caller frees. */
static CacheSlot* cache_scan(const char* dir, int* count, unsigned long long* total) {
    CacheSlot* slots = NULL;
    int n = 0, cap = 0;
    *total = 0;
#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { *count = 0; return NULL; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!cache_slot_is_countable(fd.cFileName)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            CacheSlot* grown = (CacheSlot*)realloc(slots, (size_t)cap * sizeof(CacheSlot));
            if (!grown) break;
            slots = grown;
        }
        snprintf(slots[n].name, sizeof(slots[n].name), "%s", fd.cFileName);
        slots[n].size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        /* FILETIME: 100 ns units since 1601; only the order matters. */
        unsigned long long ft = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                                fd.ftLastWriteTime.dwLowDateTime;
        slots[n].mtime_ns = ft * 100ULL;
        *total += slots[n].size;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) { *count = 0; return NULL; }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!cache_slot_is_countable(e->d_name)) continue;
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            CacheSlot* grown = (CacheSlot*)realloc(slots, (size_t)cap * sizeof(CacheSlot));
            if (!grown) break;
            slots = grown;
        }
        snprintf(slots[n].name, sizeof(slots[n].name), "%s", e->d_name);
        slots[n].size = (unsigned long long)st.st_size;
        slots[n].mtime_ns = stat_mtime_ns(&st);
        *total += slots[n].size;
        n++;
    }
    closedir(d);
#endif
    *count = n;
    return slots;
}

static void cache_sweep_stale_depfiles(const char* dir, long max_age) {
    time_t now = time(NULL);
#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*.deps", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        unsigned long long ft = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                                fd.ftLastWriteTime.dwLowDateTime;
        time_t mtime = (time_t)(ft / 10000000ULL - 11644473600ULL);
        if (now - mtime > max_age) {
            char p[1024];
            snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
            remove(p);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n <= 5 || strcmp(e->d_name + n - 5, ".deps") != 0) continue;
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && now - st.st_mtime > max_age) remove(p);
    }
    closedir(d);
#endif
}

/* Bring the cache under its cap by evicting least-recently-used slots.
 * `keep` is the slot just published (never evicted). Returns the number of
 * slots removed. `force` skips the ten-minute stamp gate (`ae cache gc`). */
int cache_enforce_limit(const char* keep, int force) {
    unsigned long long max = cache_max_bytes();
    if (!s_cache_dir[0]) return 0;
    char stamp[600];
    snprintf(stamp, sizeof(stamp), "%s/gc.stamp", s_cache_dir);
    if (!force) {
        struct stat st;
        if (stat(stamp, &st) == 0 && time(NULL) - st.st_mtime < 600) return 0;
    }
    FILE* sf = fopen(stamp, "w");
    if (sf) fclose(sf);

    /* Depfiles are keyed by SOURCE path, so one per entry file ever built
     * through this cache, forever: 3,000 of them had accumulated beside
     * 23,000 builds. A cold build rewrites its entry's depfile and a warm
     * hit touches it, so one untouched for 30 days belongs to a source
     * that has not been built or run in a month; the next build of that
     * source keys on the tree walk once and writes a new one. Runs for an
     * unlimited cache too. */
    cache_sweep_stale_depfiles(s_cache_dir, 30 * 24 * 3600);
    if (max == 0) return 0;

    int count = 0;
    unsigned long long total = 0;
    CacheSlot* slots = cache_scan(s_cache_dir, &count, &total);
    if (!slots) return 0;
    int removed = 0;
    if (total > max) {
        unsigned long long target = max - max / 10;
        qsort(slots, (size_t)count, sizeof(CacheSlot), cache_slot_older);
        const char* keep_name = NULL;
        if (keep) {
            keep_name = strrchr(keep, '/');
            const char* bs = strrchr(keep, '\\');
            if (bs > keep_name) keep_name = bs;
            keep_name = keep_name ? keep_name + 1 : keep;
        }
        for (int i = 0; i < count && total > target; i++) {
            if (keep_name && strcmp(slots[i].name, keep_name) == 0) continue;
            char p[1100];
            snprintf(p, sizeof(p), "%s/%s", s_cache_dir, slots[i].name);
            if (remove(p) == 0) {
                total -= slots[i].size;
                removed++;
            }
        }
    }
    free(slots);
    return removed;
}

/* For `ae cache`: the countable size and slot count. */
void cache_usage(unsigned long long* bytes, int* slots_out) {
    int count = 0;
    unsigned long long total = 0;
    CacheSlot* slots = cache_scan(s_cache_dir, &count, &total);
    free(slots);
    *bytes = total;
    *slots_out = count;
}

// macOS clang runs dsymutil for `-O0 -g` single-step builds, dropping a
// `<exe>.dSYM` BUNDLE (a directory) beside the output. Cache binaries
// don't need debug bundles; delete the temp's bundle so the publish
// leaves no debris (the concurrent-publish test asserts zero `.tmp.*`
// leftovers). No-op where the bundle doesn't exist — every non-macOS
// platform in practice.
void remove_dsym_bundle(const char* exe_path) {
#ifndef _WIN32
    char p[1100];
    snprintf(p, sizeof(p), "%s.dSYM", exe_path);
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        char rm_cmd[1200];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", p);
        if (system(rm_cmd) != 0) { /* best-effort; GC sweeps later */ }
    }
#else
    (void)exe_path;
#endif
}

// Sweep orphaned `*.tmp.<pid>` slots left by crashed/killed writers.
// Age-gated to an hour so we never reap a temp another process is
// actively linking. Runs once per process (from init_cache_dir); a
// directory scan over a few hundred entries is noise next to a compile.
void gc_stale_cache_tmp(const char* dir) {
    time_t now = time(NULL);
#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*.tmp.*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char p[1024];
        snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
        struct stat st;
        if (stat(p, &st) == 0 && now - st.st_mtime > 3600) remove(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strstr(e->d_name, ".tmp.")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || now - st.st_mtime <= 3600) continue;
        if (S_ISDIR(st.st_mode)) {
            // Directory-shaped debris: a macOS `.tmp.<pid>.dSYM` bundle
            // from a crashed writer (remove(2) refuses non-empty dirs).
            char rm_cmd[1200];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", p);
            if (system(rm_cmd) != 0) { /* best-effort */ }
        } else {
            remove(p);
        }
    }
    closedir(d);
#endif
}

void init_cache_dir(void) {
    if (s_cache_dir[0]) return;
    // #1032: per-process override for runners whose $HOME is read-only
    // (agent sandboxes, hermetic CI). AETHER_HOME deliberately does NOT
    // move the cache: it names the (often read-only) toolchain root,
    // while this is a writable artifact directory — two variables, two
    // meanings.
    const char* override = getenv("AETHER_CACHE_DIR");
    if (override && override[0]) {
        snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", override);
    } else {
        const char* home = get_home_dir();
        snprintf(s_cache_dir, sizeof(s_cache_dir), "%s/.aether/cache", home);
    }
    mkdirs(s_cache_dir);
    gc_stale_cache_tmp(s_cache_dir);
}

// FNV-64 hash of a string
static unsigned long long fnv64_str(const char* s) {
    unsigned long long h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

// FNV-64 hash of a file's contents
static unsigned long long fnv64_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned long long h = 14695981039346656037ULL;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    }
    /* CRITICAL: fread returns 0 for both EOF and a read error, so without this
     * an I/O error partway through hashes the bytes read so far and hands that
     * back as if it were the file's hash. This value keys the build cache, and
     * a hash over partial content is exactly how a stale binary gets served.
     * Report it the same way an unopenable file is reported. */
    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return h;
}

/* #1882: the exact-dependency cache key.
 *
 * On a warm run we prefer a depfile aetherc wrote on the previous (cold) build
 * — a manifest of every file it READ and every path it PROBED-AND-MISSED —
 * over walking whole directory trees. Hashing the manifest gives exact
 * invalidation with no duplicated resolver knowledge (Nic's chosen option 3).
 *
 * The depfile lives at a path derived from the ENTRY file's absolute path, so
 * it's stable across content edits (the edit changes a `read` line's hash, not
 * the manifest's location) and found before the content-key is known. */
static void abspath_of(const char* p, char* out, size_t outsz) {
    if (p && p[0] == '/') { snprintf(out, outsz, "%s", p); return; }
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) snprintf(out, outsz, "%s/%s", cwd, p ? p : "");
    else snprintf(out, outsz, "%s", p ? p : "");
}

// The stable depfile path for an entry source, under the cache dir.
void cache_depfile_path(const char* ae_file, char* out, size_t outsz) {
    char abs[1024];
    abspath_of(ae_file, abs, sizeof(abs));
    init_cache_dir();
    snprintf(out, outsz, "%s/%016llx.deps", s_cache_dir, fnv64_str(abs));
}

/* Fold a depfile's contents into `acc`. Returns 1 if a valid v1 manifest was
 * read (so the caller uses this key and SKIPS the tree walk), 0 otherwise
 * (missing/unreadable/wrong-version → caller falls back to the tree hash).
 *
 *   read <path>    → path string + its content hash (an edit busts the key)
 *   absent <path>  → path string + a presence bit; the bit is 1 once the file
 *                    EXISTS, so a module dropped in at a previously-missed
 *                    probe flips the key and busts the cache (the shadowing
 *                    case Nic flagged). */
static int fold_depfile(const char* depfile, unsigned long long* acc) {
    FILE* f = fopen(depfile, "r");
    if (!f) return 0;
    char line[2048];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "# aether-deps v1", 16) != 0) {
        fclose(f);
        return 0;   /* unknown/foreign format — do not trust it */
    }
    int any = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        char* sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        const char* kind = line;
        const char* path = sp + 1;
        *acc ^= fnv64_str(path);
        if (strcmp(kind, "read") == 0) {
            *acc = (*acc * 1099511628211ULL) ^ fnv64_file(path);
            any = 1;
        } else if (strcmp(kind, "absent") == 0) {
            /* presence bit: 1 iff the once-missing path now exists */
            unsigned long long present = (access(path, F_OK) == 0) ? 0x9E3779B1ULL : 0ULL;
            *acc = (*acc * 1099511628211ULL) ^ present;
            any = 1;
        }
    }
    fclose(f);
    return any;
}

// Compute a cache key from: source content + compiler mtime + lib mtime +
// every --extra C file's content + optimisation level + arbitrary salt.
// Returns 0 if the source can't be read (caching disabled for this build).
//
// Hashing extra-file *content* (not just mtime) closes a real correctness
// gap: editing an FFI shim like `--extra renderer.c` would otherwise let
// a stale cache entry mask the change.
/* Fold AETHER_LIB_DIR into tc.lib_dirs[] if no `--lib` flags were
 * passed. Matches the resolution priority documented in `ae lib-path`
 * (CLI flags win; env var seeds; default = `lib`). Without this, an
 * `ae run` invoked with `AETHER_LIB_DIR=...` would see lib_dir_count=0
 * and compute_cache_key couldn't include the env-resolved modules'
 * mtimes — so an edit to a vendored module behind that env var would
 * never invalidate the cache. Idempotent: skips if already populated. */

static void tc_seed_lib_dirs_from_env(void) {
    if (tc.lib_dir_count > 0) return;
    const char* env = getenv("AETHER_LIB_DIR");
    if (env && *env) tc_lib_dir_append(env);
}

#ifdef _WIN32
/* Windows twin of the POSIX walk below (#1235). This walk was compiled out
 * on Windows, so lib-dir contents never entered the cache key: only the
 * directory's own mtime did, and that does not change on an edit-in-place.
 * Every module edit under lib/ therefore served a stale cached binary until
 * `ae cache clear`. FindFirstFileA works on both MinGW and MSVC; dirent.h
 * does not exist under MSVC, hence a native walk rather than un-guarding
 * the POSIX one. Semantics mirror the POSIX twin exactly: bounded depth,
 * shared entry cap, relative-path + content folding. */
static int hash_lib_dir_entries(const char* dir, const char* rel,
                                unsigned long long* acc, int* count, int depth) {
    if (depth > 8 || *count >= 4096) return *count;
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return *count;
    do {
        const char* name = fd.cFileName;
        if (name[0] == '.') continue;  // skip . / .. / dotfiles
        char full[1024];
        snprintf(full, sizeof(full), "%s\\%s", dir, name);
        char childrel[1024];
        if (rel && rel[0])
            snprintf(childrel, sizeof(childrel), "%s/%s", rel, name);
        else
            snprintf(childrel, sizeof(childrel), "%s", name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            hash_lib_dir_entries(full, childrel, acc, count, depth + 1);
            continue;
        }
        size_t nlen = strlen(name);
        int interesting = 0;
        if (nlen > 3 && strcmp(name + nlen - 3, ".ae") == 0) interesting = 1;
        else if (nlen > 2 && (strcmp(name + nlen - 2, ".c") == 0 ||
                              strcmp(name + nlen - 2, ".h") == 0)) interesting = 1;
        if (!interesting) continue;
        *acc ^= fnv64_str(childrel);
        *acc = (*acc * 1099511628211ULL) ^ fnv64_file(full);
        (*count)++;
    } while (FindNextFileA(h, &fd) && *count < 4096);
    FindClose(h);
    return *count;
}
#endif

#ifndef _WIN32
/* Recursively fold every source file (.ae/.c/.h) under `dir` into `*acc`
 * (name + resolved mtime + size), returning the count hashed. Modules live in
 * SUBDIRECTORIES of a lib dir (`std/string/module.ae`,
 * `contrib/host/lua/module.ae`), so a top-level-only walk misses them — an
 * edit to a subdir module would not invalidate the cache and `ae run`/`build`
 * would serve a stale binary. We recurse (bounded depth + a shared entry cap)
 * so any module edit, at any nesting, bumps the key. `rel` is the path from
 * the lib-dir root, so the same file at the same relative path hashes
 * identically across runs but distinctly from a same-named file elsewhere. */
static int hash_lib_dir_entries(const char* dir, const char* rel,
                                unsigned long long* acc, int* count, int depth) {
    if (depth > 8 || *count >= 4096) return *count;
    DIR* d = opendir(dir);
    if (!d) return *count;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && *count < 4096) {
        const char* name = de->d_name;
        if (name[0] == '.') continue;  // skip . / .. / dotfiles
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        char childrel[1024];
        if (rel && rel[0])
            snprintf(childrel, sizeof(childrel), "%s/%s", rel, name);
        else
            snprintf(childrel, sizeof(childrel), "%s", name);
        struct stat est;
        if (stat(full, &est) != 0) continue;   // stat follows symlinks (#623)
        if (S_ISDIR(est.st_mode)) {
            hash_lib_dir_entries(full, childrel, acc, count, depth + 1);
            continue;
        }
        size_t nlen = strlen(name);
        int interesting = 0;
        if (nlen > 3 && strcmp(name + nlen - 3, ".ae") == 0) interesting = 1;
        else if (nlen > 2 && (strcmp(name + nlen - 2, ".c") == 0 ||
                              strcmp(name + nlen - 2, ".h") == 0)) interesting = 1;
        if (!interesting) continue;
        /* Hash the RELATIVE path (distinguishes a subdir module from a
         * same-named top-level file, and is stable across runs) + the file's
         * CONTENT. Content-hashing rather than mtime+size is what makes a
         * same-second, same-size edit invalidate the cache (the #1025 Bug B
         * miss: flipping a constant `0.5`->`0.7` in an editor-save loop kept
         * the same length and second); it also avoids a spurious cache miss
         * when a file is `touch`ed without a content change. The 4096-entry /
         * depth-8 caps above bound the read cost. */
        *acc ^= fnv64_str(childrel);
        *acc = (*acc * 1099511628211ULL) ^ fnv64_file(full);
        (*count)++;
    }
    closedir(d);
    return *count;
}
#endif

int extras_append(char* list, size_t cap, const char* path) {
    if (!list || !path || !path[0]) return 0;
    size_t used = strlen(list);
    int quote = strchr(path, ' ') != NULL;
    size_t need = (used ? 1 : 0) + strlen(path) + (quote ? 2 : 0);
    if (used + need + 1 > cap) return 0;
    if (used) list[used++] = ' ';
    if (quote) list[used++] = '"';
    memcpy(list + used, path, strlen(path));
    used += strlen(path);
    if (quote) list[used++] = '"';
    list[used] = '\0';
    return 1;
}

int extras_next(const char** cursor, char* out, size_t out_size) {
    const char* p = *cursor;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *cursor = p; return 0; }
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (n + 1 < out_size) out[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 < out_size) out[n++] = *p;
            p++;
        }
    }
    out[n] = '\0';
    *cursor = p;
    return 1;
}

unsigned long long compute_cache_key(const char* ae_file,
                                            const char* extra_files,
                                            const char* opt_level,
                                            const char* extra_salt) {
    unsigned long long src_hash = fnv64_file(ae_file);
    if (src_hash == 0) return 0;
    tc_seed_lib_dirs_from_env();

    char key_buf[2048];
    int pos = 0;
    pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, "%016llx", src_hash);

    /* The three toolchain binaries — aetherc (the compiler, which owns
     * codegen), the ae driver (owns the flags passed to the C compiler),
     * and libaether — are keyed by CONTENT HASH, not mtime.
     *
     * st_mtime is second-granularity, so a `make` that rebuilds aetherc and
     * an `ae run` within the same wall-clock second produced an identical key
     * and served the binary the OLD compiler emitted: a different codegen with
     * a report of success and every measurement against it silently wrong.
     * (Sub-second mtime is not portable in the key, and size alone misses a
     * same-length codegen change.) Hashing each is ~1 ms on a ~2 MB binary,
     * negligible beside the recompile a real miss triggers, and it is the
     * only fold that reflects an actual change in what these produce.
     * A hash of 0 (unreadable) folds in nothing rather than colliding. */
    struct stat st; (void)st;
    unsigned long long cc_hash = fnv64_file(tc.compiler);
    if (cc_hash) pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":cc=%016llx", cc_hash);
    char self_path[1200];
    if (get_exe_path(self_path, sizeof(self_path))) {
        unsigned long long ae_hash = fnv64_file(self_path);
        if (ae_hash) pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":ae=%016llx", ae_hash);
    }
    if (tc.has_lib) {
        unsigned long long lib_hash = fnv64_file(tc.lib);
        if (lib_hash) pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":lib=%016llx", lib_hash);
    }

    if (extra_files && extra_files[0]) {
        const char* cursor = extra_files;
        char tok[2048];
        while (extras_next(&cursor, tok, sizeof(tok))) {
            unsigned long long fh = fnv64_file(tok);
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":%016llx", fh);
        }
    }

    /* #1882: exact dependencies from a prior build's depfile, in preference to
     * hashing whole directory trees. When aetherc last built this entry it
     * wrote sysroot-of-imports manifest (files read + paths probed-and-absent);
     * folding it in gives precise invalidation and skips the conservative tree
     * walk below. On the FIRST build (no depfile yet) fold_depfile returns 0 and
     * we fall through to the tree hash — which is also what aetherc's own
     * --emit-deps run keys on, so the cold key and the tree-walk key agree. */
    int used_depfile = 0;
    {
        char depfile[1200];
        cache_depfile_path(ae_file, depfile, sizeof(depfile));
        unsigned long long dep_acc = 1469598103934665603ULL;
        if (fold_depfile(depfile, &dep_acc)) {
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":deps=%016llx", dep_acc);
            used_depfile = 1;
        }
    }

    if (!used_depfile) {
    /* #1421: the entry file's OWN directory tree.
     *
     * The key hashed the entry file's content and the lib dirs, but a
     * project's other sources are neither: `import helper` next to
     * `src/main.ae` resolves to `src/helper.ae`, which lived in no lib dir and
     * was not an extra_file. Editing it left the key unchanged, so `ae build`
     * printed "Built (cache hit)" and served a binary built from the old
     * module. Deleting `target/` did not help, because the cache lives under
     * ~/.aether/cache, so the stale result looked like the build simply had
     * nothing to do.
     *
     * That is the worst shape a cache bug can take: the output is wrong, the
     * report says success, and every measurement taken against it is quietly
     * invalid. Hash the whole tree the entry sits in, with the same
     * content-hash and the same depth/entry caps the lib-dir walk uses, so any
     * edit to any project source bumps the key. Over-invalidating costs a
     * rebuild; under-invalidating costs a wrong answer.
     */
    {
        char entry_dir[1024];
        snprintf(entry_dir, sizeof(entry_dir), "%s", ae_file);
        char* cut = strrchr(entry_dir, '/');
#ifdef _WIN32
        char* bcut = strrchr(entry_dir, '\\');
        if (!cut || (bcut && bcut > cut)) cut = bcut;
#endif
        if (cut) *cut = '\0';
        else snprintf(entry_dir, sizeof(entry_dir), ".");

        unsigned long long src_tree = 0;
        int src_count = 0;
        hash_lib_dir_entries(entry_dir, "", &src_tree, &src_count, 0);
        if (src_count > 0) {
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                            ":src=%016llx", src_tree);
        }

        /* #1882: the WORKING DIRECTORY tree, when it is not the entry's own.
         *
         * Module resolution is CWD-relative (aether_module.c "Try 3-6":
         * `src/<m>/module.ae`, `<m>/module.ae`, `<m>.ae`, all probed from the
         * process's cwd), so a project-root module resolves for an entry file
         * anywhere. The block above hashes only the directory the ENTRY sits
         * in, which is the project root when you run `ae run main.ae` and is
         * NOT when you run `ae run tests/suite.ae` — there it hashes `tests/`
         * and never sees the module that actually got compiled.
         *
         * `tests/<suite>.ae` importing a module from the project root is the
         * ordinary layout for an Aether project's own test suite, so this sat
         * on the default path: editing the module under test left the key
         * unchanged and the suite re-ran the PREVIOUS binary, reporting green
         * against code it never compiled. Same failure shape as #1421, one
         * resolution root over.
         *
         * Skipped when cwd IS the entry dir, so the ordinary root-entry case
         * does not hash the same tree twice. */
        {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) {
                char entry_abs[1024];
                int same = 0;
                if (entry_dir[0] == '/' ) {
                    same = (strcmp(entry_dir, cwd) == 0);
                } else if (strcmp(entry_dir, ".") == 0) {
                    same = 1;
                } else {
                    /* A relative entry_dir names a path UNDER cwd, so it can
                     * only be the same directory when it resolves to it. */
                    snprintf(entry_abs, sizeof(entry_abs), "%s/%s", cwd, entry_dir);
                    same = (strcmp(entry_abs, cwd) == 0);
                }
                if (!same) {
                    unsigned long long cwd_tree = 0;
                    int cwd_count = 0;
                    hash_lib_dir_entries(cwd, "", &cwd_tree, &cwd_count, 0);
                    if (cwd_count > 0) {
                        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                                        ":cwd=%016llx", cwd_tree);
                    }
                }
            }
        }
    }
    }   /* end if (!used_depfile) — the entry-dir + cwd tree walk the depfile
         * replaces. The --lib identity below always runs. */

    /* Issue #413: include the --lib search path in the cache key.
     * Two builds of the same source with different lib paths must
     * resolve different imports — they're materially different
     * outputs and need distinct cache slots. Walk the array
     * directly (no separator-string round-trip) so order and
     * dedup are reflected in the key.
     *
     * Per-directory mtime alone (#623): the dir's mtime only bumps
     * on create/delete/rename of an entry — NOT on an edit-in-place
     * to an existing file inside it (and NOT on `sed -i` of a file
     * *behind* a symlink that points outside the dir). For correct
     * cache invalidation on module edits, we ALSO fold in the mtime
     * of every top-level `.ae` entry in each lib dir, resolved
     * through symlinks via `stat` (which follows; `lstat` would not).
     * `stat` is the right call here precisely because the symlink
     * case (#623) needs the target's mtime, not the link's. We cap
     * the entry count per dir (256) so a runaway lib dir can't
     * blow the cache-key buffer; the cap is well above any realistic
     * stdlib/vendored-modules count. */
    if (tc.lib_dir_count == 0) {
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":lib=(default)");
        /* #1025 Bug A: with no --lib flag and no $AETHER_LIB_DIR, the compiler
         * still searches the default lib dir (module_add_lib_dir(
         * AETHER_DEFAULT_LIB_DIR) in aether_module.c) — the canonical
         * src/main.ae + lib/<name>/module.ae package layout. Without this walk
         * the key ignored that dir entirely, so editing a module under the
         * default lib/ served a stale binary until `ae cache clear`. Walk it
         * exactly as an explicit lib dir; no contribution when it's absent.
         * Content walk only when no depfile — the depfile records the lib files
         * actually read. */
        if (!used_depfile) {
            unsigned long long entry_hash = 0;
            int n = 0;
            hash_lib_dir_entries(AETHER_DEFAULT_LIB_DIR, "", &entry_hash, &n, 0);
            if (n > 0) {
                pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                                ":dlent=%d:dlh=%016llx", n, entry_hash);
            }
        }
    }
    for (int i = 0; i < tc.lib_dir_count; i++) {
        /* The lib-dir PATH IDENTITY (paths + order) is always part of the key,
         * depfile or not: two builds with different --lib sets or a different
         * --override (overrides join as lib dirs) are materially different even
         * when the resolved files hash the same, and the depfile is keyed only
         * on the entry source, so it cannot carry this distinction. Dropping it
         * on the depfile path let an `--override` build reuse the
         * non-overridden slot (#1882 depfile regression). */
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                        ":lib[%d]=%s", i, tc.lib_dirs[i]);
        struct stat lst;
        if (stat(tc.lib_dirs[i], &lst) == 0) {
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                            ":lmt=%lld", (long long)lst.st_mtime);
        }
        /* The CONTENT walk is what the depfile replaces (it records the lib
         * files actually read). Recurse the whole lib-dir tree only when there
         * is no depfile — modules live in subdirectories (#623 follow-up: a
         * top-level-only walk missed every std/contrib module in a subdir). */
        if (!used_depfile) {
            unsigned long long entry_hash = 0;
            int n = 0;
            hash_lib_dir_entries(tc.lib_dirs[i], "", &entry_hash, &n, 0);
            if (n > 0) {
                pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                                ":lent=%d:lh=%016llx", n, entry_hash);
            }
        }
    }

    snprintf(key_buf + pos, sizeof(key_buf) - pos, ":%s:%s",
             opt_level ? opt_level : "O0",
             extra_salt ? extra_salt : "");

    unsigned long long h = fnv64_str(key_buf);
    return h ? h : 1ULL;
}
