/* macOS only: counts fcntl(F_FULLFSYNC) calls the program makes, and
 * prints the count at exit. Loaded with DYLD_INSERT_LIBRARIES by
 * test_fs_full_fsync.sh, so the test sees that a sync asked the drive to
 * flush its cache rather than taking a successful fsync(2) on trust. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int full_fsyncs = 0;

static int counting_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (cmd == F_FULLFSYNC) full_fsyncs++;
    return fcntl(fd, cmd, arg);
}

static void report(void) {
    fprintf(stderr, "F_FULLFSYNC calls: %d\n", full_fsyncs);
}

__attribute__((constructor)) static void install(void) { atexit(report); }

__attribute__((used)) static struct { const void* repl; const void* orig; }
    interposers[] __attribute__((section("__DATA,__interpose"))) = {
        { (const void*)counting_fcntl, (const void*)fcntl },
    };
