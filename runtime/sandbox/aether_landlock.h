// aether_landlock.h: Landlock rules from grant patterns, shared by the
// kernel layer of a sandboxed spawn (aether_sandbox_kernel_linux.c) and the
// LD_PRELOAD library's backstop for a permanent policy
// (libaether_sandbox_preload.c, #3751). Header-only, as the preload is built
// on its own. Linux only.
#ifndef AETHER_LANDLOCK_H
#define AETHER_LANDLOCK_H
#if defined(__linux__) && !defined(__ANDROID__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

// A ruleset being filled: its descriptor and the fs rights it handles.
typedef struct {
    int fd;
    uint64_t fs;
} AetherLandlock;

// ---- Landlock ----
//
// Landlock (Linux 5.13+) restricts what files this process and its
// children may open, by the kernel and for an unprivileged process (no root,
// no user namespace). The grants become its rules, so a raw openat that the
// preload never sees is refused too. Network rules (bind/connect on TCP
// ports) are ABI 4 (Linux 6.7); scoping of abstract Unix sockets and signals
// is ABI 6 (Linux 6.12).
//
// The syscall numbers are the same on every architecture (444-446), and the
// ABI structs are spelt here, so the build needs no <linux/landlock.h>.
//
// How grants map (docs/containment-sandbox.md, "Landlock"):
//   fs_read   READ_FILE, READ_DIR
//   fs_write  WRITE_FILE, TRUNCATE, REMOVE_*, MAKE_*, REFER
//   fs        both
//   exec      EXECUTE, READ_FILE on the program the pattern names, and on
//             what it needs to start: its ELF interpreter (and READ_FILE
//             on the libraries beside it), a "#!" script's interpreter
//   tcp       CONNECT_TCP on the pattern's port (ABI 4); a pattern with no
//             port leaves connect to the preload and seccomp
//   tcp_listen BIND_TCP is then not restricted
// and how patterns map (an fs pattern as the libc check matches it: through
// no symlink, so one spelt through a symlink, or an entry of "/dir/name*"
// that is a symlink, gets no rule): "*" is "/"; "/dir/*" is the directory and what is
// under it; "/dir/name*" is each entry of /dir whose name starts with
// "name" when the child starts (later ones are not covered: narrower);
// an exact path is that file (an exact directory gets no rule, since a
// Landlock rule on a directory covers what is under it: narrower); a
// pattern that starts with "*" (a suffix) cannot be expressed and is an
// error under AETHER_SBX_STRICT.

#define LL_CREATE_RULESET 444
#define LL_ADD_RULE       445
#define LL_RESTRICT_SELF  446
#define LL_CREATE_RULESET_VERSION 1u
#define LL_RULE_PATH_BENEATH 1
#define LL_RULE_NET_PORT     2

#define LL_FS_EXECUTE     (1ULL << 0)
#define LL_FS_WRITE_FILE  (1ULL << 1)
#define LL_FS_READ_FILE   (1ULL << 2)
#define LL_FS_READ_DIR    (1ULL << 3)
#define LL_FS_REMOVE_DIR  (1ULL << 4)
#define LL_FS_REMOVE_FILE (1ULL << 5)
#define LL_FS_MAKE_CHAR   (1ULL << 6)
#define LL_FS_MAKE_DIR    (1ULL << 7)
#define LL_FS_MAKE_REG    (1ULL << 8)
#define LL_FS_MAKE_SOCK   (1ULL << 9)
#define LL_FS_MAKE_FIFO   (1ULL << 10)
#define LL_FS_MAKE_BLOCK  (1ULL << 11)
#define LL_FS_MAKE_SYM    (1ULL << 12)
#define LL_FS_REFER       (1ULL << 13)   /* ABI 2 */
#define LL_FS_TRUNCATE    (1ULL << 14)   /* ABI 3 */
#define LL_NET_BIND_TCP    (1ULL << 0)   /* ABI 4 */
#define LL_NET_CONNECT_TCP (1ULL << 1)
#define LL_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)   /* ABI 6 */
#define LL_SCOPE_SIGNAL               (1ULL << 1)

