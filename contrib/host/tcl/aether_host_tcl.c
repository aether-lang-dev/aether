// aether_host_tcl.c — Embedded Tcl Language Host Module
//
// Embeds libtcl in the Aether process. When run_sandboxed is called,
// pushes its grants as a level of the calling thread's (#2955), so Tcl's libc calls (open,
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
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
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
    // The bidirectional tier (#904). All real exports in 8.6 and 9.0. The
    // reference counts go through Tcl_DbIncrRefCount / Tcl_DbDecrRefCount,
    // the functions behind the Tcl_IncrRefCount macros, because those macros
    // write the refCount field, whose width 9.0 changed.
    void         (*Tcl_DbIncrRefCount)(Tcl_Obj* objPtr, const char* file, int line);
    void         (*Tcl_DbDecrRefCount)(Tcl_Obj* objPtr, const char* file, int line);
    Tcl_Obj*     (*Tcl_NewWideIntObj)(Tcl_WideInt wideValue);
    Tcl_Obj*     (*Tcl_NewDoubleObj)(double doubleValue);
    Tcl_Obj*     (*Tcl_NewListObj)(Tcl_Size objc, Tcl_Obj* const objv[]);
    int          (*Tcl_ListObjAppendElement)(Tcl_Interp* interp, Tcl_Obj* listPtr,
                                             Tcl_Obj* objPtr);
    Tcl_Obj*     (*Tcl_SetVar2Ex)(Tcl_Interp* interp, const char* part1,
                                  const char* part2, Tcl_Obj* newValuePtr, int flags);
    int          (*Tcl_GetWideIntFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                          Tcl_WideInt* widePtr);
    int          (*Tcl_GetDoubleFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                         double* doublePtr);
    int          (*Tcl_GetBooleanFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr,
                                          int* boolPtr);
    void         (*Tcl_ResetResult)(Tcl_Interp* interp);
    int          (*Tcl_EvalObjEx)(Tcl_Interp* interp, Tcl_Obj* objPtr, int flags);
    Tcl_Obj*     (*Tcl_GetReturnOptions)(Tcl_Interp* interp, int result);
    int          (*Tcl_DictObjGet)(Tcl_Interp* interp, Tcl_Obj* dictPtr,
                                   Tcl_Obj* keyPtr, Tcl_Obj** valuePtrPtr);
    int          (*Tcl_GetIntFromObj)(Tcl_Interp* interp, Tcl_Obj* objPtr, int* intPtr);
    // Execution guards: interpreter resource limits.
    // int on 8.6; declared with the header's Tcl_Size so a 9.0 build that
    // widened it gets the full register. Values stay below INT_MAX.
    void         (*Tcl_LimitSetCommands)(Tcl_Interp* interp, Tcl_Size commandLimit);
    void         (*Tcl_LimitSetTime)(Tcl_Interp* interp, Tcl_Time* timeLimitPtr);
    void         (*Tcl_LimitSetGranularity)(Tcl_Interp* interp, int type,
                                            int granularity);
    void         (*Tcl_LimitTypeSet)(Tcl_Interp* interp, int type);
    void         (*Tcl_LimitTypeReset)(Tcl_Interp* interp, int type);
    void         (*Tcl_GetTime)(Tcl_Time* timeBuf);
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
    RESOLVE(Tcl_DbIncrRefCount,    "Tcl_DbIncrRefCount");
    RESOLVE(Tcl_DbDecrRefCount,    "Tcl_DbDecrRefCount");
    RESOLVE(Tcl_NewWideIntObj,     "Tcl_NewWideIntObj");
    RESOLVE(Tcl_NewDoubleObj,      "Tcl_NewDoubleObj");
    RESOLVE(Tcl_NewListObj,        "Tcl_NewListObj");
    RESOLVE(Tcl_ListObjAppendElement, "Tcl_ListObjAppendElement");
    RESOLVE(Tcl_SetVar2Ex,         "Tcl_SetVar2Ex");
    RESOLVE(Tcl_GetWideIntFromObj, "Tcl_GetWideIntFromObj");
    RESOLVE(Tcl_GetDoubleFromObj,  "Tcl_GetDoubleFromObj");
    RESOLVE(Tcl_GetBooleanFromObj, "Tcl_GetBooleanFromObj");
    RESOLVE(Tcl_ResetResult,       "Tcl_ResetResult");
    RESOLVE(Tcl_EvalObjEx,         "Tcl_EvalObjEx");
    RESOLVE(Tcl_GetReturnOptions,  "Tcl_GetReturnOptions");
    RESOLVE(Tcl_DictObjGet,        "Tcl_DictObjGet");
    RESOLVE(Tcl_GetIntFromObj,     "Tcl_GetIntFromObj");
    RESOLVE(Tcl_LimitSetCommands,  "Tcl_LimitSetCommands");
    RESOLVE(Tcl_LimitSetTime,      "Tcl_LimitSetTime");
    RESOLVE(Tcl_LimitSetGranularity, "Tcl_LimitSetGranularity");
    RESOLVE(Tcl_LimitTypeSet,      "Tcl_LimitTypeSet");
    RESOLVE(Tcl_LimitTypeReset,    "Tcl_LimitTypeReset");
    RESOLVE(Tcl_GetTime,           "Tcl_GetTime");
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

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

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
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    aether_sandbox_push(perms);

    int result = 0;
    if (tcl_eval(T, code) != TCL_OK) {
        fprintf(stderr, "[tcl] %s\n", tcl_string_result(T));
        result = -1;
    }

    aether_sandbox_pop();

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
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    register_map_bindings(T);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    current_map_token = map_token;

    aether_sandbox_push(perms);

    int result = 0;
    if (tcl_eval(T, code) != TCL_OK) {
        fprintf(stderr, "[tcl] %s\n", tcl_string_result(T));
        result = -1;
    }

    aether_sandbox_pop();
    current_map_token = 0;

    return result;
}


