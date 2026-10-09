// aether_host_tcl.c — Embedded Tcl Language Host Module
//
// Embeds libtcl in the Aether process. When run_sandboxed is called,
// installs the Aether sandbox checker so Tcl's libc calls (open,
// getenv, socket, exec) are intercepted and checked against the
// grant list.
//
// LOADING MODEL: dlopen, not -ltcl. Mirrors contrib/host/python,
// ruby, lua, perl, js — every Tcl C-API symbol resolved via dlsym
// at first call. The bridge .a has NO unresolved Tcl_* symbols at
// link time, so end-user binaries have no DT_NEEDED libtcl and are
// ABI-portable across deploy-host Tcl minor versions (8.6 / 9.0
// supported).
//
// Tcl's C API is MOSTLY non-macro — most `Tcl_*` symbols are real
// exported functions, so the bridge is largely a plain dlsym table.
// The exceptions (since Tcl 9.0): `Tcl_GetStringResult`, `Tcl_GetString`
// and `Tcl_Eval` became function-like macros over `Tcl_GetStringFromObj`
// and `Tcl_EvalEx`, and are no longer exported. We dlsym the stable LCD
// exports (`Tcl_GetStringFromObj`, `Tcl_GetObjResult`, `Tcl_EvalEx`) and
// recompose those three accessors — see the #undef + helpers below.

#include "aether_host_tcl.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_TCL
#include <tcl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tcl 9.0 turned the result/string accessors AND Tcl_Eval into macros:
 *   #define Tcl_GetStringResult(i) Tcl_GetString(Tcl_GetObjResult(i))
 *   #define Tcl_GetString(o)       Tcl_GetStringFromObj(o, (Tcl_Size*)NULL)
 *   #define Tcl_Eval(i, s)         Tcl_EvalEx(i, s, TCL_INDEX_NONE, 0)
 * Left in place these rewrite our struct-field calls into references to
 * non-existent g_tcl members (the macOS/Homebrew Tcl 9.0 break). Tcl 8.6
 * exported all three as real functions; Tcl 9.0 exports none of them. The
 * lowest common denominator — real exports in BOTH 8.6 and 9.0 — is
 * Tcl_GetStringFromObj + Tcl_GetObjResult + Tcl_EvalEx, so we dlsym those
 * and build the accessors ourselves below. Undo the macros so our plain
 * function-pointer fields keep their names. (Harmless no-op on 8.6, which
 * lacks the macros.) */
#ifdef Tcl_GetStringResult
#undef Tcl_GetStringResult
#endif
#ifdef Tcl_GetString
#undef Tcl_GetString
#endif
#ifdef Tcl_Eval
#undef Tcl_Eval
#endif

/* Tcl 8.7 / 9.0 widened the count/length params (Tcl_EvalEx's numBytes,
 * Tcl_NewStringObj's length, Tcl_WrongNumArgs's objc) from int to Tcl_Size
 * (ptrdiff_t under the 9.0 API). Because we declare our own prototypes for
 * the dlsym'd symbols, those prototypes must use the header's own width or
 * the call ABI is wrong. Tcl 8.6 has no Tcl_Size at all; TCL_SIZE_MAX is the
 * marker for a header that does. */
#ifndef TCL_SIZE_MAX
typedef int Tcl_Size;
#endif

// --- libtcl dlopen table ----------------------------------------------------

static void* libtcl_handle = NULL;

static struct {
    // Lifecycle.
    void         (*Tcl_FindExecutable)(const char* argv0);
    Tcl_Interp*  (*Tcl_CreateInterp)(void);
    int          (*Tcl_Init)(Tcl_Interp* interp);
    void         (*Tcl_DeleteInterp)(Tcl_Interp* interp);
    void         (*Tcl_Finalize)(void);
    // Eval + result extraction. We resolve the LCD real exports
    // (present in both 8.6 and 9.0) rather than the now-macro
    // Tcl_Eval / Tcl_GetStringResult / Tcl_GetString, then compose them
    // in the tcl_eval() / tcl_string_result() / tcl_obj_string() helpers
    // below. The last arg of Tcl_GetStringFromObj is int* on 8.6 and
    // Tcl_Size* on 9.0; we only ever pass NULL, so type it void* to stay
    // width-agnostic.
    int          (*Tcl_EvalEx)(Tcl_Interp* interp, const char* script,
                               Tcl_Size numBytes, int flags);
    Tcl_Obj*     (*Tcl_GetObjResult)(Tcl_Interp* interp);
    const char*  (*Tcl_GetStringFromObj)(Tcl_Obj* objPtr, void* lengthPtr);
    // Object/string helpers (for the shared-map command callbacks).
    Tcl_Obj*     (*Tcl_NewStringObj)(const char* bytes, Tcl_Size length);
    void         (*Tcl_SetObjResult)(Tcl_Interp* interp, Tcl_Obj* resultObjPtr);
    void         (*Tcl_WrongNumArgs)(Tcl_Interp* interp, Tcl_Size objc,
                                     Tcl_Obj* const objv[], const char* message);
    Tcl_Command  (*Tcl_CreateObjCommand)(Tcl_Interp* interp, const char* cmdName,
                                         Tcl_ObjCmdProc* proc, ClientData clientData,
                                         Tcl_CmdDeleteProc* deleteProc);
} g_tcl;