#define LL_FS_V1 (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE | LL_FS_READ_DIR | \
                  LL_FS_REMOVE_DIR | LL_FS_REMOVE_FILE | LL_FS_MAKE_CHAR | LL_FS_MAKE_DIR | \
                  LL_FS_MAKE_REG | LL_FS_MAKE_SOCK | LL_FS_MAKE_FIFO | LL_FS_MAKE_BLOCK | \
                  LL_FS_MAKE_SYM)
/* The rights a rule on a file (not a directory) may carry. */
#define LL_FS_FILE (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE | LL_FS_TRUNCATE)

#define LL_READ  (LL_FS_READ_FILE | LL_FS_READ_DIR)
#define LL_WRITE (LL_FS_WRITE_FILE | LL_FS_TRUNCATE | LL_FS_REMOVE_DIR | LL_FS_REMOVE_FILE | \
                  LL_FS_MAKE_CHAR | LL_FS_MAKE_DIR | LL_FS_MAKE_REG | LL_FS_MAKE_SOCK | \
                  LL_FS_MAKE_FIFO | LL_FS_MAKE_BLOCK | LL_FS_MAKE_SYM | LL_FS_REFER)
#define LL_EXEC  (LL_FS_EXECUTE | LL_FS_READ_FILE)

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
    uint64_t scoped;
};
struct ll_path_beneath_attr {
    uint64_t allowed_access;
    int32_t parent_fd;
} __attribute__((packed));
struct ll_net_port_attr {
    uint64_t allowed_access;
    uint64_t port;
};

// Always four arguments: landlock_add_rule's fourth is its flags, which
// must be 0, and syscall() would otherwise pass whatever the register held.
static inline long aether_landlock_syscall(long nr, long a, long b, long c) {
    return syscall(nr, a, b, c, 0L);
}

// The kernel's Landlock ABI, or 0 when it has none (not built, or not in
// the lsm= list: ENOSYS, EOPNOTSUPP).
static inline int aether_landlock_abi(void) {
    long abi = aether_landlock_syscall(LL_CREATE_RULESET, 0, 0, LL_CREATE_RULESET_VERSION);
    return abi > 0 ? (int)abi : 0;
}

// A rule on what `fd` (an O_PATH descriptor) is open on, with `rights`
// (masked to what the ruleset handles, and to a file's rights when it is
// not a directory). 0, or -1 with errno. `fd` is closed.
static inline int aether_landlock_rule_fd(const AetherLandlock* k, int fd, uint64_t rights) {
    rights &= k->fs;
    struct stat st;
    if (!rights) { close(fd); return 0; }
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISDIR(st.st_mode)) rights &= LL_FS_FILE;
    int rc = 0;
    if (rights) {
        struct ll_path_beneath_attr pb = { .allowed_access = rights, .parent_fd = fd };
        if (aether_landlock_syscall(LL_ADD_RULE, k->fd, LL_RULE_PATH_BENEATH, (long)&pb) != 0) rc = -1;
    }
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

// A rule on where `path` leads, every symlink on it followed, with
// `rights`: for what the kernel itself follows (a program and its
// interpreters, the loader's floor, a file the runtime names). A path that
// does not exist gets no rule. 0, or -1 with errno.
static inline int aether_landlock_rule(const AetherLandlock* k, const char* path, uint64_t rights) {
    if (!(rights & k->fs)) return 0;
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT || errno == ENOTDIR || errno == EACCES) ? 0 : -1;
    return aether_landlock_rule_fd(k, fd, rights);
}

// `path` opened O_PATH with no symlink followed anywhere on it, as an fs
// grant matches a path: against its resolution, which has no symlink, no
// "." or "..", and no empty name in it. A grant spelt through a symlink
// matches nothing the libc check sees, so it may not reach, as a Landlock
// rule, the tree the symlink points at. The descriptor, or -1 (no rule).
static inline int aether_landlock_open_exact(const char* path) {
    if (!path || path[0] != '/') { errno = ENOENT; return -1; }
    int dir = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0 || !path[1]) return dir;
    const char* p = path + 1;
    char name[NAME_MAX + 1];
    for (;;) {
        const char* e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int last = !e || !e[1];
        if (n == 0 || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            close(dir);
            errno = ENOENT;
            return -1;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        int fd = openat(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY));
        close(dir);
        if (fd < 0) return -1;
        if (last) {
            struct stat st;
            if (fstat(fd, &st) != 0 || S_ISLNK(st.st_mode)) {
                close(fd);
                errno = ELOOP;
                return -1;
            }
            return fd;
        }
        dir = fd;
        p = e + 1;
    }
}

