/* ae_cache.c — build cache: content-hashed keys, publish, GC, and the
 * cache management command (#1221 split). Moved verbatim out of ae.c; the
 * fnv64 hashers and the lib-dir walk stay static to this file, the entry
 * points the driver uses are declared in ae_internal.h.
 */

#include "ae_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP "\\"
#else
#  include <unistd.h>
#  include <dirent.h>
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

// Atomic cache publish (#1032). Writers produce `<slot>.tmp.<pid>` in
// the cache directory and rename onto the slot, so a concurrent reader
// only ever sees a complete file (old, new, or miss — never partial).
// rename(2) within one directory is atomic on POSIX; Windows rename()
// refuses to replace an existing destination, so MoveFileEx there.
int cache_publish(const char* tmp_path, const char* final_path) {
#ifdef _WIN32
    return MoveFileExA(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp_path, final_path);
#endif
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
    fclose(f);
    return h;
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

    struct stat st;
    if (stat(tc.compiler, &st) == 0)
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":%lld", (long long)st.st_mtime);
    /* The driver's own mtime: flags ae passes to the C compiler are part
     * of the output, so a rebuilt ae must miss the cache (#1235 family). */
    char self_path[1200];
    if (get_exe_path(self_path, sizeof(self_path)) && stat(self_path, &st) == 0)
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":ae=%lld", (long long)st.st_mtime);
    if (tc.has_lib && stat(tc.lib, &st) == 0)
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":%lld", (long long)st.st_mtime);

    if (extra_files && extra_files[0]) {
        char tmp[8192];
        strncpy(tmp, extra_files, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char* tok = strtok(tmp, " \t"); tok; tok = strtok(NULL, " \t")) {
            unsigned long long fh = fnv64_file(tok);
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos, ":%016llx", fh);
        }
    }

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
         * exactly as an explicit lib dir; no contribution when it's absent. */
        {
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
        pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                        ":lib[%d]=%s", i, tc.lib_dirs[i]);
        struct stat lst;
        if (stat(tc.lib_dirs[i], &lst) == 0) {
            pos += snprintf(key_buf + pos, sizeof(key_buf) - pos,
                            ":lmt=%lld", (long long)lst.st_mtime);
        }
        /* Recurse the whole lib-dir tree — modules live in subdirectories
         * (#623 follow-up: a top-level-only walk missed every std/contrib
         * module in a subdir, so editing one served a stale cached binary). */
        {
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