// --- Tier 2: Bidirectional Embedding API (#904) ----------------------------
//
// A VM is its own interpreter (Tcl_CreateInterp), separate from the shared
// one tcl_run uses: its own globals, procs and namespaces. Tcl's C API is
// command-oriented, not stack-oriented, so the Lua/Duktape shape maps over
// like this:
//   - a host callback is a Tcl command (Tcl_CreateObjCommand). Its name is
//     taken verbatim: "app.math.add" is one command a script calls as
//     `app.math.add 2 3` (Tcl allows dots in command names), and
//     "app::math::add" puts it in the namespace ::app::math;
//   - arg_* read the command's objv; push_* set the value the command
//     returns. A Tcl command returns one value, so the last push wins;
//   - an eval's result is the interpreter's result object, which the VM
//     holds a reference to until pop_result or the next eval.
//
// Tcl is apartment-threaded: an interpreter, and every Tcl_Obj it makes,
// belong to the thread that created them. An Aether program can reach a VM
// from any scheduler thread, so every entry point checks the caller is the
// creating thread and refuses otherwise, without touching Tcl.

#define AE_TCL_TNIL   0
#define AE_TCL_TBOOL  1
#define AE_TCL_TINT   2
#define AE_TCL_TSTR   3
#define AE_TCL_TTABLE 4
#define AE_TCL_TERR   5
#define AE_TCL_TOTHER 6
#define AE_TCL_TFLOAT 7

// What tcl_raise_error returns, so a host fn can `return tcl.raise_error(vm,
// msg)`. The trampoline turns it into TCL_ERROR after the host fn has
// returned, so the host fn's defers run.
#define AE_TCL_RAISE (-1)

#define AE_TCL_WRONG_THREAD "tcl vm used from a thread other than the one that created it"

