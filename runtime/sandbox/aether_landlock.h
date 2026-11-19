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
//   exec      EXECUTE, READ_FILE on the program the pattern names
//   tcp       CONNECT_TCP on the pattern's port (ABI 4); a pattern with no
//             port leaves connect to the preload and seccomp
//   tcp_listen BIND_TCP is then not restricted
// and how patterns map: "*" is "/"; "/dir/*" is the directory and what is
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

// A rule on `path` with `rights` (masked to what the ruleset handles, and to
// a file's rights when it is not a directory). A path that does not exist
// gets no rule. 0, or -1 with errno.
static inline int aether_landlock_rule(const AetherLandlock* k, const char* path, uint64_t rights) {
    rights &= k->fs;
    if (!rights) return 0;
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT || errno == ENOTDIR || errno == EACCES) ? 0 : -1;
    struct stat st;
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

static inline int aether_landlock_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// The rules for one fs-style pattern (see the mapping above). 0; 1 when it
// cannot be expressed (a suffix pattern); -1 with errno on a failure.
static inline int aether_landlock_pattern(const AetherLandlock* k, const char* pat, uint64_t rights) {
    size_t n = strlen(pat);
    if (n == 0) return 0;
    if (strcmp(pat, "*") == 0) return aether_landlock_rule(k, "/", rights);
    if (pat[0] == '*') return 1;
    if (pat[n - 1] != '*') {
        // Exact: a file. An exact directory is narrowed to nothing.
        if (aether_landlock_is_dir(pat)) return 0;
        return aether_landlock_rule(k, pat, rights);
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
        return aether_landlock_is_dir(prefix) ? aether_landlock_rule(k, prefix, rights) : 0;
    }
    // "/dir/name*": each entry of /dir whose name starts with "name" now.
    char* slash = strrchr(prefix, '/');
    if (!slash) return 1;
    char dir[PATH_MAX];
    const char* name = slash + 1;
    size_t dn = (size_t)(slash - prefix);
    if (dn == 0) { dir[0] = '/'; dir[1] = '\0'; }
    else { memcpy(dir, prefix, dn); dir[dn] = '\0'; }
    size_t namelen = strlen(name);
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strncmp(e->d_name, name, namelen) != 0) continue;
        char entry[PATH_MAX];
        int w = snprintf(entry, sizeof entry, "%s%s%s", dir, dn ? "/" : "", e->d_name);
        if (w < 0 || (size_t)w >= sizeof entry) continue;
        if (aether_landlock_rule(k, entry, rights) != 0) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

// An exec grant's command pattern ("python3", "/usr/bin/*", "echo *", "*"):
// its first word names the program.
static inline int aether_landlock_exec_pattern(const AetherLandlock* k, const char* pat,
                                              char* (*resolve)(const char*)) {
    char word[PATH_MAX];
    size_t n = strcspn(pat, " \t");
    if (n == 0 || n >= sizeof word) return n == 0 ? 0 : 1;
    memcpy(word, pat, n);
    word[n] = '\0';
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
        return aether_landlock_pattern(k, use, LL_EXEC);
    }
    char* path = resolve(word);   // malloc'd, or NULL: not found
    if (!path) return 0;
    int rc = aether_landlock_rule(k, path, LL_EXEC);
    free(path);
    return rc;
}


#endif // __linux__ && !__ANDROID__
#endif // AETHER_LANDLOCK_H
