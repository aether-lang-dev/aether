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
// is header-only.
//
// On Windows "." and ".." are resolved by the text, as Windows itself
// resolves them before the file system sees a path (GetFullPathNameW), and
// the result is then asked of the file system: the deepest part of it that
// exists is opened and GetFinalPathNameByHandleW says where it really is,
// every junction and symbolic link on the way followed, in the case the
// volume stores; the names below it that do not exist yet are kept as
// written. A name that exists but cannot be followed (a link whose target
// is missing) is a failure, as a dangling symlink is on POSIX. It used to be
// _fullpath alone, which follows nothing, so a junction inside a granted
// directory reached anything outside it (#3763).
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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The longest path, in UTF-16 units, the Windows resolution works with.
#define AETHER_SANDBOX_WPATH 4096

// Where an existing path leads. 1 with its final path in `out` (in the
// "\\?\" namespace), 0 when nothing is there, -1 when something is there that
// cannot be followed or opened: refused rather than taken as missing, so a
// dangling link cannot be matched as the name it is.
static int aether_sandbox_final_w(const wchar_t* p, wchar_t* out, DWORD cap) {
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_INVALID_NAME &&
            e != ERROR_DIRECTORY && e != ERROR_BAD_PATHNAME)
            return -1;
        // Not there through the link, if any: the name itself may still
        // be (GetFileAttributesW does not follow a link).
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return -1;
        e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ||
                e == ERROR_DIRECTORY || e == ERROR_BAD_PATHNAME) ? 0 : -1;
    }
    DWORD n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    return (n == 0 || n >= cap) ? -1 : 1;
}

// The length of the root of an absolute path, its separator included:
// "C:\" 3, "\\server\share\" up to the separator after the share, the same
// behind a "\\?\" or "\\?\UNC\" prefix. 0 when there is none.
static size_t aether_sandbox_wroot(const wchar_t* p) {
    size_t at = 0;
    if (p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'?' || p[2] == L'.') && p[3] == L'\\') {
        at = 4;
        if ((p[4] == L'U' || p[4] == L'u') && (p[5] == L'N' || p[5] == L'n') &&
            (p[6] == L'C' || p[6] == L'c') && p[7] == L'\\') {
            at = 8;
            goto unc;
        }
    } else if (p[0] == L'\\' && p[1] == L'\\') {
        at = 2;
        goto unc;
    }
    if (p[at] && p[at + 1] == L':' && p[at + 2] == L'\\') return at + 3;
    return 0;
unc:
    // server, then share, each ended by a separator (the share's may be the end)
    for (int part = 0; part < 2; part++) {
        size_t s = at;
        while (p[at] && p[at] != L'\\') at++;
        if (at == s) return 0;
        if (p[at] == L'\\') at++;
        else if (part == 0) return 0;
    }
    return at;
}

// Returns 1 with the resolved absolute path (UTF-8) in out, or 0 if it
// cannot be resolved (out is then unspecified).
static int aether_sandbox_canon_path(const char* in, char* out, size_t out_size) {
    if (!in || !*in || !out || out_size < 4) return 0;
    wchar_t win[AETHER_SANDBOX_WPATH];
    wchar_t full[AETHER_SANDBOX_WPATH];
    wchar_t fin[AETHER_SANDBOX_WPATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, win, AETHER_SANDBOX_WPATH)) return 0;
    DWORD fl = GetFullPathNameW(win, AETHER_SANDBOX_WPATH, full, NULL);
    if (fl == 0 || fl >= AETHER_SANDBOX_WPATH) return 0;
    size_t root = aether_sandbox_wroot(full);
    if (root == 0) return 0;
    size_t len = fl;
    while (len > root && full[len - 1] == L'\\') len--;   // "C:\box\" is "C:\box"
    full[len] = L'\0';
    // full[0..cut) is the part asked of the file system; full[cut..len) the
    // names below it that do not exist yet, kept as written.
    size_t cut = len;
    for (;;) {
        wchar_t saved = full[cut];
        full[cut] = L'\0';
        int r = aether_sandbox_final_w(full, fin, AETHER_SANDBOX_WPATH);
        full[cut] = saved;
        if (r < 0) return 0;
        if (r > 0) break;
        if (cut <= root) return 0;   // not even the root is there
        size_t c = cut;
        while (c > root && full[c - 1] != L'\\') c--;
        cut = c > root ? c - 1 : root;
    }
    // \\?\D:\p is D:\p, and \\?\UNC\server\share\p is \\server\share\p.
    wchar_t* res = fin;
    if (wcsncmp(fin, L"\\\\?\\UNC\\", 8) == 0) {
        res = fin + 6;
        res[0] = L'\\';
        res[1] = L'\\';
    } else if (wcsncmp(fin, L"\\\\?\\", 4) == 0) {
        res = fin + 4;
    }
    size_t rl = wcslen(res);
    const wchar_t* tail = full + cut;   // "" or "\name..." (or "name..." after a root)
    size_t tl = wcslen(tail);
    int sep = tl > 0 && tail[0] != L'\\' && !(rl > 0 && res[rl - 1] == L'\\');
    if ((size_t)(res - fin) + rl + (size_t)sep + tl + 1 > AETHER_SANDBOX_WPATH) return 0;
    if (sep) res[rl++] = L'\\';
    memcpy(res + rl, tail, (tl + 1) * sizeof(wchar_t));
    return WideCharToMultiByte(CP_UTF8, 0, res, -1, out, (int)out_size, NULL, NULL) > 0;
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
        size_t il = strlen(in);
        if (il >= sizeof(full)) return 0;
        memcpy(full, in, il + 1);
    } else {
        if (!getcwd(full, sizeof(full))) return 0;
        size_t cl = strlen(full);
        size_t il = strlen(in);
        if (cl + 1 + il + 1 > sizeof(full)) return 0;
        full[cl] = '/';
        memcpy(full + cl + 1, in, il + 1);
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
    size_t tl = strlen(tail);
    if (rl + (size_t)sep + tl + 1 > out_size) return 0;
    memcpy(out, rdir, rl);
    if (sep) out[rl++] = sepch;
    memcpy(out + rl, tail, tl + 1);
    return 1;
}

#endif // AETHER_SANDBOX_PATH_H