/* One callback invocation: the command's arguments, and what the host fn
 * pushed or raised. A callback that evaluates on its own VM can reach a
 * second callback, so each trampoline keeps its own frame and restores the
 * one it interrupted. */
typedef struct AeTclFrame {
    int objc;
    Tcl_Obj* const* objv;
    Tcl_Obj* pushed;   /* one reference held, or NULL */
    Tcl_Obj* error;    /* one reference held, or NULL */
    struct AeTclFrame* prev;
} AeTclFrame;

typedef struct {
    Tcl_Interp* interp;
    pthread_t owner;
    Tcl_Obj* result;          /* one reference held, or NULL */
    int result_is_err;
    const char* c_error;      /* a refusal made without touching Tcl */
    AeTclFrame* frame;
    int eval_depth;
    int64_t command_budget;   /* <= 0: no command limit */
    int command_granularity;
    int64_t time_budget_ms;   /* <= 0: no time limit */
    int time_granularity;
} AeTclVm;

typedef struct {
    AeTclVm* vm;
    int (*host_fn)(void*);
} AeTclCommand;

static void ae_tcl_incr(Tcl_Obj* o) { g_tcl.Tcl_DbIncrRefCount(o, __FILE__, __LINE__); }
static void ae_tcl_decr(Tcl_Obj* o) { g_tcl.Tcl_DbDecrRefCount(o, __FILE__, __LINE__); }

/* Hold `o` in `*slot`, releasing what was there. The new reference is taken
 * first: `o` may be the object the slot already holds. */
static void ae_tcl_hold(Tcl_Obj** slot, Tcl_Obj* o) {
    if (o) ae_tcl_incr(o);
    if (*slot) ae_tcl_decr(*slot);
    *slot = o;
}

static int ae_tcl_on_owner(const AeTclVm* vm) {
    return vm && pthread_equal(vm->owner, pthread_self());
}

/* The value's type, from its internal representation. Every Tcl value is a
 * string, and one that has not been used as anything else has no other type:
 * the literal 5 in `set x 5` reads TSTR, `expr {2+3}` reads TINT. The
 * readers below convert from the string either way.
 *
 * Tcl_Obj's typePtr is read as a field. Tcl 9.0 widened refCount and length
 * from int to Tcl_Size, but on both LP64 and ILP32 that leaves typePtr at
 * three pointers in, in 8.6 and 9.0 alike, so the bridge reads the same
 * field whichever Tcl it was built against. The assertion holds the bridge
 * to that. Tcl_ObjType's first field is its name in both. */
_Static_assert(offsetof(Tcl_Obj, typePtr) == 3 * sizeof(void*),
               "Tcl_Obj.typePtr is where 8.6 and 9.0 both put it");

static int ae_tcl_type_of(Tcl_Obj* o) {
    if (!o) return AE_TCL_TNIL;
    const Tcl_ObjType* t = o->typePtr;
    if (!t || !t->name) {
        /* A pure string. Empty is Tcl's "no value", what a command that
         * returns nothing leaves. Tcl's strings never hold a raw zero byte
         * (a NUL is stored as C0 80), so a leading one means empty. */
        const char* str = tcl_obj_string(o);
        return str && str[0] ? AE_TCL_TSTR : AE_TCL_TNIL;
    }
    const char* n = t->name;
    if (strcmp(n, "int") == 0 || strcmp(n, "wideInt") == 0) return AE_TCL_TINT;
    if (strcmp(n, "double") == 0) return AE_TCL_TFLOAT;
    if (strcmp(n, "boolean") == 0 || strcmp(n, "booleanString") == 0) return AE_TCL_TBOOL;
    if (strcmp(n, "list") == 0 || strcmp(n, "dict") == 0) return AE_TCL_TTABLE;
    if (strcmp(n, "bignum") == 0) return AE_TCL_TOTHER;
    /* Anything else (a command name, a bytearray, a parsed script) is
     * read as its string. */
    return AE_TCL_TSTR;
}

