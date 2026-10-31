// aether_host_python.c — Embedded Python Language Host Module
//
// Embeds CPython in the Aether process. When run_sandboxed is called,
// pushes its grants as a level of the calling thread's (#2955), so Python's libc calls are
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
#include "../../../std/string/aether_string.h"

#ifdef AETHER_HAS_PYTHON
#include <Python.h>
#include "../aep_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include "../../../std/bytes/aether_bytes.h"

#define AE_PY_TNIL   0
#define AE_PY_TBOOL  1
#define AE_PY_TINT   2
#define AE_PY_TSTR   3
#define AE_PY_TTABLE 4
#define AE_PY_TERR   5
#define AE_PY_TOTHER 6
#define AE_PY_TBYTES 7
#define AE_PY_TFLOAT 8

// --- libpython dlopen table -------------------------------------------------
//
// All CPython C API access goes through this table. Populated by
// load_libpython() on first use; load failure makes every entry point
// return -1 with a clear message rather than segfault.

static void* libpython_handle = NULL;

static struct {
    // Lifecycle.
    void          (*Py_Initialize)(void);
    void          (*Py_Finalize)(void);
    int           (*Py_IsInitialized)(void);
    // Thread & GIL.
    PyThreadState*(*PyEval_SaveThread)(void);
    void          (*PyEval_RestoreThread)(PyThreadState*);
    PyGILState_STATE (*PyGILState_Ensure)(void);
    void          (*PyGILState_Release)(PyGILState_STATE);
    // Refcount helpers.
    void          (*Py_IncRef)(PyObject*);
    void          (*Py_DecRef)(PyObject*);
    // Execution & compilation.
    int           (*PyRun_SimpleStringFlags)(const char*, PyCompilerFlags*);
    PyObject*     (*py_compile_string_ex_flags)(const char*, const char*, int, PyCompilerFlags*, int);
    PyObject*     (*PyEval_EvalCode)(PyObject*, PyObject*, PyObject*);
    // Trace hook for instruction limit.
    void          (*PyEval_SetTrace)(Py_tracefunc, PyObject*);
    // Error handling.
    PyObject*     (*PyErr_Occurred)(void);
    void          (*PyErr_Clear)(void);
    void          (*PyErr_Fetch)(PyObject**, PyObject**, PyObject**);
    void          (*PyErr_NormalizeException)(PyObject**, PyObject**, PyObject**);
    void          (*PyErr_SetString)(PyObject*, const char*);
    // Object inspection & conversion.
    PyObject*     (*PyObject_Type)(PyObject*);
    int           (*PyType_IsSubtype)(PyTypeObject*, PyTypeObject*);
    PyObject*     (*PyObject_Str)(PyObject*);
    PyObject*     (*PyObject_GetAttrString)(PyObject*, const char*);
    int           (*PyObject_SetAttrString)(PyObject*, const char*, PyObject*);
    // Numbers.
    long long     (*PyLong_AsLongLongAndOverflow)(PyObject*, int*);
    PyObject*     (*PyLong_FromLongLong)(long long);
    double        (*PyLong_AsDouble)(PyObject*);
    double        (*PyFloat_AsDouble)(PyObject*);
    PyObject*     (*PyFloat_FromDouble)(double);
    PyObject*     (*PyBool_FromLong)(long);
    // Strings & Bytes.
    PyObject*     (*PyUnicode_FromString)(const char*);
    const char*   (*PyUnicode_AsUTF8AndSize)(PyObject*, Py_ssize_t*);
    PyObject*     (*PyBytes_FromStringAndSize)(const char*, Py_ssize_t);
    int           (*PyBytes_AsStringAndSize)(PyObject*, char**, Py_ssize_t*);
    // Collections & Tuples.
    PyObject*     (*PyList_New)(Py_ssize_t);
    int           (*PyList_Append)(PyObject*, PyObject*);
    Py_ssize_t    (*PyTuple_Size)(PyObject*);
    PyObject*     (*PyTuple_GetItem)(PyObject*, Py_ssize_t);
    // Args parsing — varargs; dlsym is fine with varargs sigs.
    int           (*PyArg_ParseTuple)(PyObject*, const char*, ...);
    // Module / dict / capsules.
    PyObject*     (*PyModule_New)(const char*);
    PyObject*     (*PyModule_Create2)(PyModuleDef*, int);
    PyObject*     (*PyImport_GetModuleDict)(void);
    PyObject*     (*PyEval_GetBuiltins)(void);
    PyObject*     (*PyDict_New)(void);
    int           (*PyDict_SetItemString)(PyObject*, const char*, PyObject*);
    PyObject*     (*PyDict_GetItemString)(PyObject*, const char*);
    PyObject*     (*py_cfunction_new_ex)(PyMethodDef*, PyObject*, PyObject*);
    PyObject*     (*PyCapsule_New)(void*, const char*, PyCapsule_Destructor);
    void*         (*PyCapsule_GetPointer)(PyObject*, const char*);

    // Data symbols (addresses of type singletons / exceptions / None).
    PyObject*     py_none;
    PyObject*     py_bool_type;
    PyObject*     py_long_type;
    PyObject*     py_float_type;
    PyObject*     py_unicode_type;
    PyObject*     py_bytes_type;
    PyObject*     py_list_type;
    PyObject*     py_dict_type;
    PyObject*     py_exc_runtime_error;
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

