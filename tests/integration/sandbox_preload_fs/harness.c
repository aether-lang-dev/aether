// Makes one file-system call the way a case of test_sandbox_preload_fs.sh
// asks, under the LD_PRELOAD library, and reports the outcome in the exit
// status: 0 done, 3 refused with EACCES (the sandbox's refusal), 4 any other
// failure, 5 it reached something outside the box (the escape the case is
// about), 6 EEXIST, 7 not on this system, 8 ENOTDIR, 9 EISDIR, 10 EPERM
// from unshare, 2 a usage error.
//
//   harness <case> <root>
//
// <root> holds box/ (the grants cover box/*) and outside/ (they do not),
// laid out by the script. Every call here is one the library did not wrap
// before #3751, or one it resolved against the wrong directory.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <ftw.h>
#include <glob.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mount.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[4096];

static const char* at(const char* rel) {
    static char buf[4][8192];
    static int next = 0;
    char* b = buf[next++ % 4];
    snprintf(b, sizeof buf[0], "%s/%s", root, rel);
    return b;
}

static int verdict(int ok) {
    if (ok) return 0;
    if (errno == EACCES) return 3;
    if (errno == EEXIST) return 6;
    if (errno == ENOTDIR) return 8;
    if (errno == EISDIR) return 9;
    return 4;
}

// What a descriptor holds, compared with the outside file's sentinel.
static int read_verdict(int fd) {
    if (fd < 0) return verdict(0);
    char got[64] = {0};
    ssize_t n = read(fd, got, sizeof got - 1);
    close(fd);
    if (n > 0 && strncmp(got, "outside", 7) == 0) return 5;
    return 0;
}

#if defined(__GLIBC__)
extern int __open_2(const char*, int);
#endif

// The race: a thread swaps box/d (a directory) with box/d_link (a symlink
// to outside/) by renames while the main thread opens box/d/secret.txt.
static volatile int stop_swap;
static char sd[8192], sr[8192], sl[8192];   // set before the thread starts
static void* swapper(void* arg) {
    (void)arg;
    while (!stop_swap) {
        rename(sd, sr);
        rename(sl, sd);
        rename(sd, sl);
        rename(sr, sd);
    }
    return NULL;
}