static inline int aether_landlock_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// The rules for one pattern (see the mapping above). `follow` 0 for an fs
// grant: matched as the libc check matches it (aether_landlock_open_exact;
// an entry of "/dir/name*" that is a symlink gets no rule, as a path
// through it resolves elsewhere); 1 for an exec grant, whose program the
// kernel reaches through any symlink. 0; 1 when it cannot be expressed (a
// suffix pattern); -1 with errno on a failure.
static inline int aether_landlock_pattern_as(const AetherLandlock* k, const char* pat, uint64_t rights,
                                            int follow) {
    size_t n = strlen(pat);
    if (n == 0) return 0;
    if (strcmp(pat, "*") == 0) return aether_landlock_rule(k, "/", rights);
    if (pat[0] == '*') return 1;
    if (pat[n - 1] != '*') {
        // Exact: a file. An exact directory is narrowed to nothing.
        if (follow) {
            if (aether_landlock_is_dir(pat)) return 0;
            return aether_landlock_rule(k, pat, rights);
        }
        int fd = aether_landlock_open_exact(pat);
        if (fd < 0) return 0;
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); return 0; }
        return aether_landlock_rule_fd(k, fd, rights);
    }
    char prefix[PATH_MAX];
    if (n - 1 >= sizeof prefix) { errno = ENAMETOOLONG; return -1; }
    memcpy(prefix, pat, n - 1);
    prefix[n - 1] = '\0';
    if (strchr(prefix, '*')) return 1;
    size_t pn = n - 1;
    if (pn > 0 && prefix[pn - 1] == '/') {
        // "/dir/*": the directory and what is under it.
        if (pn > 1) prefix[pn - 1] = '\0';
        if (follow) return aether_landlock_is_dir(prefix) ? aether_landlock_rule(k, prefix, rights) : 0;
        int fd = aether_landlock_open_exact(prefix);
        if (fd < 0) return 0;
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) { close(fd); return 0; }
        return aether_landlock_rule_fd(k, fd, rights);
    }
    // "/dir/name*": each entry of /dir whose name starts with "name" now.
    char* slash = strrchr(prefix, '/');
    if (!slash) return 1;
    char dir[PATH_MAX];
    const char* name = slash + 1;
    size_t dn = (size_t)(slash - prefix);
    if (dn == 0) { dir[0] = '/'; dir[1] = '\0'; }
    else { memcpy(dir, prefix, dn); dir[dn] = '\0'; }
    if (!follow) {
        int dfd = aether_landlock_open_exact(dir);
        if (dfd < 0) return 0;
        close(dfd);
    }
    size_t namelen = strlen(name);
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strncmp(e->d_name, name, namelen) != 0) continue;
        if (!follow) {
            int fd = openat(dirfd(d), e->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) continue;
            struct stat st;
            if (fstat(fd, &st) != 0 || S_ISLNK(st.st_mode)) { close(fd); continue; }
            if (aether_landlock_rule_fd(k, fd, rights) != 0) { rc = -1; break; }
            continue;
        }
        char entry[PATH_MAX];
        int w = snprintf(entry, sizeof entry, "%s%s%s", dir, dn ? "/" : "", e->d_name);
        if (w < 0 || (size_t)w >= sizeof entry) continue;
        if (aether_landlock_rule(k, entry, rights) != 0) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

// An fs grant's pattern.
static inline int aether_landlock_pattern(const AetherLandlock* k, const char* pat, uint64_t rights) {
    return aether_landlock_pattern_as(k, pat, rights, 0);
}

// ---- What a program needs to start ----
//
// The kernel's exec (open_exec) needs EXECUTE on the program, on its ELF
// interpreter (PT_INTERP, the dynamic loader) and, for a "#!" script, on
// the script's interpreter; the loader then reads the libraries beside it.
// A rule on the program alone lets nothing dynamically linked start. Shared
// by a spawn's child (its own program) and by the exec grants of both the
// spawn and the LD_PRELOAD library's permanent policy.

static inline uint64_t aether_landlock_elf_rd(const unsigned char* p, int bytes, int le) {
    uint64_t v = 0;
    for (int b = 0; b < bytes; b++)
        v |= (uint64_t)p[le ? b : bytes - 1 - b] << (8 * b);
    return v;
}