    RESOLVE(Py_Initialize,               "Py_Initialize");
    RESOLVE(Py_Finalize,                 "Py_Finalize");
    RESOLVE(Py_IsInitialized,            "Py_IsInitialized");
    RESOLVE(PyEval_SaveThread,           "PyEval_SaveThread");
    RESOLVE(PyEval_RestoreThread,        "PyEval_RestoreThread");
    RESOLVE(PyGILState_Ensure,           "PyGILState_Ensure");
    RESOLVE(PyGILState_Release,          "PyGILState_Release");
    RESOLVE(Py_IncRef,                   "Py_IncRef");
    RESOLVE(Py_DecRef,                   "Py_DecRef");
    RESOLVE(PyRun_SimpleStringFlags,     "PyRun_SimpleStringFlags");
    RESOLVE(py_compile_string_ex_flags,  "Py_CompileStringExFlags");
    RESOLVE(PyEval_EvalCode,             "PyEval_EvalCode");
    RESOLVE(PyEval_SetTrace,             "PyEval_SetTrace");
    RESOLVE(PyErr_Occurred,              "PyErr_Occurred");
    RESOLVE(PyErr_Clear,                 "PyErr_Clear");
    RESOLVE(PyErr_Fetch,                 "PyErr_Fetch");
    RESOLVE(PyErr_NormalizeException,    "PyErr_NormalizeException");
    RESOLVE(PyErr_SetString,             "PyErr_SetString");
    RESOLVE(PyObject_Type,               "PyObject_Type");
    RESOLVE(PyType_IsSubtype,            "PyType_IsSubtype");
    RESOLVE(PyObject_Str,                "PyObject_Str");
    RESOLVE(PyObject_GetAttrString,      "PyObject_GetAttrString");
    RESOLVE(PyObject_SetAttrString,      "PyObject_SetAttrString");
    RESOLVE(PyLong_AsLongLongAndOverflow,"PyLong_AsLongLongAndOverflow");
    RESOLVE(PyLong_FromLongLong,         "PyLong_FromLongLong");
    RESOLVE(PyLong_AsDouble,             "PyLong_AsDouble");
    RESOLVE(PyFloat_AsDouble,            "PyFloat_AsDouble");
    RESOLVE(PyFloat_FromDouble,          "PyFloat_FromDouble");
    RESOLVE(PyBool_FromLong,             "PyBool_FromLong");
    RESOLVE(PyUnicode_FromString,        "PyUnicode_FromString");
    RESOLVE(PyUnicode_AsUTF8AndSize,     "PyUnicode_AsUTF8AndSize");
    RESOLVE(PyBytes_FromStringAndSize,   "PyBytes_FromStringAndSize");
    RESOLVE(PyBytes_AsStringAndSize,     "PyBytes_AsStringAndSize");
    RESOLVE(PyList_New,                  "PyList_New");
    RESOLVE(PyList_Append,               "PyList_Append");
    RESOLVE(PyTuple_Size,                "PyTuple_Size");
    RESOLVE(PyTuple_GetItem,             "PyTuple_GetItem");
    RESOLVE(PyArg_ParseTuple,            "PyArg_ParseTuple");
    RESOLVE(PyModule_New,                "PyModule_New");
    RESOLVE(PyModule_Create2,            "PyModule_Create2");
    RESOLVE(PyImport_GetModuleDict,      "PyImport_GetModuleDict");
    RESOLVE(PyEval_GetBuiltins,          "PyEval_GetBuiltins");
    RESOLVE(PyDict_New,                  "PyDict_New");
    RESOLVE(PyDict_SetItemString,        "PyDict_SetItemString");
    RESOLVE(PyDict_GetItemString,        "PyDict_GetItemString");
    RESOLVE(py_cfunction_new_ex,         "PyCFunction_NewEx");
    RESOLVE(PyCapsule_New,               "PyCapsule_New");
    RESOLVE(PyCapsule_GetPointer,        "PyCapsule_GetPointer");

    // DATA symbols:
    g_py.py_none = (PyObject*)dlsym(h, "_Py_NoneStruct");
    if (!g_py.py_none) {
        fprintf(stderr, "aether host_python: libpython missing _Py_NoneStruct\n");
        return -1;
    }

    g_py.py_bool_type = (PyObject*)dlsym(h, "PyBool_Type");
    g_py.py_long_type = (PyObject*)dlsym(h, "PyLong_Type");
    g_py.py_float_type = (PyObject*)dlsym(h, "PyFloat_Type");
    g_py.py_unicode_type = (PyObject*)dlsym(h, "PyUnicode_Type");
    g_py.py_bytes_type = (PyObject*)dlsym(h, "PyBytes_Type");
    g_py.py_list_type = (PyObject*)dlsym(h, "PyList_Type");
    g_py.py_dict_type = (PyObject*)dlsym(h, "PyDict_Type");

    PyObject** exc_rt = (PyObject**)dlsym(h, "PyExc_RuntimeError");
    if (exc_rt) {
        g_py.py_exc_runtime_error = *exc_rt;
    }