/* A value as int64. An integer that fits reads as itself; a double, or an
 * integer too wide for 64 bits, saturates (NaN reads 0), as a C cast of one
 * out of range would be undefined; anything that is not a number reads 0. */
static int64_t ae_tcl_to_i64(Tcl_Obj* o) {
    if (!o) return 0;
    Tcl_WideInt w;
    if (g_tcl.Tcl_GetWideIntFromObj(NULL, o, &w) == TCL_OK) return (int64_t)w;
    double d;
    if (g_tcl.Tcl_GetDoubleFromObj(NULL, o, &d) != TCL_OK) return 0;
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

static double ae_tcl_to_double(Tcl_Obj* o) {
    double d;
    if (!o || g_tcl.Tcl_GetDoubleFromObj(NULL, o, &d) != TCL_OK) return 0.0;
    return d;
}

/* Tcl's own rules: 1/0, true/false, yes/no, on/off. Anything else reads 0. */
static int ae_tcl_to_bool(Tcl_Obj* o) {
    int b;
    if (!o || g_tcl.Tcl_GetBooleanFromObj(NULL, o, &b) != TCL_OK) return 0;
    return b != 0;
}

static const char* ae_tcl_to_str(Tcl_Obj* o) {
    if (!o) return "";
    const char* s = tcl_obj_string(o);
    return s ? s : "";
}

static int tcl_lib_ready = 0;

void* tcl_vm_new(void) {
    if (load_libtcl() != 0) return NULL;
    if (!tcl_lib_ready) {
        g_tcl.Tcl_FindExecutable(NULL);
        tcl_lib_ready = 1;
    }
    Tcl_Interp* interp = g_tcl.Tcl_CreateInterp();
    if (!interp) return NULL;
    if (g_tcl.Tcl_Init(interp) != TCL_OK) {
        fprintf(stderr, "[tcl] vm init: %s\n", tcl_string_result(interp));
        g_tcl.Tcl_DeleteInterp(interp);
        return NULL;
    }
    AeTclVm* vm = (AeTclVm*)calloc(1, sizeof(AeTclVm));
    if (!vm) {
        g_tcl.Tcl_DeleteInterp(interp);
        return NULL;
    }
    vm->interp = interp;
    vm->owner = pthread_self();
    return vm;
}

void tcl_vm_free(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return;
    if (!ae_tcl_on_owner(vm)) {
        /* Its interpreter can only be deleted on its own thread: leaked,
         * and said so, rather than deleted from the wrong one. */
        fprintf(stderr, "[tcl] vm_free: %s; not freed\n", AE_TCL_WRONG_THREAD);
        return;
    }
    ae_tcl_hold(&vm->result, NULL);
    /* Deleting the interpreter deletes its commands, whose delete procs free
     * their AeTclCommand records. */
    g_tcl.Tcl_DeleteInterp(vm->interp);
    free(vm);
}

static void ae_tcl_command_deleted(ClientData cd) { free(cd); }

static int ae_tcl_trampoline(ClientData cd, Tcl_Interp* interp,
                             int objc, Tcl_Obj* const objv[]) {
    AeTclCommand* cmd = (AeTclCommand*)cd;
    AeTclVm* vm = cmd->vm;
    AeTclFrame frame = {objc, objv, NULL, NULL, vm->frame};
    vm->frame = &frame;
    int rc = cmd->host_fn((void*)vm);
    vm->frame = frame.prev;

    int status = TCL_OK;
    if (rc == AE_TCL_RAISE && frame.error) {
        g_tcl.Tcl_SetObjResult(interp, frame.error);
        status = TCL_ERROR;
    } else if (rc > 0 && frame.pushed) {
        g_tcl.Tcl_SetObjResult(interp, frame.pushed);
    } else {
        g_tcl.Tcl_ResetResult(interp);
    }
    ae_tcl_hold(&frame.pushed, NULL);
    ae_tcl_hold(&frame.error, NULL);
    return status;
}

int tcl_vm_register(void* p, const char* name, void* host_fn) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !*name || !host_fn || !ae_tcl_on_owner(vm)) return -1;
    AeTclCommand* cmd = (AeTclCommand*)malloc(sizeof(AeTclCommand));
    if (!cmd) return -1;
    cmd->vm = vm;
    cmd->host_fn = (int (*)(void*))host_fn;
    if (!g_tcl.Tcl_CreateObjCommand(vm->interp, name, ae_tcl_trampoline, cmd,
                                    ae_tcl_command_deleted)) {
        free(cmd);
        return -1;
    }
    return 0;
}

