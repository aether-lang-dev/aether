/* A C host for a program built with --emit=lib: run its main() through
 * aether_main, let it keep going, stop it with aether_main_exit. */
#include <stdio.h>
#include <string.h>

/* The library is loaded by path on every platform: dlopen on POSIX,
 * LoadLibrary on Windows (#2541). */
#ifdef _WIN32
#include <windows.h>
static void* lib_open(const char* path) { return (void*)LoadLibraryA(path); }
static void* lib_sym(void* h, const char* name) {
    return (void*)GetProcAddress((HMODULE)h, name);
}
static void lib_close(void* h) { FreeLibrary((HMODULE)h); }
static const char* lib_error(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "error %lu", (unsigned long)GetLastError());
    return buf;
}
#else
#include <dlfcn.h>
static void* lib_open(const char* path) { return dlopen(path, RTLD_NOW); }
static void* lib_sym(void* h, const char* name) { return dlsym(h, name); }
static void lib_close(void* h) { dlclose(h); }
static const char* lib_error(void) { return dlerror(); }
#endif

typedef int  (*main_fn)(int, char**);
typedef void (*exit_fn)(void);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: host <lib> [once]\n"); return 2; }
    int once = argc > 2 && strcmp(argv[2], "once") == 0;
    void* h = lib_open(argv[1]);
    if (!h) { fprintf(stderr, "loading the library: %s\n", lib_error()); return 2; }
    main_fn am = (main_fn)lib_sym(h, "aether_main");
    exit_fn ax = (exit_fn)lib_sym(h, "aether_main_exit");
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