    if (!g_py.py_bool_type || !g_py.py_long_type || !g_py.py_float_type ||
        !g_py.py_unicode_type || !g_py.py_bytes_type || !g_py.py_list_type ||
        !g_py.py_dict_type || !g_py.py_exc_runtime_error) {
        fprintf(stderr, "aether host_python: libpython missing required type/exception symbol\n");
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
// The main thread state, parked by PyEval_SaveThread after Py_Initialize so
// that any thread can take the GIL with PyGILState_Ensure. NULL when the
// process already had a running interpreter (Aether loaded into Python): that
// interpreter is its owner's, so finalize leaves it alone.
static PyThreadState* g_saved_thread_state = NULL;
static int g_owns_interpreter = 0;
// init runs from whichever thread gets there first; actors can race to it.
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

static int map_module_registered = 0;

int python_init(void) {
    pthread_mutex_lock(&g_init_lock);
    int rc = 0;
    if (!python_initialized) {
        if (load_libpython() != 0) {
            rc = -1;
        } else {
            if (!g_py.Py_IsInitialized()) {
                g_py.Py_Initialize();
                // Release the GIL, so every entry point (on any thread)
                // takes it with PyGILState_Ensure.
                g_saved_thread_state = g_py.PyEval_SaveThread();
                g_owns_interpreter = 1;
            }
            python_initialized = 1;
        }
    }
    pthread_mutex_unlock(&g_init_lock);
    return rc;
}

// Any VM still alive is unusable afterwards. Must run on the thread that
// called init, as Py_Finalize must.
void python_finalize(void) {
    pthread_mutex_lock(&g_init_lock);
    if (python_initialized) {
        if (g_owns_interpreter) {
            g_py.PyEval_RestoreThread(g_saved_thread_state);
            g_py.Py_Finalize();
        }
        g_saved_thread_state = NULL;
        g_owns_interpreter = 0;
        // A later init makes a new interpreter, whose sys.modules has no
        // _aether_map yet.
        map_module_registered = 0;
        python_initialized = 0;
    }
    pthread_mutex_unlock(&g_init_lock);
}

int python_run(const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    int rc = g_py.PyRun_SimpleStringFlags(code, NULL);
    g_py.PyGILState_Release(gstate);
    return rc;
}

int python_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    // The grants as a level of this thread's, in the runtime (#2955)
    aether_sandbox_push(perms);

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    int result = g_py.PyRun_SimpleStringFlags(code, NULL);
    g_py.PyGILState_Release(gstate);

    // Restore
    aether_sandbox_pop();

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

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    register_map_module();

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }

    // Freeze inputs and set active token
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    uint64_t previous_map_token = py_current_map_token;
    py_current_map_token = map_token;

    // Inject convenience imports so user code just calls aether_map_get/put
    const char* preamble =
        "from _aether_map import aether_map_get, aether_map_put\n";

    // The grants as a level of this thread's, in the runtime (#2955)
    aether_sandbox_push(perms);

    // Run preamble + user code
    g_py.PyRun_SimpleStringFlags(preamble, NULL);
    int result = g_py.PyRun_SimpleStringFlags(code, NULL);

    // Restore
    aether_sandbox_pop();
    py_current_map_token = previous_map_token;
    g_py.PyGILState_Release(gstate);

    return result;
}

// --- Tier 2: Bidirectional Embedding API (#904) -----------------------------
//
// A VM is a globals dict on the one shared interpreter (CPython has one per
// process), so two VMs share sys.modules, imported module state and the GIL.
// Every entry point takes the GIL with PyGILState_Ensure, which nests, so a
// callback (which runs with the GIL held) can call back into the bridge.
// A VM is used by one thread at a time; the result fields are its own.

typedef struct AetherPythonVM AetherPythonVM;

// One registered host function. The function object Python holds points at
// `def` through its capsule, so this lives as long as the longer of the VM
// and the capsule: a script can keep the function after vm_free (store it
// in a module, which every VM shares), and calling it then must not reach
// the freed VM. Both owners hold a reference; vm_free sets `vm` to NULL.
typedef struct AePyCallback {
    AetherPythonVM* vm;
    int           (*host_fn)(void*);
    PyMethodDef     def;
    char*           name;
    int             refs;
} AePyCallback;

struct AetherPythonVM {
    // Globals dict for this VM (has __builtins__ injected). Owned.
    PyObject* globals;

    // The last eval's result. result_obj is owned until pop or the next
    // eval; result_str is a malloc'd copy (a str result, or the error).
    PyObject* result_obj;
    int       result_type;
    int64_t   result_int;
    double    result_double;
    int       result_bool;
    char*     result_str;

    // Per-call state of the callback now running; saved and restored around
    // each callback, so a nested eval's callbacks don't clobber it.
    PyObject* current_args;     // borrowed: the call's args tuple
    PyObject* pushed_result;    // owned new ref from python_push_*
    char*     raise_error_msg;  // set by python_raise_error

    // Instruction limit: armed by the outermost eval only, so a callback's
    // nested eval neither resets the budget nor removes the hook.
    int64_t   instruction_budget;
    int64_t   instruction_counter;
    int       instruction_tripped;
    int       eval_depth;

    AePyCallback** callbacks;
    int            n_callbacks;
    int            cap_callbacks;
};

#define AE_PY_RAISE (-1)
#define AE_PY_CB_CAPSULE "aether_host_cb"

