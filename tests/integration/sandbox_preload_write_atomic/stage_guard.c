// The preload's side of a staged write, held to its shape.
//
//   stage_guard <staged|unstaged> <path>
//
// Stands in for a runtime whose stage is hostile: in `staged` mode
// aether_sandbox_staged_for answers <path> for ANY name, as if code had
// staged whatever it liked. The preload must still check every open that is
// not a new file (O_CREAT|O_EXCL) named <path>.tmp.<this pid>.<n> as it is,
// so the stage can only ever create that one shape of sibling.
// No in-process level (check_resolved -1): the grant file decides.
//
// Prints "<case> ok" or "<case> EACCES" (or "<case> E<n>").

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char* staged_target = NULL;

const char* aether_sandbox_staged_for(const char* tmp) {
    (void)tmp;
    return staged_target;
}

int aether_sandbox_check_resolved(const char* category, const char* resource) {
    (void)category;
    (void)resource;
    return -1;
}

int aether_sandbox_levels_here(void) {
    return 0;
}

static void try_open(const char* label, const char* name, int flags, int at) {
    int fd = at ? openat(AT_FDCWD, name, flags, 0600) : open(name, flags, 0600);
    if (fd >= 0) {
        printf("%s ok\n", label);
        close(fd);
        unlink(name);
    } else if (errno == EACCES) {
        printf("%s EACCES\n", label);
    } else {
        printf("%s E%d\n", label, errno);
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <staged|unstaged> <path>\n", argv[0]);
        return 2;
    }
    const char* path = argv[2];
    if (strcmp(argv[1], "staged") == 0) staged_target = path;
    long pid = (long)getpid();
    char name[4096];
    const int excl = O_WRONLY | O_CREAT | O_EXCL;

    snprintf(name, sizeof name, "%s.tmp.%ld.1", path, pid);
    try_open("shape_excl", name, excl, 0);
    snprintf(name, sizeof name, "%s.tmp.%ld.2", path, pid);
    try_open("shape_excl_openat", name, excl, 1);
    snprintf(name, sizeof name, "%s.tmp.%ld.3", path, pid);
    try_open("shape_no_excl", name, O_WRONLY | O_CREAT, 0);
    snprintf(name, sizeof name, "%s.tmp.%ld.4", path, pid + 1);
    try_open("other_pid", name, excl, 0);
    snprintf(name, sizeof name, "%s.tmp.%ld.5x", path, pid);
    try_open("bad_counter", name, excl, 0);
    snprintf(name, sizeof name, "%s.bak", path);
    try_open("other_suffix", name, excl, 0);
    snprintf(name, sizeof name, "%s/../elsewhere.txt", path);
    try_open("elsewhere", name, excl, 0);
    return 0;
}
