// aep_dl.h — tiny portable dlopen shim for the contrib.host.* bridges.
//
// The bridges load the deploy host's libperl/libruby at RUNTIME (not at
// link) via dlopen+dlsym, so a binary is ABI-portable: compile anywhere, run
// where the language is. POSIX spells that dlopen/dlsym/dlclose (<dlfcn.h>);
// Windows spells it LoadLibrary/GetProcAddress/FreeLibrary (no <dlfcn.h>).
// This header maps the POSIX names the bridges already use onto Win32 so the
// bridge source stays single-source across FreeBSD/macOS/Linux/Windows.
#ifndef AEP_DL_H
#define AEP_DL_H

#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>   // snprintf, used by the dlerror() fallback below

// RTLD_* flags have no Win32 meaning; accept and ignore them so call sites
// (dlopen(name, RTLD_NOW | RTLD_GLOBAL)) compile unchanged.
#ifndef RTLD_NOW
#define RTLD_NOW    0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

static inline void* dlopen(const char* name, int flags) {
    (void)flags;
    return (void*)LoadLibraryA(name);
}
static inline void* dlsym(void* handle, const char* sym) {
    return (void*)GetProcAddress((HMODULE)handle, sym);
}
static inline int dlclose(void* handle) {
    // POSIX dlclose returns 0 on success; FreeLibrary returns nonzero on success.
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
// POSIX dlerror(): NULL when no error, else a human string. Map to the last
// Win32 error; returns NULL when the last error is 0 (clean), matching the
// bridge's `dlerror() ? dlerror() : "(none)"` usage. Not thread-safe (nor is
// POSIX dlerror strictly); fine for the bridge's one-shot load-failure path.
static inline char* dlerror(void) {
    DWORD e = GetLastError();
    if (e == 0) return (char*)0;
    static char buf[256];
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        (void*)0, e, 0, buf, (DWORD)sizeof(buf), (va_list*)0);
    if (n == 0) {
        // No message text; report the numeric code so it isn't silently "(none)".
        snprintf(buf, sizeof(buf), "Win32 error %lu", (unsigned long)e);
    }
    SetLastError(0);  // POSIX dlerror clears the error after reporting it.
    return buf;
}

#else  // POSIX

#include <dlfcn.h>

#endif

#endif // AEP_DL_H