static void ae_tcl_set_global(AeTclVm* vm, const char* name, Tcl_Obj* value) {
    ae_tcl_incr(value);
    g_tcl.Tcl_SetVar2Ex(vm->interp, name, NULL, value, TCL_GLOBAL_ONLY);
    ae_tcl_decr(value);
}

void tcl_vm_set_global_str(void* p, const char* name, const char* value) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    ae_tcl_set_global(vm, name, g_tcl.Tcl_NewStringObj(value ? value : "", -1));
}

void tcl_vm_set_global_int(void* p, const char* name, int64_t value) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    ae_tcl_set_global(vm, name, g_tcl.Tcl_NewWideIntObj((Tcl_WideInt)value));
}

/* A Tcl list of the strings in an Aether list. */
void tcl_vm_set_global_strlist(void* p, const char* name, void* items) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !name || !ae_tcl_on_owner(vm)) return;
    Tcl_Obj* list = g_tcl.Tcl_NewListObj(0, NULL);
    if (items) {
        int count = list_size(items);
        for (int i = 0; i < count; i++) {
            void* item = list_get_raw(items, i);
            const char* str = item ? aether_string_data(item) : "";
            g_tcl.Tcl_ListObjAppendElement(NULL, list,
                                           g_tcl.Tcl_NewStringObj(str ? str : "", -1));
        }
    }
    ae_tcl_set_global(vm, name, list);
}

/* Arms the limits for one outermost eval. A callback's nested eval runs
 * inside the same budget rather than starting a fresh one, or a script
 * could renew its own allowance through any callback that evaluates. */
static void ae_tcl_arm_limits(AeTclVm* vm) {
    Tcl_Interp* in = vm->interp;
    if (vm->command_budget > 0) {
        /* The command limit is a ceiling on the interpreter's running
         * command count, so the budget is added to the count now. */
        g_tcl.Tcl_LimitTypeReset(in, TCL_LIMIT_COMMANDS);
        int64_t now = 0;
        if (tcl_eval(in, "info cmdcount") == TCL_OK) now = ae_tcl_to_i64(g_tcl.Tcl_GetObjResult(in));
        int64_t ceiling = now + vm->command_budget;
        if (ceiling > INT_MAX) ceiling = INT_MAX;
        g_tcl.Tcl_LimitSetCommands(in, (Tcl_Size)ceiling);
        g_tcl.Tcl_LimitSetGranularity(in, TCL_LIMIT_COMMANDS, vm->command_granularity);
        g_tcl.Tcl_LimitTypeSet(in, TCL_LIMIT_COMMANDS);
    }
    if (vm->time_budget_ms > 0) {
        g_tcl.Tcl_LimitTypeReset(in, TCL_LIMIT_TIME);
        Tcl_Time t;
        g_tcl.Tcl_GetTime(&t);
        int64_t usec = (int64_t)t.usec + (vm->time_budget_ms % 1000) * 1000;
        t.sec += vm->time_budget_ms / 1000 + usec / 1000000;
        t.usec = usec % 1000000;
        g_tcl.Tcl_LimitSetTime(in, &t);
        g_tcl.Tcl_LimitSetGranularity(in, TCL_LIMIT_TIME, vm->time_granularity);
        g_tcl.Tcl_LimitTypeSet(in, TCL_LIMIT_TIME);
    }
}