static void ae_py_callback_unref(AePyCallback* cb) {
    if (--cb->refs > 0) return;
    free(cb->name);
    free(cb);
}

// Capsule destructor: the function object (and so its capsule) has gone.
static void ae_py_callback_capsule_free(PyObject* capsule) {
    AePyCallback* cb = (AePyCallback*)g_py.PyCapsule_GetPointer(capsule, AE_PY_CB_CAPSULE);
    if (cb) ae_py_callback_unref(cb);
    else g_py.PyErr_Clear();
}

// isinstance without running Python code: PyObject_IsInstance consults a
// `__class__` attribute, which a script can make lie (a class whose
// __class__ is bytes would pass for bytes).
static int py_isa(PyObject* obj, PyObject* type) {
    PyObject* t = g_py.PyObject_Type(obj);   // new ref, released below
    if (!t) { g_py.PyErr_Clear(); return 0; }
    int r = g_py.PyType_IsSubtype((PyTypeObject*)t, (PyTypeObject*)type);
    g_py.Py_DecRef(t);
    return r;
}

static int64_t double_to_i64(double d) {
    if (isnan(d)) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

// The tag of `obj`, and its numeric readings. bool is checked before int:
// in Python bool is a subclass of int. An int outside int64 is TOTHER, reads
// 0 as an int (never a wrapped value) and its nearest double as a double.
static int py_classify(PyObject* obj, int64_t* as_int, double* as_double, int* as_bool) {
    *as_int = 0; *as_double = 0.0; *as_bool = 0;
    if (!obj || obj == g_py.py_none) return AE_PY_TNIL;
    if (py_isa(obj, g_py.py_bool_type)) {
        int ov = 0;
        *as_int = g_py.PyLong_AsLongLongAndOverflow(obj, &ov) != 0;
        *as_bool = (int)*as_int;
        *as_double = (double)*as_int;
        return AE_PY_TBOOL;
    }
    if (py_isa(obj, g_py.py_long_type)) {
        int ov = 0;
        long long v = g_py.PyLong_AsLongLongAndOverflow(obj, &ov);
        if (ov != 0) {
            double d = g_py.PyLong_AsDouble(obj);   // OverflowError past ~1e308
            if (d == -1.0 && g_py.PyErr_Occurred()) {
                g_py.PyErr_Clear();
                d = ov > 0 ? HUGE_VAL : -HUGE_VAL;
            }
            *as_double = d;
            return AE_PY_TOTHER;
        }
        if (v == -1 && g_py.PyErr_Occurred()) { g_py.PyErr_Clear(); return AE_PY_TOTHER; }
        *as_int = (int64_t)v;
        *as_double = (double)v;
        return AE_PY_TINT;
    }
    if (py_isa(obj, g_py.py_float_type)) {
        double d = g_py.PyFloat_AsDouble(obj);
        if (d == -1.0 && g_py.PyErr_Occurred()) g_py.PyErr_Clear();
        *as_double = d;
        *as_int = double_to_i64(d);
        return AE_PY_TFLOAT;
    }
    if (py_isa(obj, g_py.py_bytes_type)) return AE_PY_TBYTES;
    if (py_isa(obj, g_py.py_unicode_type)) return AE_PY_TSTR;
    if (py_isa(obj, g_py.py_list_type) || py_isa(obj, g_py.py_dict_type)) return AE_PY_TTABLE;
    return AE_PY_TOTHER;
}

// A fresh AetherBytes copy of a bytes object, or NULL. Lengths past int
// (what std.bytes counts in) are refused rather than cut.
static void* py_bytes_to_handle(PyObject* obj) {
    char* buf = NULL;
    Py_ssize_t len = 0;
    if (g_py.PyBytes_AsStringAndSize(obj, &buf, &len) != 0 || !buf) {
        g_py.PyErr_Clear();
        return NULL;
    }
    if (len > INT_MAX) return NULL;
    return aether_bytes_from_ptr(buf, (int)len);
}

static void clear_vm_result(AetherPythonVM* vm) {
    if (vm->result_obj) {
        g_py.Py_DecRef(vm->result_obj);
        vm->result_obj = NULL;
    }
    free(vm->result_str);
    vm->result_str = NULL;
    vm->result_type = AE_PY_TNIL;
    vm->result_int = 0;
    vm->result_double = 0.0;
    vm->result_bool = 0;
}

void* python_vm_new(void) {
    if (python_init() != 0) return NULL;

    AetherPythonVM* vm = (AetherPythonVM*)calloc(1, sizeof(AetherPythonVM));
    if (!vm) return NULL;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    vm->globals = g_py.PyDict_New();   // owned by the VM
    PyObject* builtins = vm->globals ? g_py.PyEval_GetBuiltins() : NULL;   // borrowed
    if (!builtins || g_py.PyDict_SetItemString(vm->globals, "__builtins__", builtins) != 0) {
        g_py.PyErr_Clear();
        if (vm->globals) g_py.Py_DecRef(vm->globals);
        g_py.PyGILState_Release(gstate);
        free(vm);
        return NULL;
    }
    g_py.PyGILState_Release(gstate);

    vm->result_type = AE_PY_TNIL;
    return (void*)vm;
}

void python_vm_free(void* v) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    clear_vm_result(vm);
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    free(vm->raise_error_msg);

    // Orphan the callbacks first: a function object that outlives this
    // dict (or a reference cycle through it, freed later by the GC) then
    // raises instead of reaching the freed VM.
    for (int i = 0; i < vm->n_callbacks; i++) {
        vm->callbacks[i]->vm = NULL;
        ae_py_callback_unref(vm->callbacks[i]);
    }
    free(vm->callbacks);
    g_py.Py_DecRef(vm->globals);
    g_py.PyGILState_Release(gstate);

    free(vm);
}

