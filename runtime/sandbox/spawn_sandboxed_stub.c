// spawn_sandboxed_stub.c — fallback for platforms without an LD_PRELOAD
// sandbox backend (macOS, Windows, Android, ...).
//
// macOS has DYLD_INSERT_LIBRARIES rather than LD_PRELOAD and a hardened
// runtime that ignores it for system binaries; Windows has no equivalent
// at all. Those platforms get this stub, which fails loudly.
//
// Self-guarded: compiles to an empty object on Linux and FreeBSD, where
// spawn_sandboxed_linux.c / spawn_sandboxed_bsd.c provide the real impl.

#if (!defined(__linux__) || defined(__ANDROID__)) && !defined(__FreeBSD__)

#include <stdio.h>

int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg) {
    (void)grant_list; (void)program; (void)arg;
    fprintf(stderr, "[aether] spawn_sandboxed is only available on Linux (not Android) and FreeBSD\n");
    return -1;
}

#endif // (!__linux__ || __ANDROID__) && !__FreeBSD__