static int resolve_tcl_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_tcl.field) = dlsym(h, sym);                       \
        if (!g_tcl.field) {                                            \
            fprintf(stderr,                                            \
                "aether host_tcl: libtcl missing symbol %s\n", sym);   \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(Tcl_FindExecutable,    "Tcl_FindExecutable");
    RESOLVE(Tcl_CreateInterp,      "Tcl_CreateInterp");
    RESOLVE(Tcl_Init,              "Tcl_Init");
    RESOLVE(Tcl_DeleteInterp,      "Tcl_DeleteInterp");
    RESOLVE(Tcl_Finalize,          "Tcl_Finalize");
    RESOLVE(Tcl_EvalEx,            "Tcl_EvalEx");
    RESOLVE(Tcl_GetObjResult,      "Tcl_GetObjResult");
    RESOLVE(Tcl_GetStringFromObj,  "Tcl_GetStringFromObj");
    RESOLVE(Tcl_NewStringObj,      "Tcl_NewStringObj");
    RESOLVE(Tcl_SetObjResult,      "Tcl_SetObjResult");
    RESOLVE(Tcl_WrongNumArgs,      "Tcl_WrongNumArgs");
    RESOLVE(Tcl_CreateObjCommand,  "Tcl_CreateObjCommand");
    return 0;
#undef RESOLVE
}

/* Replacement for the Tcl_Eval macro. A -1 length means "script is
 * NUL-terminated, measure it" on both 8.6 and 9.0 (9.0 spells the same
 * value TCL_INDEX_NONE); 0 flags matches what Tcl_Eval always passed. */
static int tcl_eval(Tcl_Interp* interp, const char* script) {
    return g_tcl.Tcl_EvalEx(interp, script, (Tcl_Size)-1, 0);
}

/* Replacements for the Tcl_GetStringResult / Tcl_GetString macros,
 * composed from the two LCD real exports. NULL length pointer = "don't
 * report the length" on both 8.6 (int*) and 9.0 (Tcl_Size*). */
static const char* tcl_string_result(Tcl_Interp* interp) {
    return g_tcl.Tcl_GetStringFromObj(g_tcl.Tcl_GetObjResult(interp), NULL);
}
static const char* tcl_obj_string(Tcl_Obj* obj) {
    return g_tcl.Tcl_GetStringFromObj(obj, NULL);
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libtcl on first use. Strict two-step contract:
//   1. ${AETHER_TCL_SONAME} env var (orchestrator-supplied exact,
//      e.g. "libtcl8.6.so"). The orchestrator MUST probe via
//      `echo 'puts $tcl_version' | tclsh` (gives the major.minor)
//      and derive the soname.
//   2. libtcl.so (Debian-style unversioned symlink, present with
//      tcl-dev; not always on bare runtime hosts).
// No hardcoded version-list fallback. Orchestrator owns the probe.
static int load_libtcl(void) {
    if (libtcl_handle) return 0;

    const char* env_soname = getenv("AETHER_TCL_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libtcl.so");
    if (!h) {
        fprintf(stderr,
            "aether host_tcl: cannot dlopen libtcl "
            "(tried $AETHER_TCL_SONAME=%s, libtcl.so).\n"
            "  Install a tcl runtime on the host, or set AETHER_TCL_SONAME "
            "to the exact soname.\n"
            "  Hint: tclsh -c 'puts $tcl_library' (gives a dir; soname "
            "is libtcl<version>.so based on $tcl_version)\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_tcl_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libtcl_handle = h;
    return 0;
}

// --- bridge state (unchanged) ----------------------------------------------

static Tcl_Interp* T = NULL;

// Bridge-owned permission stack.
static void* tcl_perms_stack[64];
static int   tcl_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

static int perms_allow(void* ctx, const char* category, const char* resource) {
    if (!ctx) return 1;
    int n = list_size(ctx);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(ctx, i));
        const char* pat = aether_string_data(list_get_raw(ctx, i + 1));
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && aether_grant_match(category, pat, resource)) return 1;
    }
    return 0;
}

