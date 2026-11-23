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

// The longest resolved path, in bytes, a check keeps: PATH_MAX, except on
// Windows, where PATH_MAX is MAX_PATH (260) and a path the file system holds
// can be far longer (#3850). There it is the bound the UTF-16 resolution
// works with, so a long path is resolved and matched as a short one is.
#ifdef _WIN32
#define AETHER_SANDBOX_PATH_MAX 4096
#else
#define AETHER_SANDBOX_PATH_MAX PATH_MAX
#endif

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "aether_win_path.h"

// The longest path, in UTF-16 units, the Windows resolution works with.
#define AETHER_SANDBOX_WPATH 4096

// Where an existing path leads. 1 with its final path in `out` (in the
// "\\?\" namespace), 0 when nothing is there, -1 when something is there that
// cannot be followed or opened: refused rather than taken as missing, so a
// dangling link cannot be matched as the name it is.
static int aether_sandbox_final_w(const wchar_t* p, wchar_t* out, DWORD cap) {
    // A path at or past MAX_PATH is asked in its long form (#3850);
    // one that cannot be put in it is not followed, so it is refused.
    wchar_t lp[AETHER_SANDBOX_WPATH];
    if (aether_win_path_is_long(p)) {
        if (!aether_win_long_form(p, lp, AETHER_SANDBOX_WPATH)) return -1;
        p = lp;
    }
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_CANT_ACCESS_FILE) {
        // A reparse point the system will not follow. One that is not a
        // name surrogate (an AF_UNIX socket's, a deduplicated or cloud
        // file's) redirects no name: it is the file itself, so its own
        // final path is where the name leads. A symlink or a junction that
        // cannot be followed stays a failure, as a dangling link is.
        h = CreateFileW(p, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                        NULL);
        FILE_ATTRIBUTE_TAG_INFO ti;
        if (h != INVALID_HANDLE_VALUE &&
            (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) ||
             !(ti.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || IsReparseTagNameSurrogate(ti.ReparseTag))) {
            CloseHandle(h);
            return -1;
        }
        if (h == INVALID_HANDLE_VALUE) return -1;
    }
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

// A final path without its namespace prefix, in place: \\?\D:\p is D:\p,
// and \\?\UNC\server\share\p is \\server\share\p.
static wchar_t* aether_sandbox_wunprefix(wchar_t* fin) {
    if (wcsncmp(fin, L"\\\\?\\UNC\\", 8) == 0) {
        fin[6] = L'\\';
        fin[7] = L'\\';
        return fin + 6;
    }
    if (wcsncmp(fin, L"\\\\?\\", 4) == 0) return fin + 4;
    return fin;
}

// The final path (UTF-8, no namespace prefix) of what handle `h` is open
// on: 1, or 0 when it has none.
static int aether_sandbox_final_of_handle(HANDLE h, char* out, size_t out_size) {
    wchar_t fin[AETHER_SANDBOX_WPATH];
    DWORD n = GetFinalPathNameByHandleW(h, fin, AETHER_SANDBOX_WPATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= AETHER_SANDBOX_WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, aether_sandbox_wunprefix(fin), -1, out, (int)out_size,
                               NULL, NULL) > 0;
}

// A Win32 device ("NUL", "CON", "COM1" and the rest): a name that
// GetFullPathNameW gives as "\\.\NAME", wherever it is spelt ("C:\box\NUL"
// is NUL too). It is no file, and no directory or link on the way changes
// what it reaches, so it is its own resource: "\\.\NUL" as it stands,
// covered by a grant that names it (or by "*"), as /dev/null is on POSIX.
static int aether_sandbox_wdevice(const wchar_t* full) {
    if (!(full[0] == L'\\' && full[1] == L'\\' && full[2] == L'.' && full[3] == L'\\' && full[4]))
        return 0;
    return wcschr(full + 4, L'\\') == NULL && wcschr(full + 4, L'/') == NULL;
}

// `in` (UTF-8) as GetFullPathNameW gives it: absolute, "." and ".."
// resolved by the text, trailing dots and spaces taken off its last name,
// as Windows itself treats a path before the file system sees it. Its
// length, or 0.
static DWORD aether_sandbox_wfull(const char* in, wchar_t* full) {
    wchar_t win[AETHER_SANDBOX_WPATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, win, AETHER_SANDBOX_WPATH)) return 0;
    DWORD fl = GetFullPathNameW(win, AETHER_SANDBOX_WPATH, full, NULL);
    return (fl == 0 || fl >= AETHER_SANDBOX_WPATH) ? 0 : fl;
}