// The ELF interpreter (PT_INTERP) of `path` into `out`: 1, or 0 when it has
// none (static, not ELF).
static inline int aether_landlock_elf_interp(const char* path, char* out, size_t outsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char h[64];
    int found = 0;
    if (pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && memcmp(h, "\177ELF", 4) == 0) {
        int is64 = h[4] == 2, le = h[5] == 1;
        uint64_t phoff = is64 ? aether_landlock_elf_rd(h + 32, 8, le) : aether_landlock_elf_rd(h + 28, 4, le);
        uint64_t phentsize = aether_landlock_elf_rd(h + (is64 ? 54 : 42), 2, le);
        uint64_t phnum = aether_landlock_elf_rd(h + (is64 ? 56 : 44), 2, le);
        for (uint64_t i = 0; i < phnum && i < 128 && !found; i++) {
            unsigned char ph[64];
            if (phentsize > sizeof ph || phentsize < (is64 ? 56u : 32u)) break;
            if (pread(fd, ph, (size_t)phentsize, (off_t)(phoff + i * phentsize)) != (ssize_t)phentsize)
                break;
            if (aether_landlock_elf_rd(ph, 4, le) != 3 /* PT_INTERP */) continue;
            uint64_t off = is64 ? aether_landlock_elf_rd(ph + 8, 8, le) : aether_landlock_elf_rd(ph + 4, 4, le);
            uint64_t sz = is64 ? aether_landlock_elf_rd(ph + 32, 8, le) : aether_landlock_elf_rd(ph + 16, 4, le);
            if (sz == 0 || sz >= outsz) break;
            if (pread(fd, out, (size_t)sz, (off_t)off) == (ssize_t)sz) {
                out[sz] = '\0';
                found = out[0] == '/';
            }
        }
    }
    close(fd);
    return found;
}

// The ELF interpreter of `path`, executable, and the loader's floor: the
// libraries beside it, read only and not listed (the multiarch lib
// directory on Debian and Ubuntu, /usr/lib on Arch and Fedora). 1 when
// `path` has an interpreter, 0 when not, -1 with errno.
static inline int aether_landlock_interp(const AetherLandlock* k, const char* path) {
    char interp[PATH_MAX];
    if (!aether_landlock_elf_interp(path, interp, sizeof interp)) return 0;
    if (aether_landlock_rule(k, interp, LL_EXEC) != 0) return -1;
    char real[PATH_MAX];
    if (realpath(interp, real)) {
        char* slash = strrchr(real, '/');
        if (slash && slash != real) {
            *slash = '\0';
            if (aether_landlock_rule(k, real, LL_FS_READ_FILE) != 0) return -1;
        }
    }
    return 1;
}

// The program at `path` and what it needs to start: its ELF interpreter, or
// a "#!" script's interpreter (and that one's), or, for a file that is
// neither, `sh_path`, which a spawn runs it under. `depth` stops a "#!"
// chain. 0, or -1 with errno.
static inline int aether_landlock_program(const AetherLandlock* k, const char* path, const char* sh_path,
                                          int depth) {
    if (aether_landlock_rule(k, path, LL_EXEC) != 0) return -1;
    int r = aether_landlock_interp(k, path);
    if (r != 0) return r < 0 ? -1 : 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char head[PATH_MAX + 2];
    ssize_t n = fd >= 0 ? pread(fd, head, sizeof head - 1, 0) : -1;
    if (fd >= 0) close(fd);
    if (n > 2 && head[0] == '#' && head[1] == '!') {
        head[n] = '\0';
        char* p = head + 2;
        while (*p == ' ' || *p == '\t') p++;
        p[strcspn(p, " \t\r\n")] = '\0';
        if (p[0] == '/' && depth < 4) return aether_landlock_program(k, p, sh_path, depth + 1);
        return 0;
    }
    if (n >= 4 && memcmp(head, "\177ELF", 4) == 0) return 0;   // static ELF
    // Neither: the spawn runs it under the shell.
    if (sh_path && depth == 0) return aether_landlock_program(k, sh_path, NULL, depth + 1);
    return 0;
}

