// Opens a file the way one case of test_sandbox_open_modes.sh asks, under
// the LD_PRELOAD library, and reports the outcome in the exit status:
// 0 opened, 3 refused with EACCES (the sandbox's refusal), 4 any other
// failure, 2 a usage error.
//
//   harness <case> <file>
//
// Each case names what the open asks of the file (#3742): an fopen mode,
// a freopen onto a path or onto the stream's own file, or open() flags.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int verdict(int ok) {
    if (ok) return 0;
    return errno == EACCES ? 3 : 4;
}

static int with_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    int rc = verdict(f != NULL);
    if (f) fclose(f);
    return rc;
}

static int with_open(const char* path, int flags) {
    int fd = open(path, flags);
    int rc = verdict(fd >= 0);
    if (fd >= 0) close(fd);
    return rc;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <case> <file>\n", argv[0]);
        return 2;
    }
    const char* c = argv[1];
    const char* path = argv[2];
    if (strcmp(c, "fopen-r") == 0) return with_fopen(path, "r");
    if (strcmp(c, "fopen-r+") == 0) return with_fopen(path, "r+");
    if (strcmp(c, "fopen-rb+") == 0) return with_fopen(path, "rb+");
    if (strcmp(c, "fopen-a") == 0) return with_fopen(path, "a");
    if (strcmp(c, "fopen-w+") == 0) return with_fopen(path, "w+");
    if (strcmp(c, "open-rdonly") == 0) return with_open(path, O_RDONLY);
    if (strcmp(c, "open-wronly") == 0) return with_open(path, O_WRONLY);
    if (strcmp(c, "open-rdwr") == 0) return with_open(path, O_RDWR);
    if (strcmp(c, "freopen-r+") == 0) {
        // Onto stdin, which opens nothing, so the refusal is about `path`.
        FILE* f = freopen(path, "r+", stdin);
        int rc = verdict(f != NULL);
        if (f) fclose(f);
        return rc;
    }
    if (strcmp(c, "freopen-null-r+") == 0) {
        // The stream's own file, reopened for writing: glibc does it through
        // /proc/self/fd/N, which the read-only stream's grant must not cover.
        FILE* s = fopen(path, "r");
        if (!s) return 5;
        FILE* f = freopen(NULL, "r+", s);
        int rc = verdict(f != NULL);
        if (f) fclose(f);
        return rc;
    }
    if (strcmp(c, "freopen-null-stdin-rb") == 0) {
        // stdin reopened for no more than it holds (binary mode, the
        // portable idiom): a pipe or a device no grant names, and it must
        // still be open and readable afterwards.
        (void)path;
        FILE* f = freopen(NULL, "rb", stdin);
        if (!f) return verdict(0);
        return fileno(f) >= 0 ? 0 : 6;
    }
    fprintf(stderr, "unknown case %s\n", c);
    return 2;
}