// Called by CPython when a script calls a registered host function. Runs
// with the GIL held.
static PyObject* python_host_cb_trampoline(PyObject* self, PyObject* args) {
    AePyCallback* cb = (AePyCallback*)g_py.PyCapsule_GetPointer(self, AE_PY_CB_CAPSULE);
    if (!cb) return NULL;   // PyCapsule_GetPointer set the error
    AetherPythonVM* vm = cb->vm;
    if (!vm) {
        g_py.PyErr_SetString(g_py.py_exc_runtime_error,
                             "aether host function called after its vm was freed");
        return NULL;
    }

    PyObject* prev_args = vm->current_args;
    PyObject* prev_pushed = vm->pushed_result;
    char*     prev_raise = vm->raise_error_msg;
    vm->current_args = args;
    vm->pushed_result = NULL;
    vm->raise_error_msg = NULL;

    int rc = cb->host_fn((void*)vm);

    PyObject* ret;
    if (vm->raise_error_msg) {
        g_py.PyErr_SetString(g_py.py_exc_runtime_error, vm->raise_error_msg);
        ret = NULL;
    } else if (rc > 0 && vm->pushed_result) {
        ret = vm->pushed_result;   // the caller takes this reference
        vm->pushed_result = NULL;
    } else {
        g_py.Py_IncRef(g_py.py_none);
        ret = g_py.py_none;
    }
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    free(vm->raise_error_msg);

    vm->current_args = prev_args;
    vm->pushed_result = prev_pushed;
    vm->raise_error_msg = prev_raise;
    return ret;
}

// The attribute `part` of `parent`, made a new module when absent. Returns
// a borrowed reference (the parent holds one), or NULL with no error set.
static PyObject* ensure_namespace(AetherPythonVM* vm, PyObject* parent, const char* part) {
    PyObject* obj;
    if (!parent) {
        obj = g_py.PyDict_GetItemString(vm->globals, part);   // borrowed
        if (obj) return obj;
        obj = g_py.PyModule_New(part);   // new ref, given to the dict below
        if (!obj || g_py.PyDict_SetItemString(vm->globals, part, obj) != 0) {
            if (obj) g_py.Py_DecRef(obj);
            g_py.PyErr_Clear();
            return NULL;
        }
        g_py.Py_DecRef(obj);
        return obj;
    }
    obj = g_py.PyObject_GetAttrString(parent, part);   // new ref
    if (obj) {
        g_py.Py_DecRef(obj);   // the parent keeps it alive
        return obj;
    }
    g_py.PyErr_Clear();
    obj = g_py.PyModule_New(part);
    if (!obj || g_py.PyObject_SetAttrString(parent, part, obj) != 0) {
        if (obj) g_py.Py_DecRef(obj);
        g_py.PyErr_Clear();
        return NULL;
    }
    g_py.Py_DecRef(obj);
    return obj;
}

int python_vm_register(void* v, const char* name, void* host_fn) {
    if (!v || !name || !*name || !host_fn) return -1;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    char tmp[256];
    size_t name_len = strlen(name);
    if (name_len >= sizeof(tmp)) return -1;
    memcpy(tmp, name, name_len + 1);

    if (vm->n_callbacks == vm->cap_callbacks) {
        int cap = vm->cap_callbacks ? vm->cap_callbacks * 2 : 8;
        AePyCallback** grown = (AePyCallback**)realloc(vm->callbacks, (size_t)cap * sizeof(*grown));
        if (!grown) return -1;
        vm->callbacks = grown;
        vm->cap_callbacks = cap;
    }

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    // "app.math.add": app and app.math are modules, made or reused.
    PyObject* parent = NULL;
    char* part = tmp;
    char* dot;
    while ((dot = strchr(part, '.')) != NULL) {
        *dot = '\0';
        if (!*part || !(parent = ensure_namespace(vm, parent, part))) {
            g_py.PyGILState_Release(gstate);
            return -1;
        }
        part = dot + 1;
    }
    if (!*part) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }

    AePyCallback* cb = (AePyCallback*)calloc(1, sizeof(AePyCallback));
    char* leaf = cb ? strdup(part) : NULL;
    if (!leaf) {
        free(cb);
        g_py.PyGILState_Release(gstate);
        return -1;
    }
    cb->vm = vm;
    cb->host_fn = (int (*)(void*))host_fn;
    cb->name = leaf;
    cb->def.ml_name = leaf;
    cb->def.ml_meth = (PyCFunction)python_host_cb_trampoline;
    cb->def.ml_flags = METH_VARARGS;
    cb->refs = 1;   // the VM's
    vm->callbacks[vm->n_callbacks++] = cb;

    // The capsule's reference is taken only once it exists: if it can't be
    // made, the VM's is the only one.
    PyObject* capsule = g_py.PyCapsule_New(cb, AE_PY_CB_CAPSULE, ae_py_callback_capsule_free);
    if (!capsule) {
        g_py.PyErr_Clear();
        g_py.PyGILState_Release(gstate);
        return -1;
    }
    cb->refs++;

    PyObject* fn = g_py.py_cfunction_new_ex(&cb->def, capsule, NULL);   // new ref; holds the capsule
    g_py.Py_DecRef(capsule);
    int rc = -1;
    if (fn) {
        rc = parent ? g_py.PyObject_SetAttrString(parent, part, fn)
                    : g_py.PyDict_SetItemString(vm->globals, part, fn);
        g_py.Py_DecRef(fn);   // the namespace keeps it
    }
    if (rc != 0) {
        g_py.PyErr_Clear();
        rc = -1;
    }
    g_py.PyGILState_Release(gstate);
    return rc;
}