static int seen_outside;
static int tree_entry(const char* path, const struct stat* st, int type, struct FTW* f) {
    (void)st; (void)type; (void)f;
    if (strstr(path, "secret.txt") && strstr(path, "out_dir")) seen_outside = 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <case> <root>\n", argv[0]);
        return 2;
    }
    const char* c = argv[1];
    snprintf(root, sizeof root, "%s", argv[2]);

    // openat relative to a descriptor of the box: "../outside" from it is
    // outside, whatever the working directory (the script runs this from
    // box/sub, where "../outside" would be box/outside).
    if (strcmp(c, "openat-dirfd-out") == 0) {
        DIR* d = opendir(at("box"));
        if (!d) return verdict(0);
        int fd = openat(dirfd(d), "../outside/secret.txt", O_RDONLY);
        int rc = read_verdict(fd);
        closedir(d);
        return rc;
    }
    if (strcmp(c, "openat-dirfd-in") == 0) {
        DIR* d = opendir(at("box"));
        if (!d) return verdict(0);
        int fd = openat(dirfd(d), "inside.txt", O_RDONLY);
        int rc = read_verdict(fd);
        closedir(d);
        return rc;
    }
    if (strcmp(c, "open2-out") == 0) {
#if defined(__GLIBC__)
        return read_verdict(__open_2(at("outside/secret.txt"), O_RDONLY));
#else
        return 7;
#endif
    }
    if (strcmp(c, "creat-out") == 0) {
        int fd = creat(at("outside/created"), 0600);
        if (fd >= 0) close(fd);
        return verdict(fd >= 0);
    }
    if (strcmp(c, "unlink-out") == 0) return verdict(unlink(at("outside/secret.txt")) == 0);
    if (strcmp(c, "unlink-in") == 0) return verdict(unlink(at("box/del.txt")) == 0);
    if (strcmp(c, "unlink-link-in") == 0) return verdict(unlink(at("box/out_link")) == 0);
    if (strcmp(c, "unlink-link-out") == 0) return verdict(unlink(at("outside/in_link")) == 0);
    if (strcmp(c, "unlinkat-out") == 0) {
        int dfd = open(at("box"), O_RDONLY | O_DIRECTORY);
        if (dfd < 0) return verdict(0);
        int rc = verdict(unlinkat(dfd, "../outside/secret.txt", 0) == 0);
        close(dfd);
        return rc;
    }
    if (strcmp(c, "remove-out") == 0) return verdict(remove(at("outside/secret.txt")) == 0);
    if (strcmp(c, "rename-out") == 0)
        return verdict(rename(at("outside/secret.txt"), at("box/stolen")) == 0);
    if (strcmp(c, "rename-in") == 0)
        return verdict(rename(at("box/ren.txt"), at("box/ren2.txt")) == 0);
    if (strcmp(c, "mkdir-out") == 0) return verdict(mkdir(at("outside/newdir"), 0700) == 0);
    if (strcmp(c, "mkdir-in") == 0) return verdict(mkdir(at("box/newdir"), 0700) == 0);
    if (strcmp(c, "mkdir-exists") == 0) return verdict(mkdir(root, 0700) == 0);
    if (strcmp(c, "rmdir-out") == 0) return verdict(rmdir(at("outside/empty")) == 0);
    if (strcmp(c, "symlink-out") == 0) return verdict(symlink("x", at("outside/new_link")) == 0);
    if (strcmp(c, "link-widen") == 0)
        return verdict(link(at("outside/secret.txt"), at("box/hard")) == 0);
    if (strcmp(c, "link-in") == 0)
        return verdict(link(at("box/inside.txt"), at("box/hard_in")) == 0);
    if (strcmp(c, "chmod-out") == 0) return verdict(chmod(at("outside/secret.txt"), 0600) == 0);
    if (strcmp(c, "chmod-in") == 0) return verdict(chmod(at("box/inside.txt"), 0644) == 0);
    if (strcmp(c, "chown-out") == 0)
        return verdict(chown(at("outside/secret.txt"), getuid(), getgid()) == 0);
    if (strcmp(c, "truncate-out") == 0) return verdict(truncate(at("outside/secret.txt"), 0) == 0);
    if (strcmp(c, "utimes-out") == 0) return verdict(utimes(at("outside/secret.txt"), NULL) == 0);
    if (strcmp(c, "mkfifo-out") == 0) return verdict(mkfifo(at("outside/fifo"), 0600) == 0);
    if (strcmp(c, "opendir-out") == 0) {
        DIR* d = opendir(at("outside"));
        if (d) closedir(d);
        return verdict(d != NULL);
    }
    if (strcmp(c, "scandir-out") == 0) {
        struct dirent** list = NULL;
        int n = scandir(at("outside"), &list, NULL, alphasort);
        for (int i = 0; i < n; i++) free(list[i]);
        free(list);
        return verdict(n >= 0);
    }
    if (strcmp(c, "scandir-in") == 0) {
        struct dirent** list = NULL;
        int n = scandir(at("box"), &list, NULL, alphasort);
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i]->d_name, "inside.txt") == 0) found = 1;
            free(list[i]);
        }
        free(list);
        if (n < 0) return verdict(0);
        return found ? 0 : 4;
    }
    if (strcmp(c, "mkstemp-out") == 0) {
        char t[8192];
        snprintf(t, sizeof t, "%s", at("outside/tXXXXXX"));
        int fd = mkstemp(t);
        if (fd >= 0) close(fd);
        return verdict(fd >= 0);
    }
    if (strcmp(c, "mkstemp-in") == 0) {
        char t[8192];
        snprintf(t, sizeof t, "%s", at("box/tXXXXXX"));
        int fd = mkstemp(t);
        if (fd >= 0) close(fd);
        return verdict(fd >= 0);
    }
    if (strcmp(c, "mkdtemp-out") == 0) {
        char t[8192];
        snprintf(t, sizeof t, "%s", at("outside/dXXXXXX"));
        return verdict(mkdtemp(t) != NULL);
    }
    // glob over the box, which holds a symlink to the outside file: the
    // match through it is taken out.
    if (strcmp(c, "glob-filter") == 0) {
        glob_t g;
        memset(&g, 0, sizeof g);
        char pat[8192];
        snprintf(pat, sizeof pat, "%s", at("box/*"));
        int r = glob(pat, 0, NULL, &g);
        int leak = 0, inside = 0;
        for (size_t i = 0; r == 0 && i < g.gl_pathc; i++) {
            if (strstr(g.gl_pathv[i], "out_link")) leak = 1;
            if (strstr(g.gl_pathv[i], "inside.txt")) inside = 1;
        }
        if (r == 0) globfree(&g);
        if (r != 0) return 4;
        if (leak) return 5;
        return inside ? 0 : 4;
    }
    // nftw over the box, which holds a symlink to outside/: nothing under
    // it is passed to the caller.
    if (strcmp(c, "nftw-filter") == 0) {
        seen_outside = 0;
        int r = nftw(at("box"), tree_entry, 8, 0);
        if (r != 0) return verdict(0);
        return seen_outside ? 5 : 0;
    }
    // A child's stdin opened by posix_spawn from a path outside the box.
    if (strcmp(c, "spawn-addopen-out") == 0) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        int r = posix_spawn_file_actions_addopen(&fa, 0, at("outside/secret.txt"), O_RDONLY, 0);
        posix_spawn_file_actions_destroy(&fa);
        errno = r;
        return verdict(r == 0);
    }
    if (strcmp(c, "race") == 0) {
        snprintf(sd, sizeof sd, "%s", at("box/d"));
        snprintf(sr, sizeof sr, "%s", at("box/d_real"));
        snprintf(sl, sizeof sl, "%s", at("box/d_link"));
        char target[8192];
        snprintf(target, sizeof target, "%s", at("box/d/secret.txt"));
        pthread_t t;
        stop_swap = 0;
        if (pthread_create(&t, NULL, swapper, NULL) != 0) return 4;
        int escaped = 0;
        for (int i = 0; i < 20000 && !escaped; i++) {
            int fd = open(target, O_RDONLY);
            if (read_verdict(fd) == 5) escaped = 1;
        }
        stop_swap = 1;
        pthread_join(t, NULL);
        return escaped ? 5 : 0;
    }
    // "name/": a directory to the kernel. Unlinking a file or a symlink
    // that way is ENOTDIR, an open that would create is EISDIR, a rename
    // of a file to "x/" is ENOTDIR; rmdir of "dir/" removes it.
    if (strcmp(c, "unlink-slash-file") == 0) return verdict(unlink(at("box/inside.txt/")) == 0);
    if (strcmp(c, "unlink-slash-link") == 0) return verdict(unlink(at("box/out_dir/")) == 0);
    if (strcmp(c, "open-creat-slash") == 0) {
        int fd = open(at("box/slash_new/"), O_WRONLY | O_CREAT, 0600);
        if (fd >= 0) close(fd);
        return verdict(fd >= 0);
    }
    if (strcmp(c, "rmdir-slash") == 0) return verdict(rmdir(at("box/rmd/")) == 0);
    if (strcmp(c, "rename-slash-file") == 0)
        return verdict(rename(at("box/ren_s.txt"), at("box/ren_s_dir/")) == 0);
    // An O_TMPFILE file given its name, both ways: linkat with
    // AT_EMPTY_PATH on its descriptor, and /proc/self/fd/N followed.
    if (strcmp(c, "link-tmpfile-empty") == 0 || strcmp(c, "link-tmpfile-proc") == 0) {
#if defined(O_TMPFILE) && defined(AT_EMPTY_PATH)
        int fd = open(at("box"), O_TMPFILE | O_RDWR, 0600);
        if (fd < 0) return (errno == EOPNOTSUPP || errno == EISDIR || errno == EINVAL) ? 7 : verdict(0);
        if (write(fd, "tmp\n", 4) != 4) { close(fd); return 4; }
        int r;
        if (strcmp(c, "link-tmpfile-empty") == 0) {
            r = linkat(fd, "", AT_FDCWD, at("box/tmp_empty"), AT_EMPTY_PATH);
        } else {
            char self[64];
            snprintf(self, sizeof self, "/proc/self/fd/%d", fd);
            r = linkat(AT_FDCWD, self, AT_FDCWD, at("box/tmp_proc"), AT_SYMLINK_FOLLOW);
        }
        int rc = verdict(r == 0);
        close(fd);
        return rc;
#else
        return 7;
#endif
    }
    // A descriptor of a file outside the box, inherited on fd 9 (the script
    // opens it), given a name in the box through /proc/self/fd/9: refused.
    if (strcmp(c, "link-fd-out") == 0)
        return verdict(linkat(AT_FDCWD, "/proc/self/fd/9", AT_FDCWD, at("box/stolen_fd"),
                              AT_SYMLINK_FOLLOW) == 0);
    // The way round every path check: a mount namespace (with a user
    // namespace, which needs no privilege) and a bind of a directory outside
    // the box onto one inside it. unshare is refused (EPERM) without a
    // native grant, and the bind is refused (EACCES) for its source.
    if (strcmp(c, "unshare-ns") == 0) {
#if defined(__linux__)
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0) return 0;
        return errno == EPERM ? 10 : verdict(0);
