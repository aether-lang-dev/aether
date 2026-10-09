/* aether_script_gateway.c: in-process script gateway (issue #384).
 *
 * Mount-time pipeline:
 *   1. Load the library through std.dl: dlopen(RTLD_NOW | RTLD_LOCAL) on
 *      POSIX, LoadLibrary on Windows.
 *   2. Look up "aether_script_handle" in it.
 *   3. On Windows, check that the host and the script run on one runtime
 *      (see sg_host_runtime_check and sg_script_runtime_check).
 *   4. Allocate a ScriptMount struct holding (path_prefix, fn).
 *   5. Register a middleware on the host server that fires the fn
 *      when req->path starts with path_prefix.
 *
 * Per-request hot path is one strncmp + one indirect call: no
 * fork, no exec, no IPC.
 */

#include "aether_script_gateway.h"
#include "../../net/aether_http_server.h"
#include "../../dl/aether_dl.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/utils/aether_compiler.h"  /* AETHER_TLS */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>        /* _access */
#else
#include <unistd.h>    /* access */
#endif

/* (handle, kind, message) tuple: same ABI as fs.copy's tuple but
 * named per-module to keep the typedef tags from colliding across
 * .c files. */
typedef struct {
    int _0;            // 1 on success (mount registered) / 0 on failure
    int _1;            // AETHER_SCRIPT_GATEWAY_KIND_*
    const char* _2;    // "" on success, error message on failure
} _aether_sg_tuple;

/* Aether-side script entrypoint. Matches the standard HttpHandler
 * signature so we can use the C type directly as the symbol type. */
typedef void (*ScriptEntry)(HttpRequest* req,
                            HttpServerResponse* res,
                            void* user_data);

typedef struct ScriptMount {
    char* prefix;          // path prefix (heap copy; freed only at exit)
    size_t prefix_len;     // strlen(prefix); precomputed for hot-path strncmp
    void* dl_handle;       // std.dl handle (intentionally never closed)
    ScriptEntry fn;        // resolved entrypoint
} ScriptMount;

/* Middleware thunk: the host server calls this with the per-mount
 * ScriptMount* in `user_data`. Return 1 = continue chain (path
 * didn't match; let other middleware / route dispatch handle the
 * request). Return 0 = short-circuit (we handled it; don't call
 * downstream handlers). */
static int script_gateway_middleware(HttpRequest* req,
                                     HttpServerResponse* res,
                                     void* user_data) {
    ScriptMount* m = (ScriptMount*)user_data;
    if (!m || !m->fn) return 1;
    const char* path = http_request_path(req);
    if (!path) return 1;
    /* Match the prefix. The empty string matches everything (rare
     * but useful as a catch-all gateway). */
    if (m->prefix_len > 0 && strncmp(path, m->prefix, m->prefix_len) != 0) {
        return 1;
    }
    /* Fire the script's handler. We do NOT pass m->dl_handle through
     * user_data: the script gets a NULL ud, same as a hand-written
     * @c_callback HttpHandler that didn't bind state. If a future
     * version wants to pass per-mount state to the script it can
     * extend this thunk; not in scope for v1. */
    m->fn(req, res, NULL);
    return 0;  // short-circuit: we handled the request
}

static int sg_readable(const char* path) {
#ifdef _WIN32
    return _access(path, 4) == 0;      /* 4: read permission */
#else
    return access(path, R_OK) == 0;
#endif
}

#ifdef _WIN32
/* A POSIX host links with -rdynamic, so a script built with its runtime
 * calls left unresolved binds them to the host's own runtime at dlopen.
 * Windows has nothing like it: a DLL's imports name the module that
 * provides them, and a script cannot import from the host executable. Host
 * and script share one runtime there only when both link the shared runtime
 * (`ae build --shared-runtime`, aether.dll), which is what this checks.
 *
 * Two runtimes in one process is not a slower version of one: each keeps its
 * own resource caps, config and panic frames, while the script fills in
 * request and response objects the host's runtime later frees. So a mismatch
 * on either side is refused at mount, with the build flag that fixes it,
 * rather than left to fail somewhere inside a request. Each check returns
 * NULL when the runtime is shared, or the reason it is not. */

/* The reason a script was refused, per thread like std.dl's last error. */
static AETHER_TLS char g_sg_err[512];

/* Whether the loaded image `mod` imports from the DLL named `dll_name`,
 * read from its import directory. */
static int sg_module_imports(HMODULE mod, const char* dll_name) {
    const unsigned char* base = (const unsigned char*)mod;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress == 0 || dir->Size == 0) return 0;
    const IMAGE_IMPORT_DESCRIPTOR* imp =
        (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name != 0; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll_name) == 0) return 1;
    }
    return 0;
}