// Returns 1 with the resolved absolute path (UTF-8) in out, or 0 if it
// cannot be resolved (out is then unspecified).
static int aether_sandbox_canon_path(const char* in, char* out, size_t out_size) {
    if (!in || !*in || !out || out_size < 4) return 0;
    wchar_t full[AETHER_SANDBOX_WPATH];
    wchar_t fin[AETHER_SANDBOX_WPATH];
    DWORD fl = aether_sandbox_wfull(in, full);
    if (fl == 0) return 0;
    if (aether_sandbox_wdevice(full))
        return WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)out_size, NULL, NULL) > 0;
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
    wchar_t* res = aether_sandbox_wunprefix(fin);
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

// ---------------------------------------------------------------------------
// Using a path as it was checked (#3751)
// ---------------------------------------------------------------------------
//
// A check resolves a path and matches where it leads; the call that follows
// used to be given the path as written, and the kernel resolved it again.
// Anything that changed in between (a directory swapped for a symlink by
// another thread or process) was followed by the second resolution and not
// the first, so the check passed for one file and the call reached another.
//
// So the call is made on the resolution itself. aether_sandbox_canon_path's
// result holds no symlink and no "." or "..", and aether_sandbox_walk opens
// it following none: on Linux with openat2 and RESOLVE_NO_SYMLINKS, one
// call that fails if any component has become a symlink; elsewhere, or
// where openat2 is missing or refused, one component at a time from "/",
// each directory opened with O_NOFOLLOW relative to the one before. A
// component that turned into a symlink since the check fails the call
// (EACCES): it is the race, and nothing is reached through it. Calls that
// act on a name in a directory (unlink, rename, mkdir and the rest) are made
// with the *at variant from the directory aether_sandbox_anchor opens the
// same way.
//
// Directories on the way are opened as anchors only: O_PATH on Linux,
// O_SEARCH where the system has it, else O_RDONLY. With O_RDONLY a directory
// that may be searched but not read cannot be an anchor; the walk then makes
// the call on the whole resolved path with O_NOFOLLOW, which still refuses a
// final symlink, and leaves only the intermediate directories to the race.
//
// The calls are made through `ops`: libc's own for the in-process checks,
// and for the LD_PRELOAD library the functions behind its wrappers, so its
// own steps are not checked again.

#include <fcntl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

// The open(2) flags an fopen() mode stands for: "r" O_RDONLY, "w"
// O_WRONLY|O_CREAT|O_TRUNC, "a" O_WRONLY|O_CREAT|O_APPEND, "+" O_RDWR, "x"
// O_EXCL, "e" O_CLOEXEC; read up to a ',' as aether_fopen_access reads it.
// -1 for a mode fopen refuses.
static int aether_fopen_flags(const char* mode) {
    if (!mode) return -1;
    int flags;
    if (mode[0] == 'r') flags = O_RDONLY;
    else if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else return -1;
    for (const char* m = mode + 1; *m && *m != ','; m++) {
        if (*m == '+') flags = (flags & ~O_ACCMODE) | O_RDWR;
        else if (*m == 'x') flags |= O_EXCL;
        else if (*m == 'e') flags |= O_CLOEXEC;
    }
    return flags;
}

typedef struct {
    int (*openat)(int, const char*, int, ...);
    int (*close)(int);
    // The raw system call, for openat2 (Linux); NULL to walk instead.
    long (*syscall)(long, ...);
} AetherSandboxFsOps;

#if defined(O_PATH)
#  define AETHER_SANDBOX_ANCHOR (O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#  define AETHER_SANDBOX_ANCHOR_SEARCH_ONLY 1
#elif defined(O_SEARCH)
#  define AETHER_SANDBOX_ANCHOR (O_SEARCH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#  define AETHER_SANDBOX_ANCHOR_SEARCH_ONLY 1
#else
#  define AETHER_SANDBOX_ANCHOR (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#  define AETHER_SANDBOX_ANCHOR_SEARCH_ONLY 0
#endif