#else
        return 7;
#endif
    }
    if (strcmp(c, "bind-mount-out") == 0) {
#if defined(__linux__)
        return verdict(mount(at("outside"), at("box/sub"), NULL, MS_BIND, NULL) == 0);
#else
        return 7;
#endif
    }
    // renameat2 (Linux; FreeBSD from 15.0) out of outside/, found by name so
    // that a C library without it skips.
    if (strcmp(c, "renameat2-out") == 0) {
        int (*fn)(int, const char*, int, const char*, unsigned) =
            (int (*)(int, const char*, int, const char*, unsigned))dlsym(RTLD_DEFAULT, "renameat2");
        if (!fn) return 7;
        return verdict(fn(AT_FDCWD, at("outside/secret.txt"), AT_FDCWD, at("box/stolen2"), 0) == 0);
    }
    // FreeBSD's blocks forms of glob and fts_open, called with no block (the
    // error function and the comparison may be NULL): checked as glob and
    // fts_open are.
    if (strcmp(c, "glob-b-filter") == 0) {
        int (*fn)(const char*, int, void*, glob_t*) =
            (int (*)(const char*, int, void*, glob_t*))dlsym(RTLD_DEFAULT, "glob_b");
        if (!fn) return 7;
        glob_t g;
        memset(&g, 0, sizeof g);
        int r = fn(at("box/*"), 0, NULL, &g);
        int leak = 0, inside = 0;
        for (size_t i = 0; r == 0 && i < g.gl_pathc; i++) {
            if (strstr(g.gl_pathv[i], "out_link")) leak = 1;
            if (strstr(g.gl_pathv[i], "inside.txt")) inside = 1;
        }
        if (r == 0) globfree(&g);
        if (r != 0) return 4;
        if (leak) return 5;
        return inside ? 0 : 4;
    }
    if (strcmp(c, "fts-b-out") == 0) {
        FTS* (*fn)(char* const*, int, void*) =
            (FTS * (*)(char* const*, int, void*))dlsym(RTLD_DEFAULT, "fts_open_b");
        if (!fn) return 7;
        char* roots[] = { (char*)at("outside"), NULL };
        FTS* f = fn(roots, FTS_PHYSICAL, NULL);
        if (f) {
            fts_close(f);
            return 5;
        }
        return verdict(0);
    }
    // A plain open of the outside file: the refusal the log case (in the
    // script) writes down.
    if (strcmp(c, "open-out") == 0) return read_verdict(open(at("outside/secret.txt"), O_RDONLY));
    fprintf(stderr, "unknown case %s\n", c);
    return 2;
}
