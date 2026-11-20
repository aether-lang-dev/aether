/* A program that calls libc before any constructor runs, as a sanitizer's
 * runtime does (TSan makes the directory of its log_path with mkdir from
 * .preinit_array), and that carries the Aether runtime's hook the preload
 * asks about in-process policy. The preload must not call that hook before
 * its own constructor finished: in a TSan build the hook is instrumented
 * code, and it faulted inside TSan's start (http_ipv6 on #3768's TSan leg).
 *
 *   probe DIR    makes DIR from .preinit_array, then checks it from main.
 *
 * Exit 0 and "ok" when the early mkdir went through without the hook being
 * asked; 3 when the hook was asked before main. Linked -rdynamic, so the
 * preload's weak reference to the hook binds to the one here. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile int main_started = 0;
static volatile int early_rc = -2;

__attribute__((visibility("default"), used)) int aether_sandbox_levels_here(void) {
    if (!main_started) {
        static const char m[] = "probe: the preload asked the program before its constructors ran\n";
        (void)!write(2, m, sizeof m - 1);
        _exit(3);
    }
    return 0;
}

static void early(int argc, char** argv, char** envp) {
    (void)envp;
    /* argv, not getenv: the preload wraps getenv too, and nothing here
     * should lean on more of it than the one call under test. */
    if (argc > 1) early_rc = mkdir(argv[1], 0700);
}

__attribute__((section(".preinit_array"), used)) static void (*early_entry)(int, char**, char**) = early;

int main(int argc, char** argv) {
    main_started = 1;
    struct stat st;
    if (argc < 2) {
        fprintf(stderr, "usage: probe DIR\n");
        return 2;
    }
    if (early_rc != 0 || stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "probe: the early mkdir of %s did not make it (rc %d)\n", argv[1], early_rc);
        return 1;
    }
    printf("ok\n");
    return 0;
}
