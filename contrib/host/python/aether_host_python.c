// aether_host_python.c — Embedded Python Language Host Module
//
// Embeds CPython in the Aether process. When run_sandboxed is called,
// installs the Aether sandbox checker so Python's libc calls are
// intercepted and checked against the grant list.
//
// LOADING MODEL: dlopen, not -lpython. Resolves libpython at the
// deploy host's runtime via `dlopen("libpython3.so", RTLD_NOW|RTLD_GLOBAL)`,
// with `${AETHER_PYTHON_SONAME}` (e.g. `libpython3.14.so.1.0`) as a
// fallback for hosts where the unversioned symlink isn't present
// (typically the case on Debian unless python3-dev is installed,
// reliably present on Fedora-likes like Bazzite). All Python C API
// references go through dlsym function pointers — the bridge .a
// has NO unresolved CPython symbols at link time, so `ae build`
// produces binaries with no `-lpython` and no DT_NEEDED for libpython,
// making them ABI-portable across the deploy host's Python minor
// version. The compile-anywhere / run-where-Python-is shape.
//
// RTLD_GLOBAL is important: Python C extensions loaded later
// (e.g. ctypes, native modules) must resolve their Py_* references
// against THIS libpython, not pick up a second copy. Without
// RTLD_GLOBAL they get isolated and segfault on shared singletons
// like _Py_NoneStruct.

#include "aether_host_python.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_PYTHON
#include <Python.h>
#include "../aep_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- libpython dlopen table -------------------------------------------------
//
// All CPython C API access goes through this table. Populated by
// load_libpython() on first use; load failure makes every entry point
// return -1 with a clear message rather than segfault.

static void* libpython_handle = NULL;

static struct {
    // Lifecycle.
    void      (*Py_Initialize)(void);
    void      (*Py_Finalize)(void);
    // Refcount helpers (real exported functions, used to avoid macros
    // that touch struct internals).
    void      (*Py_IncRef)(PyObject*);
    void      (*Py_DecRef)(PyObject*);
    // Execution.
    int       (*PyRun_SimpleStringFlags)(const char*, PyCompilerFlags*);
    // Args parsing — varargs; dlsym is fine with varargs sigs.
    int       (*PyArg_ParseTuple)(PyObject*, const char*, ...);
    // Value builders.
    PyObject* (*PyUnicode_FromString)(const char*);
    // Module / dict.
    PyObject* (*PyModule_Create2)(PyModuleDef*, int);
    PyObject* (*PyImport_GetModuleDict)(void);
    int       (*PyDict_SetItemString)(PyObject*, const char*, PyObject*);
    // The None singleton — a DATA symbol, dlsym'd via its public name.
    // (Field name is lowercased to avoid collision with `Py_None`,
    // which is a macro in Python.h.)
    PyObject* py_none;
} g_py;

// Try one library name; return non-NULL handle on success.
static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Populate g_py from a loaded libpython handle. Returns 0 on success,
// -1 if any required symbol is missing.
static int resolve_python_symbols(void* h) {
#define RESOLVE(field, sym) do {                                      \
        *(void**)(&g_py.field) = dlsym(h, sym);                       \
        if (!g_py.field) {                                            \
            fprintf(stderr,                                           \
                "aether host_python: libpython missing symbol %s\n",  \
                sym);                                                 \
            return -1;                                                \
        }                                                             \
    } while (0)

    RESOLVE(Py_Initialize,           "Py_Initialize");
    RESOLVE(Py_Finalize,             "Py_Finalize");
    RESOLVE(Py_IncRef,               "Py_IncRef");
    RESOLVE(Py_DecRef,               "Py_DecRef");
    RESOLVE(PyRun_SimpleStringFlags, "PyRun_SimpleStringFlags");
    RESOLVE(PyArg_ParseTuple,        "PyArg_ParseTuple");
    RESOLVE(PyUnicode_FromString,    "PyUnicode_FromString");
    RESOLVE(PyModule_Create2,        "PyModule_Create2");
    RESOLVE(PyImport_GetModuleDict,  "PyImport_GetModuleDict");
    RESOLVE(PyDict_SetItemString,    "PyDict_SetItemString");

    // _Py_NoneStruct is a `PyObject` (the singleton itself, not a
    // pointer to it). dlsym returns the address of the singleton —
    // assign directly to our py_none pointer.
    g_py.py_none = (PyObject*)dlsym(h, "_Py_NoneStruct");
    if (!g_py.py_none) {
        fprintf(stderr, "aether host_python: libpython missing _Py_NoneStruct\n");
        return -1;
    }
    return 0;
#undef RESOLVE
}