// Stores `value` (a new reference, taken) as the global `name`.
static void set_global(AetherPythonVM* vm, const char* name, PyObject* value) {
    if (!value || g_py.PyDict_SetItemString(vm->globals, name, value) != 0) g_py.PyErr_Clear();
    if (value) g_py.Py_DecRef(value);   // the dict keeps its own
}

void python_vm_set_global_str(void* v, const char* name, const char* value) {
    if (!v || !name) return;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    set_global((AetherPythonVM*)v, name, g_py.PyUnicode_FromString(value ? value : ""));
    g_py.PyGILState_Release(gstate);
}

void python_vm_set_global_int(void* v, const char* name, int64_t value) {
    if (!v || !name) return;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    set_global((AetherPythonVM*)v, name, g_py.PyLong_FromLongLong(value));
    g_py.PyGILState_Release(gstate);
}

void python_vm_set_global_strlist(void* v, const char* name, void* items) {
    if (!v || !name) return;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    int count = items ? list_size(items) : 0;
    PyObject* list_obj = g_py.PyList_New(0);
    for (int i = 0; list_obj && i < count; i++) {
        void* item = list_get_raw(items, i);
        const char* str = item ? aether_string_data(item) : "";
        PyObject* s = g_py.PyUnicode_FromString(str ? str : "");   // new ref
        if (!s || g_py.PyList_Append(list_obj, s) != 0) {
            if (s) g_py.Py_DecRef(s);
            g_py.Py_DecRef(list_obj);
            list_obj = NULL;
            break;
        }
        g_py.Py_DecRef(s);   // the list keeps its own
    }
    set_global((AetherPythonVM*)v, name, list_obj);
    g_py.PyGILState_Release(gstate);
}

// The instruction-limit trace hook, called on every line, call and return.
// Once the budget is spent it raises on EVERY later event, not just once:
// a script that catches the exception (`except BaseException: pass`, a
// `finally`) runs its handler and is stopped again at its next line, so it
// can't catch the limit and carry on.
static int python_trace_hook(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg) {
    (void)frame; (void)what; (void)arg;
    AetherPythonVM* vm = (AetherPythonVM*)g_py.PyCapsule_GetPointer(obj, "vm_trace");
    if (!vm) return -1;
    if (!vm->instruction_tripped && --vm->instruction_counter > 0) return 0;
    vm->instruction_tripped = 1;
    g_py.PyErr_SetString(g_py.py_exc_runtime_error, "instruction limit exceeded");
    return -1;
}

// "<ExceptionType>: <message>" for the pending exception, which it clears.
static char* fetch_error_message(void) {
    PyObject *ptype = NULL, *pval = NULL, *ptrace = NULL;
    g_py.PyErr_Fetch(&ptype, &pval, &ptrace);   // all three are ours to release
    if (!ptype) return strdup("Error: unknown");
    g_py.PyErr_NormalizeException(&ptype, &pval, &ptrace);

    const char* type_name = "Error";
    PyObject* name_obj = g_py.PyObject_GetAttrString(ptype, "__name__");
    if (name_obj) {
        const char* s = g_py.PyUnicode_AsUTF8AndSize(name_obj, NULL);
        if (s) type_name = s;
    }
    const char* msg = "";
    PyObject* str_val = pval ? g_py.PyObject_Str(pval) : NULL;
    if (str_val) {
        const char* s = g_py.PyUnicode_AsUTF8AndSize(str_val, NULL);
        if (s) msg = s;
    }
    g_py.PyErr_Clear();   // anything the lookups above raised

    size_t n = strlen(type_name) + strlen(msg) + 3;
    char* out = (char*)malloc(n);
    if (out) snprintf(out, n, "%s: %s", type_name, msg);

    if (name_obj) g_py.Py_DecRef(name_obj);
    if (str_val) g_py.Py_DecRef(str_val);
    g_py.Py_DecRef(ptype);
    if (pval) g_py.Py_DecRef(pval);
    if (ptrace) g_py.Py_DecRef(ptrace);
    return out;
}

