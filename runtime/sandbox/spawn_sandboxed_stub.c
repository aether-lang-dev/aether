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

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;

// sandbox.spawn_socket fails closed here: there is nothing to contain the
// child with, so it is not started. A host that wants it anyway says so by
// name, with sandbox.spawn_socket_uncontained.
_tuple_int_int_string aether_sandbox_spawn_socket_raw(void* grant_list, const char* prog,
                                                      void* argv_list, void* env_list) {
    (void)grant_list; (void)prog; (void)argv_list; (void)env_list;
    _tuple_int_int_string out = { -1, -1,
#if defined(__APPLE__)
        "sandbox.spawn_socket: cannot contain a child on macOS, so nothing was started "
#elif defined(_WIN32)
        "sandbox.spawn_socket: cannot contain a child on Windows, so nothing was started "
#elif defined(__ANDROID__)
        "sandbox.spawn_socket: cannot contain a child on Android, so nothing was started "
#else
        "sandbox.spawn_socket: cannot contain a child on this platform, so nothing was started "
#endif
        "(sandbox.spawn_socket_uncontained runs one with no sandbox)" };
    return out;
}

int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg) {
    (void)grant_list; (void)program; (void)arg;
    fprintf(stderr, "[aether] spawn_sandboxed is only available on Linux (not Android) and FreeBSD\n");
    return -1;
}

#endif // (!__linux__ || __ANDROID__) && !__FreeBSD__