/* The module this runtime was linked into: the host executable for a
 * static build, aether.dll for a --shared-runtime one. */
static HMODULE sg_runtime_module(void) {
    HMODULE self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void*)&sg_runtime_module, &self))
        return NULL;
    return self;
}

static const char* sg_host_runtime_check(void) {
    HMODULE rt = sg_runtime_module();
    if (rt && rt != GetModuleHandleA(NULL)) return NULL;
    return "script_gateway: on Windows the host must be built with "
           "`ae build --shared-runtime`, so a script can share its runtime";
}

static const char* sg_script_runtime_check(void* script, const char* so_path) {
    char rt_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(sg_runtime_module(), rt_path, sizeof(rt_path));
    if (n == 0 || n >= sizeof(rt_path)) return "script_gateway: cannot name the host's runtime";
    const char* rt_name = rt_path;
    for (const char* p = rt_path; *p; p++)
        if (*p == '\\' || *p == '/') rt_name = p + 1;
    if (sg_module_imports((HMODULE)script, rt_name)) return NULL;
    snprintf(g_sg_err, sizeof(g_sg_err),
             "script_gateway: %s does not run on the host's runtime (%s); "
             "build it with `ae build --emit=lib --shared-runtime`",
             so_path, rt_name);
    return g_sg_err;
}
#endif  /* _WIN32 */

_aether_sg_tuple script_gateway_mount_so_raw(void* server,
                                             const char* path_prefix,
                                             const char* so_path) {
    if (!server || !path_prefix || !so_path) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 "null arg" };
        return out;
    }
    /* The `.so` path is read+exec; the host server is locally
     * authoritative on which script files it's willing to mount.
     * Sandbox check: this is essentially a `dlopen`, so gate it
     * behind the fs_read sandbox so a sandboxed Aether host can't
     * bring in arbitrary native code. */
    if (!aether_sandbox_check("fs_read", so_path)) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 "sandbox: cannot read .so" };
        return out;
    }

    /* Cheap pre-check: if the file isn't there, the loader's error
     * message is implementation-defined and not always helpful.
     * A readability probe gives us a clean KIND_NOT_FOUND surface. */
    if (!sg_readable(so_path)) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_NOT_FOUND,
                                 "so_path not readable" };
        return out;
    }

#ifdef _WIN32
    /* Before loading anything: a static host cannot share its runtime
     * with any script, so there is nothing to load the script for. */
    const char* host_err = sg_host_runtime_check();
    if (host_err) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO, host_err };
        return out;
    }
#endif

    /* POSIX: RTLD_NOW binds every symbol at load (fail fast on missing
     * runtime references, better than mysterious aborts on first
     * request) and RTLD_LOCAL keeps the script's symbols out of the
     * host's global namespace. Windows: LoadLibrary binds every import
     * at load, which is the same fail-fast. */
    void* h = aether_dl_open_raw(so_path);
    if (!h) {
        const char* err = aether_dl_last_error_raw();
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 (err && *err) ? err : "loading the library failed" };
        return out;
    }

    /* The script's exported entrypoint name is fixed: callers write
     *
     *   @c_callback aether_script_handle(req: ptr, res: ptr, ud: ptr) {
     *       ...
     *   }
     *
     * at the top level of their .ae script and aetherc emits the
     * symbol unmangled. */
    void* sym = aether_dl_symbol_raw(h, "aether_script_handle");
    if (!sym) {
        const char* err = aether_dl_last_error_raw();
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 (err && *err) ? err : "missing aether_script_handle entrypoint" };
        /* The handle leaks intentionally (see header docstring); the
         * mount failed, the host can either retry or exit. */
        return out;
    }

#ifdef _WIN32
    const char* script_err = sg_script_runtime_check(h, so_path);
    if (script_err) {
        /* Its runtime is not the host's, so nothing of it may run here. */
        aether_dl_close_raw(h);
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO, script_err };
        return out;
    }
#endif

    ScriptMount* m = (ScriptMount*)calloc(1, sizeof(ScriptMount));
    if (!m) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 "alloc failed" };
        return out;
    }
    m->prefix = strdup(path_prefix);
    if (!m->prefix) {
        free(m);
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 "alloc failed" };
        return out;
    }
    m->prefix_len = strlen(path_prefix);
    m->dl_handle = h;
    m->fn = (ScriptEntry)sym;

    http_server_use_middleware((HttpServer*)server,
                               script_gateway_middleware,
                               m);

    _aether_sg_tuple out = { 1, AETHER_SCRIPT_GATEWAY_KIND_OK, "" };
    return out;
}