#if defined(__linux__) && defined(SYS_openat2)
// struct open_how and RESOLVE_NO_SYMLINKS, from <linux/openat2.h>, which
// older C libraries do not ship.
struct aether_open_how {
    unsigned long long flags;
    unsigned long long mode;
    unsigned long long resolve;
};
#  define AETHER_RESOLVE_NO_SYMLINKS 0x04
// 1 opened (fd in *out), 0 refused because a symlink is on the way (errno
// EACCES), -1 failed otherwise (errno), 2 not available here: walk.
static int aether_sandbox_openat2(const char* resolved, int flags, unsigned mode,
                                  const AetherSandboxFsOps* ops, int* out) {
    static volatile int missing = 0;   // ENOSYS once: never again
    if (!ops->syscall || missing) return 2;
    struct aether_open_how how;
    memset(&how, 0, sizeof how);
    how.flags = (unsigned long long)(unsigned)flags;
    if (flags & O_CREAT
#  ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#  endif
    )
        how.mode = mode & 07777;
    how.resolve = AETHER_RESOLVE_NO_SYMLINKS;
    long fd = ops->syscall(SYS_openat2, (long)AT_FDCWD, resolved, &how, (long)sizeof how);
    if (fd >= 0) { *out = (int)fd; return 1; }
    if (errno == ENOSYS) { missing = 1; return 2; }
    // A seccomp filter, a flag openat2 rejects that openat ignores, a
    // struct this kernel does not know: walk instead.
    if (errno == EPERM || errno == EINVAL || errno == E2BIG || errno == EOPNOTSUPP) return 2;
    if (errno == ELOOP) { errno = EACCES; return 0; }
    return -1;
}
#endif

// Did the walk meet a symlink at `name` in `dirfd`, where the check saw none?
static int aether_sandbox_met_link(int dirfd, const char* name, int err) {
    if (err == ELOOP || err == EMLINK) return 1;
    if (err != ENOTDIR) return 0;
    struct stat st;
    return fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode);
}

#if defined(__wasi__)
#include <wasi/libc-find-relpath.h>
#endif

// Where a walk of `resolved` starts: "/" opened as an anchor, `*rest` the
// part of `resolved` below it. On WASI there is no "/" to open unless a
// preopened directory is "/": wasi-libc reaches a path only through the
// preopen that holds it, so the walk starts from that preopen
// (__wasilibc_find_abspath), which is not the caller's to close
// (`*owned` 0). The descriptor; -1 with errno; -2 when no preopen holds the
// path (the caller opens it whole, its end not followed).
static int aether_sandbox_walk_start(const char* resolved, const char** rest, int* owned,
                                     const AetherSandboxFsOps* ops) {
#if defined(__wasi__)
    (void)ops;
    const char* prefix = NULL;
    const char* rel = NULL;
    int pre = __wasilibc_find_abspath(resolved, &prefix, &rel);
    if (pre < 0) return -2;
    *owned = 0;
    *rest = (rel && strcmp(rel, ".") != 0) ? rel : "";
    while (**rest == '/') (*rest)++;
    return pre;
#else
    const char* p = resolved;
    while (*p == '/') p++;
    *rest = p;
    *owned = 1;
    return ops->openat(AT_FDCWD, "/", AETHER_SANDBOX_ANCHOR);
#endif
}