/* Clears both limits, and the interpreter's "exceeded" state with them, so
 * the VM evaluates again after a run that hit one. */
static void ae_tcl_disarm_limits(AeTclVm* vm) {
    g_tcl.Tcl_LimitTypeReset(vm->interp, TCL_LIMIT_COMMANDS);
    g_tcl.Tcl_LimitTypeReset(vm->interp, TCL_LIMIT_TIME);
}

/* What a script that ended in `return` completes with, as Tcl decides it for
 * an outermost eval: `return -code c` one level up is c (`return v` is ok,
 * `return -code error m` an error); a return aimed further up stays
 * TCL_RETURN, which the caller reports as a bad code, as Tcl does. */
static int ae_tcl_return_code(Tcl_Interp* interp) {
    Tcl_Obj* opts = g_tcl.Tcl_GetReturnOptions(interp, TCL_RETURN);
    ae_tcl_incr(opts);
    int code = TCL_OK, level = 1;
    Tcl_Obj* key = g_tcl.Tcl_NewStringObj("-code", -1);
    Tcl_Obj* v = NULL;
    ae_tcl_incr(key);
    if (g_tcl.Tcl_DictObjGet(NULL, opts, key, &v) == TCL_OK && v) g_tcl.Tcl_GetIntFromObj(NULL, v, &code);
    ae_tcl_decr(key);
    key = g_tcl.Tcl_NewStringObj("-level", -1);
    ae_tcl_incr(key);
    v = NULL;
    if (g_tcl.Tcl_DictObjGet(NULL, opts, key, &v) == TCL_OK && v) g_tcl.Tcl_GetIntFromObj(NULL, v, &level);
    ae_tcl_decr(key);
    ae_tcl_decr(opts);
    return level <= 1 ? code : TCL_RETURN;
}

/* Runs `code` at global level, even from a callback a proc made, so a
 * callback's nested eval cannot see or set the proc's locals. 0 on success;
 * -1 on an error, which is then the result (TCL_TERR, its message as the
 * string). `return v` succeeds with v, and a `break` or `continue` outside
 * a loop is an error. Tcl_EvalObjEx does both itself only for the outermost
 * eval; for one nested in a callback, the code below does.
 *
 * The script goes in as an object (Tcl_EvalObjEx), which compiles it whole,
 * not as a string (Tcl_EvalEx), which runs it a command at a time. Tcl checks
 * its limits only inside compiled code, every few dozen instructions, so a
 * top-level `while 1 {incr i}` run as a string never reaches a check: each
 * pass of its body is a fresh compiled run too short to get there. */
int tcl_vm_eval(void* p, const char* code) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return -1;
    if (!ae_tcl_on_owner(vm)) {
        vm->c_error = AE_TCL_WRONG_THREAD;
        return -1;
    }
    if (!code) return -1;
    vm->c_error = NULL;
    if (vm->eval_depth == 0) ae_tcl_arm_limits(vm);
    vm->eval_depth++;
    Tcl_Obj* script = g_tcl.Tcl_NewStringObj(code, -1);
    ae_tcl_incr(script);
    int rc = g_tcl.Tcl_EvalObjEx(vm->interp, script, TCL_EVAL_GLOBAL);
    ae_tcl_decr(script);
    vm->eval_depth--;
    if (rc == TCL_RETURN) rc = ae_tcl_return_code(vm->interp);
    if (rc != TCL_OK && rc != TCL_ERROR) {
        char msg[64];
        if (rc == TCL_BREAK) snprintf(msg, sizeof msg, "invoked \"break\" outside of a loop");
        else if (rc == TCL_CONTINUE) snprintf(msg, sizeof msg, "invoked \"continue\" outside of a loop");
        else snprintf(msg, sizeof msg, "command returned bad code: %d", rc);
        g_tcl.Tcl_SetObjResult(vm->interp, g_tcl.Tcl_NewStringObj(msg, -1));
        rc = TCL_ERROR;
    }
    ae_tcl_hold(&vm->result, g_tcl.Tcl_GetObjResult(vm->interp));
    vm->result_is_err = rc == TCL_ERROR;
    if (vm->eval_depth == 0) ae_tcl_disarm_limits(vm);
    g_tcl.Tcl_ResetResult(vm->interp);
    return rc == TCL_ERROR ? -1 : 0;
}

