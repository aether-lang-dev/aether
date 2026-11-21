// Lists one directory every way a program commonly does, and prints what each
// call returned and the errno it left, one line per call, so that a run under
// the LD_PRELOAD library with grants that refuse the directory can be
// compared byte for byte with a run against a directory the kernel refuses
// (chmod 000): a refusal must read as the kernel's own EACCES, with nothing
// else changed.
//
//   probe <dir>
//
// Each line is "<call>: <result> errno=<name>". errno is set to a sentinel
// (EDOM) before each call, so a call that fails without setting it, or one
// that succeeds and leaves a stale value, shows. A failing call's errno is
// read straight after it, before anything else runs.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* ename(int e) {
    switch (e) {
    case 0: return "0";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case ENOTDIR: return "ENOTDIR";
    case EBADF: return "EBADF";
    case EDOM: return "unchanged";
    default: return "other";
    }
}

static int dnr;
static int walk_dnr(const char* path, const struct stat* st, int type, struct FTW* f) {
    (void)path; (void)st; (void)f;
    if (type == FTW_DNR) dnr++;
    return 0;
}

static int glob_err_path;
static int glob_err(const char* path, int e) {
    (void)path;
    glob_err_path = e;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <dir>\n", argv[0]);
        return 2;
    }
    const char* dir = argv[1];
    char entry[4096];
    snprintf(entry, sizeof entry, "%s/entry.txt", dir);
    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%s/*", dir);
    int e;

    errno = EDOM;
    DIR* d = opendir(dir);
    e = errno;
    printf("opendir: %s errno=%s\n", d ? "DIR" : "NULL", ename(d ? 0 : e));
    if (d) {
        errno = 0;
        int n = 0;
        while (readdir(d)) n++;
        printf("readdir: %d entries errno=%s\n", n, ename(errno));
        closedir(d);
    }

    errno = EDOM;
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    e = errno;
    printf("open(O_DIRECTORY): %s errno=%s\n", fd >= 0 ? "fd" : "-1", ename(fd >= 0 ? 0 : e));
    if (fd >= 0) {
        errno = EDOM;
        DIR* fdd = fdopendir(fd);
        e = errno;
        printf("fdopendir: %s errno=%s\n", fdd ? "DIR" : "NULL", ename(fdd ? 0 : e));
        if (fdd) closedir(fdd);
        else close(fd);
    }

    errno = EDOM;
    int dfd = openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    e = errno;
    printf("openat(O_DIRECTORY): %s errno=%s\n", dfd >= 0 ? "fd" : "-1", ename(dfd >= 0 ? 0 : e));
    if (dfd >= 0) close(dfd);

    struct dirent** list = NULL;
    errno = EDOM;
    int n = scandir(dir, &list, NULL, alphasort);
    e = errno;
    printf("scandir: %s errno=%s\n", n >= 0 ? "list" : "-1", ename(n >= 0 ? 0 : e));
    if (n >= 0) {
        for (int i = 0; i < n; i++) free(list[i]);
        free(list);
    }

    dnr = 0;
    errno = EDOM;
    int r = nftw(dir, walk_dnr, 4, FTW_PHYS);
    e = errno;
    printf("nftw: %d unreadable=%d errno=%s\n", r, dnr, ename(r == 0 ? 0 : e));

    glob_t g;
    memset(&g, 0, sizeof g);
    glob_err_path = 0;
    errno = EDOM;
    r = glob(pattern, GLOB_ERR, glob_err, &g);
    printf("glob(GLOB_ERR): %s errfunc=%s\n",
           r == 0 ? "matched" : r == GLOB_NOMATCH ? "GLOB_NOMATCH" : r == GLOB_ABORTED ? "GLOB_ABORTED" : "other",
           ename(glob_err_path));
    if (r == 0) globfree(&g);

    memset(&g, 0, sizeof g);
    glob_err_path = 0;
    r = glob(pattern, 0, glob_err, &g);
    printf("glob: %s errfunc=%s\n",
           r == 0 ? "matched" : r == GLOB_NOMATCH ? "GLOB_NOMATCH" : r == GLOB_ABORTED ? "GLOB_ABORTED" : "other",
           ename(glob_err_path));
    if (r == 0) globfree(&g);

    memset(&g, 0, sizeof g);
    r = glob(pattern, GLOB_NOCHECK, NULL, &g);
    printf("glob(GLOB_NOCHECK): %s %s\n",
           r == 0 ? "matched" : r == GLOB_NOMATCH ? "GLOB_NOMATCH" : r == GLOB_ABORTED ? "GLOB_ABORTED" : "other",
           r == 0 && g.gl_pathc == 1 && strcmp(g.gl_pathv[0], pattern) == 0 ? "the pattern" : "something else");
    if (r == 0) globfree(&g);

    errno = EDOM;
    fd = open(entry, O_RDONLY);
    e = errno;
    printf("open(entry): %s errno=%s\n", fd >= 0 ? "fd" : "-1", ename(fd >= 0 ? 0 : e));
    if (fd >= 0) close(fd);

    errno = EDOM;
    FILE* fp = fopen(entry, "r");
    e = errno;
    printf("fopen(entry): %s errno=%s\n", fp ? "FILE" : "NULL", ename(fp ? 0 : e));
    if (fp) fclose(fp);
    return 0;
}