// Opens `resolved` (absolute, as aether_sandbox_canon_path gives it) with
// `flags` and `mode`, following no symlink on the way or at its end.
// Returns the descriptor, or -1 with errno set: EACCES when a component is
// a symlink now (the path changed since the check).
static int aether_sandbox_walk(const char* resolved, int flags, unsigned mode,
                               const AetherSandboxFsOps* ops) {
    if (!resolved || resolved[0] != '/') { errno = EINVAL; return -1; }
    // A caller that asked for O_NOFOLLOW itself gets ELOOP for a symlink at
    // the end, as the kernel says it, not the race's EACCES.
    int own_nofollow = (flags & O_NOFOLLOW) != 0;
    flags |= O_NOFOLLOW;
#if defined(__linux__) && defined(SYS_openat2)
    if (!own_nofollow) {
        int fd = -1;
        int r = aether_sandbox_openat2(resolved, flags, mode, ops, &fd);
        if (r == 1) return fd;
        if (r == 0 || r == -1) return -1;
    }
#endif
    const char* p = resolved;
    int owned = 1;   // the walk's first directory is its own to close
    int dir = aether_sandbox_walk_start(resolved, &p, &owned, ops);
    if (dir == -2) return ops->openat(AT_FDCWD, resolved, flags, mode);
    if (dir < 0) return -1;
    if (!*p) {
        // The start itself ("/", or a preopened directory on WASI).
        int fd = ops->openat(dir, ".", flags & ~O_NOFOLLOW, mode);
        int err = errno;
        if (owned) ops->close(dir);
        errno = err;
        return fd;
    }
    char name[PATH_MAX];
    for (;;) {
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n >= sizeof name) { if (owned) ops->close(dir); errno = ENAMETOOLONG; return -1; }
        memcpy(name, p, n);
        name[n] = '\0';
        while (*e == '/') e++;
        int last = *e == '\0';
        int fd = last ? ops->openat(dir, name, flags, mode)
                      : ops->openat(dir, name, AETHER_SANDBOX_ANCHOR);
        if (fd < 0) {
            int err = errno;
            int link = aether_sandbox_met_link(dir, name, err);
            if (owned) ops->close(dir);
            if (link) { errno = last && own_nofollow ? ELOOP : EACCES; return -1; }
            if (!last && err == EACCES && !AETHER_SANDBOX_ANCHOR_SEARCH_ONLY) {
                // A directory that may be searched but not read: no anchor
                // on this system, so the whole path, its end not followed.
                return ops->openat(AT_FDCWD, resolved, flags, mode);
            }
            errno = err;
            return -1;
        }
        if (owned) ops->close(dir);
        if (last) return fd;
        dir = fd;
        owned = 1;
        p = e;
    }
}