int tcl_vm_result_type(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return AE_TCL_TNIL;
    if (vm->c_error) return AE_TCL_TERR;
    if (!ae_tcl_on_owner(vm) || !vm->result) return AE_TCL_TNIL;
    if (vm->result_is_err) return AE_TCL_TERR;
    return ae_tcl_type_of(vm->result);
}

int64_t tcl_vm_result_int(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0;
    return ae_tcl_to_i64(vm->result);
}

double tcl_vm_result_double(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0.0;
    return ae_tcl_to_double(vm->result);
}

/* Valid until pop_result or the VM's next eval. */
const char* tcl_vm_result_str(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "";
    if (vm->c_error) return vm->c_error;
    if (!ae_tcl_on_owner(vm)) return "";
    return ae_tcl_to_str(vm->result);
}

int tcl_vm_result_bool(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || vm->c_error || !ae_tcl_on_owner(vm)) return 0;
    return ae_tcl_to_bool(vm->result);
}

void tcl_vm_pop_result(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return;
    vm->c_error = NULL;
    if (!ae_tcl_on_owner(vm)) return;
    ae_tcl_hold(&vm->result, NULL);
    vm->result_is_err = 0;
}

/* A budget of Tcl commands per outermost eval, checked every `every`
 * commands; `budget` <= 0 removes it. Commands are what Tcl counts, so a
 * loop that runs none (`while 1 {}`, compiled to jumps) is not bounded by
 * it: tcl_vm_set_time_limit is. "" when set. */
const char* tcl_vm_set_instruction_limit(void* p, int64_t budget, int every) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "tcl vm is null";
    if (!ae_tcl_on_owner(vm)) return AE_TCL_WRONG_THREAD;
    vm->command_budget = budget;
    vm->command_granularity = every > 0 ? every : 1;
    return "";
}

/* A wall-clock budget in milliseconds per outermost eval, checked every
 * `every` checkpoints; `ms` <= 0 removes it. Tcl checks it on a loop's
 * backward jump too, so it bounds the loops the command limit does not.
 * Time spent inside a host callback counts. "" when set. */
const char* tcl_vm_set_time_limit(void* p, int64_t ms, int every) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm) return "tcl vm is null";
    if (!ae_tcl_on_owner(vm)) return AE_TCL_WRONG_THREAD;
    vm->time_budget_ms = ms;
    vm->time_granularity = every > 0 ? every : 1;
    return "";
}

/* The current callback's argument `i` (1-based; the command's name is not
 * one), or NULL outside a callback or past the last. */
static Tcl_Obj* ae_tcl_arg(void* p, int i) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame || i < 1 || i >= vm->frame->objc) return NULL;
    return vm->frame->objv[i];
}

int tcl_arg_count(void* p) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return 0;
    return vm->frame->objc - 1;
}

int tcl_arg_type(void* p, int i) { return ae_tcl_type_of(ae_tcl_arg(p, i)); }
int64_t tcl_arg_int(void* p, int i) { return ae_tcl_to_i64(ae_tcl_arg(p, i)); }
double tcl_arg_double(void* p, int i) { return ae_tcl_to_double(ae_tcl_arg(p, i)); }
int tcl_arg_bool(void* p, int i) { return ae_tcl_to_bool(ae_tcl_arg(p, i)); }
/* Valid until the callback returns. */
const char* tcl_arg_str(void* p, int i) { return ae_tcl_to_str(ae_tcl_arg(p, i)); }

