/* A C host for a program built with --emit=lib: run its main() through
 * aether_main, let it keep going, stop it with aether_main_exit. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef int  (*main_fn)(int, char**);
typedef void (*exit_fn)(void);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: host <lib> [once]\n"); return 2; }
    int once = argc > 2 && strcmp(argv[2], "once") == 0;
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    main_fn am = (main_fn)dlsym(h, "aether_main");
    exit_fn ax = (exit_fn)dlsym(h, "aether_main_exit");
    if (!am || !ax) {
        printf("host: no aether_main / aether_main_exit export\n");
        return 3;
    }
    char* args[] = { "app", "one", "two", NULL };

    int rc = am(3, args);
    printf("host: aether_main returned %d\n", rc);
    fflush(stdout);
    /* Running: a second call is rejected and runs nothing. */
    int again = am(3, args);
    printf("host: second aether_main returned %d\n", again);
    ax();
    printf("host: aether_main_exit returned\n");
    ax();   /* idempotent */
    printf("host: second aether_main_exit returned\n");
    if (once) {
        fflush(stdout);
        return (rc == 42 && again == -1) ? 0 : 1;
    }

    /* Stopped: the program can be run again on a fresh scheduler, as an
     * Android activity recreated in the same process would. */
    int rc2 = am(1, args);
    printf("host: rerun returned %d\n", rc2);
    ax();
    printf("host: done\n");
    fflush(stdout);
    return (rc == 42 && again == -1 && rc2 == 42) ? 0 : 1;
}