int python_vm_eval(void* v, const char* code) {
    if (!v || !code) return -1;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    clear_vm_result(vm);

    // The hook goes on this thread's state for the outermost eval only.
    PyObject* trace_cap = NULL;
    if (vm->eval_depth == 0 && vm->instruction_budget > 0) {
        vm->instruction_counter = vm->instruction_budget;
        vm->instruction_tripped = 0;
        trace_cap = g_py.PyCapsule_New(vm, "vm_trace", NULL);   // ours; SetTrace takes its own
        if (trace_cap) g_py.PyEval_SetTrace(python_trace_hook, trace_cap);
        else g_py.PyErr_Clear();
    }
    vm->eval_depth++;

    // An expression gives its value; if the code isn't one, it is compiled
    // again as statements (which give None). Compiling runs nothing, so the
    // code runs exactly once either way.
    PyObject* code_obj = g_py.py_compile_string_ex_flags(code, "<eval>", Py_eval_input, NULL, -1);
    if (!code_obj) {
        g_py.PyErr_Clear();
        code_obj = g_py.py_compile_string_ex_flags(code, "<eval>", Py_file_input, NULL, -1);
    }
    PyObject* res = NULL;
    if (code_obj) {
        res = g_py.PyEval_EvalCode(code_obj, vm->globals, vm->globals);   // new ref, kept as the result
        g_py.Py_DecRef(code_obj);
    }

    vm->eval_depth--;
    if (trace_cap) {
        g_py.PyEval_SetTrace(NULL, NULL);
        g_py.Py_DecRef(trace_cap);
    }

    int rc = 0;
    if (res) {
        vm->result_obj = res;
        vm->result_type = py_classify(res, &vm->result_int, &vm->result_double, &vm->result_bool);
        if (vm->result_type == AE_PY_TSTR) {
            Py_ssize_t len = 0;
            const char* utf8 = g_py.PyUnicode_AsUTF8AndSize(res, &len);
            if (utf8) {
                vm->result_str = (char*)malloc((size_t)len + 1);
                if (vm->result_str) memcpy(vm->result_str, utf8, (size_t)len + 1);
            }
        }
    }
    if (!res || g_py.PyErr_Occurred()) {
        // A failed compile or run; or a str result that can't be UTF-8
        // (a lone surrogate), which reads as that error.
        clear_vm_result(vm);
        vm->result_type = AE_PY_TERR;
        vm->result_str = fetch_error_message();
        rc = -1;   // fetching the error cleared it: the next call starts clean
    }

    g_py.PyGILState_Release(gstate);
    return rc;
}

int python_vm_result_type(void* v) {
    return v ? ((AetherPythonVM*)v)->result_type : AE_PY_TNIL;
}

int64_t python_vm_result_int(void* v) {
    return v ? ((AetherPythonVM*)v)->result_int : 0;
}

double python_vm_result_double(void* v) {
    return v ? ((AetherPythonVM*)v)->result_double : 0.0;
}

int python_vm_result_bool(void* v) {
    return v ? ((AetherPythonVM*)v)->result_bool : 0;
}

const char* python_vm_result_str(void* v) {
    if (!v) return "";
    AetherPythonVM* vm = (AetherPythonVM*)v;
    return vm->result_str ? vm->result_str : "";
}

void* python_vm_result_bytes(void* v) {
    if (!v) return NULL;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->result_type != AE_PY_TBYTES || !vm->result_obj) return NULL;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    void* b = py_bytes_to_handle(vm->result_obj);
    g_py.PyGILState_Release(gstate);
    return b;
}

void python_vm_pop_result(void* v) {
    if (!v) return;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    clear_vm_result((AetherPythonVM*)v);
    g_py.PyGILState_Release(gstate);
}

// The budget counts trace events (lines, calls, returns), each eval's own.
// `every` is for the other hosts' signature: a trace hook sees every event,
// so there is nothing to space out.
const char* python_vm_set_instruction_limit(void* v, int64_t budget, int every) {
    (void)every;
    if (!v) return "no vm";
    AetherPythonVM* vm = (AetherPythonVM*)v;
    vm->instruction_budget = budget > 0 ? budget : 0;
    return "";
}

// --- Callback-side stack API ------------------------------------------------

static PyObject* get_arg_obj(AetherPythonVM* vm, int i) {
    if (!vm || !vm->current_args || i < 1) return NULL;
    Py_ssize_t idx = (Py_ssize_t)(i - 1);
    if (idx >= g_py.PyTuple_Size(vm->current_args)) return NULL;
    return g_py.PyTuple_GetItem(vm->current_args, idx);   // borrowed from the args tuple
}

int python_arg_count(void* v) {
    if (!v) return 0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    return vm->current_args ? (int)g_py.PyTuple_Size(vm->current_args) : 0;
}

int python_arg_type(void* v, int i) {
    int64_t n; double d; int b;
    return py_classify(get_arg_obj((AetherPythonVM*)v, i), &n, &d, &b);
}

// Valid until the callback returns (the args tuple holds the string).
const char* python_arg_str(void* v, int i) {
    PyObject* obj = get_arg_obj((AetherPythonVM*)v, i);
    if (!obj || !py_isa(obj, g_py.py_unicode_type)) return "";
    const char* s = g_py.PyUnicode_AsUTF8AndSize(obj, NULL);
    if (!s) { g_py.PyErr_Clear(); return ""; }
    return s;
}