// The directory `resolved` is in, opened by aether_sandbox_walk as an
// anchor, and its last name copied to `name`: a call on a name in a
// directory (unlinkat, mkdirat, renameat...) is made from there. -1 with
// errno set when there is no such directory, or `resolved` is "/" (EBUSY).
static int aether_sandbox_anchor(const char* resolved, char* name, size_t cap,
                                 const AetherSandboxFsOps* ops) {
    if (!resolved || resolved[0] != '/') { errno = EINVAL; return -1; }
    size_t len = strlen(resolved);
    while (len > 1 && resolved[len - 1] == '/') len--;
    size_t s = len;
    while (s > 0 && resolved[s - 1] != '/') s--;
    if (s == len) { errno = EBUSY; return -1; }   // "/" itself
    size_t n = len - s;
    if (n + 1 > cap || s >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    memcpy(name, resolved + s, n);
    name[n] = '\0';
    char parent[PATH_MAX];
    size_t pl = s > 1 ? s - 1 : 1;   // "/a/b" -> "/a"; "/a" -> "/"
    memcpy(parent, resolved, pl);
    parent[pl] = '\0';
    // O_PATH and O_SEARCH anchors already say "directory" and "not
    // followed"; O_NOFOLLOW is added by the walk.
    return aether_sandbox_walk(parent, AETHER_SANDBOX_ANCHOR & ~O_NOFOLLOW, 0, ops);
}


// A path that ends in a separator after a name ("/box/new/", not "/" or
// "x/.."): the kernel takes that name for a directory. aether_sandbox_canon_
// entry and aether_sandbox_canon_path drop the separator, so a call made on
// their result would act on "new" as whatever it is; each call on a name
// asks this first and keeps to what the kernel does with "new/". 1 when
// `in` has such a separator.
static int aether_sandbox_dir_slash(const char* in) {
    if (!in) return 0;
    size_t n = strlen(in);
    if (n < 2 || in[n - 1] != '/') return 0;
    while (n > 0 && in[n - 1] == '/') n--;
    if (n == 0) return 0;   // "/" itself
    size_t s = n;
    while (s > 0 && in[s - 1] != '/') s--;
    size_t len = n - s;
    if ((len == 1 && in[s] == '.') || (len == 2 && in[s] == '.' && in[s + 1] == '.')) return 0;
    return 1;
}

// What Linux does with "name/" for a call on the name itself, the name not
// followed (`name` in `dirfd`: an anchor and its name, or AT_FDCWD and the
// checked resolution): AETHER_SLASH_UNLINK never removes it (ENOENT when it is not
// there, EISDIR for a directory, ENOTDIR for anything else, a symlink to a
// directory included); AETHER_SLASH_DIR goes on only for a directory
// (rmdir, rename: ENOENT, or ENOTDIR for a symlink or a file);
// AETHER_SLASH_NEW makes no new non-directory (symlink, link, mknod:
// EEXIST when the name is there, else ENOENT). 0 to go on, -1 with errno.
#define AETHER_SLASH_UNLINK 1
#define AETHER_SLASH_DIR    2
#define AETHER_SLASH_NEW    3
static int aether_sandbox_slash_call(int dirfd, const char* name, int op) {
    struct stat st;
    int there = fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (op == AETHER_SLASH_NEW) {
        errno = there ? EEXIST : ENOENT;
        return -1;
    }
    if (!there) { errno = ENOENT; return -1; }
    if (op == AETHER_SLASH_UNLINK) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : ENOTDIR;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

#if !defined(__wasi__)
// chmod of `name` in the directory `dirfd`, not following it: fchmodat with
// AT_SYMLINK_NOFOLLOW. Where the C library cannot do that (glibc before
// 2.32 says ENOTSUP), the name is opened without following it and changed
// through that: by /proc/self/fd/N (chmod follows that magic link to the
// file opened, and an O_PATH descriptor takes no fchmod), else through a
// descriptor opened for reading or writing (fchmod). Only when the file can
// be opened neither way is it changed by name after a look, leaving that
// name alone to a swap between the two. A symlink found at the name fails
// with EACCES: the name was resolved, so one there now is the race.
// `fchmodat_fn` and `chmod_fn` are libc's, or the functions behind the
// preload's wrappers.
static int aether_sandbox_chmod_at(int dirfd, const char* name, mode_t mode,
                                   const AetherSandboxFsOps* ops,
                                   int (*fchmodat_fn)(int, const char*, mode_t, int),
                                   int (*chmod_fn)(const char*, mode_t)) {
    if (fchmodat_fn(dirfd, name, mode, AT_SYMLINK_NOFOLLOW) == 0) return 0;
    if (errno != ENOTSUP && errno != EOPNOTSUPP && errno != EINVAL) return -1;
    struct stat st;
#if defined(O_PATH) && defined(__linux__)
    int pfd = ops->openat(dirfd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (pfd >= 0) {
        int r = -1;
        if (fstat(pfd, &st) == 0 && S_ISLNK(st.st_mode)) {
            errno = EACCES;
        } else {
            char self[64];
            snprintf(self, sizeof self, "/proc/self/fd/%d", pfd);
            r = chmod_fn(self, mode);
        }
        int err = errno;
        ops->close(pfd);
        errno = err;
        if (r == 0 || errno != ENOENT) return r;   // ENOENT: no /proc
    }
#else
    (void)chmod_fn;
#endif
    // O_NONBLOCK: a FIFO there opens without waiting for a writer (or a
    // reader); O_NOCTTY: a terminal does not become the controlling one.
    int fd = ops->openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0 && (errno == EACCES || errno == EISDIR))
        fd = ops->openat(dirfd, name, O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd >= 0) {
        int r = fchmod(fd, mode);
        int err = errno;
        ops->close(fd);
        errno = err;
        return r;
    }
    if (errno == ELOOP || errno == EMLINK) { errno = EACCES; return -1; }
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if (S_ISLNK(st.st_mode)) { errno = EACCES; return -1; }
    return fchmodat_fn(dirfd, name, mode, 0);
}
#endif // !__wasi__
#endif // _WIN32

// A path whose last name is the thing a call acts on, not where it leads:
// unlink, rmdir, rename, the new name of link and symlink, mkdir, and an
// open with O_NOFOLLOW act on a symlink itself. Its directory is resolved
// (aether_sandbox_canon_path) and the last name kept as written. A last
// name of "." or ".." is resolved with the rest. Checking where such a link
// leads would refuse removing a link that points out of a grant, and allow
// removing one outside the grant that points in (#3751).
static int aether_sandbox_canon_entry(const char* in, char* out, size_t out_size) {
    if (!in || !*in || !out) return 0;
    size_t len = strlen(in);
#ifdef _WIN32
#  define AETHER_SBX_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#  define AETHER_SBX_IS_SEP(c) ((c) == '/')
#endif
    while (len > 1 && AETHER_SBX_IS_SEP(in[len - 1])) len--;
    size_t s = len;
    while (s > 0 && !AETHER_SBX_IS_SEP(in[s - 1])) s--;
    size_t n = len - s;
    const char* name = in + s;
    if (n == 0 || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.'))
        return aether_sandbox_canon_path(in, out, out_size);
#ifdef _WIN32
    // The whole path as Windows takes it first (GetFullPathNameW), then
    // split: the last name is the one a Win32 call would act on, its
    // trailing dots and spaces gone ("new." is "new"), and the directory
    // is resolved as aether_sandbox_canon_path resolves the whole path
    // ("sub \x" is in "sub ", not "sub"). Taken as written, the name went
    // to NtCreateFile and NtSetInformationFile, which made "new." and
    // "sp " literally and a file called "CON", none of which a Win32 call
    // can reach again. A device name is the device (aether_sandbox_wdevice).
    {
        wchar_t full[AETHER_SANDBOX_WPATH];
        DWORD fl = aether_sandbox_wfull(in, full);
        if (fl == 0) return 0;
        if (aether_sandbox_wdevice(full))
            return WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)out_size, NULL, NULL) > 0;
        size_t root = aether_sandbox_wroot(full);
        if (root == 0) return 0;
        size_t wl = fl;
        while (wl > root && full[wl - 1] == L'\\') wl--;
        full[wl] = L'\0';
        if (wl <= root) return aether_sandbox_canon_path(in, out, out_size);   // a root
        size_t ws = wl;
        while (ws > root && full[ws - 1] != L'\\') ws--;
        // full[0..ws) is the directory with its separator; a root keeps it.
        wchar_t wname[AETHER_SANDBOX_WPATH];
        memcpy(wname, full + ws, (wl - ws + 1) * sizeof(wchar_t));
        full[ws > root ? ws - 1 : root] = L'\0';
        char udir[AETHER_SANDBOX_PATH_MAX];
        char uname[AETHER_SANDBOX_PATH_MAX];
        char rdir[AETHER_SANDBOX_PATH_MAX];
        if (WideCharToMultiByte(CP_UTF8, 0, full, -1, udir, (int)sizeof udir, NULL, NULL) <= 0 ||
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, uname, (int)sizeof uname, NULL, NULL) <= 0 ||
            !aether_sandbox_canon_path(udir, rdir, sizeof rdir))
            return 0;
        size_t rl = strlen(rdir);
        size_t ul = strlen(uname);
        int sep = !(rl > 0 && rdir[rl - 1] == '\\');
        if (rl + (size_t)sep + ul + 1 > out_size) return 0;
        memcpy(out, rdir, rl);
        if (sep) out[rl++] = '\\';
        memcpy(out + rl, uname, ul + 1);
        return 1;
    }
#else
    char dir[PATH_MAX];
    if (s == 0) {
        dir[0] = '.';
        dir[1] = '\0';
    } else {
        size_t dl = s > 1 ? s - 1 : 1;   // "/x" -> "/"
        if (dl >= sizeof dir) return 0;
        memcpy(dir, in, dl);
        dir[dl] = '\0';
    }
    char rdir[PATH_MAX];
    if (!aether_sandbox_canon_path(dir, rdir, sizeof rdir)) return 0;
    size_t rl = strlen(rdir);
    int sep = !(rl > 0 && AETHER_SBX_IS_SEP(rdir[rl - 1]));
    if (rl + (size_t)sep + n + 1 > out_size) return 0;
    memcpy(out, rdir, rl);
    if (sep) out[rl++] = '/';
    memcpy(out + rl, name, n);
    out[rl + n] = '\0';
    return 1;
#endif
#undef AETHER_SBX_IS_SEP
}

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
    char dir[AETHER_SANDBOX_PATH_MAX];
    size_t dl = (size_t)(slash - pat);
    if (dl == 0) { dir[0] = '/'; dl = 1; }
    else {
        if (dl >= sizeof(dir)) return 0;
        memcpy(dir, pat, dl);
    }
    dir[dl] = '\0';
    char rdir[AETHER_SANDBOX_PATH_MAX];
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
