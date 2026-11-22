// std.dl - cross-platform dynamic library loader.
//
// See aether_dl.h for the API contract.
//
// Threading: per-thread last-error buffer, mirroring how dlerror()
// itself is thread-local on glibc/musl. Each entry point captures
// the platform error into the TLS slot before returning.
#include "aether_dl.h"
#if defined(AETHER_NO_THREADING)
#include "../../runtime/actors/aether_panic.h"   /* aether_context_local: a cooperative build's per-task state */
#endif

#include <stdio.h>
#include <string.h>

#include "../../runtime/utils/aether_compiler.h"  // AETHER_TLS
#include "../../runtime/aether_sandbox.h"   // the "native" check on a load

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define AETHER_DL_ERR_CAP 512

/* What every Aether library binds to in the runtime it runs on (#3439): its
 * stubs check the runtime ABI and read the outcome. A library built to run
 * on its host's runtime (a script the HTTP gateway loads, linked with its
 * runtime calls left to the host) binds them at load, and a host exports
 * only the archive members it linked; kept here, a program that loads
 * libraries links them, and with them the rest of the host API
 * (aether_exec.c keeps it). */
extern const char* aether_runtime_abi(void);
extern int aether_last_outcome(void);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used)) static void (*const dl_library_api[])(void) = {
    (void (*)(void))aether_runtime_abi, (void (*)(void))aether_last_outcome,
};
#endif

#if defined(AETHER_NO_THREADING)
/* A cooperative build: AETHER_TLS is empty there, which made this one
 * slot for every task with task contexts. It is a block of the calling
 * task's execution context instead (aether_context_local), and one shared
 * only when there was no memory for it, as std.json's error is. */
typedef struct { char text[AETHER_DL_ERR_CAP]; } DlErr;
static const char g_dl_last_err_key = 0;
static DlErr g_dl_last_err_no_memory;
static DlErr* dl_err_here(void) {
    DlErr* e = (DlErr*)aether_context_local(&g_dl_last_err_key, sizeof(DlErr), NULL);
    return e ? e : &g_dl_last_err_no_memory;
}
#define g_dl_last_err (dl_err_here()->text)
#else
static AETHER_TLS char g_dl_last_err[AETHER_DL_ERR_CAP];
#endif

static void dl_clear_error(void) {
    g_dl_last_err[0] = '\0';
}

static void dl_set_error(const char* msg) {
    if (msg == NULL) {
        g_dl_last_err[0] = '\0';
        return;
    }
    snprintf(g_dl_last_err, sizeof(g_dl_last_err), "%s", msg);
}

#ifdef _WIN32
static void dl_capture_winerr(const char* op, const char* arg) {
    DWORD code = GetLastError();
    char* sysmsg = NULL;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, 0, (LPSTR)&sysmsg, 0, NULL);
    if (n == 0 || sysmsg == NULL) {
        snprintf(g_dl_last_err, sizeof(g_dl_last_err),
                 "%s(%s) failed: error %lu", op, arg ? arg : "", (unsigned long)code);
    } else {
        // Strip trailing CRLF that FormatMessage appends.
        while (n > 0 && (sysmsg[n - 1] == '\r' || sysmsg[n - 1] == '\n' || sysmsg[n - 1] == ' ')) {
            sysmsg[--n] = '\0';
        }
        snprintf(g_dl_last_err, sizeof(g_dl_last_err),
                 "%s(%s) failed: %s (error %lu)", op, arg ? arg : "", sysmsg, (unsigned long)code);
        LocalFree(sysmsg);
    }
}
#endif

void* aether_dl_open_raw(const char* path) {
    dl_clear_error();
    if (path == NULL || path[0] == '\0') {
        dl_set_error("aether_dl_open_raw: path is empty");
        return NULL;
    }
    /* Loading a library runs its code with the program's authority: the
     * "native" grant (grant_native) the LD_PRELOAD layer checks dlopen
     * against, checked here too, so an in-process level sees it. */
    if (!aether_sandbox_check("native", path)) {
        dl_set_error("aether_dl_open_raw: denied by the sandbox");
        return NULL;
    }
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (h == NULL) {
        dl_capture_winerr("LoadLibraryA", path);
        return NULL;
    }
    return (void*)h;
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        const char* err = dlerror();
        dl_set_error(err ? err : "dlopen failed (no detail)");
    }
    return h;
#endif
}

void* aether_dl_symbol_raw(void* handle, const char* name) {
    dl_clear_error();
    if (handle == NULL) {
        dl_set_error("aether_dl_symbol_raw: handle is NULL");
        return NULL;
    }
    if (name == NULL || name[0] == '\0') {
        dl_set_error("aether_dl_symbol_raw: name is empty");
        return NULL;
    }
#ifdef _WIN32
    FARPROC sym = GetProcAddress((HMODULE)handle, name);
    if (sym == NULL) {
        dl_capture_winerr("GetProcAddress", name);
        return NULL;
    }
    return (void*)sym;
#else
    // Per POSIX, dlsym() may return NULL for a successfully resolved
    // symbol whose value is NULL. Distinguish via dlerror().
    (void)dlerror();
    void* sym = dlsym(handle, name);
    const char* err = dlerror();
    if (err != NULL) {
        dl_set_error(err);
        return NULL;
    }
    return sym;
#endif
}

int aether_dl_close_raw(void* handle) {
    dl_clear_error();
    if (handle == NULL) {
        return 1;
    }
#ifdef _WIN32
    if (FreeLibrary((HMODULE)handle) == 0) {
        dl_capture_winerr("FreeLibrary", NULL);
        return 0;
    }
    return 1;
#else
    if (dlclose(handle) != 0) {
        const char* err = dlerror();
        dl_set_error(err ? err : "dlclose failed (no detail)");
        return 0;
    }
    return 1;
#endif
}

const char* aether_dl_last_error_raw(void) {
    return g_dl_last_err;
}
