// aether_sandbox_path.h — where a file-system path leads, for grant matching.
//
// A grant for "/box/*" means the files under /box, so a path has to be
// matched where it leads, not as it is spelt: "/box/../etc/passwd", a symlink
// in /box that points out, "/box/link/../x" when link points out, and a
// dangling symlink in /box whose target is outside all lead elsewhere.
//
// aether_sandbox_canon_path walks the path one component at a time, the way
// the kernel does: "." is dropped; ".." goes to the parent of what has been
// resolved so far (which has no symlinks left in it, so its parent is the
// real one); a component that exists is resolved with realpath, following a
// symlink to where it leads; one that does not exist (nor anything below it)
// is taken as written, and a ".." after it removes a name not yet created.
// Every component is looked at, so a ".." that climbs back out of a missing
// directory still has the symlinks beyond it followed. A path that cannot be
// resolved (a dangling
// symlink, a loop, a directory that cannot be searched, one too long) is a
// failure, and the caller refuses it rather than match it as spelt.
//
// Resolving ".." lexically first would be wrong: "/box/link/../x" with link
// pointing to /elsewhere/dir opens /elsewhere/x, not /box/x.
//
// Shared by the in-process checks (aether_sandbox.c) and the LD_PRELOAD
// library (libaether_sandbox_preload.c), which is built on its own, so this
// is header-only. On Windows the path is made absolute and normalised with
// _fullpath; symlinks there are not followed.
#ifndef AETHER_SANDBOX_PATH_H
#define AETHER_SANDBOX_PATH_H

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32

static int aether_sandbox_canon_path(const char* in, char* out, size_t out_size) {
    if (!in || !*in || !out) return 0;
    return _fullpath(out, in, out_size) != NULL;
}

#else

#include <sys/stat.h>
#include <unistd.h>

// Append "/name" (name of length n) to res, which holds rlen bytes.
static int aether_sandbox_path_push(char* res, size_t* rlen, size_t cap, const char* name, size_t n) {
    size_t at = *rlen;
    if (at == 1 && res[0] == '/') at = 0;   // root: "/" + name, not "//name"
    if (at + 1 + n + 1 > cap) return 0;
    res[at] = '/';
    memcpy(res + at + 1, name, n);
    res[at + 1 + n] = '\0';
    *rlen = at + 1 + n;
    return 1;
}

// Returns 1 with the resolved absolute path in out, or 0 if it cannot be
// resolved (out is then unspecified).
static int aether_sandbox_canon_path(const char* in, char* out, size_t out_size) {
    if (!in || !*in || !out || out_size < 2) return 0;
    char full[PATH_MAX];
    if (in[0] == '/') {
        if (strlen(in) >= sizeof(full)) return 0;
        strcpy(full, in);
    } else {
        if (!getcwd(full, sizeof(full))) return 0;
        size_t cl = strlen(full);
        if (cl + 1 + strlen(in) + 1 > sizeof(full)) return 0;
        full[cl] = '/';
        strcpy(full + cl + 1, in);
    }

    char res[PATH_MAX];
    size_t rlen = 1;
    res[0] = '/';
    res[1] = '\0';
    const char* p = full;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && p[0] == '.') {
            // nothing
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (rlen > 1 && res[rlen - 1] != '/') rlen--;
            if (rlen > 1) rlen--;           // drop the separator too
            res[rlen] = '\0';
        } else {
            if (!aether_sandbox_path_push(res, &rlen, sizeof(res), p, n)) return 0;
            struct stat st;
            if (lstat(res, &st) == 0) {
                if (S_ISLNK(st.st_mode)) {
                    char real[PATH_MAX];
                    if (!realpath(res, real)) return 0;   // dangling, or a loop
                    rlen = strlen(real);
                    if (rlen + 1 > sizeof(res)) return 0;
                    memcpy(res, real, rlen + 1);
                }
            } else if (errno != ENOENT && errno != ENOTDIR) {
                return 0;   // cannot be searched: refuse rather than guess
            }
        }
        p = e;
    }
    if (rlen + 1 > out_size) return 0;
    memcpy(out, res, rlen + 1);
    return 1;
}

#endif // _WIN32

// A grant pattern for an fs category, resolved the same way, so a grant and
// the paths it covers are compared in one form ("/var/x/*" and a path under
// /private/var/x on macOS). The fixed part is resolved and the wildcard kept:
//   "/box/*"    -> "<resolved /box>/*"
//   "/box/a*"   -> "<resolved /box>/a*"
//   "/box/f"    -> "<resolved /box/f>"
//   "*", "*.log" (no fixed directory) -> unchanged
// Returns 1 with the pattern in out, or 0 to keep the pattern as written.
static int aether_sandbox_canon_pattern(const char* pat, char* out, size_t out_size) {
    if (!pat || !*pat || pat[0] == '*' || !out) return 0;
    size_t n = strlen(pat);
    if (pat[n - 1] != '*') return aether_sandbox_canon_path(pat, out, out_size);
    // The directory before the last separator is resolved; what follows it
    // (a name prefix, possibly empty) is kept with the '*'.
    const char* slash = NULL;
    for (const char* q = pat; q < pat + n - 1; q++) {
        if (*q == '/' || *q == '\\') slash = q;
    }
    if (!slash) return 0;
    char dir[PATH_MAX];
    size_t dl = (size_t)(slash - pat);
    if (dl == 0) { dir[0] = '/'; dl = 1; }
    else {
        if (dl >= sizeof(dir)) return 0;
        memcpy(dir, pat, dl);
    }
    dir[dl] = '\0';
    char rdir[PATH_MAX];
    if (!aether_sandbox_canon_path(dir, rdir, sizeof(rdir))) return 0;
    size_t rl = strlen(rdir);
    const char* tail = slash + 1;   // "a*" or "*"
#ifdef _WIN32
    const char sepch = '\\';
#else
    const char sepch = '/';
#endif
    int sep = !(rl > 0 && (rdir[rl - 1] == '/' || rdir[rl - 1] == '\\'));
    if (rl + (size_t)sep + strlen(tail) + 1 > out_size) return 0;
    memcpy(out, rdir, rl);
    if (sep) out[rl++] = sepch;
    strcpy(out + rl, tail);
    return 1;
}

#endif // AETHER_SANDBOX_PATH_H