static int host_tcl_checker(const char* category, const char* resource) {
    if (tcl_perms_depth <= 0) return 1;
    for (int level = 0; level < tcl_perms_depth; level++) {
        if (!perms_allow(tcl_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int tcl_init(void) {
    if (T) return 0;
    if (load_libtcl() != 0) return -1;
    g_tcl.Tcl_FindExecutable(NULL);
    T = g_tcl.Tcl_CreateInterp();
    if (!T) return -1;
    if (g_tcl.Tcl_Init(T) != TCL_OK) {
        fprintf(stderr, "[tcl] init: %s\n", tcl_string_result(T));
        g_tcl.Tcl_DeleteInterp(T);
        T = NULL;
        return -1;
    }
    return 0;
}

void tcl_finalize(void) {
    if (T) {
        g_tcl.Tcl_DeleteInterp(T);
        T = NULL;
    }
    if (g_tcl.Tcl_Finalize) g_tcl.Tcl_Finalize();
}

int tcl_run(const char* code) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    if (tcl_eval(T, code) != TCL_OK) {
        fprintf(stderr, "[tcl] %s\n", tcl_string_result(T));
        return -1;
    }
    return 0;
}

int tcl_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    if (tcl_perms_depth >= 64) return -1;

    tcl_perms_stack[tcl_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_tcl_checker;

    int result = 0;
    if (tcl_eval(T, code) != TCL_OK) {
        fprintf(stderr, "[tcl] %s\n", tcl_string_result(T));
        result = -1;
    }

    _aether_sandbox_checker = prev;
    tcl_perms_depth--;

    return result;
}

// --- Shared map native bindings for Tcl ------------------------------------

static uint64_t current_map_token = 0;

static int tcl_aether_map_get(ClientData cd, Tcl_Interp* interp,
                               int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 2) {
        g_tcl.Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    const char* key = tcl_obj_string(objv[1]);
    const char* val = aether_shared_map_get_by_token(current_map_token, key);
    if (val) {
        g_tcl.Tcl_SetObjResult(interp, g_tcl.Tcl_NewStringObj(val, -1));
    } else {
        g_tcl.Tcl_SetObjResult(interp, g_tcl.Tcl_NewStringObj("", 0));
    }
    return TCL_OK;
}

static int tcl_aether_map_put(ClientData cd, Tcl_Interp* interp,
                               int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 3) {
        g_tcl.Tcl_WrongNumArgs(interp, 1, objv, "key value");
        return TCL_ERROR;
    }
    const char* key = tcl_obj_string(objv[1]);
    const char* val = tcl_obj_string(objv[2]);
    aether_shared_map_put_by_token(current_map_token, key, val);
    return TCL_OK;
}

static void register_map_bindings(Tcl_Interp* interp) {
    g_tcl.Tcl_CreateObjCommand(interp, "aether_map_get",
                                tcl_aether_map_get, NULL, NULL);
    g_tcl.Tcl_CreateObjCommand(interp, "aether_map_put",
                                tcl_aether_map_put, NULL, NULL);
}

int tcl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (tcl_init() != 0) return -1;
    if (tcl_perms_depth >= 64) return -1;
    register_map_bindings(T);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    current_map_token = map_token;

    tcl_perms_stack[tcl_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_tcl_checker;

    int result = 0;
    if (tcl_eval(T, code) != TCL_OK) {
        fprintf(stderr, "[tcl] %s\n", tcl_string_result(T));
        result = -1;
    }

    _aether_sandbox_checker = prev;
    tcl_perms_depth--;
    current_map_token = 0;

    return result;
}

#else
#include <stdio.h>
int tcl_init(void) {
    fprintf(stderr, "error: contrib.host.tcl not available (compile with AETHER_HAS_TCL)\n");
    return -1;
}
void tcl_finalize(void) {}
int tcl_run(const char* code) { (void)code; return tcl_init(); }
int tcl_run_sandboxed(void* perms, const char* code) {
    (void)perms; (void)code; return tcl_init();
}
int tcl_run_sandboxed_with_map(void* perms, const char* code, uint64_t token) {
    (void)perms; (void)code; (void)token; return tcl_init();
}
#endif
