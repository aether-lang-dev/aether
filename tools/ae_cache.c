/* ae_cache.c — build cache: content-hashed keys, publish, GC, and the
 * cache management command (#1221 split). Moved verbatim out of ae.c; the
 * fnv64 hashers and the lib-dir walk stay static to this file, the entry
 * points the driver uses are declared in ae_internal.h.
 */

#include "ae_internal.h"
#include "ae_line.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef _WIN32
#ifndef popen
#  define popen  _popen
#  define pclose _pclose
#endif
#endif
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

/* FNV-1a over `s`, continuing from `h`: a string hashed in pieces hashes
 * as the pieces joined. */
static unsigned long long fnv64_more(unsigned long long h, const char* s) {
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

// FNV-64 hash of a string
static unsigned long long fnv64_str(const char* s) {
    return fnv64_more(14695981039346656037ULL, s);
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

/* Fold the module archives in one directory into `acc`, order-independently
 * (directory order is not stable). libaether.a is hashed on its own and
 * libaether_compiler.a is never linked into a program, so both are skipped. */
static void hash_archives_in(const char* dir, unsigned long long* acc) {
    char p[1500];
#ifdef _WIN32
    char pattern[1300];
    snprintf(pattern, sizeof(pattern), "%s\\*.a", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* name = e->d_name;
        size_t n = strlen(name);
        if (n < 3 || strcmp(name + n - 2, ".a") != 0) continue;
#endif
        if (strcmp(name, "libaether.a") == 0 || strcmp(name, "libaether_compiler.a") == 0)
            continue;
        snprintf(p, sizeof(p), "%s%s%s", dir, PATH_SEP, name);
        *acc += fnv64_str(name) ^ fnv64_file(p);
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

/* The module archives a program's @link can pull in (-laether_sqlite
 * -lsqlite3): `make contrib` leaves them in <dir of libaether.a>/contrib,
 * `make install-contrib` beside libaether.a itself. They change the binary
 * without changing any source the key sees, so switching contrib.sqlite
 * between the vendored amalgamation and the system library (#1372), or
 * rebuilding a veneer, served the previous link's binary as a cache hit.
 * 0 when there are none. */
static unsigned long long hash_contrib_archives(const char* lib_path) {
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s", lib_path);
    char* cut = strrchr(dir, '/');
#ifdef _WIN32
    char* bcut = strrchr(dir, '\\');
    if (!cut || (bcut && bcut > cut)) cut = bcut;
#endif
    if (!cut) return 0;
    *cut = '\0';
    unsigned long long acc = 0;
    hash_archives_in(dir, &acc);
    char sub[1300];
    snprintf(sub, sizeof(sub), "%s%scontrib", dir, PATH_SEP);
    hash_archives_in(sub, &acc);
    return acc;
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
/* The current directory however long it is, in a buffer the caller frees;
 * NULL when it cannot be read. */
static char* cwd_dup(void) {
    for (size_t cap = 1024; cap <= ((size_t)1 << 20); cap *= 2) {
        char* buf = malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE) return NULL;
    }
    return NULL;
}

/* Absolute as this platform reads a path: a leading `/`, and on Windows also
 * `\` or a drive (`C:`). */
static int cache_path_is_absolute(const char* p) {
    if (p[0] == '/') return 1;
#ifdef _WIN32
    if (p[0] == '\\') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return 1;
#endif
    return 0;
}

// The stable depfile path for an entry source, under the cache dir.
//
// Named for a hash of the absolute path (`<cwd>/<file>` for a relative one),
// hashed in pieces rather than built in a buffer: a 1 KB one cut a longer
// path, and a cwd past it fell back to the bare relative name, so two
// projects' `main.ae` shared one slot and each keyed on the other's
// dependencies (#2537). The bytes hashed are the ones the buffer held, so a
// slot for a shorter path keeps its name. On Windows a drive or backslash
// path is absolute too (#2538); it used to get the cwd in front, so one file
// had a slot per directory it was built from.
void cache_depfile_path(const char* ae_file, char* out, size_t outsz) {
    const char* p = ae_file ? ae_file : "";
    unsigned long long h;
    char* cwd = cache_path_is_absolute(p) ? NULL : cwd_dup();
    if (cwd) {
        h = fnv64_more(fnv64_more(fnv64_str(cwd), "/"), p);
        free(cwd);
    } else {
        h = fnv64_str(p);
    }
    init_cache_dir();
    snprintf(out, outsz, "%s/%016llx.deps", s_cache_dir, h);
}

/* Fold a depfile's contents into `acc`. Returns 1 if a valid v1 manifest was
 * read (so the caller uses this key and SKIPS the tree walk), 0 otherwise
 * (missing/unreadable/wrong-version → caller falls back to the tree hash).
 *
 *   read <path>    → path string + its content hash (an edit busts the key)
 *   absent <path>  → path string + a presence bit; the bit is 1 once the file
 *                    EXISTS, so a module dropped in at a previously-missed
 *                    probe flips the key and busts the cache (the shadowing
 *                    case Nic flagged).
 *
 * Lines are read whole (#2536). A `read` line past a 2 KB buffer hashed a
 * cut path, which never exists, so its content hash was the same 0 on every
 * run and an edit to that file was served from the cache. A line that is
 * not one of the two above, or that has no newline because the file ends
 * in the middle of it, makes the whole manifest untrusted (0), as does
 * running out of memory: the caller then walks the trees, as it does when
 * there is no depfile. */
static int fold_depfile(const char* depfile, unsigned long long* acc) {
    FILE* f = fopen(depfile, "r");
    if (!f) return 0;
    char* line = NULL;
    size_t cap = 0;
    if (ae_read_line(f, &line, &cap) <= 0 || strncmp(line, "# aether-deps v1", 16) != 0) {
        free(line);
        fclose(f);
        return 0;   /* unknown/foreign format: do not trust it */
    }
    int any = 0;
    int trusted = 1;
    int got;
    while ((got = ae_read_line(f, &line, &cap)) > 0) {
        size_t n = strlen(line);
        if (n == 0 || line[n - 1] != '\n') { trusted = 0; break; }   /* cut short */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        char* sp = strchr(line, ' ');
        if (!sp) { trusted = 0; break; }
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
        } else {
            trusted = 0;
            break;
        }
    }
    free(line);
    fclose(f);
    return (got < 0 || !trusted) ? 0 : any;
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

/* The cold cache key's walk of a source tree: every .ae/.c/.h file under a
 * root, its path relative to the root and its CONTENT folded in. Modules live
 * in SUBDIRECTORIES of a lib dir (`std/string/module.ae`,
 * `contrib/host/lua/module.ae`), so a top-level-only walk misses them; it
 * recurses, following symlinks (#623). The relative path tells a subdir
 * module from a same-named top-level file and is stable across runs.
 * Content rather than mtime+size is what makes a same-second, same-size edit
 * invalidate the cache (the #1025 Bug B miss: flipping a constant `0.5` to
 * `0.7` in an editor-save loop kept the same length and second); it also
 * avoids a spurious miss when a file is `touch`ed without a change.
 *
 * Paths are built in buffers grown as needed (#2538): 1 KB ones cut a deeper
 * path, which named no file, so the module there never reached the key and
 * an edit to it was served from the cache. The depth and file caps stay, as
 * a bound on what a cold key reads (and on a symlink cycle), but reaching one
 * no longer hides what lies past it: `incomplete` says why the walk could not
 * see every file, and compute_cache_key then makes no key at all. */
#define TREE_WALK_MAX_DEPTH 8
#define TREE_WALK_MAX_FILES 4096

typedef struct {
    unsigned long long acc;
    int count;
    const char* incomplete;
    char* full;
    size_t full_len, full_cap;
    char* rel;
    size_t rel_len, rel_cap;
} TreeWalk;

/* `sep` then `name` appended to a path buffer grown as needed. 0 when out
 * of memory. */
static int walk_path_push(char** buf, size_t* len, size_t* cap,
                          const char* sep, const char* name) {
    size_t sl = strlen(sep), nl = strlen(name);
    size_t need = *len + sl + nl + 1;
    if (need > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (ncap < need) ncap *= 2;
        char* nb = realloc(*buf, ncap);
        if (!nb) return 0;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, sep, sl);
    *len += sl;
    memcpy(*buf + *len, name, nl + 1);
    *len += nl;
    return 1;
}

static int walk_is_source(const char* name) {
    size_t n = strlen(name);
    return (n > 3 && strcmp(name + n - 3, ".ae") == 0) ||
           (n > 2 && (strcmp(name + n - 2, ".c") == 0 || strcmp(name + n - 2, ".h") == 0));
}

static void walk_dir(TreeWalk* w, int depth);

/* One entry of the directory being walked, already pushed onto both paths
 * (w->full names it on disk, w->rel within the tree). */
static void walk_entry(TreeWalk* w, const char* name, int is_dir, int depth) {
    if (is_dir) {
        if (depth + 1 > TREE_WALK_MAX_DEPTH) w->incomplete = "has a directory more than 8 levels deep";
        else walk_dir(w, depth + 1);
        return;
    }
    if (!walk_is_source(name)) return;
    if (w->count >= TREE_WALK_MAX_FILES) {
        w->incomplete = "has more than 4096 source files";
        return;
    }
    w->acc ^= fnv64_str(w->rel);
    w->acc = (w->acc * 1099511628211ULL) ^ fnv64_file(w->full);
    w->count++;
}

/* Push `name` onto both paths, visit it, pop it again. `is_dir` is -1 when
 * the listing did not say: stat follows symlinks (#623), so a linked module
 * is hashed by the content it points at, and a name that cannot be stat'ed
 * (a dangling link) is not a file the compiler can read either. */
static void walk_child(TreeWalk* w, const char* name, int is_dir, int depth,
                       const char* sep) {
    size_t full_len = w->full_len, rel_len = w->rel_len;
    if (!walk_path_push(&w->full, &w->full_len, &w->full_cap, sep, name) ||
        !walk_path_push(&w->rel, &w->rel_len, &w->rel_cap, rel_len ? "/" : "", name)) {
        w->incomplete = "could not be walked (out of memory)";
        return;
    }
    if (is_dir < 0) {
        struct stat est;
        is_dir = stat(w->full, &est) != 0 ? -1 : S_ISDIR(est.st_mode) != 0;
    }
    if (is_dir >= 0) walk_entry(w, name, is_dir, depth);
    w->full_len = full_len;
    w->full[full_len] = '\0';
    w->rel_len = rel_len;
    w->rel[rel_len] = '\0';
}

#ifdef _WIN32
/* Windows twin of the POSIX walk below (#1235). This walk was compiled out
 * on Windows, so lib-dir contents never entered the cache key: only the
 * directory's own mtime did, and that does not change on an edit-in-place.
 * Every module edit under lib/ therefore served a stale cached binary until
 * `ae cache clear`. FindFirstFileA works on both MinGW and MSVC; dirent.h
 * does not exist under MSVC, hence a native walk rather than un-guarding
 * the POSIX one. Semantics mirror the POSIX twin exactly. */
static void walk_dir(TreeWalk* w, int depth) {
    size_t full_len = w->full_len;
    if (!walk_path_push(&w->full, &w->full_len, &w->full_cap, "\\", "*")) {
        w->incomplete = "could not be walked (out of memory)";
        return;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(w->full, &fd);
    DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    w->full_len = full_len;
    w->full[full_len] = '\0';
    if (h == INVALID_HANDLE_VALUE) {
        /* An absent root is a tree with nothing in it (no lib/ dir, say). A
         * directory the listing showed but that cannot be opened (a path
         * too long for the ANSI API, no permission) hides its files. */
        if (depth > 0 || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND))
            w->incomplete = "has a directory that could not be read";
        return;
    }
    do {
        const char* name = fd.cFileName;
        if (name[0] == '.') continue;  // skip . / .. / dotfiles
        walk_child(w, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                   depth, "\\");
    } while (!w->incomplete && FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void walk_dir(TreeWalk* w, int depth) {
    DIR* d = opendir(w->full);
    if (!d) {
        /* An absent root is a tree with nothing in it (no lib/ dir, say). A
         * directory the listing showed but that cannot be opened hides its
         * files. */
        if (depth > 0 || (errno != ENOENT && errno != ENOTDIR))
            w->incomplete = "has a directory that could not be read";
        return;
    }
    struct dirent* de;
    while (!w->incomplete && (de = readdir(d)) != NULL) {
        const char* name = de->d_name;
        if (name[0] == '.') continue;  // skip . / .. / dotfiles
        walk_child(w, name, -1, depth, "/");
    }
    closedir(d);
}
#endif

/* Fold the source files under `root` into `*acc`, adding their number to
 * `*count`. NULL when the walk saw every one; otherwise why it did not, and
 * the key must not be used. */
static const char* hash_tree(const char* root, unsigned long long* acc, int* count) {
    TreeWalk w;
    memset(&w, 0, sizeof(w));
    w.acc = *acc;
    w.count = *count;
    if (!walk_path_push(&w.full, &w.full_len, &w.full_cap, "", root) ||
        !walk_path_push(&w.rel, &w.rel_len, &w.rel_cap, "", "")) {
        w.incomplete = "could not be walked (out of memory)";
    } else {
        walk_dir(&w, 0);
    }
    free(w.full);
    free(w.rel);
    *acc = w.acc;
    *count = w.count;
    return w.incomplete;
}

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

/* `a` (its first `alen` bytes), then `b`, `c` and `d`, in a string the
 * caller frees; NULL when out of memory. */
static char* str_join4(const char* a, size_t alen, const char* b, const char* c,
                       const char* d) {
    size_t bl = strlen(b), cl = strlen(c), dl = strlen(d);
    char* s = malloc(alen + bl + cl + dl + 1);
    if (!s) return NULL;
    memcpy(s, a, alen);
    memcpy(s + alen, b, bl);
    memcpy(s + alen + bl, c, cl);
    memcpy(s + alen + bl + cl, d, dl + 1);
    return s;
}

/* #2477: find `prog` the way the build's spawn will. A name with a directory
 * part is taken as it stands; a bare name is searched along PATH
 * (posix_spawnp on POSIX; _spawnvp on Windows, which also tries the current
 * directory first and the .com/.exe/.bat/.cmd extensions of an extensionless
 * name). Returns the file's path in a string the caller frees, NULL when
 * nothing is found or memory runs out. Paths of any length (#2538): a PATH
 * entry past 1 KB was cut, so the search could settle on another file than
 * the spawn runs, and an upgrade of the real compiler left the key as it was. */
static char* resolve_program(const char* prog) {
    if (!prog || !prog[0]) return NULL;
#ifdef _WIN32
    static const char* const exts[] = { "", ".com", ".exe", ".bat", ".cmd" };
    const char* base = prog;
    for (const char* p = prog; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    }
    /* An extensionless name is tried with each extension only, as _spawnvp
     * does: a bare `gcc` file (an MSYS shell script, say) is not run. */
    int has_ext = strchr(base, '.') != NULL;
    int ext_from = has_ext ? 0 : 1, ext_to = has_ext ? 1 : 5;
    int has_dir = base != prog;
    const char* path = has_dir ? "" : getenv("PATH");
    /* Directory 0 is the name as given (the current directory for a bare
     * name), then each PATH entry in order. */
    const char* cursor = path ? path : "";
    int first = 1;
    while (first || *cursor) {
        const char* dir = "";
        size_t dlen = 0;
        if (!first) {
            dlen = strcspn(cursor, ";");
            dir = cursor;
            cursor += dlen;
            if (*cursor == ';') cursor++;
            if (dlen == 0) continue;
        }
        for (int e = ext_from; e < ext_to; e++) {
            char* cand = first ? str_join4(prog, strlen(prog), exts[e], "", "")
                               : str_join4(dir, dlen, "\\", prog, exts[e]);
            if (!cand) return NULL;
            struct stat st;
            if (stat(cand, &st) == 0 && !(st.st_mode & S_IFDIR)) return cand;
            free(cand);
        }
        if (first && has_dir) return NULL;
        first = 0;
    }
    return NULL;
#else
    if (strchr(prog, '/')) {
        if (access(prog, X_OK) != 0) return NULL;
        return strdup(prog);
    }
    const char* path = getenv("PATH");
    const char* cursor = path ? path : "/usr/bin:/bin";
    for (;;) {
        size_t len = strcspn(cursor, ":");
        /* An empty PATH entry is the current directory. */
        char* cand = len ? str_join4(cursor, len, "/", prog, "")
                         : str_join4(".", 1, "/", prog, "");
        if (!cand) return NULL;
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, X_OK) == 0)
            return cand;
        free(cand);
        cursor += len;
        if (*cursor != ':') break;
        cursor++;
    }
    return NULL;
#endif
}

/* #2477: the identity of the C compiler the build will run.
 *
 * The key covered the source, aetherc, ae, libaether and the flags, but not
 * the C compiler. The same source built under a different gcc (another one
 * first on PATH, an upgrade, $CC pointing elsewhere) was served the binary
 * the previous compiler made, reported as a cache hit; a GCC 16 build handed
 * back GCC 15's executable byte for byte, crash included.
 *
 * The selection mirrors build_gcc_cmd: $AE_CC / $CC verbatim (it may carry
 * flags, or a launcher such as `ccache gcc`), else `gcc`, else on POSIX `cc`,
 * else on Windows the WinLibs gcc ae installs. The spec string is folded in,
 * and each word of it that names a program is resolved as the spawn will
 * resolve it, folding the path and the file's content: another file is
 * another compiler, and one upgraded in place has other bytes. Computed once
 * per process; it reads the compiler driver once, about what hashing aetherc
 * costs. The spec and its words are taken whole (#2538), where 1 KB copies
 * left a longer one's tail out of the key. 0 when out of memory, which makes
 * no key. */
static unsigned long long c_compiler_fingerprint(void) {
    static int done = 0;
    static unsigned long long fp = 0;
    if (done) return fp;
    done = 1;
    const char* spec = c_backend_env_override();
    char* owned = NULL;
    if (!spec) {
        char* gcc = resolve_program("gcc");
        if (gcc) {
            spec = "gcc";
            free(gcc);
        } else {
#ifdef _WIN32
            const char* home = get_home_dir() ? get_home_dir() : "";
            spec = owned = str_join4(home, strlen(home),
                                     "\\.aether\\tools\\mingw64\\bin\\gcc.exe", "", "");
            if (!owned) return 0;
#else
            spec = "cc";
#endif
        }
    }
    unsigned long long h = fnv64_str(spec);
    const char* cursor = spec;
    size_t spec_len = strlen(spec) + 1;
    char* word = malloc(spec_len);
    if (!word) { free(owned); return 0; }
    while (extras_next(&cursor, word, spec_len)) {
        if (word[0] == '-' || strchr(word, '=')) continue;
        char* resolved = resolve_program(word);
        if (!resolved) continue;
        h = (h * 1099511628211ULL) ^ fnv64_str(resolved);
        h = (h * 1099511628211ULL) ^ fnv64_file(resolved);
        /* The file found on PATH may be a trampoline whose bytes never
         * change when the compiler behind it does: macOS's /usr/bin/gcc
         * and /usr/bin/clang (xcrun shims, switched by xcode-select or an
         * Xcode update), a ccache masquerade directory. What the program
         * says it is comes from the compiler that will actually run, so
         * its first line of `--version` is folded in too; a program with
         * no such line folds in nothing. */
        char* cmd = str_join4("\"", 1, resolved, "\" --version", "");
        free(resolved);
        if (!cmd) continue;
        FILE* pipe = popen(cmd, "r");
        free(cmd);
        if (!pipe) continue;
        char line[512];
        if (fgets(line, sizeof(line), pipe)) {
            h = (h * 1099511628211ULL) ^ fnv64_str(line);
        }
        pclose(pipe);
    }
    free(word);
    free(owned);
    fp = h ? h : 1ULL;
    return fp;
}

/* The key text, grown as needed (#2538). It was built in 2 KB and cut at the
 * end, and what fell past the cut never reached the key: with eight long
 * --lib directories, or a hundred --extra files, the -D defines and the
 * optimisation level at its end did not count, and a build with other
 * defines was served the first one's binary. */
typedef struct {
    char* buf;
    size_t len, cap;
    int oom;
} KeyText;

static void key_append(KeyText* k, const char* fmt, ...) {
    if (k->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = k->buf ? vsnprintf(k->buf + k->len, k->cap - k->len, fmt, ap) : -1;
        va_end(ap);
        if (k->buf && n < 0) { k->oom = 1; return; }
        if (k->buf && (size_t)n < k->cap - k->len) { k->len += (size_t)n; return; }
        size_t ncap = k->cap ? k->cap * 2 : 2048;
        while (n > 0 && ncap <= k->len + (size_t)n) ncap *= 2;
        char* nb = realloc(k->buf, ncap);
        if (!nb) { k->oom = 1; return; }
        if (!k->buf) nb[0] = '\0';
        k->buf = nb;
        k->cap = ncap;
    }
}

/* Set when the last compute_cache_key made no key only because a source
 * tree walk could not see every file (#2538). The build can still have
 * aetherc write a depfile, and the next key comes from that, with no walk. */
static int s_key_walk_incomplete = 0;

int cache_key_walk_incomplete(void) { return s_key_walk_incomplete; }

/* A tree walk for the key; 0 (after --verbose says why) when it could not see
 * every file. */
static int key_hash_tree(const char* root, unsigned long long* acc, int* count) {
    const char* why = hash_tree(root, acc, count);
    if (!why) return 1;
    if (tc.verbose) fprintf(stderr, "[cache] no key: %s %s\n", root, why);
    s_key_walk_incomplete = 1;
    return 0;
}

unsigned long long compute_cache_key(const char* ae_file,
                                            const char* extra_files,
                                            const char* opt_level,
                                            const char* extra_salt) {
    s_key_walk_incomplete = 0;
    unsigned long long src_hash = fnv64_file(ae_file);
    if (src_hash == 0) return 0;
    tc_seed_lib_dirs_from_env();

    KeyText key = { NULL, 0, 0, 0 };
    int complete = 1;
    key_append(&key, "%016llx", src_hash);

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
    if (cc_hash) key_append(&key, ":cc=%016llx", cc_hash);
    char self_path[1200];
    if (get_exe_path(self_path, sizeof(self_path))) {
        unsigned long long ae_hash = fnv64_file(self_path);
        if (ae_hash) key_append(&key, ":ae=%016llx", ae_hash);
    }
    /* #2477: and the C compiler that turns aetherc's output into the binary. */
    unsigned long long ccid = c_compiler_fingerprint();
    if (!ccid) key.oom = 1;
    key_append(&key, ":ccid=%016llx", ccid);
    if (tc.has_lib) {
        unsigned long long lib_hash = fnv64_file(tc.lib);
        if (lib_hash) key_append(&key, ":lib=%016llx", lib_hash);
        unsigned long long contrib_hash = hash_contrib_archives(tc.lib);
        if (contrib_hash)
            key_append(&key, ":contrib=%016llx", contrib_hash);
    }

    if (extra_files && extra_files[0]) {
        /* Any one path is no longer than the whole list (a 2 KB copy cut a
         * longer one, which then named no file and hashed as 0). */
        size_t list_len = strlen(extra_files) + 1;
        char* tok = malloc(list_len);
        if (!tok) key.oom = 1;
        const char* cursor = extra_files;
        while (tok && extras_next(&cursor, tok, list_len)) {
            unsigned long long fh = fnv64_file(tok);
            key_append(&key, ":%016llx", fh);
        }
        free(tok);
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
            key_append(&key, ":deps=%016llx", dep_acc);
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
        /* The entry's directory and the cwd at any length (#2538). */
        const char* cut = strrchr(ae_file, '/');
#ifdef _WIN32
        const char* bcut = strrchr(ae_file, '\\');
        if (!cut || (bcut && bcut > cut)) cut = bcut;
#endif
        char* entry_dir = cut ? str_join4(ae_file, (size_t)(cut - ae_file), "", "", "")
                              : str_join4(".", 1, "", "", "");
        if (!entry_dir) key.oom = 1;

        unsigned long long src_tree = 0;
        int src_count = 0;
        if (entry_dir && !key_hash_tree(entry_dir, &src_tree, &src_count)) complete = 0;
        if (complete && src_count > 0) {
            key_append(&key, ":src=%016llx", src_tree);
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
         * does not hash the same tree twice. A relative entry dir other than
         * "." names a directory under the cwd, never the cwd itself. */
        char* cwd = (complete && entry_dir) ? cwd_dup() : NULL;
        if (complete && entry_dir && !cwd) {
            /* Not knowing the cwd is not knowing what resolves from it. */
            if (tc.verbose)
                fprintf(stderr, "[cache] no key: the working directory could not be read\n");
            s_key_walk_incomplete = 1;
            complete = 0;
        }
        if (cwd) {
            int same = cache_path_is_absolute(entry_dir) ? strcmp(entry_dir, cwd) == 0
                                                         : strcmp(entry_dir, ".") == 0;
            if (!same) {
                unsigned long long cwd_tree = 0;
                int cwd_count = 0;
                if (!key_hash_tree(cwd, &cwd_tree, &cwd_count)) complete = 0;
                else if (cwd_count > 0) key_append(&key, ":cwd=%016llx", cwd_tree);
            }
            free(cwd);
        }
        free(entry_dir);
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
     * cache invalidation on module edits, the content walk below folds
     * in every source file in each lib dir, resolved through symlinks
     * via `stat` (which follows; `lstat` would not). `stat` is the
     * right call here precisely because the symlink case (#623) needs
     * the target's content, not the link's. */
    if (tc.lib_dir_count == 0) {
        key_append(&key, ":lib=(default)");
        /* #1025 Bug A: with no --lib flag and no $AETHER_LIB_DIR, the compiler
         * still searches the default lib dir (module_add_lib_dir(
         * AETHER_DEFAULT_LIB_DIR) in aether_module.c) — the canonical
         * src/main.ae + lib/<name>/module.ae package layout. Without this walk
         * the key ignored that dir entirely, so editing a module under the
         * default lib/ served a stale binary until `ae cache clear`. Walk it
         * exactly as an explicit lib dir; no contribution when it's absent.
         * Content walk only when no depfile — the depfile records the lib files
         * actually read. */
        if (!used_depfile && complete) {
            unsigned long long entry_hash = 0;
            int n = 0;
            if (!key_hash_tree(AETHER_DEFAULT_LIB_DIR, &entry_hash, &n)) complete = 0;
            else if (n > 0) key_append(&key, ":dlent=%d:dlh=%016llx", n, entry_hash);
        }
    }
    for (int i = 0; complete && i < tc.lib_dir_count; i++) {
        /* The lib-dir PATH IDENTITY (paths + order) is always part of the key,
         * depfile or not: two builds with different --lib sets or a different
         * --override (overrides join as lib dirs) are materially different even
         * when the resolved files hash the same, and the depfile is keyed only
         * on the entry source, so it cannot carry this distinction. Dropping it
         * on the depfile path let an `--override` build reuse the
         * non-overridden slot (#1882 depfile regression). */
        key_append(&key, ":lib[%d]=%s", i, tc.lib_dirs[i]);
        struct stat lst;
        if (stat(tc.lib_dirs[i], &lst) == 0) {
            key_append(&key, ":lmt=%lld", (long long)lst.st_mtime);
        }
        /* The CONTENT walk is what the depfile replaces (it records the lib
         * files actually read). Recurse the whole lib-dir tree only when there
         * is no depfile — modules live in subdirectories (#623 follow-up: a
         * top-level-only walk missed every std/contrib module in a subdir). */
        if (!used_depfile) {
            unsigned long long entry_hash = 0;
            int n = 0;
            if (!key_hash_tree(tc.lib_dirs[i], &entry_hash, &n)) complete = 0;
            else if (n > 0) key_append(&key, ":lent=%d:lh=%016llx", n, entry_hash);
        }
    }

    key_append(&key, ":%s:%s", opt_level ? opt_level : "O0",
               extra_salt ? extra_salt : "");

    if (key.oom && tc.verbose) fprintf(stderr, "[cache] no key: out of memory\n");
    unsigned long long h = (complete && !key.oom) ? fnv64_str(key.buf) : 0;
    free(key.buf);
    if (!complete || key.oom) return 0;
    return h ? h : 1ULL;
}