// Load libpython on first use. Strict two-step contract:
//   1. ${AETHER_PYTHON_SONAME} env var (orchestrator-supplied exact
//      soname, e.g. "libpython3.14.so.1.0"). The orchestrator
//      (aeb-ctr or equivalent) should probe the host's python via
//      sysconfig and pass this; aeb-ctr does so.
//   2. "libpython3.so" (unversioned symlink, present in the runtime
//      package on Fedora-likes; -dev-only on Debian-likes).
// No versioned fallback list — hardcoding `libpython3.X.so.1.0`
// would be a maintenance treadmill (new minor → silent miss → looks
// like a runtime bug). If both fail, error out clearly so the
// orchestrator gets a real signal.
// Returns 0 on success, -1 on any failure.
static int load_libpython(void) {
    if (libpython_handle) return 0;

    const char* env_soname = getenv("AETHER_PYTHON_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libpython3.so");
    if (!h) {
        fprintf(stderr,
            "aether host_python: cannot dlopen libpython "
            "(tried $AETHER_PYTHON_SONAME=%s, libpython3.so).\n"
            "  Install a python3 runtime on the host, or set "
            "AETHER_PYTHON_SONAME to the exact soname "
            "(e.g. libpython3.12.so.1.0).\n"
            "  Hint: $(python3 -c 'import sysconfig; "
            "print(sysconfig.get_config_var(\"INSTSONAME\"))')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_python_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }

    libpython_handle = h;
    return 0;
}

// --- sandbox / permission stack (bridge-local; unchanged from pre-dlopen) ---

static int python_initialized = 0;

static void* python_perms_stack[64];
static int   python_perms_depth = 0;

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

static int host_python_checker(const char* category, const char* resource) {
    if (python_perms_depth <= 0) return 1;
    for (int level = 0; level < python_perms_depth; level++) {
        if (!perms_allow(python_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int python_init(void) {
    if (python_initialized) return 0;
    if (load_libpython() != 0) return -1;
    g_py.Py_Initialize();
    python_initialized = 1;
    return 0;
}

void python_finalize(void) {
    if (python_initialized) {
        g_py.Py_Finalize();
        python_initialized = 0;
    }
}

int python_run(const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    return g_py.PyRun_SimpleStringFlags(code, NULL);
}

int python_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    if (python_perms_depth >= 64) return -1;

    // Push perms onto our stack and install our checker
    python_perms_stack[python_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_python_checker;

    int result = g_py.PyRun_SimpleStringFlags(code, NULL);

    // Restore
    _aether_sandbox_checker = prev;
    python_perms_depth--;

    return result;
}

// --- Shared map bindings for Python -----------------------------------------

static uint64_t py_current_map_token = 0;

// Python callable: aether_map_get(key) → str or None
static PyObject* py_aether_map_get(PyObject* self, PyObject* args) {
    (void)self;
    const char* key;
    if (!g_py.PyArg_ParseTuple(args, "s", &key)) return NULL;
    const char* val = aether_shared_map_get_by_token(py_current_map_token, key);
    if (val) return g_py.PyUnicode_FromString(val);
    // Py_RETURN_NONE equivalent: incref None and return it.
    g_py.Py_IncRef(g_py.py_none);
    return g_py.py_none;
}

// Python callable: aether_map_put(key, value)
static PyObject* py_aether_map_put(PyObject* self, PyObject* args) {
    (void)self;
    const char* key;
    const char* value;
    if (!g_py.PyArg_ParseTuple(args, "ss", &key, &value)) return NULL;
    aether_shared_map_put_by_token(py_current_map_token, key, value);
    g_py.Py_IncRef(g_py.py_none);
    return g_py.py_none;
}

static PyMethodDef aether_map_methods[] = {
    {"aether_map_get", py_aether_map_get, METH_VARARGS, "Get value from Aether shared map"},
    {"aether_map_put", py_aether_map_put, METH_VARARGS, "Put value to Aether shared map"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef aether_map_module = {
    PyModuleDef_HEAD_INIT, "_aether_map", NULL, -1, aether_map_methods,
    NULL, NULL, NULL, NULL
};

static int map_module_registered = 0;

static void register_map_module(void) {
    if (map_module_registered) return;
    // PyModule_Create(&def) is a macro that calls
    // PyModule_Create2(&def, PYTHON_API_VERSION). We dlsym the latter
    // directly. PYTHON_API_VERSION is from the headers we compiled
    // against; ABI-compatible with the host's libpython for the
    // module-init handshake (the value has been stable across recent
    // CPython releases; the version it cares about is the one in
    // PyModuleDef_HEAD_INIT).
    PyObject* mod = g_py.PyModule_Create2(&aether_map_module, PYTHON_API_VERSION);
    if (mod) {
        PyObject* sys_modules = g_py.PyImport_GetModuleDict();
        g_py.PyDict_SetItemString(sys_modules, "_aether_map", mod);
        g_py.Py_DecRef(mod);
    }
    map_module_registered = 1;
}

int python_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    register_map_module();

    // Freeze inputs and set active token
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    py_current_map_token = map_token;

    // Inject convenience imports so user code just calls aether_map_get/put
    const char* preamble =
        "from _aether_map import aether_map_get, aether_map_put\n";

    if (python_perms_depth >= 64) return -1;

    // Push perms and install checker
    python_perms_stack[python_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_python_checker;

    // Run preamble + user code
    g_py.PyRun_SimpleStringFlags(preamble, NULL);
    int result = g_py.PyRun_SimpleStringFlags(code, NULL);

    // Restore
    _aether_sandbox_checker = prev;
    python_perms_depth--;
    py_current_map_token = 0;

    return result;
}

#else
// Stubs when Python is not available
#include <stdio.h>
int python_init(void) {
    fprintf(stderr, "error: contrib.host.python not available (compile with AETHER_HAS_PYTHON)\n");
    return -1;
}
void python_finalize(void) {}
int python_run(const char* code) { (void)code; return python_init(); }
int python_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return python_init();
}
int python_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return python_init();
}
#endif