int64_t python_arg_int(void* v, int i) {
    int64_t n; double d; int b;
    py_classify(get_arg_obj((AetherPythonVM*)v, i), &n, &d, &b);
    return n;
}

double python_arg_double(void* v, int i) {
    int64_t n; double d; int b;
    py_classify(get_arg_obj((AetherPythonVM*)v, i), &n, &d, &b);
    return d;
}

int python_arg_bool(void* v, int i) {
    int64_t n; double d; int b;
    py_classify(get_arg_obj((AetherPythonVM*)v, i), &n, &d, &b);
    return b;
}

void* python_arg_bytes(void* v, int i) {
    PyObject* obj = get_arg_obj((AetherPythonVM*)v, i);
    if (!obj || !py_isa(obj, g_py.py_bytes_type)) return NULL;
    return py_bytes_to_handle(obj);
}

// --- Callback pushers -------------------------------------------------------
// A callback returns one value, so each push replaces the last. Each takes
// a new reference, which the trampoline hands to Python or releases.

static void push_obj(void* v, PyObject* obj) {
    if (!v) {
        if (obj) g_py.Py_DecRef(obj);
        return;
    }
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    if (!obj) g_py.PyErr_Clear();
    vm->pushed_result = obj;
}

void python_push_int(void* v, int64_t val) {
    if (v) push_obj(v, g_py.PyLong_FromLongLong(val));
}

void python_push_str(void* v, const char* val) {
    if (v) push_obj(v, g_py.PyUnicode_FromString(val ? val : ""));
}

void python_push_bool(void* v, int val) {
    if (v) push_obj(v, g_py.PyBool_FromLong(val != 0));
}

void python_push_nil(void* v) {
    if (!v) return;
    g_py.Py_IncRef(g_py.py_none);
    push_obj(v, g_py.py_none);
}

void python_push_double(void* v, double val) {
    if (v) push_obj(v, g_py.PyFloat_FromDouble(val));
}

// `b` is a std.bytes handle; the bytes are copied.
void python_push_bytes(void* v, void* b) {
    if (!v) return;
    int len = b ? aether_bytes_length((AetherBytes*)b) : -1;
    if (len < 0) {
        python_push_nil(v);
        return;
    }
    const char* data = len > 0 ? (const char*)aether_bytes_data((AetherBytes*)b) : "";
    push_obj(v, g_py.PyBytes_FromStringAndSize(data, (Py_ssize_t)len));
}

// `return python.raise_error(vm, msg)`: the RuntimeError is raised after
// the callback has returned, so the callback's defers run.
int python_raise_error(void* v, const char* msg) {
    if (!v) return AE_PY_RAISE;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    free(vm->raise_error_msg);
    vm->raise_error_msg = strdup(msg ? msg : "error");
    return AE_PY_RAISE;
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

void* python_vm_new(void) { python_init(); return NULL; }
void python_vm_free(void* vm) { (void)vm; }
int python_vm_register(void* vm, const char* name, void* host_fn) { (void)vm; (void)name; (void)host_fn; return -1; }
void python_vm_set_global_str(void* vm, const char* name, const char* value) { (void)vm; (void)name; (void)value; }
void python_vm_set_global_int(void* vm, const char* name, int64_t value) { (void)vm; (void)name; (void)value; }
void python_vm_set_global_strlist(void* vm, const char* name, void* items) { (void)vm; (void)name; (void)items; }
int python_vm_eval(void* vm, const char* code) { (void)vm; (void)code; return -1; }
int python_vm_result_type(void* vm) { (void)vm; return 0; }
int64_t python_vm_result_int(void* vm) { (void)vm; return 0; }
const char* python_vm_result_str(void* vm) { (void)vm; return ""; }
int python_vm_result_bool(void* vm) { (void)vm; return 0; }
double python_vm_result_double(void* vm) { (void)vm; return 0.0; }
void* python_vm_result_bytes(void* vm) { (void)vm; return NULL; }
void python_vm_pop_result(void* vm) { (void)vm; }
const char* python_vm_set_instruction_limit(void* vm, int64_t budget, int every) { (void)vm; (void)budget; (void)every; return "execution limit unavailable: built without AETHER_HAS_PYTHON"; }

int python_arg_count(void* vm) { (void)vm; return 0; }
int python_arg_type(void* vm, int i) { (void)vm; (void)i; return 0; }
const char* python_arg_str(void* vm, int i) { (void)vm; (void)i; return ""; }
int64_t python_arg_int(void* vm, int i) { (void)vm; (void)i; return 0; }
int python_arg_bool(void* vm, int i) { (void)vm; (void)i; return 0; }
double python_arg_double(void* vm, int i) { (void)vm; (void)i; return 0.0; }
void* python_arg_bytes(void* vm, int i) { (void)vm; (void)i; return NULL; }

void python_push_int(void* vm, int64_t v) { (void)vm; (void)v; }
void python_push_str(void* vm, const char* v) { (void)vm; (void)v; }
void python_push_bool(void* vm, int v) { (void)vm; (void)v; }
void python_push_nil(void* vm) { (void)vm; }
void python_push_double(void* vm, double v) { (void)vm; (void)v; }
void python_push_bytes(void* vm, void* b) { (void)vm; (void)b; }
int python_raise_error(void* vm, const char* msg) { (void)vm; (void)msg; return -1; }
#endif
