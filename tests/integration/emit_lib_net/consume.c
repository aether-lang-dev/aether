/*
 * consume.c — C host for the --emit=lib --with=net PIC round-trip test.
 *
 * loads the shared library built from a std.http-importing module
 * (so the link pulled in the TLS-using HTTP runtime object) and calls
 * its aether_* exports. Proves the .so both links and loads — the thing
 * that was impossible before the runtime archive was built -fPIC.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

typedef int32_t (*aether_ping_fn)(void);
typedef const char* (*aether_echo_fn)(const char*);

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-libnetmod.so>\n", argv[0]);
        return 2;
    }

    void* h = lib_open(argv[1]);
    if (!h) {
        fprintf(stderr, "FAIL: loading %s: %s\n", argv[1], lib_error());
        return 1;
    }

    aether_ping_fn ping = (aether_ping_fn)lib_sym(h, "aether_ping");
    aether_echo_fn echo = (aether_echo_fn)lib_sym(h, "aether_echo");
    if (!ping) { fprintf(stderr, "FAIL: aether_ping not found: %s\n", lib_error()); return 1; }
    if (!echo) { fprintf(stderr, "FAIL: aether_echo not found: %s\n", lib_error()); return 1; }

    int32_t p = ping();
    if (p != 7) {
        fprintf(stderr, "FAIL: aether_ping() = %d, expected 7\n", p);
        return 1;
    }

    const char* e = echo("hi");
    if (!e || strcmp(e, "echo:hi") != 0) {
        fprintf(stderr, "FAIL: aether_echo(\"hi\") = %s, expected \"echo:hi\"\n", e ? e : "(null)");
        return 1;
    }

    lib_close(h);
    printf("OK: --emit=lib --with=net library links, loads, and round-trips\n");
    return 0;
}