// The floor a running program needs past its own image and its grants,
// read only: the loader's cache and the locale archive (a later dlopen,
// setlocale), /dev/urandom, and /dev/null (read and write); with them the
// LD_PRELOAD library `preload` and the grant file `grants_path` when named
// (NULL: none), which a child that keeps the preload reads before its main.
// 0, or -1 with errno and the path that failed in `*failed`.
static inline int aether_landlock_floor(const AetherLandlock* k, const char* preload, const char* grants_path,
                                        const char** failed) {
    static const char* const floor_read[] = { "/etc/ld.so.cache", "/usr/lib/locale", "/dev/urandom" };
    for (size_t i = 0; i < sizeof floor_read / sizeof floor_read[0]; i++) {
        if (aether_landlock_rule(k, floor_read[i], LL_FS_READ_FILE) != 0) { *failed = floor_read[i]; return -1; }
    }
    if (aether_landlock_rule(k, "/dev/null", LL_FS_READ_FILE | LL_FS_WRITE_FILE) != 0) {
        *failed = "/dev/null";
        return -1;
    }
    if (preload && aether_landlock_rule(k, preload, LL_FS_READ_FILE) != 0) { *failed = preload; return -1; }
    if (grants_path && aether_landlock_rule(k, grants_path, LL_FS_READ_FILE) != 0) {
        *failed = grants_path;
        return -1;
    }
    return 0;
}

// Does a tcp grant's pattern name a host by name ("example.com:443", "*"),
// which the C library resolves, rather than an address?
static inline int aether_landlock_names_host(const char* tcp_pattern) {
    const char* p = tcp_pattern;
    if (*p == '[') return 0;   // "[v6]:port"
    for (; *p && *p != ':'; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) return 1;
    }
    return p == tcp_pattern;   // ":port" alone: any host
}

// What the C library reads to resolve a host name, through its own
// internal open that no wrapper sees: read only. A file that is not there
// gets no rule. 0, or -1 with errno and the path in `*failed`.
static inline int aether_landlock_resolver(const AetherLandlock* k, const char** failed) {
    static const char* const files[] = { "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
                                         "/etc/host.conf", "/etc/gai.conf", "/etc/localtime" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (aether_landlock_rule(k, files[i], LL_FS_READ_FILE) != 0) { *failed = files[i]; return -1; }
    }
    return 0;
}

// An exec grant's command pattern ("python3", "/usr/bin/*", "echo *", "*"):
// its first word names the program. A program it names (a path, or a bare
// name found on PATH by `resolve`) gets what it needs to start
// (aether_landlock_program); a pattern of programs gets EXECUTE on what it
// covers, the kernel following symlinks to them as it does.
static inline int aether_landlock_exec_pattern(const AetherLandlock* k, const char* pat,
                                              char* (*resolve)(const char*)) {
    char word[PATH_MAX];
    size_t n = strcspn(pat, " \t");
    if (n == 0 || n >= sizeof word) return n == 0 ? 0 : 1;
    memcpy(word, pat, n);
    word[n] = '\0';
    if (word[0] == '/' && !strchr(word, '*')) return aether_landlock_program(k, word, NULL, 0);
    if (strcmp(word, "*") == 0 || strchr(word, '/')) {
        char canon[PATH_MAX];
        const char* use = word;
        // Resolve the directory part as fs grants are resolved.
        if (word[0] == '/' && word[n - 1] == '*' && n > 1) {
            char head[PATH_MAX];
            memcpy(head, word, n - 1);
            head[n - 1] = '\0';
            char* slash = strrchr(head, '/');
            if (slash) {
                *slash = '\0';
                char real[PATH_MAX];
                if (realpath(head[0] ? head : "/", real)) {
                    int w = snprintf(canon, sizeof canon, "%s/%s", strcmp(real, "/") ? real : "",
                                     slash + 1);
                    if (w > 0 && (size_t)w < sizeof canon) {
                        size_t cl = strlen(canon);
                        if (cl + 1 < sizeof canon) { canon[cl] = '*'; canon[cl + 1] = '\0'; use = canon; }
                    }
                }
            }
        }
        return aether_landlock_pattern_as(k, use, LL_EXEC, 1);
    }
    char* path = resolve(word);   // malloc'd, or NULL: not found
    if (!path) return 0;
    int rc = aether_landlock_program(k, path, NULL, 0);
    free(path);
    return rc;
}

#endif // __linux__ && !__ANDROID__
#endif // AETHER_LANDLOCK_H
