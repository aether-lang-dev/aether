#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int (*aether_sandbox_check_fn)(const char*, const char*);

static int allow_check(const char* category, const char* resource) {
    (void)category;
    (void)resource;
    return 1;
}

static int deny_check(const char* category, const char* resource) {
    (void)category;
    (void)resource;
    return 0;
}

// -1 means "no sandbox level on this thread": the embedded checker has no
// opinion and the preload file grants must decide.
static int none_check(const char* category, const char* resource) {
    (void)category;
    (void)resource;
    return -1;
}

static aether_sandbox_check_fn active_check = allow_check;

int aether_sandbox_check_resolved(const char* category, const char* resource) {
    return active_check(category, resource);
}

int aether_sandbox_levels_here(void) {
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <allow|deny|none> <file>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "deny") == 0) active_check = deny_check;
    else if (strcmp(argv[1], "none") == 0) active_check = none_check;
    else active_check = allow_check;

    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) return errno == EACCES ? 3 : 4;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0) return 5;
    if (write(STDOUT_FILENO, buf, (size_t)n) != n) return 6;
    return 0;
}
