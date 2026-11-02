// aether_sandbox_preload_path.h: where libaether_sandbox.so is (#3177).
//
// A sandboxed child starts with this library in LD_PRELOAD, so the parent
// has to find it from wherever the parent program runs. spawn_sandboxed
// (Linux and FreeBSD) and the contrib process hosts (contrib/host/aether,
// contrib/host/go) all look it up here, in one order.
//
// It used to be looked for only beside the running program and in
// ../build/ from it. A program `ae run` builds lives in the build cache,
// and one an installed toolchain builds lives anywhere, so neither found
// it, and spawn_sandboxed refused to start the child.
//
// The places tried, in order:
//   1. AETHER_SANDBOX_PRELOAD, the library's own path, when it is set;
//   2. beside the running program, where a deployment can ship it;
//   3. under AETHER_HOME, which `ae` sets for every program it runs (the
//      toolchain's root, as it finds its other files): lib/aether/ (an
//      install.sh or `make install` prefix), lib/ (a release archive's
//      layout), then build/ (a source tree);
//   4. the same three from the program's directory's parent (a program
//      installed in <prefix>/bin, or one in a source tree's directory).
#ifndef AETHER_SANDBOX_PRELOAD_PATH_H
#define AETHER_SANDBOX_PRELOAD_PATH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define AETHER_SANDBOX_PRELOAD_NAME "libaether_sandbox.so"

/* The running program's directory, with no trailing slash, or "" when the
 * platform will not say. */
static inline void aether_sandbox_exe_dir(char* dir, size_t size) {
    dir[0] = '\0';
    char exe[1024];
    size_t len = 0;
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    len = (size_t)n;
#elif defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t got = sizeof(exe);
    if (sysctl(mib, 4, exe, &got, NULL, 0) != 0 || got == 0) return;
    len = strnlen(exe, got < sizeof(exe) ? got : sizeof(exe) - 1);
#elif defined(__APPLE__)
    uint32_t got = (uint32_t)sizeof(exe);
    if (_NSGetExecutablePath(exe, &got) != 0) return;
    len = strlen(exe);
#else
    (void)exe;
    return;
#endif
    exe[len] = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(dir, size, "%s", exe);
}

/* Try one candidate: copy it to `buf` and say whether it exists; note it in
 * `tried` either way. A candidate longer than `buf` is not taken: cut short,
 * it would name some other file. Every snprintf here checks its result, for
 * that reason (and gcc's -Wformat-truncation, which -Werror builds hit). */
static inline int aether_sandbox_try_preload(const char* path, char* buf, size_t size,
                                             char* tried, size_t tried_size) {
    if (tried && tried_size > 0) {
        size_t used = strlen(tried);
        if (used + 1 < tried_size)
            snprintf(tried + used, tried_size - used, "%s%s", used ? ", " : "", path);
    }
    if (access(path, F_OK) != 0) return 0;
    int n = snprintf(buf, size, "%s", path);
    if (n < 0 || (size_t)n >= size) {
        if (size) buf[0] = '\0';
        return 0;
    }
    return 1;
}

/* The library under a toolchain root: lib/aether/, lib/, build/. */
static inline int aether_sandbox_try_root(const char* root, char* buf, size_t size, char* tried,
                                          size_t tried_size) {
    static const char* const subdirs[] = { "lib/aether", "lib", "build" };
    char candidate[1200];
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        int n = snprintf(candidate, sizeof(candidate), "%s/%s/" AETHER_SANDBOX_PRELOAD_NAME,
                         root, subdirs[i]);
        if (n < 0 || (size_t)n >= sizeof(candidate)) continue;
        if (aether_sandbox_try_preload(candidate, buf, size, tried, tried_size)) return 1;
    }
    return 0;
}

/* Find the library: 1 with its path in `buf`, 0 when it is in none of the
 * places above. `tried` (may be NULL) receives the paths looked at, for the
 * message when none had it. */
static inline int aether_sandbox_find_preload(char* buf, size_t size, char* tried,
                                              size_t tried_size) {
    char candidate[1200];
    if (tried && tried_size > 0) tried[0] = '\0';

    const char* explicit_path = getenv("AETHER_SANDBOX_PRELOAD");
    if (explicit_path && explicit_path[0])
        return aether_sandbox_try_preload(explicit_path, buf, size, tried, tried_size);

    char exe_dir[1024];
    aether_sandbox_exe_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0]) {
        int n = snprintf(candidate, sizeof(candidate), "%s/" AETHER_SANDBOX_PRELOAD_NAME, exe_dir);
        if (n >= 0 && (size_t)n < sizeof(candidate) &&
            aether_sandbox_try_preload(candidate, buf, size, tried, tried_size)) return 1;
    }

    const char* home = getenv("AETHER_HOME");
    if (home && home[0]) {
        if (aether_sandbox_try_root(home, buf, size, tried, tried_size)) return 1;
    }
    if (exe_dir[0]) {
        int n = snprintf(candidate, sizeof(candidate), "%s/..", exe_dir);
        if (n >= 0 && (size_t)n < sizeof(candidate) &&
            aether_sandbox_try_root(candidate, buf, size, tried, tried_size)) return 1;
    }
    return 0;
}

/* The message for a library none of the places had, to stderr, prefixed
 * with who asks ("[aether]", "[go-host]"). */
static inline void aether_sandbox_preload_missing(const char* who, const char* tried) {
    fprintf(stderr,
            "%s cannot find " AETHER_SANDBOX_PRELOAD_NAME " (looked at: %s).\n"
            "  `make` builds it into build/ and install.sh puts it in <prefix>/lib/aether/;\n"
            "  AETHER_SANDBOX_PRELOAD names it anywhere else.\n",
            who, tried && tried[0] ? tried : "nowhere: no program path and no AETHER_HOME");
}

#endif
