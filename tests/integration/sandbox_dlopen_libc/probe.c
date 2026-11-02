/* Run under the preload with no native grant. Tries to dlopen libc and the
 * dynamic loader by every spelling a guest can reach (the bare name, the
 * path the loader itself used, that path through the other of /lib and
 * /usr/lib, "..", a symlink with an innocent name, that name found on
 * LD_LIBRARY_PATH) and prints one line per
 * try: "<label> refused" or "<label> loaded". A library that is not libc
 * (libm) must still load. Labels for spellings this system lacks are not
 * printed; the script checks the ones that must exist. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif

static void try(const char* label, const char* name) {
    void* h = dlopen(name, RTLD_NOW);
    printf("%s %s\n", label, h ? "loaded" : "refused");
    fflush(stdout);
    if (h) dlclose(h);
}

/* Where `p` would be with /usr added or taken away: /lib/x -> /usr/lib/x and
 * /usr/lib/x -> /lib/x. 0 when it is neither. */
static int other_lib(const char* p, char* out, size_t cap) {
    if (strncmp(p, "/usr/lib", 8) == 0) return snprintf(out, cap, "%s", p + 4) < (int)cap;
    if (strncmp(p, "/lib", 4) == 0) return snprintf(out, cap, "/usr%s", p) < (int)cap;
    return 0;
}

int main(int argc, char** argv) {
    const char* tmp = argc > 1 ? argv[1] : "/tmp";
    Dl_info info;
    char libc_path[PATH_MAX] = "";
    if (dladdr((void*)strlen, &info) && info.dli_fname) {
        snprintf(libc_path, sizeof libc_path, "%s", info.dli_fname);
    }
    try("bare", "libc.so.6");
    if (libc_path[0] == '/') {
        char alt[PATH_MAX + 8], dots[PATH_MAX + 16], link[PATH_MAX];
        try("path", libc_path);
        if (other_lib(libc_path, alt, sizeof alt) && access(alt, F_OK) == 0) try("other-lib", alt);
        const char* slash = strrchr(libc_path, '/');
        /* "<dir>/../<last>/libc.so.6": up out of libc's directory and back. */
        const char* last = slash;
        while (last > libc_path && last[-1] != '/') last--;
        if (last > libc_path) {
            snprintf(dots, sizeof dots, "%.*s/..%s", (int)(slash - libc_path), libc_path, last - 1);
            if (access(dots, F_OK) == 0) try("dotdot", dots);
        }
        snprintf(link, sizeof link, "%s/libplain.so", tmp);
        unlink(link);
        if (symlink(libc_path, link) == 0) {
            try("symlink", link);
            /* The same link by bare name, found on LD_LIBRARY_PATH (the
             * script points it at tmp): no path to resolve, so only the
             * handle the loader returns says it is libc. */
            try("search", "libplain.so");
        }
    }
#if defined(__linux__)
    unsigned long base = getauxval(AT_BASE);
    if (base && dladdr((void*)base, &info) && info.dli_fname && info.dli_fname[0] == '/') {
        try("loader", info.dli_fname);
    }
#endif
    try("libm", "libm.so.6");
    return 0;
}