static void ae_tcl_push(void* p, Tcl_Obj* o) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return;
    ae_tcl_hold(&vm->frame->pushed, o);
}

static int ae_tcl_in_callback(void* p) {
    return p && ((AeTclVm*)p)->frame != NULL;
}

void tcl_push_int(void* p, int64_t v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewWideIntObj((Tcl_WideInt)v));
}
void tcl_push_double(void* p, double v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewDoubleObj(v));
}
/* Tcl has no boolean type: true is 1, false is 0. */
void tcl_push_bool(void* p, int v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewWideIntObj(v != 0));
}
void tcl_push_str(void* p, const char* v) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, g_tcl.Tcl_NewStringObj(v ? v : "", -1));
}
/* Tcl has no nil: the empty string, which reads back TCL_TNIL. */
void tcl_push_nil(void* p) {
    if (ae_tcl_in_callback(p)) ae_tcl_push(p, NULL);
}

int tcl_raise_error(void* p, const char* msg) {
    AeTclVm* vm = (AeTclVm*)p;
    if (!vm || !vm->frame) return 0;
    ae_tcl_hold(&vm->frame->error, g_tcl.Tcl_NewStringObj(msg ? msg : "error", -1));
    return AE_TCL_RAISE;
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
void* tcl_vm_new(void) { tcl_init(); return NULL; }
void tcl_vm_free(void* vm) { (void)vm; }
int tcl_vm_register(void* vm, const char* name, void* host_fn) { (void)vm; (void)name; (void)host_fn; return -1; }
void tcl_vm_set_global_str(void* vm, const char* name, const char* value) { (void)vm; (void)name; (void)value; }
void tcl_vm_set_global_int(void* vm, const char* name, int64_t value) { (void)vm; (void)name; (void)value; }
void tcl_vm_set_global_strlist(void* vm, const char* name, void* items) { (void)vm; (void)name; (void)items; }
int tcl_vm_eval(void* vm, const char* code) { (void)vm; (void)code; return -1; }
int tcl_vm_result_type(void* vm) { (void)vm; return 0; }
int64_t tcl_vm_result_int(void* vm) { (void)vm; return 0; }
double tcl_vm_result_double(void* vm) { (void)vm; return 0.0; }
const char* tcl_vm_result_str(void* vm) { (void)vm; return ""; }
int tcl_vm_result_bool(void* vm) { (void)vm; return 0; }
void tcl_vm_pop_result(void* vm) { (void)vm; }
const char* tcl_vm_set_instruction_limit(void* vm, int64_t budget, int every) { (void)vm; (void)budget; (void)every; return "contrib.host.tcl not available"; }
const char* tcl_vm_set_time_limit(void* vm, int64_t ms, int every) { (void)vm; (void)ms; (void)every; return "contrib.host.tcl not available"; }
int tcl_arg_count(void* vm) { (void)vm; return 0; }
int tcl_arg_type(void* vm, int i) { (void)vm; (void)i; return 0; }
int64_t tcl_arg_int(void* vm, int i) { (void)vm; (void)i; return 0; }
double tcl_arg_double(void* vm, int i) { (void)vm; (void)i; return 0.0; }
int tcl_arg_bool(void* vm, int i) { (void)vm; (void)i; return 0; }
const char* tcl_arg_str(void* vm, int i) { (void)vm; (void)i; return ""; }
void tcl_push_int(void* vm, int64_t v) { (void)vm; (void)v; }
void tcl_push_double(void* vm, double v) { (void)vm; (void)v; }
void tcl_push_bool(void* vm, int v) { (void)vm; (void)v; }
void tcl_push_str(void* vm, const char* v) { (void)vm; (void)v; }
void tcl_push_nil(void* vm) { (void)vm; }
int tcl_raise_error(void* vm, const char* msg) { (void)vm; (void)msg; return 0; }
#endif
